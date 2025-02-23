/*
 * kate: space-indent on; indent-width 2; mixedindent off; indent-mode cstyle; remove-trailing-space on;
 * Copyright (C) 2008-2023 the xine project
 * Copyright (C) 2008 Christophe Thommeret <hftom@free.fr>
 *
 * This file is part of xine, a free video player.
 *
 * xine is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * xine is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 *
 * video_out_vdpau.c, a video output plugin
 * using VDPAU (Video Decode and Presentation Api for Unix)
 *
 *
 */

/* #define LOG */
#define LOG_MODULE "video_out_vdpau"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <ctype.h>
#include <pthread.h>

#include <xine.h>
#include <xine/video_out.h>
#include <xine/vo_scale.h>
#include <xine/xine_internal.h>
#include <xine/xineutils.h>

#include <vdpau/vdpau_x11.h>
#include "accel_vdpau.h"



#define NUM_FRAMES_BACK 1

#ifndef HAVE_THREAD_SAFE_X11
#define LOCKDISPLAY /*define this if you have a buggy libX11/xcb*/
#endif

typedef enum {
  DEINT_NONE = 0,
  DEINT_BOB,
  DEINT_HALF_TEMPORAL,
  DEINT_HALF_TEMPORAL_SPATIAL,
  DEINT_TEMPORAL,
  DEINT_TEMPORAL_SPATIAL,
  DEINT_LAST
} _vdpau_deint_t;

#define NUMBER_OF_DEINTERLACERS 5

static const char *const vdpau_deinterlacer_name[] = {
  "bob",
  "half temporal",
  "half temporal_spatial",
  "temporal",
  "temporal_spatial",
  NULL
};

static const char *const vdpau_deinterlacer_description [] = {
  "bob\nBasic deinterlacing, doing 50i->50p.\n\n",
  "half temporal\nDisplays first field only, doing 50i->25p\n\n",
  "half temporal_spatial\nDisplays first field only, doing 50i->25p\n\n",
  "temporal\nVery good, 50i->50p\n\n",
  "temporal_spatial\nThe best, but very GPU intensive.\n\n",
  NULL
};


static const VdpOutputSurfaceRenderBlendState blend = {
  VDP_OUTPUT_SURFACE_RENDER_BLEND_STATE_VERSION,
  VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ONE,
  VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
  VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ONE,
  VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
  VDP_OUTPUT_SURFACE_RENDER_BLEND_EQUATION_ADD,
  VDP_OUTPUT_SURFACE_RENDER_BLEND_EQUATION_ADD,
  { 0.f, 0.f, 0.f, 0.f }
};


typedef struct {
  VdpOutputSurface  surface;
  uint32_t          width, height, size;
} vdpau_output_surface_t;

typedef struct {
  VdpVideoSurface surface;
  VdpChromaType   chroma;
  uint32_t        width, height, a_width, a_height;
} vdpau_video_surface_t;

typedef struct {
  xine_grab_video_frame_t grab_frame;

  vo_driver_t *vo_driver;
  vdpau_output_surface_t render_surface;
  int width, height;
  uint32_t *rgba;
} vdpau_grab_video_frame_t;


typedef struct {
  vo_frame_t         vo_frame;

  int                width, height, format, flags;
  double             ratio;

  vdpau_video_surface_t surf;

  int                surface_cleared_nr;

  vdpau_accel_t     vdpau_accel_data;
} vdpau_frame_t;


typedef struct {

  int               x;             /* x start of subpicture area       */
  int               y;             /* y start of subpicture area       */
  int               width;         /* width of subpicture area         */
  int               height;        /* height of subpicture area        */

  /* area within osd extent to scale video to */
  int               video_window_x;
  int               video_window_y;
  int               video_window_width;
  int               video_window_height;

  /* extent of reference coordinate system */
  int               extent_width;
  int               extent_height;

  int               unscaled;       /* true if it should be blended unscaled */
  int               use_dirty_rect; /* true if update of dirty rect only is possible */

  vo_overlay_t      *ovl;

  vdpau_output_surface_t render_surface;
} vdpau_overlay_t;


typedef struct {
  int         id;
  const char *name;
} vdpau_func_t;

static const vdpau_func_t vdpau_funcs[] = {
  {  VDP_FUNC_ID_PREEMPTION_CALLBACK_REGISTER, "VdpPreemptionCallbackRegister" },
  {  VDP_FUNC_ID_DEVICE_DESTROY, "VdpDeviceDestroy" },

  {  VDP_FUNC_ID_GET_ERROR_STRING, "VdpGetErrorString" },
  {  VDP_FUNC_ID_GET_API_VERSION, "VdpGetApiVersion" },
  {  VDP_FUNC_ID_GET_INFORMATION_STRING, "VdpGetInformationString" },

  {  VDP_FUNC_ID_VIDEO_SURFACE_QUERY_CAPABILITIES, "VdpVideoSurfaceQueryCapabilities" },
  {  VDP_FUNC_ID_VIDEO_SURFACE_QUERY_GET_PUT_BITS_Y_CB_CR_CAPABILITIES, "VdpVideoSurfaceQueryGetPutBitsYCbCrCapabilities" },
  {  VDP_FUNC_ID_VIDEO_SURFACE_CREATE, "VdpVideoSurfaceCreate" },
  {  VDP_FUNC_ID_VIDEO_SURFACE_DESTROY, "VdpVideoSurfaceDestroy" },
  {  VDP_FUNC_ID_VIDEO_SURFACE_PUT_BITS_Y_CB_CR, "VdpVideoSurfacePutBitsYCbCr" },
  {  VDP_FUNC_ID_VIDEO_SURFACE_GET_BITS_Y_CB_CR, "VdpVideoSurfaceGetBitsYCbCr" },
  {  VDP_FUNC_ID_VIDEO_SURFACE_GET_PARAMETERS, "VdpVideoSurfaceGetParameters" },

  {  VDP_FUNC_ID_OUTPUT_SURFACE_QUERY_CAPABILITIES, "VdpOutputSurfaceQueryCapabilities" },
  {  VDP_FUNC_ID_OUTPUT_SURFACE_QUERY_GET_PUT_BITS_NATIVE_CAPABILITIES, "VdpOutputSurfaceQueryGetPutBitsNativeCapabilities" },
  {  VDP_FUNC_ID_OUTPUT_SURFACE_QUERY_PUT_BITS_Y_CB_CR_CAPABILITIES, "VdpOutputSurfaceQueryPutBitsYCbCrCapabilities" },
  {  VDP_FUNC_ID_OUTPUT_SURFACE_CREATE, "VdpOutputSurfaceCreate" },
  {  VDP_FUNC_ID_OUTPUT_SURFACE_DESTROY, "VdpOutputSurfaceDestroy" },
  {  VDP_FUNC_ID_OUTPUT_SURFACE_RENDER_OUTPUT_SURFACE, "VdpOutputSurfaceRenderOutputSurface" },
  {  VDP_FUNC_ID_OUTPUT_SURFACE_PUT_BITS_NATIVE, "VdpOutputSurfacePutBitsNative"},
  {  VDP_FUNC_ID_OUTPUT_SURFACE_PUT_BITS_Y_CB_CR, "VdpOutputSurfacePutBitsYCbCr" },
  {  VDP_FUNC_ID_OUTPUT_SURFACE_GET_BITS_NATIVE, "VdpOutputSurfaceGetBitsNative" },

  {  VDP_FUNC_ID_VIDEO_MIXER_CREATE, "VdpVideoMixerCreate" },
  {  VDP_FUNC_ID_VIDEO_MIXER_DESTROY, "VdpVideoMixerDestroy" },
  {  VDP_FUNC_ID_VIDEO_MIXER_RENDER, "VdpVideoMixerRender" },
  {  VDP_FUNC_ID_VIDEO_MIXER_SET_ATTRIBUTE_VALUES, "VdpVideoMixerSetAttributeValues" },
  {  VDP_FUNC_ID_VIDEO_MIXER_SET_FEATURE_ENABLES, "VdpVideoMixerSetFeatureEnables" },
  {  VDP_FUNC_ID_VIDEO_MIXER_GET_FEATURE_ENABLES, "VdpVideoMixerGetFeatureEnables" },
  {  VDP_FUNC_ID_VIDEO_MIXER_QUERY_FEATURE_SUPPORT, "VdpVideoMixerQueryFeatureSupport" },
  {  VDP_FUNC_ID_VIDEO_MIXER_QUERY_PARAMETER_SUPPORT, "VdpVideoMixerQueryParameterSupport" },
  {  VDP_FUNC_ID_VIDEO_MIXER_QUERY_ATTRIBUTE_SUPPORT, "VdpVideoMixerQueryAttributeSupport" },
  {  VDP_FUNC_ID_VIDEO_MIXER_QUERY_PARAMETER_VALUE_RANGE, "VdpVideoMixerQueryParameterValueRange" },
  {  VDP_FUNC_ID_VIDEO_MIXER_QUERY_ATTRIBUTE_VALUE_RANGE, "VdpVideoMixerQueryAttributeValueRange" },

  {  VDP_FUNC_ID_GENERATE_CSC_MATRIX, "VdpGenerateCscMatrix" },

  {  VDP_FUNC_ID_PRESENTATION_QUEUE_TARGET_CREATE_X11, "VdpPresentationQueueTargetCreateX11" },
  {  VDP_FUNC_ID_PRESENTATION_QUEUE_TARGET_DESTROY, "VdpPresentationQueueTargetDestroy" },
  {  VDP_FUNC_ID_PRESENTATION_QUEUE_CREATE, "VdpPresentationQueueCreate" },
  {  VDP_FUNC_ID_PRESENTATION_QUEUE_DESTROY, "VdpPresentationQueueDestroy" },
  {  VDP_FUNC_ID_PRESENTATION_QUEUE_DISPLAY, "VdpPresentationQueueDisplay" },
  {  VDP_FUNC_ID_PRESENTATION_QUEUE_BLOCK_UNTIL_SURFACE_IDLE, "VdpPresentationQueueBlockUntilSurfaceIdle" },
  {  VDP_FUNC_ID_PRESENTATION_QUEUE_SET_BACKGROUND_COLOR, "VdpPresentationQueueSetBackgroundColor" },
  {  VDP_FUNC_ID_PRESENTATION_QUEUE_GET_TIME, "VdpPresentationQueueGetTime" },
  {  VDP_FUNC_ID_PRESENTATION_QUEUE_QUERY_SURFACE_STATUS, "VdpPresentationQueueQuerySurfaceStatus" },

  {  VDP_FUNC_ID_DECODER_QUERY_CAPABILITIES, "VdpDecoderQueryCapabilities" },
  {  VDP_FUNC_ID_DECODER_CREATE, "VdpDecoderCreate" },
  {  VDP_FUNC_ID_DECODER_DESTROY, "VdpDecoderDestroy" },
  {  VDP_FUNC_ID_DECODER_RENDER, "VdpDecoderRender" },

  {  0, NULL }
};

typedef struct {

  vo_driver_t        vo_driver;

  VdpDevice vdp_device;
  VdpPresentationQueue vdp_queue;
  VdpPresentationQueueTarget vdp_queue_target;

  VdpGetProcAddress *vdp_get_proc_address;
  /* vdpau api funcs - same order as in vdpau_funcs!! */
  union {
    void *funcs[46];
    struct {
      VdpPreemptionCallbackRegister *preemption_callback_register;
      struct {
        VdpDeviceDestroy *destroy;
      } device;
      struct {
        VdpGetErrorString *error_string;
        VdpGetApiVersion *api_version;
        VdpGetInformationString *information_string;
      } get;
      struct {
        VdpVideoSurfaceQueryCapabilities *query_capabilities;
        VdpVideoSurfaceQueryGetPutBitsYCbCrCapabilities *query_get_put_bits_ycbcr_capabilities;
        VdpVideoSurfaceCreate *create;
        VdpVideoSurfaceDestroy *destroy;
        VdpVideoSurfacePutBitsYCbCr *putbits_ycbcr;
        VdpVideoSurfaceGetBitsYCbCr *getbits_ycbcr;
        VdpVideoSurfaceGetParameters *get_parameters;
      } video_surface;
      struct {
        VdpOutputSurfaceQueryCapabilities *query_capabilities;
        VdpOutputSurfaceQueryGetPutBitsNativeCapabilities  *query_get_put_bits_native_capabilities;
        VdpOutputSurfaceQueryPutBitsYCbCrCapabilities *query_put_bits_ycbcr_capabilities;
        VdpOutputSurfaceCreate *create;
        VdpOutputSurfaceDestroy *destroy;
        VdpOutputSurfaceRenderOutputSurface *render_output_surface;
        VdpOutputSurfacePutBitsNative *put_bits;
        VdpOutputSurfacePutBitsYCbCr *put_bits_ycbcr;
        VdpOutputSurfaceGetBitsNative *get_bits;
      } output_surface;
      struct {
        VdpVideoMixerCreate *create;
        VdpVideoMixerDestroy *destroy;
        VdpVideoMixerRender *render;
        VdpVideoMixerSetAttributeValues *set_attribute_values;
        VdpVideoMixerSetFeatureEnables *set_feature_enables;
        VdpVideoMixerGetFeatureEnables *get_feature_enables;
        VdpVideoMixerQueryFeatureSupport *query_feature_support;
        VdpVideoMixerQueryParameterSupport *query_parameter_support;
        VdpVideoMixerQueryAttributeSupport *query_attribute_support;
        VdpVideoMixerQueryParameterValueRange *query_parameter_value_range;
        VdpVideoMixerQueryAttributeValueRange *query_attribute_value_range;
      } video_mixer;
      VdpGenerateCSCMatrix *generate_csc_matrix;
      struct {
        VdpPresentationQueueTargetCreateX11 *target_create_x11;
        VdpPresentationQueueTargetDestroy *target_destroy;
        VdpPresentationQueueCreate *create;
        VdpPresentationQueueDestroy *destroy;
        VdpPresentationQueueDisplay *display;
        VdpPresentationQueueBlockUntilSurfaceIdle *block;
        VdpPresentationQueueSetBackgroundColor *set_background_color;
        VdpPresentationQueueGetTime *get_time;
        VdpPresentationQueueQuerySurfaceStatus *query_surface_status;
      } queue;
      struct {
        VdpDecoderQueryCapabilities *query_capabilities;
        VdpDecoderCreate *create;
        VdpDecoderDestroy *destroy;
        VdpDecoderRender *render;
      } decoder;
    } vdp;
  } a;

  vo_scale_t         sc;

  Display           *display;
  int                screen;
  Drawable           drawable;
  pthread_mutex_t    drawable_lock;
  uint32_t           display_width;
  uint32_t           display_height;

  int                 ovl_changed;
  int                 num_ovls;
  int                 old_num_ovls;
  vdpau_overlay_t     overlays[XINE_VORAW_MAX_OVL];
  uint32_t           *ovl_pixmap;
  int                 ovl_pixmap_size;
  VdpOutputSurface    ovl_layer_surface;
  VdpRect             ovl_src_rect;
  VdpRect             ovl_dest_rect;
  VdpRect             ovl_video_dest_rect;
  vdpau_output_surface_t ovl_main_render_surface;

  vdpau_video_surface_t soft_surface;
  int                  soft_surface_format;

#define NOUTPUTSURFACEBUFFER  25
  vdpau_output_surface_t output_surface_buffer[NOUTPUTSURFACEBUFFER];
  int                  output_surface_buffer_size;
  int                  num_big_output_surfaces_created;
  pthread_mutex_t      os_mutex;

#define NOUTPUTSURFACE 8
  vdpau_output_surface_t output_surfaces[NOUTPUTSURFACE];
  uint8_t              init_queue;
  uint8_t              queue_user_length;
  uint8_t              queue_length;
  uint8_t              current_output_surface;

  vdpau_grab_video_frame_t *pending_grab_request;
  pthread_mutex_t      grab_lock;
  pthread_cond_t       grab_cond;

  struct {
    VdpVideoMixer      handle;
    VdpChromaType      chroma;
    int                layer_bug;
    uint32_t           width, height;
    uint32_t           max_width, max_height, max_layers[4];
  }                    video_mixer;

  const char*          deinterlacers_name[NUMBER_OF_DEINTERLACERS+1];
  int                  deinterlacers_method[NUMBER_OF_DEINTERLACERS];

  int                  scaling_level_max;
  int                  scaling_level_current;

  VdpColor             back_color;

  vdpau_frame_t        *back_frame[ NUM_FRAMES_BACK ];

  uint32_t          capabilities, features;
  xine_t            *xine;

#define _VOVDP_S_BRIGHTNESS  0x0001
#define _VOVDP_S_CONTRAST    0x0002
#define _VOVDP_S_SATURATION  0x0004
#define _VOVDP_S_HUE         0x0008
#define _VOVDP_S_SHARPNESS   0x0010
#define _VOVDP_S_NOISE_RED   0x0020
#define _VOVDP_S_CSC         0x0040
#define _VOVDP_S_TRANSFORM   0x0080
#define _VOVDP_S_SCALE_LEVEL 0x0100
#define _VOVDP_S_DEINT       0x0200
#define _VOVDP_S_NO_CHROMA   0x0400
#define _VOVDP_S_INV_TELE    0x0800
#define _VOVDP_S_BGCOLOR     0x1000
#define _VOVDP_S_ALL         0x1fff
  uint32_t          prop_changed;

  int               transform;
  int               hue;
  int               saturation;
  int               brightness;
  int               contrast;
  int               sharpness;
  int               noise;
  int               deinterlace;
  int               deinterlace_method_hd;
  int               deinterlace_method_sd;
  int               enable_inverse_telecine;
  int               honor_progressive;
  int               skip_chroma;
  int               sd_only_properties;
  int               background;

  int               vdp_runtime_nr;
  int               reinit_needed;

  int               surface_cleared_nr;

  int               allocated_surfaces;
  int		            zoom_x;
  int		            zoom_y;

  int               color_matrix;
  int               cm_state;
  uint8_t           cm_lut[32];
} vdpau_driver_t;

/* import common color matrix stuff */
#define CM_LUT
#define CM_HAVE_YCGCO_SUPPORT 1
#define CM_HAVE_BT2020_SUPPORT 1
#define CM_DRIVER_T vdpau_driver_t
#include "color_matrix.c"


typedef struct {
  video_driver_class_t driver_class;
  xine_t              *xine;
} vdpau_class_t;

typedef struct {
  uint32_t val1, val2;
  uint8_t func;
  char name[31];
} _vdpau_features_t;

#ifndef VDP_DECODER_PROFILE_MPEG4_PART2_ASP
#  define VDP_DECODER_PROFILE_MPEG4_PART2_ASP 13
#endif

typedef enum {
  _VOVDP_V_yuv422 = 0,
  _VOVDP_V_yuv420,
  _VOVDP_V_yuy2,
  _VOVDP_V_yv12,
  _VOVDP_O_rgba,
  _VOVDP_O_rgba_soft,
  _VOVDP_O_yuv,
  _VOVDP_D_h264,
  _VOVDP_D_vc1,
  _VOVDP_D_mpeg12,
  _VOVDP_D_mpeg4,
  _VOVDP_M_noise_reduction,
  _VOVDP_M_sharpness,
  _VOVDP_I_temporal,
  _VOVDP_I_temporal_spatial,
  _VOVDP_I_inverse_telecine,
  _VOVDP_I_no_chroma,
  _VOVDP_A_background_color,
  _VOVDP_A_color_matrix,
  _VOVDP_LAST
} _vdpau_feature_t;

static const _vdpau_features_t vdpau_feature_list[] = {
  [_VOVDP_V_yuv422]           = { VDP_CHROMA_TYPE_422,                 0,                         1, "video_yuv422" },
  [_VOVDP_V_yuv420]           = { VDP_CHROMA_TYPE_420,                 0,                         1, "video_yuv420" },
  [_VOVDP_V_yuy2]             = { VDP_CHROMA_TYPE_422,                 VDP_YCBCR_FORMAT_YUYV,     2, "video_yuy2" },
  [_VOVDP_V_yv12]             = { VDP_CHROMA_TYPE_420,                 VDP_YCBCR_FORMAT_YV12,     2, "video_yv12" },
  [_VOVDP_O_rgba]             = { VDP_RGBA_FORMAT_B8G8R8A8,            0,                         3, "output_rgba" },
  [_VOVDP_O_rgba_soft]        = { VDP_RGBA_FORMAT_B8G8R8A8,            0,                         4, "output_rgba_soft" },
  [_VOVDP_O_yuv]              = { VDP_RGBA_FORMAT_B8G8R8A8,            VDP_YCBCR_FORMAT_V8U8Y8A8, 5, "output_yuv" },
  [_VOVDP_D_h264]             = { VDP_DECODER_PROFILE_H264_MAIN,       VO_CAP_VDPAU_H264,         6, "decode_h264" },
  [_VOVDP_D_vc1]              = { VDP_DECODER_PROFILE_VC1_MAIN,        VO_CAP_VDPAU_VC1,          6, "decode_vc1" },
  [_VOVDP_D_mpeg12]           = { VDP_DECODER_PROFILE_MPEG2_MAIN,      VO_CAP_VDPAU_MPEG12,       6, "decode_mpeg12" },
  [_VOVDP_D_mpeg4]            = { VDP_DECODER_PROFILE_MPEG4_PART2_ASP, VO_CAP_VDPAU_MPEG4,        6, "decode_mpeg4" },
  [_VOVDP_M_noise_reduction]  = { VDP_VIDEO_MIXER_FEATURE_NOISE_REDUCTION,VO_CAP_NOISE_REDUCTION, 7, "mix_noise_reduction" },
  [_VOVDP_M_sharpness]        = { VDP_VIDEO_MIXER_FEATURE_SHARPNESS,   VO_CAP_SHARPNESS,          7, "mix_sharpness" },
  [_VOVDP_I_temporal]         = { VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL, 0,                7, "deint_temporal" },
  [_VOVDP_I_temporal_spatial] = { VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL_SPATIAL, 0,        7, "deint_temporal_spatial" },
  [_VOVDP_I_inverse_telecine] = { VDP_VIDEO_MIXER_FEATURE_INVERSE_TELECINE, 0,                    7, "deint_inverse_telecine" },
  [_VOVDP_I_no_chroma]        = { VDP_VIDEO_MIXER_ATTRIBUTE_SKIP_CHROMA_DEINTERLACE, 0,           8, "deint_no_chroma" },
  [_VOVDP_A_background_color] = { VDP_VIDEO_MIXER_ATTRIBUTE_BACKGROUND_COLOR, 0,                  8, "attr_background_color" },
  [_VOVDP_A_color_matrix]     = { VDP_VIDEO_MIXER_ATTRIBUTE_CSC_MATRIX, 0,                        8, "attr_color_matrix" },
  [_VOVDP_LAST]               = { 0,                                   0,                         0, "" }
};

static const uint32_t _vdpau_required_features =
  (1 << _VOVDP_V_yuv422) |
  (1 << _VOVDP_V_yuv420) |
  (1 << _VOVDP_V_yuy2) |
  (1 << _VOVDP_V_yv12) |
  (1 << _VOVDP_O_rgba) |
  (1 << _VOVDP_O_rgba_soft) |
  (1 << _VOVDP_O_yuv);

static int _vdpau_feature_have (vdpau_driver_t *this, _vdpau_feature_t feature) {
  return (this->features & (1 << feature)) ? 1 : 0;
}

static void _vdpau_feature_names (char *buf, size_t bsize, uint32_t features) {
  char *p = buf, *e = buf + bsize;
  uint32_t u, bit;

  for (u = 0, bit = 1; u < sizeof(vdpau_feature_list)/sizeof(vdpau_feature_list[0]); u++, bit <<= 1) {
    if (features & bit)
      p += snprintf (p, e - p, "%s ", vdpau_feature_list[u].name);
  }
  if (p > buf)
    p--;
  *p = 0;
}

static void _vdpau_video_mixer_test (vdpau_driver_t *this, VdpVideoMixerParameter p, uint32_t *max_value) {
  uint32_t mx, my = 0;
  VdpStatus st;
  VdpBool ok;

  *max_value = 0;
  ok = VDP_FALSE;
  st = this->a.vdp.video_mixer.query_parameter_support (this->vdp_device, p, &ok);
  if ((st == VDP_STATUS_OK) && (ok != VDP_FALSE)) {
    st = this->a.vdp.video_mixer.query_parameter_value_range (this->vdp_device, p, &mx, &my);
    if (st == VDP_STATUS_OK)
      *max_value = my;
  }
}

static void _vdpau_feature_test (vdpau_driver_t *this) {
  char buf[1024], *p = buf, *e = buf + sizeof (buf);
  const _vdpau_features_t *f;
  uint32_t res = 0, bit, mx, my;
  VdpStatus st;
  VdpBool ok;

  {
    const char *s;
    this->video_mixer.max_layers[2] = ~0u;
    st = this->a.vdp.get.information_string (&s);
    if (st == VDP_STATUS_OK) {
      if (strstr (s, "G3DVL VDPAU"))
        this->video_mixer.max_layers[2] = 0;
    } else {
      s = this->a.vdp.get.error_string (st);
    }
    xprintf (this->xine, XINE_VERBOSITY_LOG, LOG_MODULE ": implementation: %s\n", s);
  }

  for (bit = 1, f = vdpau_feature_list; f->func; bit <<= 1, f++) {
    uint32_t ml = 0, mr = 0;
    mx = my = 0;
    ok = VDP_FALSE;
    switch (f->func) {
      case 1:
        st = this->a.vdp.video_surface.query_capabilities (this->vdp_device, f->val1, &ok, &mx, &my);
        break;
      case 2:
        st = this->a.vdp.video_surface.query_get_put_bits_ycbcr_capabilities (this->vdp_device, f->val1, f->val2, &ok);
        break;
      case 3:
        st = this->a.vdp.output_surface.query_capabilities (this->vdp_device, f->val1, &ok, &mx, &my);
        break;
      case 4:
        st = this->a.vdp.output_surface.query_get_put_bits_native_capabilities (this->vdp_device, f->val1, &ok);
        break;
      case 5:
        st = this->a.vdp.output_surface.query_put_bits_ycbcr_capabilities (this->vdp_device, f->val1, f->val2, &ok);
        break;
      case 6:
        st = this->a.vdp.decoder.query_capabilities (this->vdp_device, f->val1, &ok, &ml, &mr, &mx, &my);
        break;
      case 7:
        st = this->a.vdp.video_mixer.query_feature_support (this->vdp_device, f->val1, &ok);
        break;
      case 8:
        st = this->a.vdp.video_mixer.query_attribute_support (this->vdp_device, f->val1, &ok);
        break;
      default:
        continue;
    }
    if (st != VDP_STATUS_OK) {
      xprintf (this->xine, XINE_VERBOSITY_LOG, LOG_MODULE ": failed to test %s: %s.\n",
        f->name, this->a.vdp.get.error_string (st));
    } else if (ok != VDP_FALSE) {
      res |= bit;
      if ((f->func == 6) || (f->func == 7))
        this->capabilities |= f->val2;
      if (mx && my) {
        p += snprintf (p, e - p, "%s(%ux%u) ", f->name, (unsigned int)mx, (unsigned int)my);
      } else {
        p += snprintf (p, e - p, "%s ", f->name);
      }
    }
  }

#ifdef VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1
  for (mx = 0; mx < 9; mx++) {
    ok = VDP_FALSE;
    st = this->a.vdp.video_mixer.query_feature_support (this->vdp_device,
      VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1 + mx, &ok);
    if ((st != VDP_STATUS_OK) || (ok == VDP_FALSE))
      break;
  }
  this->scaling_level_max = mx;
  p += snprintf (p, e - p, "mix_scale_level_%u ", (unsigned int)mx);
#endif

  _vdpau_video_mixer_test (this, VDP_VIDEO_MIXER_PARAMETER_VIDEO_SURFACE_WIDTH, &this->video_mixer.max_width);
  _vdpau_video_mixer_test (this, VDP_VIDEO_MIXER_PARAMETER_VIDEO_SURFACE_HEIGHT, &this->video_mixer.max_height);
  _vdpau_video_mixer_test (this, VDP_VIDEO_MIXER_PARAMETER_LAYERS, &this->video_mixer.max_layers[0]);
  if (this->video_mixer.max_width && this->video_mixer.max_height)
    p += snprintf (p, e - p, "mix(%ux%u) ",
      (unsigned int)this->video_mixer.max_width, (unsigned int)this->video_mixer.max_height);
  if (this->video_mixer.max_layers[0])
    p += snprintf (p, e - p, "mix_layers(%u) ", (unsigned int)this->video_mixer.max_layers[0]);
  /* for performance, we join OSD layers before use. */
  this->video_mixer.max_layers[0] = this->video_mixer.max_layers[0] ? 1 : 0;
  this->video_mixer.max_layers[1] = 0;
  this->video_mixer.max_layers[2] &= this->video_mixer.max_layers[0];
  this->video_mixer.max_layers[3] = 0;

  if (p > buf)
    p--;
  *p = 0;
  xprintf (this->xine, XINE_VERBOSITY_DEBUG, LOG_MODULE ": features available: %s.\n", buf);
  this->features = res;
}
  
static void vdpau_oslock_lock (vdpau_driver_t *this) {
  pthread_mutex_lock (&this->os_mutex);
}

static void vdpau_oslock_unlock (vdpau_driver_t *this) {
  pthread_mutex_unlock (&this->os_mutex);
}

#ifdef LOCKDISPLAY
#define DO_LOCKDISPLAY(this)    XLockDisplay (this->display)
#define DO_UNLOCKDISPLAY(this)  XUnlockDisplay (this->display)
static void vdpau_lockdisplay (vo_frame_t *frame) {
  vdpau_driver_t *this = (vdpau_driver_t *)frame->driver;
  XLockDisplay (this->display);
}
static void vdpau_unlockdisplay (vo_frame_t *frame) {
  vdpau_driver_t *this = (vdpau_driver_t *)frame->driver;
  XUnlockDisplay (this->display);
}
#else
#define DO_LOCKDISPLAY(this)
#define DO_UNLOCKDISPLAY(this)
#endif

#define VDPAU_IF_ERROR(msg) \
  if (st != VDP_STATUS_OK) \
    xprintf (this->xine, XINE_VERBOSITY_LOG, LOG_MODULE ": " msg ": %s.\n", this->a.vdp.get.error_string (st))
#define VDPAU_ERROR(msg) \
    xprintf (this->xine, XINE_VERBOSITY_LOG, LOG_MODULE ": " msg ": %s.\n", this->a.vdp.get.error_string (st))

static void vdpau_video_surf_delete (vdpau_driver_t *this, vdpau_video_surface_t *s) {
  if (s->surface != VDP_INVALID_HANDLE) {
    DO_LOCKDISPLAY (this);
    this->a.vdp.video_surface.destroy (s->surface);
    DO_UNLOCKDISPLAY (this);
    xprintf (this->xine, XINE_VERBOSITY_DEBUG,
      LOG_MODULE ": deleted video surface #%u.\n", (unsigned int)s->surface);
    s->surface = VDP_INVALID_HANDLE;
  }
}

static VdpStatus vdpau_video_surf_new (vdpau_driver_t *this, vdpau_video_surface_t *s) {
  VdpStatus st;

  DO_LOCKDISPLAY (this);
  st = this->a.vdp.video_surface.create (this->vdp_device, s->chroma, s->width, s->height, &s->surface);
  DO_UNLOCKDISPLAY (this);
  if (st != VDP_STATUS_OK) {
    xprintf (this->xine, XINE_VERBOSITY_LOG,
      LOG_MODULE ": failed to create %s video surface %u x %u: %s!!\n",
      s->chroma == VDP_CHROMA_TYPE_422 ? "4:2:2" : "4:2:0",
      (unsigned int)s->width, (unsigned int)s->height, this->a.vdp.get.error_string (st));
    return st;
  }

  s->a_width = 0;
  s->a_height = 0;
  st = this->a.vdp.video_surface.get_parameters (s->surface, &s->chroma, &s->a_width, &s->a_height);
  if (st != VDP_STATUS_OK) {
    s->a_width = s->width;
    s->a_height = s->height;
    xprintf (this->xine, XINE_VERBOSITY_LOG,
      LOG_MODULE ": failed to get video surface #%u parameters!!\n", (unsigned int)s->surface);
    return VDP_STATUS_OK;
  }

  if ((s->a_width < s->width) || (s->a_height < s->height)) {
    xprintf (this->xine, XINE_VERBOSITY_LOG,
      LOG_MODULE ": video surface #%u size mismatch (%u x %u) != (%u x %u). Segfaults ahead!\n",
      (unsigned int)s->surface,
      (unsigned int)s->width, (unsigned int)s->height,
      (unsigned int)s->a_width, (unsigned int)s->a_height);
  } else if ((s->a_width != s->width) || (s->a_height != s->height)) {
    xprintf (this->xine, XINE_VERBOSITY_DEBUG,
      LOG_MODULE ": video surface #%u (%u x %u) aligned to (%u x %u).\n",
      (unsigned int)s->surface,
      (unsigned int)s->width, (unsigned int)s->height,
      (unsigned int)s->a_width, (unsigned int)s->a_height);
  } else {
    xprintf (this->xine, XINE_VERBOSITY_DEBUG,
      LOG_MODULE ": video surface #%u (%u x %u).\n",
      (unsigned int)s->surface,
      (unsigned int)s->width, (unsigned int)s->height);
  }

  return VDP_STATUS_OK;
}

static void vdpau_output_surf_delete (vdpau_driver_t *this, vdpau_output_surface_t *s) {
  if (s->surface != VDP_INVALID_HANDLE) {
    DO_LOCKDISPLAY (this);
    this->a.vdp.output_surface.destroy (s->surface);
    DO_UNLOCKDISPLAY (this);
    xprintf (this->xine, XINE_VERBOSITY_DEBUG,
      LOG_MODULE ": deleted output surface #%u.\n", (unsigned int)s->surface);
    s->surface = VDP_INVALID_HANDLE;
  }
}

static VdpStatus vdpau_output_surf_new (vdpau_driver_t *this, vdpau_output_surface_t *s) {
  VdpStatus st;

  DO_LOCKDISPLAY (this);
  st = this->a.vdp.output_surface.create (this->vdp_device, VDP_RGBA_FORMAT_B8G8R8A8, s->width, s->height, &s->surface);
  DO_UNLOCKDISPLAY (this);
  if (st != VDP_STATUS_OK) {
    xprintf (this->xine, XINE_VERBOSITY_LOG,
    LOG_MODULE ": failed to create output surface %u x %u: %s!!\n",
    (unsigned int)s->width, (unsigned int)s->height, this->a.vdp.get.error_string (st));
    return st;
  }
  s->size = s->width * s->height;
  xprintf (this->xine, XINE_VERBOSITY_DEBUG,
    LOG_MODULE ": output surface #%u (%u x %u).\n",
    (unsigned int)s->surface, (unsigned int)s->width, (unsigned int)s->height);
  return VDP_STATUS_OK;
}

static VdpStatus vdpau_get_output_surface (vdpau_driver_t *this, uint32_t width, uint32_t height, vdpau_output_surface_t *r)
{
  int i, full = 1;
  vdpau_output_surface_t *smallest = NULL, *best = NULL;
  vdpau_output_surface_t *l = &this->output_surface_buffer[0];
  VdpStatus st = VDP_STATUS_OK;

  vdpau_oslock_lock (this);

  for (i = this->output_surface_buffer_size; i; --i, ++l) {
    if (l->surface == VDP_INVALID_HANDLE)
      full = 0;
    else if ((l->width >= width && l->height >= height) && (best == NULL || l->size < best->size))
      best = l;
    else if (smallest == NULL || l->size < smallest->size)
      smallest = l;
  }

  if (best != NULL) {
    *r = *best;
    best->surface = VDP_INVALID_HANDLE;
  } else if (full) {
    *r = *smallest;
    smallest->surface = VDP_INVALID_HANDLE;
  } else
    r->surface = VDP_INVALID_HANDLE;

  vdpau_oslock_unlock (this);

  if (r->surface != VDP_INVALID_HANDLE && (r->width < width || r->height < height))
    vdpau_output_surf_delete (this, r);

  if (r->surface == VDP_INVALID_HANDLE) {
    if (this->num_big_output_surfaces_created < this->output_surface_buffer_size) {
        /* We create big output surfaces which should fit for many output buffer requests as long
         * as the reuse buffer can hold them */
      if (width < this->video_mixer.width)
        width = this->video_mixer.width;
      if (height < this->video_mixer.height)
        height = this->video_mixer.height;

      if (width < this->display_width)
        width = this->display_width;
      if (height < this->display_height)
        height = this->display_height;

      ++this->num_big_output_surfaces_created;
    }

    r->width = width;
    r->height = height;
    st = vdpau_output_surf_new (this, r);
  }

  return st;
}


static void vdpau_free_output_surface (vdpau_driver_t *this, vdpau_output_surface_t *os)
{
  vdpau_output_surface_t *smallest = NULL, *l = &this->output_surface_buffer[0], temp;
  int i;

  if (os->surface == VDP_INVALID_HANDLE)
    return;

  vdpau_oslock_lock (this);

  for (i = this->output_surface_buffer_size; i; --i, ++l) {
    if (l->surface == VDP_INVALID_HANDLE) {
      *l = *os;
      vdpau_oslock_unlock (this);
      os->surface = VDP_INVALID_HANDLE;
      return;
    } else if (smallest == NULL || l->size < smallest->size)
      smallest = l;
  }

  if (smallest && smallest->size < os->size) {
    temp = *smallest;
    *smallest = *os;
    vdpau_oslock_unlock (this);
  } else {
    vdpau_oslock_unlock (this);
    temp = *os;
  }

  vdpau_output_surf_delete (this, &temp);
  os->surface = VDP_INVALID_HANDLE;
}


static void vdpau_overlay_begin (vo_driver_t *this_gen, vo_frame_t *frame_gen, int changed)
{
  vdpau_driver_t  *this = (vdpau_driver_t *) this_gen;

  (void)frame_gen;
  this->ovl_changed = changed;
  if ( changed ) {
    this->old_num_ovls = this->num_ovls;
    this->num_ovls = 0;
    lprintf("overlay begin\n");
  }
}


static void vdpau_overlay_blend (vo_driver_t *this_gen, vo_frame_t *frame_gen, vo_overlay_t *voovl)
{
  vdpau_driver_t  *this = (vdpau_driver_t *) this_gen;

  (void)frame_gen;
  if (!this->ovl_changed)
    return;

  int i = this->num_ovls;
  if (i >= XINE_VORAW_MAX_OVL)
    return;

  if (voovl->width <= 0 || voovl->height <= 0 || voovl->width > 32768 || voovl->height > 32768)
    return;

  if (!voovl->rle && (!voovl->argb_layer || !voovl->argb_layer->buffer))
    return;

  if (voovl->rle)
    lprintf("overlay[%d] rle %s%s %dx%d@%d,%d  extend %dx%d  hili %d,%d-%d,%d  video %dx%d@%d,%d\n", i,
                  voovl->unscaled ? " unscaled ": " scaled ",
                  (voovl->rgb_clut > 0 || voovl->hili_rgb_clut > 0) ? " rgb ": " ycbcr ",
                  voovl->width, voovl->height, voovl->x, voovl->y,
                  voovl->extent_width, voovl->extent_height,
                  voovl->hili_left, voovl->hili_top,
                  voovl->hili_right, voovl->hili_bottom,
                  voovl->video_window_width,voovl->video_window_height,
                  voovl->video_window_x,voovl->video_window_y);
  if (voovl->argb_layer && voovl->argb_layer->buffer)
    lprintf("overlay[%d] argb %s %dx%d@%d,%d  extend %dx%d, dirty %d,%d-%d,%d  video %dx%d@%d,%d\n", i,
                  voovl->unscaled ? " unscaled ": " scaled ",
                  voovl->width, voovl->height, voovl->x, voovl->y,
                  voovl->extent_width, voovl->extent_height,
                  voovl->argb_layer->x1, voovl->argb_layer->y1,
                  voovl->argb_layer->x2, voovl->argb_layer->y2,
                  voovl->video_window_width,voovl->video_window_height,
                  voovl->video_window_x,voovl->video_window_y);

  vdpau_overlay_t *ovl = &this->overlays[i];

  if (i >= this->old_num_ovls ||
      (ovl->use_dirty_rect &&
        (ovl->render_surface.surface == VDP_INVALID_HANDLE ||
        voovl->rle ||
        ovl->x != voovl->x || ovl->y != voovl->y ||
        ovl->width != voovl->width || ovl->height != voovl->height)))
    ovl->use_dirty_rect = 0;

  ovl->ovl = voovl;
  ovl->x = voovl->x;
  ovl->y = voovl->y;
  ovl->width = voovl->width;
  ovl->height = voovl->height;
  ovl->extent_width = voovl->extent_width;
  ovl->extent_height = voovl->extent_height;
  ovl->unscaled = voovl->unscaled;
  ovl->video_window_x = voovl->video_window_x;
  ovl->video_window_y = voovl->video_window_y;
  ovl->video_window_width = voovl->video_window_width;
  ovl->video_window_height = voovl->video_window_height;

  this->num_ovls = i + 1;
}


static void vdpau_overlay_end (vo_driver_t *this_gen, vo_frame_t *frame_gen)
{
  vdpau_driver_t  *this = (vdpau_driver_t *) this_gen;

  (void)frame_gen;
  if (!this->ovl_changed)
    return;

  int i;
  for (i = 0; i < this->old_num_ovls; ++i) {
    vdpau_overlay_t *ovl = &this->overlays[i];
    if (i >= this->num_ovls || !ovl->use_dirty_rect) {
      lprintf("overlay[%d] free render surface %d\n", i, (int)ovl->render_surface.surface);
      vdpau_free_output_surface(this, &ovl->render_surface);
    }
  }
  if (this->ovl_main_render_surface.surface != VDP_INVALID_HANDLE) {
    lprintf("overlay free main render surface %d\n", (int)this->ovl_main_render_surface.surface);
    vdpau_free_output_surface(this, &this->ovl_main_render_surface);
  }

  for (i = 0; i < this->num_ovls; ++i) {
    vdpau_overlay_t *ovl = &this->overlays[i];
    vo_overlay_t *voovl = ovl->ovl;
    int aw = ovl->width, ah = ovl->height;
    uint32_t *pixmap;

    if (!ovl->use_dirty_rect) {
      vdpau_get_output_surface(this, ovl->width, ovl->height, &ovl->render_surface);
      lprintf("overlay[%d] get render surface %dx%d -> %d\n", i, ovl->width, ovl->height, (int)ovl->render_surface.surface);
    }

    if (voovl->rle) {
      int pmsize;
      if (!voovl->rgb_clut || !voovl->hili_rgb_clut) {
        _x_overlay_clut_yuv2rgb (voovl, this->color_matrix);
      }
      aw = (aw + 31) & ~31;
      ah = (ah + 31) & ~31;
      pmsize = aw * ah;
      if (pmsize > this->ovl_pixmap_size) {
        this->ovl_pixmap_size = pmsize;
        xine_free_aligned (this->ovl_pixmap);
        this->ovl_pixmap = xine_mallocz_aligned (pmsize * sizeof (uint32_t));
        xprintf (this->xine, XINE_VERBOSITY_DEBUG, LOG_MODULE ": overlay argb buffer enlarged to %dx%d = %d.\n",
          aw, ah, pmsize);
      }
      pixmap = this->ovl_pixmap;
      if (!pixmap)
        continue;
      _x_overlay_to_argb32 (voovl, pixmap, aw, "BGRA");
    } else {
      pthread_mutex_lock(&voovl->argb_layer->mutex);
      pixmap = voovl->argb_layer->buffer;
    }

    VdpRect put_rect;
    if (ovl->use_dirty_rect) {
      put_rect.x0 = voovl->argb_layer->x1;
      put_rect.y0 = voovl->argb_layer->y1;
      put_rect.x1 = voovl->argb_layer->x2;
      put_rect.y1 = voovl->argb_layer->y2;
    } else {
      put_rect.x0 = 0;
      put_rect.y0 = 0;
      put_rect.x1 = ovl->width;
      put_rect.y1 = ovl->height;
    }

    VdpStatus st;
    uint32_t pitch = aw * sizeof (uint32_t);
    const void * const ppixmap = pixmap;
    lprintf("overlay[%d] put %s %d,%d:%d,%d\n", i, ovl->use_dirty_rect ? "dirty argb": "argb", put_rect.x0, put_rect.y0, put_rect.x1, put_rect.y1);
    st = this->a.vdp.output_surface.put_bits (ovl->render_surface.surface, &ppixmap, &pitch, &put_rect);
    VDPAU_IF_ERROR ("vdpau_overlay_end: vdp_output_surface_put_bits_native failed");

    if (voovl->rle)
      ovl->use_dirty_rect = 0;
    else {
      pthread_mutex_unlock(&voovl->argb_layer->mutex);
      ovl->use_dirty_rect = 1;
    }
  }
}


static void vdpau_process_overlays (vdpau_driver_t *this)
{
  int novls = this->num_ovls;
  if (!novls) {
    this->ovl_changed = 0;
    return;
  }

  int zoom = (this->sc.delivered_width > this->sc.displayed_width || this->sc.delivered_height > this->sc.displayed_height);

  VdpRect vid_src_rect;
  if (zoom) {
    /* compute displayed video window coordinates */
    vid_src_rect.x0 = this->sc.displayed_xoffset;
    vid_src_rect.y0 = this->sc.displayed_yoffset;
    vid_src_rect.x1 = this->sc.displayed_width + this->sc.displayed_xoffset;
    vid_src_rect.y1 = this->sc.displayed_height + this->sc.displayed_yoffset;
  }

  /* compute video window output coordinates */
  VdpRect vid_rect;
  vid_rect.x0 = this->sc.output_xoffset;
  vid_rect.y0 = this->sc.output_yoffset;
  vid_rect.x1 = this->sc.output_xoffset + this->sc.output_width;
  vid_rect.y1 = this->sc.output_yoffset + this->sc.output_height;
  this->ovl_video_dest_rect = vid_rect;

  VdpRect ovl_rects[XINE_VORAW_MAX_OVL], ovl_src_rects[XINE_VORAW_MAX_OVL];
  int i, first_visible = 0, nvisible = 0;
  for (i = 0; i < novls; ++i) {
    vdpau_overlay_t *ovl = &this->overlays[i];

    /* compute unscaled displayed overlay window coordinates */
    VdpRect ovl_src_rect;
    ovl_src_rect.x0 = 0;
    ovl_src_rect.y0 = 0;
    ovl_src_rect.x1 = ovl->width;
    ovl_src_rect.y1 = ovl->height;

    /* compute unscaled overlay window output coordinates */
    VdpRect ovl_rect;
    ovl_rect.x0 = ovl->x;
    ovl_rect.y0 = ovl->y;
    ovl_rect.x1 = ovl->x + ovl->width;
    ovl_rect.y1 = ovl->y + ovl->height;

    /* Note: Always coordinates of last overlay osd video window is taken into account */
    if (ovl->video_window_width > 0 && ovl->video_window_height > 0) {
      /* compute unscaled osd video window output coordinates */
      vid_rect.x0 = ovl->video_window_x;
      vid_rect.y0 = ovl->video_window_y;
      vid_rect.x1 = ovl->video_window_x + ovl->video_window_width;
      vid_rect.y1 = ovl->video_window_y + ovl->video_window_height;
      this->ovl_video_dest_rect = vid_rect;
    }

    if (!ovl->unscaled) {
      double rx, ry;

      if (zoom) {
        VdpRect clip_rect;
        if (ovl->extent_width > 0 && ovl->extent_height > 0) {
          /* compute frame size to extend size scaling factor */
          rx = (double)ovl->extent_width / (double)this->sc.delivered_width;
          ry = (double)ovl->extent_height / (double)this->sc.delivered_height;
          /* scale displayed video window coordinates to extend coordinates */
          clip_rect.x0 = vid_src_rect.x0 * rx + 0.5;
          clip_rect.y0 = vid_src_rect.y0 * ry + 0.5;
          clip_rect.x1 = vid_src_rect.x1 * rx + 0.5;
          clip_rect.y1 = vid_src_rect.y1 * ry + 0.5;
          /* compute displayed size to output size scaling factor */
          rx = (double)this->sc.output_width / (double)(clip_rect.x1 - clip_rect.x0);
          ry = (double)this->sc.output_height / (double)(clip_rect.y1 - clip_rect.y0);
        } else {
          clip_rect = vid_src_rect;
          /* compute displayed size to output size scaling factor */
          rx = (double)this->sc.output_width / (double)this->sc.displayed_width;
          ry = (double)this->sc.output_height / (double)this->sc.displayed_height;
        }
        /* clip overlay window to margins of displayed video window */
        if (ovl_rect.x0 < clip_rect.x0) {
          ovl_src_rect.x0 = clip_rect.x0 - ovl_rect.x0;
          ovl_rect.x0 = clip_rect.x0;
        }
        if (ovl_rect.y0 < clip_rect.y0) {
          ovl_src_rect.y0 = clip_rect.y0 - ovl_rect.y0;
          ovl_rect.y0 = clip_rect.y0;
        }
        if (ovl_rect.x1 > clip_rect.x1) {
          ovl_src_rect.x1 -= (ovl_rect.x1 - clip_rect.x1);
          ovl_rect.x1 = clip_rect.x1;
        }
        if (ovl_rect.y1 > clip_rect.y1) {
          ovl_src_rect.y1 -= (ovl_rect.y1 - clip_rect.y1);
          ovl_rect.y1 = clip_rect.y1;
        }
        ovl_rect.x0 -= clip_rect.x0;
        ovl_rect.y0 -= clip_rect.y0;
        ovl_rect.x1 -= clip_rect.x0;
        ovl_rect.y1 -= clip_rect.y0;
        /* scale overlay window coordinates to output window coordinates */
        ovl_rect.x0 = (double)ovl_rect.x0 * rx + 0.5;
        ovl_rect.y0 = (double)ovl_rect.y0 * ry + 0.5;
        ovl_rect.x1 = (double)ovl_rect.x1 * rx + 0.5;
        ovl_rect.y1 = (double)ovl_rect.y1 * ry + 0.5;
        ovl_rect.x0 += this->sc.output_xoffset;
        ovl_rect.y0 += this->sc.output_yoffset;
        ovl_rect.x1 += this->sc.output_xoffset;
        ovl_rect.y1 += this->sc.output_yoffset;
        if (ovl->video_window_width > 0 && ovl->video_window_height > 0) {
          /* clip osd video window to margins of displayed video window */
          if (vid_rect.x0 < clip_rect.x0)
            vid_rect.x0 = clip_rect.x0;
          if (vid_rect.y0 < clip_rect.y0)
            vid_rect.y0 = clip_rect.y0;
          if (vid_rect.x1 > clip_rect.x1)
            vid_rect.x1 = clip_rect.x1;
          if (vid_rect.y1 > clip_rect.y1)
            vid_rect.y1 = clip_rect.y1;
          vid_rect.x0 -= clip_rect.x0;
          vid_rect.y0 -= clip_rect.y0;
          vid_rect.x1 -= clip_rect.x0;
          vid_rect.y1 -= clip_rect.y0;
          /* scale osd video window coordinates to output window coordinates */
          vid_rect.x0 = (double)vid_rect.x0 * rx + 0.5;
          vid_rect.y0 = (double)vid_rect.y0 * ry + 0.5;
          vid_rect.x1 = (double)vid_rect.x1 * rx + 0.5;
          vid_rect.y1 = (double)vid_rect.y1 * ry + 0.5;
          vid_rect.x0 += this->sc.output_xoffset;
          vid_rect.y0 += this->sc.output_yoffset;
          vid_rect.x1 += this->sc.output_xoffset;
          vid_rect.y1 += this->sc.output_yoffset;
          /* take only visible osd video windows into account */
          if (vid_rect.x0 < vid_rect.x1 && vid_rect.y0 < vid_rect.y1)
            this->ovl_video_dest_rect = vid_rect;
        }
        this->ovl_changed = 1;

      } else { /* no zoom */

        if (ovl->extent_width > 0 && ovl->extent_height > 0) {
          /* compute extend size to output size scaling factor */
          rx = (double)this->sc.output_width / (double)ovl->extent_width;
          ry = (double)this->sc.output_height / (double)ovl->extent_height;
        } else {
          /* compute displayed size to output size scaling factor */
          rx = (double)this->sc.output_width / (double)this->sc.displayed_width;
          ry = (double)this->sc.output_height / (double)this->sc.displayed_height;
        }
        /* scale overlay window coordinates to output window coordinates */
        ovl_rect.x0 = (double)ovl_rect.x0 * rx + 0.5;
        ovl_rect.y0 = (double)ovl_rect.y0 * ry + 0.5;
        ovl_rect.x1 = (double)ovl_rect.x1 * rx + 0.5;
        ovl_rect.y1 = (double)ovl_rect.y1 * ry + 0.5;

        ovl_rect.x0 += this->sc.output_xoffset;
        ovl_rect.y0 += this->sc.output_yoffset;
        ovl_rect.x1 += this->sc.output_xoffset;
        ovl_rect.y1 += this->sc.output_yoffset;

        if (ovl->video_window_width > 0 && ovl->video_window_height > 0) {

          /* scale osd video window coordinates to output window coordinates */
          vid_rect.x0 = (double)vid_rect.x0 * rx + 0.5;
          vid_rect.y0 = (double)vid_rect.y0 * ry + 0.5;
          vid_rect.x1 = (double)vid_rect.x1 * rx + 0.5;
          vid_rect.y1 = (double)vid_rect.y1 * ry + 0.5;

          vid_rect.x0 += this->sc.output_xoffset;
          vid_rect.y0 += this->sc.output_yoffset;
          vid_rect.x1 += this->sc.output_xoffset;
          vid_rect.y1 += this->sc.output_yoffset;

          /* take only visible osd video windows into account */
          if (vid_rect.x0 < vid_rect.x1 && vid_rect.y0 < vid_rect.y1)
            this->ovl_video_dest_rect = vid_rect;
        }

        this->ovl_changed = 1;
      }
    }

    ovl_rects[i] = ovl_rect;
    ovl_src_rects[i] = ovl_src_rect;

    /* take only visible overlays into account */
    if (ovl_rect.x0 < ovl_rect.x1 && ovl_rect.y0 < ovl_rect.y1) {
      /* compute overall output window size */
      if (nvisible == 0) {
        first_visible = i;
        this->ovl_dest_rect = ovl_rect;
      } else {
        if (ovl_rect.x0 < this->ovl_dest_rect.x0)
          this->ovl_dest_rect.x0 = ovl_rect.x0;
        if (ovl_rect.y0 < this->ovl_dest_rect.y0)
          this->ovl_dest_rect.y0 = ovl_rect.y0;
        if (ovl_rect.x1 > this->ovl_dest_rect.x1)
          this->ovl_dest_rect.x1 = ovl_rect.x1;
        if (ovl_rect.y1 > this->ovl_dest_rect.y1)
          this->ovl_dest_rect.y1 = ovl_rect.y1;
      }
      ++nvisible;
    }
  }

  if (nvisible == 0) {
    this->ovl_layer_surface = VDP_INVALID_HANDLE;
    this->ovl_changed = 0;
    lprintf("overlays not visible\n");
    return;
  } else if (nvisible == 1) {
    /* we have only one visible overlay object so we can use it directly as overlay layer surface */
    this->ovl_src_rect = ovl_src_rects[first_visible];
    this->ovl_layer_surface = this->overlays[first_visible].render_surface.surface;
  } else {
    this->ovl_src_rect.x0 = 0;
    this->ovl_src_rect.y0 = 0;
    this->ovl_src_rect.x1 = this->ovl_dest_rect.x1 - this->ovl_dest_rect.x0;
    this->ovl_src_rect.y1 = this->ovl_dest_rect.y1 - this->ovl_dest_rect.y0;
    this->ovl_layer_surface = this->ovl_main_render_surface.surface;
  }

  lprintf("overlay output %d,%d:%d,%d -> %d,%d:%d,%d  video window %d,%d:%d,%d\n",
                  this->ovl_src_rect.x0, this->ovl_src_rect.y0, this->ovl_src_rect.x1, this->ovl_src_rect.y1,
                  this->ovl_dest_rect.x0, this->ovl_dest_rect.y0, this->ovl_dest_rect.x1, this->ovl_dest_rect.y1,
                  this->ovl_video_dest_rect.x0, this->ovl_video_dest_rect.y0, this->ovl_video_dest_rect.x1, this->ovl_video_dest_rect.y1);

  if (!this->ovl_changed)
    return;

  if (nvisible == 1) {
    this->ovl_changed = 0;
    return;
  }

  if (this->ovl_main_render_surface.surface != VDP_INVALID_HANDLE) {
    lprintf("overlay free main render surface %d\n", (int)this->ovl_main_render_surface.surface);
    vdpau_free_output_surface(this, &this->ovl_main_render_surface);
  }

  vdpau_get_output_surface(this, this->ovl_src_rect.x1, this->ovl_src_rect.y1, &this->ovl_main_render_surface);
  lprintf("overlay get main render surface %dx%d -> %d\n", this->ovl_src_rect.x1, this->ovl_src_rect.y1, (int)this->ovl_main_render_surface.surface);

  this->ovl_layer_surface = this->ovl_main_render_surface.surface;

  /* Clear main render surface if first overlay does not cover hole output window */
  if (this->ovl_dest_rect.x0 != ovl_rects[first_visible].x0 ||
                  this->ovl_dest_rect.x1 != ovl_rects[first_visible].x1 ||
                  this->ovl_dest_rect.y0 != ovl_rects[first_visible].y0 ||
                  this->ovl_dest_rect.y1 != ovl_rects[first_visible].y1) {
    int aw = (this->ovl_src_rect.x1 + 31) & ~31;

    lprintf("overlay clear main render output surface %dx%d\n", this->ovl_src_rect.x1, this->ovl_src_rect.y1);

    if (aw > this->ovl_pixmap_size) {
      this->ovl_pixmap_size = aw;
      xine_free_aligned (this->ovl_pixmap);
      this->ovl_pixmap = xine_mallocz_aligned (this->ovl_pixmap_size * sizeof (uint32_t));
      xprintf (this->xine, XINE_VERBOSITY_DEBUG, LOG_MODULE ": overlay argb buffer enlarged to %dx%d = %d.\n",
        aw, 1, aw);
    } else {
      memset (this->ovl_pixmap, 0, aw * sizeof (uint32_t));
    }

    uint32_t pitch = 0;
    const void * const prgba = this->ovl_pixmap;
    VdpStatus st = this->a.vdp.output_surface.put_bits (this->ovl_layer_surface, &prgba, &pitch, &this->ovl_src_rect);
    VDPAU_IF_ERROR ("vdpau_process_overlays: vdp_output_surface_put_bits (clear) failed");
  }

  /* Render all visible overlays into main render surface */
  for (i = 0; i < novls; ++i) {
    vdpau_overlay_t *ovl = &this->overlays[i];

    if (ovl_rects[i].x0 < ovl_rects[i].x1 && ovl_rects[i].y0 < ovl_rects[i].y1) {
      /* compensate overall output offset of main render surface */
      VdpRect render_rect;
      render_rect.x0 = ovl_rects[i].x0 - this->ovl_dest_rect.x0;
      render_rect.x1 = ovl_rects[i].x1 - this->ovl_dest_rect.x0;
      render_rect.y0 = ovl_rects[i].y0 - this->ovl_dest_rect.y0;
      render_rect.y1 = ovl_rects[i].y1 - this->ovl_dest_rect.y0;

      lprintf("overlay[%d] render %d,%d:%d,%d -> %d,%d:%d,%d\n",
                      i, ovl_rects[i].x0, ovl_rects[i].y0, ovl_rects[i].x1, ovl_rects[i].y1, render_rect.x0, render_rect.y0, render_rect.x1, render_rect.y1);

      const VdpOutputSurfaceRenderBlendState *bs = (i > first_visible) ? &blend: NULL;
      VdpStatus st = this->a.vdp.output_surface.render_output_surface (this->ovl_layer_surface, &render_rect,
        ovl->render_surface.surface, &ovl_src_rects[i], NULL, bs, 0 );
      VDPAU_IF_ERROR ("vdpau_process_overlays: vdp_output_surface_render_output_surface failed");
    }
  }
  this->ovl_changed = 0;
}


static void vdpau_frame_proc_slice (vo_frame_t *vo_img, uint8_t **src)
{
  /*vdpau_frame_t  *frame = (vdpau_frame_t *) vo_img;*/

  (void)src;
  vo_img->proc_called = 1;
}


static void vdpau_frame_field (vo_frame_t *vo_img, int which_field)
{
  (void)vo_img;
  (void)which_field;
}


static void vdpau_frame_dispose (vo_frame_t *vo_img)
{
  vdpau_frame_t  *frame = (vdpau_frame_t *) vo_img ;
  vdpau_driver_t *this  = (vdpau_driver_t *) frame->vo_frame.driver;

  xine_freep_aligned (&frame->vo_frame.base[0]);
  frame->vo_frame.base[1] = NULL;
  frame->vo_frame.base[2] = NULL;
  vdpau_video_surf_delete (this, &frame->surf);
  pthread_mutex_destroy (&frame->vo_frame.mutex);
  free (frame);
}


static vo_frame_t *vdpau_alloc_frame (vo_driver_t *this_gen)
{
  vdpau_frame_t  *frame;
  vdpau_driver_t *this = (vdpau_driver_t *) this_gen;

  lprintf("vdpau_alloc_frame\n" );

  frame = (vdpau_frame_t *) calloc(1, sizeof(vdpau_frame_t));

  if (!frame)
    return NULL;

  frame->vo_frame.base[0] = frame->vo_frame.base[1] = frame->vo_frame.base[2] = NULL;
  frame->width = frame->height = frame->format = frame->flags = 0;

  frame->vo_frame.accel_data = &frame->vdpau_accel_data;

  pthread_mutex_init (&frame->vo_frame.mutex, NULL);

  /*
   * supply required functions/fields
   */
  frame->vo_frame.proc_duplicate_frame_data = NULL;
  frame->vo_frame.proc_slice = vdpau_frame_proc_slice;
  frame->vo_frame.proc_frame = NULL;
  frame->vo_frame.field      = vdpau_frame_field;
  frame->vo_frame.dispose    = vdpau_frame_dispose;
  frame->vo_frame.driver     = this_gen;

  frame->surf.surface = VDP_INVALID_HANDLE;
  frame->surface_cleared_nr = 0;

  frame->vdpau_accel_data.vo_frame = &frame->vo_frame;
  frame->vdpau_accel_data.vdp_device = this->vdp_device;
  frame->vdpau_accel_data.surface = VDP_INVALID_HANDLE;
  frame->vdpau_accel_data.chroma = VDP_CHROMA_TYPE_420;
#ifdef LOCKDISPLAY
  frame->vdpau_accel_data.lock = vdpau_lockdisplay;
  frame->vdpau_accel_data.unlock = vdpau_unlockdisplay;
#else
  frame->vdpau_accel_data.lock = NULL;
  frame->vdpau_accel_data.unlock = NULL;
#endif
  frame->vdpau_accel_data.vdp_decoder_create = this->a.vdp.decoder.create;
  frame->vdpau_accel_data.vdp_decoder_destroy = this->a.vdp.decoder.destroy;
  frame->vdpau_accel_data.vdp_decoder_render = this->a.vdp.decoder.render;
  frame->vdpau_accel_data.vdp_get_error_string = this->a.vdp.get.error_string;
  frame->vdpau_accel_data.vdp_runtime_nr = this->vdp_runtime_nr;
  frame->vdpau_accel_data.current_vdp_runtime_nr = &this->vdp_runtime_nr;

  return &frame->vo_frame;
}


static void vdpau_provide_standard_frame_data (vo_frame_t *frame, xine_current_frame_data_t *data)
{
  VdpStatus st;
  VdpYCbCrFormat format;
  uint32_t pitches[3];
  void *base[3];
  vdpau_driver_t *this = (vdpau_driver_t *)frame->driver;

  if (frame->format != XINE_IMGFMT_VDPAU) {
    xprintf (this->xine, XINE_VERBOSITY_LOG,
      LOG_MODULE ": vdpau_provide_standard_frame_data: unexpected frame format 0x%08x!\n", frame->format);
    return;
  }

  vdpau_accel_t *accel = (vdpau_accel_t *) frame->accel_data;

  if (accel->vdp_runtime_nr != *(accel->current_vdp_runtime_nr))
    return;

  frame = accel->vo_frame;

  if (accel->chroma == VDP_CHROMA_TYPE_420) {
    data->format = XINE_IMGFMT_YV12;
    data->img_size = frame->width * frame->height
                   + ((frame->width + 1) / 2) * ((frame->height + 1) / 2)
                   + ((frame->width + 1) / 2) * ((frame->height + 1) / 2);
    if (data->img) {
      pitches[0] = frame->width;
      pitches[2] = frame->width / 2;
      pitches[1] = frame->width / 2;
      base[0] = data->img;
      base[2] = data->img + frame->width * frame->height;
      base[1] = data->img + frame->width * frame->height + frame->width * frame->height / 4;
      format = VDP_YCBCR_FORMAT_YV12;
    }
  } else {
    data->format = XINE_IMGFMT_YUY2;
    data->img_size = frame->width * frame->height
                   + ((frame->width + 1) / 2) * frame->height
                   + ((frame->width + 1) / 2) * frame->height;
    if (data->img) {
      pitches[0] = frame->width * 2;
      base[0] = data->img;
      format = VDP_YCBCR_FORMAT_YUYV;
    }
  }

  if (data->img) {
    st = this->a.vdp.video_surface.getbits_ycbcr (accel->surface, format, base, pitches);
    VDPAU_IF_ERROR ("failed to get surface bits !!");
  }
}


static void vdpau_duplicate_frame_data (vo_frame_t *frame_gen, vo_frame_t *original)
{
  vdpau_driver_t *this = (vdpau_driver_t *)frame_gen->driver;
  vdpau_frame_t *frame = (vdpau_frame_t *)frame_gen;
  vdpau_frame_t *orig = (vdpau_frame_t *)original;
  VdpStatus st;
  VdpYCbCrFormat format;

  if (orig->vo_frame.format != XINE_IMGFMT_VDPAU) {
    xprintf (this->xine, XINE_VERBOSITY_LOG,
      LOG_MODULE ": vdpau_duplicate_frame_data: unexpected frame format 0x%08x!\n", orig->vo_frame.format);
    return;
  }

  if(orig->vdpau_accel_data.vdp_runtime_nr != frame->vdpau_accel_data.vdp_runtime_nr) {
    xprintf (this->xine, XINE_VERBOSITY_LOG, LOG_MODULE ": vdpau_duplicate_frame_data: called with invalid frame\n");
    return;
  }

  if (frame->vo_frame.format != XINE_IMGFMT_VDPAU) {
    /* should not happen */
    xine_freep_aligned (&frame->vo_frame.base[0]);
    frame->vo_frame.base[1] = NULL;
    frame->vo_frame.base[2] = NULL;
    frame->vo_frame.format = XINE_IMGFMT_VDPAU;
    frame->surf.surface = VDP_INVALID_HANDLE;
  }

  if ((frame->surf.width ^ orig->surf.width) | (frame->surf.height ^ orig->surf.height)
    | (frame->surf.chroma ^ orig->surf.chroma) | !(frame->surf.surface ^ VDP_INVALID_HANDLE)) {
    vdpau_video_surf_delete (this, &frame->surf);
    frame->vdpau_accel_data.surface = VDP_INVALID_HANDLE;
    frame->surf.width = orig->surf.width;
    frame->surf.height = orig->surf.height;
    frame->surf.chroma = orig->surf.chroma;
  }

  if (frame->surf.surface == VDP_INVALID_HANDLE)
    vdpau_video_surf_new (this, &frame->surf);
  frame->vdpau_accel_data.surface = frame->surf.surface;

  if (!(orig->flags & VO_CHROMA_422)) {
    int w = (orig->surf.a_width + 15) & ~15;
    int ysize = w * orig->surf.a_height;
    int uvsize = (w >> 1) * ((orig->surf.a_height + 1) >> 1);
    frame->vo_frame.pitches[0] = w;
    frame->vo_frame.pitches[1] = w >> 1;
    frame->vo_frame.pitches[2] = w >> 1;
    frame->vo_frame.base[0] = xine_malloc_aligned (ysize + 2 * uvsize);
    frame->vo_frame.base[1] = frame->vo_frame.base[0] + ysize;
    frame->vo_frame.base[2] = frame->vo_frame.base[1] + uvsize;
    format = VDP_YCBCR_FORMAT_YV12;
  } else {
    frame->vo_frame.pitches[0] = ((orig->surf.a_width + 15) & ~15) << 1;
    frame->vo_frame.base[0] = xine_malloc_aligned (frame->vo_frame.pitches[0] * orig->surf.a_height);
    format = VDP_YCBCR_FORMAT_YUYV;
  }

  if (frame->vo_frame.base[0]) {
    void * const ptemp[] = {frame->vo_frame.base[0], frame->vo_frame.base[1], frame->vo_frame.base[2]};
    const uint32_t pitches[] = {frame->vo_frame.pitches[0], frame->vo_frame.pitches[1], frame->vo_frame.pitches[2]};
    st = this->a.vdp.video_surface.getbits_ycbcr (orig->vdpau_accel_data.surface, format, ptemp, pitches);
    VDPAU_IF_ERROR ("failed to get surface bits !!");
    DO_LOCKDISPLAY (this);
    st = this->a.vdp.video_surface.putbits_ycbcr (frame->vdpau_accel_data.surface, format, (const void * const *)ptemp, pitches);
    DO_UNLOCKDISPLAY (this);
    VDPAU_IF_ERROR ("failed to put surface bits !!");
  }

  xine_freep_aligned (&frame->vo_frame.base[0]);
  frame->vo_frame.base[1] = NULL;
  frame->vo_frame.base[2] = NULL;
}


static void vdpau_update_frame_format (vo_driver_t *this_gen, vo_frame_t *frame_gen,
      uint32_t width, uint32_t height, double ratio, int format, int flags)
{
  vdpau_driver_t *this = (vdpau_driver_t *) this_gen;
  vdpau_frame_t *frame = (vdpau_frame_t *) frame_gen;
  int clear = 0;

  if ( flags & VO_NEW_SEQUENCE_FLAG )
    ++this->surface_cleared_nr;

  /* VDPAU constraints?? */
  width = (width + 3) & ~3;
  height = (height + 3) & ~3;

  /* fast check the most common case */
  if ((frame->width ^ width) | (frame->height ^ height)
    | (frame->format ^ format)
    | ((frame->flags ^ flags) & VO_CHROMA_422)
    | (frame->vdpau_accel_data.vdp_runtime_nr ^ this->vdp_runtime_nr)) {

    /* (re-) allocate render space */
    xine_freep_aligned (&frame->vo_frame.base[0]);
    frame->vo_frame.base[1] = NULL;
    frame->vo_frame.base[2] = NULL;

    if (format == XINE_IMGFMT_YV12) {
      int w = (width + 15) & ~15;
      int ysize = w * height;
      int uvsize = (w >> 1) * ((height + 1) >> 1);
      frame->surf.a_width = w;
      frame->surf.a_height = height;
      frame->vo_frame.pitches[0] = w;
      frame->vo_frame.pitches[1] = w >> 1;
      frame->vo_frame.pitches[2] = w >> 1;
      frame->vo_frame.base[0] = xine_malloc_aligned (ysize + 2 * uvsize);
      if (!frame->vo_frame.base[0]) {
        frame->width = 0;
        frame->vo_frame.width = 0; /* tell vo_get_frame () to retry later */
        return;
      }
      memset (frame->vo_frame.base[0], 0, ysize);
      frame->vo_frame.base[1] = frame->vo_frame.base[0] + ysize;
      memset (frame->vo_frame.base[1], 128, 2 * uvsize);
      frame->vo_frame.base[2] = frame->vo_frame.base[1] + uvsize;
    } else if (format == XINE_IMGFMT_YUY2){
      frame->vo_frame.pitches[0] = ((width + 15) & ~15) << 1;
      frame->surf.a_width = frame->vo_frame.pitches[0] >> 1;
      frame->surf.a_height = height;
      frame->vo_frame.base[0] = xine_malloc_aligned (frame->vo_frame.pitches[0] * height);
      if (frame->vo_frame.base[0]) {
        const union {uint8_t bytes[4]; uint32_t word;} black = {{0, 128, 0, 128}};
        uint32_t *q = (uint32_t *)frame->vo_frame.base[0];
        int i;
        for (i = frame->vo_frame.pitches[0] * height / 4; i > 0; i--)
          *q++ = black.word;
      } else {
        frame->width = 0;
        frame->vo_frame.width = 0; /* tell vo_get_frame () to retry later */
        return;
      }
    }

    if (frame->vdpau_accel_data.vdp_runtime_nr != this->vdp_runtime_nr) {
      frame->surf.surface = frame->vdpau_accel_data.surface = VDP_INVALID_HANDLE;
      frame->vdpau_accel_data.vdp_runtime_nr = this->vdp_runtime_nr;
      frame->vdpau_accel_data.vdp_device = this->vdp_device;
      frame->vo_frame.proc_duplicate_frame_data = NULL;
      frame->vo_frame.proc_provide_standard_frame_data = NULL;
    }

    if (frame->vdpau_accel_data.surface != VDP_INVALID_HANDLE) {
      if ((frame->width ^ width) | (frame->height ^ height)
        | ((frame->flags ^ flags) & VO_CHROMA_422)
        | (format ^ XINE_IMGFMT_VDPAU)) {
        lprintf(LOG_MODULE ": update_frame - destroy surface\n");
        vdpau_video_surf_delete (this, &frame->surf);
        frame->vdpau_accel_data.surface = VDP_INVALID_HANDLE;
        --this->allocated_surfaces;
        frame->vo_frame.proc_duplicate_frame_data = NULL;
        frame->vo_frame.proc_provide_standard_frame_data = NULL;
      }
    }

    if ((format == XINE_IMGFMT_VDPAU) && (frame->vdpau_accel_data.surface == VDP_INVALID_HANDLE)) {
      VdpStatus st;

      frame->surf.chroma = (flags & VO_CHROMA_422) ? VDP_CHROMA_TYPE_422 : VDP_CHROMA_TYPE_420;
      frame->surf.width = width;
      frame->surf.height = height;
      st = vdpau_video_surf_new (this, &frame->surf);
      if (st == VDP_STATUS_OK) {
        clear = 1;
        frame->vdpau_accel_data.surface = frame->surf.surface;
        frame->vdpau_accel_data.chroma = frame->surf.chroma;
        ++this->allocated_surfaces;
        frame->vo_frame.proc_duplicate_frame_data = vdpau_duplicate_frame_data;
        frame->vo_frame.proc_provide_standard_frame_data = vdpau_provide_standard_frame_data;
      }
    }

    frame->width = width;
    frame->height = height;
    frame->format = format;
    frame->flags = flags;

    vdpau_frame_field (&frame->vo_frame, flags);
  }

  if ( (format == XINE_IMGFMT_VDPAU) && (clear || (frame->surface_cleared_nr != this->surface_cleared_nr)) ) {
    static const uint32_t pitches[] = { 0, 0, 0 };
    lprintf( "clear surface: %d\n", frame->vdpau_accel_data.surface );
    if ( frame->vdpau_accel_data.chroma == VDP_CHROMA_TYPE_422 ) {
      uint8_t *cb = malloc (frame->surf.a_width * 2);
      if (cb) {
        VdpStatus st;
        const void * const data[] = { cb };
        const union {uint8_t bytes[4]; uint32_t word;} black = {{0, 128, 0, 128}};
        uint32_t *q = (uint32_t *)cb;
        int i;
        for (i = frame->surf.a_width * 2 / 4; i > 0; i--)
          *q++ = black.word;
        DO_LOCKDISPLAY (this);
        st = this->a.vdp.video_surface.putbits_ycbcr (frame->vdpau_accel_data.surface, VDP_YCBCR_FORMAT_YUYV, data, pitches);
        DO_UNLOCKDISPLAY (this);
        free (cb);
        VDPAU_IF_ERROR ("failed to clear surface");
      }
    }
    else {
      uint8_t *cb = malloc ((frame->surf.a_width * 3 + 1) / 2);
      if (cb) {
        VdpStatus st;
        const void * const data[] = { cb, cb + frame->surf.a_width, cb + frame->surf.a_width };
        memset (cb, 0, frame->surf.a_width);
        memset (cb + frame->surf.a_width, 128, (frame->surf.a_width + 1) >> 1);
        DO_LOCKDISPLAY (this);
        st = this->a.vdp.video_surface.putbits_ycbcr (frame->vdpau_accel_data.surface, VDP_YCBCR_FORMAT_YV12, data, pitches);
        DO_UNLOCKDISPLAY (this);
        free (cb);
        VDPAU_IF_ERROR ("failed to clear surface");
      }
    }
    if ( frame->surface_cleared_nr != this->surface_cleared_nr )
      frame->surface_cleared_nr = this->surface_cleared_nr;
  }

  frame->ratio = ratio;
  frame->vo_frame.future_frame = NULL;
}


static int vdpau_redraw_needed (vo_driver_t *this_gen)
{
  vdpau_driver_t  *this = (vdpau_driver_t *) this_gen;

  _x_vo_scale_compute_ideal_size( &this->sc );
  if ( _x_vo_scale_redraw_needed( &this->sc ) ) {
    _x_vo_scale_compute_output_size( &this->sc );
    return 1;
  }
  return this->prop_changed ? 1 : 0;
}


static int vdpau_release_back_frames (vdpau_driver_t *this) {
  int i, n = 0;

  for (i = 0; i < NUM_FRAMES_BACK; ++i) {
    if (this->back_frame[i]) {
      this->back_frame[i]->vo_frame.free (&this->back_frame[i]->vo_frame);
      this->back_frame[i] = NULL;
      n++;
    }
  }
  return n;
}


static void vdpau_backup_frame (vdpau_driver_t *this, vdpau_frame_t *frame)
{
#if NUM_FRAMES_BACK > 1
  int i;
#endif

  if ( this->back_frame[NUM_FRAMES_BACK-1]) {
    this->back_frame[NUM_FRAMES_BACK-1]->vo_frame.free (&this->back_frame[NUM_FRAMES_BACK-1]->vo_frame);
  }
#if NUM_FRAMES_BACK > 1
  for ( i=NUM_FRAMES_BACK-1; i>0; i-- )
    this->back_frame[i] = this->back_frame[i-1];
#endif
  this->back_frame[0] = frame;
}

static void vdpau_update_deinterlace_method_sd( void *this_gen, xine_cfg_entry_t *entry )
{
  vdpau_driver_t  *this  = (vdpau_driver_t *) this_gen;

  this->deinterlace_method_sd = entry->num_value;
  lprintf(LOG_MODULE ": deinterlace_method_sd=%d\n", this->deinterlace_method_sd );
  this->prop_changed |= _VOVDP_S_DEINT;
}


static void vdpau_update_deinterlace_method_hd( void *this_gen, xine_cfg_entry_t *entry )
{
  vdpau_driver_t  *this  = (vdpau_driver_t *) this_gen;

  this->deinterlace_method_hd = entry->num_value;
  lprintf(LOG_MODULE ": deinterlace_method_hd=%d\n", this->deinterlace_method_hd );
  this->prop_changed |= _VOVDP_S_DEINT;
}


static void vdpau_update_scaling_level( void *this_gen, xine_cfg_entry_t *entry )
{
  vdpau_driver_t  *this  = (vdpau_driver_t *) this_gen;

  this->scaling_level_current = entry->num_value;
  lprintf(LOG_MODULE ": scaling_quality=%d\n", this->scaling_level_current );
  this->prop_changed |= _VOVDP_S_SCALE_LEVEL;
}


static void vdpau_update_enable_inverse_telecine( void *this_gen, xine_cfg_entry_t *entry )
{
  vdpau_driver_t  *this  = (vdpau_driver_t *) this_gen;

  this->enable_inverse_telecine = entry->num_value;
  lprintf(LOG_MODULE ": enable inverse_telecine=%d\n", this->enable_inverse_telecine );
  this->prop_changed |= _VOVDP_S_INV_TELE;
}


static void vdpau_honor_progressive_flag( void *this_gen, xine_cfg_entry_t *entry )
{
  vdpau_driver_t  *this  = (vdpau_driver_t *) this_gen;

  this->honor_progressive = entry->num_value;
  lprintf(LOG_MODULE ": honor_progressive=%d\n", this->honor_progressive );
}

static void vdpau_update_prop (vdpau_driver_t *this) {
  VdpVideoMixerFeature mixer_features[20];
  VdpBool mixer_feature_enables[20];
  VdpVideoMixerAttribute mixer_attributes[8];
  VdpColor bg;
  float attribute_values[8];
  void *attribute_ptrs[8];
  uint32_t n_features = 0, n_values = 0;
  VdpStatus st;

  if (!(this->prop_changed &
    (_VOVDP_S_BGCOLOR | _VOVDP_S_NOISE_RED | _VOVDP_S_SHARPNESS |
    _VOVDP_S_SCALE_LEVEL | _VOVDP_S_DEINT | _VOVDP_S_NO_CHROMA | _VOVDP_S_INV_TELE)))
    return;

  if ((this->prop_changed & _VOVDP_S_BGCOLOR) && _vdpau_feature_have (this, _VOVDP_A_background_color)) {
    mixer_attributes[n_values] = VDP_VIDEO_MIXER_ATTRIBUTE_BACKGROUND_COLOR;
    bg.red = (this->background >> 16) / 255.f;
    bg.green = ((this->background >> 8) & 0xff) / 255.f;
    bg.blue = (this->background & 0xff) / 255.f;
    bg.alpha = 1;
    attribute_ptrs[n_values] = &bg;
    n_values++;
  }

  if ((this->prop_changed & _VOVDP_S_NOISE_RED) && _vdpau_feature_have (this, _VOVDP_M_noise_reduction)) {
    mixer_features[n_features] = VDP_VIDEO_MIXER_FEATURE_NOISE_REDUCTION;
    mixer_feature_enables[n_features] =
      ((this->noise == 0) || ((this->sd_only_properties & 1) && (this->video_mixer.width >= 800))) ?
      VDP_FALSE : VDP_TRUE;
    n_features++;
    mixer_attributes[n_values] = VDP_VIDEO_MIXER_ATTRIBUTE_NOISE_REDUCTION_LEVEL;
    attribute_values[n_values] = this->noise / 100.0;
    attribute_ptrs[n_values] = attribute_values + n_features;
    n_values++;
  }

  if ((this->prop_changed & _VOVDP_S_SHARPNESS) && _vdpau_feature_have (this, _VOVDP_M_sharpness)) {
    mixer_features[n_features] = VDP_VIDEO_MIXER_FEATURE_SHARPNESS;
    mixer_feature_enables[n_features] =
      ((this->sharpness == 0) || ((this->sd_only_properties & 2) && (this->video_mixer.width >= 800))) ?
      VDP_FALSE : VDP_TRUE;
    n_features++;
    mixer_attributes[n_values] = VDP_VIDEO_MIXER_ATTRIBUTE_SHARPNESS_LEVEL;
    attribute_values[n_values] = this->sharpness / 100.0;
    attribute_ptrs[n_values] = attribute_values + n_values;
    n_values++;
  }

#ifdef VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1
  if (this->prop_changed & _VOVDP_S_SCALE_LEVEL) {
    int l;
    for (l = 0; l < this->scaling_level_max; l++) {
      mixer_features[n_features + l] = VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1 + l;
      mixer_feature_enables[n_features + l] = VDP_FALSE;
    }
    if (this->scaling_level_current > 0)
      mixer_feature_enables[n_features + this->scaling_level_current - 1] = VDP_TRUE;
    n_features += this->scaling_level_max;
  }
#endif

  if (this->prop_changed & _VOVDP_S_DEINT) {
    static const uint8_t e[DEINT_LAST][2] = {
      [DEINT_NONE]                  = {0, 0},
      [DEINT_BOB]                   = {0, 0},
      [DEINT_HALF_TEMPORAL]         = {1, 0},
      [DEINT_HALF_TEMPORAL_SPATIAL] = {1, 1},
      [DEINT_TEMPORAL]              = {1, 0},
      [DEINT_TEMPORAL_SPATIAL]      = {1, 1}
    };
    int deinterlace_method;
    if (this->deinterlace) {
      deinterlace_method = (this->video_mixer.width < 800) ? this->deinterlace_method_sd : this->deinterlace_method_hd;
      deinterlace_method = this->deinterlacers_method[deinterlace_method];
      if ((deinterlace_method < 0) || (deinterlace_method >= DEINT_LAST))
        deinterlace_method = DEINT_BOB;
    } else {
      deinterlace_method = DEINT_NONE;
    }
    if (_vdpau_feature_have (this, _VOVDP_I_temporal)) {
      mixer_features[n_features] = VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL;
      mixer_feature_enables[n_features] = e[deinterlace_method][0] ? VDP_TRUE : VDP_FALSE;
      n_features++;
    }
    if (_vdpau_feature_have (this, _VOVDP_I_temporal_spatial)) {
      mixer_features[n_features] = VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL_SPATIAL;
      mixer_feature_enables[n_features] = e[deinterlace_method][1] ? VDP_TRUE : VDP_FALSE;
      n_features++;
    }
  }

  if ((this->prop_changed & _VOVDP_S_NO_CHROMA) && _vdpau_feature_have (this, _VOVDP_I_no_chroma)) {
    mixer_attributes[n_values] = VDP_VIDEO_MIXER_ATTRIBUTE_SKIP_CHROMA_DEINTERLACE;
    attribute_ptrs[n_values] = &this->skip_chroma;
    n_values++;
  }

  if ((this->prop_changed & (_VOVDP_S_DEINT | _VOVDP_S_INV_TELE)) && _vdpau_feature_have (this, _VOVDP_I_inverse_telecine)) {
    mixer_features[n_features] = VDP_VIDEO_MIXER_FEATURE_INVERSE_TELECINE;
    mixer_feature_enables[n_features] = (this->deinterlace && this->enable_inverse_telecine) ? VDP_TRUE : VDP_FALSE;
    n_features++;
  }

  if (n_features > 0) {
    this->a.vdp.video_mixer.set_feature_enables (this->video_mixer.handle,
      n_features, mixer_features, mixer_feature_enables);
  }
  if (n_values > 0) {
    st = this->a.vdp.video_mixer.set_attribute_values (this->video_mixer.handle,
      n_values, mixer_attributes, (const void * const)attribute_ptrs);
    VDPAU_IF_ERROR ("can't set mixer props !!");
  }
  this->prop_changed &=
    ~(_VOVDP_S_BGCOLOR | _VOVDP_S_NOISE_RED | _VOVDP_S_SHARPNESS |
    _VOVDP_S_SCALE_LEVEL | _VOVDP_S_DEINT | _VOVDP_S_NO_CHROMA | _VOVDP_S_INV_TELE);
}

static void vdpau_update_sd_only_properties( void *this_gen, xine_cfg_entry_t *entry )
{
  vdpau_driver_t  *this  = (vdpau_driver_t *) this_gen;

  this->sd_only_properties = entry->num_value;
  lprintf( LOG_MODULE ": enable sd only noise=%d, sd only sharpness %d\n", ((this->sd_only_properties & 1) != 0), (this->sd_only_properties >= 2) );
  this->prop_changed |= _VOVDP_S_NOISE_RED | _VOVDP_S_SHARPNESS;
}


static void vdpau_update_csc_matrix (vdpau_driver_t *this, vdpau_frame_t *frame) {
  int color_matrix;

  color_matrix = cm_from_frame (&frame->vo_frame);

  if ((this->prop_changed & (_VOVDP_S_HUE | _VOVDP_S_SATURATION | _VOVDP_S_CONTRAST | _VOVDP_S_BRIGHTNESS | _VOVDP_S_CSC))
    || (this->color_matrix != color_matrix)) {
    VdpStatus st;
    float matrix[12];
    float hue = (float)this->hue * M_PI / 128.0;
    float saturation = (float)this->saturation / 128.0;
    float contrast = (float)this->contrast / 128.0;
    float brightness = this->brightness;

    cm_fill_matrix(matrix, color_matrix, hue, saturation, contrast, brightness);

    this->color_matrix = color_matrix;
    this->prop_changed &= ~(_VOVDP_S_HUE | _VOVDP_S_SATURATION | _VOVDP_S_CONTRAST | _VOVDP_S_BRIGHTNESS | _VOVDP_S_CSC);

    VdpCSCMatrix _matrix = {{matrix[0], matrix[1], matrix[2], matrix[3]},
                            {matrix[4], matrix[5], matrix[6], matrix[7]},
                            {matrix[8], matrix[9], matrix[10], matrix[11]}};
    const VdpVideoMixerAttribute attributes [] = {VDP_VIDEO_MIXER_ATTRIBUTE_CSC_MATRIX};
    const void * const attribute_values[] = {&_matrix};
    st = this->a.vdp.video_mixer.set_attribute_values (this->video_mixer.handle, 1, attributes, attribute_values);
    VDPAU_IF_ERROR ("can't set csc matrix !!");

    xprintf (this->xine, XINE_VERBOSITY_LOG,LOG_MODULE ": b %d c %d s %d h %d [%s]\n",
      this->brightness, this->contrast, this->saturation, this->hue, cm_names[color_matrix]);
  }
}

static void vdpau_set_skip_chroma( void *this_gen, xine_cfg_entry_t *entry )
{
  vdpau_driver_t  *this  = (vdpau_driver_t *) this_gen;
  this->skip_chroma = entry->num_value;
  this->prop_changed |= _VOVDP_S_NO_CHROMA;
}


static void vdpau_set_background( void *this_gen, xine_cfg_entry_t *entry )
{
  vdpau_driver_t  *this  = (vdpau_driver_t *) this_gen;
  entry->num_value &= 0xffffff;
  this->background = entry->num_value;
  this->prop_changed |= _VOVDP_S_BGCOLOR;
}


static void vdpau_shift_queue (vdpau_driver_t *this)
{
  if ( this->init_queue < this->queue_length )
    ++this->init_queue;
  ++this->current_output_surface;
  if ( this->current_output_surface >= this->queue_length )
    this->current_output_surface = 0;
}


static void vdpau_check_output_size (vdpau_driver_t *this)
{
  vdpau_output_surface_t *s = &this->output_surfaces[this->current_output_surface];

  if  ((this->sc.gui_width > (int)s->width) || (this->sc.gui_height > (int)s->height)) {
    /* recreate output surface to match window size */
    lprintf( LOG_MODULE ": output_surface size update\n" );
    vdpau_output_surf_delete (this, s);
    s->width = this->sc.gui_width;
    s->height = this->sc.gui_height;
    vdpau_output_surf_new (this, s);
  }
}


static void vdpau_grab_current_output_surface (vdpau_driver_t *this, int64_t vpts)
{
  pthread_mutex_lock(&this->grab_lock);

  vdpau_grab_video_frame_t *frame = this->pending_grab_request;
  if (frame) {
    vdpau_output_surface_t *s = &this->output_surfaces[this->current_output_surface];
    int width = this->sc.gui_width;
    int height = this->sc.gui_height;
    VdpStatus st;

    this->pending_grab_request = NULL;
    frame->grab_frame.vpts = -1;

    /* take cropping parameters into account */
    width -= frame->grab_frame.crop_left - frame->grab_frame.crop_right;
    height -= frame->grab_frame.crop_top - frame->grab_frame.crop_bottom;
    if (width < 1)
      width = 1;
    if (height < 1)
      height = 1;

    /* if caller does not specify frame size we return the actual size of grabbed frame */
    if (frame->grab_frame.width <= 0)
      frame->grab_frame.width = width;
    if (frame->grab_frame.height <= 0)
      frame->grab_frame.height = height;

    if (frame->grab_frame.width != frame->width || frame->grab_frame.height != frame->height) {
      free(frame->rgba);
      free(frame->grab_frame.img);
      frame->rgba = NULL;
      frame->grab_frame.img = NULL;

      frame->width = frame->grab_frame.width;
      frame->height = frame->grab_frame.height;
    }

    if (frame->rgba == NULL) {
      frame->rgba = (uint32_t *) calloc(frame->width * frame->height, sizeof(uint32_t));
      if (frame->rgba == NULL) {
        pthread_cond_broadcast(&this->grab_cond);
        pthread_mutex_unlock(&this->grab_lock);
        return;
      }
    }

    if (frame->grab_frame.img == NULL) {
      frame->grab_frame.img = (uint8_t *) calloc(frame->width * frame->height, 3);
      if (frame->grab_frame.img == NULL) {
        pthread_cond_broadcast(&this->grab_cond);
        pthread_mutex_unlock(&this->grab_lock);
        return;
      }
    }

    {
      uint32_t pitches = frame->width * sizeof(uint32_t);
      void * const prgba = frame->rgba;
      VdpRect src_rect = {
        frame->grab_frame.crop_left,
        frame->grab_frame.crop_top,
        width + frame->grab_frame.crop_left,
        height + frame->grab_frame.crop_top
      };

      if (frame->width != width || frame->height != height) {
        VdpRect dst_rect = { 0, 0, frame->width, frame->height };

        st = vdpau_get_output_surface (this, frame->width, frame->height, &frame->render_surface);
        if (st == VDP_STATUS_OK) {
          lprintf("grab got render output surface %dx%d -> %d\n", frame->width, frame->height, (int)frame->render_surface.surface);

          st = this->a.vdp.output_surface.render_output_surface (frame->render_surface.surface,
            &dst_rect, s->surface, &src_rect, NULL, NULL, VDP_OUTPUT_SURFACE_RENDER_ROTATE_0);
          if (st == VDP_STATUS_OK) {
            st = this->a.vdp.output_surface.get_bits (frame->render_surface.surface, &dst_rect, &prgba, &pitches);
            VDPAU_IF_ERROR ("Can't get output surface bits for raw frame grabbing");
          } else
            VDPAU_ERROR ("Can't render output surface for raw frame grabbing");

          vdpau_free_output_surface (this, &frame->render_surface);
        }
      } else {
        st = this->a.vdp.output_surface.get_bits (s->surface, &src_rect, &prgba, &pitches);
        VDPAU_IF_ERROR ("Can't get output surface bits for raw frame grabbing");
      }
    }
    if (st == VDP_STATUS_OK)
      frame->grab_frame.vpts = vpts;

    pthread_cond_broadcast(&this->grab_cond);
  }

  pthread_mutex_unlock(&this->grab_lock);
}


static VdpStatus vdpau_new_video_mixer (vdpau_driver_t *this) {

  VdpVideoMixerFeature features[15];
  int features_count = 0;
  if (_vdpau_feature_have (this, _VOVDP_M_noise_reduction))
    features[features_count++] = VDP_VIDEO_MIXER_FEATURE_NOISE_REDUCTION;
  if (_vdpau_feature_have (this, _VOVDP_M_sharpness))
    features[features_count++] = VDP_VIDEO_MIXER_FEATURE_SHARPNESS;
  if (_vdpau_feature_have (this, _VOVDP_I_temporal))
    features[features_count++] = VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL;
  if (_vdpau_feature_have (this, _VOVDP_I_temporal_spatial))
    features[features_count++] = VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL_SPATIAL;
  if (_vdpau_feature_have (this, _VOVDP_I_inverse_telecine))
    features[features_count++] = VDP_VIDEO_MIXER_FEATURE_INVERSE_TELECINE;
#ifdef VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1
  {
    int n = this->scaling_level_max;
    int f = VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1;
    while (n--)
      features[features_count++] = f++;
  }
#endif

  static const VdpVideoMixerParameter params[] = {
    VDP_VIDEO_MIXER_PARAMETER_VIDEO_SURFACE_WIDTH,
    VDP_VIDEO_MIXER_PARAMETER_VIDEO_SURFACE_HEIGHT,
    VDP_VIDEO_MIXER_PARAMETER_CHROMA_TYPE,
    VDP_VIDEO_MIXER_PARAMETER_LAYERS
  };
  void const *param_values[] = {
    &this->video_mixer.width,
    &this->video_mixer.height,
    &this->video_mixer.chroma,
    /* requesting buggy layers seems safe, while using them is not. */
    &this->video_mixer.max_layers[0]
  };
  return this->a.vdp.video_mixer.create (this->vdp_device, features_count, features,
    this->video_mixer.max_layers[0] ? 4 : 3, params, param_values, &this->video_mixer.handle);
}


static void vdpau_update_display_dimension (vdpau_driver_t *this)
{
  XLockDisplay (this->display);

  this->display_width  = DisplayWidth(this->display, this->screen);
  this->display_height = DisplayHeight(this->display, this->screen);

  XUnlockDisplay(this->display);
}


static void vdp_preemption_callback(VdpDevice device, void *context)
{
  vdpau_driver_t *this = (vdpau_driver_t *)context;
  this->reinit_needed = 1;
  xprintf (this->xine, XINE_VERBOSITY_DEBUG, LOG_MODULE ": VDPAU preemption callback\n");
  (void)device;
}


static void vdpau_reinit (vdpau_driver_t *this)
{
  int i;

  xprintf (this->xine, XINE_VERBOSITY_DEBUG, LOG_MODULE ": VDPAU was pre-empted. Reinit.\n");

  DO_LOCKDISPLAY (this);
  vdpau_release_back_frames (this);

  VdpStatus st = vdp_device_create_x11 (this->display, this->screen, &this->vdp_device, &this->vdp_get_proc_address);
  if (st != VDP_STATUS_OK) {
    if (st == VDP_STATUS_NO_IMPLEMENTATION)
      xprintf (this->xine, XINE_VERBOSITY_LOG,
        LOG_MODULE ": Can't create vdp device: No vdpau implementation.\n");
    else
      xprintf (this->xine, XINE_VERBOSITY_LOG,
        LOG_MODULE ": Can't create vdp device: unsupported GPU?\n");
    DO_UNLOCKDISPLAY (this);
    return;
  }
#define VDPAU_BAIL_REINIT(msg) \
  if (st != VDP_STATUS_OK) { \
    xprintf (this->xine, XINE_VERBOSITY_LOG, LOG_MODULE ": %s: %s.\n", msg, this->a.vdp.get.error_string (st)); \
    DO_UNLOCKDISPLAY (this); \
  }
  st = this->a.vdp.queue.target_create_x11 (this->vdp_device, this->drawable, &this->vdp_queue_target);
  VDPAU_BAIL_REINIT ("Can't create presentation queue target !!");

  st = this->a.vdp.queue.create (this->vdp_device, this->vdp_queue_target, &this->vdp_queue);
  VDPAU_BAIL_REINIT ("Can't create presentation queue !!");
  this->a.vdp.queue.set_background_color (this->vdp_queue, &this->back_color);

  this->soft_surface.chroma = VDP_CHROMA_TYPE_420;
  st = vdpau_video_surf_new (this, &this->soft_surface);
  VDPAU_BAIL_REINIT ("Can't create video surface !!");

  vdpau_update_display_dimension(this);
  this->current_output_surface = 0;
  this->init_queue = 0;
  this->queue_length = this->queue_user_length;
  for (i = 0; i < this->queue_length; ++i) {
    vdpau_output_surface_t *s = this->output_surfaces + i;
    s->width = this->display_width;
    s->height = this->display_height;
    st = vdpau_output_surf_new (this, s);
    if (st != VDP_STATUS_OK)
      break;
  }
  if (i < this->queue_length) {
    int j;
    for (j = i; j >= 0; j--)
      vdpau_output_surf_delete (this, this->output_surfaces + j);
    vdpau_video_surf_delete (this, &this->soft_surface);
    this->queue_length = i;
    VDPAU_BAIL_REINIT ("Can't create output surface !!");
  }

  this->num_big_output_surfaces_created = 0;
  for (i = 0; i < this->output_surface_buffer_size; ++i)
    this->output_surface_buffer[i].surface = VDP_INVALID_HANDLE;

  this->ovl_layer_surface = VDP_INVALID_HANDLE;
  this->ovl_main_render_surface.surface = VDP_INVALID_HANDLE;
  for (i = 0; i < this->num_ovls; ++i)
    this->overlays[i].render_surface.surface = VDP_INVALID_HANDLE;
  this->num_ovls = 0;
  this->ovl_changed = 1;

  this->video_mixer.chroma = this->soft_surface.chroma;
  st = vdpau_new_video_mixer (this);
  if (st != VDP_STATUS_OK) {
    int j;
    for (j = this->queue_length; j >= 0; j--)
      vdpau_output_surf_delete (this, this->output_surfaces + j);
    vdpau_video_surf_delete (this, &this->soft_surface);
    this->queue_length = 0;
    VDPAU_BAIL_REINIT ("Can't create video mixer !!");
  }

  this->prop_changed = _VOVDP_S_ALL;

  this->a.vdp.preemption_callback_register (this->vdp_device, &vdp_preemption_callback, (void*)this);

  this->vdp_runtime_nr++;
  this->reinit_needed = 0;
  DO_UNLOCKDISPLAY (this);
  xprintf (this->xine, XINE_VERBOSITY_DEBUG, LOG_MODULE ": Reinit done.\n");
}

static void vdpau_display_frame (vo_driver_t *this_gen, vo_frame_t *frame_gen)
{
  vdpau_driver_t  *this  = (vdpau_driver_t *) this_gen;
  vdpau_frame_t   *frame = (vdpau_frame_t *) frame_gen;
  static const VdpOutputSurfaceRenderBlendState ovl_blend = {
    .struct_version = VDP_OUTPUT_SURFACE_RENDER_BLEND_STATE_VERSION,
    .blend_factor_source_color = VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_SRC_ALPHA,
    .blend_factor_destination_color = VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    .blend_factor_source_alpha = VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ZERO,
    .blend_factor_destination_alpha = VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ONE,
    .blend_equation_color = VDP_OUTPUT_SURFACE_RENDER_BLEND_EQUATION_ADD,
    .blend_equation_alpha = VDP_OUTPUT_SURFACE_RENDER_BLEND_EQUATION_ADD,
    .blend_constant = {.red = 0, .green = 0, .blue = 0, .alpha = 0 }
  };
  VdpStatus st;
  VdpVideoSurface surface;
  VdpChromaType chroma = this->video_mixer.chroma;
  uint32_t mix_w = this->video_mixer.width;
  uint32_t mix_h = this->video_mixer.height;
  VdpTime stream_speed;
  int redraw_needed;

  this->sc.force_redraw |=
    (((frame->width                ^ this->sc.delivered_width)
    | (frame->height               ^ this->sc.delivered_height)
    | (frame->vo_frame.crop_left   ^ this->sc.crop_left)
    | (frame->vo_frame.crop_right  ^ this->sc.crop_right)
    | (frame->vo_frame.crop_top    ^ this->sc.crop_top)
    | (frame->vo_frame.crop_bottom ^ this->sc.crop_bottom))
    || (frame->ratio != this->sc.delivered_ratio)) ? 1 : 0;

  this->sc.delivered_height = frame->height;
  this->sc.delivered_width  = frame->width;
  this->sc.delivered_ratio  = frame->ratio;
  this->sc.crop_left        = frame->vo_frame.crop_left;
  this->sc.crop_right       = frame->vo_frame.crop_right;
  this->sc.crop_top         = frame->vo_frame.crop_top;
  this->sc.crop_bottom      = frame->vo_frame.crop_bottom;

  redraw_needed = vdpau_redraw_needed( this_gen );

  pthread_mutex_lock(&this->drawable_lock); /* protect drawble from being changed */

  if(this->reinit_needed)
    vdpau_reinit (this);

  if ( (frame->format == XINE_IMGFMT_YV12) || (frame->format == XINE_IMGFMT_YUY2) ) {
    chroma = ( frame->format==XINE_IMGFMT_YV12 )? VDP_CHROMA_TYPE_420 : VDP_CHROMA_TYPE_422;
    if  ((frame->width != (int)this->soft_surface.width)
      || (frame->height != (int)this->soft_surface.height)
      || (frame->format != this->soft_surface_format)) {
      lprintf( LOG_MODULE ": soft_surface size update\n" );
      /* recreate surface to match frame changes */
      this->soft_surface.chroma = chroma;
      this->soft_surface.width = frame->width;
      this->soft_surface.height = frame->height;
      this->soft_surface_format = frame->format;
      vdpau_video_surf_delete (this, &this->soft_surface);
      vdpau_video_surf_new (this, &this->soft_surface);
    }
    /* FIXME: have to swap U and V planes to get correct colors !! */
    uint32_t pitches[] = { frame->vo_frame.pitches[0], frame->vo_frame.pitches[2], frame->vo_frame.pitches[1] };
    const void* const data[] = { frame->vo_frame.base[0], frame->vo_frame.base[2], frame->vo_frame.base[1] };
    if ( frame->format==XINE_IMGFMT_YV12 ) {
      DO_LOCKDISPLAY (this);
      st = this->a.vdp.video_surface.putbits_ycbcr (this->soft_surface.surface, VDP_YCBCR_FORMAT_YV12, data, pitches);
      DO_UNLOCKDISPLAY (this);
      VDPAU_IF_ERROR ("vdp_video_surface_putbits_ycbcr YV12 error");
    }
    else {
      DO_LOCKDISPLAY (this);
      st = this->a.vdp.video_surface.putbits_ycbcr (this->soft_surface.surface, VDP_YCBCR_FORMAT_YUYV, data, pitches);
      DO_UNLOCKDISPLAY (this);
      VDPAU_IF_ERROR ("vdp_video_surface_putbits_ycbcr YUY2 error");
    }
    surface = this->soft_surface.surface;
    mix_w = this->soft_surface.width;
    mix_h = this->soft_surface.height;
  }
  else if (frame->format == XINE_IMGFMT_VDPAU) {
    surface = frame->vdpau_accel_data.surface;
    mix_w = frame->width;
    mix_h = frame->height;
    chroma = (frame->vo_frame.flags & VO_CHROMA_422) ? VDP_CHROMA_TYPE_422 : VDP_CHROMA_TYPE_420;
  }
  else {
    /* unknown format */
    xprintf (this->xine, XINE_VERBOSITY_LOG, LOG_MODULE ": got an unknown image -------------\n");
    frame->vo_frame.free( &frame->vo_frame );
    pthread_mutex_unlock(&this->drawable_lock); /* allow changing drawable again */
    return;
  }

  vdpau_update_csc_matrix (this, frame);
  vdpau_update_prop (this);

  if ( (mix_w != this->video_mixer.width) || (mix_h != this->video_mixer.height) || (chroma != this->video_mixer.chroma)) {
    vdpau_release_back_frames (this); /* empty past frames array */
    lprintf(LOG_MODULE ": recreate mixer to match frames: width=%d, height=%d, chroma=%d\n", mix_w, mix_h, chroma);

    this->a.vdp.video_mixer.destroy (this->video_mixer.handle);
    this->video_mixer.handle = VDP_INVALID_HANDLE;

    this->video_mixer.chroma = chroma;
    this->video_mixer.width = mix_w;
    this->video_mixer.height = mix_h;
    vdpau_new_video_mixer (this);

    this->prop_changed = _VOVDP_S_ALL;
  }

  if (this->ovl_changed || redraw_needed)
    vdpau_process_overlays(this);

  uint32_t layer_count1, layer_count2 = 0;
  VdpLayer *layer1, *layer2 = NULL, ovl_layer;
  VdpRect *vid_dest, vid_dest_rect;
  if (this->num_ovls && this->ovl_layer_surface != VDP_INVALID_HANDLE) {
    ovl_layer.struct_version = VDP_LAYER_VERSION;
    ovl_layer.source_surface = this->ovl_layer_surface;
    ovl_layer.source_rect = &this->ovl_src_rect;
    ovl_layer.destination_rect = &this->ovl_dest_rect;
    layer1 = &ovl_layer;
    layer_count1 = 1;
    vid_dest = &this->ovl_video_dest_rect;
  } else {
    layer1 = NULL;
    layer_count1 = 0;
    vid_dest_rect.x0 = this->sc.output_xoffset;
    vid_dest_rect.y0 = this->sc.output_yoffset;
    vid_dest_rect.x1 = this->sc.output_xoffset + this->sc.output_width;
    vid_dest_rect.y1 = this->sc.output_yoffset + this->sc.output_height;
    vid_dest = &vid_dest_rect;
  }

  VdpRect vid_source, out_dest;
  if (!(this->transform & XINE_VO_TRANSFORM_FLIP_H)) {
    vid_source.x0 = this->sc.displayed_xoffset;
    vid_source.x1 = this->sc.displayed_width + this->sc.displayed_xoffset;
  } else {
    vid_source.x0 = this->sc.displayed_width + this->sc.displayed_xoffset;
    vid_source.x1 = this->sc.displayed_xoffset;
  }
  if (!(this->transform & XINE_VO_TRANSFORM_FLIP_V)) {
    vid_source.y0 = this->sc.displayed_yoffset;
    vid_source.y1 = this->sc.displayed_height + this->sc.displayed_yoffset;
  } else {
    vid_source.y0 = this->sc.displayed_height + this->sc.displayed_yoffset;
    vid_source.y1 = this->sc.displayed_yoffset;
  }
  out_dest.x0 = out_dest.y0 = 0;
  out_dest.x1 = this->sc.gui_width;
  out_dest.y1 = this->sc.gui_height;
  this->prop_changed = 0;

  stream_speed = frame->vo_frame.stream ? xine_get_param(frame->vo_frame.stream, XINE_PARAM_FINE_SPEED) : 0;

  /* try to get frame duration from previous img->pts when frame->duration is 0 */
  int frame_duration = frame->vo_frame.duration;
  if ( !frame_duration && this->back_frame[0] ) {
    int duration = frame->vo_frame.pts - this->back_frame[0]->vo_frame.pts;
    if ( duration>0 && duration<4000 )
      frame_duration = duration;
  }
  int non_progressive;
  if ( frame->vo_frame.progressive_frame < 0 )
    non_progressive = 0;
  else
    non_progressive = (this->honor_progressive && !frame->vo_frame.progressive_frame) || !this->honor_progressive;

  /* render may crash if we add too much. */
  {
    uint32_t num_layers = this->video_mixer.max_layers[this->video_mixer.layer_bug & 3];
    if (layer_count1 > num_layers) {
      layer_count2 = layer_count1 - num_layers;
      layer_count1 = num_layers;
      layer2 = layer1 + layer_count1;
    }
  }
  if (!layer_count1)
    layer1 = NULL;

  vdpau_output_surface_t *s = this->output_surfaces + this->current_output_surface;
  VdpTime last_time;

  if ( this->init_queue>1 )
    this->a.vdp.queue.block (this->vdp_queue, s->surface, &last_time);

  DO_LOCKDISPLAY (this);

  vdpau_check_output_size (this);

  if ( frame->format==XINE_IMGFMT_VDPAU && this->deinterlace && non_progressive && !(frame->vo_frame.flags & VO_STILL_IMAGE) && frame_duration>2500 ) {
    VdpTime current_time = 0;
    VdpVideoSurface past[2];
    VdpVideoSurface future[1];
    VdpVideoMixerPictureStructure picture_structure;

    past[1] = past[0] = (this->back_frame[0] && (this->back_frame[0]->format==XINE_IMGFMT_VDPAU)) ? this->back_frame[0]->vdpau_accel_data.surface : VDP_INVALID_HANDLE;
    future[0] = surface;
    picture_structure = ( frame->vo_frame.top_field_first ) ? VDP_VIDEO_MIXER_PICTURE_STRUCTURE_TOP_FIELD : VDP_VIDEO_MIXER_PICTURE_STRUCTURE_BOTTOM_FIELD;

    st = this->a.vdp.video_mixer.render (this->video_mixer.handle,
      VDP_INVALID_HANDLE, NULL,
      picture_structure,
      2, past,
      surface,
      1, future,
      &vid_source,
      s->surface,
      &out_dest,
      vid_dest,
      layer_count1, layer1
    );
    VDPAU_IF_ERROR ("vdp_video_mixer_render error");

    while (layer_count2) {
      /* blend manually */
      this->a.vdp.output_surface.render_output_surface (s->surface, layer2->destination_rect,
      layer2->source_surface, layer2->source_rect, NULL, &ovl_blend, VDP_OUTPUT_SURFACE_RENDER_ROTATE_0);
      layer2++;
      layer_count2--;
    }

    vdpau_grab_current_output_surface( this, frame->vo_frame.vpts );
    this->a.vdp.queue.get_time (this->vdp_queue, &current_time);
    this->a.vdp.queue.display (this->vdp_queue, s->surface, this->sc.gui_width, this->sc.gui_height, 0); /* display _now_ */
    vdpau_shift_queue (this);
    s = this->output_surfaces + this->current_output_surface;

    int dm;
    if ( this->video_mixer.width < 800 )
      dm = this->deinterlacers_method[this->deinterlace_method_sd];
    else
      dm = this->deinterlacers_method[this->deinterlace_method_hd];
    
    if ( (dm != DEINT_HALF_TEMPORAL) && (dm != DEINT_HALF_TEMPORAL_SPATIAL) && frame->vo_frame.future_frame ) {  /* process second field */
      if ( this->init_queue >= this->queue_length ) {
        DO_UNLOCKDISPLAY (this);
        this->a.vdp.queue.block (this->vdp_queue, s->surface, &last_time);
        DO_LOCKDISPLAY (this);
      }

      vdpau_check_output_size (this);

      picture_structure = ( frame->vo_frame.top_field_first ) ? VDP_VIDEO_MIXER_PICTURE_STRUCTURE_BOTTOM_FIELD : VDP_VIDEO_MIXER_PICTURE_STRUCTURE_TOP_FIELD;
      past[0] = surface;
      if ( frame->vo_frame.future_frame!=NULL && ((vdpau_frame_t*)(frame->vo_frame.future_frame))->format==XINE_IMGFMT_VDPAU )
        future[0] = ((vdpau_frame_t*)(frame->vo_frame.future_frame))->vdpau_accel_data.surface;
      else
        future[0] = VDP_INVALID_HANDLE;

      st = this->a.vdp.video_mixer.render (this->video_mixer.handle,
        VDP_INVALID_HANDLE, NULL,
        picture_structure,
        2, past,
        surface,
        1, future,
        &vid_source,
        s->surface,
        &out_dest,
        vid_dest,
        layer_count1, layer1
      );
      VDPAU_IF_ERROR ("vdp_video_mixer_render error");

      while (layer_count2) {
        /* blend manually */
        this->a.vdp.output_surface.render_output_surface (s->surface, layer2->destination_rect,
          layer2->source_surface, layer2->source_rect, NULL, &ovl_blend, VDP_OUTPUT_SURFACE_RENDER_ROTATE_0);
        layer2++;
        layer_count2--;
      }

      if ( stream_speed > 0 )
        current_time += frame->vo_frame.duration * 1000000ull * XINE_FINE_SPEED_NORMAL / (180 * stream_speed);

      this->a.vdp.queue.display (this->vdp_queue, s->surface, this->sc.gui_width, this->sc.gui_height, current_time);
      vdpau_shift_queue (this);
    }
  }
  else {
    if ( frame->vo_frame.flags & VO_STILL_IMAGE )
      lprintf( LOG_MODULE ": VO_STILL_IMAGE\n");
    st = this->a.vdp.video_mixer.render (this->video_mixer.handle,
      VDP_INVALID_HANDLE, NULL, /* background */
      VDP_VIDEO_MIXER_PICTURE_STRUCTURE_FRAME, /* src */
      0, NULL, /* past */
      surface, /* src */
      0, NULL, /* future */
      &vid_source, /* src rect */
      s->surface, /* dest */
      &out_dest, /* dest clip */
      vid_dest, /* dest video rect */
      layer_count1, layer1 /* ovl */
    );
    VDPAU_IF_ERROR ("vdp_video_mixer_render error");

    while (layer_count2) {
        /* blend manually */
        this->a.vdp.output_surface.render_output_surface (s->surface, layer2->destination_rect,
        layer2->source_surface, layer2->source_rect, NULL, &ovl_blend, VDP_OUTPUT_SURFACE_RENDER_ROTATE_0);
        layer2++;
        layer_count2--;
    }

    vdpau_grab_current_output_surface( this, frame->vo_frame.vpts );
    this->a.vdp.queue.display (this->vdp_queue, s->surface, this->sc.gui_width, this->sc.gui_height, 0);
    vdpau_shift_queue (this);
    /* s = this->output_surfaces + this->current_output_surface; */
  }

  DO_UNLOCKDISPLAY (this);

  if ( stream_speed ) 
    vdpau_backup_frame (this, frame);
  else /* do not release past frame if paused, it will be used for redrawing */
    frame->vo_frame.free( &frame->vo_frame );

  pthread_mutex_unlock(&this->drawable_lock); /* allow changing drawable again */
}


static int vdpau_get_property (vo_driver_t *this_gen, int property)
{
  vdpau_driver_t *this = (vdpau_driver_t*)this_gen;

  switch (property) {
    case VO_PROP_MAX_NUM_FRAMES:
      return 30;
    case VO_PROP_WINDOW_WIDTH:
      return this->sc.gui_width;
    case VO_PROP_WINDOW_HEIGHT:
      return this->sc.gui_height;
    case VO_PROP_OUTPUT_WIDTH:
      return this->sc.output_width;
    case VO_PROP_OUTPUT_HEIGHT:
      return this->sc.output_height;
    case VO_PROP_OUTPUT_XOFFSET:
      return this->sc.output_xoffset;
    case VO_PROP_OUTPUT_YOFFSET:
      return this->sc.output_yoffset;
    case VO_PROP_HUE:
      return this->hue;
    case VO_PROP_SATURATION:
      return this->saturation;
    case VO_PROP_CONTRAST:
      return this->contrast;
    case VO_PROP_BRIGHTNESS:
      return this->brightness;
    case VO_PROP_SHARPNESS:
      return this->sharpness;
    case VO_PROP_NOISE_REDUCTION:
      return this->noise;
    case VO_PROP_ZOOM_X:
      return this->zoom_x;
    case VO_PROP_ZOOM_Y:
      return this->zoom_y;
    case VO_PROP_ASPECT_RATIO:
      return this->sc.user_ratio;
    case VO_PROP_CAPS2:
      return VO_CAP2_TRANSFORM;
    case VO_PROP_TRANSFORM:
      return this->transform;
  }

  return -1;
}


static int vdpau_set_property (vo_driver_t *this_gen, int property, int value)
{
  vdpau_driver_t *this = (vdpau_driver_t*)this_gen;

  lprintf("vdpau_set_property: property=%d, value=%d\n", property, value );

  switch (property) {
    case VO_PROP_DISCARD_FRAMES:
      if (value == -1)
        value = vdpau_release_back_frames (this);
      break;
    case VO_PROP_INTERLACED:
      this->deinterlace = value;
      this->prop_changed |= _VOVDP_S_DEINT;
      break;
    case VO_PROP_ZOOM_X:
      if ((value >= XINE_VO_ZOOM_MIN) && (value <= XINE_VO_ZOOM_MAX)) {
        this->zoom_x = value;
        this->sc.zoom_factor_x = (double)value / (double)XINE_VO_ZOOM_STEP;
        _x_vo_scale_compute_ideal_size( &this->sc );
        this->sc.force_redraw = 1;    /* trigger re-calc of output size */
      }
      break;
    case VO_PROP_ZOOM_Y:
      if ((value >= XINE_VO_ZOOM_MIN) && (value <= XINE_VO_ZOOM_MAX)) {
        this->zoom_y = value;
        this->sc.zoom_factor_y = (double)value / (double)XINE_VO_ZOOM_STEP;
        _x_vo_scale_compute_ideal_size( &this->sc );
        this->sc.force_redraw = 1;    /* trigger re-calc of output size */
      }
      break;
    case VO_PROP_ASPECT_RATIO:
      if ( value>=XINE_VO_ASPECT_NUM_RATIOS )
        value = XINE_VO_ASPECT_AUTO;
      this->sc.user_ratio = value;
      this->sc.force_redraw = 1;    /* trigger re-calc of output size */
      break;
    case VO_PROP_HUE:
      this->hue = value;
      this->prop_changed |= _VOVDP_S_HUE;
      break;
    case VO_PROP_SATURATION:
      this->saturation = value;
      this->prop_changed |= _VOVDP_S_SATURATION;
      break;
    case VO_PROP_CONTRAST:
      this->contrast = value;
      this->prop_changed |= _VOVDP_S_CONTRAST;
      break;
    case VO_PROP_BRIGHTNESS:
      this->brightness = value;
      this->prop_changed |= _VOVDP_S_BRIGHTNESS;
      break;
    case VO_PROP_SHARPNESS:
      this->sharpness = value;
      this->prop_changed |= _VOVDP_S_SHARPNESS;
      break;
    case VO_PROP_NOISE_REDUCTION:
      this->noise = value;
      this->prop_changed |= _VOVDP_S_NOISE_RED;
      break;
    case VO_PROP_TRANSFORM:
      value &= XINE_VO_TRANSFORM_FLIP_H | XINE_VO_TRANSFORM_FLIP_V;
        this->prop_changed |= (value ^ this->transform) ? _VOVDP_S_TRANSFORM : 0;
      this->transform = value;
      break;
  }

  return value;
}


static void vdpau_get_property_min_max (vo_driver_t *this_gen, int property, int *min, int *max)
{
  (void)this_gen;
  switch ( property ) {
    case VO_PROP_HUE:
      *max = 127; *min = -128; break;
    case VO_PROP_SATURATION:
      *max = 255; *min = 0; break;
    case VO_PROP_CONTRAST:
      *max = 255; *min = 0; break;
    case VO_PROP_BRIGHTNESS:
      *max = 127; *min = -128; break;
    case VO_PROP_SHARPNESS:
      *max = 100; *min = -100; break;
    case VO_PROP_NOISE_REDUCTION:
      *max = 100; *min = 0; break;
    default:
      *max = 0; *min = 0;
  }
}


/*
 * functions for grabbing RGB images from displayed frames
 */
static void vdpau_dispose_grab_video_frame(xine_grab_video_frame_t *frame_gen)
{
  vdpau_grab_video_frame_t *frame = (vdpau_grab_video_frame_t *) frame_gen;

  free(frame->grab_frame.img);
  free(frame->rgba);
  free(frame);
}


/*
 * grab next displayed output surface.
 */
static int vdpau_grab_grab_video_frame (xine_grab_video_frame_t *frame_gen) {
  vdpau_grab_video_frame_t *frame = (vdpau_grab_video_frame_t *) frame_gen;
  vdpau_driver_t *this = (vdpau_driver_t *) frame->vo_driver;
  int yes = 0;

  do {
    if (frame->grab_frame.flags & XINE_GRAB_VIDEO_FRAME_FLAGS_WAIT_NEXT)
      break;
    pthread_mutex_lock (&this->drawable_lock);
    if (this->back_frame[0]) {
      vdpau_grab_video_frame_t *pending_frame;

      pthread_mutex_lock (&this->grab_lock);
      pending_frame = this->pending_grab_request;
      this->pending_grab_request = frame;
      pthread_mutex_unlock (&this->grab_lock);

      vdpau_grab_current_output_surface (this, this->back_frame[0]->vo_frame.vpts);

      pthread_mutex_lock (&this->grab_lock);
      this->pending_grab_request = pending_frame;
      pthread_mutex_unlock (&this->grab_lock);

      yes = 1;
    }
    pthread_mutex_unlock (&this->drawable_lock);
  } while (0);

  if (!yes) {
    struct timeval tvnow;
    struct timespec ts;
    /* calculate absolute timeout time */
    gettimeofday (&tvnow, NULL);
    tvnow.tv_sec += frame->grab_frame.timeout / 1000;
    tvnow.tv_usec += (frame->grab_frame.timeout % 1000) * 1000;
    if (tvnow.tv_usec >= 1000000) {
      tvnow.tv_usec -= 1000000;
      tvnow.tv_sec += 1;
    }
    ts.tv_sec  = tvnow.tv_sec;
    ts.tv_nsec = tvnow.tv_usec * 1000;

    pthread_mutex_lock (&this->grab_lock);

    /* wait until other pending grab request is finished */
    while (this->pending_grab_request) {
      if (pthread_cond_timedwait (&this->grab_cond, &this->grab_lock, &ts) == ETIMEDOUT) {
        pthread_mutex_unlock (&this->grab_lock);
        return 1;   /* no frame available */
      }
    }

    this->pending_grab_request = frame;

    /* wait until our request is finished */
    while (this->pending_grab_request) {
      if (pthread_cond_timedwait (&this->grab_cond, &this->grab_lock, &ts) == ETIMEDOUT) {
        this->pending_grab_request = NULL;
        pthread_mutex_unlock (&this->grab_lock);
        return 1;   /* no frame available */
      }
    }

    pthread_mutex_unlock (&this->grab_lock);
  }

  if (frame->grab_frame.vpts == -1)
    return -1; /* error happened */

  /* convert ARGB image to RGB image */
  uint32_t *src = frame->rgba;
  uint8_t *dst = frame->grab_frame.img;
  int n = frame->width * frame->height;
  while (n--) {
    uint32_t rgba = *src++;
    *dst++ = (uint8_t)(rgba >> 16);  /*R*/
    *dst++ = (uint8_t)(rgba >> 8);   /*G*/
    *dst++ = (uint8_t)(rgba);        /*B*/
  }

  return 0;
}


static xine_grab_video_frame_t * vdpau_new_grab_video_frame(vo_driver_t *this)
{
  vdpau_grab_video_frame_t *frame = calloc(1, sizeof(vdpau_grab_video_frame_t));
  if (frame) {
    frame->grab_frame.dispose = vdpau_dispose_grab_video_frame;
    frame->grab_frame.grab = vdpau_grab_grab_video_frame;
    frame->grab_frame.vpts = -1;
    frame->grab_frame.timeout = XINE_GRAB_VIDEO_FRAME_DEFAULT_TIMEOUT;
    frame->vo_driver = this;
    frame->render_surface.surface = VDP_INVALID_HANDLE;
  }

  return (xine_grab_video_frame_t *) frame;
}

static void vdpau_set_process_snapshots (void *this_gen, xine_cfg_entry_t *entry) {
  vdpau_driver_t *this = (vdpau_driver_t *)this_gen;

  /* vdpau_new_grab_video_frame () is always there, and the pointer to it
   * reads and writes atomically. i guess we dont need locks here ;-) */
  this->vo_driver.new_grab_video_frame = entry->num_value ? vdpau_new_grab_video_frame : NULL;
}

static void vdpau_set_layer_bug (void *this_gen, xine_cfg_entry_t *entry) {
  vdpau_driver_t *this = (vdpau_driver_t *)this_gen;

  if (this->video_mixer.layer_bug != entry->num_value) {
    xprintf (this->xine, XINE_VERBOSITY_DEBUG,
      LOG_MODULE ": layer bug workaround %s%s.\n",
      (entry->num_value == 2) ? "auto " : "",
      this->video_mixer.max_layers[entry->num_value & 3] ? "off" : "on");
  }
  this->video_mixer.layer_bug = entry->num_value;
}


static int vdpau_gui_data_exchange (vo_driver_t *this_gen, int data_type, void *data)
{
  vdpau_driver_t *this = (vdpau_driver_t*)this_gen;

  switch (data_type) {
#ifndef XINE_DISABLE_DEPRECATED_FEATURES
    case XINE_GUI_SEND_COMPLETION_EVENT:
      break;
#endif

    case XINE_GUI_SEND_EXPOSE_EVENT: {
      if ( this->init_queue ) {
        pthread_mutex_lock(&this->drawable_lock); /* wait for other thread which is currently displaying */
        DO_LOCKDISPLAY (this);
        int previous;
        if ( this->current_output_surface )
          previous = this->current_output_surface - 1;
        else
          previous = this->queue_length - 1;
        this->a.vdp.queue.display (this->vdp_queue, this->output_surfaces[previous].surface, 0, 0, 0);
        DO_UNLOCKDISPLAY (this);
        pthread_mutex_unlock(&this->drawable_lock);
      }
      break;
    }

    case XINE_GUI_SEND_DRAWABLE_CHANGED: {
      VdpStatus st;
      pthread_mutex_lock(&this->drawable_lock); /* wait for other thread which is currently displaying */
      DO_LOCKDISPLAY (this);
      this->drawable = (Drawable) data;
      this->a.vdp.queue.destroy (this->vdp_queue);
      this->a.vdp.queue.target_destroy (this->vdp_queue_target);
      st = this->a.vdp.queue.target_create_x11 (this->vdp_device, this->drawable, &this->vdp_queue_target);
      if ( st != VDP_STATUS_OK ) {
        VDPAU_ERROR ("FATAL !! Can't recreate presentation queue target after drawable change !!");
        DO_UNLOCKDISPLAY (this);
        pthread_mutex_unlock(&this->drawable_lock);
        break;
      }
      st = this->a.vdp.queue.create (this->vdp_device, this->vdp_queue_target, &this->vdp_queue);
      if ( st != VDP_STATUS_OK ) {
        VDPAU_ERROR ("FATAL !! Can't recreate presentation queue after drawable change !!");
        DO_UNLOCKDISPLAY (this);
        pthread_mutex_unlock(&this->drawable_lock);
        break;
      }
      this->a.vdp.queue.set_background_color (this->vdp_queue, &this->back_color);
      DO_UNLOCKDISPLAY (this);
      pthread_mutex_unlock(&this->drawable_lock);
      this->sc.force_redraw = 1;
      break;
    }

    case XINE_GUI_SEND_TRANSLATE_GUI_TO_VIDEO: {
      int x1, y1, x2, y2;
      x11_rectangle_t *rect = data;

      _x_vo_scale_translate_gui2video(&this->sc, rect->x, rect->y, &x1, &y1);
      _x_vo_scale_translate_gui2video(&this->sc, rect->x + rect->w, rect->y + rect->h, &x2, &y2);
      rect->x = x1;
      rect->y = y1;
      rect->w = x2-x1;
      rect->h = y2-y1;
      break;
    }

    default:
      return -1;
  }

  return 0;
}


static uint32_t vdpau_get_capabilities (vo_driver_t *this_gen)
{
  vdpau_driver_t *this = (vdpau_driver_t *) this_gen;

  return this->capabilities;
}


static void vdpau_dispose (vo_driver_t *this_gen)
{
  vdpau_driver_t *this = (vdpau_driver_t *) this_gen;
  int i;

  pthread_mutex_destroy (&this->os_mutex);

  /* cm_close already does this.
  this->xine->config->unregister_callbacks (this->xine->config, NULL, NULL, this, sizeof (*this));
  */
  cm_close (this);
  _x_vo_scale_cleanup (&this->sc, this->xine->config);

  if ( this->vdp_queue != VDP_INVALID_HANDLE )
    this->a.vdp.queue.destroy (this->vdp_queue);

  if ( this->vdp_queue_target != VDP_INVALID_HANDLE )
    this->a.vdp.queue.target_destroy (this->vdp_queue_target);

  if (this->video_mixer.handle != VDP_INVALID_HANDLE)
    this->a.vdp.video_mixer.destroy (this->video_mixer.handle);

  vdpau_video_surf_delete (this, &this->soft_surface);

  if (this->a.vdp.output_surface.destroy) {
    DO_LOCKDISPLAY (this);
    vdpau_output_surf_delete (this, &this->ovl_main_render_surface);
    for (i = 0; i < this->num_ovls; ++i) {
      vdpau_overlay_t *ovl = &this->overlays[i];
      vdpau_output_surf_delete (this, &ovl->render_surface);
    }
    for (i = 0; i < this->queue_length; ++i)
      vdpau_output_surf_delete (this, this->output_surfaces + i);
    for (i = 0; i < this->output_surface_buffer_size; ++i)
      vdpau_output_surf_delete (this, this->output_surface_buffer + i);
    DO_UNLOCKDISPLAY (this);
  }

  for ( i=0; i<NUM_FRAMES_BACK; i++ )
    if ( this->back_frame[i] )
      this->back_frame[i]->vo_frame.dispose( &this->back_frame[i]->vo_frame );

  if ( (this->vdp_device != VDP_INVALID_HANDLE) && this->a.vdp.device.destroy )
    this->a.vdp.device.destroy (this->vdp_device);

  pthread_mutex_destroy(&this->grab_lock);
  pthread_cond_destroy(&this->grab_cond);
  pthread_mutex_destroy(&this->drawable_lock);
  xine_free_aligned (this->ovl_pixmap);
  free (this);
}


static int vdpau_get_funcs (vdpau_driver_t *this) {
  const vdpau_func_t *func = &vdpau_funcs[0];
  void **t = this->a.funcs;
  {
    VdpStatus st = this->vdp_get_proc_address (this->vdp_device, func->id, t);
    if (st != VDP_STATUS_OK) {
      xprintf (this->xine, XINE_VERBOSITY_LOG,
        LOG_MODULE ": no address for %s.\n", func->name);
      return 1;
    }
    func++;
    t++;
  }
  while (func->name) {
    VdpStatus st = this->vdp_get_proc_address (this->vdp_device, func->id, t);
    if (st != VDP_STATUS_OK) {
      xprintf (this->xine, XINE_VERBOSITY_LOG,
        LOG_MODULE ": no address for %s: %s.\n", func->name, this->a.vdp.get.error_string (st));
      return 1;
    }
    func++;
    t++;
  }
  return 0;
}


static vo_driver_t *vdpau_open_plugin (video_driver_class_t *class_gen, const void *visual_gen)
{
  vdpau_class_t       *class   = (vdpau_class_t *) class_gen;
  const x11_visual_t  *visual  = (const x11_visual_t *) visual_gen;
  vdpau_driver_t      *this;
  config_values_t      *config  = class->xine->config;
  int i;

  this = (vdpau_driver_t *) calloc(1, sizeof(vdpau_driver_t));
  if (!this)
    return NULL;

  this->xine = class->xine;

  this->vdp_device = VDP_INVALID_HANDLE;
  VdpStatus st = vdp_device_create_x11 (visual->display, visual->screen, &this->vdp_device, &this->vdp_get_proc_address);
  if (st != VDP_STATUS_OK) {
    if (st == VDP_STATUS_NO_IMPLEMENTATION)
      xprintf (this->xine, XINE_VERBOSITY_LOG,
        LOG_MODULE ": no vdp device: No vdpau implementation.\n");
    else
      xprintf (this->xine, XINE_VERBOSITY_LOG,
        LOG_MODULE ": no vdp device: unsupported GPU?\n");
    free (this);
    return NULL;
  }
  if (vdpau_get_funcs (this)) {
    free (this);
    return NULL;
  }

  this->display       = visual->display;
  this->screen        = visual->screen;
  this->drawable      = visual->d;
  pthread_mutex_init(&this->drawable_lock, 0);

  _x_vo_scale_init(&this->sc, 1, 0, config);
  this->sc.frame_output_cb  = visual->frame_output_cb;
  this->sc.dest_size_cb     = visual->dest_size_cb;
  this->sc.user_data        = visual->user_data;
  this->sc.user_ratio       = XINE_VO_ASPECT_AUTO;

  this->zoom_x              = 100;
  this->zoom_y              = 100;


  this->vo_driver.get_capabilities     = vdpau_get_capabilities;
  this->vo_driver.alloc_frame          = vdpau_alloc_frame;
  this->vo_driver.update_frame_format  = vdpau_update_frame_format;
  this->vo_driver.overlay_begin        = vdpau_overlay_begin;
  this->vo_driver.overlay_blend        = vdpau_overlay_blend;
  this->vo_driver.overlay_end          = vdpau_overlay_end;
  this->vo_driver.display_frame        = vdpau_display_frame;
  this->vo_driver.get_property         = vdpau_get_property;
  this->vo_driver.set_property         = vdpau_set_property;
  this->vo_driver.get_property_min_max = vdpau_get_property_min_max;
  this->vo_driver.gui_data_exchange    = vdpau_gui_data_exchange;
  this->vo_driver.dispose              = vdpau_dispose;
  this->vo_driver.redraw_needed        = vdpau_redraw_needed;
  this->vo_driver.new_grab_video_frame = NULL; /* see below */

  this->video_mixer.handle = VDP_INVALID_HANDLE;
  for (i = 0; i < NOUTPUTSURFACE; ++i)
    this->output_surfaces[i].surface = VDP_INVALID_HANDLE;
  this->soft_surface.surface = VDP_INVALID_HANDLE;
  this->vdp_queue = VDP_INVALID_HANDLE;
  this->vdp_queue_target = VDP_INVALID_HANDLE;

  /*
  this->vdp_output_surface.destroy = NULL;
  this->vdp_device.destroy = NULL;
  */
#ifndef HAVE_ZERO_SAFE_MEM
  for (i = 0; i < NUM_FRAMES_BACK; i++)
    this->back_frame[i] = NULL;
  this->pending_grab_request = NULL;
  this->allocated_surfaces = 0;
  this->noise = 0;
  this->deinterlace = 0;

  this->hue = 0;
  this->brightness = 0;
  this->sharpness = 0;
  this->scaling_level_max = 0;
  this->scaling_level_current = 0;

  this->transform = 0;
  this->surface_cleared_nr = 0;
  this->ovl_changed = 0;
  this->num_ovls = 0;
  this->old_num_ovls = 0;
  this->ovl_pixmap = NULL;
  this->ovl_pixmap_size = 0;
  this->ovl_src_rect.x0 = 0;
  this->ovl_src_rect.y0 = 0;
#endif
  this->saturation = 128;
  this->contrast = 128;
  this->color_matrix = 10;

  this->vdp_runtime_nr = 1;

  this->ovl_layer_surface = VDP_INVALID_HANDLE;
  this->ovl_main_render_surface.surface = VDP_INVALID_HANDLE;

  this->capabilities =
    VO_CAP_YV12 | VO_CAP_YUY2 | VO_CAP_COLOR_MATRIX | VO_CAP_FULLRANGE |
    VO_CAP_CROP | VO_CAP_UNSCALED_OVERLAY | VO_CAP_CUSTOM_EXTENT_OVERLAY |
    VO_CAP_ARGB_LAYER_OVERLAY | VO_CAP_VIDEO_WINDOW_OVERLAY |
    VO_CAP_HUE | VO_CAP_SATURATION | VO_CAP_CONTRAST | VO_CAP_BRIGHTNESS;

  {
    uint32_t tmp = 0;
    this->a.vdp.get.api_version (&tmp);
    xprintf (class->xine, XINE_VERBOSITY_LOG, LOG_MODULE ": API version %u\n", (unsigned int)tmp);
  }
#define VDPAU_INIT_BAIL(text) \
  if (st != VDP_STATUS_OK) { \
    xprintf (this->xine, XINE_VERBOSITY_LOG, LOG_MODULE ": %s: %s.\n", text, this->a.vdp.get.error_string (st)); \
    this->vo_driver.dispose (&this->vo_driver); \
    return NULL; \
  }

  _vdpau_feature_test (this);
  {
    uint32_t missing = (~this->features) & _vdpau_required_features;
    if (missing) {
      char buf[1024];
      _vdpau_feature_names (buf, sizeof (buf), missing);
      xprintf (this->xine, XINE_VERBOSITY_DEBUG, LOG_MODULE ": features required but missing: %s.\n", buf);
      this->vo_driver.dispose (&this->vo_driver);
      return NULL;
    }
  }

  st = this->a.vdp.preemption_callback_register (this->vdp_device, &vdp_preemption_callback, (void*)this);
  VDPAU_INIT_BAIL ("Can't register preemption callback !!");

  st = this->a.vdp.queue.target_create_x11 (this->vdp_device, this->drawable, &this->vdp_queue_target);
  VDPAU_INIT_BAIL ("Can't create presentation queue target !!");

  st = this->a.vdp.queue.create (this->vdp_device, this->vdp_queue_target, &this->vdp_queue);
  VDPAU_INIT_BAIL ("Can't create presentation queue !!");

  /* choose almost black as backcolor for color keying */
  this->back_color.red = 0.02;
  this->back_color.green = 0.01;
  this->back_color.blue = 0.03;
  this->back_color.alpha = 1;
  this->a.vdp.queue.set_background_color (this->vdp_queue, &this->back_color);

  this->soft_surface.width = 320;
  this->soft_surface.height = 240;
  this->soft_surface_format = XINE_IMGFMT_YV12;
  this->soft_surface.chroma = VDP_CHROMA_TYPE_420;
  st = vdpau_video_surf_new (this, &this->soft_surface);
  VDPAU_INIT_BAIL ("Can't create video surface !!");

  this->output_surface_buffer_size = config->register_num (config,
    "video.output.vdpau_output_surface_buffer_size", 10, /* default */
    _("maximum number of output surfaces buffered for reuse"),
    _("The maximum number of video output surfaces buffered for reuse"),
    20, NULL, this);
  if (this->output_surface_buffer_size < 2)
    this->output_surface_buffer_size = 2;
  if (this->output_surface_buffer_size > NOUTPUTSURFACEBUFFER)
    this->output_surface_buffer_size = NOUTPUTSURFACEBUFFER;
  xprintf (class->xine, XINE_VERBOSITY_LOG,
    LOG_MODULE ": hold a maximum of %d video output surfaces for reuse\n",
    this->output_surface_buffer_size);

  this->num_big_output_surfaces_created = 0;
  for (i = 0; i < this->output_surface_buffer_size; ++i)
    this->output_surface_buffer[i].surface = VDP_INVALID_HANDLE;

  vdpau_update_display_dimension(this);

  this->queue_user_length = config->register_num (config,
    "video.output.vdpau_display_queue_length", 3, /* default */
    _("default length of display queue"),
    _("The default number of video output surfaces to create for the display queue"),
    20, NULL, this);
  if (this->queue_user_length < 2)
    this->queue_user_length = 2;
  if (this->queue_user_length > NOUTPUTSURFACE)
    this->queue_user_length = NOUTPUTSURFACE;
  this->queue_length = this->queue_user_length;
  xprintf (class->xine, XINE_VERBOSITY_LOG,
    LOG_MODULE ": using %d output surfaces of size %dx%d for display queue\n",
    this->queue_length, this->display_width, this->display_height);

  DO_LOCKDISPLAY (this);
  this->current_output_surface = 0;
  this->init_queue = 0;
  for ( i=0; i<this->queue_length; ++i ) {
    this->output_surfaces[i].width = this->display_width;
    this->output_surfaces[i].height = this->display_height;
    st = vdpau_output_surf_new (this, this->output_surfaces + i);
    if (st != VDP_STATUS_OK)
      break;
  }
  DO_UNLOCKDISPLAY (this);
  if (i < this->queue_length) {
    this->queue_length = i;
    VDPAU_INIT_BAIL ("Can't create output surface !!");
  }

  {
    char deinterlacers_description[1024], *q = deinterlacers_description, *e = deinterlacers_description + 1024;
    int deint_count = 0, deint_default = 0;

    this->deinterlacers_name[deint_count] = vdpau_deinterlacer_name[0];
    this->deinterlacers_method[deint_count++] = DEINT_BOB;
    q += strlcpy (q, vdpau_deinterlacer_description[0], e - q);

    if (_vdpau_feature_have (this, _VOVDP_I_temporal)) {
      this->deinterlacers_name[deint_count] = vdpau_deinterlacer_name[1];
      this->deinterlacers_method[deint_count++] = DEINT_HALF_TEMPORAL;
      q += strlcpy (q, vdpau_deinterlacer_description[1], e - q);
    }

    if (_vdpau_feature_have (this, _VOVDP_I_temporal_spatial)) {
      this->deinterlacers_name[deint_count] = vdpau_deinterlacer_name[2];
      this->deinterlacers_method[deint_count++] = DEINT_HALF_TEMPORAL_SPATIAL;
      q += strlcpy (q, vdpau_deinterlacer_description[2], e - q);
    }

    if (_vdpau_feature_have (this, _VOVDP_I_temporal)) {
      this->deinterlacers_name[deint_count] = vdpau_deinterlacer_name[3];
      this->deinterlacers_method[deint_count] = DEINT_TEMPORAL;
      q += strlcpy (q, vdpau_deinterlacer_description[3], e - q);
      deint_default = deint_count++;
    }
    if (_vdpau_feature_have (this, _VOVDP_I_temporal_spatial)) {
      this->deinterlacers_name[deint_count] = vdpau_deinterlacer_name[4];
      this->deinterlacers_method[deint_count++] = DEINT_TEMPORAL_SPATIAL;
      q += strlcpy (q, vdpau_deinterlacer_description[4], e - q);
    }

    *q = 0;
    this->deinterlacers_name[deint_count] = NULL;

    this->video_mixer.chroma = this->soft_surface.chroma;
    this->video_mixer.width = this->soft_surface.width;
    this->video_mixer.height = this->soft_surface.height;
    st = vdpau_new_video_mixer (this);
    VDPAU_INIT_BAIL ("Can't create video mixer !!");

    this->deinterlace_method_hd = config->register_enum (config,
      "video.output.vdpau_hd_deinterlace_method",
      deint_default, (char **)this->deinterlacers_name,
      _("vdpau: HD deinterlace method"),
      deinterlacers_description,
      10, vdpau_update_deinterlace_method_hd, this);

    this->deinterlace_method_sd = config->register_enum (config,
      "video.output.vdpau_sd_deinterlace_method",
      deint_default, (char **)this->deinterlacers_name,
      _("vdpau: SD deinterlace method"),
      deinterlacers_description,
      10, vdpau_update_deinterlace_method_sd, this);
  }

  if (this->scaling_level_max)
    this->scaling_level_current = config->register_range (config,
      "video.output.vdpau_scaling_quality",
      0, 0, this->scaling_level_max,
      _("vdpau: Scaling Quality"),
      _("Scaling Quality Level"),
      10, vdpau_update_scaling_level, this);

  if (_vdpau_feature_have (this, _VOVDP_I_inverse_telecine))
    this->enable_inverse_telecine = config->register_bool (config,
      "video.output.vdpau_enable_inverse_telecine", 1,
      _("vdpau: Try to recreate progressive frames from pulldown material"),
      _("Enable this to detect bad-flagged progressive content to which\n"
        "a 2:2 or 3:2 pulldown was applied.\n\n"),
      10, vdpau_update_enable_inverse_telecine, this);

  this->honor_progressive = config->register_bool (config,
    "video.output.vdpau_honor_progressive", 0,
    _("vdpau: disable deinterlacing when progressive_frame flag is set"),
    _("Set to true if you want to trust the progressive_frame stream's flag.\n"
      "This flag is not always reliable.\n\n"),
    10, vdpau_honor_progressive_flag, this);

  if (_vdpau_feature_have (this, _VOVDP_I_no_chroma))
    this->skip_chroma = config->register_bool (config,
      "video.output.vdpau_skip_chroma_deinterlace", 0,
      _("vdpau: disable advanced deinterlacers chroma filter"),
      _("Setting to true may help if your video card isn't able to run advanced deinterlacers.\n\n"),
      10, vdpau_set_skip_chroma, this);

  if (_vdpau_feature_have (this, _VOVDP_A_background_color))
    this->background = config->register_num (config,
      "video.output.vdpau_background_color", 0,
      _("vdpau: colour of none video area in output window"),
      _("Displaying 4:3 images on 16:9 plasma TV sets lets the inactive pixels outside the "
        "video age slower than the pixels in the active area. Setting a different background "
        "colour (e. g. 8421504) makes all pixels age similarly. The number to enter for a "
        "certain colour can be derived from its 6 digit hexadecimal RGB value.\n\n"),
      10, vdpau_set_background, this);

  {
    static const char *const vdpau_sd_only_properties[] = {
      "none", "noise", "sharpness", "noise+sharpness", NULL
    };
    this->sd_only_properties = config->register_enum (config,
      "video.output.vdpau_sd_only_properties", 0, (char **)vdpau_sd_only_properties,
      _("vdpau: restrict enabling video properties for SD video only"),
      _("none\n"
        "No restrictions\n\n"
        "noise\n"
        "Restrict noise reduction property.\n\n"
        "sharpness\n"
        "Restrict sharpness property.\n\n"
        "noise+sharpness"
        "Restrict noise and sharpness properties.\n\n"),
      10, vdpau_update_sd_only_properties, this);
  }
  /* number of video frames from config - register it with the default value. */
  {
    int frame_num = config->register_num (config,
      "engine.buffers.video_num_frames", 15, /* default */
      _("default number of video frames"),
      _("The default number of video frames to request from xine video out driver. "
        "Some drivers will override this setting with their own values."),
      20, NULL, this);

    /* now make sure we have at least 22 frames, to prevent locks with vdpau_h264. */
    if (frame_num < 22)
      config->update_num (config, "engine.buffers.video_num_frames", 22);
  }

  this->vo_driver.new_grab_video_frame = config->register_bool (config,
    "video.output.vdpau_process_snapshots", 0,
    _("vdpau: take snapshots as shown on the screen"),
    _("If set, snapshots will be scaled, cropped, padded and subtitled.\n"
      "Otherwise, you get the pure original video image.\n"),
    10, vdpau_set_process_snapshots, this) ? vdpau_new_grab_video_frame : NULL;

  {
    static const char * const vdpau_layer_bug_opts[] = {"Off", "On", "Auto", NULL};
    this->video_mixer.layer_bug = config->register_enum (config,
      "video.output.vdpau_layer_bug", 2, (char **)vdpau_layer_bug_opts,
      _("vdpau: work around broken video mixer overlays"),
      _("Some 2022 vdpau_radeonsi system driver crashes when showing\n"
        "subtitles or onscreen messages the regular way.\n"
        "The \"On\" setting is slightly slower but helps there.\n"
        "The \"Off\" setting avoids flicker on some other drivers.\n"),
      10, vdpau_set_layer_bug, this);
  }
  xprintf (this->xine, XINE_VERBOSITY_DEBUG,
    LOG_MODULE ": layer bug workaround %s%s.\n",
    (this->video_mixer.layer_bug == 2) ? "auto " : "",
    this->video_mixer.max_layers[this->video_mixer.layer_bug & 3] ? "off" : "on");

  pthread_mutex_init(&this->os_mutex, NULL);
  pthread_mutex_init(&this->grab_lock, NULL);
  pthread_cond_init(&this->grab_cond, NULL);

  cm_init (this);

  return &this->vo_driver;
}


/*
 * class functions
 */

static void *vdpau_init_class (xine_t *xine, const void *visual_gen)
{
  vdpau_class_t *this;

  (void)visual_gen;

  this = calloc(1, sizeof(*this));
  if (!this)
    return NULL;

  this->driver_class.open_plugin     = vdpau_open_plugin;
  this->driver_class.identifier      = "vdpau";
  this->driver_class.description     = N_("xine video output plugin using VDPAU hardware acceleration");
  this->driver_class.dispose         = default_video_driver_class_dispose;
  this->xine                         = xine;

  return this;
}


static const vo_info_t vo_info_vdpau = {
  .priority    = 11,
  .visual_type = XINE_VISUAL_TYPE_X11,
};


/*
 * exported plugin catalog entry
 */

const plugin_info_t xine_plugin_info[] EXPORTED = {
  /* type, API, "name", version, special_info, init_function */
  { PLUGIN_VIDEO_OUT, 22, "vdpau", XINE_VERSION_CODE, &vo_info_vdpau, vdpau_init_class },
  { PLUGIN_NONE, 0, NULL, 0, NULL, NULL }
};
