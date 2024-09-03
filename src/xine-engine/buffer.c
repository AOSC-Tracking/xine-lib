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
 *
 *
 * contents:
 *
 * buffer_entry structure - serves as a transport encapsulation
 *   of the mpeg audio/video data through xine
 *
 * free buffer pool management routines
 *
 * FIFO buffer structures/routines
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

/********** logging **********/
#define LOG_MODULE "buffer"
#define LOG_VERBOSE
/*
#define LOG
*/

#include <xine/buffer.h>
#include <xine/xineutils.h>
#include <xine/xine_internal.h>
#include "xine_private.h"

/** TJ. NOTE:
 *  vdr-libxineoutput issue #1:
 *  some versions of vdr-libxineoutput use their own replacement of _x_fifo_buffer_new ().
 *  this involves use of an incomplete fifo_buffer_t with our native methods copied
 *  from stream->video_fifo. test for nativity, and fall back to very old behaviour if not.
 *  vdr-libxineoutput issue #2:
 *  we will get some custom buf types, most notably
 *  0x0f010000 CONTROL_BUF_BLANK
 *  0x05010000 BUF_NETWORK_BLOCK
 *  0x05020000 BUF_LOCAL_BLOCK
 *  make sure to treat them like control bufs which keeps put order. */

static const uint8_t _fifo_buf_type_index[256] = {
  [BUF_AUDIO_BASE >> 24] = 1,
  [BUF_VIDEO_BASE >> 24] = 1,
  [BUF_SPU_BASE >> 24] = 2
};

typedef struct {
  fifo_buffer_t   b;

  uint32_t       *fds;
  buf_element_t **last_add[2];
} _fifo_buffer_t;

static void _fifo_mark_native (_fifo_buffer_t *fifo) {
  fifo->fds = &fifo->b.fifo_data_size;
}

static int _fifo_is_native (_fifo_buffer_t *fifo) {
  return fifo->fds == &fifo->b.fifo_data_size;
}

static void _fifo_mux_init (_fifo_buffer_t *fifo) {
  fifo->last_add[0] = fifo->last_add[1] = &fifo->b.first;
}

static void _fifo_mux_last (_fifo_buffer_t *fifo) {
  if (_fifo_is_native (fifo))
    fifo->last_add[0] = fifo->last_add[1] = fifo->b.last ? &fifo->b.last->next : &fifo->b.first;
}

/* The large buffer feature.
 * If we have enough contigous memory, and if we can afford to hand if out,
 * provide an oversize item there. The buffers covering that extra space will
 * hide inside our buffer array, and buffer_pool_free () will reappear them
 * mysteriously later.
 * Small bufs are requested frequently, so we dont do a straightforward
 * heap manager. Instead, we keep bufs in pool sorted by address, and
 * be_ei_t.nbufs holds the count of contigous bufs when this is the
 * first of such a group.
 * API permits using bufs in a different fifo than their pool origin
 * (see demux_mpeg_block). Thats why we test buf->source.
 * It is also possible to supply fully custom bufs. We detect these by
 * buf->free_buffer != buffer_pool_free.
 */

typedef struct {
  buf_element_t elem; /* needs to be first */
  int nbufs;          /* # of contigous bufs */
  extra_info_t  ei;
} be_ei_t;

#define LARGE_NUM 0x7fffffff

/* The file buf ctrl feature.
 * After stream start/seek (fifo flush), there is a phase when a few decoded frames
 * are better than a lot of merely demuxed ones. Net_buf_ctrl wants large fifos to
 * handle fragment and other stuttering streams. Lets assume that it knows what to
 * do there. For plain files, however, demux is likely to drain processor time from
 * decoders initially.
 * A separate file_buf_ctrl module should not mess with fifo internals, thus lets
 * do a little soft start version here when there are no callbacks:
 * fifo->alloc_cb[0] == fbc_dummy,
 * fifo->alloc_cb_data[0] == count of yet not to be used bufs. */

static void fbc_dummy (fifo_buffer_t *fifo, void *data) {
  (void)fifo;
  (void)data;
}

int xine_fbc_set (fifo_buffer_t *_fifo, int on) {
  _fifo_buffer_t *fifo = (_fifo_buffer_t *)_fifo;

  if (!_fifo)
    return 0;
  pthread_mutex_lock (&fifo->b.mutex);

  if (on) {
    int n;
    if (fifo->b.alloc_cb[0]) {
      n = (fifo->b.alloc_cb[0] == fbc_dummy);
      pthread_mutex_unlock (&fifo->b.mutex);
      return n;
    }
    fifo->b.alloc_cb[0] = fbc_dummy;
    n = (fifo->b.buffer_pool_capacity * 3) >> 2;
    if (n < 75)
      n = 0;
    fifo->b.alloc_cb_data[0] = (void *)(intptr_t)n;
    pthread_mutex_unlock (&fifo->b.mutex);
    return 1;
  }

  if (fifo->b.alloc_cb[0] == fbc_dummy) {
    fifo->b.alloc_cb[0] = NULL;
    fifo->b.alloc_cb_data[0] = (void *)0;
  }
  pthread_mutex_unlock (&fifo->b.mutex);
  return 0;
}

static int _fbc_avail (_fifo_buffer_t *fifo) {
  return fifo->b.alloc_cb[0] != fbc_dummy
    ? fifo->b.buffer_pool_num_free
    : fifo->b.buffer_pool_num_free - (intptr_t)fifo->b.alloc_cb_data[0];
}

static void _fbc_reset (_fifo_buffer_t *fifo) {
  if (fifo->b.alloc_cb[0] == fbc_dummy) {
    int n = (fifo->b.buffer_pool_capacity * 3) >> 2;
    if (n < 75)
      n = 0;
    fifo->b.alloc_cb_data[0] = (void *)(intptr_t)n;
  }
}

static void _fbc_sub (_fifo_buffer_t *fifo, int n) {
  if (fifo->b.alloc_cb[0] == fbc_dummy) {
    n = (intptr_t)fifo->b.alloc_cb_data[0] - n;
    if (n < 0)
      n = 0;
    fifo->b.alloc_cb_data[0] = (void *)(intptr_t)n;
  }
}

/*
 * put a previously allocated buffer element back into the buffer pool
 */
static void buffer_pool_free (buf_element_t *element) {
  _fifo_buffer_t *fifo = (_fifo_buffer_t *) element->source;
  be_ei_t *newhead, *newtail, *nexthead;
  int n;

  pthread_mutex_lock (&fifo->b.buffer_pool_mutex);

  newhead = (be_ei_t *)element;
  n = newhead->nbufs;
  _fbc_sub (fifo, n);
  fifo->b.buffer_pool_num_free += n;
  if (fifo->b.buffer_pool_num_free > fifo->b.buffer_pool_capacity) {
    fprintf(stderr, _("xine-lib: buffer.c: There has been a fatal error: TOO MANY FREE's\n"));
    _x_abort();
  }

  /* we might be a new chunk */
  newtail = newhead + 1;
  while (--n > 0) {
    newtail[-1].elem.next = &newtail[0].elem;
    newtail++;
  }

  nexthead = (be_ei_t *)fifo->b.buffer_pool_top;
  if (!nexthead || (nexthead >= newtail)) {
    /* add head */
    fifo->b.buffer_pool_top = &newhead->elem;
    newtail[-1].elem.next = &nexthead->elem;
    /* merge with next chunk if no gap */
    if (newtail == nexthead)
      newhead->nbufs += nexthead->nbufs;
  } else {
    /* Keep the pool sorted, elem1 > elem2 implies elem1->mem > elem2->mem. */
    be_ei_t *prevhead, *prevtail;
    while (1) {
      prevhead = nexthead;
      prevtail = prevhead + prevhead->nbufs;
      nexthead = (be_ei_t *)prevtail[-1].elem.next;
      if (!nexthead || (nexthead >= newtail))
        break;
    }
    prevtail[-1].elem.next = &newhead->elem;
    newtail[-1].elem.next = &nexthead->elem;
    /* merge with next chunk if no gap */
    if (newtail == nexthead)
      newhead->nbufs += nexthead->nbufs;
    /* merge with prev chunk if no gap */
    if (prevtail == newhead)
      prevhead->nbufs += newhead->nbufs;
  }

  /* dont provoke useless wakeups */
  if (fifo->b.buffer_pool_num_waiters ||
    (fifo->b.buffer_pool_large_wait <= _fbc_avail (fifo)))
    pthread_cond_signal (&fifo->b.buffer_pool_cond_not_empty);

  pthread_mutex_unlock (&fifo->b.buffer_pool_mutex);
}

/*
 * allocate a buffer from buffer pool
 */

static buf_element_t *_buffer_pool_size_alloc (_fifo_buffer_t *fifo, int n) {

  int i;
  be_ei_t *buf;

  for (i = 0; fifo->b.alloc_cb[i]; i++)
    fifo->b.alloc_cb[i] (&fifo->b, fifo->b.alloc_cb_data[i]);

  if (n < 1)
    n = 1;
  /* we always keep one free buffer for emergency situations like
   * decoder flushes that would need a buffer in buffer_pool_try_alloc() */
  n += 2;
  if (_fbc_avail (fifo) < n) {
    /* Paranoia: someone else than demux calling this in parallel ?? */
    if (fifo->b.buffer_pool_large_wait != LARGE_NUM) {
      fifo->b.buffer_pool_num_waiters++;
      do {
        pthread_cond_wait (&fifo->b.buffer_pool_cond_not_empty, &fifo->b.buffer_pool_mutex);
      } while (_fbc_avail (fifo) < n);
      fifo->b.buffer_pool_num_waiters--;
    } else {
      fifo->b.buffer_pool_large_wait = n;
      do {
        pthread_cond_wait (&fifo->b.buffer_pool_cond_not_empty, &fifo->b.buffer_pool_mutex);
      } while (_fbc_avail (fifo) < n);
      fifo->b.buffer_pool_large_wait = LARGE_NUM;
    }
  }
  n -= 2;

  buf = (be_ei_t *)fifo->b.buffer_pool_top;
  if (n == 1) {

    fifo->b.buffer_pool_top = buf->elem.next;
    i = buf->nbufs - 1;
    if (i > 0)
      buf[1].nbufs = i;
    fifo->b.buffer_pool_num_free--;

  } else {

    buf_element_t **link = &fifo->b.buffer_pool_top, **bestlink = link;
    int bestsize = 0;
    while (1) {
      int l = buf->nbufs;
      if (l > n) {
        be_ei_t *next = buf + n;
        next->nbufs = l - n;
        *link = &next->elem;
        break;
      } else if (l == n) {
        *link = buf[l - 1].elem.next;
        break;
      }
      if (l > bestsize) {
        bestsize = l;
        bestlink = link;
      }
      buf += l - 1;
      link = &buf->elem.next;
      buf = (be_ei_t *)(*link);
      if (!buf) {
        buf = (be_ei_t *)(*bestlink);
        n = bestsize;
        *bestlink = buf[n - 1].elem.next;
        break;
      }
    }
    fifo->b.buffer_pool_num_free -= n;

  }

  pthread_mutex_unlock (&fifo->b.buffer_pool_mutex);

  /* set sane values to the newly allocated buffer */
  buf->elem.content = buf->elem.mem; /* 99% of demuxers will want this */
  buf->elem.pts = 0;
  buf->elem.size = 0;
  buf->elem.max_size = n * fifo->b.buffer_pool_buf_size;
  buf->elem.decoder_flags = 0;
  buf->nbufs = n;
  memset (buf->elem.decoder_info, 0, sizeof (buf->elem.decoder_info));
  memset (buf->elem.decoder_info_ptr, 0, sizeof (buf->elem.decoder_info_ptr));
  _x_extra_info_reset (buf->elem.extra_info);

  return &buf->elem;
}

static buf_element_t *buffer_pool_size_alloc (fifo_buffer_t *_fifo, size_t size) {
  _fifo_buffer_t *fifo = (_fifo_buffer_t *)_fifo;
  int n;

  if (!_fifo_is_native (fifo))
    return fifo->b.buffer_pool_alloc (&fifo->b);

  n = size ? ((int)size + fifo->b.buffer_pool_buf_size - 1) / fifo->b.buffer_pool_buf_size : 1;
  if (n > (fifo->b.buffer_pool_capacity >> 2))
    n = fifo->b.buffer_pool_capacity >> 2;
  pthread_mutex_lock (&fifo->b.buffer_pool_mutex);
  return _buffer_pool_size_alloc (fifo, n);
}


static buf_element_t *buffer_pool_alloc (fifo_buffer_t *_fifo) {
  _fifo_buffer_t *fifo = (_fifo_buffer_t *)_fifo;
  be_ei_t *buf;
  int i;

  pthread_mutex_lock (&fifo->b.buffer_pool_mutex);

  for (i = 0; fifo->b.alloc_cb[i]; i++)
    fifo->b.alloc_cb[i] (&fifo->b, fifo->b.alloc_cb_data[i]);

  /* we always keep one free buffer for emergency situations like
   * decoder flushes that would need a buffer in buffer_pool_try_alloc() */
  if (_fbc_avail (fifo) < 2) {
    fifo->b.buffer_pool_num_waiters++;
    do {
      pthread_cond_wait (&fifo->b.buffer_pool_cond_not_empty, &fifo->b.buffer_pool_mutex);
    } while (_fbc_avail (fifo) < 2);
    fifo->b.buffer_pool_num_waiters--;
  }

  buf = (be_ei_t *)fifo->b.buffer_pool_top;
  fifo->b.buffer_pool_top = buf->elem.next;
  if (_fifo_is_native (fifo)) {
    i = buf->nbufs - 1;
    if (i > 0)
      buf[1].nbufs = i;
    buf->nbufs = 1;
  }
  fifo->b.buffer_pool_num_free--;

  pthread_mutex_unlock (&fifo->b.buffer_pool_mutex);

  /* set sane values to the newly allocated buffer */
  buf->elem.content = buf->elem.mem; /* 99% of demuxers will want this */
  buf->elem.pts = 0;
  buf->elem.size = 0;
  buf->elem.max_size = fifo->b.buffer_pool_buf_size;
  buf->elem.decoder_flags = 0;
  memset (buf->elem.decoder_info, 0, sizeof (buf->elem.decoder_info));
  memset (buf->elem.decoder_info_ptr, 0, sizeof (buf->elem.decoder_info_ptr));
  _x_extra_info_reset (buf->elem.extra_info);

  return &buf->elem;
}

static buf_element_t *buffer_pool_realloc (buf_element_t *buf, size_t new_size) {
  _fifo_buffer_t *fifo;
  buf_element_t **last_buf;
  be_ei_t *old_buf = (be_ei_t *)buf, *new_buf, *want_buf;
  int n;

  if (!old_buf)
    return NULL;
  if ((int)new_size <= old_buf->elem.max_size)
    return NULL;
  if (old_buf->elem.free_buffer != buffer_pool_free)
    return NULL;
  fifo = (_fifo_buffer_t *)old_buf->elem.source;
  if (!fifo)
    return NULL;

  if (!_fifo_is_native (fifo))
    return fifo->b.buffer_pool_alloc (&fifo->b);

  n = ((int)new_size + fifo->b.buffer_pool_buf_size - 1) / fifo->b.buffer_pool_buf_size;
  /* limit size to keep pool fluent */
  if (n > (fifo->b.buffer_pool_capacity >> 3))
    n = fifo->b.buffer_pool_capacity >> 3;
  n -= old_buf->nbufs;

  want_buf = old_buf + old_buf->nbufs;
  last_buf = &fifo->b.buffer_pool_top;
  pthread_mutex_lock (&fifo->b.buffer_pool_mutex);
  while (1) {
    new_buf = (be_ei_t *)(*last_buf);
    if (!new_buf)
      break;
    if (new_buf == want_buf)
      break;
    if (new_buf > want_buf) {
      new_buf = NULL;
      break;
    }
    new_buf += new_buf->nbufs;
    last_buf = &(new_buf[-1].elem.next);
  }

  if (new_buf) do {
    int s;
    /* save emergecy buf */
    if (n > fifo->b.buffer_pool_num_free - 1)
      n = fifo->b.buffer_pool_num_free - 1;
    if (n < 1)
      break;
    s = new_buf->nbufs - n;
    if (s > 0) {
      new_buf += n;
      new_buf->nbufs = s;
      *last_buf = &new_buf->elem;
    } else {
      n = new_buf->nbufs;
      new_buf += n;
      *last_buf = new_buf[-1].elem.next;
    }
    fifo->b.buffer_pool_num_free -= n;
    pthread_mutex_unlock (&fifo->b.buffer_pool_mutex);
    old_buf->nbufs += n;
    old_buf->elem.max_size = old_buf->nbufs * fifo->b.buffer_pool_buf_size;
    return NULL;
  } while (0);

  return _buffer_pool_size_alloc (fifo, n);
}

/*
 * allocate a buffer from buffer pool - may fail if none is available
 */

static buf_element_t *buffer_pool_try_alloc (fifo_buffer_t *_fifo) {
  _fifo_buffer_t *fifo = (_fifo_buffer_t *)_fifo;
  be_ei_t *buf;
  int i;

  pthread_mutex_lock (&fifo->b.buffer_pool_mutex);
  buf = (be_ei_t *)fifo->b.buffer_pool_top;
  if (!buf) {
    pthread_mutex_unlock (&fifo->b.buffer_pool_mutex);
    return NULL;
  }

  fifo->b.buffer_pool_top = buf->elem.next;
  if (_fifo_is_native (fifo)) {
    i = buf->nbufs - 1;
    if (i > 0)
      buf[1].nbufs = i;
    buf->nbufs = 1;
  }
  fifo->b.buffer_pool_num_free--;
  pthread_mutex_unlock (&fifo->b.buffer_pool_mutex);

  /* set sane values to the newly allocated buffer */
  buf->elem.content = buf->elem.mem; /* 99% of demuxers will want this */
  buf->elem.pts = 0;
  buf->elem.size = 0;
  buf->elem.max_size = fifo->b.buffer_pool_buf_size;
  buf->elem.decoder_flags = 0;
  memset (buf->elem.decoder_info, 0, sizeof (buf->elem.decoder_info));
  memset (buf->elem.decoder_info_ptr, 0, sizeof (buf->elem.decoder_info_ptr));
  _x_extra_info_reset (buf->elem.extra_info);

  return &buf->elem;
}


/*
 * append buffer element to fifo buffer
 */
static void fifo_buffer_put (fifo_buffer_t *_fifo, buf_element_t *element) {
  _fifo_buffer_t *fifo = (_fifo_buffer_t *)_fifo;
  buf_element_t **next;
  uint32_t type;
  int i;

  pthread_mutex_lock (&fifo->b.mutex);

  if (element->decoder_flags & BUF_FLAG_MERGE) {
    be_ei_t *new = (be_ei_t *)element, *prev = (be_ei_t *)fifo->b.last;
    new->elem.decoder_flags &= ~BUF_FLAG_MERGE;
    if (prev && (prev + prev->nbufs == new)
      && (prev->elem.type == new->elem.type)
      && (prev->nbufs < (fifo->b.buffer_pool_capacity >> 3))) {
      fifo->b.fifo_size += new->nbufs;
      fifo->b.fifo_data_size += new->elem.size;
      prev->nbufs += new->nbufs;
      prev->elem.max_size += new->elem.max_size;
      prev->elem.size += new->elem.size;
      prev->elem.decoder_flags |= new->elem.decoder_flags;
      pthread_mutex_unlock (&fifo->b.mutex);
      return;
    }
  }

  for(i = 0; fifo->b.put_cb[i]; i++)
    fifo->b.put_cb[i] (&fifo->b, element, fifo->b.put_cb_data[i]);

  /* try to mux spu tracks, especially separate ones. */
  type = _fifo_buf_type_index[(uint32_t)element->type >> 24];
  if (!_fifo_is_native (fifo)) {
    element->next = NULL;
    if (fifo->b.first) {
      fifo->b.first->next = fifo->b.last = element;
    } else {
      fifo->b.first = fifo->b.last = element;
    }
  } else if (type == 0) {
    /* always add ctrl and custom stuff fence at the end. */
    element->next = NULL;
    next = fifo->b.last ? &fifo->b.last->next : &fifo->b.first;
    *next = element;
    fifo->b.last = element;
    fifo->last_add[0] = fifo->last_add[1] = &element->next;
  } else {
    /* keep order within same type group.
     * look from last add in this group. if there are more bufs,
     * they must be from the other group - no need to test again. */
    type--;
    next = fifo->last_add[type];
    if (element->pts) {
      /* allow a second of overlap to compensate for frame reordering. */
      static const int overlap[2] = {90000, -90000};
      int64_t epts = element->pts + overlap[type];
      buf_element_t *b2;
      while ((b2 = *next)) {
        if (b2->pts > epts)
          break;
        next = &b2->next;
      }
    }
    if (!(element->next = *next))
      fifo->b.last = element;
    *next = element;
    fifo->last_add[type] = &element->next;
  }

  if (element->free_buffer == buffer_pool_free) {
    be_ei_t *beei = (be_ei_t *)element;
    fifo->b.fifo_size += beei->nbufs;
  } else {
    fifo->b.fifo_size += 1;
  }
  fifo->b.fifo_data_size += element->size;

  if (fifo->b.fifo_num_waiters)
    pthread_cond_signal (&fifo->b.not_empty);

  pthread_mutex_unlock (&fifo->b.mutex);
}

/*
 * simulate append buffer element to fifo buffer
 */
static void dummy_fifo_buffer_put (fifo_buffer_t *_fifo, buf_element_t *element) {
  _fifo_buffer_t *fifo = (_fifo_buffer_t *)_fifo;
  int i;

  pthread_mutex_lock (&fifo->b.mutex);

  for (i = 0; fifo->b.put_cb[i]; i++)
    fifo->b.put_cb[i] (&fifo->b, element, fifo->b.put_cb_data[i]);

  pthread_mutex_unlock (&fifo->b.mutex);

  element->free_buffer(element);
}

/*
 * insert buffer element to fifo buffer (demuxers MUST NOT call this one)
 */
static void fifo_buffer_insert (fifo_buffer_t *_fifo, buf_element_t *element) {
  _fifo_buffer_t *fifo = (_fifo_buffer_t *)_fifo;

  pthread_mutex_lock (&fifo->b.mutex);

  element->next = fifo->b.first;
  fifo->b.first = element;

  if( !fifo->b.last )
    fifo->b.last = element;

  if (_fifo_is_native (fifo)) {
    if (fifo->last_add[0] == &fifo->b.first)
      fifo->last_add[0] = &element->next;
    if (fifo->last_add[1] == &fifo->b.first)
      fifo->last_add[1] = &element->next;
  }

  if (element->free_buffer == buffer_pool_free) {
    be_ei_t *beei = (be_ei_t *)element;
    fifo->b.fifo_size += beei->nbufs;
  } else {
    fifo->b.fifo_size += 1;
  }
  fifo->b.fifo_data_size += element->size;

  if (fifo->b.fifo_num_waiters)
    pthread_cond_signal (&fifo->b.not_empty);

  pthread_mutex_unlock (&fifo->b.mutex);
}

/*
 * insert buffer element to fifo buffer (demuxers MUST NOT call this one)
 */
static void dummy_fifo_buffer_insert (fifo_buffer_t *fifo, buf_element_t *element) {
  (void)fifo;
  element->free_buffer(element);
}

/*
 * get element from fifo buffer
 */
static buf_element_t *fifo_buffer_get (fifo_buffer_t *_fifo) {
  _fifo_buffer_t *fifo = (_fifo_buffer_t *)_fifo;
  buf_element_t *buf;
  int i;

  pthread_mutex_lock (&fifo->b.mutex);

  if (!fifo->b.first) {
    fifo->b.fifo_num_waiters++;
    do {
      pthread_cond_wait (&fifo->b.not_empty, &fifo->b.mutex);
    } while (!fifo->b.first);
    fifo->b.fifo_num_waiters--;
  }

  buf = fifo->b.first;
  if (!(fifo->b.first = buf->next))
    fifo->b.last = NULL;
  buf->next = NULL;
  if (_fifo_is_native (fifo)) {
    if (fifo->last_add[0] == &buf->next)
      fifo->last_add[0] = &fifo->b.first;
    if (fifo->last_add[1] == &buf->next)
      fifo->last_add[1] = &fifo->b.first;
  }

  if (buf->free_buffer == buffer_pool_free) {
    be_ei_t *beei = (be_ei_t *)buf;
    fifo->b.fifo_size -= beei->nbufs;
  } else {
    fifo->b.fifo_size -= 1;
  }
  fifo->b.fifo_data_size -= buf->size;

  for (i = 0; fifo->b.get_cb[i]; i++)
    fifo->b.get_cb[i] (&fifo->b, buf, fifo->b.get_cb_data[i]);

  pthread_mutex_unlock (&fifo->b.mutex);

  return buf;
}

static buf_element_t *fifo_buffer_tget (fifo_buffer_t *_fifo, xine_ticket_t *ticket) {
  /* Optimization: let decoders hold port ticket by default.
   * Unfortunately, fifo callbacks are 1 big freezer, as they run with fifo locked,
   * and may try to revoke ticket for pauseing or other stuff.
   * Always releasing ticket when there are callbacks is safe but inefficient.
   * Instead, we release ticket when we are going to wait for fifo or a buffer,
   * and of course, when the ticket has been revoked.
   * This should melt the "put" side. We could still freeze ourselves directly
   * at the "get" side, what ticket->revoke () self grant hack shall fix.
   */
  _fifo_buffer_t *fifo = (_fifo_buffer_t *)_fifo;
  buf_element_t *buf;
  int mode = ticket ? 2 : 0, i;

  if (pthread_mutex_trylock (&fifo->b.mutex)) {
    if (mode & 2) {
      ticket->release (ticket, 0);
      mode = 1;
    }
    pthread_mutex_lock (&fifo->b.mutex);
  }

  if (!fifo->b.first) {
    if (mode & 2) {
      ticket->release (ticket, 0);
      mode = 1;
    }
    fifo->b.fifo_num_waiters++;
    do {
      pthread_cond_wait (&fifo->b.not_empty, &fifo->b.mutex);
    } while (!fifo->b.first);
    fifo->b.fifo_num_waiters--;
  }

  buf = fifo->b.first;
  if (!(fifo->b.first = buf->next))
    fifo->b.last = NULL;
  buf->next = NULL;
  if (_fifo_is_native (fifo)) {
    if (fifo->last_add[0] == &buf->next)
      fifo->last_add[0] = &fifo->b.first;
    if (fifo->last_add[1] == &buf->next)
      fifo->last_add[1] = &fifo->b.first;
  }

  if (buf->free_buffer == buffer_pool_free) {
    be_ei_t *beei = (be_ei_t *)buf;
    fifo->b.fifo_size -= beei->nbufs;
  } else {
    fifo->b.fifo_size -= 1;
  }
  fifo->b.fifo_data_size -= buf->size;

  if ((mode & 2) && ticket->ticket_revoked) {
    ticket->release (ticket, 0);
    mode = 1;
  }

  for(i = 0; fifo->b.get_cb[i]; i++)
    fifo->b.get_cb[i] (&fifo->b, buf, fifo->b.get_cb_data[i]);

  pthread_mutex_unlock (&fifo->b.mutex);

  if (mode & 1)
    ticket->acquire (ticket, 0);

  return buf;
}


/*
 * clear buffer (put all contained buffer elements back into buffer pool)
 */
static void fifo_buffer_clear (fifo_buffer_t *_fifo) {
  _fifo_buffer_t *fifo = (_fifo_buffer_t *)_fifo;
  be_ei_t *start;

  pthread_mutex_lock (&fifo->b.mutex);

  /* take out all at once */
  start = (be_ei_t *)fifo->b.first;
  fifo->b.first = fifo->b.last = NULL;
  fifo->b.fifo_size = 0;
  fifo->b.fifo_data_size = 0;

  while (start) {
    be_ei_t *buf, *next;
    int n;

    /* keep control bufs (flush, ...) */
    if ((start->elem.type & BUF_MAJOR_MASK) == BUF_CONTROL_BASE) {
      if (!fifo->b.first)
        fifo->b.first = &start->elem;
      else
        fifo->b.last->next = &start->elem;
      fifo->b.last = &start->elem;
      fifo->b.fifo_size += 1;
      fifo->b.fifo_data_size += start->elem.size;
      buf = (be_ei_t *)start->elem.next;
      start->elem.next = NULL;
      start = buf;
      continue;
    }

    /* free custom buf */
    if (start->elem.free_buffer != buffer_pool_free) {
      buf = (be_ei_t *)start->elem.next;
      start->elem.next = NULL;
      start->elem.free_buffer (&start->elem);
      start = buf;
      continue;
    }

    /* optimize: get contiguous chunk */
    buf = start;
    n = 0;
    while (1) {
      int i = buf->nbufs;
      next = (be_ei_t *)buf->elem.next;
      n += i;
      if (buf + i != next) /* includes next == NULL et al ;-) */
        break;
      if ((next->elem.type & BUF_MAJOR_MASK) == BUF_CONTROL_BASE)
        break;
      buf = next;
    }
    start->nbufs = n;
    start->elem.free_buffer (&start->elem);
    start = next;
  }

  _fbc_reset (fifo);
  _fifo_mux_last (fifo);
  /* printf("Free buffers after clear: %d\n", fifo->buffer_pool_num_free); */
  pthread_mutex_unlock (&fifo->b.mutex);
}

static void _fifo_buffer_all_clear (_fifo_buffer_t *fifo) {
  be_ei_t *start;

  pthread_mutex_lock (&fifo->b.mutex);

  /* take out all at once */
  start = (be_ei_t *)fifo->b.first;
  fifo->b.first = fifo->b.last = NULL;
  fifo->b.fifo_size = 0;
  fifo->b.fifo_data_size = 0;

  while (start) {
    be_ei_t *buf, *next;
    int n;

    /* free custom buf */
    if (start->elem.free_buffer != buffer_pool_free) {
      buf = (be_ei_t *)start->elem.next;
      start->elem.next = NULL;
      start->elem.free_buffer (&start->elem);
      start = buf;
      continue;
    }

    /* optimize: get contiguous chunk */
    buf = start;
    n = 0;
    while (1) {
      int i = buf->nbufs;
      next = (be_ei_t *)buf->elem.next;
      n += i;
      if (buf + i != next) /* includes next == NULL ;-) */
        break;
      buf = next;
    }
    /* free just sibling bufs */
    if (start->elem.source != (void *)fifo) {
      start->nbufs = n;
      start->elem.free_buffer (&start->elem);
    }
    start = next;
  }

  _fifo_mux_last (fifo);
  pthread_mutex_unlock (&fifo->b.mutex);
}

/*
 * Return the number of elements in the fifo buffer
 */
static int fifo_buffer_size (fifo_buffer_t *_fifo) {
  _fifo_buffer_t *fifo = (_fifo_buffer_t *)_fifo;
  int size;

  pthread_mutex_lock (&fifo->b.mutex);
  size = fifo->b.fifo_size;
  pthread_mutex_unlock (&fifo->b.mutex);

  return size;
}

/*
 * Return the amount of the data in the fifo buffer
 */
static uint32_t fifo_buffer_data_size (fifo_buffer_t *_fifo) {
  _fifo_buffer_t *fifo = (_fifo_buffer_t *)_fifo;
  uint32_t data_size;

  pthread_mutex_lock (&fifo->b.mutex);
  data_size = fifo->b.fifo_data_size;
  pthread_mutex_unlock (&fifo->b.mutex);

  return data_size;
}

/*
 * Return the number of free elements in the pool
 */
static int fifo_buffer_num_free (fifo_buffer_t *_fifo) {
  _fifo_buffer_t *fifo = (_fifo_buffer_t *)_fifo;
  int buffer_pool_num_free;

  pthread_mutex_lock (&fifo->b.buffer_pool_mutex);
  buffer_pool_num_free = fifo->b.buffer_pool_num_free;
  pthread_mutex_unlock (&fifo->b.buffer_pool_mutex);

  return buffer_pool_num_free;
}

/*
 * Destroy the buffer
 */
static void fifo_buffer_dispose (fifo_buffer_t *_fifo) {
  _fifo_buffer_t *fifo = (_fifo_buffer_t *)_fifo;

  if (!fifo)
    return;

  _fifo_buffer_all_clear (fifo);
  if (_fifo_is_native (fifo))
    xine_free_aligned (fifo->b.buffer_pool_base);
  pthread_mutex_destroy (&fifo->b.mutex);
  pthread_cond_destroy (&fifo->b.not_empty);
  pthread_mutex_destroy (&fifo->b.buffer_pool_mutex);
  pthread_cond_destroy (&fifo->b.buffer_pool_cond_not_empty);
  free (fifo);
}

/*
 * Register an "alloc" callback
 */
static void fifo_register_alloc_cb (fifo_buffer_t *_fifo,
  void (*cb) (fifo_buffer_t *fifo, void *data_cb), void *data_cb) {
  _fifo_buffer_t *fifo = (_fifo_buffer_t *)_fifo;
  int i;

  pthread_mutex_lock (&fifo->b.mutex);
  if (fifo->b.alloc_cb[0] == fbc_dummy) {
    fifo->b.alloc_cb[0] = NULL;
    fifo->b.alloc_cb_data[0] = NULL;
  }
  for (i = 0; fifo->b.alloc_cb[i]; i++)
    ;
  if (i != BUF_MAX_CALLBACKS - 1) {
    fifo->b.alloc_cb[i] = cb;
    fifo->b.alloc_cb_data[i] = data_cb;
    fifo->b.alloc_cb[i + 1] = NULL;
    fifo->b.alloc_cb_data[i+1] = (void *)(intptr_t)0;
  }
  pthread_mutex_unlock (&fifo->b.mutex);
}

/*
 * Register a "put" callback
 */
static void fifo_register_put_cb (fifo_buffer_t *_fifo,
  void (*cb) (fifo_buffer_t *fifo, buf_element_t *buf, void *data_cb), void *data_cb) {
  _fifo_buffer_t *fifo = (_fifo_buffer_t *)_fifo;
  int i;

  pthread_mutex_lock (&fifo->b.mutex);
  for (i = 0; fifo->b.put_cb[i]; i++)
    ;
  if (i != BUF_MAX_CALLBACKS - 1) {
    fifo->b.put_cb[i] = cb;
    fifo->b.put_cb_data[i] = data_cb;
    fifo->b.put_cb[i+1] = NULL;
  }
  pthread_mutex_unlock (&fifo->b.mutex);
}

/*
 * Register a "get" callback
 */
static void fifo_register_get_cb (fifo_buffer_t *_fifo,
  void (*cb) (fifo_buffer_t *fifo, buf_element_t *buf, void *data_cb), void *data_cb) {
  _fifo_buffer_t *fifo = (_fifo_buffer_t *)_fifo;
  int i;

  pthread_mutex_lock (&fifo->b.mutex);
  for (i = 0; fifo->b.get_cb[i]; i++)
    ;
  if (i != BUF_MAX_CALLBACKS - 1) {
    fifo->b.get_cb[i] = cb;
    fifo->b.get_cb_data[i] = data_cb;
    fifo->b.get_cb[i+1] = NULL;
  }
  pthread_mutex_unlock (&fifo->b.mutex);
}

/*
 * Unregister an "alloc" callback
 */
static void fifo_unregister_alloc_cb (fifo_buffer_t *_fifo,
  void (*cb) (fifo_buffer_t *fifo, void *data_cb)) {
  _fifo_buffer_t *fifo = (_fifo_buffer_t *)_fifo;
  int i,j;

  pthread_mutex_lock (&fifo->b.mutex);
  for (i = 0; fifo->b.alloc_cb[i]; i++) {
    if (fifo->b.alloc_cb[i] == cb) {
      for (j = i; fifo->b.alloc_cb[j]; j++) {
        fifo->b.alloc_cb[j] = fifo->b.alloc_cb[j+1];
        fifo->b.alloc_cb_data[j] = fifo->b.alloc_cb_data[j+1];
      }
    }
  }
  pthread_mutex_unlock (&fifo->b.mutex);
}

/*
 * Unregister a "put" callback
 */
static void fifo_unregister_put_cb (fifo_buffer_t *_fifo,
  void (*cb) (fifo_buffer_t *fifo, buf_element_t *buf, void *data_cb)) {
  _fifo_buffer_t *fifo = (_fifo_buffer_t *)_fifo;
  int i,j;

  pthread_mutex_lock (&fifo->b.mutex);
  for (i = 0; fifo->b.put_cb[i]; i++) {
    if (fifo->b.put_cb[i] == cb) {
      for (j = i; fifo->b.put_cb[j]; j++) {
        fifo->b.put_cb[j] = fifo->b.put_cb[j + 1];
        fifo->b.put_cb_data[j] = fifo->b.put_cb_data[j + 1];
      }
    }
  }
  pthread_mutex_unlock (&fifo->b.mutex);
}

/*
 * Unregister a "get" callback
 */
static void fifo_unregister_get_cb (fifo_buffer_t *_fifo,
  void (*cb) (fifo_buffer_t *fifo, buf_element_t *buf, void *data_cb)) {
  _fifo_buffer_t *fifo = (_fifo_buffer_t *)_fifo;
  int i, j;

  pthread_mutex_lock (&fifo->b.mutex);
  for (i = 0; fifo->b.get_cb[i]; i++) {
    if (fifo->b.get_cb[i] == cb) {
      for (j = i; fifo->b.get_cb[j]; j++) {
        fifo->b.get_cb[j] = fifo->b.get_cb[j + 1];
        fifo->b.get_cb_data[j] = fifo->b.get_cb_data[j + 1];
      }
    }
  }
  pthread_mutex_unlock (&fifo->b.mutex);
}

/*
 * allocate and initialize new (empty) fifo buffer
 */
fifo_buffer_t *_x_fifo_buffer_new (int num_buffers, uint32_t buf_size) {
  _fifo_buffer_t *fifo;
  union {
    be_ei_t       *beei;
    unsigned char *b;
  } multi_buffer;

  fifo = calloc (1, sizeof (*fifo));
  if (!fifo)
    return NULL;
#ifndef HAVE_ZERO_SAFE_MEM
  /* Do these first, when compiler still knows "this" is all zeroed.
   * Let it optimize away this on most systems where clear mem
   * interpretes as 0, 0f or NULL safely.
   */
  fifo->b.first                   = NULL;
  fifo->b.last                    = NULL;
  fifo->b.fifo_size               = 0;
  fifo->b.fifo_num_waiters        = 0;
  fifo->b.buffer_pool_num_waiters = 0;
  fifo->b.alloc_cb[0]             = NULL;
  fifo->b.get_cb[0]               = NULL;
  fifo->b.put_cb[0]               = NULL;
  fifo->b.alloc_cb_data[0]        = NULL;
  fifo->b.get_cb_data[0]          = NULL;
  fifo->b.put_cb_data[0]          = NULL;
#endif
  _fifo_mark_native (fifo);
  _fifo_mux_init (fifo);

  if (num_buffers <= 0)
    num_buffers = 1;
  buf_size = (buf_size + 31) & ~31u;
  /* printf ("Allocating %d buffers of %ld bytes in one chunk\n", num_buffers, (long int) buf_size); */
  multi_buffer.beei = xine_mallocz_aligned (num_buffers * (buf_size + sizeof (be_ei_t)));
  if (!multi_buffer.beei) {
    free (fifo);
    return NULL;
  }

  fifo->b.put                 = fifo_buffer_put;
  fifo->b.insert              = fifo_buffer_insert;
  fifo->b.get                 = fifo_buffer_get;
  fifo->b.tget                = fifo_buffer_tget;
  fifo->b.clear               = fifo_buffer_clear;
  fifo->b.size                = fifo_buffer_size;
  fifo->b.num_free            = fifo_buffer_num_free;
  fifo->b.data_size           = fifo_buffer_data_size;
  fifo->b.dispose             = fifo_buffer_dispose;
  fifo->b.register_alloc_cb   = fifo_register_alloc_cb;
  fifo->b.register_get_cb     = fifo_register_get_cb;
  fifo->b.register_put_cb     = fifo_register_put_cb;
  fifo->b.unregister_alloc_cb = fifo_unregister_alloc_cb;
  fifo->b.unregister_get_cb   = fifo_unregister_get_cb;
  fifo->b.unregister_put_cb   = fifo_unregister_put_cb;
  pthread_mutex_init (&fifo->b.mutex, NULL);
  pthread_cond_init (&fifo->b.not_empty, NULL);

  /* init buffer pool */
  pthread_mutex_init (&fifo->b.buffer_pool_mutex, NULL);
  pthread_cond_init (&fifo->b.buffer_pool_cond_not_empty, NULL);

  fifo->b.buffer_pool_num_free   =
  fifo->b.buffer_pool_capacity   = num_buffers;
  fifo->b.buffer_pool_buf_size   = buf_size;
  fifo->b.buffer_pool_alloc      = buffer_pool_alloc;
  fifo->b.buffer_pool_try_alloc  = buffer_pool_try_alloc;
  fifo->b.buffer_pool_size_alloc = buffer_pool_size_alloc;
  fifo->b.buffer_pool_realloc    = buffer_pool_realloc;

  fifo->b.buffer_pool_large_wait  = LARGE_NUM;

  {
    unsigned char *mem = multi_buffer.b;
    be_ei_t *beei, *end;

    fifo->b.buffer_pool_base = mem;
    multi_buffer.b += num_buffers * buf_size;
    beei = multi_buffer.beei;
    end = beei + num_buffers;
    fifo->b.buffer_pool_top  = &beei->elem;
    beei->nbufs = num_buffers;

    do {
      beei->elem.mem         = mem;
      mem                   += buf_size;
      beei->elem.max_size    = buf_size;
      beei->elem.free_buffer = buffer_pool_free;
      beei->elem.source      = fifo;
      beei->elem.extra_info  = &beei->ei;
      beei->elem.next        = &beei[1].elem;
      beei++;
    } while (beei < end);

    beei[-1].elem.next = NULL;
  }

  return &fifo->b;
}

/*
 * allocate and initialize new (empty) fifo buffer
 */
fifo_buffer_t *_x_dummy_fifo_buffer_new (int num_buffers, uint32_t buf_size) {
  fifo_buffer_t *this = _x_fifo_buffer_new (num_buffers, buf_size);
  if (this) {
    this->put    = dummy_fifo_buffer_put;
    this->insert = dummy_fifo_buffer_insert;
  }
  return this;
}

void _x_free_buf_elements (buf_element_t *head) {
  buf_element_t *here, *next;

  for (here = head; here; here = next) {
    next = here->next;
    here->next = NULL;
    here->free_buffer (here);
  }
}

