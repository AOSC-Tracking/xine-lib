/*
 * Copyright (C) 2010-2022 the xine project
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110, USA
 *
 * DVB Subtitle decoder (ETS 300 743)
 * (c) 2004 Mike Lampard <mlampard@users.sourceforge.net>
 * based on the application dvbsub by Dave Chapman
 *
 * TODO:
 * - Implement support for teletext based subtitles
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef GEN_DEFAULT_CLUT

#include <stdint.h>
#include <stdio.h>
#include "../../include/xine/xineutils.h"

int main (int argn, char **argv) {
  int y_r_table[256];
  int y_g_table[256];
  int y_b_table[256];
  int uv_br_table[256];
  int u_r_table[256];
  int u_g_table[256];
  int v_b_table[256];
  int v_g_table[256];
  uint8_t default_clut[256][4];
  int i;

  static const uint8_t f[5 * 4 * 2] = {
    0,  85, 0, 170,   0,   0,   0,   0,
    0,  85, 0, 170,   0,   0, 128, 128,
    0,  43, 0,  85, 127, 127,   0,   0,
    0,  43, 0,  85,   0,   0,   0,   0,
    0, 255, 0,   0,   0,   0,   0,   0
  };

  /* ITU-R 601 */
#define KB 0.114
#define KR 0.299

#define YR (219.0/255.0)*KR*SCALEFACTOR
#define YG (219.0/255.0)*(1.0-KB-KR)*SCALEFACTOR
#define YB (219.0/255.0)*KB*SCALEFACTOR

#define UR (112.0/255.0)*(KR/(KB-1.0))*SCALEFACTOR
#define UG (112.0/255.0)*((1.0-KB-KR)/(KB-1.0))*SCALEFACTOR
#define UB (112.0/255.0)*SCALEFACTOR

#define VR (112.0/255.0)*SCALEFACTOR
#define VG (112.0/255.0)*((1.0-KB-KR)/(KR-1.0))*SCALEFACTOR
#define VB (112.0/255.0)*(KB/(KR-1.0))*SCALEFACTOR

  /* initialize the RGB -> YUV tables */
  for (i = 0; i < 256; i++) {

    /* fast and correctly rounded */
    y_r_table[i] = YR * i;
    y_g_table[i] = YG * i + 16 * SCALEFACTOR + SCALEFACTOR / 2;
    y_b_table[i] = YB * i;

    uv_br_table[i] = UB * i + CENTERSAMPLE * SCALEFACTOR + SCALEFACTOR / 2;
    u_r_table[i] = UR * i;
    u_g_table[i] = UG * i;
    v_g_table[i] = VG * i;
    v_b_table[i] = VB * i;
  }

#define YUVA(i,r,g,b,trans) do { \
  default_clut[i][0] = COMPUTE_Y (r, g, b); \
  default_clut[i][1] = COMPUTE_U (r, g, b); \
  default_clut[i][2] = COMPUTE_V (r, g, b); \
  default_clut[i][3] = trans; \
} while (0)

  YUVA (0, 0, 0, 0, 255);
  for (i = 1; i < 8; i++)
    YUVA (i, f[32 + ((i >> 0) & 1)], f[32 + ((i >> 1) & 1)], f[32 + ((i >> 2) & 1)], 192);
  for (; i < 256; i++) {
    uint8_t j, r, g, b, trans;
    /* bits 7, 3 --> 4, 3 */
    const uint8_t *t = f + ((((i & 0x88) * 0x09) >> 3) & 0x18);
    r = t[0 + ((i >> 0) & 1)] + t[2 + ((i >> 4) & 1)] + t[4];
    g = t[0 + ((i >> 1) & 1)] + t[2 + ((i >> 5) & 1)] + t[4];
    b = t[0 + ((i >> 2) & 1)] + t[2 + ((i >> 6) & 1)] + t[4];
    trans = t[6];
    YUVA (i, r, g, b, trans);
  }

  printf ("  static const clut_union_t default_clut[256] = {\n");
  for (i = 0; i < 256; i++)
    printf ("    [%3d] = {.c = {%3u, %3u, %3u, %3u}},\n", i,
        (unsigned int)default_clut[i][0],
        (unsigned int)default_clut[i][1],
        (unsigned int)default_clut[i][2],
        (unsigned int)default_clut[i][3]);
  printf ("  };\n");
  return 0;
}

#else /* !GEN_DEFAULT_CLUT */

#include <time.h>
#include <pthread.h>
#include <errno.h>

/*#define LOG*/
#define LOG_MODULE "spudvb"

#include <xine/xine_internal.h>
#include <xine/spu.h>
#include <xine/osd.h>

#include "xine-engine/bswap.h"

#define MAX_REGIONS 16

#define SPU_MAX_WIDTH 1920
#define SPU_MAX_HEIGHT 1080

#ifndef INT64_MAX
#  define INT64_MAX ((int64_t)((~(uint64_t)0) >> 1))
#endif

/* sparse_array - handle large arrays efficiently when only a few entries are used */

typedef struct {
  const uint32_t key;
  uint32_t value;
} sparse_array_entry_t;

typedef struct {
  uint32_t key, value;
} _sparse_array_entry_t;

typedef struct {
  uint32_t sorted_entries, used_entries, max_entries;
  _sparse_array_entry_t *entries;
} sparse_array_t;

static void sparse_array_new (sparse_array_t *sa) {
  sa->sorted_entries =
  sa->used_entries   =
  sa->max_entries    = 0;
  sa->entries        = NULL;
}

static void sparse_array_clear (sparse_array_t *sa) {
  sa->sorted_entries =
  sa->used_entries   =
  sa->max_entries    = 0;
}

static void sparse_array_delete (sparse_array_t *sa) {
  sa->sorted_entries =
  sa->used_entries   =
  sa->max_entries    = 0;
  _x_freep (&sa->entries);
}

static uint32_t _sparse_array_find (sparse_array_t *sa, uint32_t key) {
  uint32_t b = 0, e = sa->sorted_entries;

  while (b < e) {
    uint32_t m = (b + e) >> 1;

    if (key <= sa->entries[m].key)
      e = m;
    else
      b = m + 1;
  }
  return b;
}

static void _sparse_array_sort (sparse_array_t *sa) {
  uint32_t left = sa->max_entries - sa->used_entries;
  uint32_t i = left + sa->sorted_entries;

  /* move unsorted part to end of buf */
  memmove (sa->entries + i, sa->entries + sa->sorted_entries,
    (sa->used_entries - sa->sorted_entries) * sizeof (_sparse_array_entry_t));

  /* iterate it */
  while (i < sa->max_entries) {
    uint32_t j, pos, startkey, stopkey, lastkey;
    startkey = sa->entries[i].key;
    pos = _sparse_array_find (sa, startkey);
    if ((pos < sa->sorted_entries) && (sa->entries[pos].key == startkey)) {
      /* eliminate duplicate */
      sa->entries[pos].value = sa->entries[i].value;
      i++;
      continue;
    }
    /* find sorted range */
    stopkey = (pos < sa->sorted_entries) ? sa->entries[pos].key : 0xffffffff;
    lastkey = startkey;
    for (j = i + 1; j < sa->max_entries; j++) {
      uint32_t thiskey = sa->entries[j].key;
      if ((thiskey <= lastkey) || (thiskey >= stopkey))
        break;
      lastkey = thiskey;
    }
    j -= i;
    if (j > left)
      j = left;
    /* insert it */
    if (pos < sa->sorted_entries)
      memmove (sa->entries + pos + j, sa->entries + pos,
        (sa->sorted_entries - pos) * sizeof (_sparse_array_entry_t));
    memcpy (sa->entries + pos, sa->entries + i, j * sizeof (_sparse_array_entry_t));
    sa->sorted_entries += j;
    i += j;
  }
  sa->used_entries = sa->sorted_entries;
  lprintf ("sparse_array_sort: %u entries\n", (unsigned int)sa->used_entries);
}

static int sparse_array_set (sparse_array_t *sa, uint32_t key, uint32_t value) {
  /* give some room for later sorting too */
  if (!sa->entries || (sa->used_entries + 8 >= sa->max_entries)) {
    uint32_t n = sa->max_entries + 128;
    _sparse_array_entry_t *se = realloc (sa->entries, n * sizeof (_sparse_array_entry_t));
    if (!se)
      return 0;
    sa->max_entries = n;
    sa->entries = se;
  }
  sa->entries[sa->used_entries].key = key;
  sa->entries[sa->used_entries++].value = value;
  return 1;
}

static sparse_array_entry_t *sparse_array_get (sparse_array_t *sa, uint32_t key) {
  uint32_t pos;
  if (sa->sorted_entries != sa->used_entries)
    _sparse_array_sort (sa);
  pos = _sparse_array_find (sa, key);
  return (sparse_array_entry_t *)sa->entries + pos;
}

static void sparse_array_unset (sparse_array_t *sa, uint32_t key, uint32_t mask) {
  _sparse_array_entry_t *here = sa->entries, *p = NULL, *q = sa->entries;
  uint32_t i, n = 0;
  if (sa->sorted_entries != sa->used_entries)
    _sparse_array_sort (sa);
  key &= mask;
  for (i = sa->used_entries; i > 0; i--) {
    if ((here->key & mask) == key) {
      if (p) {
        n = here - p;
        if (n && (p != q))
          memmove (q, p, n * sizeof (_sparse_array_entry_t));
        p = NULL;
        q += n;
      }
    } else {
      if (!p)
        p = here;
    }
    here++;
  }
  if (p) {
    n = here - p;
    if (n && (p != q))
      memmove (q, p, n * sizeof (_sparse_array_entry_t));
    q += n;
  }
  sa->sorted_entries =
  sa->used_entries   = q - sa->entries;
}

/* ! sparse_array */

typedef struct {
  uint16_t              id;
  uint8_t               time_out;
  uint8_t               version:6, state:2;
  int                   max_hold_vpts;
  struct {
    uint16_t            x, y;
  }                     regions[MAX_REGIONS];
} page_t;

typedef struct {
  uint8_t               version;
  uint8_t               depth;
  uint8_t               CLUT_id;
#define _REGION_FLAG_CHANGED 1
#define _REGION_FLAG_FILL    2
#define _REGION_FLAG_SHOW    4
#define _REGION_FLAG_VISIBLE 8
  uint8_t               flags;
  uint16_t              width, height;
  int64_t               show_vpts;
  int64_t               hide_vpts;
  int64_t               stream_hide_vpts;
  unsigned char        *img;
  osd_object_t         *osd;
} region_t;

typedef union {
  clut_t   c;
  uint32_t u32;
} clut_union_t;

typedef struct {
  uint8_t  version_number;
  uint8_t  windowed;
  uint16_t width;
  uint16_t height;
  /* window, TODO */
  /* uint16_t x0, x1, y0, y1; */
} dds_t;

typedef struct {
/* dvbsub stuff */
  int                   x;
  int                   y;
  unsigned int          curr_obj;
  unsigned int          curr_reg[64];

  uint8_t              *buf;
  int                   i;
  int                   i_bits;

  int                   compat_depth;

  page_t                page;
  dds_t                 dds;

  uint32_t              region_num;
  uint8_t               region_ids[256];
  int64_t               region_vpts[MAX_REGIONS];
  region_t              regions[MAX_REGIONS];

  clut_union_t          colours[MAX_REGIONS * 256];
  uint8_t               trans[MAX_REGIONS * 256];
  uint8_t               clut_cm[MAX_REGIONS];
  int                   clut_gen[MAX_REGIONS + 1];
  uint32_t              clut_num;
  uint8_t               clut_ids[256];
  struct _lut_group_s {
    uint8_t             lut24[4], lut28[4], lut48[16];
  }                     lut[MAX_REGIONS];

  sparse_array_t        object_pos;
} dvbsub_func_t;

typedef struct dvb_spu_decoder_s {
  spu_decoder_t spu_decoder;

  xine_stream_t        *stream;

  spu_dvb_descriptor_t spu_descriptor;

  char                 *pes_pkt_wrptr;
  unsigned int          pes_pkt_size;

  int                   timeout;
  int                   longest_hold_vpts;
  int64_t               vpts;

  dvbsub_func_t         dvbsub;
  int                   show;

  char                  pes_pkt[65 * 1024];

} dvb_spu_decoder_t;

static void _region_init (dvbsub_func_t *dvbsub) {
  dvbsub->region_num = 0;
  memset (dvbsub->region_ids, 255, sizeof (dvbsub->region_ids));
}

static void _region_deinit (dvbsub_func_t *dvbsub) {
  (void)dvbsub;
}

static uint32_t _region_find (dvbsub_func_t *dvbsub, uint32_t id, int new) {
  uint32_t u;

  id &= 255;
  u = dvbsub->region_ids[id];
  if (u != 255)
    return u;
    
  if (new && (dvbsub->region_num < MAX_REGIONS)) {
    dvbsub->region_ids[id] = u = dvbsub->region_num++;
    return u;
  }

  return ~0u;
}

static void _clut_init (dvbsub_func_t *dvbsub) {
  dvbsub->clut_num = 0;
  memset (dvbsub->clut_ids, 255, sizeof (dvbsub->clut_ids));
}

static void _clut_deinit (dvbsub_func_t *dvbsub) {
  (void)dvbsub;
}

static clut_union_t *_clut_find (dvbsub_func_t *dvbsub, uint32_t id, int new) {
  uint32_t u;

  id &= 255;
  u = dvbsub->clut_ids[id];
  if (u != 255)
    return dvbsub->colours + u * 256;
    
  if (new && (dvbsub->clut_num < MAX_REGIONS)) {
    dvbsub->clut_ids[id] = u = dvbsub->clut_num++;
    return dvbsub->colours + u * 256;
  }

  /* nasty fallback - wrong colors are better than nothing. */
  return dvbsub->colours + (id & (MAX_REGIONS - 1)) * 256;
}

static void _clut_reset (dvbsub_func_t *dvbsub) {
  /** look at the output of
   *  $ gcc -DGEN_DEFAULT_CLUT -o ~/bin/default_clut spudvb_decoder.c
   *  $ default_clut
   *  $ rm ~/bin/default_clut
   */
  static const clut_union_t default_clut[256] = {
    [  0] = {.c = { 16, 128, 128, 255}},
    [  1] = {.c = { 81,  90, 240, 192}},
    [  2] = {.c = {145,  54,  34, 192}},
    [  3] = {.c = {210,  16, 146, 192}},
    [  4] = {.c = { 41, 240, 110, 192}},
    [  5] = {.c = {106, 202, 222, 192}},
    [  6] = {.c = {170, 166,  16, 192}},
    [  7] = {.c = {235, 128, 128, 192}},
    [  8] = {.c = { 16, 128, 128, 128}},
    [  9] = {.c = { 38, 115, 165, 128}},
    [ 10] = {.c = { 59, 103,  97, 128}},
    [ 11] = {.c = { 81,  91, 134, 128}},
    [ 12] = {.c = { 24, 165, 122, 128}},
    [ 13] = {.c = { 46, 153, 159, 128}},
    [ 14] = {.c = { 67, 141,  91, 128}},
    [ 15] = {.c = { 89, 128, 128, 128}},
    [ 16] = {.c = { 60, 103, 203,   0}},
    [ 17] = {.c = { 81,  90, 240,   0}},
    [ 18] = {.c = {103,  78, 171,   0}},
    [ 19] = {.c = {124,  65, 209,   0}},
    [ 20] = {.c = { 68, 140, 197,   0}},
    [ 21] = {.c = { 90, 128, 234,   0}},
    [ 22] = {.c = {111, 115, 165,   0}},
    [ 23] = {.c = {133, 103, 203,   0}},
    [ 24] = {.c = { 60, 103, 203, 128}},
    [ 25] = {.c = { 81,  90, 240, 128}},
    [ 26] = {.c = {103,  78, 171, 128}},
    [ 27] = {.c = {124,  65, 209, 128}},
    [ 28] = {.c = { 68, 140, 197, 128}},
    [ 29] = {.c = { 90, 128, 234, 128}},
    [ 30] = {.c = {111, 115, 165, 128}},
    [ 31] = {.c = {133, 103, 203, 128}},
    [ 32] = {.c = {102,  79,  65,   0}},
    [ 33] = {.c = {124,  66, 103,   0}},
    [ 34] = {.c = {145,  54,  34,   0}},
    [ 35] = {.c = {166,  41,  72,   0}},
    [ 36] = {.c = {110, 116,  59,   0}},
    [ 37] = {.c = {132, 103,  97,   0}},
    [ 38] = {.c = {153,  91,  28,   0}},
    [ 39] = {.c = {175,  79,  65,   0}},
    [ 40] = {.c = {102,  79,  65, 128}},
    [ 41] = {.c = {124,  66, 103, 128}},
    [ 42] = {.c = {145,  54,  34, 128}},
    [ 43] = {.c = {166,  41,  72, 128}},
    [ 44] = {.c = {110, 116,  59, 128}},
    [ 45] = {.c = {132, 103,  97, 128}},
    [ 46] = {.c = {153,  91,  28, 128}},
    [ 47] = {.c = {175,  79,  65, 128}},
    [ 48] = {.c = {145,  53, 140,   0}},
    [ 49] = {.c = {167,  41, 177,   0}},
    [ 50] = {.c = {188,  29, 109,   0}},
    [ 51] = {.c = {210,  16, 146,   0}},
    [ 52] = {.c = {154,  91, 134,   0}},
    [ 53] = {.c = {176,  78, 171,   0}},
    [ 54] = {.c = {197,  66, 103,   0}},
    [ 55] = {.c = {218,  53, 140,   0}},
    [ 56] = {.c = {145,  53, 140, 128}},
    [ 57] = {.c = {167,  41, 177, 128}},
    [ 58] = {.c = {188,  29, 109, 128}},
    [ 59] = {.c = {210,  16, 146, 128}},
    [ 60] = {.c = {154,  91, 134, 128}},
    [ 61] = {.c = {176,  78, 171, 128}},
    [ 62] = {.c = {197,  66, 103, 128}},
    [ 63] = {.c = {218,  53, 140, 128}},
    [ 64] = {.c = { 33, 203, 116,   0}},
    [ 65] = {.c = { 54, 190, 153,   0}},
    [ 66] = {.c = { 75, 178,  85,   0}},
    [ 67] = {.c = { 97, 165, 122,   0}},
    [ 68] = {.c = { 41, 240, 110,   0}},
    [ 69] = {.c = { 63, 227, 147,   0}},
    [ 70] = {.c = { 84, 215,  79,   0}},
    [ 71] = {.c = {106, 203, 116,   0}},
    [ 72] = {.c = { 33, 203, 116, 128}},
    [ 73] = {.c = { 54, 190, 153, 128}},
    [ 74] = {.c = { 75, 178,  85, 128}},
    [ 75] = {.c = { 97, 165, 122, 128}},
    [ 76] = {.c = { 41, 240, 110, 128}},
    [ 77] = {.c = { 63, 227, 147, 128}},
    [ 78] = {.c = { 84, 215,  79, 128}},
    [ 79] = {.c = {106, 203, 116, 128}},
    [ 80] = {.c = { 76, 177, 191,   0}},
    [ 81] = {.c = { 98, 165, 228,   0}},
    [ 82] = {.c = {119, 153, 159,   0}},
    [ 83] = {.c = {141, 140, 197,   0}},
    [ 84] = {.c = { 85, 215, 184,   0}},
    [ 85] = {.c = {106, 202, 222,   0}},
    [ 86] = {.c = {127, 190, 153,   0}},
    [ 87] = {.c = {149, 177, 191,   0}},
    [ 88] = {.c = { 76, 177, 191, 128}},
    [ 89] = {.c = { 98, 165, 228, 128}},
    [ 90] = {.c = {119, 153, 159, 128}},
    [ 91] = {.c = {141, 140, 197, 128}},
    [ 92] = {.c = { 85, 215, 184, 128}},
    [ 93] = {.c = {106, 202, 222, 128}},
    [ 94] = {.c = {127, 190, 153, 128}},
    [ 95] = {.c = {149, 177, 191, 128}},
    [ 96] = {.c = {118, 153,  53,   0}},
    [ 97] = {.c = {140, 141,  91,   0}},
    [ 98] = {.c = {161, 128,  22,   0}},
    [ 99] = {.c = {183, 116,  59,   0}},
    [100] = {.c = {127, 191,  47,   0}},
    [101] = {.c = {148, 178,  85,   0}},
    [102] = {.c = {170, 166,  16,   0}},
    [103] = {.c = {191, 153,  53,   0}},
    [104] = {.c = {118, 153,  53, 128}},
    [105] = {.c = {140, 141,  91, 128}},
    [106] = {.c = {161, 128,  22, 128}},
    [107] = {.c = {183, 116,  59, 128}},
    [108] = {.c = {127, 191,  47, 128}},
    [109] = {.c = {148, 178,  85, 128}},
    [110] = {.c = {170, 166,  16, 128}},
    [111] = {.c = {191, 153,  53, 128}},
    [112] = {.c = {162, 128, 128,   0}},
    [113] = {.c = {184, 115, 165,   0}},
    [114] = {.c = {205, 103,  97,   0}},
    [115] = {.c = {227,  91, 134,   0}},
    [116] = {.c = {170, 165, 122,   0}},
    [117] = {.c = {192, 153, 159,   0}},
    [118] = {.c = {213, 141,  91,   0}},
    [119] = {.c = {235, 128, 128,   0}},
    [120] = {.c = {162, 128, 128, 128}},
    [121] = {.c = {184, 115, 165, 128}},
    [122] = {.c = {205, 103,  97, 128}},
    [123] = {.c = {227,  91, 134, 128}},
    [124] = {.c = {170, 165, 122, 128}},
    [125] = {.c = {192, 153, 159, 128}},
    [126] = {.c = {213, 141,  91, 128}},
    [127] = {.c = {235, 128, 128, 128}},
    [128] = {.c = {125, 128, 128,   0}},
    [129] = {.c = {136, 122, 147,   0}},
    [130] = {.c = {147, 115, 112,   0}},
    [131] = {.c = {158, 109, 131,   0}},
    [132] = {.c = {129, 147, 125,   0}},
    [133] = {.c = {140, 141, 144,   0}},
    [134] = {.c = {151, 134, 109,   0}},
    [135] = {.c = {162, 128, 128,   0}},
    [136] = {.c = { 16, 128, 128,   0}},
    [137] = {.c = { 27, 122, 147,   0}},
    [138] = {.c = { 38, 115, 112,   0}},
    [139] = {.c = { 49, 109, 131,   0}},
    [140] = {.c = { 20, 147, 125,   0}},
    [141] = {.c = { 31, 141, 144,   0}},
    [142] = {.c = { 42, 134, 109,   0}},
    [143] = {.c = { 53, 128, 128,   0}},
    [144] = {.c = {147, 115, 165,   0}},
    [145] = {.c = {158, 109, 184,   0}},
    [146] = {.c = {169, 103, 150,   0}},
    [147] = {.c = {180,  97, 168,   0}},
    [148] = {.c = {151, 134, 162,   0}},
    [149] = {.c = {162, 128, 181,   0}},
    [150] = {.c = {173, 122, 146,   0}},
    [151] = {.c = {184, 115, 165,   0}},
    [152] = {.c = { 38, 115, 165,   0}},
    [153] = {.c = { 49, 109, 184,   0}},
    [154] = {.c = { 60, 103, 150,   0}},
    [155] = {.c = { 71,  97, 168,   0}},
    [156] = {.c = { 42, 134, 162,   0}},
    [157] = {.c = { 53, 128, 181,   0}},
    [158] = {.c = { 64, 122, 146,   0}},
    [159] = {.c = { 75, 115, 165,   0}},
    [160] = {.c = {168, 103,  97,   0}},
    [161] = {.c = {179,  97, 116,   0}},
    [162] = {.c = {190,  91,  81,   0}},
    [163] = {.c = {201,  84, 100,   0}},
    [164] = {.c = {172, 122,  94,   0}},
    [165] = {.c = {183, 116, 113,   0}},
    [166] = {.c = {194, 110,  78,   0}},
    [167] = {.c = {205, 103,  97,   0}},
    [168] = {.c = { 59, 103,  97,   0}},
    [169] = {.c = { 70,  97, 116,   0}},
    [170] = {.c = { 81,  91,  81,   0}},
    [171] = {.c = { 92,  84, 100,   0}},
    [172] = {.c = { 63, 122,  94,   0}},
    [173] = {.c = { 74, 116, 113,   0}},
    [174] = {.c = { 85, 110,  78,   0}},
    [175] = {.c = { 96, 103,  97,   0}},
    [176] = {.c = {190,  91, 134,   0}},
    [177] = {.c = {201,  84, 153,   0}},
    [178] = {.c = {211,  78, 118,   0}},
    [179] = {.c = {222,  72, 137,   0}},
    [180] = {.c = {194, 110, 131,   0}},
    [181] = {.c = {205, 103, 150,   0}},
    [182] = {.c = {216,  97, 115,   0}},
    [183] = {.c = {227,  91, 134,   0}},
    [184] = {.c = { 81,  91, 134,   0}},
    [185] = {.c = { 92,  84, 153,   0}},
    [186] = {.c = {102,  78, 118,   0}},
    [187] = {.c = {113,  72, 137,   0}},
    [188] = {.c = { 85, 110, 131,   0}},
    [189] = {.c = { 96, 103, 150,   0}},
    [190] = {.c = {107,  97, 115,   0}},
    [191] = {.c = {118,  91, 134,   0}},
    [192] = {.c = {133, 165, 122,   0}},
    [193] = {.c = {144, 159, 141,   0}},
    [194] = {.c = {155, 153, 106,   0}},
    [195] = {.c = {166, 146, 125,   0}},
    [196] = {.c = {138, 184, 119,   0}},
    [197] = {.c = {149, 178, 138,   0}},
    [198] = {.c = {159, 172, 103,   0}},
    [199] = {.c = {170, 165, 122,   0}},
    [200] = {.c = { 24, 165, 122,   0}},
    [201] = {.c = { 35, 159, 141,   0}},
    [202] = {.c = { 46, 153, 106,   0}},
    [203] = {.c = { 57, 146, 125,   0}},
    [204] = {.c = { 29, 184, 119,   0}},
    [205] = {.c = { 40, 178, 138,   0}},
    [206] = {.c = { 50, 172, 103,   0}},
    [207] = {.c = { 61, 165, 122,   0}},
    [208] = {.c = {155, 153, 159,   0}},
    [209] = {.c = {166, 146, 178,   0}},
    [210] = {.c = {177, 140, 143,   0}},
    [211] = {.c = {188, 134, 162,   0}},
    [212] = {.c = {159, 172, 156,   0}},
    [213] = {.c = {170, 165, 175,   0}},
    [214] = {.c = {181, 159, 140,   0}},
    [215] = {.c = {192, 153, 159,   0}},
    [216] = {.c = { 46, 153, 159,   0}},
    [217] = {.c = { 57, 146, 178,   0}},
    [218] = {.c = { 68, 140, 143,   0}},
    [219] = {.c = { 79, 134, 162,   0}},
    [220] = {.c = { 50, 172, 156,   0}},
    [221] = {.c = { 61, 165, 175,   0}},
    [222] = {.c = { 72, 159, 140,   0}},
    [223] = {.c = { 83, 153, 159,   0}},
    [224] = {.c = {176, 141,  91,   0}},
    [225] = {.c = {187, 134, 110,   0}},
    [226] = {.c = {198, 128,  75,   0}},
    [227] = {.c = {209, 122,  94,   0}},
    [228] = {.c = {180, 159,  88,   0}},
    [229] = {.c = {191, 153, 106,   0}},
    [230] = {.c = {202, 147,  72,   0}},
    [231] = {.c = {213, 141,  91,   0}},
    [232] = {.c = { 67, 141,  91,   0}},
    [233] = {.c = { 78, 134, 110,   0}},
    [234] = {.c = { 89, 128,  75,   0}},
    [235] = {.c = {100, 122,  94,   0}},
    [236] = {.c = { 71, 159,  88,   0}},
    [237] = {.c = { 82, 153, 106,   0}},
    [238] = {.c = { 93, 147,  72,   0}},
    [239] = {.c = {104, 141,  91,   0}},
    [240] = {.c = {198, 128, 128,   0}},
    [241] = {.c = {209, 122, 147,   0}},
    [242] = {.c = {220, 115, 112,   0}},
    [243] = {.c = {231, 109, 131,   0}},
    [244] = {.c = {202, 147, 125,   0}},
    [245] = {.c = {213, 141, 144,   0}},
    [246] = {.c = {224, 134, 109,   0}},
    [247] = {.c = {235, 128, 128,   0}},
    [248] = {.c = { 89, 128, 128,   0}},
    [249] = {.c = {100, 122, 147,   0}},
    [250] = {.c = {111, 115, 112,   0}},
    [251] = {.c = {122, 109, 131,   0}},
    [252] = {.c = { 93, 147, 125,   0}},
    [253] = {.c = {104, 141, 144,   0}},
    [254] = {.c = {115, 134, 109,   0}},
    [255] = {.c = {126, 128, 128,   0}},
  };
  static const struct _lut_group_s default_lg = {
    .lut24 = { 0x0,  0x7,  0x8,  0xf},
    .lut28 = {0x00, 0x77, 0x88, 0xff},
    .lut48 = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
              0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff}
  };
  unsigned int r;

  /* Reset the colour LUTs */
  for (r = 0; r < MAX_REGIONS; ++r) {
    memcpy (dvbsub->colours + r * 256, default_clut, sizeof (default_clut));
    dvbsub->clut_cm[r] = 10; /* SD, mpeg range */
    dvbsub->clut_gen[r] = 0;
  }

  /* Reset the colour index LUTs */
  for (r = 0; r < MAX_REGIONS; ++r)
    dvbsub->lut[r] = default_lg;

  _clut_init (dvbsub);
}

static void update_region (region_t *reg, uint32_t region_id,
  int32_t region_width, int32_t region_height, int32_t fill_color) {
  /* reject invalid sizes and set some limits ! */
  if (((uint32_t)(region_width - 1) >= SPU_MAX_WIDTH) ||
    ((uint32_t)(region_height - 1) >= SPU_MAX_HEIGHT)) {
    _x_freep (&reg->img);
    lprintf ("region %d invalid size %d x %d.\n", (int)region_id, (int)region_width, (int)region_height);
    return;
  }

  if (reg->width * reg->height < region_width * region_height) {
    lprintf ("region %d enlarged to %d x %d.\n", (int)region_id, (int)region_width, (int)region_height);
    _x_freep (&reg->img);
  }

  if (!reg->img) {
    reg->img = calloc (1, region_width * region_height);
    if (!reg->img) {
      lprintf ("region %d no memory.\n", (int)region_id);
      return;
    }
    reg->flags &= ~_REGION_FLAG_FILL;
    reg->img[0] = ~fill_color;
  }

  if ((fill_color >= 0) && (!(reg->flags & _REGION_FLAG_FILL) || (reg->img[0] != fill_color))) {
    memset (reg->img, fill_color, region_width * region_height);
    reg->flags |= _REGION_FLAG_FILL;
    lprintf ("region %d fill color %d.\n", region_id, fill_color);
  }
  reg->width = region_width;
  reg->height = region_height;
}

static void plot (dvbsub_func_t *dvbsub, int r, uint32_t run_length, uint8_t pixel) {
  region_t *reg = dvbsub->regions + r;
  uint32_t e = reg->width * reg->height;
  uint32_t i = dvbsub->y * reg->width + dvbsub->x;

  if (e > i + run_length)
    e = i + run_length;
  dvbsub->x += e - i;
  while (i < e)
    reg->img[i++] = pixel;
  reg->flags |= _REGION_FLAG_CHANGED;
  reg->flags &= ~_REGION_FLAG_FILL;
}

static const uint8_t *lookup_lut (const dvbsub_func_t *dvbsub, int r)
{
  static const uint8_t identity_lut[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };

  switch (dvbsub->compat_depth)
  {
  case 012: return dvbsub->lut[r].lut24;
  case 013: return dvbsub->lut[r].lut28;
  case 023: return dvbsub->lut[r].lut48;
  default:  return identity_lut;
  }
}

static unsigned char next_datum (dvbsub_func_t *dvbsub, int width)
{
  unsigned char x = 0;

  if (!dvbsub->i_bits)
    dvbsub->i_bits = 8;

  if (dvbsub->i_bits < width)
  {
    /* need to read from more than one byte; split it up */
    width -= dvbsub->i_bits;
    x = dvbsub->buf[dvbsub->i++] & ((1 << dvbsub->i_bits) - 1);
    dvbsub->i_bits = 8;
    return x << width | next_datum (dvbsub, width);
  }

  dvbsub->i_bits = (dvbsub->i_bits - width) & 7;
  x = (dvbsub->buf[dvbsub->i] >> dvbsub->i_bits) & ((1 << width) - 1);

  if (!dvbsub->i_bits)
    ++dvbsub->i;

  return x;
}

static void decode_2bit_pixel_code_string (dvbsub_func_t *dvbsub, int r, int n)
{
  int j;
  const uint8_t *lut = lookup_lut (dvbsub, r);

  dvbsub->i_bits = 0;
  j = dvbsub->i + n;

  while (dvbsub->i < j)
  {
    unsigned int next_bits = next_datum (dvbsub, 2);
    unsigned int run_length;

    if (next_bits)
    {
      /* single pixel */
      plot (dvbsub, r, 1, lut[next_bits]);
      continue;
    }

    /* switch 1 */
    if (next_datum (dvbsub, 1) == 0)
    {
      /* run length, 3 to 10 pixels, colour given */
      run_length = next_datum (dvbsub, 3);
      plot (dvbsub, r, run_length + 3, lut[next_datum (dvbsub, 2)]);
      continue;
    }

    /* switch 2 */
    if (next_datum (dvbsub, 1) == 1)
    {
      /* single pixel, colour 0 */
      plot (dvbsub, r, 1, lut[0]);
      continue;
    }

    /* switch 3 */
    switch (next_datum (dvbsub, 2))
    {
    case 0: /* end-of-string */
      j = dvbsub->i; /* set the while cause FALSE */
      break;
    case 1: /* two pixels, colour 0 */
      plot (dvbsub, r, 2, lut[0]);
      break;
    case 2: /* run length, 12 to 27 pixels (4-bit), colour given */
      run_length = next_datum (dvbsub, 4);
      plot (dvbsub, r, run_length + 12, lut[next_datum (dvbsub, 2)]);
      break;
    case 3: /* run length, 29 to 284 pixels (8-bit), colour given */
      run_length = next_datum (dvbsub, 8);
      plot (dvbsub, r, run_length + 29, lut[next_datum (dvbsub, 2)]);
    }
  }

  if (dvbsub->i_bits) {
    dvbsub->i++;
    dvbsub->i_bits = 0;
  }
}

static void decode_4bit_pixel_code_string (dvbsub_func_t *dvbsub, int r, int n)
{
  int j;
  const uint8_t *lut = lookup_lut (dvbsub, r);

  dvbsub->i_bits = 0;
  j = dvbsub->i + n;

  while (dvbsub->i < j)
  {
    unsigned int next_bits = next_datum (dvbsub, 4);
    unsigned int run_length;

    if (next_bits)
    {
      /* single pixel */
      plot (dvbsub, r, 1, lut[next_bits]);
      continue;
    }

    /* switch 1 */
    if (next_datum (dvbsub, 1) == 0)
    {
      run_length = next_datum (dvbsub, 3);
      if (!run_length)
        /* end-of-string */
        break;

      /* run length, 3 to 9 pixels, colour 0 */
      plot (dvbsub, r, run_length + 2, lut[0]);
      continue;
    }

    /* switch 2 */
    if (next_datum (dvbsub, 1) == 0)
    {
      /* run length, 4 to 7 pixels, colour given */
      run_length = next_datum (dvbsub, 2);
      plot (dvbsub, r, run_length + 4, lut[next_datum (dvbsub, 4)]);
      continue;
    }

    /* switch 3 */
    switch (next_datum (dvbsub, 2))
    {
    case 0: /* single pixel, colour 0 */
      plot (dvbsub, r, 1, lut[0]);
      break;
    case 1: /* two pixels, colour 0 */
      plot (dvbsub, r, 2, lut[0]);
      break;
    case 2: /* run length, 9 to 24 pixels (4-bit), colour given */
      run_length = next_datum (dvbsub, 4);
      plot (dvbsub, r, run_length + 9, lut[next_datum (dvbsub, 4)]);
      break;
    case 3: /* run length, 25 to 280 pixels (8-bit), colour given */
      run_length = next_datum (dvbsub, 8);
      plot (dvbsub, r, run_length + 25, lut[next_datum (dvbsub, 4)]);
    }
  }

  if (dvbsub->i_bits) {
    dvbsub->i++;
    dvbsub->i_bits = 0;
  }
}

static void decode_8bit_pixel_code_string (dvbsub_func_t *dvbsub, int r, int n)
{
  int j;

  j = dvbsub->i + n;

  while (dvbsub->i < j)
  {
    unsigned int next_bits = dvbsub->buf[dvbsub->i++];
    unsigned int run_length;

    if (next_bits)
    {
      /* single pixel */
      plot (dvbsub, r, 1, next_bits);
      continue;
    }

    /* switch 1 */
    run_length = dvbsub->buf[dvbsub->i] & 127;

    if (dvbsub->buf[dvbsub->i++] & 128)
    {
      /* run length, 3 to 127 pixels, colour given */
      if (run_length > 2)
        plot (dvbsub, r, run_length + 4, dvbsub->buf[dvbsub->i++]);
      continue;
    }

    if (!run_length)
      /* end-of-string */
      break;

    /* run length, 1 to 127 pixels, colour 0 */
    plot (dvbsub, r, run_length + 2, 0);
  }
}

static void process_alt_CLUT_segment (dvbsub_func_t *dvbsub) {
  const uint8_t *p, *e;
  clut_union_t *q;
  uint32_t /* page_id, CLUT_version_number, */ CLUT_id, flags, d10;
  int n = 0;

  p = dvbsub->buf + dvbsub->i;
  /* page_id = _X_BE_16 (p); */
  e = p + 4 + _X_BE_16 (p + 2);
  dvbsub->i = e - dvbsub->buf;

  CLUT_id = p[4];
  /* CLUT_version_number = (p[5] & 0xf0) >> 4; */
  flags = _X_BE_16 (p + 6);
  d10 = ((flags >> 9) & 7) == 1;
  p += 8;

  q = _clut_find (dvbsub, CLUT_id, 1);
  CLUT_id = (q - dvbsub->colours) >> 8;

  if (d10) {
    /* paranoia */
    const uint8_t *m = p + 256 * 5;

    if (e > m)
      e = m;

    while (p < e) {
      clut_union_t un;
      uint32_t v = _X_BE_32 (p + 1);

      un.c.y = p[0];
      un.c.cr = v >> 22;
      un.c.cb = v >> 12;
      un.c.foo = v >> 2;
      p += 5;

      if (un.u32 != q[0].u32) {
        q[0].u32 = un.u32;
        dvbsub->clut_cm[CLUT_id] = 2; /* HD, mpeg range */
        dvbsub->clut_gen[CLUT_id] = 0;
        n++;
      }
      q++;
    }
  } else {
    /* paranoia */
    const uint8_t *m = p + 256 * 4;

    if (e > m)
      e = m;

    while (p < e) {
      clut_union_t un;

      un.c.y  = p[0];
      un.c.cr = p[1];
      un.c.cb = p[2];
      un.c.foo = p[3];
      p += 4;

      if (un.u32 != q[0].u32) {
        q[0].u32 = un.u32;
        dvbsub->clut_cm[CLUT_id] = 2; /* HD, mpeg range */
        dvbsub->clut_gen[CLUT_id] = 0;
        n++;
      }
      q++;
    }
  }

  (void)n;
  lprintf ("alt_clut %d bits %d with %d new colors.\n", (int)CLUT_id, d10 ? 10 : 8, n);
}

static void process_CLUT_definition_segment(dvbsub_func_t *dvbsub) {
  const uint8_t *p, *e;
  clut_union_t *q;
  uint32_t /* page_id, CLUT_version_number, */ CLUT_id;
  int n = 0;

  p = dvbsub->buf + dvbsub->i;
  /* page_id = _X_BE_16 (p); */
  e = p + 4 + _X_BE_16 (p + 2);
  dvbsub->i = e - dvbsub->buf;
  p += 4;

  CLUT_id = p[0]; p++;
  /* CLUT_version_number = (p[0] & 0xf0) >> 4; */ p++;
  q = _clut_find (dvbsub, CLUT_id, 1);
  CLUT_id = (q - dvbsub->colours) >> 8;

  while (p < e) {
    clut_union_t un;
    uint32_t CLUT_entry_id = p[0]; p++;
    /* uint32_t CLUT_flag_2_bit = (p[0] >> 7) & 1; */
    /* uint32_t CLUT_flag_4_bit = (p[0] >> 6) & 1; */
    /* uint32_t CLUT_flag_8_bit = (p[0] >> 5) & 1; */
    uint32_t full_range_flag = p[0] & 1;
    p++;

    if (full_range_flag) {
      un.c.y = p[0];
      un.c.cr = p[1];
      un.c.cb = p[2];
      un.c.foo = p[3];
      p += 4;
    } else {
      /* expand the coarse values. make sure that y == 16 and cx == 128 stay as is. */
      uint32_t v = _X_BE_16 (p);
      un.c.y  =  ((v >> 8) & 0xfc) | (v >> 14);
      un.c.cr = (((v >> 2) & 0xf0) | ((v >> 6) & 0x0f)) - 0x08;
      un.c.cb = (((v << 2) & 0xf0) | ((v >> 2) & 0x0f)) - 0x08;
      un.c.foo =  (v & 3) * 0x55;
      p += 2;
    }
    if (un.u32 != q[CLUT_entry_id].u32) {
      q[CLUT_entry_id].u32 = un.u32;
      dvbsub->clut_cm[CLUT_id] = 10; /* SD, mpeg range */
      dvbsub->clut_gen[CLUT_id] = 0;
      n++;
    }
  }

  (void)n;
  lprintf ("clut %d with %d new colors.\n", (int)CLUT_id, n);
}

static void process_pixel_data_sub_block (dvbsub_func_t *dvbsub, int r, unsigned int pos, int ofs, int n) {
  const uint8_t *p = dvbsub->buf + dvbsub->i, *e = p + n;

  dvbsub->x = pos >> 16;
  dvbsub->y = (pos & 0xffff) + ofs;

  while (p < e) {
    uint32_t data_type = *p++;

    switch (data_type) {
      case 0:
        /* FIXME: 2017 spec does not mention a data type of 0x00.
         * it should be treated as unknown/reserved.
         * however, this skip/fall through code has been imported from
         * somewhere back in 2004. it seems to be some workaround for
         * an old buggy encoder, so lets keep it for now. */
        p++;
        /* fall through */
      case 0x10:
        dvbsub->i = p - dvbsub->buf;
        decode_2bit_pixel_code_string (dvbsub, r, n - 1);
        p = dvbsub->buf + dvbsub->i;
        break;
      case 0x11:
        dvbsub->i = p - dvbsub->buf;
        decode_4bit_pixel_code_string (dvbsub, r, n - 1);
        p = dvbsub->buf + dvbsub->i;
        break;
      case 0x12:
        dvbsub->i = p - dvbsub->buf;
        decode_8bit_pixel_code_string (dvbsub, r, n - 1);
        p = dvbsub->buf + dvbsub->i;
        break;
      case 0x20: /* 2-to-4bit colour index map */
        /* should this be implemented since we have an 8-bit overlay? */
        dvbsub->lut[r].lut24[0] = p[0] >> 4;
        dvbsub->lut[r].lut24[1] = p[0] & 0x0f;
        dvbsub->lut[r].lut24[2] = p[1] >> 4;
        dvbsub->lut[r].lut24[3] = p[1] & 0x0f;
        p += 2;
        break;
      case 0x21: /* 2-to-8bit colour index map */
        memcpy (dvbsub->lut[r].lut28, p, 4);
        p += 4;
        break;
      case 0x22:
        memcpy (dvbsub->lut[r].lut48, p, 16);
        p += 16;
        break;
      case 0xf0:
        dvbsub->x = pos >> 16;
        dvbsub->y += 2;
        break;
      default:
        /* FIXME: we have neither a data size nor sync bytes.
         * maybe we should just stop here like this?
         * p = e; */
        lprintf ("unimplemented data_type 0x%02x in pixel_data_sub_block\n", data_type);
    }
  }
  dvbsub->i = e - dvbsub->buf;
}

static void process_page_composition_segment (dvbsub_func_t *dvbsub) {
  int version;
  unsigned int r;
  const uint8_t *p = dvbsub->buf + dvbsub->i, *e;

  dvbsub->page.id = _X_BE_16 (p + 0);
  e = p + 4 + _X_BE_16 (p + 2);
  dvbsub->i = e - dvbsub->buf;

  dvbsub->page.time_out = p[4];

  version = p[5] >> 4;
  if (version == dvbsub->page.version)
    return;
  dvbsub->page.version = version;
  dvbsub->page.state = (p[5] >> 2) & 3;
  p += 6;

  for (r = 0; r < dvbsub->region_num; r++) { /* reset */
    dvbsub->regions[r].flags |= _REGION_FLAG_CHANGED;
    dvbsub->regions[r].flags &= ~_REGION_FLAG_SHOW;
  }

  while (p < e) {
    unsigned int region_id, region_index, region_x, region_y;
    region_id = p[0];
    region_index = _region_find (dvbsub, region_id, 1);
    /* p[1] reserved */
    region_x = _X_BE_16 (p + 2);
    region_y = _X_BE_16 (p + 4);
    p += 6;
    lprintf ("page %u region %u @ (%u, %u).\n", dvbsub->page.id, region_id, region_x, region_y);
    if (region_index == ~0u)
      continue;
    dvbsub->page.regions[region_index].x = region_x;
    dvbsub->page.regions[region_index].y = region_y;
    dvbsub->regions[region_index].flags |= _REGION_FLAG_CHANGED | _REGION_FLAG_SHOW;
  }
}


static void process_region_composition_segment (dvbsub_func_t *dvbsub) {
  unsigned int region_id, region_index, region_version_number, region_fill_flag,
    region_width, region_height, region_level_of_compatibility, region_depth, CLUT_id,
    /* region_8_bit_pixel_code, */ region_4_bit_pixel_code /*, region_2_bit_pixel_code*/;
  unsigned int object_id, object_type, /*object_provider_flag,*/
    object_x, object_y /*, foreground_pixel_code, background_pixel_code */;
  const uint8_t *p = dvbsub->buf + dvbsub->i, *e;

  dvbsub->page.id = _X_BE_16 (p + 0);
  e = p + 4 + _X_BE_16 (p + 2);
  dvbsub->i = e - dvbsub->buf;

  region_id = p[4];
  region_version_number = p[5] >> 4;
  region_fill_flag = (p[5] >> 3) & 1;
  region_width = _X_BE_16 (p + 6);
  region_height = _X_BE_16 (p + 8);
  region_level_of_compatibility = (p[10] >> 5) & 7;
  region_depth = (p[10] >> 2) & 7;
  dvbsub->compat_depth = (region_level_of_compatibility << 3) | region_depth;
  CLUT_id = p[11];
  /* region_8_bit_pixel_code = p[12]; */
  region_4_bit_pixel_code = p[13] >> 4;
  /*region_2_bit_pixel_code = (p[13] >> 2) & 3; */
  p += 14;
  lprintf ("page %d region %d compose %d x %d.\n", dvbsub->page.id, region_id, region_width, region_height);

  region_index = _region_find (dvbsub, region_id, 1);
  if (region_index == ~0u)
    return;
  if (dvbsub->regions[region_index].version == region_version_number)
    return;
  dvbsub->regions[region_index].version = region_version_number;

  dvbsub->regions[region_index].flags |= _REGION_FLAG_CHANGED;
  /* dvbsub->regions[region_index].flags &= ~_REGION_FLAG_SHOW; */
  /* Check if region size has changed and fill background. */
  update_region (&dvbsub->regions[region_index], region_id, region_width, region_height,
    region_fill_flag ? (int32_t)region_4_bit_pixel_code : -1);
  dvbsub->regions[region_index].CLUT_id = CLUT_id;

  sparse_array_unset (&dvbsub->object_pos, region_index, 0xff);

  while (p < e) {
    object_id = _X_BE_16 (p);
    object_type = p[2] >> 6;
    /* object_provider_flag = (p[2] >> 4) & 3; */
    object_x = _X_BE_16 (p + 2) & 0x0fff;
    object_y = _X_BE_16 (p + 4) & 0x0fff;
    p += 6;

    sparse_array_set (&dvbsub->object_pos, (object_id << 8) | region_index, (object_x << 16) | object_y);

    if ((object_type == 0x01) || (object_type == 0x02)) {
      /* foreground_pixel_code = p[0]; */
      /* background_pixel_code = p[1]; */
      p += 2;
    }
  }

}

static void process_object_data_segment (dvbsub_func_t *dvbsub) {
  unsigned int /* segment_length, */ object_id, /* object_version_number, */
    object_coding_method /*, non_modifying_colour_flag */;
  int top_field_data_block_length, bottom_field_data_block_length;
  sparse_array_entry_t *start, *stop;
  const uint8_t *p = dvbsub->buf + dvbsub->i, *e;

  dvbsub->page.id = _X_BE_16 (p);
  e = p + 4 + _X_BE_16 (p + 2);
  dvbsub->curr_obj = object_id = _X_BE_16 (p + 4);
  /* object_version_number = p[6] >> 4; */
  object_coding_method = (p[6] >> 2) & 3;
  /* non_modifying_colour_flag = (p[6] >> 1) & 1; */
  p += 7;

  start = sparse_array_get (&dvbsub->object_pos, object_id << 8);
  stop  = sparse_array_get (&dvbsub->object_pos, (object_id << 8) | dvbsub->region_num);

  for (; start < stop; start++) {
    uint32_t r = start->key & 255;
    uint32_t pos = start->value;
    /* If this object is in this region... */
    if (dvbsub->regions[r].img) {
      if (object_coding_method == 0) {
        top_field_data_block_length = _X_BE_16 (p);
        bottom_field_data_block_length = _X_BE_16 (p + 2);
        dvbsub->i = p + 4 - dvbsub->buf;
        process_pixel_data_sub_block (dvbsub, r, pos, 0, top_field_data_block_length);
        if (bottom_field_data_block_length == 0) {
          /* handle bottom field == top field */
          bottom_field_data_block_length = top_field_data_block_length;
          dvbsub->i = p + 4 - dvbsub->buf;
        }
        process_pixel_data_sub_block (dvbsub, r, pos, 1, bottom_field_data_block_length);
      }
    }
  }
  dvbsub->i = e - dvbsub->buf;
}

static void process_display_definition_segment(dvbsub_func_t *dvbsub)
{
  unsigned int version_number, segment_length;
  uint8_t *buf = &dvbsub->buf[dvbsub->i];

  /*page_id = _X_BE_16(buf); */
  segment_length = _X_BE_16(buf + 2);
  buf += 4;

  if (segment_length < 5)
    return;

  version_number = (buf[0] >> 4);

  /* Check version number */
  if (version_number == dvbsub->dds.version_number)
    return;

  dvbsub->dds.version_number = version_number;
  dvbsub->dds.windowed = (buf[0] & 0x08) >> 3;

  dvbsub->dds.width  = _X_BE_16(buf + 1) + 1;
  dvbsub->dds.height = _X_BE_16(buf + 3) + 1;

  lprintf("display_definition_segment: %ux%u, windowed=%u\n",
          dvbsub->dds.width, dvbsub->dds.height, dvbsub->dds.windowed);

  if (dvbsub->dds.windowed) {
#if 0
    if (segment_length < 13)
      return;
    /* TODO: window currently disabled (no samples) */
    dvbsub->dds.x0 = _X_BE_16(buf + 5);
    dvbsub->dds.x1 = _X_BE_16(buf + 7);
    dvbsub->dds.y0 = _X_BE_16(buf + 9);
    dvbsub->dds.y1 = _X_BE_16(buf + 11);

    lprintf("display_definition_segment: window (%u,%u)-(%u,%u)\n",
            dvbsub->dds.x0, dvbsub->dds.y0, dvbsub->dds.x1, dvbsub->dds.y1,
#endif
  }
}

static void _hide_overlays (dvb_spu_decoder_t *this) {
  unsigned int i;
  for (i = 0; i < this->dvbsub.region_num; i++) {
    if (this->dvbsub.regions[i].osd)
      this->stream->osd_renderer->hide (this->dvbsub.regions[i].osd, 0);
  }
}

static void update_osd (dvb_spu_decoder_t *this, region_t *reg) {
  if (!reg->img) {
    if (reg->osd) {
      this->stream->osd_renderer->free_object (reg->osd);
      reg->osd = NULL;
    }
    return;
  }
  if (reg->osd) {
    if ((reg->width != reg->osd->width) || (reg->height != reg->osd->height)) {
      this->stream->osd_renderer->free_object (reg->osd);
      reg->osd = NULL;
    }
  }
  if (!reg->osd)
    reg->osd = this->stream->osd_renderer->new_object (this->stream->osd_renderer, reg->width, reg->height);
}

static void downscale_region_image( region_t *reg, unsigned char *dest, int dest_width )
{
  float i, k, inc=reg->width/(float)dest_width;
  int j;
  for ( j=0; j<reg->height; j++ ) {
    for ( i=0,k=0; i<reg->width && k<dest_width; i+=inc,k++ ) {
      dest[(j*dest_width)+(int)k] = reg->img[(j*reg->width)+(int)i];
    }
  }
}

static void recalculate_trans (dvb_spu_decoder_t *this) {
  uint32_t u;
  int gen = this->dvbsub.clut_gen[0], *list;

  _x_spu_dvb_opacity (this->stream->xine, this->dvbsub.trans, &this->dvbsub.colours[0].c, &gen, 256);
  this->dvbsub.clut_gen[0] = gen;

  /* ARGH: using clut_gen directly yields a false gcc7 array range warning. */
  list = this->dvbsub.clut_gen;
  for (u = 1; u < MAX_REGIONS; ) {
    uint32_t v;

    list[MAX_REGIONS] = ~gen;
    while (list[u] == gen)
      u++;
    if (u >= MAX_REGIONS)
      break;
    v = u;
    list[MAX_REGIONS] = gen;
    while (list[u] != gen)
      u++;
    gen = list[v];
    _x_spu_dvb_opacity (this->stream->xine, this->dvbsub.trans + v * 256,
      &this->dvbsub.colours[v * 256].c, &gen, (u - v) * 256);
    while (v < u)
      list[v++] = gen;
  }
}

static void draw_subtitles (dvb_spu_decoder_t * this)
{
  unsigned int r;
  int display = 0, page_time_out;
  int64_t dum, hide_vpts_1, hide_vpts_2;
  int dest_width = 0, dest_height, max_x = 0, max_y = 0;

  this->stream->video_out->status (this->stream->video_out, NULL, &dest_width, &dest_height, &dum);
  if (!dest_width || !dest_height)
    return;

  /* render all regions onto the page */

  for (r = 0; r < this->dvbsub.region_num; r++) {
    if (this->dvbsub.regions[r].flags & _REGION_FLAG_SHOW) {
      /* additional safety for broken HD streams without DDS ... */
      int x2 = this->dvbsub.page.regions[r].x + this->dvbsub.regions[r].width;
      int y2 = this->dvbsub.page.regions[r].y + this->dvbsub.regions[r].height;
      max_x = (max_x < x2) ? x2 : max_x;
      max_y = (max_y < y2) ? y2 : max_y;
      display++;
    }
  }

  if (display) {
    uint32_t vo_caps = this->stream->video_out->get_capabilities (this->stream->video_out);

    for (r = 0; r < this->dvbsub.region_num; r++) {
      region_t *reg = &this->dvbsub.regions[r];

      if ((reg->flags & (_REGION_FLAG_FILL | _REGION_FLAG_SHOW)) == _REGION_FLAG_SHOW) {
        const uint8_t *img = reg->img;
        /* region contains something */
        if (img) {
          uint8_t *tmp = NULL;
          int img_width = reg->width;

          update_osd (this, reg);
          if (!reg->osd)
            continue;
          /* clear osd */
          this->stream->osd_renderer->clear (reg->osd);
          if ((reg->width > dest_width) && !(vo_caps & VO_CAP_CUSTOM_EXTENT_OVERLAY)) {
            lprintf ("region %d downscaling width %d -> %d.\n", (int)this->dvbsub.region_ids[r], reg->width, dest_width);
            tmp = malloc (dest_width * 576);
            if (tmp) {
              downscale_region_image (reg, tmp, dest_width);
              img = tmp;
              img_width = dest_width;
            }
          }
          {
            clut_union_t *c = _clut_find (&this->dvbsub, reg->CLUT_id, 0);
            uint32_t clut_offs = c - this->dvbsub.colours;
            clut_union_t save[4];

            memcpy (save, c, sizeof (save));
            _X_SET_CLUT_CM (&c->u32, this->dvbsub.clut_cm[clut_offs >> 8]);
            this->stream->osd_renderer->set_palette (reg->osd, &c->u32,
              this->dvbsub.trans + clut_offs);
            memcpy (c, save, sizeof (save));
            lprintf ("region %d draw %d x %d.\n", (int)this->dvbsub.region_ids[r], img_width, reg->height);
            this->stream->osd_renderer->draw_bitmap (reg->osd, img, 0, 0, img_width, reg->height, NULL);
          }
          free (tmp);
        }
      } else if (this->timeout
        && (reg->hide_vpts > this->vpts)
        && (reg->stream_hide_vpts - 3600 < this->vpts)) {
        reg->hide_vpts = 0;
        reg->flags |= _REGION_FLAG_VISIBLE;
        reg->flags &= ~_REGION_FLAG_SHOW;
      }
    }
  }

  /** f***ing complex timing rule:
   *  1. When user_timeout == 0, do exactly what the stream says.
   *     This often leads to _very_ hectic flicker. There will be gaps between
   *     parts of the same sentece, and short texts will disappear right when
   *     viewer starts reading them.
   *  2. When user_timeout > 0, use stream hide time or user timeout, whatever
   *     is later. When new regions appear, hide all earlier regions whose
   *     stream hide time has roughly expired. This will close gaps, and it
   *     will still allow stream intended overlaps. */

  page_time_out = this->dvbsub.page.time_out;
  lprintf ("vpts %" PRId64 ", page_time_out %d.\n", this->vpts, page_time_out);
  /* NOTE: i have seen only the value 65 here so far. */
  page_time_out *= 90000;
  hide_vpts_1 = this->vpts + (this->timeout ? this->longest_hold_vpts : page_time_out);
  hide_vpts_2 = this->vpts + this->timeout;

  for (r = 0; r < this->dvbsub.region_num; r++) {
    static const uint8_t _mode[16] = {
        /* _REGION_FLAG_SHOW && !_REGION_FLAG_FILL && (!_REGION_FLAG_VISIBLE || _REGION_FLAG_CHANGED) */
        [_REGION_FLAG_SHOW] = 1,
        [_REGION_FLAG_SHOW | _REGION_FLAG_CHANGED] = 1,
        [_REGION_FLAG_SHOW | _REGION_FLAG_CHANGED | _REGION_FLAG_VISIBLE] = 1,
        /* _REGION_FLAG_VISIBLE && !_REGION_FLAG_SHOW */
        [_REGION_FLAG_VISIBLE] = 2,
        [_REGION_FLAG_VISIBLE | _REGION_FLAG_CHANGED] = 2,
        [_REGION_FLAG_VISIBLE | _REGION_FLAG_FILL] = 2,
        [_REGION_FLAG_VISIBLE | _REGION_FLAG_CHANGED | _REGION_FLAG_FILL] = 2,
    };
    region_t *reg = &this->dvbsub.regions[r];
    uint32_t mode = _mode[reg->flags & 15];
    switch (mode) {
      case 1:
        reg->flags |= _REGION_FLAG_VISIBLE;
        reg->flags &= ~_REGION_FLAG_CHANGED;
        reg->show_vpts = this->vpts;
        reg->hide_vpts = hide_vpts_2;
        reg->stream_hide_vpts = 0;
        if (reg->osd) {
          if (max_x <= this->dvbsub.dds.width && max_y <= this->dvbsub.dds.height)
            this->stream->osd_renderer->set_extent (reg->osd, this->dvbsub.dds.width, this->dvbsub.dds.height);
          this->stream->osd_renderer->set_position (reg->osd, this->dvbsub.page.regions[r].x, this->dvbsub.page.regions[r].y);
          this->stream->osd_renderer->show (reg->osd, this->vpts);
          this->stream->osd_renderer->hide (reg->osd, hide_vpts_1);
        }
        xprintf (this->stream->xine, XINE_VERBOSITY_DEBUG,
          LOG_MODULE ": region %d%s%s%s show @ %" PRId64 " hide @ %" PRId64 ".\n",
          (int)this->dvbsub.region_ids[r],
          reg->osd ? " [osd]" : "",
          reg->flags & _REGION_FLAG_SHOW ? " [visible]" : "",
          reg->flags & _REGION_FLAG_FILL ? " [empty]" : "", this->vpts, hide_vpts_1);
        break;
      case 2:
        reg->flags &= ~_REGION_FLAG_VISIBLE;
        reg->stream_hide_vpts = this->vpts;
        {
          int d = reg->stream_hide_vpts - reg->show_vpts;
          if (d > this->longest_hold_vpts) {
            this->longest_hold_vpts = d;
            if (this->longest_hold_vpts > page_time_out)
              this->longest_hold_vpts = page_time_out;
          }
          if (reg->hide_vpts < this->vpts)
            reg->hide_vpts = this->vpts;
          if (reg->osd)
            this->stream->osd_renderer->hide (reg->osd, reg->hide_vpts);
          xprintf (this->stream->xine, XINE_VERBOSITY_DEBUG,
            LOG_MODULE ": region %d%s%s%s hide @ %" PRId64 ".\n", (int)this->dvbsub.region_ids[r],
            reg->osd ? " [osd]" : "",
            reg->flags & _REGION_FLAG_SHOW ? " [visible]" : "",
            reg->flags & _REGION_FLAG_FILL ? " [empty]" : "", reg->hide_vpts);
        }
        break;
      default: ;
        lprintf ("region %d%s%s%s.\n", (int)this->dvbsub.region_ids[r],
          reg->osd ? " [osd]" : "",
          reg->flags & _REGION_FLAG_SHOW ? " [visible]" : "",
          reg->flags & _REGION_FLAG_FILL ? " [empty]" : "");
    }
  }
}

/*
 *
 */

static void spudec_decode_data (spu_decoder_t * this_gen, buf_element_t * buf) {
  dvb_spu_decoder_t *this = (dvb_spu_decoder_t *) this_gen;
  /*int data_identifier, subtitle_stream_id;*/
  int new_i;

  if ((buf->type & 0xffff0000) != BUF_SPU_DVB)
    return;

  if (buf->decoder_flags & BUF_FLAG_SPECIAL) {
    if (buf->decoder_info[1] == BUF_SPECIAL_SPU_DVB_DESCRIPTOR) {
      if (buf->decoder_info[2] == 0) {
        /* Hide the osd - note that if the timeout thread times out, it'll rehide, which is harmless */
        _hide_overlays(this);
      } else {
        if (buf->decoder_info[2] < sizeof (this->spu_descriptor)) {
          xprintf (this->stream->xine, XINE_VERBOSITY_LOG,
            LOG_MODULE ": too small spu_descriptor, ignoring.\n");
        } else {
          memcpy (&this->spu_descriptor, buf->decoder_info_ptr[2], sizeof (this->spu_descriptor));
          xprintf (this->stream->xine, XINE_VERBOSITY_DEBUG,
            LOG_MODULE ": listening to page %d (%s).\n",
            (int)this->spu_descriptor.comp_page_id, this->spu_descriptor.lang);
        }
      }
    }
    return;
  }

  /* accumulate data */
  if (buf->decoder_info[2]) {

    this->pes_pkt_wrptr = this->pes_pkt;
    this->pes_pkt_size = buf->decoder_info[2];

    xine_fast_memcpy (this->pes_pkt, buf->content, buf->size);
    this->pes_pkt_wrptr += buf->size;
    memset (this->pes_pkt_wrptr, 0xff, sizeof(this->pes_pkt) - buf->size);

    this->vpts = 0;

    /* set DDS default (SD resolution) */
    this->dvbsub.dds.version_number = 0xff;
    this->dvbsub.dds.width = 720;
    this->dvbsub.dds.height = 576;
    this->dvbsub.dds.windowed = 0;
  }
  else {
    if (this->pes_pkt_wrptr != this->pes_pkt) {
      xine_fast_memcpy (this->pes_pkt_wrptr, buf->content, buf->size);
      this->pes_pkt_wrptr += buf->size;
    }
  }

  if (buf->pts > 0) {
    metronom_t *metronom = this->stream->metronom;
    this->vpts = metronom->got_spu_packet (metronom, buf->pts);
    lprintf ("pts %"PRId64" -> vpts %"PRId64".\n", buf->pts, this->vpts);
  }

  /* completely ignore pts since it makes a lot of problems with various providers */
  /* this->vpts = 0; */

  /* process the pes section */
  this->dvbsub.buf = this->pes_pkt;
  /* data_identifier = this->dvbsub->buf[0] */
  /* subtitle_stream_id = this->dvbsub->buf[1] */

  for (this->dvbsub.i = 2; this->dvbsub.i <= (int)this->pes_pkt_size; this->dvbsub.i = new_i) {
    const uint8_t *buf = this->dvbsub.buf + this->dvbsub.i;
    /* SUBTITLING SEGMENT */
    this->dvbsub.i += 2;
    this->dvbsub.page.id = _X_BE_16 (buf + 2);
    new_i = this->dvbsub.i + 4 + _X_BE_16 (buf + 4);

    /* only process complete segments */
    if (new_i > (this->pes_pkt_wrptr - this->pes_pkt))
      break;
    /* verify we've the right segment */
    if (this->dvbsub.page.id != this->spu_descriptor.comp_page_id) {
      if (this->spu_descriptor.comp_page_id || this->spu_descriptor.lang[0])
        continue;
      xprintf (this->stream->xine, XINE_VERBOSITY_LOG,
        LOG_MODULE ": warning: got page %d packet without initial descriptor.\n", (int)this->dvbsub.page.id);
      this->spu_descriptor.comp_page_id = this->dvbsub.page.id;
    }
    /* SEGMENT_DATA_FIELD */
    switch (buf[1]) {
      case 0x10:
        process_page_composition_segment (&this->dvbsub);
        break;
      case 0x11:
        process_region_composition_segment (&this->dvbsub);
        break;
      case 0x12:
        process_CLUT_definition_segment (&this->dvbsub);
        break;
      case 0x13:
        process_object_data_segment (&this->dvbsub);
        break;
      case 0x14:
        process_display_definition_segment (&this->dvbsub);
        break;
      case 0x16:
        process_alt_CLUT_segment (&this->dvbsub);
        break;
      case 0x80:
        /* Page is now completely rendered */
        recalculate_trans (this);
        draw_subtitles (this);
        break;
      case 0xFF:
        /* stuffing */
        break;
      default:
        xprintf (this->stream->xine, XINE_VERBOSITY_LOG,
          LOG_MODULE ": unknown segment type %d\n", (int)buf[1]);
    }
  }
}

static void spudec_reset (spu_decoder_t * this_gen)
{
  dvb_spu_decoder_t *this = (dvb_spu_decoder_t *) this_gen;
  unsigned int i;

  xprintf (this->stream->xine, XINE_VERBOSITY_DEBUG,
    LOG_MODULE ": reset.\n");

  /* Hide the osd - if the timeout thread times out, it'll rehide harmlessly */
  _hide_overlays(this);

  for (i = 0; i < MAX_REGIONS; i++)
    this->dvbsub.regions[i].version = 0x3f;
  this->dvbsub.page.version = 0x3f;
  /* _clut_reset (&this->dvbsub); */
  sparse_array_clear (&this->dvbsub.object_pos);

  this->pes_pkt_wrptr = this->pes_pkt;

  this->longest_hold_vpts = this->timeout;
}

static void spudec_discontinuity (spu_decoder_t * this_gen)
{
  /* do nothing */
  (void)this_gen;
}

static void spudec_dispose (spu_decoder_t * this_gen) {
  dvb_spu_decoder_t *this = (dvb_spu_decoder_t *) this_gen;
  unsigned int i;

  this->stream->xine->config->unregister_callbacks (this->stream->xine->config, NULL, NULL, this, sizeof (*this));

  for (i = 0; i < MAX_REGIONS; i++) {
    _x_freep (&this->dvbsub.regions[i].img);
    if (this->dvbsub.regions[i].osd)
      this->stream->osd_renderer->free_object (this->dvbsub.regions[i].osd);
  }
  sparse_array_delete (&this->dvbsub.object_pos);
  _region_deinit (&this->dvbsub);
  _clut_deinit (&this->dvbsub);
  free (this);
}

static void _spudvb_set_timeout (void *data, xine_cfg_entry_t *entry) {
  dvb_spu_decoder_t *this = (dvb_spu_decoder_t *)data;

  this->timeout = entry->num_value * 90000;
}

static spu_decoder_t *dvb_spu_class_open_plugin (spu_decoder_class_t * class_gen, xine_stream_t * stream)
{
  dvb_spu_decoder_t *this;
  unsigned int i;

  (void)class_gen;
  this = calloc(1, sizeof (dvb_spu_decoder_t));
  if (!this)
    return NULL;

  this->spu_decoder.decode_data = spudec_decode_data;
  this->spu_decoder.reset = spudec_reset;
  this->spu_decoder.discontinuity = spudec_discontinuity;
  this->spu_decoder.dispose = spudec_dispose;
  this->spu_decoder.get_interact_info = NULL;
  this->spu_decoder.set_button = NULL;

  this->stream = stream;

#ifndef HAVE_ZERO_SAFE_MEM
  for (i = 0; i < MAX_REGIONS; i++) {
    this->dvbsub.regions[i].img = NULL;
    this->dvbsub.regions[i].osd = NULL;
  }
#endif

  sparse_array_new (&this->dvbsub.object_pos);

  for (i = 0; i < MAX_REGIONS; i++) {
    this->dvbsub.regions[i].version = 0x3f;
  }
  this->dvbsub.page.version = 0x3f;

  _clut_reset (&this->dvbsub);
  _region_init (&this->dvbsub);

  /* since this already is registered inside class init, we just need
   * to add the callback here. */
  this->longest_hold_vpts =
  this->timeout = this->stream->xine->config->register_num (this->stream->xine->config,
    "subtitles.separate.timeout", 4,
    NULL, NULL, 20, _spudvb_set_timeout, this) * 90000;

  return &this->spu_decoder;
}

static void *init_spu_decoder_plugin (xine_t * xine, const void *data) {
  static const spu_decoder_class_t decode_dvb_spu_class = {
    .open_plugin = dvb_spu_class_open_plugin,
    .identifier  = "spudvb",
    .description = N_("DVB subtitle decoder plugin"),
    .dispose = NULL,
  };

  (void)data;

  /* registering inside class init and with data != NULL tells configfile.c
   * that there will be a change callback. this makes the entry always
   * visible (even with this plugin not loaded), and it prevents the wrong and
   * annoying "need a restart" message in the application. */
  xine->config->register_num (xine->config,
    "subtitles.separate.timeout", 4,
    _("default duration of subtitle display in seconds"),
    _("Some subtitle formats do not explicitly give a duration for each subtitle. "
      "For these, you can set a default duration here. Setting to zero will result "
      "in the subtitle being shown until the next one takes over."),
    20, NULL, (void *)1);

  return (void *)&decode_dvb_spu_class;
}


/* plugin catalog information */
static const uint32_t supported_types[] = { BUF_SPU_DVB, 0 };

static const decoder_info_t spudec_info = {
  .supported_types = supported_types,
  .priority        = 1,
};

const plugin_info_t xine_plugin_info[] EXPORTED = {
/* type, API, "name", version, special_info, init_function */
  {PLUGIN_SPU_DECODER, 17, "spudvb", XINE_VERSION_CODE, &spudec_info,
   &init_spu_decoder_plugin},
  {PLUGIN_NONE, 0, NULL, 0, NULL, NULL}
};

#endif /* GEN_DEFAULT_CLUT */
