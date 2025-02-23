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
#ifndef XINEUTILS_H
#define XINEUTILS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
#include <pthread.h>

#include <time.h>
#ifdef WIN32
#else
#  include <sys/time.h>
#endif
#include <xine/os_types.h>
#include <xine/attributes.h>
#include <xine/compat.h>
#include <xine/xmlparser.h>
#include <xine/xine_buffer.h>
#include <xine/configfile.h>
#include <xine/list.h>
#include <xine/array.h>
#include <xine/sorted_array.h>

#include <stdio.h>
#include <string.h>

/*
 * Mark exported data symbols for link engine library clients with older
 * Win32 compilers
 */
#if defined(WIN32) && !defined(XINE_LIBRARY_COMPILE)
#  define DL_IMPORT __declspec(dllimport)
#  define extern DL_IMPORT extern
#endif


/* Amiga style doubly linked lists, taken from TJtools.
 * Most compilers will support the straightforward aliasing safe version.
 * For others, try that "volatile" hack. */

typedef struct dnode_st {
  struct dnode_st *next, *prev;
} dnode_t;

#ifdef HAVE_NAMELESS_STRUCT_IN_UNION
#  define DLIST_H(l) (&(l)->h)
#  define DLIST_T(l) (&(l)->t)
typedef union {
  struct { dnode_t *head, *null, *tail; };
  struct { dnode_t h; dnode_t *dummy1;  };
  struct { dnode_t *dummy2; dnode_t t;  };
} dlist_t;
#else
#  define DLIST_H(l) ((void *)(&(l)->head))
#  define DLIST_T(l) ((void *)(&(l)->null))
typedef struct {
  dnode_t * volatile head;
  dnode_t *null;
  dnode_t * volatile tail;
} dlist_t;
#endif

#define DLIST_IS_EMPTY(l) ((l)->head == DLIST_T(l))

#define DLIST_REMOVE(n) { \
  dnode_t *dl_rm_this = n; \
  dnode_t *dl_rm_prev = dl_rm_this->prev; \
  dnode_t *dl_rm_next = dl_rm_this->next; \
  dl_rm_next->prev = dl_rm_prev; \
  dl_rm_prev->next = dl_rm_next; \
}

#define DLIST_ADD_HEAD(n,l) { \
  dlist_t *dl_ah_list = l; \
  dnode_t *dl_ah_node = n; \
  dnode_t *dl_ah_head = dl_ah_list->head; \
  dl_ah_node->next = dl_ah_head; \
  dl_ah_node->prev = DLIST_H(dl_ah_list); \
  dl_ah_list->head = dl_ah_node; \
  dl_ah_head->prev = dl_ah_node; \
}

#define DLIST_ADD_TAIL(n,l) { \
  dlist_t *dl_at_list = l; \
  dnode_t *dl_at_node = n; \
  dnode_t *dl_at_tail = dl_at_list->tail; \
  dl_at_node->next = DLIST_T(dl_at_list); \
  dl_at_node->prev = dl_at_tail; \
  dl_at_tail->next = dl_at_node; \
  dl_at_list->tail = dl_at_node; \
}

#define DLIST_INSERT(n,h) { \
  dnode_t *dl_i_node = n; \
  dnode_t *dl_i_here = h; \
  dnode_t *dl_i_prev = dl_i_here->prev; \
  dl_i_prev->next = dl_i_node; \
  dl_i_here->prev = dl_i_node; \
  dl_i_node->next = dl_i_here; \
  dl_i_node->prev = dl_i_prev; \
}

#define DLIST_INIT(l) { \
  dlist_t *dl_in_list = l; \
  dl_in_list->head = DLIST_T(dl_in_list); \
  dl_in_list->null = NULL; \
  dl_in_list->tail = DLIST_H(dl_in_list); }


  /*
   * debugable mutexes
   */

  typedef struct {
    pthread_mutex_t  mutex;
    char             id[80];
    char            *locked_by;
  } xine_mutex_t;

  int xine_mutex_init    (xine_mutex_t *mutex, const pthread_mutexattr_t *mutexattr,
			  const char *id) XINE_PROTECTED;

  int xine_mutex_lock    (xine_mutex_t *mutex, const char *who) XINE_PROTECTED;
  int xine_mutex_unlock  (xine_mutex_t *mutex, const char *who) XINE_PROTECTED;
  int xine_mutex_destroy (xine_mutex_t *mutex) XINE_PROTECTED;



			/* CPU Acceleration */

/*
 * The type of an value that fits in an MMX register (note that long
 * long constant values MUST be suffixed by LL and unsigned long long
 * values by ULL, lest they be truncated by the compiler)
 */

/* generic accelerations */
#define MM_ACCEL_MLIB           0x00000001

/* x86 accelerations */
#define MM_ACCEL_X86_MMX        0x80000000
#define MM_ACCEL_X86_3DNOW      0x40000000
#define MM_ACCEL_X86_MMXEXT     0x20000000
#define MM_ACCEL_X86_SSE        0x10000000
#define MM_ACCEL_X86_SSE2       0x08000000
#define MM_ACCEL_X86_SSE3       0x04000000
#define MM_ACCEL_X86_SSSE3      0x02000000
#define MM_ACCEL_X86_SSE4       0x01000000
#define MM_ACCEL_X86_SSE42      0x00800000
#define MM_ACCEL_X86_AVX        0x00400000

/* powerpc accelerations and features */
#define MM_ACCEL_PPC_ALTIVEC    0x04000000
#define MM_ACCEL_PPC_CACHE32    0x02000000

/* SPARC accelerations */

#define MM_ACCEL_SPARC_VIS      0x01000000
#define MM_ACCEL_SPARC_VIS2     0x00800000

/* x86 compat defines */
#define MM_MMX                  MM_ACCEL_X86_MMX
#define MM_3DNOW                MM_ACCEL_X86_3DNOW
#define MM_MMXEXT               MM_ACCEL_X86_MMXEXT
#define MM_SSE                  MM_ACCEL_X86_SSE
#define MM_SSE2                 MM_ACCEL_X86_SSE2

uint32_t xine_mm_accel (void) XINE_CONST XINE_PROTECTED;

int xine_cpu_count(void) XINE_CONST XINE_PROTECTED;


		     /* Optimized/fast memcpy */

extern void *(* xine_fast_memcpy)(void *to, const void *from, size_t len) XINE_PROTECTED;

/* len (usually) < 500, but not a build time constant. */
#define xine_small_memcpy(xsm_to,xsm_from,xsm_len) memcpy (xsm_to, xsm_from, xsm_len)

#if (defined(__GNUC__) && (__GNUC__ >= 4)) || defined(__clang__)
#  if defined(ARCH_X86)
#    undef xine_small_memcpy
static inline void *xine_small_memcpy (void *to, const void *from, size_t len) {
  void *t2 = to;
  size_t l2 = len;
#    if !defined(__clang__) && !defined(__cplusplus)
  __asm__ __volatile__ (
    "cld\n\trep movsb"
    : "=S" (from), "=D" (t2), "=c" (l2), "=m" (*(struct {char foo[len];} *)to)
    : "0"  (from), "1"  (t2), "2"  (l2)
    : "cc"
  );
#    else /* clang dislikes virtual variable size struct */
  __asm__ __volatile__ (
      "cld\n\trep movsb"
      : "=S" (from), "=D" (t2), "=c" (l2)
      : "0"  (from), "1"  (t2), "2"  (l2)
      : "cc", "memory"
  );
#    endif
  (void)from;
  (void)t2;
  (void)l2;
  return to;
}
#  endif
#endif

/*
 * Debug stuff
 */
/*
 * profiling (unworkable in non DEBUG isn't defined)
 */
void xine_profiler_init (void) XINE_PROTECTED;
int xine_profiler_allocate_slot (const char *label) XINE_PROTECTED;
void xine_profiler_start_count (int id) XINE_PROTECTED;
void xine_profiler_stop_count (int id) XINE_PROTECTED;
void xine_profiler_print_results (void) XINE_PROTECTED;

/*
 * xine_container_of()
 * calculate struct pointer from field pointer
 */

#if defined(__GNUC__)
#  define xine_container_of(ptr, type, member)           \
  ({                                                     \
     const typeof(((type *)0)->member) *__mptr = (ptr);  \
     (type *)(void *)((char *)__mptr - offsetof(type, member));  \
  })
#else
#  define xine_container_of(ptr, type, member) \
  ((type *)(void *)((char *)(1 ? (ptr) : &((type *)0)->member) - offsetof(type, member)))
#endif

/*
 * Allocate and clean memory size_t 'size', then return the pointer
 * to the allocated memory.
 */
void *xine_xmalloc(size_t size) XINE_MALLOC XINE_DEPRECATED XINE_PROTECTED;

void *xine_xcalloc(size_t nmemb, size_t size) XINE_MALLOC XINE_PROTECTED;

/*
 * Free allocated memory and set pointer to NULL
 * @param ptr Pointer to the pointer to the memory block which should be freed.
 */
static inline void _x_freep(void *ptr) {
  void **p = (void **)ptr;
  free (*p);
  *p = NULL;
}

static inline void _x_freep_wipe_string(char **pp) {
  char *p = *pp;
  while (p && *p)
    *p++ = 0;
  _x_freep(pp);
}

/*
 * Copy blocks of memory.
 */
void *xine_memdup (const void *src, size_t length) XINE_PROTECTED;
void *xine_memdup0 (const void *src, size_t length) XINE_PROTECTED;

/**
 * Get/resize/free aligned memory.
 */

#ifndef XINE_MEM_ALIGN
#  define XINE_MEM_ALIGN 32
#endif

void *xine_mallocz_aligned (size_t size)            XINE_PROTECTED XINE_MALLOC;
void *xine_malloc_aligned  (size_t size)            XINE_PROTECTED XINE_MALLOC;
void  xine_free_aligned    (void *ptr)              XINE_PROTECTED;
void *xine_realloc_aligned (void *ptr, size_t size) XINE_PROTECTED;
#define xine_freep_aligned(xinefreepptr) do {xine_free_aligned (*(xinefreepptr)); *(xinefreepptr) = NULL; } while (0)

/**
 * Base64 encoder.
 * from: pointer to binary input.
 * to:   pointer to output string buffer.
 * size: byte length of input.
 * ret:  length of output string (without \0).
 * Note that both buffers need 4 writable padding bytes.
 */
size_t xine_base64_encode (uint8_t *from, char *to, size_t size) XINE_PROTECTED;
/**
 * Base64 decoder.
 * from: pointer to input string or line formatted / indented, null terminated text.
 * to:   pointer to output buffer.
 * ret:  length of output in bytes.
 */
size_t xine_base64_decode (const char *from, uint8_t *to) XINE_PROTECTED;

/**
 * Checksum calculator.
 */
uint32_t xine_crc32_ieee (uint32_t crc, const uint8_t *data, size_t len) XINE_PROTECTED;
uint32_t xine_crc16_ansi (uint32_t crc, const uint8_t *data, size_t len) XINE_PROTECTED;

/*
 * Get user home directory.
 */
const char *xine_get_homedir(void) XINE_PROTECTED;

#if defined(WIN32) || defined(__CYGWIN__)
/*
 * Get other xine directories.
 */
const char *xine_get_pluginroot(void) XINE_PROTECTED;
const char *xine_get_plugindir(void) XINE_PROTECTED;
const char *xine_get_fontdir(void) XINE_PROTECTED;
const char *xine_get_localedir(void) XINE_PROTECTED;
#endif

/*
 * Clean a string (remove spaces and '=' at the begin,
 * and '\n', '\r' and spaces at the end.
 */
char *xine_chomp (char *str) XINE_PROTECTED;

/*
 * A thread-safe usecond sleep
 */
void xine_usec_sleep(unsigned usec) XINE_PROTECTED;

/* compatibility macros */
#define xine_strpbrk(S, ACCEPT) strpbrk((S), (ACCEPT))
#define xine_strsep(STRINGP, DELIM) strsep((STRINGP), (DELIM))
#define xine_setenv(NAME, VAL, XX) setenv((NAME), (VAL), (XX))

/**
 * append to a string, reallocating
 * normally, updates & returns *dest
 * on error, *dest is unchanged & NULL is returned.
 */
char *xine_strcat_realloc (char **dest, const char *append) XINE_PROTECTED;

/**
 * asprintf wrapper
 * allocate a string large enough to hold the output, and return a pointer to
 * it. This pointer should be passed to free when it is no longer needed.
 * return NULL on error.
 */
char *_x_asprintf(const char *format, ...) XINE_PROTECTED XINE_MALLOC XINE_FORMAT_PRINTF(1, 2);

/**
 * opens a file, ensuring that the descriptor will be closed
 * automatically after a fork/execute.
 */
int xine_open_cloexec(const char *name, int flags) XINE_PROTECTED;

/**
 * creates a file, ensuring that the descriptor will be closed
 * automatically after a fork/execute.
 */
int xine_create_cloexec(const char *name, int flags, mode_t mode) XINE_PROTECTED;

/**
 * creates a socket, ensuring that the descriptor will be closed
 * automatically after a fork/execute.
 */
int xine_socket_cloexec(int domain, int type, int protocol) XINE_PROTECTED;

/*
 * Color Conversion Utility Functions
 * The following data structures and functions facilitate the conversion
 * of RGB images to packed YUV (YUY2) images. There are also functions to
 * convert from YUV9 -> YV12. All of the meaty details are written in
 * color.c.
 */

typedef struct yuv_planes_s {

  unsigned char *y;
  unsigned char *u;
  unsigned char *v;
  unsigned int row_width;    /* frame width */
  unsigned int row_count;    /* frame height */

} yuv_planes_t;

void init_yuv_conversion(void) XINE_PROTECTED;
void init_yuv_planes(yuv_planes_t *yuv_planes, int width, int height) XINE_PROTECTED;
void free_yuv_planes(yuv_planes_t *yuv_planes) XINE_PROTECTED;

extern void (*yuv444_to_yuy2)
  (const yuv_planes_t *yuv_planes, unsigned char *yuy2_map, int pitch) XINE_PROTECTED;
extern void (*yuv9_to_yv12)
  (const unsigned char *y_src, int y_src_pitch, unsigned char *y_dest, int y_dest_pitch,
   const unsigned char *u_src, int u_src_pitch, unsigned char *u_dest, int u_dest_pitch,
   const unsigned char *v_src, int v_src_pitch, unsigned char *v_dest, int v_dest_pitch,
   int width, int height) XINE_PROTECTED;
extern void (*yuv411_to_yv12)
  (const unsigned char *y_src, int y_src_pitch, unsigned char *y_dest, int y_dest_pitch,
   const unsigned char *u_src, int u_src_pitch, unsigned char *u_dest, int u_dest_pitch,
   const unsigned char *v_src, int v_src_pitch, unsigned char *v_dest, int v_dest_pitch,
   int width, int height) XINE_PROTECTED;
extern void (*yv12_to_yuy2)
  (const unsigned char *y_src, int y_src_pitch,
   const unsigned char *u_src, int u_src_pitch,
   const unsigned char *v_src, int v_src_pitch,
   unsigned char *yuy2_map, int yuy2_pitch,
   int width, int height, int progressive) XINE_PROTECTED;
extern void (*yuy2_to_yv12)
  (const unsigned char *yuy2_map, int yuy2_pitch,
   unsigned char *y_dst, int y_dst_pitch,
   unsigned char *u_dst, int u_dst_pitch,
   unsigned char *v_dst, int v_dst_pitch,
   int width, int height) XINE_PROTECTED;


/* convert full range rgb to mpeg range yuv */
#define SCALESHIFT 16
#define SCALEFACTOR (1<<SCALESHIFT)
#define CENTERSAMPLE 128

/* new fast and more accurate macros. Simply recompile to use them */
#define COMPUTE_Y(r, g, b) \
  (unsigned char) \
  ((y_r_table[r] + y_g_table[g] + y_b_table[b]) >> SCALESHIFT)
#define COMPUTE_U(r, g, b) \
  (unsigned char) \
  ((u_r_table[r] + u_g_table[g] + uv_br_table[b]) >> SCALESHIFT)
#define COMPUTE_V(r, g, b) \
  (unsigned char) \
  ((uv_br_table[r] + v_g_table[g] + v_b_table[b]) >> SCALESHIFT)

/* Binaries using these old ones keep working,
   and get the full vs mpeg range bug fixed transparently as well.
#define COMPUTE_Y(r, g, b) \
  (unsigned char) \
  ((y_r_table[r] + y_g_table[g] + y_b_table[b]) / SCALEFACTOR)
#define COMPUTE_U(r, g, b) \
  (unsigned char) \
  ((u_r_table[r] + u_g_table[g] + u_b_table[b]) / SCALEFACTOR + CENTERSAMPLE)
#define COMPUTE_V(r, g, b) \
  (unsigned char) \
  ((v_r_table[r] + v_g_table[g] + v_b_table[b]) / SCALEFACTOR + CENTERSAMPLE)
*/

#define UNPACK_BGR15(packed_pixel, r, g, b) \
  b = (packed_pixel & 0x7C00) >> 7; \
  g = (packed_pixel & 0x03E0) >> 2; \
  r = (packed_pixel & 0x001F) << 3;

#define UNPACK_BGR16(packed_pixel, r, g, b) \
  b = (packed_pixel & 0xF800) >> 8; \
  g = (packed_pixel & 0x07E0) >> 3; \
  r = (packed_pixel & 0x001F) << 3;

#define UNPACK_RGB15(packed_pixel, r, g, b) \
  r = (packed_pixel & 0x7C00) >> 7; \
  g = (packed_pixel & 0x03E0) >> 2; \
  b = (packed_pixel & 0x001F) << 3;

#define UNPACK_RGB16(packed_pixel, r, g, b) \
  r = (packed_pixel & 0xF800) >> 8; \
  g = (packed_pixel & 0x07E0) >> 3; \
  b = (packed_pixel & 0x001F) << 3;

extern int y_r_table[256] XINE_PROTECTED;
extern int y_g_table[256] XINE_PROTECTED;
extern int y_b_table[256] XINE_PROTECTED;

extern int uv_br_table[256] XINE_PROTECTED;

extern int u_r_table[256] XINE_PROTECTED;
extern int u_g_table[256] XINE_PROTECTED;
extern int u_b_table[256] XINE_PROTECTED;

extern int v_r_table[256] XINE_PROTECTED;
extern int v_g_table[256] XINE_PROTECTED;
extern int v_b_table[256] XINE_PROTECTED;

/* TJ. direct sliced rgb -> yuy2 conversion */
typedef struct rgb2yuy2_s rgb2yuy2_t;
extern rgb2yuy2_t *rgb2yuy2_alloc (int color_matrix, const char *format) XINE_PROTECTED;
extern void  rgb2yuy2_free (rgb2yuy2_t *rgb2yuy2) XINE_PROTECTED;
extern void  rgb2yuy2_slice (rgb2yuy2_t *rgb2yuy2, const uint8_t *in, int ipitch, uint8_t *out, int opitch,
  int width, int height) XINE_PROTECTED;
extern void  rgb2yuy2_palette (rgb2yuy2_t *rgb2yuy2, const uint8_t *pal, int num_colors, int bits_per_pixel)
  XINE_PROTECTED;

extern void rgb2yv12_slice (rgb2yuy2_t *rgb2yuy2, const uint8_t *src, int src_stride,
                           uint8_t *y_dst, int y_pitch,
                           uint8_t *u_dst, int u_pitch,
                           uint8_t *v_dst, int v_pitch,
                           int width, int height) XINE_PROTECTED;

/* frame copying functions */
extern void yv12_to_yv12
  (const unsigned char *y_src, int y_src_pitch, unsigned char *y_dst, int y_dst_pitch,
   const unsigned char *u_src, int u_src_pitch, unsigned char *u_dst, int u_dst_pitch,
   const unsigned char *v_src, int v_src_pitch, unsigned char *v_dst, int v_dst_pitch,
   int width, int height) XINE_PROTECTED;
extern void yuy2_to_yuy2
  (const unsigned char *src, int src_pitch,
   unsigned char *dst, int dst_pitch,
   int width, int height) XINE_PROTECTED;

void _x_nv12_to_yv12(const uint8_t *y_src,  int y_src_pitch,
                     const uint8_t *uv_src, int uv_src_pitch,
                     uint8_t *y_dst, int y_dst_pitch,
                     uint8_t *u_dst, int u_dst_pitch,
                     uint8_t *v_dst, int v_dst_pitch,
                     int width, int height) XINE_PROTECTED;
void _x_yv12_to_nv12(const uint8_t *y_src, int y_src_pitch,
                     const uint8_t *u_src, int u_src_pitch,
                     const uint8_t *v_src, int v_src_pitch,
                     uint8_t *y_dst,  int y_dst_pitch,
                     uint8_t *uv_dst, int uv_dst_pitch,
                     int width, int height) XINE_PROTECTED;
void _x_yuy2_to_nv12(const uint8_t *src_yuy2_map, int yuy2_pitch,
                     uint8_t *y_dst,  int y_dst_pitch,
                     uint8_t *uv_dst, int uv_dst_pitch,
                     int width, int height) XINE_PROTECTED;


/* print a hexdump of the given data */
void xine_hexdump (const void *buf, int length) XINE_PROTECTED;

/*
 * Optimization macros for conditions
 * Taken from the FIASCO L4 microkernel sources
 */
#if !defined(__GNUC__) || __GNUC__ < 3
#  define EXPECT_TRUE(x)  (x)
#  define EXPECT_FALSE(x) (x)
#else
#  define EXPECT_TRUE(x)  __builtin_expect((x),1)
#  define EXPECT_FALSE(x) __builtin_expect((x),0)
#endif

#ifdef NDEBUG
#define _x_assert(exp) \
  do {                                                                \
    if (!(exp))                                                       \
      fprintf(stderr, "assert: %s:%d: %s: Assertion `%s' failed.\n",  \
              __FILE__, __LINE__, __XINE_FUNCTION__, #exp);           \
  } while(0)
#else
#define _x_assert(exp) \
  do {                                                                \
    if (!(exp)) {                                                     \
      fprintf(stderr, "assert: %s:%d: %s: Assertion `%s' failed.\n",  \
              __FILE__, __LINE__, __XINE_FUNCTION__, #exp);           \
      abort();                                                        \
    }                                                                 \
  } while(0)
#endif

XINE_DEPRECATED static inline void _x_abort_is_deprecated(void) {}
#define _x_abort()                                                    \
  do {                                                                \
    fprintf(stderr, "abort: %s:%d: %s: Aborting.\n",                  \
            __FILE__, __LINE__, __XINE_FUNCTION__);                   \
    abort();                                                          \
    _x_abort_is_deprecated();                                         \
  } while(0)


/****** logging with xine **********************************/

#ifndef LOG_MODULE
  #define LOG_MODULE __FILE__
#endif /* LOG_MODULE */

#define LOG_MODULE_STRING printf("%s: ", LOG_MODULE );

#ifdef LOG_VERBOSE
  #define LONG_LOG_MODULE_STRING                                            \
    printf("%s: (%s:%d) ", LOG_MODULE, __XINE_FUNCTION__, __LINE__ );
#else
  #define LONG_LOG_MODULE_STRING  LOG_MODULE_STRING
#endif /* LOG_VERBOSE */

#ifdef LOG
  #if defined(__GNUC__) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 199901L)
    #define lprintf(fmt, args...)                                           \
      do {                                                                  \
        LONG_LOG_MODULE_STRING                                              \
        printf(fmt, ##args);                                                \
        fflush(stdout);                                                     \
      } while(0)
  #else /* __GNUC__ */
    #ifdef _MSC_VER
      #define lprintf(fmtargs)                                              \
        do {                                                                \
          LONG_LOG_MODULE_STRING                                            \
          printf("%s", fmtargs);                                            \
          fflush(stdout);                                                   \
        } while(0)
    #else /* _MSC_VER */
      #define lprintf(...)                                                  \
        do {                                                                \
          LONG_LOG_MODULE_STRING                                            \
          printf(__VA_ARGS__);                                              \
          fflush(stdout);                                                   \
        } while(0)
    #endif  /* _MSC_VER */
  #endif /* __GNUC__ */
#else /* LOG */
  #if defined(DEBUG) && defined(XINE_COMPILE)
XINE_FORMAT_PRINTF(1, 2) static inline void lprintf(const char * fmt, ...) { (void)fmt; }
  #elif defined(__STDC_VERSION__) &&  __STDC_VERSION__ >= 199901L
    #define lprintf(...)              do {} while(0)
  #elif defined(__GNUC__)
      #define lprintf(fmt, args...)     do {} while(0)
  #elif defined(_MSC_VER)
void __inline lprintf(const char * fmt, ...) {}
  #else
    #define lprintf(...)              do {} while(0)
  #endif
#endif /* LOG */

#if defined(__GNUC__) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 199901L)
  #define llprintf(cat, fmt, args...)                                       \
    do{                                                                     \
      if(cat){                                                              \
        LONG_LOG_MODULE_STRING                                              \
        printf( fmt, ##args );                                              \
      }                                                                     \
    }while(0)
#else
#ifdef _MSC_VER
  #define llprintf(cat, fmtargs)                                            \
    do{                                                                     \
      if(cat){                                                              \
        LONG_LOG_MODULE_STRING                                              \
        printf( "%s", fmtargs );                                            \
      }                                                                     \
    }while(0)
#else
  #define llprintf(cat, ...)                                                \
    do{                                                                     \
      if(cat){                                                              \
        LONG_LOG_MODULE_STRING                                              \
        printf( __VA_ARGS__ );                                              \
      }                                                                     \
    }while(0)
#endif /* _MSC_VER */
#endif /* __GNUC__ */

#if defined(__GNUC__) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 199901L)
  #define xprintf(xine, verbose, fmt, args...)                              \
    do {                                                                    \
      if((xine) && (xine)->verbosity >= verbose){                           \
        xine_log(xine, XINE_LOG_TRACE, fmt, ##args);                        \
      }                                                                     \
    } while(0)
#else
#ifdef _MSC_VER
void xine_xprintf(xine_t *xine, int verbose, const char *fmt, ...);
  #define xprintf xine_xprintf
#else
  #define xprintf(xine, verbose, ...)                                       \
    do {                                                                    \
      if((xine) && (xine)->verbosity >= verbose){                           \
        xine_log(xine, XINE_LOG_TRACE, __VA_ARGS__);                        \
      }                                                                     \
    } while(0)
#endif /* _MSC_VER */
#endif /* __GNUC__ */

/* time measuring macros for profiling tasks */

#ifdef DEBUG
#  define XINE_PROFILE(function)                                            \
     do {                                                                   \
       struct timeval current_time;                                         \
       double dtime;                                                        \
       gettimeofday(&current_time, NULL);                                   \
       dtime = -(current_time.tv_sec + (current_time.tv_usec / 1000000.0)); \
       function;                                                            \
       gettimeofday(&current_time, NULL);                                   \
       dtime += current_time.tv_sec + (current_time.tv_usec / 1000000.0);   \
       printf("%s: (%s:%d) took %lf seconds\n",                             \
              LOG_MODULE, __XINE_FUNCTION__, __LINE__, dtime);              \
     } while(0)
#  define XINE_PROFILE_ACCUMULATE(function)                                 \
     do {                                                                   \
       struct timeval current_time;                                         \
       static double dtime = 0;                                             \
       gettimeofday(&current_time, NULL);                                   \
       dtime -= current_time.tv_sec + (current_time.tv_usec / 1000000.0);   \
       function;                                                            \
       gettimeofday(&current_time, NULL);                                   \
       dtime += current_time.tv_sec + (current_time.tv_usec / 1000000.0);   \
       printf("%s: (%s:%d) took %lf seconds\n",                             \
              LOG_MODULE, __XINE_FUNCTION__, __LINE__, dtime);              \
     } while(0)
#else
#  define XINE_PROFILE(function) function
#  define XINE_PROFILE_ACCUMULATE(function) function
#endif /* DEBUG */

/**
 * get encoding of current locale
 */
char *xine_get_system_encoding(void) XINE_MALLOC XINE_PROTECTED;

/*
 * guess default encoding for the subtitles
 */
const char *xine_guess_spu_encoding(void) XINE_PROTECTED;

/*
 * use the best clock reference (API compatible with gettimeofday)
 * note: it will be a monotonic clock, if available.
 */
struct timezone;
int xine_monotonic_clock(struct timeval *tv, struct timezone *tz) XINE_PROTECTED;

/**
 * Unknown FourCC reporting functions
 */
void _x_report_video_fourcc (xine_t *, const char *module, uint32_t) XINE_PROTECTED;
void _x_report_audio_format_tag (xine_t *, const char *module, uint32_t) XINE_PROTECTED;

/** fast strings are either inside an application supplied buffer,
 *  or they are (re)allocated by xine_fast_string_set ().
 *  the returned pointer will be at least 8 byte aligned,
 *  and can be read like an ordinary C string. */
#define XINE_FAST_STRING 1
/** return the byte size needed for an application supplied buffer. */
size_t xine_fast_string_need (size_t max_strlen) XINE_PROTECTED;
/** return the actual max strlen. */
size_t xine_fast_string_max (char *fast_string) XINE_PROTECTED;
/** set up a fast string inside an application supplied buffer. */
char *xine_fast_string_init (char *buf, size_t bsize) XINE_PROTECTED;
/** set or change the contents of a fast string.
 *  fast_text may be NULL, to allocate a new one.
 *  you can even edit the string manually, then apply the new size with text == NULL. */
char *xine_fast_string_set (char *fast_string, const char *text, size_t tsize) XINE_PROTECTED;
/** fast strcmp ().
 *  BTW: XINE_PROTECTED does not link on some systems when doing
 *  xine_sarray_new (size, (xine_sarray_comparator_t)xine_fast_string_cmp);. */
int xine_fast_string_cmp (char *fast_string1, char *fast_string2);
/** free a fast string if it is not application supplied. */
void xine_fast_string_free (char **fast_string) XINE_PROTECTED;

#define XINE_REF_STRING 1
/** create or reuse a xine reference counted string, whose contents shall remain unchanged.
 *  use len == -1 if not kown. */
char *xine_ref_string_ref (const char *s, int len) XINE_PROTECTED;
/** fast if s is a xine reference counted string. */
size_t xine_ref_string_len (const char *s) XINE_PROTECTED;
/** same as _x_freep if s is _not_ a xine reference counted string. */
int xine_ref_string_unref (char **s) XINE_PROTECTED;

/** stream parsers do delay data, but often do not care about the frame pts
 *  that need to be delayed as well. this may help here. */
#define XINE_PTS_QUEUE 1
typedef struct xine_pts_queue_s xine_pts_queue_t;
/** create a new instance. */
xine_pts_queue_t *xine_pts_queue_new (void) XINE_PROTECTED;
/** reset after a stream seek. */
void xine_pts_queue_reset (xine_pts_queue_t *queue) XINE_PROTECTED;
/** tell what the parser has consumed. pts == 0 if unknown. */
void xine_pts_queue_put (xine_pts_queue_t *queue, size_t bytes, int64_t pts) XINE_PROTECTED;
/** tell what the parser has delivered, and get the filtered pts or 0. */
int64_t xine_pts_queue_get (xine_pts_queue_t *queue, size_t bytes) XINE_PROTECTED;
/** you no longer need the queue. */
void xine_pts_queue_delete (xine_pts_queue_t **queue) XINE_PROTECTED;

/** xine timespec magic. */
#define XINE_TS 1
/** XINE_TS >= 1: Well.
 *  libcurl.curl_getdate () fails on plain "01 January 1970 00:00:04 GMT".
 *  shell.date fails on "2020-08-31T23:55:00.000+02:00".
 *  here comes yet another "we can do better" attempt:
 *  input:
 *    ts: the fallback or relative base time.
 *    string: the date/time in some halfway common format.
 *  output:
 *    ts: what we got.
 *    return: 0 (OK) or some Exxx error code. */
int xine_ts_from_string (struct timespec *ts, const char *string) XINE_PROTECTED;
/** XINE_TS >= 1: a += b. */
void xine_ts_add (struct timespec *a, const struct timespec *b) XINE_PROTECTED;
/** XINE_TS >= 1: a -= b. */
void xine_ts_sub (struct timespec *a, const struct timespec *b) XINE_PROTECTED;
/** XINE_TS >= 1: ts * timebase. */
int64_t xine_ts_to_timebase (const struct timespec *ts, uint32_t timebase) XINE_PROTECTED;

/** xine rational numbers. */
#define XINE_RATS 1
typedef struct {
  int64_t num, den;
} xine_rats_t;
/** XINE_RATS >= 1: shorten value. */
void xine_rats_shorten (xine_rats_t *value) XINE_PROTECTED;

/* don't harm following code */
#ifdef extern
#  undef extern
#endif

#ifdef __cplusplus
}
#endif

#endif
