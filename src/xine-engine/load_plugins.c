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
 *
 * Load input/demux/audio_out/video_out/codec plugins
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#ifdef HAVE_DIRENT_H
#include <dirent.h>
#endif
#include <dlfcn.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <signal.h>

#include <basedir.h>

#define LOG_MODULE "load_plugins"
#define LOG_VERBOSE

/*
#define LOG
#define DEBUG
*/

#define XINE_ENABLE_EXPERIMENTAL_FEATURES 1

/* save 1 lookup per entry by caching entry pointers not keys. */
#define FAST_SCAN_PLUGINS

#include <xine/xine_internal.h>
#include <xine/xine_plugin.h>
#include <xine/plugin_catalog.h>
#include <xine/demux.h>
#include <xine/input_plugin.h>
#include <xine/video_out.h>
#include <xine/post.h>
#include <xine/xine_module.h>
#include <xine/metronom.h>
#include <xine/configfile.h>
#include <xine/xineutils.h>
#include <xine/compat.h>

#include "xine_private.h"

#ifdef XINE_MAKE_BUILTINS
#include "builtins.h"
#endif

#if 0

static char *plugin_name;

#if DONT_CATCH_SIGSEGV

#define install_segv_handler()
#define remove_segv_handler()

#else

void (*old_segv_handler)(int);

static void segv_handler (int hubba) {
  printf ("\nload_plugins: Initialization of plugin '%s' failed (segmentation fault).\n",plugin_name);
  printf ("load_plugins: You probably need to remove the offending file.\n");
  printf ("load_plugins: (This error is usually due an incorrect plugin version)\n");
  _x_abort();
}

static void install_segv_handler(void){
  old_segv_handler = signal (SIGSEGV, segv_handler);
}

static void remove_segv_handler(void){
  signal (SIGSEGV, old_segv_handler );
}

#endif
#endif /* 0 */

#define CACHE_CATALOG_VERSION 5
#define CACHE_CATALOG_VERSION_STR "5"

#define MAX_DUPL_CFG_ENTRIES 256

#define __Max(a,b) ((a) > (b) ? (a) : (b))
static const uint8_t plugin_iface_versions[__Max(PLUGIN_TYPE_MAX, PLUGIN_XINE_MODULE) + 1] = {
  [PLUGIN_INPUT]         = INPUT_PLUGIN_IFACE_VERSION,
  [PLUGIN_DEMUX]         = DEMUXER_PLUGIN_IFACE_VERSION,
  [PLUGIN_AUDIO_DECODER] = AUDIO_DECODER_IFACE_VERSION,
  [PLUGIN_VIDEO_DECODER] = VIDEO_DECODER_IFACE_VERSION,
  [PLUGIN_SPU_DECODER]   = SPU_DECODER_IFACE_VERSION,
  [PLUGIN_AUDIO_OUT]     = AUDIO_OUT_IFACE_VERSION,
  [PLUGIN_VIDEO_OUT]     = VIDEO_OUT_DRIVER_IFACE_VERSION,
  [PLUGIN_POST]          = POST_PLUGIN_IFACE_VERSION,
  [PLUGIN_XINE_MODULE]   = XINE_MODULE_IFACE_VERSION,
};

typedef union {
  vo_info_t      vo_info;
  ao_info_t      ao_info;
  demuxer_info_t demuxer_info;
  input_info_t   input_info;
  decoder_info_t decoder_info;
  post_info_t    post_info;
  xine_module_info_t module_info;
} all_info_t;

typedef struct fat_node_st {
  plugin_node_t  node;
  plugin_info_t  info[2];
  all_info_t     ainfo;
  plugin_file_t  file;
#define FAT_NODE_FLAG_PROBE_CLASS 1
  uint32_t       flags;
  struct fat_node_st *nextplugin, *lastplugin;
  xine_t        *xine;
  uint32_t       supported_types[1];
} fat_node_t;
/* effectively next:
  uint32_t       supported_types[num_supported_types];
  char           id[idlen + 1];
  xine_fast_string_t filename[fnlen + n];
*/

#define IS_FAT_NODE(_node) (_node->node.info == &_node->info[0])

static void _fat_node_init (fat_node_t *node) {
#ifdef HAVE_ZERO_SAFE_MEM
  memset (node, 0, sizeof (*node));
#else
  node->node.file              = NULL;
  node->node.info              = NULL;
  node->node.plugin_class      = NULL;
  node->node.config_entry_list = NULL;
  node->node.ref               = 0;
  node->node.priority          = 0;
  node->info[0].type         = 0;
  node->info[0].API          = 0;
  node->info[0].id           = NULL;
  node->info[0].version      = 0;
  node->info[0].special_info = NULL;
  node->info[0].init         = NULL;
  node->info[1].type         = 0;
  node->ainfo.decoder_info.supported_types = NULL;
  node->ainfo.decoder_info.priority        = 0;
  node->flags            = 0;
  node->file.filename    = NULL;
  node->file.filesize    = 0;
  node->file.filemtime   = 0;
  node->file.lib_handle  = NULL;
  node->file.ref         = 0;
  node->file.no_unload   = 0;
  node->supported_types[0] = 0;
  node->nextplugin         = NULL;
  node->xine               = NULL;
#endif
  node->lastplugin = node;
}

static int _fat_node_file_cmp (void *a_gen, void *b_gen) {
  fat_node_t *a = a_gen, *b = b_gen;
  if (a->file.filesize != b->file.filesize)
    return a->file.filesize < b->file.filesize ? -1 : 1;
  if (a->file.filemtime != b->file.filemtime)
    return a->file.filemtime < b->file.filemtime ? -1 : 1;
  /* 2 files with same size and time are extremely rare.
   * however, same file (with long file name) is frequent
   * when scanning the plugin cache,
   * and thie alwayy uses fat_node_t. */
  return xine_fast_string_cmp (a->file.filename, b->file.filename);
}

static const uint8_t tab_tolower[256] = {
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
   16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
   32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
   48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
   64,'a','b','c','d','e','f','g','h','i','j','k','l','m','n','o',
  'p','q','r','s','t','u','v','w','x','y','z', 91, 92, 93, 94, 95,
   96,'a','b','c','d','e','f','g','h','i','j','k','l','m','n','o',
  'p','q','r','s','t','u','v','w','x','y','z',123,124,125,126,127,
  128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,
  144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,
  160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,
  176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,
  192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,
  208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,
  224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,
  240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255
};

static const uint8_t tab_mime[256] = {
  ['\0'] = 1,
  ['\r'] = 2,
  ['\n'] = 2,
  ['\t'] = 2,
  [' ']  = 2,
  [':']  = 4,
  [';']  = 8
};

static void _mime_set (uint8_t *buf, size_t bsize, const char *needle) {
  const uint8_t *n = (const uint8_t *)needle;
  uint8_t *e = buf + bsize - 1, *q = buf;

  if (!n) {
    buf[0] = 0;
    return;
  }

  /* skip leading needle space */
  while (tab_mime[*n] & 2)
    n++;
  /* pattern = lowercase needle */
  while ((q < e) && !(tab_mime[*n] & (1 | 2 | 4 | 8)))
    *q++ = tab_tolower[*n++];
  e = q;
  /* safe brake */
  *e = 'A';
}

static int _mime_find (uint8_t *buf, const char *haystack) {
  const uint8_t *h = (const uint8_t *)haystack;

  if (!buf[0] || !h)
    return -1;

  while (1) {
    const uint8_t *n;
    int o;
    while (tab_mime[*h] & 2)
      h++;
    o = h - (const uint8_t *)haystack;
    n = buf;
    while (*n == tab_tolower[*h])
      n++, h++;
    if ((*n == 'A') && (tab_mime[*h] & (1 | 2 | 4 | 8)))
      return o;
    while (!(tab_mime[*h] & (1 | 8)))
      h++;
    if (!*h)
      break;
    h++;
  }
  return -1;
}

static const char * const *_build_list_typed_plugins (xine_t *xine, int type, uint64_t mask) {
  plugin_catalog_t *catalog = xine->plugin_catalog;
  xine_sarray_t    *a, *list;
  int               list_id, list_size, i;

  pthread_mutex_lock (&catalog->lock);
  list = catalog->plugin_lists[type - 1];
  list_size = xine_sarray_size (list);
  /* Note const char * --> void * is ok here as the strings are never written to. */
  a = xine_sarray_new (list_size, (xine_sarray_comparator_t)strcmp);
  if (!a) {
    catalog->ids[0] = NULL;
    pthread_mutex_unlock (&catalog->lock);
    return catalog->ids;
  }
  xine_sarray_set_mode (a, XINE_SARRAY_MODE_UNIQUE);

  if (type == PLUGIN_VIDEO_OUT) {
    for (list_id = 0, i = 0; list_id < list_size; list_id++) {
      plugin_node_t *node = xine_sarray_get (list, list_id);
      const vo_info_t *info = (const vo_info_t *)node->info->special_info;
      if (info) {
        if (!(mask & ((uint64_t)1 << info->visual_type)))
          continue;
      }
      /* add only unique ids to the list */
      if (xine_sarray_add (a, (void *)node->info->id) >= 0)
        catalog->ids[i++] = node->info->id;
    }
  } else {
    for (list_id = 0, i = 0; list_id < list_size; list_id++) {
      plugin_node_t *node = xine_sarray_get (list, list_id);
      /* add only unique ids to the list */
      if (xine_sarray_add (a, (void *)node->info->id) >= 0)
        catalog->ids[i++] = node->info->id;
    }
  }

  catalog->ids[i] = NULL;
  pthread_mutex_unlock (&catalog->lock);

  xine_sarray_delete (a);
  return catalog->ids;
}

static void inc_file_ref(plugin_file_t *file) {
/* all users do dereference "file" before calling this
  _x_assert(file); */
  file->ref++;
}

static void dec_file_ref(plugin_file_t *file) {
/* all users do test "file" before calling this
  _x_assert(file); */
  _x_assert(file->ref > 0);

  file->ref--;
  lprintf("file %s referenced %d time(s)\n", file->filename, file->ref);
}

static void inc_node_ref(plugin_node_t *node) {
/* all users do dereference "node" before calling this
  _x_assert(node); */
  node->ref++;
}

static void dec_node_ref(plugin_node_t *node) {
/* all users do test "node" before calling this
  _x_assert(node); */
  _x_assert(node->ref > 0);

  node->ref--;
  lprintf("node %s referenced %d time(s)\n", node->info->id, node->ref);
}

#ifndef FAST_SCAN_PLUGINS
static void _free_string_list(xine_list_t **plist) {
  xine_list_t *list = *plist;

  if (list) {
    char *key;
    xine_list_iterator_t ite = NULL;
    while ((key = xine_list_next_value (list, &ite)))
      free (key);
    xine_list_delete(list);
    *plist = NULL;
  }
}
#endif

/*
 * plugin list/catalog management functions
 */

static void map_decoder_list (xine_t *this,
			      xine_sarray_t *decoder_list,
			      plugin_node_t *decoder_map[DECODER_MAX][PLUGINS_PER_TYPE]) {
  int i;
  int list_id, list_size;

  /* init */
  for (i = 0; i < DECODER_MAX; i++) {
    decoder_map[i][0] = NULL;
  }

  /*
   * map decoders
   */
  list_size = xine_sarray_size(decoder_list);
  /* this is sorted by falling priorities */
  for (list_id = 0; list_id < list_size; list_id++) {

    plugin_node_t *node = xine_sarray_get(decoder_list, list_id);
    const uint32_t *type = ((const decoder_info_t *)node->info->special_info)->supported_types;

    lprintf ("mapping decoder %s\n", node->info->id);

    while (type && (*type)) {

      int pos;
      int streamtype = ((*type)>>16) & 0xFF;

      lprintf ("load_plugins: decoder handles stream type %02x, priority %d\n", streamtype, node->priority);

      /* find the right place based on the priority */
      for (pos = 0; pos < PLUGINS_PER_TYPE; pos++) {
        if (!decoder_map[streamtype][pos])
          break;
      }

      if ( pos == PLUGINS_PER_TYPE ) {
	xine_log (this, XINE_LOG_PLUGIN,
		  _("map_decoder_list: no space for decoder, skipped.\n"));
	type++;
	continue;
      }

      /* shift the decoder list for this type by one to make room for new decoder */
      if (pos < PLUGINS_PER_TYPE - 1)
        decoder_map[streamtype][pos + 1] = NULL;

      /* insert new decoder */
      decoder_map[streamtype][pos] = node;

      lprintf("decoder inserted in decoder map at %d\n", pos);

      type++;
    }
  }
}

static void map_decoders (xine_t *this) {

  plugin_catalog_t *catalog = this->plugin_catalog;

  lprintf ("map_decoders\n");

  /*
   * map audio decoders
   */
  map_decoder_list(this, catalog->plugin_lists[PLUGIN_AUDIO_DECODER - 1], catalog->audio_decoder_map);
  map_decoder_list(this, catalog->plugin_lists[PLUGIN_VIDEO_DECODER - 1], catalog->video_decoder_map);
  map_decoder_list(this, catalog->plugin_lists[PLUGIN_SPU_DECODER - 1], catalog->spu_decoder_map);

}

/* Decoder priority callback */
static void _decoder_priority_cb (void *data, xine_cfg_entry_t *cfg) {
  /* sort decoders by priority */
  xine_sarray_t *list;
  int type;
  fat_node_t *node = data;

  if (!node)
    return;

  type = node->info->type & PLUGIN_TYPE_MASK;
  list = node->xine->plugin_catalog->plugin_lists[type - 1];

  if (xine_sarray_remove_ptr (list, node) == ~0)
    /* callback was triggered before the entry was added to plugin list */
    return;
  {
    int user_prio = cfg->num_value;
    /* user given priorities should definitely override defaults, so multiply them */
    node->node.priority = user_prio ? user_prio * 100 : node->ainfo.decoder_info.priority;
  }
  xine_sarray_add (list, node);
  map_decoder_list (node->xine, list,
      type == PLUGIN_AUDIO_DECODER ? node->xine->plugin_catalog->audio_decoder_map
    : type == PLUGIN_VIDEO_DECODER ? node->xine->plugin_catalog->video_decoder_map
    : node->xine->plugin_catalog->spu_decoder_map);
}


static plugin_file_t *_insert_file (xine_list_t *list, const char *filename,
  const struct stat *statbuffer, void *lib, uint32_t name_len) {
  plugin_file_t *entry;
  char *p;

  /* create the file entry */
  p = malloc (sizeof (*entry) + name_len + 1);
  if (!p)
    return NULL;
  entry = (plugin_file_t *)p;

  p += sizeof (*entry);
  entry->filename = p;
  xine_small_memcpy (p, filename, name_len + 1);

  entry->filesize  = statbuffer->st_size;
  entry->filemtime = statbuffer->st_mtime;
  entry->lib_handle = lib;
  entry->ref = 0;
  entry->no_unload = 0;

  xine_list_push_back (list, entry);
  return entry;
}

static int _insert_node (xine_t *this, plugin_file_t *file, fat_node_t *node_cache, const plugin_info_t *info, uint32_t flags) {
  fat_node_t       *entry;
  const all_info_t *ainfo;
  unsigned int num_supported_types = 0;
  unsigned int plugin_type = info->type & PLUGIN_TYPE_MASK;
  int          left;
  const char  *what;

  /* sanity test */
  left = PLUGIN_MAX - this->plugin_catalog->plugin_count;
  do {
    unsigned int flag;
    if ((plugin_type <= 0) || ((plugin_type > PLUGIN_TYPE_MAX) && plugin_type != PLUGIN_XINE_MODULE)) {
      if (file)
        xine_log (this, XINE_LOG_PLUGIN,
          _("load_plugins: unknown plugin type %d in %s\n"), plugin_type, file->filename);
      else
        xine_log (this, XINE_LOG_PLUGIN,
          _("load_plugins: unknown statically linked plugin type %d\n"), plugin_type);
      return 1;
    }
    if (!info->id) {
      what = "id";
      break;
    }
    if (info->API != plugin_iface_versions[plugin_type]) {
      xine_log (this, XINE_LOG_PLUGIN,
        _("load_plugins: ignoring plugin %s, wrong iface version %d (should be %d)\n"),
        info->id, info->API, plugin_iface_versions[plugin_type]);
      return 1;
    }
    if (!node_cache && !info->init) {
      what = "init";
      break;
    }
    ainfo = info->special_info;
    flag = 1u << plugin_type;
    if (flag & ((1u << PLUGIN_VIDEO_OUT) | (1u << PLUGIN_AUDIO_OUT) | (1u << PLUGIN_POST) |
                (1u << PLUGIN_AUDIO_DECODER) | (1u << PLUGIN_VIDEO_DECODER) |
                (1u << PLUGIN_SPU_DECODER) | (1u << PLUGIN_XINE_MODULE))) {
      if (!ainfo) {
        what = "special_info";
        break;
      }
    }
    if (flag & ((1u << PLUGIN_AUDIO_DECODER) | (1u << PLUGIN_VIDEO_DECODER) | (1u << PLUGIN_SPU_DECODER))) {
      if (!ainfo->decoder_info.supported_types) {
        what = "supported_types";
        break;
      }
      if (!node_cache) {
        while (ainfo->decoder_info.supported_types[num_supported_types])
          num_supported_types++;
      }
      if (left > DECODER_MAX - this->plugin_catalog->decoder_count)
        left = DECODER_MAX - this->plugin_catalog->decoder_count;
    }
    what = NULL;
  } while (0);
  if (what) {
    xine_log (this, XINE_LOG_PLUGIN,
      "load_plugins: plugin %s from %s is broken: %s = NULL\n",
      info->id ? info->id : "??", file ? file->filename : "user", what);
    return 1;
  }
  if (left <= 0) {
    if (file)
      xine_log (this, XINE_LOG_PLUGIN,
        _("load_plugins: plugin limit reached, %s could not be loaded\n"), file->filename);
    else
      xine_log (this, XINE_LOG_PLUGIN,
        _("load_plugins: plugin limit reached, static plugin could not be loaded\n"));
    return 2;
  }

  /* get target */
  if (node_cache) {
    entry = node_cache;
  } else {
    size_t idlen = xine_find_byte (info->id, 0) + 1;
    char *q;
    entry = malloc (sizeof (*entry) + num_supported_types * sizeof (uint32_t) + idlen);
    if (!entry)
      return 2;
    _fat_node_init (entry);
    entry->node.info  = &entry->info[0];
    entry->info[0]    = *info;
    entry->flags      = flags;
    q = (char *)entry + sizeof (*entry) + num_supported_types * sizeof (uint32_t);
    entry->info[0].id = q;
    xine_small_memcpy (q, info->id, idlen);
  }
  entry->lastplugin = entry;
  entry->xine       = this;
  entry->node.file  = file;

  /* type specific stuff */
  switch (plugin_type) {

  case PLUGIN_VIDEO_OUT:
    entry->node.priority = entry->ainfo.vo_info.priority = ainfo->vo_info.priority;
    entry->ainfo.vo_info.visual_type = ainfo->vo_info.visual_type;
    entry->flags &= ~FAT_NODE_FLAG_PROBE_CLASS;
    break;

  case PLUGIN_AUDIO_OUT:
    entry->node.priority = entry->ainfo.ao_info.priority = ainfo->ao_info.priority;
    entry->flags &= ~FAT_NODE_FLAG_PROBE_CLASS;
    break;

  case PLUGIN_AUDIO_DECODER:
  case PLUGIN_VIDEO_DECODER:
  case PLUGIN_SPU_DECODER:
    if (num_supported_types)
      memcpy (&entry->supported_types[0], ainfo->decoder_info.supported_types, (num_supported_types + 1) * sizeof (uint32_t));
    entry->ainfo.decoder_info.supported_types = &entry->supported_types[0];
    entry->ainfo.decoder_info.priority = ainfo->decoder_info.priority;

    {
      /* cnfig does dup all strings. no need to keep them here. */
      xine_private_t *_xine = (xine_private_t *)this;
      char key[128], desc[256];
      int user_prio;
      memcpy (key, "engine.decoder_priorities.", 26);
      strlcpy (key + 26, info->id, sizeof (key) - 26);
      snprintf (desc, sizeof (desc) - 1, _("priority for %s decoder"), info->id);
      user_prio = this->config->register_num (this->config, key, 0, desc,
        _xine->strings.decoder_pri_help, 20, _decoder_priority_cb, entry);
      /* reset priority on old config files */
      if (this->config->current_version < 1) {
        this->config->update_num (this->config, key, 0);
        user_prio = 0;
      }
      entry->node.priority = user_prio ? user_prio * 100 : entry->ainfo.decoder_info.priority;
    }
    this->plugin_catalog->decoder_count++;
    break;

  case PLUGIN_POST:
    entry->ainfo.post_info.type = ainfo->post_info.type;
    break;

  case PLUGIN_DEMUX:
    if (ainfo) {
      entry->node.priority = entry->ainfo.demuxer_info.priority = ainfo->demuxer_info.priority;
      lprintf("demux: %s, priority: %d\n", info->id, entry->node.priority);
    } else {
      xprintf(this, XINE_VERBOSITY_LOG,
              _("load_plugins: demuxer plugin %s does not provide a priority,"
                " xine-lib will use the default priority.\n"),
              info->id);
      entry->node.priority = entry->ainfo.demuxer_info.priority = 0;
    }
    break;

  case PLUGIN_INPUT:
    if (ainfo) {
      entry->node.priority = entry->ainfo.input_info.priority = ainfo->input_info.priority;
      lprintf("input: %s, priority: %d\n", info->id, entry->node.priority);
    } else {
      xprintf(this, XINE_VERBOSITY_LOG,
              _("load_plugins: input plugin %s does not provide a priority,"
                " xine-lib will use the default priority.\n"),
              info->id);
      entry->node.priority = entry->ainfo.input_info.priority = 0;
    }
    break;

  case PLUGIN_XINE_MODULE:
    entry->node.priority = ainfo->module_info.priority;
    entry->ainfo.module_info = ainfo->module_info;
    entry->flags &= ~FAT_NODE_FLAG_PROBE_CLASS;
    break;
  }
  entry->info[0].special_info = &entry->ainfo;

  if (file && (info->type & PLUGIN_NO_UNLOAD)) {
    file->no_unload = 1;
  }

  if (plugin_type == PLUGIN_XINE_MODULE)
    xine_sarray_add (this->plugin_catalog->modules_list, &entry->node);
  else
    xine_sarray_add (this->plugin_catalog->plugin_lists[plugin_type - 1], &entry->node);
  this->plugin_catalog->plugin_count++;
  return 0;
}


static int _plugin_node_comparator(void *a, void *b) {
  const plugin_node_t *node_a = (const plugin_node_t *)a;
  const plugin_node_t *node_b = (const plugin_node_t *)b;

  return (node_a->priority < node_b->priority) - (node_a->priority > node_b->priority);
}

/* xine-ui simply makes a user config enum from post plugin list, and assumes the first one
 * as the default. This effectively becomes a random choice, depending on the presence of
 * other plugins, and of the order the file system returns them. So lets sort them by name
 * as well here. */
static int _post_plugin_node_comparator (void *a, void *b) {
  const plugin_node_t *node_a = (const plugin_node_t *)a;
  const plugin_node_t *node_b = (const plugin_node_t *)b;

  if (node_a->priority != node_b->priority)
    return node_a->priority < node_b->priority ? 1 : -1;
  return strcmp (node_a->info->id, node_b->info->id);
}

static plugin_catalog_t *_new_catalog(void){
  plugin_catalog_t *catalog;

  catalog = calloc (1, sizeof (plugin_catalog_t));
  if (catalog) {
    int i;
    for (i = 0; i < PLUGIN_TYPE_MAX; i++) {
      catalog->plugin_lists[i] = xine_sarray_new (0,
        i == PLUGIN_POST - 1 ? _post_plugin_node_comparator : _plugin_node_comparator);
      if (!catalog->plugin_lists[i])
        break;
    }
    if (i == PLUGIN_TYPE_MAX) {
      catalog->cache_list = xine_sarray_new (0, _fat_node_file_cmp);
      if (catalog->cache_list) {
        xine_sarray_set_mode (catalog->cache_list, XINE_SARRAY_MODE_UNIQUE);
        catalog->file_list = xine_list_new ();
        if (catalog->file_list) {
          catalog->modules_list = xine_sarray_new (0, _plugin_node_comparator);
          if (catalog->modules_list) {
          pthread_mutexattr_t attr;
          pthread_mutexattr_init (&attr);
          pthread_mutexattr_settype (&attr, PTHREAD_MUTEX_RECURSIVE);
          pthread_mutex_init (&catalog->lock, &attr);
          pthread_mutexattr_destroy (&attr);
          return catalog;
          }
          xine_list_delete (catalog->file_list);
        }
        xine_sarray_delete (catalog->cache_list);
      }
    }
    for (--i; i >= 0; i--)
      xine_sarray_delete (catalog->plugin_lists[i]);
    free (catalog);
  }
  return NULL;
}

static void _register_plugins_internal (xine_t *this, plugin_file_t *file,
  fat_node_t *node_cache, const plugin_info_t *info, uint32_t flags) {
  /* user supplied xine_register_plugins () */
  static const char * const st_names[10] = {
    "user/none",
    "user/input",
    "user/demux",
    "user/audio_decoder",
    "user/video_decoder",
    "user/spu_decoder",
    "user/audio_out",
    "user/video_out",
    "user/post",
    "user"
  };
  const char * const *names = st_names;
#ifdef XINE_MAKE_BUILTINS
  static const char * const builtin_names[10] = {
    "libxine/builtins/none",
    "libxine/builtins/input",
    "libxine/builtins/demux",
    "libxine/builtins/audio_decoder",
    "libxine/builtins/video_decoder",
    "libxine/builtins/spu_decoder",
    "libxine/builtins/audio_out",
    "libxine/builtins/video_out",
    "libxine/builtins/post",
    "libxine/builtins"
  };
  if (info == xine_builtin_plugin_info)
    names = builtin_names;
#endif
/* we had worse NOPs before ;-)
  _x_assert(info); */

  while ( info && info->type != PLUGIN_NONE ) {
    fat_node_t *cache_next = node_cache ? node_cache->nextplugin : NULL;
    const char *fn;
    int status;
    if (file) {
      fn = file->filename;
    } else {
      int n = info->type & PLUGIN_TYPE_MASK;
      if (n > 9)
        n = 9;
      fn = names[n];
    }
    xine_log (this, XINE_LOG_PLUGIN, _("load_plugins: plugin %s:%s found\n"), fn, info->id);
    status = _insert_node (this, file, node_cache, info, flags);
    /* get next info */
    if (file && !file->lib_handle) {
      lprintf("get cached info\n");
      if (status)
        free (node_cache);
      node_cache = cache_next;
      info = node_cache ? node_cache->node.info : NULL;
    } else {
      info++;
    }
  }
}

void xine_register_plugins(xine_t *self, const plugin_info_t *info) {
  if (self)
    _register_plugins_internal (self, NULL, NULL, info, 0);
}

/*
 * First stage plugin loader (catalog builder)
 *
 ***************************************************************************/

/* NOTE: path actually is a xine_fast_string_t *. */
static void collect_plugins (xine_t *this, char *path, char *stop, char *pend) {

  char          *adds[5];
  DIR           *dirs[5];
  struct stat    statbuf;
  int            level;

  lprintf ("collect_plugins in %s\n", path);

  /* we need a dir to start */
  if (stat (path, &statbuf))
    return;
  if (!S_ISDIR (statbuf.st_mode))
    return;

  adds[0] = stop;
  dirs[0] = NULL;
  level   = 0;
  while (1) {
    struct dirent *dent;

    /* enter dir */
    if (!dirs[level]) {
      dirs[level] = opendir (path);
      if (!dirs[level]) {
        xine_log (this, XINE_LOG_PLUGIN,
          _("load_plugins: skipping unreadable plugin directory %s.\n"), path);
        level--;
        if (level < 0)
          break;
        continue;
      }
    }

    /* get entry */
    dent = readdir (dirs[level]);
    if (!dent) {
      closedir (dirs[level]);
      level--;
      if (level < 0)
        break;
      continue;
    }

    {
      void                *lib   = NULL;
      const plugin_info_t *info  = NULL;
      fat_node_t          *fatn_found;
      char                *part  = adds[level], *q;

      *part++ = '/';
      q = part + strlcpy (part, dent->d_name, pend - part);
      if (q >= pend)
        continue;

      if (stat (path, &statbuf)) {
        xine_log (this, XINE_LOG_PLUGIN, _("load_plugins: unable to stat %s\n"), path);
        continue;
      }
      switch (statbuf.st_mode & S_IFMT) {

	case S_IFREG:
	  /* regular file, ie. plugin library, found => load it */

	  /* this will fail whereever shared libs are called *.dll or such
	   * better solutions:
           * a) don't install .la files on user's system
           * b) also cache negative hits, ie. files that failed to dlopen()
	   */
#if defined(__hpux)
          if (!strstr (part, ".sl")
#elif defined(__CYGWIN__) || defined(WIN32)
          if (!strstr (part, ".dll") || strstr (part, ".dll.a")
#else
          if (!strstr (part, ".so")
#endif
#ifdef HOST_OS_DARWIN
             && !strcasestr (part, ".xineplugin")
#endif
            )
	    break;

	  lib = NULL;

	  /* get the first plugin_info_t */
          {
            fat_node_t fatn_try;
            int index;
            xine_fast_string_set (path, NULL, q - path);
            fatn_try.file.filename = path;
            fatn_try.file.filesize = statbuf.st_size;
            fatn_try.file.filemtime = statbuf.st_mtime;
            index = xine_sarray_binary_search (this->plugin_catalog->cache_list, &fatn_try);
            if (index >= 0) {
              fatn_found = xine_sarray_get (this->plugin_catalog->cache_list, index);
              xine_sarray_remove (this->plugin_catalog->cache_list, index);
            } else {
              fatn_found = NULL;
            }
          }
          info = fatn_found ? fatn_found->node.info : NULL;
#ifdef LOG
	  if( info )
            printf ("load_plugins: using cached %s\n", path);
	  else
            printf ("load_plugins: %s not cached\n", path);
#endif

          if (!info && (lib = dlopen (path, RTLD_LAZY | RTLD_GLOBAL)) == NULL) {
	    const char *error = dlerror();
	    /* too noisy -- but good to catch unresolved references */
            xprintf (this, XINE_VERBOSITY_LOG,
              _("load_plugins: cannot open plugin lib %s:\n%s\n"), path, error);

	  } else {

	    if (info || (info = dlsym(lib, "xine_plugin_info"))) {
	      plugin_file_t *file;

              file = _insert_file (this->plugin_catalog->file_list, path, &statbuf, lib, q - path);
              if (file) {
                _register_plugins_internal (this, file, fatn_found, info, fatn_found ? 0 : FAT_NODE_FLAG_PROBE_CLASS);
              } else {
                if (lib != NULL)
                  dlclose(lib);
              }
	    }
	    else {
	      const char *error = dlerror();

              xine_log (this, XINE_LOG_PLUGIN,
                _("load_plugins: can't get plugin info from %s:\n%s\n"), path, error);
              dlclose(lib);
	    }
	  }
	  break;
	case S_IFDIR:

	  /* unless ".", "..", ".hidden" or vidix driver dirs */
          if ((part[0] != '.') && strcmp (part, "vidix")) {
            if (level < 4) {
              level++;
              adds[level] = q;
              dirs[level] = NULL;
            }
	  }
      } /* switch */
    }
  } /* while */
} /* collect_plugins */

/*
 * generic 2nd stage plugin loader
 */

static inline int _plugin_info_equal(const plugin_info_t *a,
                                     const plugin_info_t *b) {
  if (a->type != b->type ||
      a->API != b->API ||
      strcasecmp(a->id, b->id) ||
      a->version != b->version)
    return 0;

  switch (a->type & PLUGIN_TYPE_MASK) {
    case PLUGIN_VIDEO_OUT:
      /* FIXME: Could special_info be NULL? */
      if (a->special_info && b->special_info) {
        return (((const vo_info_t*)a->special_info)->visual_type ==
                ((const vo_info_t*)b->special_info)->visual_type);
      }
      /* if special info is missing, plugin file is broken ... */
      return 0; /* skip it */
    case PLUGIN_XINE_MODULE:
      if (a->special_info && b->special_info) {
        return !strcmp(((const xine_module_info_t*)a->special_info)->type,
                       ((const xine_module_info_t*)b->special_info)->type);
      }
      return !(!a->special_info - !b->special_info);
    default:
      break;
  }

  return 1;
}

#ifdef FAST_SCAN_PLUGINS
typedef struct {
  config_values_t *v;
  plugin_node_t   *node;
  cfg_entry_t     *cfg_entry;
} new_entry_data_t;
#else
static void _attach_entry_to_node (plugin_node_t *node, char *key) {

  if (!node->config_entry_list) {
    node->config_entry_list = xine_list_new();
  }

  xine_list_push_back(node->config_entry_list, key);
}
#endif

/*
 * This callback is called by the config entry system when a plugin register a
 * new config entry.
 */
static void _new_entry_cb (void *user_data, xine_cfg_entry_t *entry) {
#ifdef FAST_SCAN_PLUGINS
  new_entry_data_t *d = user_data;
  if (!d->node->config_entry_list)
    d->node->config_entry_list = xine_list_new ();
  if (d->node->config_entry_list && d->v->cur && !xine_list_find (d->node->config_entry_list, d->v->cur))
    d->cfg_entry = d->v->cur, xine_list_push_back (d->node->config_entry_list, d->v->cur);
  (void)entry;
#else
  plugin_node_t *node = (plugin_node_t *)user_data;

  _attach_entry_to_node(node, strdup(entry->key));
#endif
}

static int _load_plugin_class(xine_t *this,
			      plugin_node_t *node,
                              const void *data) {
  if (node->file) {
    const char *filename = node->file->filename;
    const plugin_info_t *target = node->info;
    const plugin_info_t *info;
    void *lib;

    /* load the dynamic library if needed */
    if (!node->file->lib_handle) {
      lprintf("dlopen %s\n", filename);
      if ((lib = dlopen (filename, RTLD_LAZY | RTLD_GLOBAL)) == NULL) {
	const char *error = dlerror();

	xine_log (this, XINE_LOG_PLUGIN,
		  _("load_plugins: cannot (stage 2) open plugin lib %s:\n%s\n"), filename, error);
	return 0;
      } else {
	node->file->lib_handle = lib;
      }
    } else {
      lprintf("%s already loaded\n", filename);
    }

    if ((info = dlsym(node->file->lib_handle, "xine_plugin_info"))) {
      /* TODO: use sigsegv handler */
      while (info->type != PLUGIN_NONE) {
	if (_plugin_info_equal(info, target)) {
          config_values_t *config = this->config;
#ifdef FAST_SCAN_PLUGINS
          new_entry_data_t d;
          d.v = config;
          d.node = node;
#endif
	  /* the callback is called for each entry registered by this plugin */
          lprintf("plugin init %s\n", node->info->id);
          if (info->init) {
#ifdef FAST_SCAN_PLUGINS
            config->set_new_entry_callback (config, _new_entry_cb, &d);
#else
            config->set_new_entry_callback (config, _new_entry_cb, node);
#endif
            node->plugin_class = info->init(this, data);
            config->unset_new_entry_callback (config);
          }

	  if (node->plugin_class) {
	    inc_file_ref(node->file);
	    return 1;
	  } else {
	    return 0;
	  }
	}
	info++;
      }
      lprintf("plugin not found\n");

    } else {
      xine_log (this, XINE_LOG_PLUGIN,
		_("load_plugins: Yikes! %s doesn't contain plugin info.\n"), filename);
    }
  } else {
    /* statically linked plugin */
    lprintf("statically linked plugin\n");
    if (node->info->init) {
      node->plugin_class = node->info->init(this, data);
      return 1;
    }
  }
  return 0; /* something failed if we came here... */
}

static void _dispose_plugin_class(plugin_node_t *node) {

  if (node->plugin_class) {
    void *cls = node->plugin_class;

    _x_assert(node->info);
    /* dispose of plugin class */
    switch (node->info->type & PLUGIN_TYPE_MASK) {
    case PLUGIN_INPUT:
      if (((input_class_t *)cls)->dispose)
        ((input_class_t *)cls)->dispose ((input_class_t *)cls);
      break;
    case PLUGIN_DEMUX:
      if (((demux_class_t *)cls)->dispose)
        ((demux_class_t *)cls)->dispose ((demux_class_t *)cls);
      break;
    case PLUGIN_SPU_DECODER:
      if (((spu_decoder_class_t *)cls)->dispose)
        ((spu_decoder_class_t *)cls)->dispose ((spu_decoder_class_t *)cls);
      break;
    case PLUGIN_AUDIO_DECODER:
      if (((audio_decoder_class_t *)cls)->dispose)
        ((audio_decoder_class_t *)cls)->dispose ((audio_decoder_class_t *)cls);
      break;
    case PLUGIN_VIDEO_DECODER:
      if (((video_decoder_class_t *)cls)->dispose)
        ((video_decoder_class_t *)cls)->dispose ((video_decoder_class_t *)cls);
      break;
    case PLUGIN_AUDIO_OUT:
      if (((audio_driver_class_t *)cls)->dispose)
        ((audio_driver_class_t *)cls)->dispose ((audio_driver_class_t *)cls);
      break;
    case PLUGIN_VIDEO_OUT:
      if (((video_driver_class_t *)cls)->dispose)
        ((video_driver_class_t *)cls)->dispose ((video_driver_class_t *)cls);
      break;
    case PLUGIN_POST:
      if (((post_class_t *)cls)->dispose)
        ((post_class_t *)cls)->dispose ((post_class_t *)cls);
      break;
    case PLUGIN_XINE_MODULE:
      if (((xine_module_class_t *)cls)->dispose)
        ((xine_module_class_t *)cls)->dispose (cls);
      break;
    }
    node->plugin_class = NULL;
    if (node->file)
      dec_file_ref(node->file);
  }
}

/*
 *  load input+demuxer plugins
 *  load plugins that asked to be initialized
 */
static void _load_required_plugins(xine_t *this, xine_sarray_t *list) {

  int list_id = 0;
  int list_size;

  list_size = xine_sarray_size(list);
  while (list_id < list_size) {
    fat_node_t *node = xine_sarray_get (list, list_id);
    uint32_t flags = IS_FAT_NODE (node) ? node->flags : 0;

    /*
     * preload plugins if not cached
     */
    do {
      if (!((node->node.info->type & PLUGIN_MUST_PRELOAD) | (flags & FAT_NODE_FLAG_PROBE_CLASS))) /* no preload needed */
        break;
      if (node->node.plugin_class) /* is already loaded */
        break;
      if (node->node.file && !node->node.file->lib_handle) /* lib unavailable */
        break;

      lprintf ("preload plugin %s from %s\n", node->node.info->id, node->node.file ? node->node.file->filename : "libxine/builtins");

      if (!_load_plugin_class (this, &node->node, NULL)) {
	/* in case of failure remove from list */

	xine_sarray_remove(list, list_id);
	list_size = xine_sarray_size(list);
        list_id--;
        break;
      }

      if (node->node.info->type & PLUGIN_MUST_PRELOAD)
        break;

      _dispose_plugin_class (&node->node);
    } while (0);
    list_id++;
  }
}

static void load_required_plugins(xine_t *this) {
  int i;

  for (i = 0; i < PLUGIN_TYPE_MAX; i++) {
    _load_required_plugins (this, this->plugin_catalog->plugin_lists[i]);
  }
}

static unsigned int _cfg_entry_hash (void *a) {
  char **d = (char **)a;
  return ((uintptr_t)(*d) >> 4) & 127;
}

static int _cfg_entry_cmp (void *a, void *b) {
  char **d = (char **)a, **e = (char **)b;
  return *d < *e ? -1 : *d > *e ? 1 : 0;
}

/*
 *  save plugin list information to file (cached catalog)
 */
typedef struct {
  xine_sarray_t *list;
  cfg_entry_t *entries[MAX_DUPL_CFG_ENTRIES];
  uint32_t have;
} _load_plugins_dupl_cfg_t;

static void save_plugin_list (xine_t *this, FILE *fp, xine_sarray_t *list, _load_plugins_dupl_cfg_t *cfg) {
  int list_id = 0;
  int list_size;
#define SAVE_PLUGIN_BUF_SIZE 4096
  char b[SAVE_PLUGIN_BUF_SIZE];
  char *e = b + SAVE_PLUGIN_BUF_SIZE - 78 - 2 * XINE_MAX_INT64_STR - 5 * XINE_MAX_INT32_STR;
  const plugin_file_t *last_file = (const plugin_file_t *)1;
  uint32_t last_type = ~0u, last_api = ~0u, last_vers = ~0u;

  list_size = xine_sarray_size (list);
  while (list_id < list_size) {
    int pri;
    char *q = b;
    const plugin_node_t *node = xine_sarray_get (list, list_id);
    const plugin_file_t *file = node->file;
    if (file != last_file) {
      last_file = file;
      last_type = last_api = last_vers = ~0u;
      if (file) {
        *q++ = '[';
        q += strlcpy (q, file->filename, e - q);
        if (q >= e)
          q = e - 1;
        memcpy (q, "]\nsize=", 7); q += 7;
        xine_uint2str (&q, file->filesize);
        memcpy (q, "\nmtime=", 7); q += 7;
        xine_uint2str (&q, file->filemtime);
        *q++ = '\n';
      } else {
        /* dump builtins for debugging */
        memcpy (q, "[libxine/builtins]\n", 20); q += 19;
      }
    }
    if (node->info->type != last_type) {
      last_type = node->info->type;
      last_api = last_vers = ~0u;
      memcpy (q, "type=", 5); q += 5;
      xine_uint32_2str (&q, node->info->type);
      *q++ = '\n';
    }
    if (node->info->API != last_api) {
      last_api = node->info->API;
      memcpy (q, "api=", 4); q += 4;
      xine_uint32_2str (&q, node->info->API);
      *q++ = '\n';
    }
    if (node->info->version != last_vers) {
      last_vers = node->info->version;
      memcpy (q, "version=", 8); q += 8;
      xine_uint32_2str (&q, node->info->version);
      *q++ = '\n';
    }
    memcpy (q, "id=", 3); q += 3;
    q += strlcpy (q, node->info->id, e - q);
    if (q >= e)
      q = e - 1;
    *q++ = '\n';

    switch (node->info->type & PLUGIN_TYPE_MASK){

      case PLUGIN_VIDEO_OUT: {
        const vo_info_t *vo_info = node->info->special_info;
        memcpy (q, "visual_type=", 12); q += 12;
        xine_int32_2str (&q, vo_info->visual_type);
        memcpy (q, "\nvo_priority=", 13); q += 13;
        pri = vo_info->priority;
        goto write_pri;
      }
      case PLUGIN_AUDIO_OUT: {
        const ao_info_t *ao_info = node->info->special_info;
        memcpy (q, "ao_priority=", 12); q += 12;
        pri = ao_info->priority;
        goto write_pri;
      }
      case PLUGIN_AUDIO_DECODER:
      case PLUGIN_VIDEO_DECODER:
      case PLUGIN_SPU_DECODER: {
        const decoder_info_t *decoder_info = node->info->special_info;
        const uint32_t *t;
        memcpy (q, "supported_types=", 16); q += 16;
        t = decoder_info->supported_types;
        while (*t) {
          xine_uint32_2str (&q, *t++);
          if (q >= e) {
            fwrite (b, 1, q - b, fp);
            q = b;
          }
          *q++ = ' ';
        }
        q[-1] = '\n';
        memcpy (q, "decoder_priority=", 17); q += 17;
        pri = decoder_info->priority;
        goto write_pri;
      }
      case PLUGIN_DEMUX: {
        const demuxer_info_t *demuxer_info = node->info->special_info;
        memcpy (q, "demuxer_priority=", 17); q += 17;
        pri = demuxer_info->priority;
        goto write_pri;
      }
      case PLUGIN_INPUT: {
        const input_info_t *input_info = node->info->special_info;
        memcpy (q, "input_priority=", 15); q += 15;
        pri = input_info->priority;
        goto write_pri;
      }
      case PLUGIN_POST: {
        const post_info_t *post_info = node->info->special_info;
        memcpy (q, "post_type=", 10); q += 10;
        xine_uint32_2str (&q, post_info->type);
        *q++ = '\n';
        break;
      }
      case PLUGIN_XINE_MODULE: {
        const xine_module_info_t *module_info = node->info->special_info;
        size_t type_len = xine_find_byte (module_info->type, 0);
        memcpy (q, "module_type=", 12); q += 12;
        memcpy (q, module_info->type, type_len); q += type_len;
        memcpy (q, "\nmodule_sub_type=", 17); q += 17;
        xine_int32_2str (&q, module_info->sub_type);
        memcpy (q, "\nmodule_priority=", 17); q += 17;
        pri = module_info->priority;
        goto write_pri;
      }
      write_pri:
        xine_int32_2str (&q, pri);
        *q++ = '\n';
    }
    fwrite (b, 1, q - b, fp);

    /* config entries */
    if (node->config_entry_list) {
      xine_list_iterator_t ite = NULL;
#ifdef FAST_SCAN_PLUGINS
      cfg_entry_t *entry;
#else
      const char *entry;
#endif
      while ((entry = xine_list_next_value (node->config_entry_list, &ite))) {
        char *key_value;
#ifdef FAST_SCAN_PLUGINS
        if (cfg->have < MAX_DUPL_CFG_ENTRIES) {
          int cfg_index;
          cfg->entries[cfg->have] = entry;
          cfg_index = xine_sarray_add (cfg->list, cfg->entries + cfg->have);
          if (cfg_index < 0) {
            char b2[80], *q = b2;
            memcpy (q, "config_key=[", 12); q += 12;
            xine_uint32_2str (&q, (cfg_entry_t **)xine_sarray_get (cfg->list, ~cfg_index) - cfg->entries);
            memcpy (q, "]\n", 2); q += 2;
            fwrite (b2, 1, q - b2, fp);
            continue;
          }
          cfg->have++;
        }
        pthread_mutex_lock (&this->config->config_lock);
        this->config->cur = entry;
        key_value = this->config->get_serialized_entry (this->config, NULL);
        pthread_mutex_unlock (&this->config->config_lock);
#else
        /* now serialize the config key */
        key_value = this->config->get_serialized_entry (this->config, entry);
#endif
        if (key_value) {
          size_t slen = xine_find_byte (key_value, 0);
#ifdef FAST_SCAN_PLUGINS
          lprintf ("  config key: %s, serialization: %zu bytes\n", entry->key, slen);
#else
          lprintf ("  config key: %s, serialization: %zu bytes\n", entry, slen);
#endif
          fwrite ("config_key=", 1, 11, fp);
          key_value[slen] = '\n';
          fwrite (key_value, 1, slen + 1, fp);
          free (key_value);
        }
      }
    }

    fwrite ("\n", 1, 1, fp);
    list_id++;
  }
}

/*
 *  load plugin list information from file (cached catalog)
 */
typedef enum {
  _K_NONE = 0,
  _K_cache_catalog_version,
  _K_flush,
  _K_filename,
  _K_size,
  _K_mtime,
  _K_type,
  _K_api,
  _K_id,
  _K_version,
  _K_visual_type,
  _K_supported_types,
  _K_vo_priority,
  _K_ao_priority,
  _K_decoder_priority,
  _K_demuxer_priority,
  _K_input_priority,
  _K_post_type,
  _K_config_key,
  _K_module_priority,
  _K_module_sub_type,
  _K_module_type,
  _K_LAST
} _k_t;

static _k_t _key_2_index (const char *key, size_t klen) {
  switch (klen) {
    int d;
    case 0:
      return _K_flush;
    case 2:
      if (!memcmp (key, "id", 2))
        return _K_id;
      break;
    case 3:
      if (!memcmp (key, "api", 3))
        return _K_api;
      break;
    case 4:
      if (!memcmp (key, "size", 4))
        return _K_size;
      if (!memcmp (key, "type", 4))
        return _K_type;
      break;
    case 5:
      if (!memcmp (key, "mtime", 5))
        return _K_mtime;
      break;
    case 7:
      if (!memcmp (key, "version", 7))
        return _K_version;
      break;
    case 9:
      if (!memcmp (key, "post_type", 9))
        return _K_post_type;
      break;
    case 10:
      if (!memcmp (key, "config_key", 10))
        return _K_config_key;
      break;
    case 11:
      d = memcmp (key, "module_type", 11);
      if (d == 0) {
        return _K_module_type;
      } else if (d < 0) {
        if (!memcmp (key, "ao_priority", 11))
          return _K_ao_priority;
      } else {
        if (!memcmp (key, "visual_type", 11))
          return _K_visual_type;
        if (!memcmp (key, "vo_priority", 11))
          return _K_vo_priority;
      }
      break;
    case 14:
      if (!memcmp (key, "input_priority", 14))
        return _K_input_priority;
      break;
    case 15:
      d = memcmp (key, "module_sub_type", 15);
      if (d == 0) {
        return _K_module_sub_type;
      } else if (d < 0) {
        if (!memcmp (key, "module_priority", 15))
          return _K_module_priority;
      } else {
        if (!memcmp (key, "supported_types", 15))
          return _K_supported_types;
      }
      break;
    case 16:
      if (!memcmp (key, "decoder_priority", 16))
        return _K_decoder_priority;
      if (!memcmp (key, "demuxer_priority", 16))
        return _K_demuxer_priority;
      break;
    case 21:
      if (!memcmp (key, "cache_catalog_version", 21))
        return _K_cache_catalog_version;
      break;
    default: ;
  }
  return _K_NONE;
}

static void load_plugin_list (xine_t *this, const char *filename, xine_sarray_t *plugins) {
#ifdef FAST_SCAN_PLUGINS
  struct {
    cfg_entry_t *entries[MAX_DUPL_CFG_ENTRIES];
    uint32_t have;
  } cfg;
#endif
  fat_node_t *first_in_file, node; /** << node.file.filename is not a xine_fast_string_t, never passed there. */
  size_t stlen, fnlen, idlen;
  /* We dont have that many types yet ;-) */
  uint32_t supported_types[256];
  char *cfgentries[256], dummy_line[1] = "";
  int numcfgs;

  xine_fast_text_t *xft;
  int version_ok = 0, again = 1;
  int skip = 0;

  /* TJ. I got far less than 100k, so > 2M is probably insane. */
  xft = xine_fast_text_load (filename, 2 << 20);
  if (!xft)
    return;

  _fat_node_init (&node);
  fnlen = 0;
  stlen = 0;
  idlen = 0;
  fnlen = 0;
  numcfgs = 0;
  first_in_file = NULL;
#ifdef FAST_SCAN_PLUGINS
  cfg.have = 0;
#endif

  while (again) {
    _k_t index;
    size_t lsize;
    char *line = xine_fast_text_line (xft, &lsize);

    if (line) {
      /* skip comments */
      if (line[0] == '#')
        continue;
    } else {
      /* make sure to flush the last one */
      again = 0;
      line = dummy_line;
      lsize = 0;
    }

    if ((line[0] == '[') && version_ok) {
      /* file name */
      if (line[--lsize] != ']')
        continue;
      line[lsize] = 0;
      line++;
      lsize--;
      index = _K_filename;
    } else {
      /* key=value */
      size_t l;
      line[lsize] = '=';
      l = xine_find_byte (line, '=');
      line[lsize] = 0;
      index = _key_2_index (line, l);
      line += l;
      lsize -= l;
      if (*line)
        *line++ = 0, lsize--;
    }

    if (skip && (index != _K_filename))
      continue;

    if (!version_ok) {
      if (index == _K_cache_catalog_version) {
        const char *val = line;
        uint32_t vers = xine_str2uint32 (&val);
        /* we are slightly backward compatible */
        if ((vers == CACHE_CATALOG_VERSION) || (vers == CACHE_CATALOG_VERSION - 1)) {
          version_ok = 1;
        } else {
          xine_fast_text_unload (&xft);
          return;
        }
      }
      continue;
    }

    {
      const char *val = line;
      union {
        uint64_t llu;
        unsigned int u;
        int i;
      } v = {0};
      unsigned int mask = 1u << index;
      const uint32_t
        set_int32 = (1 << _K_type) | (1 << _K_api) | (1 << _K_visual_type) | (1 << _K_vo_priority)
                  | (1 << _K_ao_priority) | (1 << _K_decoder_priority) | (1 << _K_demuxer_priority)
                  | (1 << _K_input_priority) | (1 << _K_module_priority),
        set_uint32 = (1 << _K_version) | (1 << _K_post_type) | (1 << _K_module_sub_type),
        set_uint64 = (1 << _K_size) | (1 << _K_mtime);
      if (set_int32 & mask)
        v.i = xine_str2int32 (&val);
      else if (set_uint32 & mask)
        v.u = xine_str2uint32 (&val);
      else if (set_uint64 & mask)
        v.llu = xine_str2uint64 (&val);
      switch (index) {
        case _K_flush:
          if (idlen) {
            /* flush this entry */
            fat_node_t *n;
            char *q;
            /* get mem for new node */
            n = malloc (sizeof (node) + stlen + idlen + 1 + fnlen + 1 + 32);
            if (!n)
              break;
            /* fill in */
            *n = node;
            n->node.info = &n->info[0];
            q = (char *)n + sizeof (*n);
            if (stlen) {
              memcpy (&n->supported_types[0], &supported_types[0], stlen);
              q += stlen;
              n->ainfo.decoder_info.supported_types = &n->supported_types[0];
            }
            if (node.info[0].id) {
              xine_small_memcpy (q, node.info[0].id, idlen + 1);
              n->info[0].id = q;
              q += idlen + 1;
            }
            n->file.filename = xine_fast_string_init (q, fnlen + 32);
            xine_fast_string_set (n->file.filename, node.file.filename, fnlen);
            /* q += fn_need; */
            n->node.file = &n->file;
            n->info[0].special_info = &n->ainfo;
            /* register */
            if (first_in_file) {
              first_in_file->lastplugin->nextplugin = n;
              first_in_file->lastplugin = n;
            } else {
              int i = xine_sarray_add (plugins, n);
              if (i >= 0) { /* new file */
                n->lastplugin = n;
                first_in_file = n;
              } else {
                first_in_file = xine_sarray_get (plugins, ~i);
                first_in_file->lastplugin->nextplugin = n;
                first_in_file->lastplugin = n;
              }
            }
            if (numcfgs) {
              char **cfgentry;
#ifdef FAST_SCAN_PLUGINS
              new_entry_data_t ned;
              ned.v = this->config;
              ned.node = &n->node;
              this->config->set_new_entry_callback (this->config, _new_entry_cb, &ned);
#endif
              cfgentries[numcfgs] = NULL;
              for (cfgentry = cfgentries; *cfgentry; cfgentry++) {
                char *cfg_key;
#ifdef FAST_SCAN_PLUGINS
                if (cfgentry[0][0] == '[') {
                  const char *p = *cfgentry + 1;
                  uint32_t cfg_index = xine_str2uint32 (&p);
                  if (cfg_index < cfg.have) {
                    if (!n->node.config_entry_list)
                      n->node.config_entry_list = xine_list_new ();
                    if (n->node.config_entry_list && cfg.entries[cfg_index] &&
                      !xine_list_find (n->node.config_entry_list, cfg.entries[cfg_index]))
                      xine_list_push_back (n->node.config_entry_list, cfg.entries[cfg_index]);
                  }
                  continue;
                }
                ned.cfg_entry = NULL;
                cfg_key = this->config->register_serialized_entry (this->config, *cfgentry);
                if (cfg.have < MAX_DUPL_CFG_ENTRIES)
                  cfg.entries[cfg.have++] = ned.cfg_entry;
#else
                cfg_key = this->config->register_serialized_entry (this->config, *cfgentry);
#endif
                if (cfg_key) {
                  /* this node is a cached node */
#ifdef FAST_SCAN_PLUGINS
                  free (cfg_key);
#else
                  _attach_entry_to_node (&n->node, cfg_key);
#endif
                } else {
                  lprintf ("failed to deserialize config entry key\n");
                }
              }
#ifdef FAST_SCAN_PLUGINS
              this->config->unset_new_entry_callback (this->config);
#endif
            }
            /* reset */
            node.info[0].id = NULL;
            idlen = 0;
            supported_types[0] = 0;
            stlen = 0;
            memset (&node.ainfo, 0, sizeof (node.ainfo));
            numcfgs = 0;
          }
          break;
        case _K_filename:
          node.file.filename = line;
          fnlen = lsize;
          skip = ((lsize == 16) && !memcmp (line, "libxine/builtins", 16)) ? 1 : 0;
          first_in_file = NULL;
          break;
        case _K_size:
          node.file.filesize = v.llu;
          break;
        case _K_mtime:
          node.file.filemtime = v.llu;
          break;
        case _K_type:
          node.info[0].type = v.i;
          break;
        case _K_api:
          node.info[0].API = v.i;
          break;
        case _K_id:
          node.info[0].id = line;
          idlen = lsize;
          break;
        case _K_version:
          node.info[0].version = v.u;
          break;
        case _K_visual_type:
          node.ainfo.vo_info.visual_type = v.i;
          break;
        case _K_supported_types: {
          int i;
          for (i = 0; i < 255; i++) {
            if ((supported_types[i] = xine_str2uint32 (&val)) == 0)
              break;
          }
          supported_types[i++] = 0;
          stlen = i * sizeof (*supported_types);
          break;
        }
        case _K_vo_priority:
          node.ainfo.vo_info.priority = v.i;
          break;
        case _K_ao_priority:
          node.ainfo.ao_info.priority = v.i;
          break;
        case _K_decoder_priority:
          node.ainfo.decoder_info.priority = v.i;
          break;
        case _K_demuxer_priority:
          node.ainfo.demuxer_info.priority = v.i;
          break;
        case _K_input_priority:
          node.ainfo.input_info.priority = v.i;
          break;
        case _K_post_type:
          node.ainfo.post_info.type = v.u;
          break;
        case _K_config_key:
          if (numcfgs < 255)
            cfgentries[numcfgs++] = line;
          break;
        case _K_module_priority:
          node.ainfo.module_info.priority = v.i;
          break;
        case _K_module_sub_type:
          node.ainfo.module_info.sub_type = v.u;
          break;
        case _K_module_type:
          strlcpy (node.ainfo.module_info.type, line, sizeof (node.ainfo.module_info.type));
        default: ;
      }
    }
  }

  xine_fast_text_unload (&xft);
}

/**
 * @brief Returns the complete filename for the plugins' cache file
 * @param this xine instance pointer, used for logging and libxdg-basedir.
 * @param buf write filename here.
 * @param bsize write at most this many bytes.
 * @param createdir If not zero, create the directory structure in which
 *        the file has to reside.
 * @return the strlen () of filename, or 0 (eg if a directory could not be created).
 * @internal
 *
 * @see XDG Base Directory specification:
 *      http://standards.freedesktop.org/basedir-spec/latest/index.html
 */
static size_t catalog_filename (xine_t *this, char *buf, size_t bsize, int createdir) {
  const char *const xdg_cache_home = xdgCacheHome(&this->basedir_handle);
  size_t l1, l2;

  if (!xdg_cache_home)
    return 0;

  l1 = xine_find_byte (xdg_cache_home, 0);
  l2 = sizeof (PACKAGE) - 1;
  if (bsize < l1 + 1 + l2 + 1 + 14 + 1)
    return 0;

  memcpy (buf, xdg_cache_home, l1 + 1);
  memcpy (buf + l1, "/" PACKAGE "/plugins.cache", 1 + l2 + 14 + 1);

  /* If we're going to create the directory structure, we concatenate
   * piece by piece the path, so that we can try to create all the
   * directories.
   * If we don't need to create anything, we just concatenate the
   * whole path at once.
   */
  if ( createdir ) {
    int result = 0, e;

    buf[l1] = 0;
    result = mkdir (buf, 0700);
    if ((result != 0) && ((e = errno) != EEXIST)) {
      xprintf (this, XINE_VERBOSITY_LOG, _("Unable to create %s directory: %s\n"), buf, strerror (e));
      return 0;
    }
    buf[l1] = '/';

    buf[l1 + 1 + l2] = 0;
    result = mkdir (buf, 0700);
    if ((result != 0) && ((e = errno) != EEXIST)) {
      xprintf (this, XINE_VERBOSITY_LOG, _("Unable to create %s directory: %s\n"), buf, strerror (e));
      return 0;
    }
    buf[l1 + 1 + l2] = '/';
  }

  return l1 + 1 + l2 + 1 + 14;
}

/*
 * save catalog to cache file
 */
static void save_catalog (xine_t *this) {
  FILE       *fp;
  char oldname[1024 - 4], newname[1024];
  size_t nlen = catalog_filename (this, oldname, sizeof (oldname), 1);

  if (!nlen)
    return;

  memcpy (newname, oldname, nlen);
  memcpy (newname + nlen, ".new", 5);

  if ((fp = fopen (newname, "wb")) != NULL) {
    _load_plugins_dupl_cfg_t cfg;
    int i;

    cfg.list = xine_sarray_new (MAX_DUPL_CFG_ENTRIES, _cfg_entry_cmp);
    /* this is mainly a test :-) */
    xine_sarray_set_hash (cfg.list, _cfg_entry_hash, 128);
    xine_sarray_set_mode (cfg.list, XINE_SARRAY_MODE_UNIQUE);
    cfg.have = 0;

    fwrite ("# this file is automatically created by xine, do not edit.\n\n"
            "cache_catalog_version=" CACHE_CATALOG_VERSION_STR "\n\n", 1, 85, fp);

    for (i = 0; i < PLUGIN_TYPE_MAX; i++) {
      save_plugin_list (this, fp, this->plugin_catalog->plugin_lists[i], &cfg);
    }
    save_plugin_list (this, fp, this->plugin_catalog->modules_list, &cfg);

    xine_sarray_delete (cfg.list);

    if (fclose(fp))
    {
      const char *err = strerror (errno);
      xine_log (this, XINE_LOG_MSG,
		_("failed to save catalogue cache: %s\n"), err);
      goto do_unlink;
    }
    else if (rename (newname, oldname))
    {
      const char *err = strerror (errno);
      xine_log (this, XINE_LOG_MSG,
		_("failed to replace catalogue cache: %s\n"), err);
      do_unlink:
      if (unlink (newname) && errno != ENOENT)
      {
	err = strerror (errno);
	xine_log (this, XINE_LOG_MSG,
		  _("failed to remove new catalogue cache: %s\n"), err);
      }
    }
  }
}

/*
 * load cached catalog from file
 */
static void load_cached_catalog (xine_t *this) {
  char filename[1024];

  if (catalog_filename (this, filename, sizeof (filename), 0) > 0)
    load_plugin_list (this, filename, this->plugin_catalog->cache_list);
}


/*
 *  initialize catalog, load all plugins into new catalog
 */
int _x_scan_plugins (xine_t *this_gen) {
#define XSP_BUFSIZE 4096
  xine_private_t *this = (xine_private_t *)this_gen;
  char _buf[XSP_BUFSIZE + 32];
  char *buf = xine_fast_string_init (_buf, sizeof (_buf));
  char *bufend = buf + XSP_BUFSIZE - 16;
  const char *pluginpath = NULL;
  const char *homedir;
  size_t homelen;

  lprintf("_x_scan_plugins()\n");

  _x_assert(this);
  _x_assert (this->x.config);
  _x_assert (!this->x.plugin_catalog);

  homedir = xine_get_homedir ();
  if (!homedir)
    return -1;
  homelen = xine_find_byte (homedir, 0);
  if (homelen > XSP_BUFSIZE - 16)
    homelen = XSP_BUFSIZE - 16;

  this->x.plugin_catalog = _new_catalog();
  if (!this->x.plugin_catalog)
    return -1;

  this->strings.decoder_pri_help = xine_ref_string_ref (
    _("The priority provides a ranking in case some media can be handled by more than one decoder.\n"
      "A priority of 0 enables the decoder's default priority."), -1);

  XINE_PROFILE (load_cached_catalog (&this->x));

#ifdef XINE_MAKE_BUILTINS
  lprintf ("collect_plugins in libxine\n");
  _register_plugins_internal (&this->x, NULL, NULL , xine_builtin_plugin_info, FAT_NODE_FLAG_PROBE_CLASS);
#endif

  if ((pluginpath = getenv("XINE_PLUGIN_PATH")) != NULL && *pluginpath) {

    const char *start = pluginpath, *stop;
    char *q;
    size_t len;
    while (1) {
      q = buf;
      if ((start[0] == '~') && (start[1] == '/')) {
        start += 1;
        xine_small_memcpy (q, homedir, homelen);
        q += homelen;
      }
      stop = strchr (start, XINE_PATH_SEPARATOR_CHAR);
      if (!stop)
        break;
      len = stop - start;
      if (len > (size_t)(bufend - q))
        len = bufend - q;
      xine_small_memcpy (q, start, len); q += len;
      q[0] = 0;
      start = stop + 1;
      collect_plugins (&this->x, buf, q, bufend);
    }
    len = xine_find_byte (start, 0);
    if (len > (size_t)(bufend - q))
      len = bufend - q;
    xine_small_memcpy (q, start, len); q += len;
    q[0] = 0;
    collect_plugins (&this->x, buf, q, bufend);

  } else {

    const char *p;
    size_t len;
    int i;

    xine_small_memcpy (buf, homedir, homelen);
    memcpy (buf + homelen, "/.xine/plugins", 15);
    collect_plugins (&this->x, buf, buf + homelen + 15, bufend);

    p = XINE_PLUGINROOT;
    len = sizeof (XINE_PLUGINROOT) - 1;
    xine_small_memcpy (buf, p, len);
    buf[len++] = '.';
    for (i = XINE_LT_AGE; i >= 0; i--) {
      char *q = buf + len;
      xine_uint32_2str (&q, i);
      collect_plugins (&this->x, buf, q, bufend);
    }
  }

  load_required_plugins (&this->x);

  if ((this->flags & XINE_FLAG_NO_WRITE_CACHE) == 0)
    XINE_PROFILE (save_catalog (&this->x));

  map_decoders (&this->x);

  return 0;
}

/*
 * generic module loading
 */
xine_module_t *_x_find_module(xine_t *xine, const char *type, const char *id,
                              unsigned sub_type, const void *params) {

  plugin_catalog_t *catalog = xine->plugin_catalog;
  plugin_node_t    *node;
  xine_module_t    *plugin = NULL;
  int               list_id, list_size;
  const xine_module_info_t *info;

  pthread_mutex_lock (&catalog->lock);

  list_size = xine_sarray_size(catalog->modules_list);
  for (list_id = 0; list_id < list_size; list_id++) {
    node = xine_sarray_get (catalog->modules_list, list_id);

    if (id && strcmp (node->info->id, id))
      continue;

    info = node->info->special_info;
    if (sub_type != info->sub_type)
      continue;
    if (type && strcmp (info->type, type))
      continue;

    if (node->plugin_class || _load_plugin_class(xine, node, params)) {
      if ((plugin = ((xine_module_class_t *)node->plugin_class)->get_instance(node->plugin_class, params))) {
        inc_node_ref(node);
        plugin->node = node;
        break;
      }
    }
  }

  pthread_mutex_unlock (&catalog->lock);

  return plugin;
}

void _x_free_module(xine_t *xine, xine_module_t **pmodule) {

  xine_module_t    *module = *pmodule;
  plugin_catalog_t *catalog = xine->plugin_catalog;
  plugin_node_t    *node = module->node;

  *pmodule = NULL;

  module->dispose(module);

  if (node) {
    pthread_mutex_lock(&catalog->lock);
    dec_node_ref(node);
    pthread_mutex_unlock(&catalog->lock);
  }
}

/*
 * input / demuxer plugin loading
 */

input_plugin_t *_x_find_input_plugin (xine_stream_t *stream, const char *mrl) {

  xine_stream_private_t *s;
  xine_t *xine;
  plugin_catalog_t *catalog;
  input_plugin_t *plugin;
  input_class_t *skip_class;
  uint32_t n;

  if (!stream || !mrl)
    return NULL;

  s = (xine_stream_private_t *)stream;
  xine = s->s.xine;
  catalog = xine->plugin_catalog;
  plugin = NULL;

  /* prevent recursion during input_plugin->open (). */
  skip_class = s->s.input_plugin ? s->s.input_plugin->input_class : NULL;

  pthread_mutex_lock (&catalog->lock);

  /* prevent recursion during input_class->get_instance (). */
  n = !s->query_input_plugins[0] ? 0
    : !s->query_input_plugins[1] ? 1 : 2;
  if (n != 2) {
    int list_id, list_size;
    list_size = xine_sarray_size (catalog->plugin_lists[PLUGIN_INPUT - 1]);
    for (list_id = 0; list_id < list_size; list_id++) {
      plugin_node_t *node = xine_sarray_get (catalog->plugin_lists[PLUGIN_INPUT - 1], list_id);
      input_class_t *class = (input_class_t *)node->plugin_class;
      if (!class) {
        _load_plugin_class (xine, node, NULL);
        class = (input_class_t *)node->plugin_class;
      }
      if (class) {
        s->query_input_plugins[n] = class;
        if ((class != skip_class) && (s->query_input_plugins[0] != s->query_input_plugins[1])) {
          plugin = class->get_instance (class, stream, mrl);
          if (plugin) {
            inc_node_ref (node);
            plugin->node = node;
            break;
          }
        }
      }
    }
    s->query_input_plugins[n] = NULL;
  }

  pthread_mutex_unlock (&catalog->lock);

  return plugin;
}


void _x_free_input_plugin (xine_stream_t *stream, input_plugin_t *input) {
  plugin_catalog_t *catalog;
  plugin_node_t    *node;

  if (!input)
    return;
  node = input->node;
  input->dispose (input);

  if (!stream)
    return;
  catalog = stream->xine->plugin_catalog;
  if (node) {
    pthread_mutex_lock(&catalog->lock);
    dec_node_ref(node);
    pthread_mutex_unlock(&catalog->lock);
  }
}

demux_plugin_t *_x_find_demux_plugin (xine_stream_t *stream, input_plugin_t *input) {
  uint8_t           mbuf[256];
  int               methods[3], i;
  plugin_catalog_t *catalog;
  demux_plugin_t   *plugin;
  const char       *mime_type = "";

  if (!stream || !input)
    return NULL;

  switch (stream->xine->demux_strategy) {
    default:
      xprintf (stream->xine, XINE_VERBOSITY_LOG,
        _("load_plugins: unknown content detection strategy %d\n"),
        stream->xine->demux_strategy);
      /* fall through */
    case XINE_DEMUX_DEFAULT_STRATEGY:
      methods[0] = METHOD_BY_CONTENT;
      methods[1] = METHOD_BY_MRL;
      break;
    case XINE_DEMUX_REVERT_STRATEGY:
      methods[0] = METHOD_BY_MRL;
      methods[1] = METHOD_BY_CONTENT;
      break;
    case XINE_DEMUX_CONTENT_STRATEGY:
      methods[0] = METHOD_BY_CONTENT;
      methods[1] = -1;
      mime_type = NULL;
      break;
    case XINE_DEMUX_EXTENSION_STRATEGY:
      methods[0] = METHOD_BY_MRL;
      methods[1] = -1;
      break;
  }
  methods[2] = -1;

  if (mime_type
    && input->get_optional_data
    && (input->get_optional_data (input, NULL, INPUT_OPTIONAL_DATA_DEMUX_MIME_TYPE) != INPUT_OPTIONAL_UNSUPPORTED)
    && (input->get_optional_data (input, &mime_type, INPUT_OPTIONAL_DATA_MIME_TYPE) != INPUT_OPTIONAL_UNSUPPORTED)
    && strcasecmp (mime_type, "text/plain")) {
    ;
  } else {
    mime_type = NULL;
  }
  _mime_set (mbuf, sizeof (mbuf), mime_type);

  plugin = NULL;
  catalog = stream->xine->plugin_catalog;

  for (i = 0; (methods[i] >= 0) && !plugin; i++) {
    int list_id, list_size;

    pthread_mutex_lock (&catalog->lock);

    list_size = xine_sarray_size(catalog->plugin_lists[PLUGIN_DEMUX - 1]);
    for (list_id = 0; list_id < list_size; list_id++) {
      plugin_node_t *node;

      node = xine_sarray_get (catalog->plugin_lists[PLUGIN_DEMUX - 1], list_id);

      xprintf(stream->xine, XINE_VERBOSITY_DEBUG, "load_plugins: probing demux '%s'\n", node->info->id);

      if (node->plugin_class || _load_plugin_class(stream->xine, node, NULL)) {
        demux_class_t *class = (demux_class_t *)node->plugin_class;

        /* If detecting by MRL, try the MIME type first (but not text/plain)... */
        stream->content_detection_method = METHOD_EXPLICIT;
        if ((methods[i] == METHOD_BY_MRL)
          && (_mime_find (mbuf, class->mimetypes) >= 0)
          && (plugin = class->open_plugin (class, stream, input))) {
          inc_node_ref(node);
          plugin->node = node;
          break;
        }

        /* ... then try the extension */
        stream->content_detection_method = methods[i];
        if ((stream->content_detection_method == METHOD_BY_MRL) &&
            !_x_demux_check_extension (input->get_mrl (input), class->extensions))
          continue;

        if ((plugin = class->open_plugin (class, stream, input))) {
	  inc_node_ref(node);
	  plugin->node = node;
	  break;
        }
      }
    }

    pthread_mutex_unlock (&catalog->lock);
  }

  if (input == stream->input_plugin) {
    xine_stream_private_t *s = (xine_stream_private_t *)stream;
    s->demux.input_caps = input->get_capabilities (input);
  }

  return plugin;
}

demux_plugin_t *_x_find_demux_plugin_by_name(xine_stream_t *stream, const char *name, input_plugin_t *input) {

  plugin_catalog_t  *catalog = stream->xine->plugin_catalog;
  plugin_node_t     *node;
  demux_plugin_t    *plugin = NULL;
  int                list_id, list_size;

  pthread_mutex_lock(&catalog->lock);

  stream->content_detection_method = METHOD_EXPLICIT;

  list_size = xine_sarray_size(catalog->plugin_lists[PLUGIN_DEMUX - 1]);
  for (list_id = 0; list_id < list_size; list_id++) {

    node = xine_sarray_get(catalog->plugin_lists[PLUGIN_DEMUX - 1], list_id);

    if (strcasecmp(node->info->id, name) == 0) {
      if (node->plugin_class || _load_plugin_class(stream->xine, node, NULL)) {
#if 0
        /* never triggered (method is set to EXPLICIT few lines earlier) */
	if ( stream->content_detection_method == METHOD_BY_MRL &&
	     ! _x_demux_check_extension(input->get_mrl(input),
					 ((demux_class_t *)node->plugin_class)->extensions)
	     )
	  continue;
#endif
        if ((plugin = ((demux_class_t *)node->plugin_class)->open_plugin(node->plugin_class, stream, input))) {
	  inc_node_ref(node);
	  plugin->node = node;
	  break;
        }
      }
    }
  }

  pthread_mutex_unlock(&catalog->lock);
  return plugin;
}

/*
 * this is a special test mode for content detection: all demuxers are probed
 * by content and extension except last_demux_name which is tested after
 * every other demuxer.
 *
 * this way we can make sure no demuxer will interfere on probing of a
 * known stream.
 */

demux_plugin_t *_x_find_demux_plugin_last_probe(xine_stream_t *stream, const char *last_demux_name, input_plugin_t *input) {

  int               i;
  int               methods[3];
  xine_t           *xine = stream->xine;
  plugin_catalog_t *catalog = xine->plugin_catalog;
  plugin_node_t    *last_demux = NULL;
  demux_plugin_t   *plugin = NULL;

  methods[0] = METHOD_BY_CONTENT;
  methods[1] = METHOD_BY_MRL;
  methods[2] = -1;

  i = 0;
  while (methods[i] != -1 && !plugin) {
    int list_id, list_size;

    stream->content_detection_method = methods[i];

    pthread_mutex_lock (&catalog->lock);

    list_size = xine_sarray_size(catalog->plugin_lists[PLUGIN_DEMUX - 1]);
    for (list_id = 0; list_id < list_size; list_id++) {
      plugin_node_t *node;

      node = xine_sarray_get (catalog->plugin_lists[PLUGIN_DEMUX - 1], list_id);

      lprintf ("probing demux '%s'\n", node->info->id);

      if (strcasecmp(node->info->id, last_demux_name) == 0) {
        last_demux = node;
      } else {
	xprintf(stream->xine, XINE_VERBOSITY_DEBUG,
		"load_plugin: probing '%s' (method %d)...\n", node->info->id, stream->content_detection_method );
	if (node->plugin_class || _load_plugin_class(xine, node, NULL)) {

	  if ( stream->content_detection_method == METHOD_BY_MRL &&
	       ! _x_demux_check_extension(input->get_mrl(input),
					   ((demux_class_t *)node->plugin_class)->extensions)
	       )
	    continue;


          if ((plugin = ((demux_class_t *)node->plugin_class)->open_plugin(node->plugin_class, stream, input))) {
	    xprintf (stream->xine, XINE_VERBOSITY_DEBUG,
		     "load_plugins: using demuxer '%s' (instead of '%s')\n", node->info->id, last_demux_name);
	    inc_node_ref(node);
	    plugin->node = node;
	    break;
          }
        }
      }
    }

    pthread_mutex_unlock (&catalog->lock);

    i++;
  }

  if( plugin )
    return plugin;

  if( !last_demux )
    return NULL;

  stream->content_detection_method = METHOD_BY_CONTENT;

  pthread_mutex_lock (&catalog->lock);
  if (last_demux->plugin_class || _load_plugin_class(xine, last_demux, NULL)) {

  if ((plugin = ((demux_class_t *)last_demux->plugin_class)->open_plugin(last_demux->plugin_class, stream, input))) {
    xprintf (stream->xine, XINE_VERBOSITY_LOG, _("load_plugins: using demuxer '%s'\n"), last_demux_name);
    inc_node_ref(last_demux);
    plugin->node = last_demux;
  }
  }
  pthread_mutex_unlock (&catalog->lock);

  return plugin;
}


void _x_free_demux_plugin (xine_stream_t *stream, demux_plugin_t **pdemux) {
  demux_plugin_t   *demux = *pdemux;
  plugin_catalog_t *catalog = stream->xine->plugin_catalog;
  plugin_node_t    *node = demux->node;

  *pdemux = NULL;

  demux->dispose(demux);

  if (node) {
    pthread_mutex_lock(&catalog->lock);
    dec_node_ref(node);
    pthread_mutex_unlock(&catalog->lock);
  }
}


const char *const *xine_get_autoplay_input_plugin_ids(xine_t *this) {
  const char **last, **end, *test;
  int list_id, list_size;
  plugin_catalog_t *catalog = this->plugin_catalog;

  pthread_mutex_lock (&catalog->lock);
  end = &catalog->ids[0] + sizeof (catalog->ids) / sizeof (catalog->ids[0]) - 1;
  last = &catalog->ids[0];
  *last = NULL;
  test = NULL;

  list_size = xine_sarray_size (catalog->plugin_lists[PLUGIN_INPUT - 1]);
  for (list_id = 0; list_id < list_size; list_id++) {

    plugin_node_t *node = xine_sarray_get (catalog->plugin_lists[PLUGIN_INPUT - 1], list_id);
    input_class_t *ic = node->plugin_class;
    if (!ic) {
      _load_plugin_class (this, node, NULL);
      ic = node->plugin_class;
    }
    if (ic && ic->get_autoplay_list) {
      if (!strcasecmp (node->info->id, "TEST")) {
        /* dont let TEST push user media devices out of xine-ui 1 click list. */
        test = node->info->id;
      } else {
        const char **here = &catalog->ids[0], **p;
        while (*here && strcasecmp (*here, node->info->id) < 0)
          here++;
        last++;
        for (p = last; p > here; p--)
          p[0] = p[-1];
        *here = node->info->id;
      }
      if (last >= end)
        break;
    }

  }

  if (last < end) {
    *last++ = test;
    *last = NULL;
  }

  pthread_mutex_unlock (&catalog->lock);

  return catalog->ids;
}

const char *const *xine_get_browsable_input_plugin_ids(xine_t *this) {
  const char **last, **end, *test;
  int list_id, list_size;
  plugin_catalog_t *catalog = this->plugin_catalog;

  pthread_mutex_lock (&catalog->lock);
  end = &catalog->ids[0] + sizeof (catalog->ids) / sizeof (catalog->ids[0]) - 1;
  last = &catalog->ids[0];
  *last = NULL;
  test = NULL;

  list_size = xine_sarray_size (catalog->plugin_lists[PLUGIN_INPUT - 1]);
  for (list_id = 0; list_id < list_size; list_id++) {

    plugin_node_t *node = xine_sarray_get (catalog->plugin_lists[PLUGIN_INPUT - 1], list_id);
    input_class_t *ic = node->plugin_class;
    if (!ic) {
      _load_plugin_class (this, node, NULL);
      ic = node->plugin_class;
    }
    if (ic && ic->get_dir) {
      if (!strcasecmp (node->info->id, "TEST")) {
        /* dont let TEST push user media devices out of xine-ui 1 click list. */
        test = node->info->id;
      } else {
        const char **here = &catalog->ids[0], **p;
        while (*here && strcasecmp (*here, node->info->id) < 0)
          here++;
        last++;
        for (p = last; p > here; p--)
          p[0] = p[-1];
        *here = node->info->id;
      }
      if (last >= end)
        break;
    }

  }

  if (last < end) {
    *last++ = test;
    *last = NULL;
  }

  pthread_mutex_unlock (&catalog->lock);

  return catalog->ids;
}

/*
 *  video out plugins section
 */

static vo_driver_t *_load_video_driver (xine_t *this, plugin_node_t *node,
                                        const void *data) {

  vo_driver_t *driver;

  if (!node->plugin_class && !_load_plugin_class (this, node, data))
    return NULL;

  driver = ((video_driver_class_t *)node->plugin_class)->open_plugin(node->plugin_class, data);

  if (driver) {
    inc_node_ref(node);
    driver->node = node;
  }

  return driver;
}

vo_driver_t *_x_load_video_output_plugin(xine_t *this,
                                         const char *id,
                                         int visual_type,
                                         const void *visual) {

  plugin_node_t      *node;
  vo_driver_t        *driver = NULL;
  const vo_info_t    *vo_info;
  plugin_catalog_t   *catalog = this->plugin_catalog;
  int                 list_id, list_size;

  if (id && !strcasecmp(id, "auto"))
    id = NULL;

  pthread_mutex_lock (&catalog->lock);

  list_size = xine_sarray_size (catalog->plugin_lists[PLUGIN_VIDEO_OUT - 1]);
  for (list_id = 0; list_id < list_size; list_id++) {

    node = xine_sarray_get (catalog->plugin_lists[PLUGIN_VIDEO_OUT - 1], list_id);

    vo_info = (const vo_info_t *)node->info->special_info;
    if (vo_info->visual_type == visual_type) {
      if (id) {
	if (!strcasecmp (node->info->id, id)) {
	  driver = _load_video_driver (this, node, visual);
	  break;
	}

      } else {

	driver = _load_video_driver (this, node, visual);

	if (driver) {

	  break;
	}
      }
    }
  }

  pthread_mutex_unlock (&catalog->lock);

  return driver;
}

xine_video_port_t *xine_open_video_driver (xine_t *this,
					   const char *id,
                                           int visual_type,
                                           const void *visual) {

  vo_driver_t        *driver;
  xine_video_port_t  *port;

  driver = _x_load_video_output_plugin(this, id, visual_type, visual);

  if (!driver) {
    lprintf ("failed to load video output plugin <%s>\n", id);
    return NULL;
  }

  port = _x_vo_new_port(this, driver, 0);

  return port;
}

xine_video_port_t *xine_new_framegrab_video_port (xine_t *this) {

  plugin_node_t      *node;
  vo_driver_t        *driver;
  xine_video_port_t  *port;
  plugin_catalog_t   *catalog = this->plugin_catalog;
  const char         *id;
  int                 list_id, list_size;

  driver = NULL;
  id     = "none";

  pthread_mutex_lock (&catalog->lock);

  list_size = xine_sarray_size (catalog->plugin_lists[PLUGIN_VIDEO_OUT - 1]);
  for (list_id = 0; list_id < list_size; list_id++) {

    node = xine_sarray_get (catalog->plugin_lists[PLUGIN_VIDEO_OUT - 1], list_id);

    if (!strcasecmp (node->info->id, id)) {
      driver = _load_video_driver (this, node, NULL);
      break;
    }
  }

  pthread_mutex_unlock (&catalog->lock);

  if (!driver) {
    lprintf ("failed to load video output plugin <%s>\n", id);
    return NULL;
  }

  port = _x_vo_new_port(this, driver, 1);

  return port;
}

/*
 *  audio output plugins section
 */

const char *const *xine_list_audio_output_plugins (xine_t *xine) {
  return _build_list_typed_plugins (xine, PLUGIN_AUDIO_OUT, 0);
}

const char *const *xine_list_video_output_plugins (xine_t *xine) {
  return _build_list_typed_plugins (xine, PLUGIN_VIDEO_OUT, ~(uint64_t)0);
}

const char * const *xine_list_video_output_plugins_typed (xine_t *xine, uint64_t typemask) {
  return _build_list_typed_plugins (xine, PLUGIN_VIDEO_OUT, typemask);
}

static ao_driver_t *_load_audio_driver (xine_t *this, plugin_node_t *node,
                                        const void *data) {

  ao_driver_t *driver;

  if (!node->plugin_class && !_load_plugin_class (this, node, data))
    return NULL;

  driver = ((audio_driver_class_t *)node->plugin_class)->open_plugin(node->plugin_class, data);

  if (driver) {
    inc_node_ref(node);
    driver->node = node;
  }

  return driver;
}

ao_driver_t *_x_load_audio_output_plugin (xine_t *this, const char *id)
{
  plugin_node_t      *node;
  ao_driver_t        *driver = NULL;
  plugin_catalog_t   *catalog = this->plugin_catalog;
  int                 list_id, list_size;

  pthread_mutex_lock (&catalog->lock);

  list_size = xine_sarray_size (this->plugin_catalog->plugin_lists[PLUGIN_AUDIO_OUT - 1]);
  for (list_id = 0; list_id < list_size; list_id++) {

    node = xine_sarray_get (this->plugin_catalog->plugin_lists[PLUGIN_AUDIO_OUT - 1], list_id);

    if (!strcasecmp(node->info->id, id)) {
      driver = _load_audio_driver (this, node, NULL);
      break;
    }
  }

  pthread_mutex_unlock (&catalog->lock);

  if (!driver) {
    xprintf (this, XINE_VERBOSITY_LOG,
        _("load_plugins: failed to load audio output plugin <%s>\n"), id);
  }
  return driver;
}

xine_audio_port_t *xine_open_audio_driver (xine_t *this, const char *id,
                                           const void *data) {

  plugin_node_t      *node;
  ao_driver_t        *driver = NULL;
  xine_audio_port_t  *port;
  const ao_info_t    *ao_info;
  plugin_catalog_t   *catalog = this->plugin_catalog;
  int                 list_id, list_size;

  if (id && !strcasecmp(id, "auto") )
    id = NULL;

  pthread_mutex_lock (&catalog->lock);

  list_size = xine_sarray_size (this->plugin_catalog->plugin_lists[PLUGIN_AUDIO_OUT - 1]);
  for (list_id = 0; list_id < list_size; list_id++) {

    node = xine_sarray_get (this->plugin_catalog->plugin_lists[PLUGIN_AUDIO_OUT - 1], list_id);

    ao_info = (const ao_info_t *)node->info->special_info;

    if (id) {
      if (!strcasecmp(node->info->id, id)) {
	driver = _load_audio_driver (this, node, data);
	break;
      }
    } else if( ao_info->priority >= 0 ) {
      driver = _load_audio_driver (this, node, data);
      if (driver) {
	break;
      }
    }
  }

  pthread_mutex_unlock (&catalog->lock);

  if (!driver) {
    if (id)
      xprintf (this, XINE_VERBOSITY_LOG,
	       _("load_plugins: failed to load audio output plugin <%s>\n"), id);
    else
      xprintf (this, XINE_VERBOSITY_LOG,
	       _("load_plugins: audio output auto-probing didn't find any usable audio driver.\n"));
    return NULL;
  }

  port = _x_ao_new_port(this, driver, 0);

  return port;
}

xine_audio_port_t *xine_new_framegrab_audio_port (xine_t *this) {

  xine_audio_port_t  *port;

  port = _x_ao_new_port (this, NULL, 1);

  return port;
}

void _x_free_audio_driver (xine_t *xine, ao_driver_t **pdriver) {

  ao_driver_t      *driver = *pdriver;
  plugin_catalog_t *catalog = xine->plugin_catalog;
  plugin_node_t    *node = driver->node;

  *pdriver = NULL;

  driver->exit(driver);

  if (node) {
    pthread_mutex_lock(&catalog->lock);
    dec_node_ref(node);
    pthread_mutex_unlock(&catalog->lock);
  }
}

void _x_free_video_driver (xine_t *xine, vo_driver_t **pdriver) {

  vo_driver_t      *driver = *pdriver;
  plugin_catalog_t *catalog = xine->plugin_catalog;
  plugin_node_t    *node = driver->node;

  *pdriver = NULL;

  driver->dispose (driver);

  if (node) {
    pthread_mutex_lock(&catalog->lock);
    dec_node_ref(node);
    pthread_mutex_unlock(&catalog->lock);
  }
}

void xine_close_audio_driver (xine_t *this, xine_audio_port_t  *ao_port) {

  (void)this;
  if( ao_port )
    ao_port->exit(ao_port);

}

void xine_close_video_driver (xine_t *this, xine_video_port_t  *vo_port) {

  (void)this;
  if( vo_port )
    vo_port->exit(vo_port);

}


/*
 * get autoplay mrl list from input plugin
 */

static input_class_t *_get_input_class (xine_t *this, const char *plugin_id) {

  plugin_catalog_t     *catalog;
  plugin_node_t        *node;
  int                   list_id, list_size;

  catalog = this->plugin_catalog;

  list_size = xine_sarray_size (catalog->plugin_lists[PLUGIN_INPUT - 1]);
  for (list_id = 0; list_id < list_size; list_id++) {

    node = xine_sarray_get (catalog->plugin_lists[PLUGIN_INPUT - 1], list_id);

    if (!strcasecmp (node->info->id, plugin_id)) {
      if (node->plugin_class || _load_plugin_class (this, node, NULL)) {
        return node->plugin_class;
      }
    }
  }
  return NULL;
}

const char * const *xine_get_autoplay_mrls (xine_t *this, const char *plugin_id,
					    int *num_mrls) {

  plugin_catalog_t     *catalog;
  input_class_t        *ic;
  const char * const   *mrls = NULL;

  catalog = this->plugin_catalog;

  pthread_mutex_lock (&catalog->lock);

  ic = _get_input_class(this, plugin_id);
  if (ic && ic->get_autoplay_list)
    mrls = ic->get_autoplay_list (ic, num_mrls);

  pthread_mutex_unlock (&catalog->lock);

  return mrls;
}

/*
 * input plugin mrl browser support
 */

xine_mrl_t **xine_get_browse_mrls (xine_t *this, const char *plugin_id,
                                   const char *start_mrl, int *num_mrls) {
  plugin_catalog_t   *catalog;
  input_class_t      *ic;
  xine_mrl_t        **mrls = NULL;

  catalog = this->plugin_catalog;

  pthread_mutex_lock (&catalog->lock);

  ic = _get_input_class (this, plugin_id);
  if (ic && ic->get_dir)
    mrls = ic->get_dir (ic, start_mrl, num_mrls);

  pthread_mutex_unlock (&catalog->lock);

  return mrls;
}

video_decoder_t *_x_get_video_decoder (xine_stream_t *stream, uint8_t stream_type) {

  plugin_node_t    *node;
  int               i, j;
  plugin_catalog_t *catalog = stream->xine->plugin_catalog;
  video_decoder_t  *vd = NULL;

  lprintf ("looking for video decoder for streamtype %02x\n", stream_type);
  _x_assert(stream_type < DECODER_MAX);

  pthread_mutex_lock (&catalog->lock);

  for (i = 0; i < PLUGINS_PER_TYPE; i++) {

    node = catalog->video_decoder_map[stream_type][i];

    if (!node) {
      break;
    }

    if (!node->plugin_class && !_load_plugin_class (stream->xine, node, NULL)) {
      /* remove non working plugin from catalog */
      xprintf(stream->xine, XINE_VERBOSITY_DEBUG,
	      "load_plugins: plugin %s failed to init its class.\n", node->info->id);
      for (j = i + 1; j < PLUGINS_PER_TYPE; j++)
        catalog->video_decoder_map[stream_type][j - 1] =
          catalog->video_decoder_map[stream_type][j];
      catalog->video_decoder_map[stream_type][PLUGINS_PER_TYPE-1] = NULL;
      i--;
      continue;
    }

    vd = ((video_decoder_class_t *)node->plugin_class)->open_plugin(node->plugin_class, stream);

    if (vd == (video_decoder_t*)1) {
      /* HACK: plugin failed to instantiate because required resources are unavailable at that time,
         but may be available later, so don't remove this plugin from catalog. */
      xprintf(stream->xine, XINE_VERBOSITY_DEBUG,
          "load_plugins: plugin %s failed to instantiate, resources temporarily unavailable.\n", node->info->id);
    }
    else if (vd) {
      inc_node_ref(node);
      vd->node = node;
      xprintf(stream->xine, XINE_VERBOSITY_DEBUG,
          "load_plugins: plugin %s will be used for video streamtype %02x.\n",
          node->info->id, stream_type);

      break;
    } else {
      /* remove non working plugin from catalog */
      xprintf(stream->xine, XINE_VERBOSITY_DEBUG,
	      "load_plugins: plugin %s failed to instantiate itself.\n", node->info->id);
      for (j = i + 1; j < PLUGINS_PER_TYPE; j++)
        catalog->video_decoder_map[stream_type][j - 1] =
          catalog->video_decoder_map[stream_type][j];
      catalog->video_decoder_map[stream_type][PLUGINS_PER_TYPE-1] = NULL;
      i--;
    }
  }

  pthread_mutex_unlock (&catalog->lock);
  return vd;
}

void _x_free_video_decoder (xine_stream_t *stream, video_decoder_t *vd) {
  plugin_catalog_t *catalog = stream->xine->plugin_catalog;
  plugin_node_t    *node = vd->node;

  vd->dispose (vd);

  if (node) {
    pthread_mutex_lock (&catalog->lock);
    dec_node_ref(node);
    pthread_mutex_unlock (&catalog->lock);
  }
}


audio_decoder_t *_x_get_audio_decoder (xine_stream_t *stream, uint8_t stream_type) {

  plugin_node_t    *node;
  int               i, j;
  plugin_catalog_t *catalog = stream->xine->plugin_catalog;
  audio_decoder_t  *ad = NULL;

  lprintf ("looking for audio decoder for streamtype %02x\n", stream_type);
  _x_assert(stream_type < DECODER_MAX);

  pthread_mutex_lock (&catalog->lock);

  for (i = 0; i < PLUGINS_PER_TYPE; i++) {

    node = catalog->audio_decoder_map[stream_type][i];

    if (!node) {
      break;
    }

    if (!node->plugin_class && !_load_plugin_class (stream->xine, node, NULL)) {
      /* remove non working plugin from catalog */
      xprintf(stream->xine, XINE_VERBOSITY_DEBUG,
	      "load_plugins: plugin %s failed to init its class.\n", node->info->id);
      for (j = i + 1; j < PLUGINS_PER_TYPE; j++)
        catalog->audio_decoder_map[stream_type][j - 1] =
          catalog->audio_decoder_map[stream_type][j];
      catalog->audio_decoder_map[stream_type][PLUGINS_PER_TYPE-1] = NULL;
      i--;
      continue;
    }

    ad = ((audio_decoder_class_t *)node->plugin_class)->open_plugin(node->plugin_class, stream);

    if (ad == (audio_decoder_t*)1) {
      /* HACK: plugin failed to instantiate because required resources are unavailable at that time,
         but may be available later, so don't remove this plugin from catalog. */
      xprintf(stream->xine, XINE_VERBOSITY_DEBUG,
          "load_plugins: plugin %s failed to instantiate, resources temporarily unavailable.\n", node->info->id);
    }
    else if (ad) {
      inc_node_ref(node);
      ad->node = node;
      xprintf(stream->xine, XINE_VERBOSITY_DEBUG,
          "load_plugins: plugin %s will be used for audio streamtype %02x.\n",
          node->info->id, stream_type);
      break;
    } else {
      /* remove non working plugin from catalog */
      xprintf(stream->xine, XINE_VERBOSITY_DEBUG,
	      "load_plugins: plugin %s failed to instantiate itself.\n", node->info->id);
      for (j = i + 1; j < PLUGINS_PER_TYPE; j++)
        catalog->audio_decoder_map[stream_type][j - 1] =
          catalog->audio_decoder_map[stream_type][j];
      catalog->audio_decoder_map[stream_type][PLUGINS_PER_TYPE-1] = NULL;
      i--;
    }
  }

  pthread_mutex_unlock (&catalog->lock);
  return ad;
}

void _x_free_audio_decoder (xine_stream_t *stream, audio_decoder_t *ad) {
  plugin_catalog_t *catalog = stream->xine->plugin_catalog;
  plugin_node_t    *node = ad->node;

  ad->dispose (ad);

  if (node) {
    pthread_mutex_lock (&catalog->lock);
    dec_node_ref(node);
    pthread_mutex_unlock (&catalog->lock);
  }
}

int _x_decoder_available (xine_t *xine, uint32_t buftype)
{
  plugin_catalog_t *catalog = xine->plugin_catalog;
  int stream_type = (buftype>>16) & 0xFF;

  _x_assert(stream_type < DECODER_MAX);

  if ( (buftype & 0xFF000000) == BUF_VIDEO_BASE ) {
    if( catalog->video_decoder_map[stream_type][0] )
      return 1;
  } else
  if ( (buftype & 0xFF000000) == BUF_AUDIO_BASE ) {
    if( catalog->audio_decoder_map[stream_type][0] )
      return 1;
  } else
  if ( (buftype & 0xFF000000) == BUF_SPU_BASE ) {
    if( catalog->spu_decoder_map[stream_type][0] )
      return 1;
  }

  return 0;
}

#ifdef LOG
static void _display_file_plugin_list (xine_list_t *list, plugin_file_t *file) {
  xine_list_iterator_t ite = NULL;
  plugin_node_t *node;
  while ((node = xine_list_next_value (list, &ite))) {
    if ((node->file == file) && (node->ref)) {
      printf("    plugin: %s, class: %p , %d instance(s)\n",
	     node->info->id, node->plugin_class, node->ref);
    }
  }
}
#endif

static void _unload_unref_plugin(xine_t *xine, plugin_node_t *node) {
  if (node->ref == 0) {
    plugin_file_t *file = node->file;

    /* no plugin of this class is instancied */
    _dispose_plugin_class(node);

    /* check file references */
    if (file && !file->ref && file->lib_handle && !file->no_unload) {
      /* unload this file */
      lprintf("unloading plugin %s\n", file->filename);
      if (dlclose(file->lib_handle)) {
        const char *error = dlerror();

        xine_log (xine, XINE_LOG_PLUGIN,
                  _("load_plugins: cannot unload plugin lib %s:\n%s\n"), file->filename, error);
      }
      file->lib_handle = NULL;
    }
  }
}

static void _unload_unref_plugins(xine_t *xine, xine_sarray_t *list) {

  plugin_node_t *node;
  int            list_id, list_size;

  list_size = xine_sarray_size (list);
  for (list_id = 0; list_id < list_size; list_id++) {

    node = xine_sarray_get (list, list_id);

    _unload_unref_plugin(xine, node);
  }
}

void xine_plugins_garbage_collector(xine_t *self) {
  plugin_catalog_t *catalog = self->plugin_catalog;
  int i;

  pthread_mutex_lock (&catalog->lock);
  for(i = 0; i < PLUGIN_TYPE_MAX; i++) {
    _unload_unref_plugins(self, self->plugin_catalog->plugin_lists[i]);
  }

#if 0
  {
    plugin_file_t *file;

    printf("\nPlugin summary after garbage collection : \n");
    file = xine_list_first_content(self->plugin_catalog->file);
    while (file) {
      if (file->ref) {
	printf("\n  file %s referenced %d time(s)\n", file->filename, file->ref);

	for(i = 0; i < PLUGIN_TYPE_MAX; i++) {
	  _display_file_plugin_list (self->plugin_catalog->plugin_lists[i], file)
	}
      }
      file = xine_list_next_content(self->plugin_catalog->file);
    }
    printf("End of plugin summary\n\n");
  }
#endif

  pthread_mutex_unlock (&catalog->lock);
}

spu_decoder_t *_x_get_spu_decoder (xine_stream_t *stream, uint8_t stream_type) {

  plugin_node_t    *node;
  int               i, j;
  plugin_catalog_t *catalog = stream->xine->plugin_catalog;
  spu_decoder_t    *sd = NULL;

  lprintf ("looking for spu decoder for streamtype %02x\n", stream_type);
  _x_assert(stream_type < DECODER_MAX);

  pthread_mutex_lock (&catalog->lock);

  for (i = 0; i < PLUGINS_PER_TYPE; i++) {

    node = catalog->spu_decoder_map[stream_type][i];

    if (!node) {
      break;
    }

    if (!node->plugin_class && !_load_plugin_class (stream->xine, node, NULL)) {
      /* remove non working plugin from catalog */
      xprintf(stream->xine, XINE_VERBOSITY_DEBUG,
	      "load_plugins: plugin %s failed to init its class.\n", node->info->id);
      for (j = i + 1; j < PLUGINS_PER_TYPE; j++)
        catalog->spu_decoder_map[stream_type][j - 1] =
          catalog->spu_decoder_map[stream_type][j];
      catalog->spu_decoder_map[stream_type][PLUGINS_PER_TYPE-1] = NULL;
      i--;
      continue;
    }

    sd = ((spu_decoder_class_t *)node->plugin_class)->open_plugin(node->plugin_class, stream);

    if (sd) {
      inc_node_ref(node);
      sd->node = node;
      xprintf(stream->xine, XINE_VERBOSITY_DEBUG,
          "load_plugins: plugin %s will be used for spu streamtype %02x.\n",
          node->info->id, stream_type);
      break;
    } else {
      /* remove non working plugin from catalog */
      xprintf(stream->xine, XINE_VERBOSITY_DEBUG,
	      "load_plugins: plugin %s failed to instantiate itself.\n", node->info->id);
      for (j = i + 1; j < PLUGINS_PER_TYPE; j++)
        catalog->spu_decoder_map[stream_type][j - 1] =
          catalog->spu_decoder_map[stream_type][j];
      catalog->spu_decoder_map[stream_type][PLUGINS_PER_TYPE-1] = NULL;
      i--;
    }
  }

  pthread_mutex_unlock (&catalog->lock);
  return sd;
}

void _x_free_spu_decoder (xine_stream_t *stream, spu_decoder_t *sd) {
  plugin_catalog_t *catalog = stream->xine->plugin_catalog;
  plugin_node_t    *node = sd->node;

  sd->dispose (sd);

  if (node) {
    pthread_mutex_lock (&catalog->lock);
    dec_node_ref(node);
    pthread_mutex_unlock (&catalog->lock);
  }
}

const char *const *xine_list_demuxer_plugins(xine_t *xine) {
  return _build_list_typed_plugins (xine, PLUGIN_DEMUX, 0);
}

const char *const *xine_list_input_plugins(xine_t *xine) {
  return _build_list_typed_plugins (xine, PLUGIN_INPUT, 0);
}

const char *const *xine_list_spu_plugins(xine_t *xine) {
  return _build_list_typed_plugins (xine, PLUGIN_SPU_DECODER, 0);
}

const char *const *xine_list_audio_decoder_plugins(xine_t *xine) {
  return _build_list_typed_plugins (xine, PLUGIN_AUDIO_DECODER, 0);
}

const char *const *xine_list_video_decoder_plugins(xine_t *xine) {
  return _build_list_typed_plugins (xine, PLUGIN_VIDEO_DECODER, 0);
}

const char *const *xine_list_post_plugins(xine_t *xine) {
  return _build_list_typed_plugins (xine, PLUGIN_POST, 0);
}

const char *const *xine_list_post_plugins_typed(xine_t *xine, uint32_t type) {
  plugin_catalog_t *catalog = xine->plugin_catalog;
  plugin_node_t    *node;
  int               i;
  int               list_id, list_size;

  pthread_mutex_lock (&catalog->lock);

  i = 0;
  list_size = xine_sarray_size (catalog->plugin_lists[PLUGIN_POST - 1]);

  for (list_id = 0; list_id < list_size; list_id++) {
    node = xine_sarray_get (catalog->plugin_lists[PLUGIN_POST - 1], list_id);
    if (((const post_info_t *)node->info->special_info)->type == type)
      catalog->ids[i++] = node->info->id;
  }
  catalog->ids[i] = NULL;

  pthread_mutex_unlock (&catalog->lock);
  return catalog->ids;
}

#define GET_PLUGIN_DESC(NAME,TYPE,CATITEM) \
  const char *xine_get_##NAME##_plugin_description (xine_t *this, const char *plugin_id) { \
    plugin_catalog_t *catalog = this->plugin_catalog;					   \
    plugin_node_t    *node;                                                                \
    int               list_id, list_size;                                                  \
    pthread_mutex_lock (&catalog->lock);                                                   \
    list_size = xine_sarray_size (catalog->plugin_lists[CATITEM - 1]);                     \
    for (list_id = 0; list_id < list_size; list_id++) {                                    \
      node = xine_sarray_get (catalog->plugin_lists[CATITEM - 1], list_id);                \
      if (!strcasecmp (node->info->id, plugin_id)) {					   \
	TYPE##_class_t *ic = (TYPE##_class_t *) node->plugin_class;			   \
        const char *ret = NULL;                                                            \
	if (!ic) {									   \
	  if (_load_plugin_class (this, node, NULL))					   \
	    ic = node->plugin_class;							   \
	}										   \
        if (ic)                                                                            \
          ret = dgettext(ic->text_domain ? ic->text_domain : XINE_TEXTDOMAIN, ic->description); \
        pthread_mutex_unlock (&catalog->lock);                                             \
        return ret;                                                                        \
      }                                                                                    \
    }                                                                                      \
    pthread_mutex_unlock (&catalog->lock);                                                 \
    return NULL;									   \
  }

GET_PLUGIN_DESC (input,		input,		PLUGIN_INPUT)
GET_PLUGIN_DESC (demux,		demux,		PLUGIN_DEMUX)
GET_PLUGIN_DESC (spu,		spu_decoder,	PLUGIN_SPU_DECODER)
GET_PLUGIN_DESC (audio,		audio_decoder,	PLUGIN_AUDIO_DECODER)
GET_PLUGIN_DESC (video,		video_decoder,	PLUGIN_VIDEO_DECODER)
GET_PLUGIN_DESC (audio_driver,	audio_driver,	PLUGIN_AUDIO_OUT)
GET_PLUGIN_DESC (video_driver,	video_driver,	PLUGIN_VIDEO_OUT)
GET_PLUGIN_DESC (post,		post,		PLUGIN_POST)

xine_post_t *xine_post_init (xine_t *xine_gen, const char *name, int inputs,
			    xine_audio_port_t **audio_target,
			    xine_video_port_t **video_target) {
  xine_private_t *xine = (xine_private_t *)xine_gen;
  plugin_catalog_t *catalog = xine->x.plugin_catalog;
  plugin_node_t    *node;
  post_plugin_t    *post = NULL;
  int               list_id, list_size;

  if( !name )
    return NULL;

  pthread_mutex_lock(&catalog->lock);

  list_size = xine_sarray_size (catalog->plugin_lists[PLUGIN_POST - 1]);

  for (list_id = 0; list_id < list_size; list_id++) {
    node = xine_sarray_get (catalog->plugin_lists[PLUGIN_POST - 1], list_id);

    if (strcmp(node->info->id, name) == 0) {

      if (!node->plugin_class && !_load_plugin_class (&xine->x, node, NULL)) {
        xprintf (&xine->x, XINE_VERBOSITY_DEBUG,
		"load_plugins: requested post plugin %s failed to load\n", name);
	break;
      }

      post = ((post_class_t *)node->plugin_class)->open_plugin(node->plugin_class,
        inputs, audio_target, video_target);

      if (post) {
	post->running_ticket = xine->port_ticket;
        post->xine = &xine->x;
	post->node = node;
	inc_node_ref(node);

	/* init the lists of announced connections */
        post->input_ids = malloc (sizeof (char *) * (xine_list_size (post->input) + 1));
        if (post->input_ids) {
          int i = 0;
          xine_list_iterator_t ite = NULL;
          xine_post_in_t *input;
          while ((input = xine_list_next_value (post->input, &ite)))
            post->input_ids[i++] = input->name;
          post->input_ids[i] = NULL;
        }
        post->output_ids = malloc (sizeof (char *) * (xine_list_size (post->output) + 1));
        if (post->output_ids) {
          int i = 0;
          xine_list_iterator_t ite = NULL;
          xine_post_out_t *output;
          while ((output = xine_list_next_value (post->output, &ite)))
            post->output_ids[i++] = output->name;
          post->output_ids[i] = NULL;
        }
	/* copy the post plugin type to the public part */
	post->xine_post.type = ((const post_info_t *)node->info->special_info)->type;

	break;
      } else {
        xprintf (&xine->x, XINE_VERBOSITY_DEBUG,
		"load_plugins: post plugin %s failed to instantiate itself\n", name);
	break;
      }
    }
  }

  pthread_mutex_unlock(&catalog->lock);

  if(post)
    return &post->xine_post;
  else {
    xprintf (&xine->x, XINE_VERBOSITY_DEBUG, "load_plugins: no post plugin named %s found\n", name);
    return NULL;
  }
}

void xine_post_dispose(xine_t *xine, xine_post_t *post_gen) {
  post_plugin_t *post = (post_plugin_t *)post_gen;
  (void)xine;
  post->dispose(post);
  /* we cannot decrement the reference counter, since post plugins can delay
   * their disposal if they are still in use => post.c handles the counting for us */
}

static char *_get_demux_strings (xine_t *self, int kind) {
  plugin_catalog_t *catalog = self->plugin_catalog;
  struct {
    const char *s;
    size_t len;
  } *slist = NULL, *slitem, *slend;
  char *res = NULL;

  pthread_mutex_lock (&catalog->lock);
  do {
    {
      int num, list_id;
      size_t size;

      num = xine_sarray_size (catalog->plugin_lists[PLUGIN_DEMUX - 1]);
      if (num <= 0)
        break;
      slist = malloc (num * sizeof (*slist));
      if (!slist)
        break;

      slitem = slist;
      size = 0;
      for (list_id = 0; list_id < num; list_id++) {
        plugin_node_t *const node = xine_sarray_get (catalog->plugin_lists[PLUGIN_DEMUX - 1], list_id);
        if (!node->plugin_class)
          _load_plugin_class (self, node, NULL);
        if (node->plugin_class) {
          demux_class_t *const cls = (demux_class_t *)node->plugin_class;
          const char *s = kind ? cls->extensions : cls->mimetypes;
          if (s) {
            slitem->s = s;
            size += (slitem->len = xine_find_byte (s, 0));
            slitem++;
          }
        }
      }
      slend = slitem;
      if (slend == slist)
        break;
      res = malloc (size + (slend - slist) * (kind ? 1 : 0) + 1);
      if (!res)
        break;
    }

    {
      char *q = res;
      slitem = slist;
      if (kind) {
        do {
          xine_small_memcpy (q, slitem->s, slitem->len);
          q += slitem->len;
          *q++ = ' ';
          slitem++;
        } while (slitem < slend);
        q[-1] = 0;
      } else {
        do {
          xine_small_memcpy (q, slitem->s, slitem->len);
          q += slitem->len;
          slitem++;
        } while (slitem < slend);
        q[0] = 0;
      }
    }
  } while (0);
  pthread_mutex_unlock (&catalog->lock);
  free (slist);
  return res;
}

/* get a list of file extensions for file types supported by xine
 * the list is separated by spaces
 *
 * the pointer returned can be free()ed when no longer used */
char *xine_get_file_extensions (xine_t *self) {
  return _get_demux_strings (self, 1);
}

/* get a list of mime types supported by xine
 *
 * the pointer returned can be free()ed when no longer used */
char *xine_get_mime_types (xine_t *self) {
  return _get_demux_strings (self, 0);
}


/* get the demuxer identifier that handles a given mime type
 *
 * the pointer returned can be free()ed when no longer used
 * returns NULL if no demuxer is available to handle this. */
char *xine_get_demux_for_mime_type (xine_t *xine, const char *mime_type) {
  uint8_t           mbuf[256];
  plugin_catalog_t *catalog;
  char             *id = NULL;
  int               list_id, list_size;

  if (!xine || !mime_type)
    return NULL;

  _mime_set (mbuf, sizeof (mbuf), mime_type);
  catalog = xine->plugin_catalog;
  pthread_mutex_lock (&catalog->lock);

  list_size = xine_sarray_size (catalog->plugin_lists[PLUGIN_DEMUX - 1]);

  for (list_id = 0; (list_id < list_size) && !id; list_id++) {
    plugin_node_t *node = xine_sarray_get (catalog->plugin_lists[PLUGIN_DEMUX - 1], list_id);

    if (node->plugin_class || _load_plugin_class (xine, node, NULL)) {
      demux_class_t *class = (demux_class_t *)node->plugin_class;

      if (_mime_find (mbuf, class->mimetypes) >= 0)
        id = strdup (node->info->id);
    }
  }

  pthread_mutex_unlock (&catalog->lock);

  return id;
}

static int dispose_plugin_list (xine_sarray_t *list, int is_cache) {

  decoder_info_t *decoder_info;
  int             list_id, list_size;
  int             num = 0;

  if (!list)
    return 0;

  list_size = xine_sarray_size (list);
  for (list_id = 0; list_id < list_size; list_id++) {

    fat_node_t *node, *nextnode;
    for (node = xine_sarray_get (list, list_id); node; node = nextnode) {

      nextnode = is_cache && (node->node.file == &node->file) ? node->nextplugin : NULL;

      if (node->node.ref == 0)
        _dispose_plugin_class (&node->node);
      else {
        lprintf ("node \"%s\" still referenced %d time(s)\n", node->node.info->id, node->node.ref);
	continue;
      }

      /* free special info */
      switch (node->info->type & PLUGIN_TYPE_MASK) {
      case PLUGIN_SPU_DECODER:
      case PLUGIN_AUDIO_DECODER:
      case PLUGIN_VIDEO_DECODER:
	decoder_info = (decoder_info_t *)node->node.info->special_info;
        if (!(IS_FAT_NODE (node) && (decoder_info->supported_types == &node->supported_types[0])))
          _x_freep (&decoder_info->supported_types);
        /* fall thru */
      default:
        if (!(IS_FAT_NODE (node) && node->node.info->special_info  == &node->ainfo))
          _x_freep (&node->node.info->special_info);
	break;
      }

      /* free info structure and string copies */
      if (!IS_FAT_NODE (node)) {
        _x_freep (&node->node.info->id);
        _x_freep (&node->node.info);
      }
#ifdef FAST_SCAN_PLUGINS
      if (node->node.config_entry_list) {
        xine_list_delete (node->node.config_entry_list);
        node->node.config_entry_list = NULL;
      }
#else
      _free_string_list (&node->node.config_entry_list);
#endif
      /* file entries in cache list are "dummies" (do not refer to opened files) */
      /* those are not in file list, so free here */
      if (is_cache && node->node.file) {
        _x_assert (node->node.file->lib_handle == NULL);
        _x_assert (node->node.file->ref == 0);
        if (node->node.file != &node->file) {
          _x_freep (&node->node.file->filename);
          _x_freep (&node->node.file);
        }
      }
      free (node);
      num++;
    }
  }
  xine_sarray_delete (list);
  return num;
}


static void dispose_plugin_file_list (xine_list_t *list) {
  plugin_file_t        *file;
  xine_list_iterator_t  ite = NULL;

  while ((file = xine_list_next_value (list, &ite))) {
    if ((char *)file + sizeof (*file) != file->filename) {
      _x_freep (&file->filename);
    }
    free (file);
  }
  xine_list_delete (list);
}


/*
 * dispose all currently loaded plugins (shutdown)
 */

void _x_dispose_plugins (xine_t *this) {

  if(this->plugin_catalog) {
    int i;

    if (this->config) {
      i = this->config->unregister_callbacks (this->config, NULL, _decoder_priority_cb, NULL, 0);
      if (i)
        xprintf (this, XINE_VERBOSITY_DEBUG,
          "load_plugins: unregistered %d decoder priority callbacks.\n", i);
    }

    for (i = 0; i < PLUGIN_TYPE_MAX; i++) {
      dispose_plugin_list (this->plugin_catalog->plugin_lists[i], 0);
    }
    dispose_plugin_list (this->plugin_catalog->modules_list, 0);

    i = dispose_plugin_list (this->plugin_catalog->cache_list, 1);
    if (i)
      xprintf (this, XINE_VERBOSITY_DEBUG,
        "load_plugins: dropped %d outdated cache entries.\n", i);
    dispose_plugin_file_list (this->plugin_catalog->file_list);

    for (i = 0; this->plugin_catalog->prio_desc[i]; i++)
      _x_freep(&this->plugin_catalog->prio_desc[i]);

    pthread_mutex_destroy(&this->plugin_catalog->lock);

    _x_freep (&this->plugin_catalog);

    {
      xine_private_t *_xine = (xine_private_t *)this;

      xine_ref_string_unref (&_xine->strings.decoder_pri_help);
    }
  }
}
