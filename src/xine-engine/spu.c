/*
 * Copyright (C) 2007-2022 the xine project
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
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <xine/configfile.h>
#include <xine/xine_internal.h>
#include <xine/spu.h>
#include <xine.h>
#include "xine_private.h"

#define BLACK_OPACITY   67
#define COLOUR_OPACITY 100

static void _spu_dvbsub_make_tab (xine_private_t *xine) {
  uint32_t u, black = xine->dvbsub.black, color = xine->dvbsub.color, _b, _c;

  if (black > 100)
    xine->dvbsub.black = black = BLACK_OPACITY;
  if (color > 100)
    xine->dvbsub.color = color = COLOUR_OPACITY;
  black = (15u * (1u << 20) * black + 12750u) / 25500u;
  color = (15u * (1u << 20) * color + 12750u) / 25500u;

  for (_b = 255 * black, _c = 255 * color, u = 0; u < 256 * 2; _b -= black, _c -= color, u += 2) {
    xine->dvbsub.tab[u] = (_b + (1 << 19)) >> 20;
    xine->dvbsub.tab[u + 1] = (_c + (1 << 19)) >> 20;
  }
  xine->dvbsub.gen++;
}

static void _spu_dvbsub_set_black (void *data, xine_cfg_entry_t *entry) {
  xine_private_t *xine = (xine_private_t *)data;

  xine->dvbsub.black = entry->num_value;
  _spu_dvbsub_make_tab (xine);
}

static void _spu_dvbsub_set_color (void *data, xine_cfg_entry_t *entry) {
  xine_private_t *xine = (xine_private_t *)data;

  xine->dvbsub.color = entry->num_value;
  _spu_dvbsub_make_tab (xine);
}

void _x_spu_misc_init (xine_t *this) {
  xine_private_t *xine = (xine_private_t *)this;

  if (xine) {
    xine->dvbsub.black = xine->x.config->register_range (xine->x.config,
      "subtitles.bitmap.black_opacity", BLACK_OPACITY, 0, 100,
      _("opacity for the black parts of bitmapped subtitles"),
      NULL, 10, _spu_dvbsub_set_black, xine);
    xine->dvbsub.color = xine->x.config->register_range (xine->x.config,
      "subtitles.bitmap.colour_opacity", COLOUR_OPACITY, 0, 100,
      _("opacity for the colour parts of bitmapped subtitles"),
      NULL, 10, _spu_dvbsub_set_color, xine);
    xine->dvbsub.gen = 0;
    _spu_dvbsub_make_tab (xine);
  }
}

void _x_spu_get_opacity (xine_t *this, xine_spu_opacity_t *opacity) {
  xine_private_t *xine = (xine_private_t *)this;

  if (xine && opacity) {
    opacity->black = xine->dvbsub.black;
    opacity->colour = xine->dvbsub.color;
  }
}

int _x_spu_calculate_opacity (const clut_t *clut, uint8_t trans, const xine_spu_opacity_t *opacity)
{
  int value = (clut->y == 0 || (clut->y == 16 && clut->cb == 128 && clut->cr == 128))
	      ? opacity->black
	      : opacity->colour;
  return value * (255 - trans) / 100;
}

typedef union {
  clut_t c;
  uint32_t w;
} clut_union_t;

void _x_spu_dvb_opacity (xine_t *this, uint8_t *opacity, const clut_t *clut, int *gen, uint32_t n) {
  xine_private_t *xine = (xine_private_t *)this;
  const union {uint32_t v; uint8_t b;} endian = {1};
  const clut_union_t *dummy = (const clut_union_t *)0,
                      mask_y_cr_cb = {.c = {.y = 255, .cr = 255, .cb = 255}},
                      black        = {.c = {.y =  16, .cr = 128, .cb = 128}};
  const uint32_t shift_y = endian.b
                         ? 24 - ((const uint8_t *)&dummy->c.y - (const uint8_t *)dummy) * 8
                         :      ((const uint8_t *)&dummy->c.y - (const uint8_t *)dummy) * 8;
  int _gen = 0;
  uint32_t u;

  if (!xine || !opacity || !clut)
    return;
  if (!gen)
    gen = &_gen;
  if (*gen == xine->dvbsub.gen)
    return;
  *gen = xine->dvbsub.gen;

  for (u = 0; u < n; u++) {
    clut_union_t v;
    uint32_t mask;

    v.c = clut[u];
    /* ETSI-300-743 says "full transparency if Y == 0".
     * This is used to cut off currently unused parts of a region. */
    /* mask = v.c.y ? ~0u : 0u; */
    mask = (int32_t)(((v.w << shift_y >> 1) | 0x80000000) - 0x00800000) >> 31;
    opacity[u] = xine->dvbsub.tab[2 * v.c.foo + ((v.w & mask_y_cr_cb.w) != black.w)] & mask;
  }
}
