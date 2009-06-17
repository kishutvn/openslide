/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2009 Carnegie Mellon University
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 2.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with OpenSlide. If not, see <http://www.gnu.org/licenses/>.
 *
 *  Linking OpenSlide statically or dynamically with other modules is
 *  making a combined work based on OpenSlide. Thus, the terms and
 *  conditions of the GNU General Public License cover the whole
 *  combination.
 */

#include "config.h"

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>

#include <glib.h>

#include "openslide-private.h"
#include "openslide-cache.h"
#include "openslide-tilehelper.h"

typedef bool (*vendor_fn)(openslide_t *osr, const char *filename);

static const vendor_fn all_formats[] = {
  _openslide_try_mirax,
  _openslide_try_hamamatsu,
  _openslide_try_trestle,
  _openslide_try_aperio,
  NULL
};

static void destroy_associated_image(gpointer data) {
  struct _openslide_associated_image *img = data;

  g_free(img->argb_data);
  g_slice_free(struct _openslide_associated_image, img);
}

static bool try_all_formats(openslide_t *osr, const char *filename) {
  const vendor_fn *fn = all_formats;
  while (*fn) {
    if (osr) {
      g_hash_table_remove_all(osr->properties);
      g_hash_table_remove_all(osr->associated_images);
    }

    if ((*fn)(osr, filename)) {
      return true;
    }
    fn++;
  }
  return false;
}

bool openslide_can_open(const char *filename) {
  // quick test
  return try_all_formats(NULL, filename);
}


struct add_key_to_strv_data {
  int i;
  const char **strv;
};

static void add_key_to_strv(gpointer key,
			    gpointer value,
			    gpointer user_data) {
  struct add_key_to_strv_data *d = user_data;

  d->strv[d->i++] = key;
}

static const char **strv_from_hashtable_keys(GHashTable *h) {
  const char **result = g_new0(const char *, g_hash_table_size(h) + 1);

  struct add_key_to_strv_data data = { 0, result };
  g_hash_table_foreach(h, add_key_to_strv, &data);

  return result;
}

openslide_t *openslide_open(const char *filename) {
  // we are threading
  if (!g_thread_supported ()) g_thread_init (NULL);

  // alloc memory
  openslide_t *osr = g_slice_new0(openslide_t);
  osr->properties = g_hash_table_new_full(g_str_hash, g_str_equal,
					  g_free, g_free);
  osr->associated_images = g_hash_table_new_full(g_str_hash, g_str_equal,
						 g_free, destroy_associated_image);

  // try to read it
  if (!try_all_formats(osr, filename)) {
    // failure
    openslide_close(osr);
    return NULL;
  }

  // compute downsamples
  int64_t blw, blh;
  osr->downsamples = g_new(double, osr->layer_count);
  osr->downsamples[0] = 1.0;
  openslide_get_layer0_dimensions(osr, &blw, &blh);
  for (int32_t i = 0; i < osr->layer_count; i++) {
    int64_t w, h;
    openslide_get_layer_dimensions(osr, i, &w, &h);

    if (i > 0) {
      osr->downsamples[i] =
	(((double) blh / (double) h) +
	 ((double) blw / (double) w)) / 2.0;

      g_debug("downsample: %g", osr->downsamples[i]);

      if (osr->downsamples[i] < osr->downsamples[i - 1]) {
	g_warning("Downsampled images not correctly ordered: %g < %g",
		  osr->downsamples[i], osr->downsamples[i - 1]);
	openslide_close(osr);
	return NULL;
      }
    }
  }

  // fill in names
  osr->associated_image_names = strv_from_hashtable_keys(osr->associated_images);
  osr->property_names = strv_from_hashtable_keys(osr->properties);

  // start cache
  osr->cache = _openslide_cache_create(_OPENSLIDE_USEFUL_CACHE_SIZE);
  //osr->cache = _openslide_cache_create(0);

  return osr;
}


void openslide_close(openslide_t *osr) {
  if (osr->ops) {
    (osr->ops->destroy)(osr);
  }

  g_hash_table_unref(osr->associated_images);
  g_hash_table_unref(osr->properties);

  g_free(osr->associated_image_names);
  g_free(osr->property_names);

  g_free(osr->downsamples);

  if (osr->cache) {
    _openslide_cache_destroy(osr->cache);
  }

  g_slice_free(openslide_t, osr);
}


void openslide_get_layer0_dimensions(openslide_t *osr,
				     int64_t *w, int64_t *h) {
  openslide_get_layer_dimensions(osr, 0, w, h);
}

void openslide_get_layer_dimensions(openslide_t *osr, int32_t layer,
				    int64_t *w, int64_t *h) {
  if (layer > osr->layer_count || layer < 0) {
    *w = 0;
    *h = 0;
  } else {
    int64_t tiles_across;
    int64_t tiles_down;
    int32_t tile_width;
    int32_t tile_height;
    int32_t last_tile_width;
    int32_t last_tile_height;

    (osr->ops->get_dimensions)(osr, layer, &tiles_across, &tiles_down,
			       &tile_width, &tile_height,
			       &last_tile_width, &last_tile_height);

    *w = (tiles_across - 1) * tile_width + last_tile_width;
    *h = (tiles_down - 1) * tile_height + last_tile_height;
  }
}

const char *openslide_get_comment(openslide_t *osr) {
  return openslide_get_property_value(osr, _OPENSLIDE_COMMENT_NAME);
}


int32_t openslide_get_layer_count(openslide_t *osr) {
  return osr->layer_count;
}


int32_t openslide_get_best_layer_for_downsample(openslide_t *osr,
						double downsample) {
  // too small, return first
  if (downsample < osr->downsamples[0]) {
    return 0;
  }

  // find where we are in the middle
  for (int32_t i = 1; i < osr->layer_count; i++) {
    if (downsample < osr->downsamples[i]) {
      return i - 1;
    }
  }

  // too big, return last
  return osr->layer_count - 1;
}


double openslide_get_layer_downsample(openslide_t *osr, int32_t layer) {
  if (layer > osr->layer_count || layer < 0) {
    return 0.0;
  }

  return osr->downsamples[layer];
}


int openslide_give_prefetch_hint(openslide_t *osr,
				 int64_t x, int64_t y,
				 int32_t layer,
				 int64_t w, int64_t h) {
  // TODO
  return 0;
}

void openslide_cancel_prefetch_hint(openslide_t *osr, int prefetch_id) {
  // TODO
  return;
}

static void convert_coordinate(double downsample,
			       int64_t x, int64_t y,
			       int64_t tiles_across, int64_t tiles_down,
			       int32_t tile_width, int32_t tile_height,
			       int64_t *tile_x, int64_t *tile_y,
			       int32_t *offset_x_in_tile, int32_t *offset_y_in_tile) {
  int64_t ds_x = x / downsample;
  int64_t ds_y = y / downsample;

  // x
  *tile_x = ds_x / tile_width;
  *offset_x_in_tile = ds_x % tile_width;
  if (*tile_x >= tiles_across - 1) {
    // this is the last tile
    *tile_x = tiles_across - 1;
    *offset_x_in_tile = ds_x - ((tiles_across - 1) * tile_width);
  }

  // y
  *tile_y = ds_y / tile_height;
  *offset_y_in_tile = ds_y % tile_height;
  if (*tile_y >= tiles_down - 1) {
    // this is the last tile
    *tile_y = tiles_down - 1;
    *offset_y_in_tile = ds_y - ((tiles_down - 1) * tile_height);
  }

  /*
  g_debug("convert_coordinate: (%" PRId64 ",%" PRId64") ->"
	  " t(%" PRId64 ",%" PRId64 ") + (%d,%d)",
	  x, y, *tile_x, *tile_y, *offset_x_in_tile, *offset_y_in_tile);
  */
}

void openslide_read_region(openslide_t *osr,
			   uint32_t *dest,
			   int64_t x, int64_t y,
			   int32_t layer,
			   int64_t w, int64_t h) {
  if (w <= 0 || h <= 0) {
    return;
  }

  // start cleared
  if (dest != NULL) {
    for (int64_t i = 0; i < w * h; i++) {
      dest[i] = osr->fill_color_argb;
    }
  }

  //  for (int64_t i = 0; i < w * h; i++) {
  //    dest[i] = 0xFFFF0000; // red
  //  }

  // check constraints
  if (layer > osr->layer_count || layer < 0 || x < 0 || y < 0) {
    return;
  }

  // we could also check to make sure that (x / ds) + w and (y / ds) + h
  // doesn't exceed the bounds of the image, but this situation is
  // less harmful and can be cleanly handled by the ops backends anyway,
  // so we allow it
  //
  // also, we don't want to introduce rounding errors here with our
  // double representation of downsampling -- backends have more precise
  // ways of representing this


  // now fully within all bounds, go for it


  // get the dimensions
  int64_t tiles_across;
  int64_t tiles_down;
  int32_t tile_width;
  int32_t tile_height;
  int32_t last_tile_width;
  int32_t last_tile_height;
  (osr->ops->get_dimensions)(osr, layer, &tiles_across, &tiles_down,
			     &tile_width, &tile_height,
			     &last_tile_width, &last_tile_height);

  g_debug("tiles: %" PRId64 ",%" PRId64 " [%dx%d] + [%dx%d]",
	  tiles_across, tiles_down, tile_width, tile_height, last_tile_width, last_tile_height);

  g_assert(tiles_across > 0);
  g_assert(tiles_down > 0);
  g_assert(tile_width > 0);
  g_assert(tile_height > 0);
  g_assert(last_tile_width > 0);
  g_assert(last_tile_height > 0);

  // convert into start coordinate
  int64_t tile_x;
  int64_t tile_y;
  int32_t offset_x_in_tile;
  int32_t offset_y_in_tile;
  convert_coordinate(openslide_get_layer_downsample(osr, layer),
		     x, y,
		     tiles_across, tiles_down,
		     tile_width, tile_height,
		     &tile_x, &tile_y,
		     &offset_x_in_tile,
		     &offset_y_in_tile);


  _openslide_read_tiles(tile_x, tile_y, offset_x_in_tile, offset_y_in_tile,
			w, h, layer, tile_width, tile_height,
			last_tile_width, last_tile_height,
			tiles_across, tiles_down,
			osr->ops->read_tile, osr,
			dest, osr->cache);
}


const char * const *openslide_get_property_names(openslide_t *osr) {
  return osr->property_names;
}

const char *openslide_get_property_value(openslide_t *osr, const char *name) {
  return g_hash_table_lookup(osr->properties, name);
}

const char * const *openslide_get_associated_image_names(openslide_t *osr) {
  return osr->associated_image_names;
}

void openslide_get_associated_image_dimensions(openslide_t *osr, const char *name,
					       int64_t *w, int64_t *h) {
  struct _openslide_associated_image *img = g_hash_table_lookup(osr->associated_images,
								name);
  if (img) {
    *w = img->w;
    *h = img->h;
  } else {
    *w = 0;
    *h = 0;
  }
}

void openslide_read_associated_image(openslide_t *osr,
				     const char *name,
				     uint32_t *dest) {
  struct _openslide_associated_image *img = g_hash_table_lookup(osr->associated_images,
								name);
  if (img && dest) {
    memcpy(dest, img->argb_data, img->w * img->h * 4);
  }
}
