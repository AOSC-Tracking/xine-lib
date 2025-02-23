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
 * config file management
 */

#ifndef HAVE_CONFIGFILE_H
#define HAVE_CONFIGFILE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <pthread.h>

#include <xine.h>

#define CONFIG_FILE_VERSION 2

/**
 * config entries above this experience
 * level must never be changed from MRL
 */
#define XINE_CONFIG_SECURITY 30


typedef struct cfg_entry_s cfg_entry_t;
typedef struct config_values_s config_values_t;

struct cfg_entry_s {
  cfg_entry_t     *next;
  config_values_t *config;

  char            *key;
  int              type;

  /** user experience level */
  int              exp_level;

  /** type unknown */
  char            *unknown_value;

  /** type string */
  char            *str_value;
  char            *str_default;

  /** common to range, enum, num, bool: */
  int              num_value;
  int              num_default;

  /** type range specific: */
  int              range_min;
  int              range_max;  /* also used for enum */

  /** type enum specific: */
  char           **enum_values;

  /** help info for the user */
  char            *description;
  char            *help;

  /** callback function and data for live changeable values */
  xine_config_cb_t callback;
  void            *callback_data;
};

struct config_values_s {

  /*
   * register config values
   *
   * these functions return the current value of the
   * registered item, i.e. the default value if it was
   * not found in the config file or the current value
   * from the config file otherwise
   * NOTE: config entries registered during class init of
   * input, demux, decoder, or post plugins will _always_
   * show up in the application - even if that plugin is
   * not currently loaded.
   *
   * NOTE on callbacks:
   * - callback shall be safe to run from _any_ thread.
   *   There will be no 2 calls at the same time, though.
   * - callback shall be safe to call at any time between
   *   entering register_foo (), and leaving unregister_foo ().
   * - There can be multiple callbacks for the same key.
   *   They will run in no fixed order.
   * - if cb_data is a real pointer, make sure it points to
   *   valid thread shared memory (malloc'ed or static).
   *   Plain stack variables will not work, and may cause
   *   strange malfunction.
   */

  char* (*register_string) (config_values_t *self,
			    const char *key,
			    const char *def_value,
			    const char *description,
			    const char *help,
			    int exp_level,
			    xine_config_cb_t changed_cb,
			    void *cb_data);

  char* (*register_filename) (config_values_t *self,
			      const char *key,
			      const char *def_value,
			      int req_type,
			      const char *description,
			      const char *help,
			      int exp_level,
			      xine_config_cb_t changed_cb,
			      void *cb_data);

  int (*register_range) (config_values_t *self,
			 const char *key,
			 int def_value,
			 int min, int max,
			 const char *description,
			 const char *help,
			 int exp_level,
			 xine_config_cb_t changed_cb,
			 void *cb_data);

  int (*register_enum) (config_values_t *self,
			const char *key,
			int def_value,
			char **values,
			const char *description,
			const char *help,
			int exp_level,
			xine_config_cb_t changed_cb,
			void *cb_data);

  int (*register_num) (config_values_t *self,
		       const char *key,
		       int def_value,
		       const char *description,
		       const char *help,
		       int exp_level,
		       xine_config_cb_t changed_cb,
		       void *cb_data);

  int (*register_bool) (config_values_t *self,
			const char *key,
			int def_value,
			const char *description,
			const char *help,
			int exp_level,
			xine_config_cb_t changed_cb,
			void *cb_data);

  /** not yet implemented */
  void (*register_entry) (config_values_t *self, cfg_entry_t* entry);

  /** convenience function to update range, enum, num and bool values */
  void (*update_num) (config_values_t *self, const char *key, int value);

  /** convenience function to update string values */
  void (*update_string) (config_values_t *self, const char *key, const char *value);

  /** small utility function for enum handling */
  int (*parse_enum) (const char *str, const char **values);

  /**
   * @brief lookup config entries
   *
   * remember to call the changed_cb if it exists
   * and you changed the value of this item
   */

  cfg_entry_t* (*lookup_entry) (config_values_t *self, const char *key);

  /**
   * unregister _all_ entry callback functions for this key.
   * if there may be multiple callbacks on different cb_data,
   * consider using unregister_callbacks (self, NULL, NULL, my_data, sizeof (*my_data))
   * before freeing each instance instead. this also eliminates the need
   * to unregister every key separately.
   */
  void (*unregister_callback) (config_values_t *self, const char *key);

  /**
   * dispose of all config entries in memory
   */
  void (*dispose) (config_values_t *self);

  /**
   * callback called when a new config entry is registered
   */
  void (*set_new_entry_callback) (config_values_t *self, xine_config_cb_t new_entry_cb, void *cb_data);

  /**
   * unregister the callback
   */
  void (*unset_new_entry_callback) (config_values_t *self);

  /**
   * serialize a config entry.
   * return a base64 null terminated string.
   */
  char* (*get_serialized_entry) (config_values_t *self, const char *key);

  /**
   * deserialize a config entry.
   * value is a base 64 encoded string
   * return the key of the serialized entry
   */
  char* (*register_serialized_entry) (config_values_t *self, const char *value);

  /**
   * config values are stored here:
   */
  cfg_entry_t         *first, *last, *cur;

  /**
   * new entry callback
   */
  xine_config_cb_t    new_entry_cb;
  void                *new_entry_cbdata;

  /**
   * mutex for modification to the config
   */
  pthread_mutex_t      config_lock;

  /**
   * current config file's version number
   */
  int current_version;

  /**
   * unregister multiple entry callback functions.
   * all 3 values need to match unless they are NULL.
   * if cb_data_size is not zero, data pointers within the range
   * (cb_data <= ptr < cb_data + cb_data_size) will match.
   * returns the count of unregistered functions.
   */
  int (*unregister_callbacks) (config_values_t *self,
    const char *key, xine_config_cb_t changed_cb, void *cb_data, size_t cb_data_size);

  /**
   * Set this manually to enable logging.
   */
  xine_t *xine;

  /**
   * MT-safe convenience function to lookup string values.
   * Returns copy of current value or NULL.
   * Returned string must be freed with config->free_string().
   */
  char * (*lookup_string)(config_values_t *, const char *key);
  void   (*free_string)(config_values_t *, char **);

  /** convenience function to lookup numeric values */
  int    (*lookup_num)(config_values_t *, const char *key, int def_value);
};

/**
 * @brief allocate and init a new xine config object
 * @internal
 */
config_values_t *_x_config_init (void);

/**
 * @brief interpret stream_setup part of mrls for config value changes
 * @internal
 */

int _x_config_change_opt(config_values_t *config, const char *opt);

/** deprecated in favour of config_values_t->unregister_callbacks (). */
void _x_config_unregister_cb_class_d (config_values_t *config, void *callback_data) XINE_PROTECTED;
void _x_config_unregister_cb_class_p (config_values_t *config, xine_config_cb_t callback) XINE_PROTECTED;

#ifdef __cplusplus
}
#endif

#endif
