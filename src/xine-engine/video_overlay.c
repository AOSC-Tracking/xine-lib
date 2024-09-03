/*
 * Copyright (C) 2000-2023 the xine project
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
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define LOG_MODULE "video_overlay"
/*
#define LOG_DEBUG
*/

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>

#include <xine/buffer.h>
#include <xine/xine_internal.h>
#include <xine/sorted_array.h>
#include <xine/xineutils.h>
#include <xine/video_overlay.h>

#include "bswap.h"

#ifndef INT64_MAX
#  define INT64_MAX (int64_t)(((uint64_t)1 << (8 * sizeof (uint64_t) - 1)) - 1)
#endif

typedef struct {
  int8_t next, prev;
} _video_overlay_node_t;

typedef enum {
  _VOVL_FREE_FIRST = 0,
  _VOVL_FREE_LAST,
  _VOVL_USED_FIRST,
  _VOVL_USED_LAST,
  _VOVL_LAST
} _video_overlay_node_index_t;

static void _video_overlay_list_init (_video_overlay_node_t *a, uint32_t n) {
  uint32_t u;

  a[_VOVL_FREE_FIRST].next = _VOVL_LAST;
  a[_VOVL_FREE_FIRST].prev = -1;
  a[_VOVL_FREE_LAST].next = -1;
  a[_VOVL_FREE_LAST].prev = _VOVL_LAST + n - 1;
  for (u = 0; u < n; u++)
    a[_VOVL_LAST + u].next = _VOVL_LAST + u + 1, a[_VOVL_LAST + u].prev = _VOVL_LAST + u - 1;
  a[_VOVL_LAST].prev = _VOVL_FREE_FIRST;
  a[_VOVL_LAST + u - 1].next = _VOVL_FREE_LAST;

  a[_VOVL_USED_FIRST].next = _VOVL_USED_LAST;
  a[_VOVL_USED_FIRST].prev = -1;
  a[_VOVL_USED_LAST].next = -1;
  a[_VOVL_USED_LAST].prev = _VOVL_USED_FIRST;
}

static void _video_overlay_node_remove (_video_overlay_node_t *a, uint32_t indx) {
  uint32_t next = a[_VOVL_LAST + indx].next;
  uint32_t prev = a[_VOVL_LAST + indx].prev;
  a[prev].next = next;
  a[next].prev = prev;
  a[_VOVL_LAST + indx].next = -1;
  a[_VOVL_LAST + indx].prev = -1;
}

static void _video_overlay_node_append (_video_overlay_node_t *a, uint32_t used, uint32_t indx) {
  uint32_t next, prev;

  if (used) {
    next = _VOVL_USED_LAST;
    prev = a[_VOVL_USED_LAST].prev;
  } else {
    next = _VOVL_FREE_LAST;
    prev = a[_VOVL_FREE_LAST].prev;
  }
  a[_VOVL_LAST + indx].next = next;
  a[_VOVL_LAST + indx].prev = prev;
  a[prev].next =
  a[next].prev = _VOVL_LAST + indx;
}

static const uint32_t _vovl_bits[32] = {
  0x00000001, 0x00000002, 0x00000004, 0x00000008,
  0x00000010, 0x00000020, 0x00000040, 0x00000080,
  0x00000100, 0x00000200, 0x00000400, 0x00000800,
  0x00001000, 0x00002000, 0x00004000, 0x00008000,
  0x00010000, 0x00020000, 0x00040000, 0x00080000,
  0x00100000, 0x00200000, 0x00400000, 0x00800000,
  0x01000000, 0x02000000, 0x04000000, 0x08000000,
  0x10000000, 0x20000000, 0x40000000, 0x80000000
};

static uint32_t _vovl_bits_test (const uint32_t *field, uint32_t bit) {
  return field[bit >> 5] & _vovl_bits[bit & 31];
};

static uint32_t _vovl_bits_set (uint32_t *field, uint32_t bit) {
  uint32_t r = field[bit >> 5] & _vovl_bits[bit & 31];
  field[bit >> 5] |= _vovl_bits[bit & 31];
  return r;
};
/*
static void _vovl_bits_clear (uint32_t *field, uint32_t bit) {
  field[bit >> 5] &= ~_vovl_bits[bit & 31];
};
*/
typedef struct video_overlay_s {
  video_overlay_manager_t   video_overlay;

  xine_t                   *xine;

  int64_t                   last_vpts;

  struct {
    video_overlay_event_t   buf[MAX_EVENTS];
    uint8_t                 last_hide[MAX_OBJECTS];
    pthread_mutex_t         mutex_wait;
    xine_sarray_t          *wait;
    int64_t                 first_vpts;
    pthread_mutex_t         mutex_free;
#define _VOVL_EVENT_INDEX_FIRST MAX_EVENTS
#define _VOVL_EVENT_INDEX_LAST (MAX_EVENTS + 2)
    uint8_t                 list_free[MAX_EVENTS + 3];
  }                         event;

  struct {
    video_overlay_object_t  buf[MAX_OBJECTS];
    _video_overlay_node_t   indx_f[MAX_OBJECTS + _VOVL_LAST];
  }                         objects;

  struct {
    pthread_mutex_t         mutex;
    int8_t                  indx_r[MAX_OBJECTS];
    _video_overlay_node_t   indx_f[MAX_SHOWING + _VOVL_LAST];
    int8_t                  handle[MAX_SHOWING];
    int                     have, changed;
  }                         showing;
} video_overlay_t;

static void _vovl_event_free_reset (video_overlay_t *this) {
  uint32_t u;
  for (u = 0; u < MAX_EVENTS - 1; u++)
    this->event.list_free[u] = u + 1;
  this->event.list_free[MAX_EVENTS - 1] = _VOVL_EVENT_INDEX_FIRST;
  this->event.list_free[_VOVL_EVENT_INDEX_FIRST] = 0;
  this->event.list_free[_VOVL_EVENT_INDEX_LAST] = MAX_EVENTS - 1;
}

static uint32_t _vovl_event_free_get (video_overlay_t *this) {
  uint32_t u, v;
  u = this->event.list_free[_VOVL_EVENT_INDEX_FIRST];
  /* yes this is empty safe :-)
   * if (u < MAX_EVENTS) */
  v = this->event.list_free[_VOVL_EVENT_INDEX_FIRST] = this->event.list_free[u];
  v = (v + 0x1000 - MAX_EVENTS) >> 12;
  this->event.list_free[_VOVL_EVENT_INDEX_LAST - 1 + v] = _VOVL_EVENT_INDEX_FIRST;
  return u;
}

static void _vovl_event_free_put (video_overlay_t *this, uint32_t u) {
  this->event.list_free[this->event.list_free[_VOVL_EVENT_INDEX_LAST]] = u;
  this->event.list_free[u] = _VOVL_EVENT_INDEX_FIRST;
  this->event.list_free[_VOVL_EVENT_INDEX_LAST] = u;
}

static void add_showing_handle (video_overlay_t *this, int32_t handle, int changed) {
  /* already showing? */
  if (this->showing.indx_r[handle] < 0) {
    uint32_t n = this->showing.indx_f[_VOVL_FREE_FIRST].next;
    if (this->showing.indx_f[n].next >= 0) {
      n -= _VOVL_LAST;
      _video_overlay_node_remove (this->showing.indx_f, n);
      this->showing.handle[n] = handle;
      this->showing.indx_r[handle] = n;
      _video_overlay_node_append (this->showing.indx_f, 1, n);
      changed |= 1;
    } else {
      changed = 0;
      xprintf (this->xine, XINE_VERBOSITY_DEBUG,
        LOG_MODULE ": (%d) not enough showing slots.\n", (int)handle);
    }
  }
  this->showing.changed += changed;
}

static void remove_showing_handle (video_overlay_t *this, int32_t handle) {
  if (this->showing.indx_r[handle] >= 0) {
    uint32_t n = this->showing.indx_r[handle];

    _video_overlay_node_remove (this->showing.indx_f, n);
    this->showing.handle[n] = -1;
    this->showing.indx_r[handle] = -1;
    _video_overlay_node_append (this->showing.indx_f, 0, n);
    this->showing.changed++;
  }
}

/*
  allocate a handle from the object pool (exported function)
 */
static int32_t video_overlay_get_handle(video_overlay_manager_t *this_gen, int object_type ) {
  video_overlay_t *this = (video_overlay_t *) this_gen;
  int32_t n;

  pthread_mutex_lock (&this->event.mutex_free);
  n = this->objects.indx_f[_VOVL_FREE_FIRST].next;
  if (this->objects.indx_f[n].next >= 0) {
    n -= _VOVL_LAST;
    _video_overlay_node_remove (this->objects.indx_f, n);
    this->objects.buf[n].handle = n;
    this->objects.buf[n].object_type = object_type;
    _video_overlay_node_append (this->objects.indx_f, 1, n);
  } else {
    n = -1;
  }
  pthread_mutex_unlock (&this->event.mutex_free);
  return n;
}

/*
  free a handle from the object pool (internal function)
 */
static void _video_overlay_free_handle (video_overlay_t *this, int32_t handle) {
  _video_overlay_node_remove (this->objects.indx_f, handle);
  if (this->objects.buf[handle].overlay) {
    set_argb_layer_ptr (&this->objects.buf[handle].overlay->argb_layer, NULL);
    _x_freep (&this->objects.buf[handle].overlay->rle);
    _x_freep (&this->objects.buf[handle].overlay);
  }
  this->objects.buf[handle].handle = -1;
  _video_overlay_node_append (this->objects.indx_f, 0, handle);
}

/*
   exported free handle function. must take care of removing the object
   from showing and events lists.
*/
static void video_overlay_free_handle (video_overlay_manager_t *this_gen, int32_t handle) {
  video_overlay_t *this = (video_overlay_t *) this_gen;
  uint8_t h1[MAX_EVENTS];
  uint32_t n1;

  if ((handle < 0) || (handle >= MAX_OBJECTS))
    return;

  /* paranoia... */
  pthread_mutex_lock (&this->showing.mutex);
  remove_showing_handle (this,handle);
  pthread_mutex_unlock (&this->showing.mutex);

  n1 = 0;
  pthread_mutex_lock (&this->event.mutex_wait);
  {
    uint32_t u;
    video_overlay_event_t *event;

    for (u = 0; (event = xine_sarray_get (this->event.wait, u)) != NULL; ) {
      if (event->object.handle == handle) {
        xine_sarray_remove (this->event.wait, u);
        if (event->object.overlay) {
          _x_freep (&event->object.overlay->rle);
          _x_freep (&event->object.overlay);
        }
        event->event_type = OVERLAY_EVENT_NULL;
        h1[n1++] = event - this->event.buf;
      } else {
        u++;
      }
    }
  }
  this->event.last_hide[handle] = 255;
  pthread_mutex_unlock (&this->event.mutex_wait);

  pthread_mutex_lock (&this->event.mutex_free);
  {
    uint32_t u;
    for (u = 0; u < n1; u++)
      _vovl_event_free_put (this, h1[u]);
  }
  _video_overlay_free_handle (this, handle);
  pthread_mutex_unlock (&this->event.mutex_free);
}

static void video_overlay_init (video_overlay_manager_t *this_gen) {
  video_overlay_t *this = (video_overlay_t *) this_gen;
  int i;

  pthread_mutex_lock (&this->showing.mutex);
  for (i = 0; i < MAX_SHOWING; i++)
    this->showing.handle[i] = -1;
  this->showing.changed = 0;
  pthread_mutex_unlock (&this->showing.mutex);

  pthread_mutex_lock (&this->event.mutex_wait);
  xine_sarray_clear (this->event.wait);
  for (i = 0; i < MAX_EVENTS; i++) {
    this->event.buf[i].event_type = 0;
    this->event.buf[i].object.overlay = NULL;
    this->event.buf[i].object.palette = NULL;
  }
  pthread_mutex_unlock (&this->event.mutex_wait);

  pthread_mutex_lock (&this->event.mutex_free);
  for (i=0; i < MAX_OBJECTS; i++)
    _video_overlay_free_handle (this, i);
  _vovl_event_free_reset (this);
  pthread_mutex_unlock (&this->event.mutex_free);
}

static void _video_overlay_clip_trans (uint8_t *tab) {
  /* keep compiler happy. tab will always be 4 aligned, see xine/video_out.h:vo_pverlay_s. */
  uint32_t *w = (uint32_t *)(tab - ((uintptr_t)tab & 3)), u;

  for (u = 0; u < OVL_PALETTE_SIZE / 4; u++) {
    uint32_t v = *w, h = v;
    /* byte = (byte <= 0x0f) ? byte : 0x0f; */
    h = ((h >> 1) | 0x80808080) - 0x08080808;
    v |= 0x10101010 - ((h >> 7) & 0x01010101);
    v &= 0x0f0f0f0f;
    *w++ = v;
  }
}

/* add an event to the events queue, sort the queue based on vpts.
 * This can be the API entry point for DVD subtitles.
 * One calls this function with an event, the event contains an overlay
 * and a vpts when to action/process it. vpts of 0 means action the event now.
 * One also has a handle, so one can match show and hide events.
 *
 * note: on success event->object.overlay is "taken" (caller will not have access
 *       to overlay data including rle).
 * note2: handle will not be freed on HIDE events
 *        the handle is removed from the currently showing list.
 */
static int32_t video_overlay_add_event (video_overlay_manager_t *this_gen, void *event_gen) {
  video_overlay_t *this = (video_overlay_t *)this_gen;
  video_overlay_event_t *event = (video_overlay_event_t *)event_gen;
  vo_overlay_t *new_overlay;
  uint32_t u;

  if (!this || !event)
    return -1;
  u = event->object.handle;
  do {
    if ((u < MAX_OBJECTS) && ((uint32_t)this->objects.buf[u].handle == u))
      break;
    xprintf (this->xine, XINE_VERBOSITY_DEBUG, LOG_MODULE ": add_event: invalid handle %d.\n", (int)event->object.handle);
    return -1;
  } while (0);

  /* FIXME: find the stream this manager belongs to, and then ask its metronom? */
  if (event->vpts <= 0)
    event->vpts = this->last_vpts;

  if (event->object.overlay) {
    new_overlay = malloc (sizeof (*new_overlay));
    if (new_overlay) {
      memcpy (new_overlay, event->object.overlay, sizeof (*new_overlay));
      _video_overlay_clip_trans (&new_overlay->trans[0]);
      _video_overlay_clip_trans (&new_overlay->hili_trans[0]);
      /* We took the callers rle and data, therefore it will be our job to free it.
       * clear callers overlay so it will not be freed twice. */
      memset (event->object.overlay, 0, sizeof (*event->object.overlay));
    }
  } else {
    new_overlay = NULL;
  }

  pthread_mutex_lock (&this->event.mutex_free);
  u = _vovl_event_free_get (this);
  pthread_mutex_unlock (&this->event.mutex_free);

  if (u < MAX_EVENTS) {
    video_overlay_event_t *new_event = this->event.buf + u;
    /* the smart "update hide time of same handle" feature:
     * - HIDE again just updates time.
     * - SHOW revokes pending HIDE with same or later time.
     * thus,
     * SHOW(5) HIDE(9) SHOW(7) HIDE(100) HIDE(11)
     * will yield
     * SHOW(5) [HIDE and] SHOW(7) HIDE(11). */
    uint32_t free_event = 255;

    /* memcpy everything except the actual image */
    new_event->event_type = event->event_type;
    new_event->vpts = event->vpts;
    new_event->object.handle = event->object.handle;
    new_event->object.pts = event->object.pts;

    if (new_event->object.overlay) {
      xprintf (this->xine, XINE_VERBOSITY_DEBUG,
        LOG_MODULE ": (%d) add_event: event->object.overlay was not freed!\n", (int)new_event->object.handle);
    }
    new_event->object.overlay = new_overlay;

    pthread_mutex_lock (&this->event.mutex_wait);
    /* optimize */
    if (event->vpts < this->event.first_vpts)
        this->event.first_vpts = event->vpts;
    if (new_event->event_type == OVERLAY_EVENT_HIDE) {
      free_event = this->event.last_hide[new_event->object.handle];
      this->event.last_hide[new_event->object.handle] = u;
    } else if (new_event->event_type == OVERLAY_EVENT_SHOW) {
      free_event = this->event.last_hide[new_event->object.handle];
      this->event.last_hide[new_event->object.handle] = 255;
      if ((free_event != 255) && (this->event.buf[free_event].vpts < new_event->vpts))
        free_event = 255;
    }
    if (free_event != 255)
      xine_sarray_remove_ptr (this->event.wait, this->event.buf + free_event);
    xine_sarray_add (this->event.wait, new_event);
    pthread_mutex_unlock (&this->event.mutex_wait);

    if (free_event != 255) {
      new_event = this->event.buf + free_event;
      if (new_event->object.overlay) {
        set_argb_layer_ptr (&new_event->object.overlay->argb_layer, NULL);
        _x_freep (&new_event->object.overlay->rle);
        _x_freep (&new_event->object.overlay);
      }
      pthread_mutex_lock (&this->event.mutex_free);
      _vovl_event_free_put (this, free_event);
      pthread_mutex_unlock (&this->event.mutex_free);
    }

    return u;

  } else {

    free (new_overlay);
    xprintf (this->xine, XINE_VERBOSITY_DEBUG,
        LOG_MODULE ": (%d) add_event: not enough event slots.\n", (int)event->object.handle);
    return -1;

  }
}

/* not currently used. James might need this for debugging menu stuff */
#ifdef LOG_DEBUG
static void video_overlay_print_overlay( vo_overlay_t *ovl ) {
  printf (LOG_MODULE ": OVERLAY to show\n");
  printf (LOG_MODULE ": \tx = %d y = %d width = %d height = %d\n",
	  ovl->x, ovl->y, ovl->width, ovl->height );
  printf (LOG_MODULE ": \tclut [%x %x %x %x]\n",
	  ovl->color[0], ovl->color[1], ovl->color[2], ovl->color[3]);
  printf (LOG_MODULE ": \ttrans [%d %d %d %d]\n",
	  ovl->trans[0], ovl->trans[1], ovl->trans[2], ovl->trans[3]);
  printf (LOG_MODULE ": \tclip top=%d bottom=%d left=%d right=%d\n",
	  ovl->hili_top, ovl->hili_bottom, ovl->hili_left, ovl->hili_right);
  printf (LOG_MODULE ": \tclip_clut [%x %x %x %x]\n",
	  ovl->hili_color[0], ovl->hili_color[1], ovl->hili_color[2], ovl->hili_color[3]);
  printf (LOG_MODULE ": \thili_trans [%d %d %d %d]\n",
	  ovl->hili_trans[0], ovl->hili_trans[1], ovl->hili_trans[2], ovl->hili_trans[3]);
  return;
}
#endif

/*
   process overlay events
   if vpts == 0 will process everything now (used in flush)
   return true if something has been processed
*/
static int video_overlay_event (video_overlay_t *this, int64_t vpts) {
  uint8_t h1[MAX_EVENTS], h2[MAX_OBJECTS];
  uint32_t n1, ndone, nremove = 0, refs[(MAX_OBJECTS + 31) >> 5] = {[0] = 0};

  if (vpts <= 0)
    vpts = INT64_MAX;
  else
    this->last_vpts = vpts;

  pthread_mutex_lock (&this->event.mutex_wait);
  /* the way most frequent case: no changes for now. */
  if (vpts < this->event.first_vpts) {
    pthread_mutex_unlock (&this->event.mutex_wait);
    return 0;
  }

  /* take out relevant events. */
  {
    int i;
    video_overlay_event_t *event, e;
    e.vpts = vpts;
    i = xine_sarray_binary_search (this->event.wait, &e);
    if (i < 0)
      ndone = ~i;
    else
      ndone = i + 1;
    event = xine_sarray_get (this->event.wait, ndone);
    this->event.first_vpts = event ? event->vpts : INT64_MAX;
  }
  if (!ndone) {
    pthread_mutex_unlock (&this->event.mutex_wait);
    return 0;
  }
  for (n1 = 0; n1 < ndone; n1++) {
    video_overlay_event_t *event = xine_sarray_remove (this->event.wait, 0);
    h1[n1] = event - this->event.buf;
    if ((event->event_type == OVERLAY_EVENT_HIDE) && (this->event.last_hide[event->object.handle] == h1[n1]))
      this->event.last_hide[event->object.handle] = 255;
  }
  pthread_mutex_unlock (&this->event.mutex_wait);

  /* perform changes. */
  pthread_mutex_lock (&this->showing.mutex);
  for (n1 = 0; n1 < ndone; n1++) {
    video_overlay_event_t *event = this->event.buf + h1[n1];
    int32_t handle = event->object.handle;
#ifdef LOG_DEBUG
    printf (LOG_MODULE ": video_overlay_event: handle = %d\n", handle);
#endif
    switch (event->event_type) {
      case OVERLAY_EVENT_SHOW:
        {
          int changed = 0;
#ifdef LOG_DEBUG
          printf (LOG_MODULE ": SHOW SPU NOW\n");
#endif
          if (event->object.overlay) {
            /* set new image */
            changed = 1;
#ifdef LOG_DEBUG
            video_overlay_print_overlay (event->object.overlay);
#endif
            /* this->objects.buf[handle].overlay is about to be
             * overwritten by this event data. make sure we free it if needed.
             */
            if (this->objects.buf[handle].overlay) {
              /* it is legal to SHOW again with new image, without extra HIDE.
              xprintf(this->xine, XINE_VERBOSITY_DEBUG,
                LOG_MODULE ": (%d) object->overlay was not freed!\n", (int)handle);
              */
              set_argb_layer_ptr (&this->objects.buf[handle].overlay->argb_layer, NULL);
              _x_freep (&this->objects.buf[handle].overlay->rle);
              _x_freep (&this->objects.buf[handle].overlay);
            }
            this->objects.buf[handle].overlay = event->object.overlay;
            event->object.overlay = NULL;
          }
          /* it is also legal to re-SHOW after HIDE without a new image. */
          if (this->objects.buf[handle].overlay) {
            this->objects.buf[handle].handle = handle;
            this->objects.buf[handle].pts = event->object.pts;
            add_showing_handle (this, handle, changed);
          }
        }
        break;

      case OVERLAY_EVENT_HIDE:
#ifdef LOG_DEBUG
        printf (LOG_MODULE ": HIDE SPU NOW\n");
#endif
        /* free any (unneeded) overlay associated with this event */
        if (event->object.overlay) {
          set_argb_layer_ptr (&event->object.overlay->argb_layer, NULL);
          _x_freep (&event->object.overlay->rle);
          _x_freep (&event->object.overlay);
        }
        remove_showing_handle (this, handle);
        break;

      case OVERLAY_EVENT_FREE_HANDLE:
#ifdef LOG_DEBUG
        printf (LOG_MODULE ": FREE SPU NOW\n");
#endif
        /* free any overlay associated with this event */
        if (event->object.overlay) {
          set_argb_layer_ptr (&event->object.overlay->argb_layer, NULL);
          _x_freep (&event->object.overlay->rle);
          _x_freep (&event->object.overlay);
        }
        remove_showing_handle (this, handle);
        event->object.handle = -1;
        if (!_vovl_bits_set (refs, handle))
          h2[nremove++] = handle;
        break;

      case OVERLAY_EVENT_MENU_BUTTON:
        /* mixes palette and copy clip coords */
#ifdef LOG_DEBUG
        printf (LOG_MODULE ": MENU BUTTON NOW\n");
#endif
#if 0
        /* This code drops buttons, where the button PTS derived from the NAV
	 * packet on DVDs does not match the SPU PTS. Practical experience shows,
	 * that this is not necessary and causes problems with some DVDs */
        if (event->object.pts != this->objects.buf[handle].pts) {
          xprintf (this->xine, XINE_VERBOSITY_DEBUG,
		   LOG_MODULE ": MENU BUTTON DROPPED menu pts=%lld spu pts=%lld\n",
            event->object.pts,
            this->objects.buf[handle].pts);
          break;
        }
#endif
        if (event->object.overlay) {
          if (this->objects.buf[handle].overlay) {
            vo_overlay_t *overlay = this->objects.buf[handle].overlay;
            vo_overlay_t *event_overlay = event->object.overlay;
#ifdef LOG_DEBUG
            printf (LOG_MODULE ": overlay present\n");
#endif
            this->objects.buf[handle].handle = handle;
            overlay->hili_top = event_overlay->hili_top;
            overlay->hili_bottom = event_overlay->hili_bottom;
            overlay->hili_left = event_overlay->hili_left;
            overlay->hili_right = event_overlay->hili_right;
            overlay->hili_color[0] = event_overlay->hili_color[0];
            overlay->hili_color[1] = event_overlay->hili_color[1];
            overlay->hili_color[2] = event_overlay->hili_color[2];
            overlay->hili_color[3] = event_overlay->hili_color[3];
            memcpy (&overlay->hili_trans[0], &event_overlay->hili_trans[0], 4);
            overlay->hili_rgb_clut = event_overlay->hili_rgb_clut;
#ifdef LOG_DEBUG
            video_overlay_print_overlay (event->object.overlay);
#endif
            add_showing_handle (this, handle, 1);
          } else {
            xprintf (this->xine, XINE_VERBOSITY_DEBUG,
              LOG_MODULE ": (%d) EVENT_MENU_BUTTON without base image.\n", (int)handle);
          }
          set_argb_layer_ptr (&event->object.overlay->argb_layer, NULL);
          if (event->object.overlay->rle) {
            _x_freep (&event->object.overlay->rle);
            xprintf (this->xine, XINE_VERBOSITY_DEBUG,
                LOG_MODULE ": (%d) warning: EVENT_MENU_BUTTON with rle data\n", (int)handle);
          }
          _x_freep (&event->object.overlay);
        } else {
          xprintf (this->xine, XINE_VERBOSITY_DEBUG,
            LOG_MODULE ": (%d) EVENT_MENU_BUTTON without button image.\n", (int)handle);
        }
        break;

      default:
        xprintf (this->xine, XINE_VERBOSITY_DEBUG,
          LOG_MODULE ": (%d) unhandled event type %d.\n", (int)handle, event->event_type);
        break;
    }
  }
  pthread_mutex_unlock (&this->showing.mutex);

  /* remove orphaned events */
  if (nremove) {
    video_overlay_event_t *event;
    pthread_mutex_lock (&this->event.mutex_wait);
    for (n1 = 0; (event = xine_sarray_get (this->event.wait, n1)) != NULL; ) {
      uint32_t handle = event->object.handle;
      event->object.handle = -1;
      if (_vovl_bits_test (refs, handle)) {
        this->event.last_hide[handle] = 255;
        xine_sarray_remove (this->event.wait, n1);
        h1[ndone++] = event - this->event.buf;
        if (event->object.overlay) {
          set_argb_layer_ptr (&event->object.overlay->argb_layer, NULL);
          _x_freep (&event->object.overlay->rle);
          _x_freep (&event->object.overlay);
        }
      } else {
        n1++;
      }
    }
    pthread_mutex_unlock (&this->event.mutex_wait);
  }

  /* free events/handles. */
  pthread_mutex_lock (&this->event.mutex_free);
  for (n1 = 0; n1 < ndone; n1++)
    _vovl_event_free_put (this, h1[n1]);
  for (n1 = 0; n1 < nremove; n1++)
    _video_overlay_free_handle (this, h2[n1]);
  pthread_mutex_unlock (&this->event.mutex_free);

  return ndone;
}

void _x_overlay_clut_yuv2rgb(vo_overlay_t *overlay, int video_color_matrix)
{
  int cm = 10; /* ITU-R 601 (SD) */

  if (!overlay->rgb_clut) {
    uint8_t *p = (uint8_t *)overlay->color;
    if ((p[3] == 'X') && (p[7] == 'C') && (p[11] == 'M')) {
      cm = p[15];
      if ((cm >> 1) == 2) /* undefined */
        cm = video_color_matrix;
    }
    _x_clut_yuv2rgb(overlay->color, sizeof(overlay->color) / sizeof (overlay->color[0]), cm);
    overlay->rgb_clut++;
  }

  if (!overlay->hili_rgb_clut) {
    _x_clut_yuv2rgb(overlay->hili_color, sizeof (overlay->color) / sizeof (overlay->color[0]), cm);
    overlay->hili_rgb_clut++;
  }
}

static void clut_to_argb(const uint32_t *color, const uint8_t *trans, int num_items, uint32_t *argb, const char *format)
{
  /* xine/alphablend.h says:
    struct clut_s {
        uint8_t cb;
        uint8_t cr;
        uint8_t y;
        uint8_t foo;
    }
  */
  typedef union {
    uint32_t w;
    clut_t   c;
  } _clut_t;
  /* (n * 255 + 7) / 15 */
  static const _clut_t t[] = {
    {.c = {.foo =   0}},
    {.c = {.foo =  17}},
    {.c = {.foo =  34}},
    {.c = {.foo =  51}},
    {.c = {.foo =  68}},
    {.c = {.foo =  85}},
    {.c = {.foo = 102}},
    {.c = {.foo = 119}},
    {.c = {.foo = 136}},
    {.c = {.foo = 153}},
    {.c = {.foo = 170}},
    {.c = {.foo = 187}},
    {.c = {.foo = 204}},
    {.c = {.foo = 221}},
    {.c = {.foo = 238}},
    {.c = {.foo = 255}}
  };
  const _clut_t mask1 = {.c = {.cr = 255}},
                mask2 = {.c = {.cb = 255, .y = 255}},
                mask3 = {.c = {.cb = 255, .cr = 255, .y = 255}};
  int i;

  if (!strcmp(format, "BGRA")) {
    for (i = 0; i < num_items; i++)
      argb[i] = (color[i] & mask3.w) + t[trans[i] & 15].w;
  }
  else if (!strcmp(format, "RGBA")) {
    for (i = 0; i < num_items; i++) {
      uint32_t v = color[i];
      argb[i] = (((v << 16) | (v >> 16)) & mask2.w) + (v & mask1.w) + t[trans[i] & 15].w;
    }
  }
  else {
    fprintf(stderr, "clut_to_argb: unknown format %s\n", format);
  }
}

#define LUT_SIZE (sizeof(overlay->color)/sizeof(overlay->color[0]))
void _x_overlay_to_argb32 (const vo_overlay_t *overlay, uint32_t *rgba_buf, int stride, const char *format) {
  const rle_elem_t *rle = overlay->rle, *rle_end = rle + overlay->num_rle;
  int lines1, lines2, lines3;
  int pixels1, pixels2, pixels3;
  int prest, pad = stride - overlay->width;
  uint32_t *rgba = rgba_buf, colors[LUT_SIZE * 2], color;

  clut_to_argb (overlay->color, overlay->trans, LUT_SIZE, colors, format);

#define GET_DIM(dest,src,max) dest = src; if (dest < 0) dest = 0; else if (dest > max) dest = max;
  GET_DIM (lines1, overlay->hili_top, overlay->height);
  GET_DIM (lines2, overlay->hili_bottom - overlay->hili_top + 1, overlay->height - lines1);
  lines3 = overlay->height - lines1 - lines2;
  GET_DIM (pixels1, overlay->hili_left, overlay->width);
  GET_DIM (pixels2, overlay->hili_right - overlay->hili_left + 1, overlay->width - pixels1);
  pixels3 = overlay->width - pixels1 - pixels2;
#undef GET_DIM
  if ((lines2 > 0) && (pixels2 > 0)) { /* highlight */
    clut_to_argb (overlay->hili_color, overlay->hili_trans, LUT_SIZE, colors + LUT_SIZE, format);
  } else {
    lines1 += lines3;
    lines2 = lines3 = 0;
    pixels1 += pixels3;
    pixels2 = pixels3 = 0;
  }

#define MAKE_LINE(offs) \
  while (1) { \
    int pleft = prest > pixels ? pixels : prest; \
    pixels -= pleft; \
    prest -= pleft; \
    while (pleft > 0) { \
      *rgba++ = color; \
      pleft--; \
    } \
    if (pixels <= 0) \
      break; \
    if (rle >= rle_end) \
      goto _fill; \
    color = colors[rle->color + offs]; \
    prest = rle->len; \
    rle++; \
  }

  prest = 0;
  color = 0;
  /* top */
  while (lines1 > 0) {
    int pixels = overlay->width;
    MAKE_LINE (0);
    rgba += pad;
    lines1--;
  }

  /* highlight */
  while (lines2 > 0) {
    /* left */
    int pixels = pixels1;
    MAKE_LINE (0);
    /* highlighted */
    if (prest > 0)
      color = colors[rle[-1].color + LUT_SIZE];
    pixels = pixels2;
    MAKE_LINE (LUT_SIZE);
    /* right */
    if (prest > 0)
      color = colors[rle[-1].color];
    pixels = pixels3;
    MAKE_LINE (0);
    rgba += pad;
    lines2--;
  }

  /* bottom */
  while (lines3 > 0) {
    int pixels = overlay->width;
    MAKE_LINE (0);
    rgba += pad;
    lines3--;
  }

#undef MAKE_LINE

  return;

  _fill:
  {
    int n;
    n = rgba_buf + stride * overlay->height - rgba;
    if (n > 0)
      memset (rgba, 0, n * sizeof (uint32_t));
  }
}
#undef LUT_SIZE

/* This is called from video_out.c
 * must call output->overlay_blend for each active overlay.
 */
static void video_overlay_multiple_overlay_blend (video_overlay_manager_t *this_gen, int64_t vpts,
						  vo_driver_t *output, vo_frame_t *vo_img, int enabled) {
  video_overlay_t *this = (video_overlay_t *) this_gen;

  /* Look at next events, if current video vpts > first event on queue, process the event
   * else just continue
   */
  video_overlay_event (this, vpts);

  /* Scan through 5 entries and display any present.*/
  pthread_mutex_lock (&this->showing.mutex);

  if (output->overlay_begin)
    output->overlay_begin (output, vo_img, this->showing.changed);

  if (enabled && output->overlay_blend) {
    uint32_t n;

    for (n = this->showing.indx_f[_VOVL_USED_FIRST].next; this->showing.indx_f[n].next >= 0; n = this->showing.indx_f[n].next)
      output->overlay_blend (output, vo_img, this->objects.buf[this->showing.handle[n - _VOVL_LAST]].overlay);
  }

  if (output->overlay_end)
    output->overlay_end (output, vo_img);

  if (this->showing.changed) {
    xprintf (this->xine, XINE_VERBOSITY_DEBUG,
      LOG_MODULE ": %d showing changes @ vpts %" PRId64 ".\n", this->showing.changed, vpts);
    this->showing.changed = 0;
  }

  pthread_mutex_unlock (&this->showing.mutex);
}

/* this should be called on stream end or stop to make sure every
   hide event is processed.
*/
static void video_overlay_flush_events (video_overlay_manager_t *this_gen) {
  video_overlay_t *this = (video_overlay_t *) this_gen;

  video_overlay_event (this, 0);
}

/* this is called from video_out.c on still frames to check
   if a redraw is needed.
*/
static int video_overlay_redraw_needed (video_overlay_manager_t *this_gen, int64_t vpts) {
  video_overlay_t *this = (video_overlay_t *) this_gen;

  video_overlay_event( this, vpts );
  if (this->showing.changed) {
    xprintf (this->xine, XINE_VERBOSITY_DEBUG,
      LOG_MODULE ": %d showing changes @ vpts %" PRId64 ".\n", this->showing.changed, vpts);
  }
  return this->showing.changed;
}

static void video_overlay_dispose (video_overlay_manager_t *this_gen) {
  video_overlay_t *this = (video_overlay_t *) this_gen;
  int i;

  pthread_mutex_lock (&this->event.mutex_wait);
  for (i = 0; i < MAX_EVENTS; i++) {
    if (this->event.buf[i].object.overlay) {
      _x_freep (&this->event.buf[i].object.overlay->rle);
      _x_freep (&this->event.buf[i].object.overlay);
    }
  }
  for (i = 0; i < MAX_OBJECTS; i++)
    _video_overlay_free_handle (this, i);
  pthread_mutex_unlock (&this->event.mutex_wait);

  xine_sarray_delete (this->event.wait);

  pthread_mutex_destroy (&this->showing.mutex);
  pthread_mutex_destroy (&this->event.mutex_free);
  pthread_mutex_destroy (&this->event.mutex_wait);

  free (this);
}

static int _video_overlay_event_cmp (void *a, void *b) {
  video_overlay_event_t *d = (video_overlay_event_t *)a;
  video_overlay_event_t *e = (video_overlay_event_t *)b;

  return (d->vpts < e->vpts) ? -1 : (d->vpts > e->vpts) ? 1 : 0;
}

video_overlay_manager_t *_x_video_overlay_new_manager (xine_t *xine) {
  int i;
  video_overlay_t *this = calloc (1, sizeof (*this));

  if (!this)
    return NULL;

  pthread_mutex_init (&this->event.mutex_free, NULL);

  pthread_mutex_init (&this->event.mutex_wait, NULL);
  this->event.wait = xine_sarray_new (MAX_EVENTS, _video_overlay_event_cmp);
  xine_sarray_set_mode (this->event.wait, XINE_SARRAY_MODE_LAST);

  pthread_mutex_init (&this->showing.mutex, NULL);

#ifndef HAVE_ZERO_SAFE_MEM
  this->last_vpts = 0;
  for (i = 0; i < MAX_EVENTS; i++) {
    this->event.buf[i].object.overlay = NULL;
    this->event.buf[i].event_type = 0;
  }
  this->showing.have = 0;
#endif
  this->event.first_vpts = INT64_MAX;
  memset (this->event.last_hide, 255, sizeof (this->event.last_hide));

  for (i = 0; i < MAX_OBJECTS; i++) {
    this->objects.buf[i].handle = -1;
#ifndef HAVE_ZERO_SAFE_MEM
    this->objects.buf[i].object_type = 0;
    this->objects.buf[i].pts = 0;
    this->objects.buf[i].overlay = NULL;
    this->objects.buf[i].palette = NULL;
    this->objects.buf[i].palette_type = 0;
#endif
  }

  _video_overlay_list_init (this->objects.indx_f, MAX_OBJECTS);

  memset (this->showing.handle, -1 & 255, sizeof (this->showing.handle));
  memset (this->showing.indx_r, -1 & 255, sizeof (this->showing.indx_r));
  _video_overlay_list_init (this->showing.indx_f, MAX_SHOWING);

  this->xine                              = xine;
  this->video_overlay.init                = video_overlay_init;
  this->video_overlay.dispose             = video_overlay_dispose;
  this->video_overlay.get_handle          = video_overlay_get_handle;
  this->video_overlay.free_handle         = video_overlay_free_handle;
  this->video_overlay.add_event           = video_overlay_add_event;
  this->video_overlay.flush_events        = video_overlay_flush_events;
  this->video_overlay.redraw_needed       = video_overlay_redraw_needed;
  this->video_overlay.multiple_overlay_blend = video_overlay_multiple_overlay_blend;

  return &this->video_overlay;
}
