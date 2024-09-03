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
#define	_POSIX_PTHREAD_SEMANTICS 1	/* for 5-arg getpwuid_r on solaris */

/*
#define LOG
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <xine/xineutils.h>
#include <xine/xineintl.h>
#ifdef _MSC_VER
#include <xine/xine_internal.h>
#endif
#include "xine_private.h"
#include "../xine-engine/bswap.h"

#include <errno.h>
#ifdef HAVE_PWD_H
#include <pwd.h>
#endif
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#if HAVE_EXECINFO_H
#include <execinfo.h>
#endif

#if HAVE_UCONTEXT_H
#include <ucontext.h>
#endif

#ifdef HAVE_NL_LANGINFO
#include <langinfo.h>
#endif

#if defined(WIN32)
#include <windows.h>
#include <shlobj.h>
#endif

#ifndef O_CLOEXEC
#  define O_CLOEXEC  0
#endif

typedef struct {
  const char    language[16];     /* name of the locale */
  const char    encoding[16];     /* typical encoding */
  const char    spu_encoding[16]; /* default spu encoding */
  const char    modifier[8];
} lang_locale_t;


/*
 * information about locales used in xine
 */
static const lang_locale_t lang_locales[] = {
  { "af_ZA",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "ar_AE",    "iso-8859-6",  "iso-8859-6",  ""         },
  { "ar_BH",    "iso-8859-6",  "iso-8859-6",  ""         },
  { "ar_DZ",    "iso-8859-6",  "iso-8859-6",  ""         },
  { "ar_EG",    "iso-8859-6",  "iso-8859-6",  ""         },
  { "ar_IN",    "utf-8",       "utf-8",       ""         },
  { "ar_IQ",    "iso-8859-6",  "iso-8859-6",  ""         },
  { "ar_JO",    "iso-8859-6",  "iso-8859-6",  ""         },
  { "ar_KW",    "iso-8859-6",  "iso-8859-6",  ""         },
  { "ar_LB",    "iso-8859-6",  "iso-8859-6",  ""         },
  { "ar_LY",    "iso-8859-6",  "iso-8859-6",  ""         },
  { "ar_MA",    "iso-8859-6",  "iso-8859-6",  ""         },
  { "ar_OM",    "iso-8859-6",  "iso-8859-6",  ""         },
  { "ar_QA",    "iso-8859-6",  "iso-8859-6",  ""         },
  { "ar_SA",    "iso-8859-6",  "iso-8859-6",  ""         },
  { "ar_SD",    "iso-8859-6",  "iso-8859-6",  ""         },
  { "ar_SY",    "iso-8859-6",  "iso-8859-6",  ""         },
  { "ar_TN",    "iso-8859-6",  "iso-8859-6",  ""         },
  { "ar_YE",    "iso-8859-6",  "iso-8859-6",  ""         },
  { "be_BY",    "cp1251",      "cp1251",      ""         },
  { "bg_BG",    "cp1251",      "cp1251",      ""         },
  { "br_FR",    "iso-8859-1",  "iso-88591",   ""         },
  { "bs_BA",    "iso-8859-2",  "cp1250",      ""         },
  { "ca_ES",    "iso-8859-1",  "iso-88591",   ""         },
  { "ca_ES",    "iso-8859-15", "iso-8859-15", "euro"     },
  { "cs_CZ",    "iso-8859-2",  "cp1250",      ""         },
  { "cy_GB",    "iso-8859-14", "iso-8859-14", ""         },
  { "da_DK",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "de_AT",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "de_AT",    "iso-8859-15", "iso-8859-15", "euro"     },
  { "de_BE",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "de_BE",    "iso-8859-15", "iso-8859-15", "euro"     },
  { "de_CH",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "de_DE",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "de_DE",    "iso-8859-15", "iso-8859-15", "euro"     },
  { "de_LU",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "de_LU",    "iso-8859-15", "iso-8859-15", "euro"     },
  { "el_GR",    "iso-8859-7",  "iso-8859-7",  ""         },
  { "en_AU",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "en_BW",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "en_CA",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "en_DK",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "en_GB",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "en_HK",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "en_IE",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "en_IE",    "iso-8859-15", "iso-8859-15", "euro"     },
  { "en_IN",    "utf-8",       "utf-8",       ""         },
  { "en_NZ",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "en_PH",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "en_SG",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "en_US",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "en_ZA",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "en_ZW",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "es_AR",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "es_BO",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "es_CL",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "es_CO",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "es_CR",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "es_DO",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "es_EC",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "es_ES",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "es_ES",    "iso-8859-15", "iso-8859-15", "euro"     },
  { "es_GT",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "es_HN",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "es_MX",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "es_NI",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "es_PA",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "es_PE",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "es_PR",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "es_PY",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "es_SV",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "es_US",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "es_UY",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "es_VE",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "et_EE",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "eu_ES",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "eu_ES",    "iso-8859-15", "iso-8859-15", "euro"     },
  { "fa_IR",    "utf-8",       "utf-8",       ""         },
  { "fi_FI",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "fi_FI",    "iso-8859-15", "iso-8859-15", "euro"     },
  { "fo_FO",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "fr_BE",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "fr_BE",    "iso-8859-15", "iso-8859-15", "euro"     },
  { "fr_CA",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "fr_CH",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "fr_FR",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "fr_FR",    "iso-8859-15", "iso-8859-15", "euro"     },
  { "fr_LU",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "fr_LU",    "iso-8859-15", "iso-8859-15", "euro"     },
  { "ga_IE",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "ga_IE",    "iso-8859-15", "iso-8859-15", "euro"     },
  { "gl_ES",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "gl_ES",    "iso-8859-15", "iso-8859-15", "euro"     },
  { "gv_GB",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "he_IL",    "iso-8859-8",  "iso-8859-8",  ""         },
  { "hi_IN",    "utf-8",       "utf-8",       ""         },
  { "hr_HR",    "iso-8859-2",  "cp1250",      ""         },
  { "hu_HU",    "iso-8859-2",  "cp1250",      ""         },
  { "id_ID",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "is_IS",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "it_CH",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "it_IT",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "it_IT",    "iso-8859-15", "iso-8859-15", "euro"     },
  { "iw_IL",    "iso-8859-8",  "iso-8859-8",  ""         },
  { "ja_JP",    "euc-jp",      "euc-jp",      ""         },
  { "ja_JP",    "ujis",        "ujis",        ""         },
  { "japanese", "euc",         "euc",         ""         },
  { "ka_GE",    "georgian-ps", "georgian-ps", ""         },
  { "kl_GL",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "ko_KR",    "euc-kr",      "euc-kr",      ""         },
  { "ko_KR",    "utf-8",       "utf-8",       ""         },
  { "korean",   "euc",         "euc",         ""         },
  { "kw_GB",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "lt_LT",    "iso-8859-13", "iso-8859-13", ""         },
  { "lv_LV",    "iso-8859-13", "iso-8859-13", ""         },
  { "mi_NZ",    "iso-8859-13", "iso-8859-13", ""         },
  { "mk_MK",    "iso-8859-5",  "cp1251",      ""         },
  { "mr_IN",    "utf-8",       "utf-8",       ""         },
  { "ms_MY",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "mt_MT",    "iso-8859-3",  "iso-8859-3",  ""         },
  { "nb_NO",    "ISO-8859-1",  "ISO-8859-1",  ""         },
  { "nl_BE",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "nl_BE",    "iso-8859-15", "iso-8859-15", "euro"     },
  { "nl_NL",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "nl_NL",    "iso-8859-15", "iso-8859-15", "euro"     },
  { "nn_NO",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "no_NO",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "oc_FR",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "pl_PL",    "iso-8859-2",  "cp1250",      ""         },
  { "pt_BR",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "pt_PT",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "pt_PT",    "iso-8859-15", "iso-8859-15", "euro"     },
  { "ro_RO",    "iso-8859-2",  "cp1250",      ""         },
  { "ru_RU",    "iso-8859-5",  "cp1251",      ""         },
  { "ru_RU",    "koi8-r",      "cp1251",      ""         },
  { "ru_UA",    "koi8-u",      "cp1251",      ""         },
  { "se_NO",    "utf-8",       "utf-8",       ""         },
  { "sk_SK",    "iso-8859-2",  "cp1250",      ""         },
  { "sl_SI",    "iso-8859-2",  "cp1250",      ""         },
  { "sq_AL",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "sr_YU",    "iso-8859-2",  "cp1250",      ""         },
  { "sr_YU",    "iso-8859-5",  "cp1251",      "cyrillic" },
  { "sv_FI",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "sv_FI",    "iso-8859-15", "iso-8859-15", "euro"     },
  { "sv_SE",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "ta_IN",    "utf-8",       "utf-8",       ""         },
  { "te_IN",    "utf-8",       "utf-8",       ""         },
  { "tg_TJ",    "koi8-t",      "cp1251",      ""         },
  { "th_TH",    "tis-620",     "tis-620",     ""         },
  { "tl_PH",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "tr_TR",    "iso-8859-9",  "iso-8859-9",  ""         },
  { "uk_UA",    "koi8-u",      "cp1251",      ""         },
  { "ur_PK",    "utf-8",       "utf-8",       ""         },
  { "uz_UZ",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "vi_VN",    "tcvn",        "tcvn",        ""         },
  { "vi_VN",    "utf-8",       "utf-8",       ""         },
  { "wa_BE",    "iso-8859-1",  "iso-8859-1",  ""         },
  { "wa_BE",    "iso-8859-15", "iso-8859-15", "euro"     },
  { "yi_US",    "cp1255",      "cp1255",      ""         },
  { "zh_CN",    "gb18030",     "gb18030",     ""         },
  { "zh_CN",    "gb2312",      "gb2312",      ""         },
  { "zh_CN",    "gbk",         "gbk",         ""         },
  { "zh_HK",    "big5-hkscs",  "big5-hkscs",  ""         },
  { "zh_TW",    "big-5",       "big-5",       ""         },
  { "zh_TW",    "euc-tw",      "euc-tw",      ""         },
};

/**
 * @brief Allocate and clean memory size_t 'size', then return the
 *        pointer to the allocated memory.
 * @param size Size of the memory area to allocate.
 *
 * @return A pointer to the allocated memory area, or NULL in case of
 *         error.
 *
 * The behaviour of this function differs from standard malloc() as
 * xine_xmalloc(0) will not return a NULL pointer, but rather a
 * pointer to a memory area of size 1 byte.
 *
 * The NULL value is only ever returned in case of an error in
 * malloc(), and is reported to stderr stream.
 *
 * @deprecated This function has been deprecated, as the behaviour of
 *             allocating a 1 byte memory area on zero size is almost
 *             never desired, and the function is thus mostly misused.
 */
void *xine_xmalloc(size_t size) {
  void *ptr;

  /* prevent xine_xmalloc(0) of possibly returning NULL */
  if( !size )
    size++;

  if((ptr = calloc(1, size)) == NULL) {
    fprintf(stderr, "%s: malloc() failed: %s.\n",
	    __XINE_FUNCTION__, strerror(errno));
    return NULL;
  }

  return ptr;
}

/**
 * @brief Wrapper around calloc() function.
 * @param nmemb Number of elements to allocate
 * @param size Size of each element to allocate
 *
 * This is a simple wrapper around calloc(), the only thing
 * it does more than calloc() is outputting an error if
 * the calloc fails (returning NULL).
 */
void *xine_xcalloc(size_t nmemb, size_t size) {
  void *ptr;

  if((ptr = calloc(nmemb, size)) == NULL) {
    fprintf(stderr, "%s: calloc() failed: %s.\n",
	    __XINE_FUNCTION__, strerror(errno));
    return NULL;
  }

  return ptr;
}

void *xine_memdup (const void *src, size_t length)
{
  void *dst = malloc (length);
  if (!dst) {
    return NULL;
  }
  return xine_fast_memcpy (dst, src, length);
}

void *xine_memdup0 (const void *src, size_t length)
{
  char *dst = malloc (length + 1);
  if (!dst) {
    return NULL;
  }
  dst[length] = 0;
  return xine_fast_memcpy (dst, src, length);
}

#ifdef __CYGWIN__
/*
 * Parse command line with Windows XP syntax and copy the command (argv[0]).
 */
static size_t xine_strcpy_command(const char *cmdline, char *cmd, size_t maxlen) {
  size_t i, j;

  i = 0;
  j = 0;
  while (cmdline[i] && isspace(cmdline[i])) i++;

  while (cmdline[i] && !isspace(cmdline[i]) && j + 2 < maxlen) {
    switch (cmdline[i]) {
    case '\"':
      i++;
      while (cmdline[i] && cmdline[i] != '\"') {
        if (cmdline[i] == '\\') {
          i++;
          if (cmdline[i] == '\"') cmd[j++] = '\"';
          else {
            cmd[j++] = '\\';
            cmd[j++] = cmdline[i];
          }
        } else cmd[j++] = cmdline[i];
        if (cmdline[i]) i++;
      }
      break;

    case '\\':
      i++;
      if (cmdline[i] == '\"') cmd[j++] = '\"';
      else {
        cmd[j++] = '\\';
        i--;
      }
      break;

    default:
      cmd[j++] = cmdline[i];
    }

    i++;
  }
  cmd[j] = '\0';

  return j;
}
#endif

#ifndef BUFSIZ
#define BUFSIZ 256
#endif

const char *xine_get_homedir(void) {
#if defined(__CYGWIN__)

  static char homedir[1024] = {0, };
  char *s;

  xine_strcpy_command(GetCommandLine(), homedir, sizeof(homedir));
  s = strdup(homedir);
  GetFullPathName(s, sizeof(homedir), homedir, NULL);
  free(s);
  if ((s = strrchr(homedir, '\\')))
    *s = '\0';

  return homedir;

#elif defined (WIN32)

  static char homedir[MAX_PATH] = {0, };
  wchar_t wdir[MAX_PATH];

  /* Get the "Application Data" folder for the user */
  if (!homedir[0]) {
    if (S_OK == SHGetFolderPathW(NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE,
                                 NULL, SHGFP_TYPE_CURRENT, wdir)) {
      WideCharToMultiByte (CP_UTF8, 0, wdir, -1, homedir, MAX_PATH, NULL, NULL);
    } else {
      fprintf(stderr, "Can't find user configuration directory !\n");
    }
  }
  return homedir;

#else /* __CYGWIN__ , WIN32 */

  struct passwd pwd, *pw = NULL;
  static char homedir[BUFSIZ] = {0,};

#ifdef HAVE_GETPWUID_R
  if(getpwuid_r(getuid(), &pwd, homedir, sizeof(homedir), &pw) != 0 || pw == NULL)
#else
  if((pw = getpwuid(getuid())) == NULL)
#endif
  {
    char *tmp = getenv("HOME");
    if(tmp) {
      strncpy(homedir, tmp, sizeof(homedir));
      homedir[sizeof(homedir) - 1] = '\0';
    }
  } else {
    char *s = strdup(pw->pw_dir);
    strncpy(homedir, s, sizeof(homedir));
    homedir[sizeof(homedir) - 1] = '\0';
    free(s);
  }

  if(!homedir[0]) {
    printf("xine_get_homedir: Unable to get home directory, set it to /tmp.\n");
    strcpy(homedir, "/tmp");
  }

  return homedir;
#endif /* __CYGWIN__ , WIN32 */
}

#if defined(WIN32) || defined(__CYGWIN__)
static void xine_get_rootdir(char *rootdir, size_t maxlen) {
# if defined(__CYGWIN__)
  char *s;

  strncpy(rootdir, xine_get_homedir(), maxlen - 1);
  rootdir[maxlen - 1] = '\0';
  if ((s = strrchr(rootdir, XINE_DIRECTORY_SEPARATOR_CHAR))) *s = '\0';
# else /* WIN32 */
  /* use location of libxine.dll */
  static char marker;
  HMODULE hModule;
  wchar_t wpath[MAX_PATH];

  rootdir[0] = 0;

  if (GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        (LPCTSTR)&marker, &hModule)) {

    DWORD dw = GetModuleFileNameW(hModule, wpath, MAX_PATH);
    if (dw > 0 && dw < MAX_PATH) {
      WideCharToMultiByte(CP_UTF8, 0, wpath, -1, rootdir, maxlen, NULL, NULL);
      /* cut library name from path */
      char *p = strrchr(rootdir, '\\');
      if (p) {
        *p = 0;
      }
      lprintf("xine library dir is %s\n", rootdir);
      return;
    }
  }

  fprintf(stderr, "Can't determine libxine.dll install path\n");
# endif
}

const char *xine_get_pluginroot(void) {
  static char pluginroot[1024] = {0, };

  if (!pluginroot[0]) {
    xine_get_rootdir(pluginroot, sizeof(pluginroot) - strlen(XINE_REL_PLUGINROOT) - 1);
    strcat(pluginroot, XINE_DIRECTORY_SEPARATOR_STRING XINE_REL_PLUGINROOT);
  }

  return pluginroot;
}

const char *xine_get_plugindir(void) {
  static char plugindir[1024] = {0, };

  if (!plugindir[0]) {
    xine_get_rootdir(plugindir, sizeof(plugindir) - strlen(XINE_REL_PLUGINDIR) - 1);
    strcat(plugindir, XINE_DIRECTORY_SEPARATOR_STRING XINE_REL_PLUGINDIR);
  }

  return plugindir;
}

const char *xine_get_fontdir(void) {
  static char fontdir[1024] = {0, };

  if (!fontdir[0]) {
    xine_get_rootdir(fontdir, sizeof(fontdir) - strlen(XINE_REL_FONTDIR) - 1);
    strcat(fontdir, XINE_DIRECTORY_SEPARATOR_STRING XINE_REL_FONTDIR);
  }

  return fontdir;
}

const char *xine_get_localedir(void) {
  static char localedir[1024] = {0, };

  if (!localedir[0]) {
    xine_get_rootdir(localedir, sizeof(localedir) - strlen(XINE_REL_LOCALEDIR) - 1);
    strcat(localedir, XINE_DIRECTORY_SEPARATOR_STRING XINE_REL_LOCALEDIR);
  }

  return localedir;
}
#endif /* _WIN32 || __CYGWIN__ */

char *xine_chomp(char *str) {
  char *pbuf;

  pbuf = str;

  while(*pbuf != '\0') pbuf++;

  while(pbuf > str) {
    if(*pbuf == '\r' || *pbuf == '\n' || *pbuf == '"') *pbuf = '\0';
    pbuf--;
  }

  while(*pbuf == '=') pbuf++;

  return pbuf;
}


/*
 * a thread-safe usecond sleep
 */
void xine_usec_sleep(unsigned usec) {
#ifdef WIN32
  /* select does not work on win32 */
  Sleep(usec / 1000);
#else
#  if 0
#    if HAVE_NANOSLEEP
  /* nanosleep is prefered on solaris, because it's mt-safe */
  struct timespec ts, remaining;
  ts.tv_sec =   usec / 1000000;
  ts.tv_nsec = (usec % 1000000) * 1000;
  while (nanosleep (&ts, &remaining) == -1 && errno == EINTR)
    ts = remaining;
#    else
  usleep(usec);
#    endif
#  else
  if (usec < 10000) {
      usec = 10000;
  }
  struct timeval tm;
  tm.tv_sec  = usec / 1000000;
  tm.tv_usec = usec % 1000000;
  select(0, 0, 0, 0, &tm); /* FIXME: EINTR? */
#  endif
#endif
}


/* print a hexdump of length bytes from the data given in buf */
void xine_hexdump (const void *buf_gen, int length) {
  static const char separator[70] = "---------------------------------------------------------------------";

  const uint8_t *const buf = (const uint8_t*)buf_gen;
  int j = 0;

  /* printf ("Hexdump: %i Bytes\n", length);*/
  puts(separator);

  while(j<length) {
    int i;
    const int imax = (j+16 < length) ? (j+16) : length;

    printf ("%04X ",j);
    for (i=j; i<j+16; i++) {
      if( i<length )
        printf ("%02X ", buf[i]);
      else
        printf("   ");
    }

    for (i=j; i < imax; i++) {
      fputc ((buf[i] >= 32 && buf[i] <= 126) ? buf[i] : '.', stdout);
    }
    j=i;

    fputc('\n', stdout);
  }

  puts(separator);
}


static const lang_locale_t *_get_first_lang_locale(const char *lcal) {
  size_t lang_len;
  size_t i;
  char *mod;

  if(lcal && *lcal) {

    if ((mod = strchr(lcal, '@')))
      lang_len = mod++ - lcal;
    else
      lang_len = strlen(lcal);

    for (i = 0; i < sizeof(lang_locales)/sizeof(lang_locales[0]); i++) {
      if(!strncmp(lcal, lang_locales[i].language, lang_len)) {
        if ((!mod && !lang_locales[i].modifier[0]) || (mod && lang_locales[i].modifier[0] && !strcmp(mod, lang_locales[i].modifier)))
          return &lang_locales[i];
      }
    }
  }
  return NULL;
}


static char *_get_lang(void) {
    char *lang;

    if(!(lang = getenv("LC_ALL")))
      if(!(lang = getenv("LC_MESSAGES")))
        lang = getenv("LANG");

  return lang;
}


/*
 * get encoding of current locale
 */
char *xine_get_system_encoding(void) {
  char *codeset = NULL;

#ifdef HAVE_NL_LANGINFO
  setlocale(LC_CTYPE, "");
  codeset = nl_langinfo(CODESET);
#endif
  /*
   * guess locale codeset according to shell variables
   * when nl_langinfo(CODESET) isn't available or workig
   */
  if (!codeset || strstr(codeset, "ANSI") != 0) {
    char *lang = _get_lang();

    codeset = NULL;

    if(lang) {
      char *lg, *enc, *mod;

      lg = strdup(lang);

      if((enc = strchr(lg, '.')) && (strlen(enc) > 1)) {
        enc++;

        if((mod = strchr(enc, '@')))
          *mod = '\0';

        codeset = strdup(enc);
      }
      else {
        const lang_locale_t *llocale = _get_first_lang_locale(lg);

        if(llocale)
          codeset = strdup(llocale->encoding);
      }

      free(lg);
    }
  } else
    codeset = strdup(codeset);

  return codeset;
}


/*
 * guess default encoding of subtitles
 */
const char *xine_guess_spu_encoding(void) {
  char *lang = _get_lang();

  if (lang) {
    const lang_locale_t *llocale;
    char *lg, *enc;

    lg = strdup(lang);

    if ((enc = strchr(lg, '.'))) *enc = '\0';
    llocale = _get_first_lang_locale(lg);
    free(lg);
    if (llocale) return llocale->spu_encoding;
  }

  return "iso-8859-1";
}


#ifdef _MSC_VER
void xine_xprintf(xine_t *xine, int verbose, const char *fmt, ...) {
  char message[256];
  va_list ap;

  if (xine && xine->verbosity >= verbose) {
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    xine_log(xine, XINE_LOG_TRACE, "%s", message);
  }
}
#endif

int xine_monotonic_clock(struct timeval *tv, struct timezone *tz)
{
#if _POSIX_TIMERS > 0 && defined(_POSIX_MONOTONIC_CLOCK) && defined(HAVE_POSIX_TIMERS)
  static int xmc_mode = 0;

  do {
    struct timespec ts;

    if (xmc_mode > 1) {
      if (clock_gettime (CLOCK_MONOTONIC, &ts))
        break;
      tv->tv_sec = ts.tv_sec;
      tv->tv_usec = ts.tv_nsec / 1000;
      return 0;
    }

    if (xmc_mode == 1)
      break;

    xmc_mode = 1;

    if (clock_getres (CLOCK_MONOTONIC, &ts)) {
      lprintf ("get resolution of monotonic clock failed\n");
      break;
    }

    /* require at least milisecond resolution */
    if ((ts.tv_sec > 0) || (ts.tv_nsec > 1000000)) {
      lprintf ("monotonic clock resolution (%d:%d) too bad\n", (int)ts.tv_sec, (int)ts.tv_nsec);
      break;
    }

    if (clock_gettime (CLOCK_MONOTONIC, &ts)) {
      lprintf ("get monotonic clock failed\n");
      break;
    }

    lprintf ("using monotonic clock\n");
    xmc_mode = 2;
    tv->tv_sec = ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000;
    return 0;
  } while (0);
#endif

  return gettimeofday (tv, tz);
}

char *xine_strcat_realloc (char **dest, const char *append)
{
  char *newstr = realloc (*dest, (*dest ? strlen (*dest) : 0) + strlen (append) + 1);
  if (newstr)
    strcat (*dest = newstr, append);
  return newstr;
}

char *_x_asprintf(const char *format, ...)
{
  va_list ap;
  char *buf = NULL;

  va_start (ap, format);
  if (vasprintf (&buf, format, ap) < 0)
    buf = NULL;
  va_end (ap);

  return buf;
}

int _x_set_file_close_on_exec(int fd)
{
#ifndef WIN32
  return fcntl(fd, F_SETFD, FD_CLOEXEC);
#else
  return SetHandleInformation((HANDLE)_get_osfhandle(fd), HANDLE_FLAG_INHERIT, 0) ? 0 : -1;
#endif
}

int _x_set_socket_close_on_exec(int s)
{
#ifndef WIN32
  return fcntl(s, F_SETFD, FD_CLOEXEC);
#else
  return SetHandleInformation((HANDLE)(intptr_t)s, HANDLE_FLAG_INHERIT, 0) ? 0 : -1;
#endif
}


int xine_open_cloexec(const char *name, int flags)
{
  int fd = open(name, (flags | O_CLOEXEC));

  if (fd >= 0) {
    _x_set_file_close_on_exec(fd);
  }

  return fd;
}

int xine_create_cloexec(const char *name, int flags, mode_t mode)
{
  int fd = open(name, (flags | O_CREAT | O_CLOEXEC), mode);

  if (fd >= 0) {
    _x_set_file_close_on_exec(fd);
  }

  return fd;
}

int xine_socket_cloexec(int domain, int type, int protocol)
{
  int s = socket(domain, type, protocol);

  if (s >= 0) {
    _x_set_socket_close_on_exec(s);
  }

  return s;
}

/* get/resize/free aligned memory */

#ifndef XINE_MEM_ALIGN
#  define XINE_MEM_ALIGN 32
#endif
#define XINE_MEM_ADD (sizeof (size_t) + XINE_MEM_ALIGN)
#define XINE_MEM_MASK ((uintptr_t)XINE_MEM_ALIGN - 1)

void *xine_mallocz_aligned (size_t size) {
  uint8_t *new;
  size_t *sp;
  new = calloc (1, size + XINE_MEM_ADD);
  if (!new)
    return NULL;
  sp = (size_t *)new;
  *sp = size;
  new = (uint8_t *)(((uintptr_t)new + XINE_MEM_ADD) & ~XINE_MEM_MASK);
  new[-1] = new - (uint8_t *)sp;
  return new;
}

void *xine_malloc_aligned (size_t size) {
  uint8_t *new;
  size_t *sp;
  new = malloc (size + XINE_MEM_ADD);
  if (!new)
    return NULL;
  sp = (size_t *)new;
  *sp = size;
  new = (uint8_t *)(((uintptr_t)new + XINE_MEM_ADD) & ~XINE_MEM_MASK);
  new[-1] = new - (uint8_t *)sp;
  return new;
}

void xine_free_aligned (void *ptr) {
  uint8_t *old = (uint8_t *)ptr;
  if (!old)
    return;
  old -= old[-1];
  free (old);
}

void *xine_realloc_aligned (void *ptr, size_t size) {
  uint8_t *old = (uint8_t *)ptr, *new;
  size_t *sp, s;
  if (!size) {
    if (old)
      free (old - old[-1]);
    return NULL;
  }
  new = malloc (size + XINE_MEM_ADD);
  if (!new)
    return NULL;
  sp = (size_t *)new;
  *sp = size;
  new = (uint8_t *)(((uintptr_t)new + XINE_MEM_ADD) & ~XINE_MEM_MASK);
  new[-1] = new - (uint8_t *)sp;
  /* realloc () may break the alignment, requiring a slow memmove () afterwards */
  if (old) {
    sp = (size_t *)(old - old[-1]);
    s = *sp;
    if (size < s)
      s = size;
    xine_fast_memcpy (new, old, s);
    free (sp);
  }
  return new;
}

/* Base64 transcoder, adapted from TJtools. */
size_t xine_base64_encode (uint8_t *from, char *to, size_t size) {
  static const uint8_t tab[64] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const uint8_t *p = from;
  uint8_t *q = (uint8_t *)to;
  int l = size;
  from[size] = 0;
  from[size+1] = 0;
  while (l > 0) {
    uint32_t v = _X_BE_24 (p);
    p += 3;
    *q++ = tab[v >> 18];
    *q++ = tab[(v >> 12) & 63];
    *q++ = tab[(v >> 6) & 63];
    *q++ = tab[v & 63];
    l -= 3;
  }
  if (l < 0) {
    q[-1] = '=';
    if (l == -2) q[-2] = '=';
  }
  *q = 0;
  return q - (uint8_t *)to;
}

size_t xine_base64_decode (const char *from, uint8_t *to) {
  /* certain peopble use - _ instead of + /, lets support both ;-) */
#define rr 128 /* repeat */
#define ss 64  /* stop */
  static const uint8_t tab_unbase64[256] = {
    ss,rr,rr,rr,rr,rr,rr,rr,rr,rr,rr,rr,rr,rr,rr,rr,
    rr,rr,rr,rr,rr,rr,rr,rr,rr,rr,rr,rr,rr,rr,rr,rr,
    rr,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,62,ss,62,ss,63,
    52,53,54,55,56,57,58,59,60,61,ss,ss,ss,ss,ss,ss,
    ss, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,ss,ss,ss,ss,63,
    ss,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,ss,ss,ss,ss,ss,
    ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,
    ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,
    ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,
    ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,
    ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,
    ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,
    ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,
    ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss,ss
  };
  const uint8_t *p = (const uint8_t *)from;
  uint8_t *q = to;
  while (1) {
    uint32_t v, b;
    do b = tab_unbase64[*p++]; while (b & rr);
    if (b & ss) break;
    v = b << 18;
    do b = tab_unbase64[*p++]; while (b & rr);
    if (b & ss) break;
    v |= b << 12;
    *q++ = v >> 16;
    do b = tab_unbase64[*p++]; while (b & rr);
    if (b & ss) break;
    v |= b << 6;
    *q++ = v >> 8;
    do b = tab_unbase64[*p++]; while (b & rr);
    if (b & ss) break;
    v |= b;
    *q++ = v;
  }
#undef rr
#undef ss
  return q - to;
}

/* XXX precalculate 5k instead? */
static uint32_t tab_crc32_ieee[1280] = {0, 0,};
static uint16_t tab_crc16_ansi[768] = {0, 0,};

/* gcc -O3 recognizes this as bswap32 () */
#define rev32(n) (((n) << 24) | (((n) << 8) & 0xff0000) | (((n) >> 8) & 0xff00) | ((n) >> 24))
#define rev16(n) ((((n) << 8) | ((n) >> 8)) & 0xffff)

uint32_t xine_crc32_ieee (uint32_t crc, const uint8_t *data, size_t len) {
  uint32_t *t = tab_crc32_ieee;
  if (!t[1]) {
    uint32_t i;
    for (i = 0; i < 256; i++) {
      uint32_t j, u = i << 24;
      for (j = 0; j < 8; j++)
        u = (u << 1) ^ (((int32_t)u >> 31) & 0x4c11db7);
      t[i] = rev32 (u);
    }
    for (i = 0; i < 256; i++) {
      uint32_t v = t[i];
#ifdef WORDS_BIGENDIAN
      t[i + 256] = rev32 (v);
      v = (v >> 8) ^ t[v & 255];
      t[i + 512] = rev32 (v);
      v = (v >> 8) ^ t[v & 255];
      t[i + 768] = rev32 (v);
      v = (v >> 8) ^ t[v & 255];
      t[i + 1024] = rev32 (v);
#else
      t[i + 256] = v = (v >> 8) ^ t[v & 255];
      t[i + 512] = v = (v >> 8) ^ t[v & 255];
      t[i + 768] = (v >> 8) ^ t[v & 255];
#endif
    }
  }
  {
    size_t u;
    const uint32_t *d32;
    u = (~(uintptr_t)3 - (uintptr_t)data) & 3;
    if (u > len)
      u = len;
    len -= u;
    while (u) {
      crc = t[(uint8_t)crc ^ *data] ^ (crc >> 8);
      data++;
      u--;
    }
    d32 = (const uint32_t *)data;
    u = len / 4;
#ifdef WORDS_BIGENDIAN
    crc = rev32 (crc);
    while (u) {
      crc ^= *d32++;
      crc = t[(crc >> 24) + 1024]
          ^ t[((crc >> 16) & 0xff) + 768]
          ^ t[((crc >> 8) & 0xff) + 512]
          ^ t[(crc & 0xff) + 256];
      u--;
    }
    crc = rev32 (crc);
#else
    while (u) {
      crc ^= *d32++;
      crc = t[(crc & 0xff) + 768]
          ^ t[((crc >> 8) & 0xff) + 512]
          ^ t[((crc >> 16) & 0xff) + 256]
          ^ t[crc >> 24];
      u--;
    }
#endif
    data = (const uint8_t *)d32;
    u = len & 3;
    while (u) {
      crc = t[(uint8_t)crc ^ *data] ^ (crc >> 8);
      data++;
      u--;
    }
    return crc;
  }
}

uint32_t xine_crc16_ansi (uint32_t crc, const uint8_t *data, size_t len) {
  uint16_t *t = tab_crc16_ansi;
  if (!t[1]) {
    uint32_t i;
    for (i = 0; i < 256; i++) {
      uint32_t j, u = i << 24;
      for (j = 0; j < 8; j++)
        u = (u << 1) ^ (((int32_t)u >> 31) & 0x80050000);
      t[i] = ((u >> 8) & 0xff00) | (u >> 24);
    }
    for (i = 0; i < 256; i++) {
      uint16_t v = t[i];
#ifdef WORDS_BIGENDIAN
      t[i + 256] = rev16 (v);
      v = (v >> 8) ^ t[v & 255];
      t[i + 512] = rev16 (v);
#else
      t[i + 256] = (v >> 8) ^ t[v & 255];
#endif
    }
  }
  {
    size_t u;
    const uint32_t *d32;
    crc &= 0xffff;
    u = (~(uintptr_t)3 - (uintptr_t)data) & 3;
    if (u > len)
      u = len;
    len -= u;
    while (u) {
      crc = t[(uint8_t)crc ^ *data] ^ (crc >> 8);
      data++;
      u--;
    }
    d32 = (const uint32_t *)data;
    u = len / 4;
#ifdef WORDS_BIGENDIAN
    crc = rev16 (crc);
    while (u) {
      uint32_t v = *d32++;
      crc ^= v >> 16;
      crc = t[(crc >> 8) + 512]
          ^ t[(crc & 0xff) + 256];
      crc ^= v & 0xffff;
      crc = t[(crc >> 8) + 512]
          ^ t[(crc & 0xff) + 256];
      u--;
    }
    crc = rev16 (crc);
#else
    while (u) {
      uint32_t v = *d32++;
      crc ^= v & 0xffff;
      crc = t[(crc & 255) + 256]
          ^ t[crc >> 8];
      crc ^= v >> 16;
      crc = t[(crc & 255) + 256]
          ^ t[crc >> 8];
      u--;
    }
#endif
    data = (const uint8_t *)d32;
    u = len & 3;
    while (u) {
      crc = t[(uint8_t)crc ^ *data] ^ (crc >> 8);
      data++;
      u--;
    }
    return crc;
  }
}

/* fast string layout [uint32_t] (char):
 *   <alignment>
 *   [main_offs]
 *   [max_strlen | (application supplied ? 0x80000000 : 0)]
 *   [strlen]
 * fast_string_ptr ->
 *   (string) (0x00) (0x00)
 */

#define XFST_ALIGN (16)
#define XFST_MIN_SIZE ((XFST_ALIGN + 2 + XFST_ALIGN - 1) & ~(XFST_ALIGN - 1))

static const union {
  uint8_t z[4];
  uint32_t v;
} _xine_fast_string_mask[8] = {
  {{0xff, 0xff, 0xff, 0xff}},
  {{0x00, 0xff, 0xff, 0xff}},
  {{0x00, 0x00, 0xff, 0xff}},
  {{0x00, 0x00, 0x00, 0xff}},
  {{0x00, 0x00, 0x00, 0x00}},
  {{0xff, 0x00, 0x00, 0x00}},
  {{0xff, 0xff, 0x00, 0x00}},
  {{0xff, 0xff, 0xff, 0x00}}
};

size_t xine_fast_string_need (size_t max_strlen) {
  /* should be <= max_strlen + 32. */
  return XFST_ALIGN - 1 + 3 * 4 + ((max_strlen + 2 + 3) & ~3);
}

char *xine_fast_string_init (char *buf, size_t bsize) {
  union {
    char *b;
    uint32_t *u;
  } u;
  uint32_t *fs;

  if (!buf || (bsize < XFST_MIN_SIZE))
    return NULL;
  u.b = (char *)(((uintptr_t)buf + 3 * 4 + XFST_ALIGN - 1) & ~(XFST_ALIGN - 1));
  fs = u.u;
  fs[-3] = (char *)fs - buf;
  fs[-2] = (bsize - fs[-3] - 2) | 0x80000000;
  fs[-1] = 0;
  fs[0]  = 0;
  return (char *)fs;
}

size_t xine_fast_string_max (char *fast_string) {
  const union {
    char *b;
    uint32_t *u;
  } u = { fast_string };
  uint32_t *fs = u.u;

  return fs ? (fs[-2] & 0x7fffffff) : 0;
}

char *xine_fast_string_set (char *fast_string, const char *text, size_t tsize) {
  const union {
    char *b;
    uint32_t *u;
  } u = { fast_string };
  uint32_t *fs = u.u;

  if (fs) {
    if (fs[-2] & 0x80000000) {
      /* application supplied */
      if (tsize > (fs[-2] & 0x7fffffff))
        tsize = fs[-2] & 0x7fffffff;
    } else {
      /* auto reuse */
      if (tsize > fs[-2]) {
        /* realloc */
        size_t asize = (XFST_ALIGN + tsize + 2 + XFST_ALIGN - 1) & ~(XFST_ALIGN - 1);
        uint32_t *nfs = realloc (fs - (XFST_ALIGN >> 2), asize);

        if (nfs) {
          fs = nfs + (XFST_ALIGN >> 2);
          fs[-2] = asize - XFST_ALIGN - 2;
        } else {
          if (tsize > fs[-2])
            tsize = fs[-2];
        }
      }
    }
  } else {
    /* auto new */
    size_t asize = (XFST_ALIGN + tsize + 2 + XFST_ALIGN - 1) & ~(XFST_ALIGN - 1);

    fs = malloc (asize);
    if (!fs)
      return NULL;
    fs += XFST_ALIGN >> 2;
    fs[-3] = XFST_ALIGN >> 2;
    fs[-2] = asize - XFST_ALIGN - 2;
  }
  fs[-1] = tsize;
  if (text)
    memcpy (fs, text, tsize);
  fs[tsize >> 2] &= _xine_fast_string_mask[4 + (tsize & 3)].v;
  tsize++;
  fs[tsize >> 2] &= _xine_fast_string_mask[4 + (tsize & 3)].v;
  return (char *)fs;
}

int xine_fast_string_cmp (char *fast_string1, char *fast_string2) {
  const union {
    char *b;
    uint32_t *u;
  } u1 = { fast_string1 }, u2 = { fast_string2 };
  const union {
    uint32_t v;
    char *is_little;
  } endian = {1};
  uint32_t *fs1 = u1.u, *fs2 = u2.u, *test1 = fs1, *test2 = fs2;
  uint32_t end = fs1[-1] + 1, v1, v2;

  fs1[end >> 2] |= _xine_fast_string_mask[end & 3].v;
  while (*test1 == *test2)
    test1++, test2++;
  fs1[end >> 2] &= _xine_fast_string_mask[4 + (end & 3)].v;
  v1 = *test1;
  v2 = *test2;
  if (endian.is_little) {
    v1 = (v1 >> 24) | ((v1 & 0x00ff0000) >> 8) | ((v1 & 0x0000ff00) << 8) | (v1 << 24);
    v2 = (v2 >> 24) | ((v2 & 0x00ff0000) >> 8) | ((v2 & 0x0000ff00) << 8) | (v2 << 24);
  }
  return v1 < v2 ? -1
       : v1 > v2 ?  1
       :  0;
}

void xine_fast_string_free (char **fast_string) {
  union {
    char *b;
    uint32_t *u;
  } u;
  uint32_t *fs;

  if (!fast_string)
    return;
  u.b = *fast_string;
  fs = u.u;
  if (!fs)
    return;
  *fast_string = NULL;
  if (fs[-2] & 0x80000000)
    return;
  free ((char *)fs - fs[-3]);
}

/* The fast text feature. */
struct xine_fast_text_s {
  uint32_t scan_here;
  uint32_t line_start;
  uint32_t text_len;
  uint32_t flags;
  uint32_t dummy[3];
};

xine_fast_text_t *xine_fast_text_load (const char *filename, size_t max_size) {
  size_t filesize;
  FILE *f;
  xine_fast_text_t *xft;
  uint32_t *w;

  if (!filename) {
    errno = EINVAL;
    return NULL;
  }
  if (!filename[0]) {
    errno = EINVAL;
    return NULL;
  }

  f = fopen (filename, "rb");
  if (!f)
    return NULL;
  if (fseek (f, 0, SEEK_END)) {
    fclose(f);
    return NULL;
  }
  filesize = ftell (f);
  if (fseek (f, 0, SEEK_SET)) {
    fclose(f);
    return NULL;
  }

  if (filesize > max_size)
    filesize = max_size;
  xft = malloc (sizeof (*xft) + ((filesize + 3) & ~3));
  if (!xft) {
    fclose (f);
    errno = ENOMEM;
    return NULL;
  }
  xft->scan_here = 0;
  xft->line_start = 0;
  xft->flags = 0;
  xft->dummy[0] = 0;
  w = &xft->dummy[1];
  w[filesize >> 2] = 0x0a0a0a0a;
  w[(filesize >> 2) + 1] = 0x0a0a0a0a;
  xft->text_len = fread (&xft->dummy[1], 1, filesize, f);
  fclose (f);
  return xft;
}
  
char *xine_fast_text_line (xine_fast_text_t *xft, size_t *linesize) {
  const union {
    uint8_t b[4];
    uint32_t w;
  }
  b0 = {{0x80, 0, 0, 0}},
  b1 = {{0, 0x80, 0, 0}},
  b2 = {{0, 0, 0x80, 0}};
  uint32_t v;
  uint8_t *b, *e;

  if (xft->line_start >= xft->text_len) {
    *linesize = 0;
    return NULL;
  }
  e = (uint8_t *)&xft->dummy[1] + xft->scan_here;
  v = xft->flags;
  switch (xft->scan_here & 3) {
    case 0:
      {
        uint32_t *w = &xft->dummy[1] + (xft->scan_here >> 2); /* == (uint8_t *)e */

        do {
          v = *w++ ^ ~0x0a0a0a0a;
          v = ((v & 0x7f7f7f7f) + 0x01010101) & v & 0x80808080;
        } while (!v);
        e = (uint8_t *)(w - 1);
      }
      if (v & b0.w) {
        v &= ~b0.w;
        xft->scan_here = e - (uint8_t *)&xft->dummy[1] + (v ? 1 : 4);
        break;
      }
      e++;
      /* fall through */
    case 1:
      if (v & b1.w) {
        v &= ~b1.w;
        xft->scan_here = e - (uint8_t *)&xft->dummy[1] + (v ? 1 : 3);
        break;
      }
      e++;
      /* fall through */
    case 2:
      if (v & b2.w) {
        v &= ~b2.w;
        xft->scan_here = e - (uint8_t *)&xft->dummy[1] + (v ? 1 : 2);
        break;
      }
      e++;
      /* fall through */
    case 3:
      /* v & b3.w always true */
      v = 0;
      xft->scan_here = e - (uint8_t *)&xft->dummy[1] + 1;
      break;
  }
  xft->flags = v;
  b = (uint8_t *)&xft->dummy[1] + xft->line_start;
  xft->line_start = e - (uint8_t *)&xft->dummy[1] + 1;
  e[0] = 0;
  if (e[-1] == 0x0d)
    *--e = 0;
  *linesize = e - b;
  return (char *)b;
}

void xine_fast_text_unload (xine_fast_text_t **xft) {
  if (xft) {
    free (*xft);
    *xft = NULL;
  }
}

typedef struct {
  uint32_t refs;
  uint32_t len;
  uint32_t magic;
} _xine_ref_string_head_t;

static _xine_ref_string_head_t *_xine_ref_string_head (char *s) {
  union {
    char *b;
    _xine_ref_string_head_t *r;
  } u;
  const union {
    uint8_t b[4];
    uint32_t v;
  } _magic = {{'x', 'r', 's', 'h'}};
  _xine_ref_string_head_t *h;

  if (((uintptr_t)s & 7) != 4)
    return NULL;
  u.b = s;
  h = u.r - 1;
  if (h->magic != _magic.v)
    return NULL;
  return h;
}

char *xine_ref_string_ref (const char *s, int len) {
  const union {
    uint8_t b[4];
    uint32_t v;
  } _magic = {{'x', 'r', 's', 'h'}};
  uint32_t _len;
  char *_s = (char *)s;
  _xine_ref_string_head_t *h = _xine_ref_string_head (_s);

  if (h) {
    h->refs += 1;
    return _s;
  }
  if (!s)
    return NULL;
  _len = len < 0 ? strlen (s) : (size_t)len;
  _s = malloc (sizeof (_xine_ref_string_head_t) + _len + 1);
  if (!_s)
    return NULL;
  h = (_xine_ref_string_head_t *)_s;
  _s += sizeof (_xine_ref_string_head_t);
  h->refs = 1;
  h->len = _len;
  h->magic = _magic.v;
  memcpy (_s, s, _len + 1);
  return _s;
}

size_t xine_ref_string_len (const char *s) {
  _xine_ref_string_head_t *h = _xine_ref_string_head ((char *)s);

  return h ? h->len
       : s ? strlen (s)
       : 0;
}

int xine_ref_string_unref (char **s) {
  _xine_ref_string_head_t *h;

  if (!s)
    return 0;
  h = _xine_ref_string_head (*s);
  if (!h) {
    free (*s);
    *s = NULL;
    return 0;
  }
  if (h->refs == 1) {
    free (h);
    *s = NULL;
    return 0;
  }
  return --h->refs;
}


#define XPQ_BACKLOG_LD 3
#define XPQ_BACKLOG_SIZE (1 << XPQ_BACKLOG_LD)
#define XPQ_BACKLOG_MASK (XPQ_BACKLOG_SIZE - 1)

typedef enum {
  XPQ_A_NONE = 0,
  XPQ_A_STALL,
  XPQ_A_PUT,
  XPQ_A_READY,
  XPQ_A_GET
} xine_pts_queue_action_t;

struct xine_pts_queue_s {
  struct {
    int64_t last_pts;
    uint64_t pos;
    struct {
      int64_t pts;
      uint64_t pos;
    } backlog[XPQ_BACKLOG_SIZE];
    uint32_t ring_pos;
  } put;
  struct {
    uint64_t pos;
    uint32_t bytes;
    uint32_t num;
  } get;
  xine_pts_queue_action_t last_action;
};

xine_pts_queue_t *xine_pts_queue_new (void) {
  xine_pts_queue_t *q = calloc (1, sizeof (*q));
  return q;
}

void xine_pts_queue_reset (xine_pts_queue_t *q) {
  if (!q)
    return;
  memset (q, 0, sizeof (*q));
}

void xine_pts_queue_put (xine_pts_queue_t *q, size_t bytes, int64_t pts) {
  xine_pts_queue_action_t a = bytes ? XPQ_A_PUT : XPQ_A_READY;
  if (!q)
    return;
  if (pts && (pts != q->put.last_pts)) {
    uint32_t u = q->put.ring_pos;
    q->put.last_pts = pts;
    if (q->last_action != XPQ_A_STALL) {
      u = (u + 1) & XPQ_BACKLOG_MASK;
    } else {
      a = XPQ_A_STALL;
      q->get.pos = q->put.backlog[u].pos;
    }
    q->put.ring_pos = u;
    if (q->put.backlog[u].pts) {
      /* backlog overrun. parser seems be dropping data. */
      q->get.pos = q->put.backlog[u].pos;
      memset (q->put.backlog, 0, sizeof (q->put.backlog));
      a = XPQ_A_STALL;
    }
    q->put.backlog[u].pts = pts;
    q->put.backlog[u].pos = q->put.pos;
  }
  q->put.pos += bytes;
  q->last_action = a;
}
    
int64_t xine_pts_queue_get (xine_pts_queue_t *q, size_t bytes) {
  int64_t pts = 0;
  uint32_t u;
  if (!q)
    return 0;
  /* TODO: parser dropped data. */
  /* find suitable pts, or 0. */
  u = q->put.ring_pos;
  do {
    if (q->put.backlog[u].pos <= q->get.pos)
      break;
    u = (u + XPQ_BACKLOG_SIZE - 1) & XPQ_BACKLOG_MASK;
  } while (u != q->put.ring_pos);
  if (q->put.backlog[u].pos <= q->get.pos) {
    pts = q->put.backlog[u].pts;
    /* bytes == 0: just peek. bytes != 0: consume. */
    if (bytes) {
      q->put.backlog[u].pos = 0;
      q->put.backlog[u].pts = 0;
    }
  }
  /* advance. */
  q->get.pos += bytes;
  /* parser returned more than it has (filler frames, generated heads). */
  if (q->get.pos > q->put.pos)
    q->get.pos = q->put.pos;
  /* frame size stats. */
  q->get.bytes += bytes;
  q->get.num++;
  if ((q->get.bytes | q->get.num) & 0x80000000) {
    q->get.bytes >>= 1;
    q->get.num >>= 1;
  }
  q->last_action = XPQ_A_GET;
  return pts;
}

void xine_pts_queue_delete (xine_pts_queue_t **q) {
  if (q && *q) {
    free (*q);
    *q = NULL;
  }
}


/** xine timespec magic, taken from TJtools. */
int xine_ts_from_string (struct timespec *ts, const char *s) {
# define _DC_DIGIT  1
# define _DC_SPACE  2
# define _DC_Tt     4
# define _DC_Zz     8
# define _DC_PLUS  16
# define _DC_MINUS 32
# define _DC_DOT   64
# define _DC_END  128
  static const uint8_t tab_char[256] = {
    ['0'] = _DC_DIGIT, ['1'] = _DC_DIGIT, ['2'] = _DC_DIGIT, ['3'] = _DC_DIGIT,
    ['4'] = _DC_DIGIT, ['5'] = _DC_DIGIT, ['6'] = _DC_DIGIT, ['7'] = _DC_DIGIT,
    ['8'] = _DC_DIGIT, ['9'] = _DC_DIGIT,
    ['\t'] = _DC_SPACE, ['\r'] = _DC_SPACE, ['\n'] = _DC_SPACE, [' '] = _DC_SPACE,
    ['T'] = _DC_Tt, ['t'] = _DC_Tt,
    ['Z'] = _DC_Zz, ['z'] = _DC_Zz,
    ['+'] = _DC_PLUS,
    ['-'] = _DC_MINUS,
    ['.'] = _DC_DOT,
    [0] = _DC_END
  };
  enum {
    _DV_YEAR = 0, /* 1900- */
    _DV_MONTH,    /* 1-12 */
    _DV_DAY,      /* 1-31 */
    _DV_WEEKDAY,  /* 0-6 */
    _DV_AM,       /* 0 (24h), 1 (12-11 before noon), 2 (12-11 after noon) */
    _DV_HOUR,     /* 0-23 */
    _DV_MINUTE,   /* 0-59 */
    _DV_SECOND,   /* 0-59 */
    _DV_FRAC,     /* 0-999999999 */
    _DV_OFFS,     /* in seconds */
    _DV_LAST
  };
  static const struct {
    char s[11];
    uint8_t type;
    int value;
  } strings [] = {
    {"am         ", _DV_AM,            1},
    {"april      ", _DV_MONTH,         4},
    {"august     ", _DV_MONTH,         8},
    {"cdt        ", _DV_OFFS,     -18000},
    {"cst        ", _DV_OFFS,     -21600},
    {"december   ", _DV_MONTH,        12},
    {"edt        ", _DV_OFFS,     -14400},
    {"est        ", _DV_OFFS,     -18000},
    {"february   ", _DV_MONTH,         2},
    {"friday     ", _DV_WEEKDAY,       5},
    {"gmt        ", _DV_OFFS,          0},
    {"january    ", _DV_MONTH,         1},
    {"july       ", _DV_MONTH,         7},
    {"june       ", _DV_MONTH,         6},
    {"march      ", _DV_MONTH,         3},
    {"may        ", _DV_MONTH,         5},
    {"mdt        ", _DV_OFFS,     -21600},
    {"monday     ", _DV_WEEKDAY,       1},
    {"mst        ", _DV_OFFS,     -25200},
    {"november   ", _DV_MONTH,        11},
    {"october    ", _DV_MONTH,        10},
    {"pdt        ", _DV_OFFS,     -25200},
    {"pm         ", _DV_AM,            2},
    {"pst        ", _DV_OFFS,     -28800},
    {"saturday   ", _DV_WEEKDAY,       6},
    {"september  ", _DV_MONTH,         9},
    {"sunday     ", _DV_WEEKDAY,       0},
    {"thursday   ", _DV_WEEKDAY,       4},
    {"tuesday    ", _DV_WEEKDAY,       2},
    {"utc        ", _DV_OFFS,          0},
    {"wednesday  ", _DV_WEEKDAY,       3},
  };
  time_t res;
  int value[_DV_LAST] = {
    [_DV_YEAR]    = 1970,
    [_DV_MONTH]   = 1,
    [_DV_DAY]     = 1
  };
#define _DV_HAVE_DATE 1
#define _DV_HAVE_TIME 2
#define _DV_HAVE_ZONE 4
#define _DV_HAVE_JTIME 16
  static const uint32_t word_have[_DV_LAST] = {
    [_DV_OFFS] = _DV_HAVE_ZONE
  };
  static const uint32_t frac[] = {
    100000000,  10000000,   1000000,
       100000,     10000,      1000,
          100,        10,         1
  };
  uint64_t jtime = 0;
  uint32_t have = 0;
  const uint8_t *p;

  if (!ts)
    return EINVAL;
  if (!s)
    return 0;

  p = (const uint8_t *)s;

  if (((p[0] | 0x20) == 'p') && ((p[1] | 0x20) == 't')) {
  /* PT5H30M55S */
    uint32_t _sec = 0, _frac = 0;
    p += 2;
    while (1) {
      uint32_t v = 0, f = 0, z;
      while ((z = p[0] ^ '0') < 10u)
        v = v * 10u + z, p++;
      if (p[0] == '.') {
        uint32_t u = 0;
        p++;
        while (((z = p[0] ^ '0') < 10u) && (u < 9))
          f += frac[u++] * z, p++;
        while ((z = p[0] ^ '0') < 10u)
          p++;
      }
      z = p[0] | 0x20;
      if (z == 'h')
        _sec += 3600u * v;
      else if (z == 'm')
        _sec += 60u * v;
      else if (z == 's')
        _sec += v, _frac = f;
      else
        break;
      p++;
    }
    ts->tv_sec = _sec;
    ts->tv_nsec = _frac;
    return 0;
  }

  do {
    const uint8_t *b;
    uint8_t buf[12], type;
    uint32_t len, digits = 0;
    /* skip whitespace */
    while (tab_char[*p] & _DC_SPACE)
      p++;
    /* get word */
    type = tab_char[*p];
    if (type & _DC_Tt)
      type = tab_char[*++p];
    b = p;
    if (type & _DC_Zz) {
      type = tab_char[*++p];
      if (type & (_DC_PLUS + _DC_MINUS)) {
        b = p;
        type = tab_char[*++p];
      }
    }
    if (type & (_DC_PLUS + _DC_MINUS + _DC_DOT))
      p++;
    while (1) {
      while (!((type = tab_char[*p]) & (_DC_SPACE + _DC_Tt + _DC_Zz + _DC_PLUS + _DC_MINUS + _DC_DOT + _DC_END)))
        digits += type & _DC_DIGIT, p++;
      len = p - b;
      if (type & (_DC_SPACE + _DC_PLUS + _DC_DOT + _DC_END))
        break;
      if ((type & (_DC_Tt + _DC_Zz)) && digits)
        break;
      if ((type & _DC_MINUS) && !(((len == 4) && (digits == 4)) || ((len == 7) && (digits == 6))))
        break;
      p++;
    }
    /* evaluate */
    if ((len > 5) && (digits == len)) {
      uint64_t v = 0;
      uint32_t u;
      for (u = 0; u < len; u++)
        v = v * 10u + (b[u] ^ '0');
      jtime = v;
      have |= _DV_HAVE_JTIME;
    } else if ((len > 1) && (digits + 1 == len) && (b[0] == '@')) {
      uint64_t v = 0;
      uint32_t u;
      for (u = 1; u < len; u++)
        v = v * 10u + (b[u] ^ '0');
      jtime = v;
      have |= _DV_HAVE_JTIME;
    } else if ((digits + 1 == len) && (b[0] == '.')) {
      uint32_t u;
      b++;
      len--;
      if (len > 9)
        len = 9;
      value[_DV_FRAC] = 0;
      for (u = 0; u < len; u++)
        value[_DV_FRAC] += frac[u] * (b[u] ^ '0');
    } else if ((len == 2) && (digits == 2)) {
      /* DD */
      value[_DV_DAY] = (b[0] ^ '0') * 10u + (b[1] ^ '0');
    } else if ((len == 4) && (digits >= 3)) {
      if ((digits == 3) && (b[1] == ':')) {
        /* h:mm */
        value[_DV_HOUR] = b[0] ^ '0';
        value[_DV_MINUTE] = (b[2] ^ '0') * 10u + (b[3] ^ '0');
        value[_DV_SECOND] = 0;
        have |= _DV_HAVE_TIME;
      } else if (digits == 4) {
        /* YYYY */
        value[_DV_YEAR] = (b[0] ^ '0') * 1000u + (b[1] ^ '0') * 100u + (b[2] ^ '0') * 10u + (b[3] ^ '0');
        have |= _DV_HAVE_DATE;
      }
    } else if ((len == 5) && (digits == 4)) {
      if (b[2] == ':') {
        /* hh:mm */
        value[_DV_HOUR] = (b[0] ^ '0') * 10u + (b[1] ^ '0');
        value[_DV_MINUTE] = (b[3] ^ '0') * 10u + (b[4] ^ '0');
        value[_DV_SECOND] = 0;
        have |= _DV_HAVE_TIME;
      } else if (tab_char[b[0]] & (_DC_Zz + _DC_PLUS + _DC_MINUS)) {
        /* [Zz+-]ohom */
        value[_DV_OFFS] = (b[1] ^ '0') * 36000u + (b[2] ^ '0') * 3600u + (b[3] ^ '0') * 600u + (b[4] ^ '0') * 60u;
        if (b[0] == '-')
          value[_DV_OFFS] = -value[_DV_OFFS];
        have |= _DV_HAVE_ZONE;
      }
    } else if ((len == 7) && (digits == 5) && (b[1] == ':') && (b[4] == ':')) {
      /* h:mm:ss */
      value[_DV_HOUR] = b[0] ^ '0';
      value[_DV_MINUTE] = (b[2] ^ '0') * 10u + (b[3] ^ '0');
      value[_DV_SECOND] = (b[5] ^ '0') * 10u + (b[6] ^ '0');
      have |= _DV_HAVE_TIME;
    } else if ((len == 6) && (digits == 4) && (tab_char[b[0]] & (_DC_Zz + _DC_PLUS + _DC_MINUS)) && (b[3] == ':')) {
      /* [Zz+-]oh:om */
      value[_DV_OFFS] = (b[1] ^ '0') * 36000u + (b[2] ^ '0') * 3600u + (b[4] ^ '0') * 600u + (b[5] ^ '0') * 60u;
      if (b[0] == '-')
        value[_DV_OFFS] = -value[_DV_OFFS];
      have |= _DV_HAVE_ZONE;
    } else if ((len == 8) && (digits == 6)) {
      if ((b[2] == ':') && (b[5] == ':')) {
        /* hh:mm:ss */
        value[_DV_HOUR] = (b[0] ^ '0') * 10u + (b[1] ^ '0');
        value[_DV_MINUTE] = (b[3] ^ '0') * 10u + (b[4] ^ '0');
        value[_DV_SECOND] = (b[6] ^ '0') * 10u + (b[7] ^ '0');
        have |= _DV_HAVE_TIME;
      } else if ((b[2] == '/') && (b[5] == '/')) {
        /* MM/DD/YY */
        value[_DV_MONTH] = (b[0] ^ '0') * 10u + (b[1] ^ '0');
        value[_DV_DAY] = (b[3] ^ '0') * 10u + (b[4] ^ '0');
        value[_DV_YEAR] = (b[6] ^ '0') * 10u + (b[7] ^ '0');
        value[_DV_YEAR] += (value[_DV_YEAR] < 70) ? 2000 : 1900;
        have |= _DV_HAVE_DATE;
      }
    } else if ((len == 10) && (digits == 8)) {
      if ((b[2] == '/') && (b[5] == '/')) {
        /* MM/DD/YYYY */
        value[_DV_MONTH] = (b[0] ^ '0') * 10u + (b[1] ^ '0');
        value[_DV_DAY] = (b[3] ^ '0') * 10u + (b[4] ^ '0');
        value[_DV_YEAR] = (b[6] ^ '0') * 1000u + (b[7] ^ '0') * 100u + (b[8] ^ '0') * 10u + (b[9] ^ '0');
        have |= _DV_HAVE_DATE;
      } else if ((b[4] == '-') && (b[7] == '-')) {
        /* YYYY-MM-DD */
        value[_DV_YEAR] = (b[0] ^ '0') * 1000u + (b[1] ^ '0') * 100u + (b[2] ^ '0') * 10u + (b[3] ^ '0');
        value[_DV_MONTH] = (b[5] ^ '0') * 10u + (b[6] ^ '0');
        value[_DV_DAY] = (b[8] ^ '0') * 10u + (b[9] ^ '0');
        have |= _DV_HAVE_DATE;
      }
    } else if ((len > 0) && (len < sizeof (buf))) {
      uint32_t _b = 0, _m, _e = sizeof (strings) / sizeof (strings[0]);
      int _d;
      /* known word */
      memset (buf, ' ', sizeof (buf));
      for (_m = 0; _m < len; _m++)
        buf[_m] |= b[_m];
      do {
        _m = (_b + _e) >> 1;
        _d = memcmp (buf, strings[_m].s, sizeof (buf) - 1);
        if (_d < 0) {
          _e = _m;
        } else if (_d > 0) {
          _b = _m + 1;
        } else {
          break;
        }
      } while (_b < _e);
      if (!_d) {
        value[strings[_m].type] = strings[_m].value;
        have |= word_have[strings[_m].type];
      }
    }
  } while (*p);
  if (value[_DV_AM]) {
    if ((value[_DV_AM] == 1) && (value[_DV_HOUR] >= 12))
      value[_DV_HOUR] -= 12;
    else if ((value[_DV_AM] == 2) && (value[_DV_HOUR] < 12))
      value[_DV_HOUR] += 12;
  }
  /* relative time */
  if ((have & (_DV_HAVE_DATE + _DV_HAVE_TIME)) == 0) {
    if (have & _DV_HAVE_JTIME) {
      ts->tv_sec = jtime;
      ts->tv_nsec = value[_DV_FRAC];
    }
    return 0;
  }
  if ((have & _DV_HAVE_DATE) == 0) {
    ts->tv_sec = ts->tv_sec / (24 * 60 * 60) * (24 * 60 * 60)
               + value[_DV_HOUR] * 3600 + value[_DV_MINUTE] * 60 + value[_DV_SECOND]
               - value[_DV_OFFS];
    ts->tv_nsec = value[_DV_FRAC];
    return 0;
  }
  /* with date */
  {
    struct tm tm = {
      .tm_sec    = value[_DV_SECOND],
      .tm_min    = value[_DV_MINUTE],
      .tm_hour   = value[_DV_HOUR],
      .tm_mday   = value[_DV_DAY],
      .tm_mon    = value[_DV_MONTH] - 1,
      .tm_year   = value[_DV_YEAR] - 1900,
      .tm_wday   = value[_DV_WEEKDAY],
      .tm_isdst  = 0
    };
    const char *tz = getenv ("TZ");
    if (tz) {
      if (tz[0]) {
        char *_tz = strdup (tz);
        setenv ("TZ", "", 1);
        tzset ();
        res = mktime (&tm);
        setenv ("TZ", _tz, 1);
        free (_tz);
        tzset ();
      } else {
        tzset ();
        res = mktime (&tm);
      }
    } else {
      setenv ("TZ", "", 1);
      tzset ();
      res = mktime (&tm);
      unsetenv ("TZ");
      tzset ();
    }
  }
  if (res == -1)
    return errno;
  res -= value[_DV_OFFS];
  ts->tv_sec = res;
  ts->tv_nsec = value[_DV_FRAC];
  return 0;
}

void xine_ts_add (struct timespec *a, const struct timespec *b) {
  if (!a || !b)
    return;
  a->tv_sec += b->tv_sec;
  a->tv_nsec += b->tv_nsec;
  if (a->tv_nsec >= 1000000000) {
    a->tv_nsec -= 1000000000;
    a->tv_sec += 1;
  }
}

void xine_ts_sub (struct timespec *a, const struct timespec *b) {
  if (!a || !b)
    return;
  a->tv_sec -= b->tv_sec;
  a->tv_nsec -= b->tv_nsec;
  if (a->tv_nsec < 0) {
    a->tv_nsec += 1000000000;
    a->tv_sec -= 1;
  }
}

int64_t xine_ts_to_timebase (const struct timespec *ts, uint32_t timebase) {
  uint32_t fracbase;
  int64_t res;
  if (!ts || !timebase)
    return 0;
  fracbase = (1000000000u + (timebase >> 1)) / timebase;
  res = (int64_t)ts->tv_sec * (int32_t)timebase;
  if (fracbase)
    res += ((uint32_t)ts->tv_nsec + (fracbase >> 1)) / fracbase;
  return res;
}

/** xine rational numbers, taken from TJtools. */
void xine_rats_shorten (xine_rats_t *value) {
  static const unsigned char primediffs[] = {
    /* just say 'erat -d 75 3 32768' ;-) */
   3,  2,  2,  4,  2,  4,  2,  4,  6,  2,  6,  4,  2,  4,  6,  6,  2,  6, 
   4,  2,  6,  4,  6,  8,  4,  2,  4,  2,  4, 14,  4,  6,  2, 10,  2,  6, 
   6,  4,  6,  6,  2, 10,  2,  4,  2, 12, 12,  4,  2,  4,  6,  2, 10,  6, 
   6,  6,  2,  6,  4,  2, 10, 14,  4,  2,  4, 14,  6, 10,  2,  4,  6,  8, 
   6,  6,  4,  6,  8,  4,  8, 10,  2, 10,  2,  6,  4,  6,  8,  4,  2,  4, 
  12,  8,  4,  8,  4,  6, 12,  2, 18,  6, 10,  6,  6,  2,  6, 10,  6,  6, 
   2,  6,  6,  4,  2, 12, 10,  2,  4,  6,  6,  2, 12,  4,  6,  8, 10,  8, 
  10,  8,  6,  6,  4,  8,  6,  4,  8,  4, 14, 10, 12,  2, 10,  2,  4,  2, 
  10, 14,  4,  2,  4, 14,  4,  2,  4, 20,  4,  8, 10,  8,  4,  6,  6, 14, 
   4,  6,  6,  8,  6, 12,  4,  6,  2, 10,  2,  6, 10,  2, 10,  2,  6, 18, 
   4,  2,  4,  6,  6,  8,  6,  6, 22,  2, 10,  8, 10,  6,  6,  8, 12,  4, 
   6,  6,  2,  6, 12, 10, 18,  2,  4,  6,  2,  6,  4,  2,  4, 12,  2,  6, 
  34,  6,  6,  8, 18, 10, 14,  4,  2,  4,  6,  8,  4,  2,  6, 12, 10,  2, 
   4,  2,  4,  6, 12, 12,  8, 12,  6,  4,  6,  8,  4,  8,  4, 14,  4,  6, 
   2,  4,  6,  2,  6, 10, 20,  6,  4,  2, 24,  4,  2, 10, 12,  2, 10,  8, 
   6,  6,  6, 18,  6,  4,  2, 12, 10, 12,  8, 16, 14,  6,  4,  2,  4,  2, 
  10, 12,  6,  6, 18,  2, 16,  2, 22,  6,  8,  6,  4,  2,  4,  8,  6, 10, 
   2, 10, 14, 10,  6, 12,  2,  4,  2, 10, 12,  2, 16,  2,  6,  4,  2, 10, 
   8, 18, 24,  4,  6,  8, 16,  2,  4,  8, 16,  2,  4,  8,  6,  6,  4, 12, 
   2, 22,  6,  2,  6,  4,  6, 14,  6,  4,  2,  6,  4,  6, 12,  6,  6, 14, 
   4,  6, 12,  8,  6,  4, 26, 18, 10,  8,  4,  6,  2,  6, 22, 12,  2, 16, 
   8,  4, 12, 14, 10,  2,  4,  8,  6,  6,  4,  2,  4,  6,  8,  4,  2,  6, 
  10,  2, 10,  8,  4, 14, 10, 12,  2,  6,  4,  2, 16, 14,  4,  6,  8,  6, 
   4, 18,  8, 10,  6,  6,  8, 10, 12, 14,  4,  6,  6,  2, 28,  2, 10,  8, 
   4, 14,  4,  8, 12,  6, 12,  4,  6, 20, 10,  2, 16, 26,  4,  2, 12,  6, 
   4, 12,  6,  8,  4,  8, 22,  2,  4,  2, 12, 28,  2,  6,  6,  6,  4,  6, 
   2, 12,  4, 12,  2, 10,  2, 16,  2, 16,  6, 20, 16,  8,  4,  2,  4,  2, 
  22,  8, 12,  6, 10,  2,  4,  6,  2,  6, 10,  2, 12, 10,  2, 10, 14,  6, 
   4,  6,  8,  6,  6, 16, 12,  2,  4, 14,  6,  4,  8, 10,  8,  6,  6, 22, 
   6,  2, 10, 14,  4,  6, 18,  2, 10, 14,  4,  2, 10, 14,  4,  8, 18,  4, 
   6,  2,  4,  6,  2, 12,  4, 20, 22, 12,  2,  4,  6,  6,  2,  6, 22,  2, 
   6, 16,  6, 12,  2,  6, 12, 16,  2,  4,  6, 14,  4,  2, 18, 24, 10,  6, 
   2, 10,  2, 10,  2, 10,  6,  2, 10,  2, 10,  6,  8, 30, 10,  2, 10,  8, 
   6, 10, 18,  6, 12, 12,  2, 18,  6,  4,  6,  6, 18,  2, 10, 14,  6,  4, 
   2,  4, 24,  2, 12,  6, 16,  8,  6,  6, 18, 16,  2,  4,  6,  2,  6,  6, 
  10,  6, 12, 12, 18,  2,  6,  4, 18,  8, 24,  4,  2,  4,  6,  2, 12,  4, 
  14, 30, 10,  6, 12, 14,  6, 10, 12,  2,  4,  6,  8,  6, 10,  2,  4, 14, 
   6,  6,  4,  6,  2, 10,  2, 16, 12,  8, 18,  4,  6, 12,  2,  6,  6,  6, 
  28,  6, 14,  4,  8, 10,  8, 12, 18,  4,  2,  4, 24, 12,  6,  2, 16,  6, 
   6, 14, 10, 14,  4, 30,  6,  6,  6,  8,  6,  4,  2, 12,  6,  4,  2,  6, 
  22,  6,  2,  4, 18,  2,  4, 12,  2,  6,  4, 26,  6,  6,  4,  8, 10, 32, 
  16,  2,  6,  4,  2,  4,  2, 10, 14,  6,  4,  8, 10,  6, 20,  4,  2,  6, 
  30,  4,  8, 10,  6,  6,  8,  6, 12,  4,  6,  2,  6,  4,  6,  2, 10,  2, 
  16,  6, 20,  4, 12, 14, 28,  6, 20,  4, 18,  8,  6,  4,  6, 14,  6,  6, 
  10,  2, 10, 12,  8, 10,  2, 10,  8, 12, 10, 24,  2,  4,  8,  6,  4,  8, 
  18, 10,  6,  6,  2,  6, 10, 12,  2, 10,  6,  6,  6,  8,  6, 10,  6,  2, 
   6,  6,  6, 10,  8, 24,  6, 22,  2, 18,  4,  8, 10, 30,  8, 18,  4,  2, 
  10,  6,  2,  6,  4, 18,  8, 12, 18, 16,  6,  2, 12,  6, 10,  2, 10,  2, 
   6, 10, 14,  4, 24,  2, 16,  2, 10,  2, 10, 20,  4,  2,  4,  8, 16,  6, 
   6,  2, 12, 16,  8,  4,  6, 30,  2, 10,  2,  6,  4,  6,  6,  8,  6,  4, 
  12,  6,  8, 12,  4, 14, 12, 10, 24,  6, 12,  6,  2, 22,  8, 18, 10,  6, 
  14,  4,  2,  6, 10,  8,  6,  4,  6, 30, 14, 10,  2, 12, 10,  2, 16,  2, 
  18, 24, 18,  6, 16, 18,  6,  2, 18,  4,  6,  2, 10,  8, 10,  6,  6,  8, 
   4,  6,  2, 10,  2, 12,  4,  6,  6,  2, 12,  4, 14, 18,  4,  6, 20,  4, 
   8,  6,  4,  8,  4, 14,  6,  4, 14, 12,  4,  2, 30,  4, 24,  6,  6, 12, 
  12, 14,  6,  4,  2,  4, 18,  6, 12,  8,  6,  4, 12,  2, 12, 30, 16,  2, 
   6, 22, 14,  6, 10, 12,  6,  2,  4,  8, 10,  6,  6, 24, 14,  6,  4,  8, 
  12, 18, 10,  2, 10,  2,  4,  6, 20,  6,  4, 14,  4,  2,  4, 14,  6, 12, 
  24, 10,  6,  8, 10,  2, 30,  4,  6,  2, 12,  4, 14,  6, 34, 12,  8,  6, 
  10,  2,  4, 20, 10,  8, 16,  2, 10, 14,  4,  2, 12,  6, 16,  6,  8,  4, 
   8,  4,  6,  8,  6,  6, 12,  6,  4,  6,  6,  8, 18,  4, 20,  4, 12,  2, 
  10,  6,  2, 10, 12,  2,  4, 20,  6, 30,  6,  4,  8, 10, 12,  6,  2, 28, 
   2,  6,  4,  2, 16, 12,  2,  6, 10,  8, 24, 12,  6, 18,  6,  4, 14,  6, 
   4, 12,  8,  6, 12,  4,  6, 12,  6, 12,  2, 16, 20,  4,  2, 10, 18,  8, 
   4, 14,  4,  2,  6, 22,  6, 14,  6,  6, 10,  6,  2, 10,  2,  4,  2, 22, 
   2,  4,  6,  6, 12,  6, 14, 10, 12,  6,  8,  4, 36, 14, 12,  6,  4,  6, 
   2, 12,  6, 12, 16,  2, 10,  8, 22,  2, 12,  6,  4,  6, 18,  2, 12,  6, 
   4, 12,  8,  6, 12,  4,  6, 12,  6,  2, 12, 12,  4, 14,  6, 16,  6,  2, 
  10,  8, 18,  6, 34,  2, 28,  2, 22,  6,  2, 10, 12,  2,  6,  4,  8, 22, 
   6,  2, 10,  8,  4,  6,  8,  4, 12, 18, 12, 20,  4,  6,  6,  8,  4,  2, 
  16, 12,  2, 10,  8, 10,  2,  4,  6, 14, 12, 22,  8, 28,  2,  4, 20,  4, 
   2,  4, 14, 10, 12,  2, 12, 16,  2, 28,  8, 22,  8,  4,  6,  6, 14,  4, 
   8, 12,  6,  6,  4, 20,  4, 18,  2, 12,  6,  4,  6, 14, 18, 10,  8, 10, 
  32,  6, 10,  6,  6,  2,  6, 16,  6,  2, 12,  6, 28,  2, 10,  8, 16,  6, 
   8,  6, 10, 24, 20, 10,  2, 10,  2, 12,  4,  6, 20,  4,  2, 12, 18, 10, 
   2, 10,  2,  4, 20, 16, 26,  4,  8,  6,  4, 12,  6,  8, 12, 12,  6,  4, 
   8, 22,  2, 16, 14, 10,  6, 12, 12, 14,  6,  4, 20,  4, 12,  6,  2,  6, 
   6, 16,  8, 22,  2, 28,  8,  6,  4, 20,  4, 12, 24, 20,  4,  8, 10,  2, 
  16,  2, 12, 12, 34,  2,  4,  6, 12,  6,  6,  8,  6,  4,  2,  6, 24,  4, 
  20, 10,  6,  6, 14,  4,  6,  6,  2, 12,  6, 10,  2, 10,  6, 20,  4, 26, 
   4,  2,  6, 22,  2, 24,  4,  6,  2,  4,  6, 24,  6,  8,  4,  2, 34,  6, 
   8, 16, 12,  2, 10,  2, 10,  6,  8,  4,  8, 12, 22,  6, 14,  4, 26,  4, 
   2, 12, 10,  8,  4,  8, 12,  4, 14,  6, 16,  6,  8,  4,  6,  6,  8,  6, 
  10, 12,  2,  6,  6, 16,  8,  6,  6, 12, 10,  2,  6, 18,  4,  6,  6,  6, 
  12, 18,  8,  6, 10,  8, 18,  4, 14,  6, 18, 10,  8, 10, 12,  2,  6, 12, 
  12, 36,  4,  6,  8,  4,  6,  2,  4, 18, 12,  6,  8,  6,  6,  4, 18,  2, 
   4,  2, 24,  4,  6,  6, 14, 30,  6,  4,  6, 12,  6, 20,  4,  8,  4,  8, 
   6,  6,  4, 30,  2, 10, 12,  8, 10,  8, 24,  6, 12,  4, 14,  4,  6,  2, 
  28, 14, 16,  2, 12,  6,  4, 20, 10,  6,  6,  6,  8, 10, 12, 14, 10, 14, 
  16, 14, 10, 14,  6, 16,  6,  8,  6, 16, 20, 10,  2,  6,  4,  2,  4, 12, 
   2, 10,  2,  6, 22,  6,  2,  4, 18,  8, 10,  8, 22,  2, 10, 18, 14,  4, 
   2,  4, 18,  2,  4,  6,  8, 10,  2, 30,  4, 30,  2, 10,  2, 18,  4, 18, 
   6, 14, 10,  2,  4, 20, 36,  6,  4,  6, 14,  4, 20, 10, 14, 22,  6,  2, 
  30, 12, 10, 18,  2,  4, 14,  6, 22, 18,  2, 12,  6,  4,  8,  4,  8,  6, 
  10,  2, 12, 18, 10, 14, 16, 14,  4,  6,  6,  2,  6,  4,  2, 28,  2, 28, 
   6,  2,  4,  6, 14,  4, 12, 14, 16, 14,  4,  6,  8,  6,  4,  6,  6,  6, 
   8,  4,  8,  4, 14, 16,  8,  6,  4, 12,  8, 16,  2, 10,  8,  4,  6, 26, 
   6, 10,  8,  4,  6, 12, 14, 30,  4, 14, 22,  8, 12,  4,  6,  8, 10,  6, 
  14, 10,  6,  2, 10, 12, 12, 14,  6,  6, 18, 10,  6,  8, 18,  4,  6,  2, 
   6, 10,  2, 10,  8,  6,  6, 10,  2, 18, 10,  2, 12,  4,  6,  8, 10, 12, 
  14, 12,  4,  8, 10,  6,  6, 20,  4, 14, 16, 14, 10,  8, 10, 12,  2, 18, 
   6, 12, 10, 12,  2,  4,  2, 12,  6,  4,  8,  4, 44,  4,  2,  4,  2, 10, 
  12,  6,  6, 14,  4,  6,  6,  6,  8,  6, 36, 18,  4,  6,  2, 12,  6,  6, 
   6,  4, 14, 22, 12,  2, 18, 10,  6, 26, 24,  4,  2,  4,  2,  4, 14,  4, 
   6,  6,  8, 16, 12,  2, 42,  4,  2,  4, 24,  6,  6,  2, 18,  4, 14,  6, 
  28, 18, 14,  6, 10, 12,  2,  6, 12, 30,  6,  4,  6,  6, 14,  4,  2, 24, 
   4,  6,  6, 26, 10, 18,  6,  8,  6,  6, 30,  4, 12, 12,  2, 16,  2,  6, 
   4, 12, 18,  2,  6,  4, 26, 12,  6, 12,  4, 24, 24, 12,  6,  2, 12, 28, 
   8,  4,  6, 12,  2, 18,  6,  4,  6,  6, 20, 16,  2,  6,  6, 18, 10,  6, 
   2,  4,  8,  6,  6, 24, 16,  6,  8, 10,  6, 14, 22,  8, 16,  6,  2, 12, 
   4,  2, 22,  8, 18, 34,  2,  6, 18,  4,  6,  6,  8, 10,  8, 18,  6,  4, 
   2,  4,  8, 16,  2, 12, 12,  6, 18,  4,  6,  6,  6,  2,  6, 12, 10, 20, 
  12, 18,  4,  6,  2, 16,  2, 10, 14,  4, 30,  2, 10, 12,  2, 24,  6, 16, 
   8, 10,  2, 12, 22,  6,  2, 16, 20, 10,  2, 12, 12, 18, 10, 12,  6,  2, 
  10,  2,  6, 10, 18,  2, 12,  6,  4,  6,  2, 24, 28,  2,  4,  2, 10,  2, 
  16, 12,  8, 22,  2,  6,  4,  2, 10,  6, 20, 12, 10,  8, 12,  6,  6,  6, 
   4, 18,  2,  4, 12, 18,  2, 12,  6,  4,  2, 16, 12, 12, 14,  4,  8, 18, 
   4, 12, 14,  6,  6,  4,  8,  6,  4, 20, 12, 10, 14,  4,  2, 16,  2, 12, 
  30,  4,  6, 24, 20, 24, 10,  8, 12, 10, 12,  6, 12, 12,  6,  8, 16, 14, 
   6,  4,  6, 36, 20, 10, 30, 12,  2,  4,  2, 28, 12, 14,  6, 22,  8,  4, 
  18,  6, 14, 18,  4,  6,  2,  6, 34, 18,  2, 16,  6, 18,  2, 24,  4,  2, 
   6, 12,  6, 12, 10,  8,  6, 16, 12,  8, 10, 14, 40,  6,  2,  6,  4, 12, 
  14,  4,  2,  4,  2,  4,  8,  6, 10,  6,  6,  2,  6,  6,  6, 12,  6, 24, 
  10,  2, 10,  6, 12,  6,  6, 14,  6,  6, 52, 20,  6, 10,  2, 10,  8, 10, 
  12, 12,  2,  6,  4, 14, 16,  8, 12,  6, 22,  2, 10,  8,  6, 22,  2, 22, 
   6,  8, 10, 12, 12,  2, 10,  6, 12,  2,  4, 14, 10,  2,  6, 18,  4, 12, 
   8, 18, 12,  6,  6,  4,  6,  6, 14,  4,  2, 12, 12,  4,  6, 18, 18, 12, 
   2, 16, 12,  8, 18, 10, 26,  4,  6,  8,  6,  6,  4,  2, 10, 20,  4,  6, 
   8,  4, 20, 10,  2, 34,  2,  4, 24,  2, 12, 12, 10,  6,  2, 12, 30,  6, 
  12, 16, 12,  2, 22, 18, 12, 14, 10,  2, 12, 12,  4,  2,  4,  6, 12,  2, 
  16, 18,  2, 40,  8, 16,  6,  8, 10,  2,  4, 18,  8, 10,  8, 12,  4, 18, 
   2, 18, 10,  2,  4,  2,  4,  8, 28,  2,  6, 22, 12,  6, 14, 18,  4,  6, 
   8,  6,  6, 10,  8,  4,  2, 18, 10,  6, 20, 22,  8,  6, 30,  4,  2,  4, 
  18,  6, 30,  2,  4,  8,  6,  4,  6, 12, 14, 34, 14,  6,  4,  2,  6,  4, 
  14,  4,  2,  6, 28,  2,  4,  6,  8, 10,  2, 10,  2, 10,  2,  4, 30,  2, 
  12, 12, 10, 18, 12, 14, 10,  2, 12,  6, 10,  6, 14, 12,  4, 14,  4, 18, 
   2, 10,  8,  4,  8, 10, 12, 18, 18,  8,  6, 18, 16, 14,  6,  6, 10, 14, 
   4,  6,  2, 12, 12,  4,  6,  6, 12,  2, 16,  2, 12,  6,  4, 14,  6,  4, 
   2, 12, 18,  4, 36, 18, 12, 12,  2,  4,  2,  4,  8, 12,  4, 36,  6, 18, 
   2, 12, 10,  6, 12, 24,  8,  6,  6, 16, 12,  2, 18, 10, 20, 10,  2,  6, 
  18,  4,  2, 40,  6,  2, 16,  2,  4,  8, 18, 10, 12,  6,  2, 10,  8,  4, 
   6, 12,  2, 10, 18,  8,  6,  4, 20,  4,  6, 36,  6,  2, 10,  6, 24,  6, 
  14, 16,  6, 18,  2, 10, 20, 10,  8,  6,  4,  6,  2, 10,  2, 12,  4,  2, 
   4,  8, 10,  6, 12, 18, 14, 12, 16,  8,  6, 16,  8,  4,  2,  6, 18, 24, 
  18, 10, 12,  2,  4, 14, 10,  6,  6,  6, 18, 12,  2, 28, 18, 14, 16, 12, 
  14, 24, 12, 22,  6,  2, 10,  8,  4,  2,  4, 14, 12,  6,  4,  6, 14,  4, 
   2,  4, 30,  6,  2,  6, 10,  2, 30, 22,  2,  4,  6,  8,  6,  6, 16, 12, 
  12,  6,  8,  4,  2, 24, 12,  4,  6,  8,  6,  6, 10,  2,  6, 12, 28, 14, 
   6,  4, 12,  8,  6, 12,  4,  6, 14,  6, 12, 10,  6,  6,  8,  6,  6,  4, 
   2,  4,  8, 12,  4, 14, 18, 10,  2, 16,  6, 20,  6, 10,  8,  4, 30, 36, 
  12,  8, 22, 12,  2,  6, 12, 16,  6,  6,  2, 18,  4, 26,  4,  8, 18, 10, 
   8, 10,  6, 14,  4, 20, 22, 18, 12,  8, 28, 12,  6,  6,  8,  6, 12, 24, 
  16, 14,  4, 14, 12,  6, 10, 12, 20,  6,  4,  8, 18, 12, 18, 10,  2,  4, 
  20, 10, 14,  4,  6,  2, 10, 24, 18,  2,  4, 20, 16, 14, 10, 14,  6,  4, 
   6, 20,  6, 10,  6,  2, 12,  6, 30, 10,  8,  6,  4,  6,  8, 40,  2,  4, 
   2, 12, 18,  4,  6,  8, 10,  6, 18, 18,  2, 12, 16,  8,  6,  4,  6,  6, 
   2, 52, 14,  4, 20, 16,  2,  4,  6, 12,  2,  6, 12, 12,  6,  4, 14, 10, 
   6,  6, 14, 10, 14, 16,  8,  6, 12,  4,  8, 22,  6,  2, 18, 22,  6,  2, 
  18,  6, 16, 14, 10,  6, 12,  2,  6,  4,  8, 18, 12, 16,  2,  4, 14,  4, 
   8, 12, 12, 30, 16,  8,  4,  2,  6, 22, 12,  8, 10,  6,  6,  6, 14,  6, 
  18, 10, 12,  2, 10,  2,  4, 26,  4, 12,  8,  4, 18,  8, 10, 14, 16,  6, 
   6,  8, 10,  6,  8,  6, 12, 10, 20, 10,  8,  4, 12, 26, 18,  4, 12, 18, 
   6, 30,  6,  8,  6, 22, 12,  2,  4,  6,  6,  2, 10,  2,  4,  6,  6,  2, 
   6, 22, 18,  6, 18, 12,  8, 12,  6, 10, 12,  2, 16,  2, 10,  2, 10, 18, 
   6, 20,  4,  2,  6, 22,  6,  6, 18,  6, 14, 12, 16,  2,  6,  6,  4, 14, 
  12,  4,  2, 18, 16, 36, 12,  6, 14, 28,  2, 12,  6, 12,  6,  4,  2, 16, 
  30,  8, 24,  6, 30, 10,  2, 18,  4,  6, 12,  8, 22,  2,  6, 22, 18,  2, 
  10,  2, 10, 30,  2, 28,  6, 14, 16,  6, 20, 16,  2,  6,  4, 32,  4,  2, 
   4,  6,  2, 12,  4,  6,  6, 12,  2,  6,  4,  6,  8,  6,  4, 20,  4, 32, 
  10,  8, 16,  2, 22,  2,  4,  6,  8,  6, 16, 14,  4, 18,  8,  4, 20,  6, 
  12, 12,  6, 10,  2, 10,  2, 12, 28, 12, 18,  2, 18, 10,  8, 10, 48,  2, 
   4,  6,  8, 10,  2, 10, 30,  2, 36,  6, 10,  6,  2, 18,  4,  6,  8, 16, 
  14, 16,  6, 14,  4, 20,  4,  6,  2, 10, 12,  2,  6, 12,  6,  6,  4, 12, 
   2,  6,  4, 12,  6,  8,  4,  2,  6, 18, 10,  6,  8, 12,  6, 22,  2,  6, 
  12, 18,  4, 14,  6,  4, 20,  6, 16,  8,  4,  8, 22,  8, 12,  6,  6, 16, 
  12, 18, 30,  8,  4,  2,  4,  6, 26,  4, 14, 24, 22,  6,  2,  6, 10,  6, 
  14,  6,  6, 12, 10,  6,  2, 12, 10, 12,  8, 18, 18, 10,  6,  8, 16,  6, 
   6,  8, 16, 20,  4,  2, 10,  2, 10, 12,  6,  8,  6, 10, 20, 10, 18, 26, 
   4,  6, 30,  2,  4,  8,  6, 12, 12, 18,  4,  8, 22,  6,  2, 12, 34,  6, 
  18, 12,  6,  2, 28, 14, 16, 14,  4, 14, 12,  4,  6,  6,  2, 36,  4,  6, 
  20, 12, 24,  6, 22,  2, 16, 18, 12, 12, 18,  2,  6,  6,  6,  4,  6, 14, 
   4,  2, 22,  8, 12,  6, 10,  6,  8, 12, 18, 12,  6, 10,  2, 22, 14,  6, 
   6,  4, 18,  6, 20, 22,  2, 12, 24,  4, 18, 18,  2, 22,  2,  4, 12,  8, 
  12, 10, 14,  4,  2, 18, 16, 38,  6,  6,  6, 12, 10,  6, 12,  8,  6,  4, 
   6, 14, 30,  6, 10,  8, 22,  6,  8, 12, 10,  2, 10,  2,  6, 10,  2, 10, 
  12, 18, 20,  6,  4,  8, 22,  6,  6, 30,  6, 14,  6, 12, 12,  6, 10,  2, 
  10, 30,  2, 16,  8,  4,  2,  6, 18,  4,  2,  6,  4, 26,  4,  8,  6, 10, 
   2,  4,  6,  8,  4,  6, 30, 12,  2,  6,  6,  4, 20, 22,  8,  4,  2,  4, 
  72,  8,  4,  8, 22,  2,  4, 14, 10,  2,  4, 20,  6, 10, 18,  6, 20, 16, 
   6,  8,  6,  4, 20, 12, 22,  2,  4,  2, 12, 10, 18,  2, 22,  6, 18, 30, 
   2, 10, 14, 10,  8, 16, 50,  6, 10,  8, 10, 12,  6, 18,  2, 22,  6,  2, 
   4,  6,  8,  6,  6, 10, 18,  2, 22,  2, 16, 14, 10,  6,  2, 12, 10, 20, 
   4, 14,  6,  4, 36,  2,  4,  6, 12,  2,  4, 14, 12,  6,  4,  6,  2,  6, 
   4, 20, 10,  2, 10,  6, 12,  2, 24, 12, 12,  6,  6,  4, 24,  2,  4, 24, 
   2,  6,  4,  6,  8, 16,  6,  2, 10, 12, 14,  6, 34,  6, 14,  6,  4,  2, 
  30,  0
  };
  int64_t num, den, left, min, max;

  if (!value)
    return;
  if ((value->num == 0) || (value->den == 0)) {
    value->den = 1;
    return;
  }

  if (value->den < 0)
    value->num = -value->num, value->den = -value->den;
  num = value->num;
  den = value->den;
  min = num < 0 ? -num : num;
  if (min < den) {
    max = den;
  } else {
    max = min;
    min = den;
  }
  left = 1;

  /* prime 2, simple and fast */
  while (!((min | max) & 1)) {
    min >>= 1;
    max >>= 1;
  }
  while (!(min & 1)) {
    min >>= 1;
    left += left;
  }

  /* now, the remaining primes */
  {
    int prime = 0;
    const unsigned char *p = primediffs;
    while (*p) {
      prime += *p++;
      if (min < prime * prime)
        break;
      while (min % prime == 0) {
        min /= prime;
        if (max % prime != 0) {
          left *= prime;
          break;
        }
        max /= prime;
      }
      while (min % prime == 0) {
        min /= prime;
        left *= prime;
      }
    }
  }
  /* after stripping all prime factors up to sqrt (min), the rest _is_ prime */
  if (max % min == 0) {
    max /= min;
    min = 1;
  }
  min *= left;
  if (num < 0) {
    if (-num < den) {
      value->num = -min;
      value->den = max;
    } else {
      value->num = -max;
      value->den = min;
    }
  } else {
    if (num < den) {
      value->num = min;
      value->den = max;
    } else {
      value->num = max;
      value->den = min;
    }
  }
}

