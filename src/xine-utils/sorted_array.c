/*
 * Copyright (C) 2000-2022 the xine project
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
 * rewritten by Torsten Jager <t.jager@gmx.de>
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <xine/attributes.h>
#include <xine/sorted_array.h>
#include <xine/os_types.h>

#define MIN_CHUNK_SIZE 64

/* Array internal struct */
struct xine_sarray_s {
  void                    **chunk;
  size_t                    chunk_size;
  size_t                    size;
  xine_sarray_comparator_t  comparator;
  int                     (*find) (xine_sarray_t *sarray, void *key);
  unsigned int              mode;
  unsigned int              last_add[2];
  unsigned int              first_test;
  unsigned int              same_dir;
  unsigned int              unique_test;
  unsigned int              add_here;
  struct {
    int                    *table;
    xine_sarray_hash_func_t user_get;
    void                  (*get) (xine_sarray_t *sarray, void *item);
    unsigned int            size;
    unsigned int            start;
    unsigned int            stop;
    unsigned int            last_value;
    unsigned int            value;
  }                         hash;
  void                     *default_chunk[1];
};

static void _xine_sarray_hash_get (xine_sarray_t *sarray, void *item) {
  unsigned int value = sarray->hash.user_get (item);

  if (value > sarray->hash.size - 1)
    value = sarray->hash.size - 1;
  sarray->hash.value = value;
  sarray->hash.start = sarray->hash.table[value];
  sarray->hash.stop = sarray->hash.table[value + 1];
}
  
static void _xine_sarray_hash_none (xine_sarray_t *sarray, void *item) {
  (void)item;
  sarray->hash.value = 0;
  sarray->hash.start = 0;
  sarray->hash.stop = sarray->size;
}
  
static void _xine_sarray_hash_insert (xine_sarray_t *sarray) {
  if (sarray->hash.table) {
    unsigned int u;

    for (u = sarray->hash.value + 1; u <= sarray->hash.size; u++)
      sarray->hash.table[u] += 1;
    sarray->hash.last_value = sarray->hash.value;
  }
}

static void _xine_sarray_hash_remove (xine_sarray_t *sarray, void *item) {
  if (sarray->hash.table) {
    unsigned int u;

    sarray->hash.get (sarray, item);
    for (u = sarray->hash.value + 1; u <= sarray->hash.size; u++)
      sarray->hash.table[u] -= 1;
  }
}

static int _xine_sarray_find_default (xine_sarray_t *sarray, void *key) {
  unsigned int b = sarray->hash.start, e = sarray->hash.stop, m = sarray->first_test;

  while (b != e) {
    int d = sarray->comparator (key, sarray->chunk[m]);
    if (d == 0) {
      sarray->add_here = m;
      return m; /* found */
    }
    if (d < 0)
      e = m;
    else
      b = m + 1;
    m = (b + e) >> 1;
  }
  sarray->add_here = m;
  return ~m; /* not found */
}

static int _xine_sarray_find_first (xine_sarray_t *sarray, void *key) {
  unsigned int b = sarray->hash.start, e = sarray->hash.stop, m = sarray->first_test;

  while (b != e) {
    int d = sarray->comparator (key, sarray->chunk[m]);
    if (d == 0)
      break;
    if (d < 0)
      e = m;
    else
      b = m + 1;
    m = (b + e) >> 1;
  }
  sarray->add_here = m;
  if (b == e)
    return ~m;
  e = m;
  m = (b + e) >> 1;
  while (b != e) {
    int d = sarray->comparator (key, sarray->chunk[m]);
    if (d <= 0)
      e = m;
    else
      b = m + 1;
    m = (b + e) >> 1;
  }
  sarray->add_here = b;
  return b;
}

static int _xine_sarray_find_last (xine_sarray_t *sarray, void *key) {
  unsigned int b = sarray->hash.start, e = sarray->hash.stop, m = sarray->first_test;

  while (b != e) {
    int d = sarray->comparator (key, sarray->chunk[m]);
    if (d == 0)
      break;
    if (d < 0)
      e = m;
    else
      b = m + 1;
    m = (b + e) >> 1;
  }
  sarray->add_here = m;
  if (b == e)
    return ~m;
  b = m + 1;
  m = (b + e) >> 1;
  while (b != e) {
    int d = sarray->comparator (key, sarray->chunk[m]);
    if (d < 0)
      e = m;
    else
      b = m + 1;
    m = (b + e) >> 1;
  }
  sarray->add_here = b;
  return b - 1;
}

int xine_sarray_binary_search (xine_sarray_t *sarray, void *key) {
  if (!sarray)
    return ~0; /* not found */
  sarray->hash.get (sarray, key);
  sarray->first_test = (sarray->hash.start + sarray->hash.stop) >> 1;
  return sarray->find (sarray, key);
}

static int _xine_sarray_dummy_comp (void *item1, void *item2) {
  intptr_t d = (intptr_t)item1, e = (intptr_t)item2;

  return d < e ? -1 : d > e ? 1 : 0;
}

/* Constructor */
xine_sarray_t *xine_sarray_new (size_t initial_size, xine_sarray_comparator_t comparator) {
  xine_sarray_t *new_sarray;

  if (!comparator)
    comparator = _xine_sarray_dummy_comp;
  if (initial_size < MIN_CHUNK_SIZE)
    initial_size = MIN_CHUNK_SIZE;

  new_sarray = malloc (sizeof (*new_sarray) + initial_size * sizeof (void *));
  if (!new_sarray)
    return NULL;

  new_sarray->size            = 0;
  new_sarray->last_add[0]     = 0;
  new_sarray->last_add[1]     = 0;
  new_sarray->same_dir        = 0;
  new_sarray->hash.table      = NULL;
  new_sarray->hash.user_get   = NULL;
  new_sarray->hash.start      = 0;
  new_sarray->hash.stop       = 0;
  new_sarray->hash.value      = 0;
  new_sarray->hash.last_value = 0;

  new_sarray->chunk_size = initial_size;
  new_sarray->comparator = comparator;
  new_sarray->find       = _xine_sarray_find_default;
  new_sarray->chunk      = &new_sarray->default_chunk[0];
  new_sarray->mode       = XINE_SARRAY_MODE_DEFAULT;
  new_sarray->hash.get   = _xine_sarray_hash_none;
  new_sarray->hash.size  = 1;
  new_sarray->unique_test = 0;
  new_sarray->add_here    = 0;

  return new_sarray;
}

/* Destructor */
void xine_sarray_delete (xine_sarray_t *sarray) {
  if (sarray) {
    if (sarray->chunk != &sarray->default_chunk[0])
      free (sarray->chunk);
    free (sarray->hash.table);
    free (sarray);
  }
}

void xine_sarray_set_hash (xine_sarray_t *sarray, xine_sarray_hash_func_t hash_func, unsigned int hash_size) {
  if (!sarray)
    return;
  if (sarray->hash.user_get == hash_func)
    return;

  free (sarray->hash.table);
  sarray->hash.table = NULL;
  sarray->hash.user_get = NULL;
  sarray->hash.get = _xine_sarray_hash_none;
  sarray->hash.size = 1;

  if (hash_func && (hash_size > 1) && (hash_size <= 4096) &&
    ((sarray->hash.table = calloc (1, (hash_size + 1) * sizeof (*sarray->hash.table))))) {
    sarray->hash.user_get = hash_func;
    sarray->hash.get = _xine_sarray_hash_get;
    sarray->hash.size = hash_size;
  }
}

size_t xine_sarray_size (const xine_sarray_t *sarray) {
  return sarray ? sarray->size : 0;
}

void xine_sarray_set_mode (xine_sarray_t *sarray, unsigned int mode) {
  if (sarray) {
    sarray->mode = mode;
    sarray->find = (mode & XINE_SARRAY_MODE_FIRST) ? _xine_sarray_find_first
                 : (mode & XINE_SARRAY_MODE_LAST)  ? _xine_sarray_find_last
                 : _xine_sarray_find_default;
    sarray->unique_test = (mode & XINE_SARRAY_MODE_UNIQUE) ? 1 << (sizeof (sarray->unique_test) * 8 - 1) : 0;
  }
}

void *xine_sarray_get (xine_sarray_t *sarray, unsigned int position) {
  if (sarray) {
    if (position < sarray->size)
      return sarray->chunk[position];
  }
  return NULL;
}

void xine_sarray_clear (xine_sarray_t *sarray) {
  if (sarray) {
    sarray->size = 0;
    sarray->last_add[0] = 0;
    sarray->last_add[1] = 0;
    sarray->same_dir = 0;
    if (sarray->hash.table) {
      unsigned int u;

      for (u = 0; u <= sarray->hash.size; u++)
        sarray->hash.table[u] = 0;
    }
  }
}

void xine_sarray_move_location (xine_sarray_t *sarray, void *new_ptr, unsigned int position) {
  if (sarray && (position < sarray->size)) {
    if (new_ptr) {
      sarray->chunk[position] = new_ptr;
    } else {
      void **here = sarray->chunk + position;
      unsigned int u = sarray->size - position - 1;
      while (u--) {
        here[0] = here[1];
        here++;
      }
      sarray->size--;
      sarray->last_add[0] = 0;
      sarray->last_add[1] = 0;
      sarray->same_dir = 0;
    }
  }
}

void *xine_sarray_remove (xine_sarray_t *sarray, unsigned int position) {
  void *item = NULL;

  if (sarray) {
    if (position < sarray->size) {
      void **here = sarray->chunk + position;
      unsigned int u = sarray->size - position - 1;
      item = *here;
      while (u--) {
        here[0] = here[1];
        here++;
      }
      sarray->size--;
      sarray->last_add[0] = 0;
      sarray->last_add[1] = 0;
      sarray->same_dir = 0;
      _xine_sarray_hash_remove (sarray, item);
    }
  }
  return item;
}

int xine_sarray_remove_ptr (xine_sarray_t *sarray, void *ptr) {
  if (sarray) {
    int ret;
    void **here = sarray->chunk, **end = here + sarray->size;
    *end = ptr;
    while (*here != ptr)
      here++;
    if (here >= end)
      return ~0;
    ret = here - sarray->chunk;
    end--;
    while (here < end) {
      here[0] = here[1];
      here++;
    }
    sarray->size--;
    sarray->last_add[0] = 0;
    sarray->last_add[1] = 0;
    sarray->same_dir = 0;
    _xine_sarray_hash_remove (sarray, ptr);
    return ret;
  }
  return ~0;
}

static void _xine_sarray_insert (xine_sarray_t *sarray, void *value) {
  unsigned int pos = sarray->add_here;

  if (sarray->size + 1 > sarray->chunk_size) {
    size_t new_size;
    void **new_chunk;

    new_size = 2 * (sarray->size + 1);
    if (new_size < MIN_CHUNK_SIZE)
      new_size = MIN_CHUNK_SIZE;
    if (sarray->chunk == &sarray->default_chunk[0]) {
      new_chunk = malloc ((new_size + 1) * sizeof (void *));
      if (!new_chunk)
        return;
      memcpy (new_chunk, sarray->chunk, sarray->size * sizeof (void *));
    } else {
      new_chunk = realloc (sarray->chunk, (new_size + 1) * sizeof (void *));
      if (!new_chunk)
        return;
    }
    sarray->chunk = new_chunk;
    sarray->chunk_size = new_size;
  }

  /* this database is often built from already sorted items. optimize for that. */
  {
    int d = ((int)sarray->last_add[1] - (int)sarray->last_add[0]) ^ ((int)sarray->last_add[0] - (int)pos);
    sarray->same_dir = d < 0 ? 0 : sarray->same_dir + 1;
  }
  sarray->last_add[1] = sarray->last_add[0];
  sarray->last_add[0] = pos;

  {
    unsigned int u = sarray->size - pos;
    if (!u) {
      sarray->chunk[sarray->size++] = value;
    } else {
      void **here = sarray->chunk + sarray->size;
      do {
        here[0] = here[-1];
        here--;
      } while (--u);
      here[0] = value;
      sarray->size++;
    }
  }

  _xine_sarray_hash_insert (sarray);
}

int xine_sarray_add (xine_sarray_t *sarray, void *value) {
  if (sarray) {
    unsigned int pos2;

    sarray->hash.get (sarray, value);
    if ((sarray->same_dir >= 2) && (sarray->hash.value == sarray->hash.last_value))
      sarray->first_test = sarray->last_add[0];
    else
      sarray->first_test = (sarray->hash.start + sarray->hash.stop) >> 1;
    pos2 = ~sarray->find (sarray, value);
    if (pos2 & sarray->unique_test)
      return pos2;
    _xine_sarray_insert (sarray, value);
    return sarray->add_here;
  }
  return 0;
}
