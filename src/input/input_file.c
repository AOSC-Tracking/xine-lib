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
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_DIRENT_H
#include <dirent.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#ifdef HAVE_MMAP
#include <sys/mman.h>
#endif

#define LOG_MODULE "input_file"
#define LOG_VERBOSE
/*
#define LOG
*/

#include <xine/xine_internal.h>
#include <xine/xineutils.h>
#include <xine/compat.h>
#include <xine/input_plugin.h>

#include "input_helper.h"

#define MAXFILES      65535

#ifndef WIN32
/* MS needs O_BINARY to open files, for everyone else,
 * make sure it doesn't get in the way */
#  define O_BINARY  0
#endif

typedef struct {

  input_class_t     input_class;

  xine_t           *xine;

  const char       *origin_path;

  int               mrls_allocated_entries;
  xine_mrl_t      **mrls;

} file_input_class_t;

typedef struct {
  input_plugin_t    input_plugin;

  xine_stream_t    *stream;

  int               fh;
  enum {
    FILE_STATIC = 0,
    FILE_PROGRESSIVE,
    FILE_DONE
  }                 state;
  off_t             size;
#ifdef HAVE_MMAP
  int               mmap_on;
  uint8_t          *mmap_base;
  uint8_t          *mmap_curr;
  off_t             mmap_len;
#endif
  char             *mrl;

} file_input_plugin_t;

static void file_input_size (file_input_plugin_t *this, const struct stat *sbuf) {
  if ((sbuf->st_size != this->size) &&
#ifdef HAVE_MMAP
    !this->mmap_on &&
#endif
    (this->state != FILE_PROGRESSIVE) && S_ISREG (sbuf->st_mode)) {
    this->state = FILE_PROGRESSIVE;
    xprintf (this->stream->xine, XINE_VERBOSITY_DEBUG,
      LOG_MODULE ": \"%s\" changed size, will wait for possible updates.\n", this->mrl);
  }
  this->size = sbuf->st_size;
}

static uint32_t file_input_get_capabilities (input_plugin_t *this_gen) {

  struct stat          buf ;
  file_input_plugin_t *this = (file_input_plugin_t *) this_gen;

  if (this->fh <0)
    return 0;

#ifdef _MSC_VER
    /*return INPUT_CAP_SEEKABLE | INPUT_CAP_GET_DIR;*/
	return INPUT_CAP_CLONE | INPUT_CAP_SEEKABLE;
#else
  if (fstat (this->fh, &buf) == 0) {
    file_input_size (this, &buf);
    if (S_ISREG(buf.st_mode))
      return INPUT_CAP_CLONE | INPUT_CAP_SEEKABLE;
    else
      return 0;
  } else {
    int e = errno;

    xprintf (this->stream->xine, XINE_VERBOSITY_LOG,
        LOG_MODULE ": fstat (): %s.\n", strerror (e));
  }
  return 0;
#endif /* _MSC_VER */
}

#ifdef HAVE_MMAP
/**
 * @brief Check if the file can be read through mmap().
 * @param this The instance of the input plugin to check
 *             with
 * @return 1 if the file can still be mmapped, 0 if the file
 *         changed size
 */
static int file_input_check_mmap (file_input_plugin_t *this) {
  struct stat          sbuf;

  if ( ! this->mmap_on ) return 0;

  if ( fstat (this->fh, &sbuf) != 0 ) {
    return 0;
  }

  /* If the file grew, we're most likely dealing with a timeshifting recording
   * so switch to normal access. */
  if ( this->mmap_len != sbuf.st_size ) {
    this->mmap_on = 0;

    lseek(this->fh, this->mmap_curr - this->mmap_base, SEEK_SET);
    return 0;
  }

  file_input_size (this, &sbuf);

  return 1;
}
#endif

static off_t file_input_read (input_plugin_t *this_gen, void *buf, off_t len) {
  file_input_plugin_t *this = (file_input_plugin_t *) this_gen;
  uint8_t *b;
  ssize_t r, left;
  int try;

  if (len < 0)
    return -1;

#ifdef HAVE_MMAP
  if ( file_input_check_mmap(this) ) {
    off_t l = len;
    if ( (this->mmap_curr + len) > (this->mmap_base + this->mmap_len) )
      l = (this->mmap_base + this->mmap_len) - this->mmap_curr;

    memcpy(buf, this->mmap_curr, l);
    this->mmap_curr += l;

    return l;
  }
#endif

  b = (uint8_t *)buf;
  left = len;
  r = 0;
  while (left > 0) {
    r = read (this->fh, b, left);
    if (r <= 0)
      break;
    b += r;
    left -= r;
  }
  if (r < 0)
    return r;
  r = b - (uint8_t *)buf;
  if (r == len)
    return len;

  /* poll for updates only when playing, dont delay plain xine_open (). */
  if (this->state != FILE_PROGRESSIVE)
    return r;
  if (!_x_demux_called_from (this->stream))
    return r;

  try = 2 - 1;
  while (left > 0) {
    /* _x_io_select (, this->fh, ) tends to return XIO_READY immediately. */
    if (_x_io_select (this->stream, -1, XIO_READ_READY, 1000) == XIO_ABORTED)
      break;
    r = read (this->fh, b, left);
    if (r > 0) {
      try = 2 - 1;
      b += r;
      left -= r;
    } else if (r < 0) {
      return r;
    } else if (--try < 0) {
      break;
    }
  }
  r = b - (uint8_t *)buf;
  if (r != len) {
    this->state = FILE_DONE;
    xprintf (this->stream->xine, XINE_VERBOSITY_DEBUG,
      LOG_MODULE ": \"%s\": no update, assuming real EOF.\n", this->mrl);
  }
  return r;
}

static buf_element_t *file_input_read_block (input_plugin_t *this_gen, fifo_buffer_t *fifo, off_t todo) {

#ifdef HAVE_MMAP
  file_input_plugin_t  *this = (file_input_plugin_t *) this_gen;
  if ( file_input_check_mmap(this) ) {
    buf_element_t        *buf = fifo->buffer_pool_alloc (fifo);
    off_t len = todo;

    if (todo > buf->max_size)
      todo = buf->max_size;
    if (todo < 0) {
      buf->free_buffer (buf);
      return NULL;
    }

    buf->type = BUF_DEMUX_BLOCK;

    if ( (this->mmap_curr + len) > (this->mmap_base + this->mmap_len) )
      len = (this->mmap_base + this->mmap_len) - this->mmap_curr;

    /* We use the still-mmapped file rather than copying it */
    buf->size = len;
    buf->content = this->mmap_curr;

    /* FIXME: it's completely illegal to free buffer->mem here
     * - buffer->mem has not been allocated by malloc
     * - demuxers expect buffer->mem != NULL
     */
    /* free(buf->mem); buf->mem = NULL; */

    this->mmap_curr += len;

    return buf;
  }
#endif

  return _x_input_default_read_block(this_gen, fifo, todo);
}

static off_t file_input_seek (input_plugin_t *this_gen, off_t offset, int origin) {
  file_input_plugin_t *this = (file_input_plugin_t *) this_gen;

#ifdef HAVE_MMAP /* Simulate f*() library calls */
  if ( file_input_check_mmap(this) ) {
    uint8_t *new_point = this->mmap_curr;
    switch(origin) {
    case SEEK_SET: new_point = this->mmap_base + offset; break;
    case SEEK_CUR: new_point = this->mmap_curr + offset; break;
    case SEEK_END: new_point = this->mmap_base + this->mmap_len + offset; break;
    default:
      errno = EINVAL;
      return (off_t)-1;
    }
    if ( new_point < this->mmap_base || new_point > (this->mmap_base + this->mmap_len) ) {
      errno = EINVAL;
      return (off_t)-1;
    }

    this->mmap_curr = new_point;
    return (this->mmap_curr - this->mmap_base);
  }
#endif

  return lseek (this->fh, offset, origin);
}

static off_t file_input_get_current_pos (input_plugin_t *this_gen){
  file_input_plugin_t *this = (file_input_plugin_t *) this_gen;

  if (this->fh <0)
    return 0;

#ifdef HAVE_MMAP
  if ( file_input_check_mmap(this) )
    return (this->mmap_curr - this->mmap_base);
#endif

  return lseek (this->fh, 0, SEEK_CUR);
}

static off_t file_input_get_length (input_plugin_t *this_gen) {

  struct stat          buf ;
  file_input_plugin_t *this = (file_input_plugin_t *) this_gen;

  if (this->fh <0)
    return 0;

#ifdef HAVE_MMAP
  if ( file_input_check_mmap(this) )
    return this->mmap_len;
#endif

  if (fstat (this->fh, &buf) == 0) {
    file_input_size (this, &buf);
    return buf.st_size;
  } else
    perror ("system call fstat");
  return 0;
}

/*
 * Return 1 if filepathname is a directory, otherwise 0
 */
static int file_input_is_dir (const char *filepathname) {
  struct stat  pstat;

  if (stat(filepathname, &pstat) < 0)
    return 0;

  return (S_ISDIR(pstat.st_mode));
}

static const char* file_input_get_mrl (input_plugin_t *this_gen) {
  file_input_plugin_t *this = (file_input_plugin_t *) this_gen;

  return this->mrl;
}

static void file_input_dispose (input_plugin_t *this_gen ) {
  file_input_plugin_t *this = (file_input_plugin_t *) this_gen;

#ifdef HAVE_MMAP
  /* Check for mmap_base rather than mmap_on because the file might have
   * started as a mmap() and now might be changed to descriptor-based
   * access
   */
  if ( this->mmap_base )
    munmap(this->mmap_base, this->mmap_len);
#endif

  if (this->fh != -1)
    close(this->fh);

  _x_freep (&this->mrl);

  free (this);
}

static char *file_input_decode_uri (char *uri) {
  uri = strdup(uri);
  if (uri)
    _x_mrl_unescape (uri);
  return uri;
}

static int file_input_open (input_plugin_t *this_gen ) {
  file_input_plugin_t *this = (file_input_plugin_t *) this_gen;
  char                *filename;
  struct stat          sbuf;
  int                  sres;

  lprintf("file_input_open\n");

  if (strncasecmp (this->mrl, "file:/", 6) == 0)
  {
    if (strncasecmp (this->mrl, "file://localhost/", 16) == 0)
      filename = file_input_decode_uri(&(this->mrl[16]));
    else if (strncasecmp (this->mrl, "file://127.0.0.1/", 16) == 0)
      filename = file_input_decode_uri(&(this->mrl[16]));
    else
      filename = file_input_decode_uri(&(this->mrl[5]));
  }
  else
    filename = strdup(this->mrl); /* NEVER unescape plain file names! */
  if (!filename)
    return -1;

  this->fh = xine_open_cloexec (filename, O_RDONLY | O_BINARY);
  _x_freep (&filename);
  if (this->fh == -1) {
    if (errno == EACCES) {
      _x_message(this->stream, XINE_MSG_PERMISSION_ERROR, this->mrl, NULL);
      xine_log (this->stream->xine, XINE_LOG_MSG,
                _("input_file: Permission denied: >%s<\n"), this->mrl);
    } else if (errno == ENOENT) {
      _x_message(this->stream, XINE_MSG_FILE_NOT_FOUND, this->mrl, NULL);
      xine_log (this->stream->xine, XINE_LOG_MSG,
                _("input_file: File not found: >%s<\n"), this->mrl);
    }
    return -1;
  }

  sres = fstat (this->fh, &sbuf);

  if (!sres) {
    this->size = sbuf.st_size;
    if (S_ISREG (sbuf.st_mode)) {
      time_t now = time (NULL);

      if ((sbuf.st_mtime <= now) && (sbuf.st_mtime + 10 >= now)) {
        this->state = FILE_PROGRESSIVE;
        xprintf (this->stream->xine, XINE_VERBOSITY_DEBUG,
          LOG_MODULE ": \"%s\" is quite recent, will wait for possible updates.\n", this->mrl);
      }
    }
  }

#ifdef HAVE_MMAP
  this->mmap_on = 0;
  this->mmap_base = NULL;
  this->mmap_curr = NULL;
  this->mmap_len = 0;
#endif

  /* don't check length of fifo or character device node */
  if (!sres && !S_ISREG (sbuf.st_mode))
    return 1;

#ifdef HAVE_MMAP
  this->mmap_base = NULL;
  do {
    uint8_t *mmap_base;
    size_t tmp_size;
    /* may cause truncation - if it does, DON'T mmap! */
    tmp_size = (size_t)sbuf.st_size;
    if ((off_t)tmp_size != sbuf.st_size)
      break;
    mmap_base = mmap (NULL, tmp_size, PROT_READ, MAP_SHARED, this->fh, 0);
    if (mmap_base == (void*)-1)
      break;
    /* paranoia */
    sres = fstat (this->fh, &sbuf);
    if (sres || (off_t)tmp_size != sbuf.st_size) {
      munmap (mmap_base, tmp_size);
      break;
    }
    this->mmap_on = 1;
    this->mmap_base =
    this->mmap_curr = mmap_base;
    this->mmap_len = sbuf.st_size;
  } while (0);
#endif

  if (file_input_get_length (this_gen) == 0) {
      _x_message(this->stream, XINE_MSG_FILE_EMPTY, this->mrl, NULL);
      close (this->fh);
      this->fh = -1;
      xine_log (this->stream->xine, XINE_LOG_MSG,
		_("input_file: File empty: >%s<\n"), this->mrl);
      return -1;
  }

  return 1;
}

static int file_input_get_optional_data (input_plugin_t *this_gen, void *data, int data_type);

static input_plugin_t *file_input_get_instance (input_class_t *cls_gen, xine_stream_t *stream,
                                                const char *mrl) {

  /* file_input_class_t  *cls = (file_input_class_t *) cls_gen; */
  file_input_plugin_t *this;

  lprintf("file_input_get_instance\n");

  if ((strncasecmp (mrl, "file:", 5)) && strstr (mrl, ":/") && (strstr (mrl, ":/") < strchr(mrl, '/'))) {
    return NULL;
  }

  this = (file_input_plugin_t *) calloc(1, sizeof (file_input_plugin_t));
  if (!this)
    return NULL;

  this->stream = stream;
  this->mrl    = strdup(mrl);
  this->fh     = -1;
  this->state  = FILE_STATIC;
  this->size   = 0;

  this->input_plugin.open               = file_input_open;
  this->input_plugin.get_capabilities   = file_input_get_capabilities;
  this->input_plugin.read               = file_input_read;
  this->input_plugin.read_block         = file_input_read_block;
  this->input_plugin.seek               = file_input_seek;
  this->input_plugin.get_current_pos    = file_input_get_current_pos;
  this->input_plugin.get_length         = file_input_get_length;
  this->input_plugin.get_blocksize      = _x_input_default_get_blocksize;
  this->input_plugin.get_mrl            = file_input_get_mrl;
  this->input_plugin.get_optional_data  = file_input_get_optional_data;
  this->input_plugin.dispose            = file_input_dispose;
  this->input_plugin.input_class        = cls_gen;

  if (!this->mrl) {
    free(this);
    return NULL;
  }

  return &this->input_plugin;
}

static int file_input_get_optional_data (input_plugin_t *this_gen, void *data, int data_type) {
  if ((data_type == INPUT_OPTIONAL_DATA_CLONE) && data) {
    file_input_plugin_t *this = (file_input_plugin_t *) this_gen;
    input_plugin_t *new = file_input_get_instance (this->input_plugin.input_class, this->stream, this->mrl);
    if (new) {
      if (new->open (new) < 0) {
        new->dispose (new);
      } else {
        input_plugin_t **q = data;
        *q = new;
        return INPUT_OPTIONAL_SUCCESS;
      }
    }
  }
  return INPUT_OPTIONAL_UNSUPPORTED;
}


/*
 * plugin class functions
 */

#ifndef S_ISLNK
#define S_ISLNK(mode)  0
#endif
#ifndef S_ISFIFO
#define S_ISFIFO(mode) 0
#endif
#ifndef S_ISSOCK
#define S_ISSOCK(mode) 0
#endif
#ifndef S_ISCHR
#define S_ISCHR(mode)  0
#endif
#ifndef S_ISBLK
#define S_ISBLK(mode)  0
#endif
#ifndef S_ISREG
#define S_ISREG(mode)  0
#endif
#if !S_IXUGO
#define S_IXUGO        (S_IXUSR | S_IXGRP | S_IXOTH)
#endif

/*
 * Callback for config changes.
 */

static void file_input_origin_change_cb (void *data, xine_cfg_entry_t *cfg) {
  file_input_class_t *this = (file_input_class_t *) data;

  this->origin_path = cfg->str_value;
}

/*
 * Sorting function, it comes from GNU fileutils package.
 */
#define S_N        0x0
#define S_I        0x4
#define S_F        0x8
#define S_Z        0xC
#define CMP          2
#define LEN          3
#define ISDIGIT(c)   ((unsigned) (c) - '0' <= 9)
static int file_input_strverscmp (const char *s1, const char *s2) {
  const unsigned char *p1 = (const unsigned char *) s1;
  const unsigned char *p2 = (const unsigned char *) s2;
  unsigned char c1, c2;
  int state;
  int diff;
  static const unsigned int next_state[] = {
    S_N, S_I, S_Z, S_N,
    S_N, S_I, S_I, S_I,
    S_N, S_F, S_F, S_F,
    S_N, S_F, S_Z, S_Z
  };
  static const int result_type[] = {
    CMP, CMP, CMP, CMP, CMP, LEN, CMP, CMP,
    CMP, CMP, CMP, CMP, CMP, CMP, CMP, CMP,
    CMP,  -1,  -1, CMP,   1, LEN, LEN, CMP,
      1, LEN, LEN, CMP, CMP, CMP, CMP, CMP,
    CMP, CMP, CMP, CMP, CMP, LEN, CMP, CMP,
    CMP, CMP, CMP, CMP, CMP, CMP, CMP, CMP,
    CMP,   1,   1, CMP,  -1, CMP, CMP, CMP,
     -1, CMP, CMP, CMP
  };

  if(p1 == p2)
    return 0;

  c1 = *p1++;
  c2 = *p2++;

  state = S_N | ((c1 == '0') + (ISDIGIT(c1) != 0));

  while((diff = c1 - c2) == 0 && c1 != '\0') {
    state = next_state[state];
    c1 = *p1++;
    c2 = *p2++;
    state |= (c1 == '0') + (ISDIGIT(c1) != 0);
  }

  state = result_type[state << 2 | ((c2 == '0') + (ISDIGIT(c2) != 0))];

  switch(state) {
  case CMP:
    return diff;

  case LEN:
    while(ISDIGIT(*p1++))
      if(!ISDIGIT(*p2++))
	return 1;

    return ISDIGIT(*p2) ? -1 : diff;

  default:
    return state;
  }
}

/*
 * Wrapper to file_input_strverscmp() for qsort() calls, which sort mrl_t type array.
 */
static int file_input_sortfiles_default (const xine_mrl_t *s1, const xine_mrl_t *s2) {
  return(file_input_strverscmp(s1->mrl, s2->mrl));
}

/*
 * Return the type (OR'ed) of the given file *fully named*
 */
static uint32_t file_input_get_file_type (char *filepathname, char *origin, xine_t *xine) {
  struct stat  pstat;
  int          mode;
  uint32_t     file_type = 0;
  char         buf[XINE_PATH_MAX + XINE_NAME_MAX + 1];

  (void)xine;
  if((lstat(filepathname, &pstat)) < 0) {
    snprintf_buf(buf, "%s/%s", origin, filepathname);
    if((lstat(buf, &pstat)) < 0) {
      lprintf ("lstat failed for %s{%s}\n", filepathname, origin);
      file_type |= mrl_unknown;
      return file_type;
    }
  }

  file_type |= mrl_file;

  mode = pstat.st_mode;

  if(S_ISLNK(mode))
    file_type |= mrl_file_symlink;
  else if(S_ISDIR(mode))
    file_type |= mrl_file_directory;
  else if(S_ISCHR(mode))
    file_type |= mrl_file_chardev;
  else if(S_ISBLK(mode))
    file_type |= mrl_file_blockdev;
  else if(S_ISFIFO(mode))
    file_type |= mrl_file_fifo;
  else if(S_ISSOCK(mode))
    file_type |= mrl_file_sock;
  else {
    if(S_ISREG(mode)) {
      file_type |= mrl_file_normal;
    }
    if(mode & S_IXUGO)
      file_type |= mrl_file_exec;
  }

  if(filepathname[strlen(filepathname) - 1] == '~')
    file_type |= mrl_file_backup;

  return file_type;
}

/*
 * Return the file size of the given file *fully named*
 */
static off_t file_input_get_file_size (const char *filepathname, const char *origin) {
  struct stat  pstat;
  char         buf[XINE_PATH_MAX * 2 + XINE_NAME_MAX + 3];

  if((lstat(filepathname, &pstat)) < 0) {
    snprintf_buf(buf, "%s/%s", origin, filepathname);
    if((lstat(buf, &pstat)) < 0)
      return (off_t) 0;
  }

  return pstat.st_size;
}

static xine_mrl_t **file_input_class_get_dir (input_class_t *this_gen, const char *filename, int *nFiles) {

  /* FIXME: this code needs cleanup badly */

  file_input_class_t   *this = (file_input_class_t *) this_gen;
  struct dirent        *pdirent;
  DIR                  *pdir;
  xine_mrl_t           *hide_files, *dir_files, *norm_files;
  char                  current_dir[XINE_PATH_MAX + 1];
  char                  current_dir_slashed[XINE_PATH_MAX + 2];
  char                  fullfilename[XINE_PATH_MAX + XINE_NAME_MAX + 2];
  int                   num_hide_files  = 0;
  int                   num_dir_files   = 0;
  int                   num_norm_files  = 0;
  int                   num_files       = -1;
  int                 (*func) ()        = file_input_sortfiles_default;
  int                   already_tried   = 0;
  int                   show_hidden_files;

  *nFiles = 0;
  memset(current_dir, 0, sizeof(current_dir));

  show_hidden_files = _x_input_get_show_hidden_files(this->xine->config);

  /*
   * No origin location, so got the content of the current directory
   */
  if(!filename) {
    snprintf_buf(current_dir, "%s", this->origin_path);
  }
  else {
    snprintf_buf(current_dir, "%s", filename);

    /* Remove exceed '/' */
    while((current_dir[strlen(current_dir) - 1] == '/') && strlen(current_dir) > 1)
      current_dir[strlen(current_dir) - 1] = '\0';
  }

  /* Store new origin path */
 try_again_from_home:

  this->xine->config->update_string(this->xine->config, "media.files.origin_path", current_dir);

  if(strcasecmp(current_dir, "/"))
    snprintf_buf(current_dir_slashed, "%s/", current_dir);
  else
    strcpy(current_dir_slashed, "/");

  /*
   * Ooch!
   */
  if((pdir = opendir(current_dir)) == NULL) {

    if(!already_tried) {
      /* Try one more time with user homedir */
      snprintf_buf(current_dir, "%s", xine_get_homedir());
      already_tried++;
      goto try_again_from_home;
    }

    return NULL;
  }

  dir_files  = (xine_mrl_t *) calloc(MAXFILES, sizeof(xine_mrl_t));
  hide_files = (xine_mrl_t *) calloc(MAXFILES, sizeof(xine_mrl_t));
  norm_files = (xine_mrl_t *) calloc(MAXFILES, sizeof(xine_mrl_t));

  if (dir_files && hide_files && norm_files)
  while((pdirent = readdir(pdir)) != NULL) {

    memset(fullfilename, 0, sizeof(fullfilename));
    snprintf_buf(fullfilename, "%s/%s", current_dir, pdirent->d_name);

    if(file_input_is_dir(fullfilename)) {

      /* if user don't want to see hidden files, ignore them */
      if (show_hidden_files == 0 &&
	 ((strlen(pdirent->d_name) > 1)
	  && (pdirent->d_name[0] == '.' &&  pdirent->d_name[1] != '.'))) {
	;
      }
      else {

	dir_files[num_dir_files].origin = strdup(current_dir);
	dir_files[num_dir_files].mrl    = _x_asprintf("%s%s", current_dir_slashed, pdirent->d_name);
	dir_files[num_dir_files].link   = NULL;
	dir_files[num_dir_files].type   = file_input_get_file_type(fullfilename, current_dir, this->xine);
	dir_files[num_dir_files].size   = file_input_get_file_size(fullfilename, current_dir);

	/* The file is a link, follow it */
	if(dir_files[num_dir_files].type & mrl_file_symlink) {
	  char linkbuf[XINE_PATH_MAX + XINE_NAME_MAX + 1];
	  int linksize;

	  memset(linkbuf, 0, sizeof(linkbuf));
	  linksize = readlink(fullfilename, linkbuf, XINE_PATH_MAX + XINE_NAME_MAX);

	  if(linksize < 0)
	    xprintf (this->xine, XINE_VERBOSITY_DEBUG,
		     "input_file: readlink() failed: %s\n", strerror(errno));
	  else {
	    dir_files[num_dir_files].link =
	      strndup(linkbuf, linksize);

	    dir_files[num_dir_files].type |= file_input_get_file_type(dir_files[num_dir_files].link, current_dir, this->xine);
	  }
	}

	num_dir_files++;
      }

    } /* Hmmmm, an hidden file ? */
    else if((strlen(pdirent->d_name) > 1)
	    && (pdirent->d_name[0] == '.' &&  pdirent->d_name[1] != '.')) {

      /* if user don't want to see hidden files, ignore them */
      if (show_hidden_files) {

	hide_files[num_hide_files].origin = strdup(current_dir);
	hide_files[num_hide_files].mrl    = _x_asprintf("%s%s", current_dir_slashed, pdirent->d_name);
	hide_files[num_hide_files].link   = NULL;
	hide_files[num_hide_files].type   = file_input_get_file_type(fullfilename, current_dir, this->xine);
	hide_files[num_hide_files].size   = file_input_get_file_size(fullfilename, current_dir);

	/* The file is a link, follow it */
	if(hide_files[num_hide_files].type & mrl_file_symlink) {
	  char linkbuf[XINE_PATH_MAX + XINE_NAME_MAX + 1];
	  int linksize;

	  memset(linkbuf, 0, sizeof(linkbuf));
	  linksize = readlink(fullfilename, linkbuf, XINE_PATH_MAX + XINE_NAME_MAX);

	  if(linksize < 0) {
	    xprintf (this->xine, XINE_VERBOSITY_DEBUG,
		     "input_file: readlink() failed: %s\n", strerror(errno));
	  }
	  else {
	    hide_files[num_hide_files].link =
	      strndup(linkbuf, linksize);
	    hide_files[num_hide_files].type |= file_input_get_file_type(hide_files[num_hide_files].link, current_dir, this->xine);
	  }
	}

	num_hide_files++;
      }

    } /* So a *normal* one. */
    else {

      norm_files[num_norm_files].origin = strdup(current_dir);
      norm_files[num_norm_files].mrl    = _x_asprintf("%s%s", current_dir_slashed, pdirent->d_name);
      norm_files[num_norm_files].link   = NULL;
      norm_files[num_norm_files].type   = file_input_get_file_type(fullfilename, current_dir, this->xine);
      norm_files[num_norm_files].size   = file_input_get_file_size(fullfilename, current_dir);

      /* The file is a link, follow it */
      if(norm_files[num_norm_files].type & mrl_file_symlink) {
	char linkbuf[XINE_PATH_MAX + XINE_NAME_MAX + 1];
	int linksize;

	memset(linkbuf, 0, sizeof(linkbuf));
	linksize = readlink(fullfilename, linkbuf, XINE_PATH_MAX + XINE_NAME_MAX);

	if(linksize < 0) {
	  xprintf (this->xine, XINE_VERBOSITY_DEBUG,
		   "input_file: readlink() failed: %s\n", strerror(errno));
	}
	else {
	  norm_files[num_norm_files].link =
	    strndup(linkbuf, linksize);
	  norm_files[num_norm_files].type |= file_input_get_file_type(norm_files[num_norm_files].link, current_dir, this->xine);
	}
      }

      num_norm_files++;
    }

    num_files++;
  }

  closedir(pdir);

  /*
   * Ok, there are some files here, so sort
   * them then store them into global mrls array.
   */
  if(num_files > 0) {
    int i;

    num_files = 0;

    /*
     * Sort arrays
     */
    if(num_dir_files)
      qsort(dir_files, num_dir_files, sizeof(xine_mrl_t), func);

    if(num_hide_files)
      qsort(hide_files, num_hide_files, sizeof(xine_mrl_t), func);

    if(num_norm_files)
      qsort(norm_files, num_norm_files, sizeof(xine_mrl_t), func);

    /*
     * Add directories entries
     */
    for(i = 0; i < num_dir_files; i++) {

      if(num_files >= this->mrls_allocated_entries) {
	++this->mrls_allocated_entries;
	this->mrls = realloc(this->mrls, (this->mrls_allocated_entries+1) * sizeof(xine_mrl_t*));
	this->mrls[num_files] = calloc(1, sizeof(xine_mrl_t));
      }
      else
	MRL_ZERO(this->mrls[num_files]);

      *(this->mrls[num_files]) = dir_files[i];

      num_files++;
    }

    /*
     * Add hidden files entries
     */
    for(i = 0; i < num_hide_files; i++) {

      if(num_files >= this->mrls_allocated_entries) {
	++this->mrls_allocated_entries;
	this->mrls = realloc(this->mrls, (this->mrls_allocated_entries+1) * sizeof(xine_mrl_t*));
	this->mrls[num_files] = calloc(1, sizeof(xine_mrl_t));
      }
      else
	MRL_ZERO(this->mrls[num_files]);

      *(this->mrls[num_files]) = hide_files[i];

      num_files++;
    }

    /*
     * Add other files entries
     */
    for(i = 0; i < num_norm_files; i++) {

      if(num_files >= this->mrls_allocated_entries) {
	++this->mrls_allocated_entries;
	this->mrls = realloc(this->mrls, (this->mrls_allocated_entries+1) * sizeof(xine_mrl_t*));
	this->mrls[num_files] = calloc(1, sizeof(xine_mrl_t));
      }
      else
	MRL_ZERO(this->mrls[num_files]);

      *(this->mrls[num_files]) = norm_files[i];

      num_files++;
    }

    /* Some cleanups before leaving */
    free(dir_files);
    free(hide_files);
    free(norm_files);
  }
  else {
    free(hide_files);
    free(dir_files);
    free(norm_files);
    return NULL;
  }

  /*
   * Inform caller about files found number.
   */
  *nFiles = num_files;

  /*
   * Freeing exceeded mrls if exists.
   */
  while(this->mrls_allocated_entries > num_files) {
    this->mrls_allocated_entries--;
    MRL_ZERO(this->mrls[this->mrls_allocated_entries]);
    _x_freep(&this->mrls[this->mrls_allocated_entries]);
  }

  /*
   * This is useful to let UI know where it should stops ;-).
   */
  this->mrls[num_files] = NULL;

  /*
   * Some debugging info
   */
  /*
  {
    int j = 0;
    while(this->mrls[j]) {
      printf("mrl[%d] = '%s'\n", j, this->mrls[j]->mrl);
      j++;
    }
  }
  */

  return this->mrls;
}

static void file_input_class_dispose (input_class_t *this_gen) {
  file_input_class_t  *this = (file_input_class_t *) this_gen;
  config_values_t     *config = this->xine->config;

  config->unregister_callbacks (config, NULL, NULL, this, sizeof (*this));

  while(this->mrls_allocated_entries) {
    this->mrls_allocated_entries--;
    MRL_ZERO(this->mrls[this->mrls_allocated_entries]);
    _x_freep(&this->mrls[this->mrls_allocated_entries]);
  }
  _x_freep (&this->mrls);

  free (this);
}

static void *file_input_init_plugin (xine_t *xine, const void *data) {

  file_input_class_t  *this;
  config_values_t     *config;

  (void)data;
  this = (file_input_class_t *) calloc(1, sizeof (file_input_class_t));
  if (!this)
    return NULL;

  this->xine   = xine;
  config       = xine->config;

  this->input_class.get_instance       = file_input_get_instance;
  this->input_class.identifier         = "file";
  this->input_class.description        = N_("file input plugin");
  this->input_class.get_dir            = file_input_class_get_dir;
  this->input_class.get_autoplay_list  = NULL;
  this->input_class.dispose            = file_input_class_dispose;
  this->input_class.eject_media        = NULL;

  this->mrls = (xine_mrl_t **) calloc(1, sizeof(xine_mrl_t*));
  this->mrls_allocated_entries = 0;

  {
    char current_dir[XINE_PATH_MAX + 1];

    if(getcwd(current_dir, sizeof(current_dir)) == NULL)
      strcpy(current_dir, ".");

    this->origin_path = config->register_filename(config, "media.files.origin_path",
						current_dir, XINE_CONFIG_STRING_IS_DIRECTORY_NAME,
						_("file browsing start location"),
						_("The browser to select the file to play will "
						  "start at this location."),
						0, file_input_origin_change_cb, (void *) this);
  }

  _x_input_register_show_hidden_files(config);

  return this;
}

/*
 * exported plugin catalog entry
 */

#define INPUT_FILE_CATALOG { PLUGIN_INPUT | PLUGIN_MUST_PRELOAD, 18, "FILE", XINE_VERSION_CODE, NULL, file_input_init_plugin }

#ifndef XINE_MAKE_BUILTINS
const plugin_info_t xine_plugin_info[] EXPORTED = {
  /* type, API, "name", version, special_info, init_function */
  INPUT_FILE_CATALOG,
  { PLUGIN_NONE, 0, NULL, 0, NULL, NULL }
};
#endif
