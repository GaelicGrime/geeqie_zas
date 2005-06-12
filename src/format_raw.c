/*
 *  GQView
 *  (C) 2005 John Ellis
 *
 *  Authors:
 *    Original version 2005 Lars Ellenberg, base on dcraw by David coffin.
 *
 * This software is released under the GNU General Public License (GNU GPL).
 * Please read the included file COPYING for more information.
 * This software comes with no warranty of any kind, use at your own risk!
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif


#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <glib.h>

#include "intl.h"

#include "format_raw.h"

#include "format_canon.h"
#include "format_fuji.h"
#include "format_nikon.h"
#include "format_olympus.h"


/* so that debugging is honored */
extern gint debug;


typedef struct _FormatRawEntry FormatRawEntry;
struct _FormatRawEntry {
	const gchar *extension;
	FormatRawMatchType magic_type;
	const guint magic_offset;
	const void *magic_pattern;
	const guint magic_length;
	const gchar *description;
	FormatRawParseFunc func_parse;
};

static FormatRawEntry format_raw_list[] = {
	FORMAT_RAW_CANON,
	FORMAT_RAW_FUJI,
	FORMAT_RAW_NIKON,
	{ NULL, 0, 0, NULL, 0, NULL, NULL }
};


typedef struct _FormatExifEntry FormatExifEntry;
struct _FormatExifEntry {
	FormatExifMatchType header_type;
	const void *header_pattern;
	const guint header_length;
	const gchar *description;
	FormatExifParseFunc func_parse;
};

static FormatExifEntry format_exif_list[] = {
	FORMAT_EXIF_CANON,
	FORMAT_EXIF_FUJI,
	FORMAT_EXIF_NIKON,
	FORMAT_EXIF_OLYMPUS,
	{ 0, NULL, 0, NULL }
};


static guint tiff_table(unsigned char *data, const guint len, guint offset, ExifByteOrder bo,
			guint tag, ExifFormatType type,
			guint *result_offset, guint *result_count)
{
	guint count;
	guint i;

	if (len < offset + 2) return 0;
	if (type < 0 || type > EXIF_FORMAT_COUNT) return 0;

	count = exif_byte_get_int16(data + offset, bo);
	offset += 2;
	if (len < offset + count * 12 + 4) return 0;

	for (i = 0; i < count; i++)
		{
		guint segment;

		segment = offset + i * 12;
		if (exif_byte_get_int16(data + segment, bo) == tag &&
		    exif_byte_get_int16(data + segment + 2, bo) == type)
			{
			guint chunk_count;
			guint chunk_offset;
			guint chunk_length;

			chunk_count = exif_byte_get_int32(data + segment + 4, bo);
			chunk_length = ExifFormatList[type].size * chunk_count;

			if (chunk_length > 4)
				{
				chunk_offset = exif_byte_get_int32(data + segment + 8, bo);
				}
			else
				{
				chunk_offset = segment + 8;
				}

			if (chunk_offset + chunk_length <= len)
				{
				*result_offset = chunk_offset;
				*result_count = chunk_count;
				}

			return 0;
			}
		}

	return exif_byte_get_int32(data + offset + count * 12, bo);
}

static gint format_tiff_find_tag_data(unsigned char *data, const guint len,
				      guint tag, ExifFormatType type,
				      guint *result_offset, guint *result_count)
{
	ExifByteOrder bo;
	guint offset;

	if (len < 8) return FALSE;

	if (memcmp(data, "II", 2) == 0)
		{
		bo = EXIF_BYTE_ORDER_INTEL;
		}
	else if (memcmp(data, "MM", 2) == 0)
		{
		bo = EXIF_BYTE_ORDER_MOTOROLA;
		}
	else
		{
		return FALSE;
		}

	if (exif_byte_get_int16(data + 2, bo) != 0x002A)
		{
		return FALSE;
		}

	offset = exif_byte_get_int32(data + 4, bo);

	while (offset != 0)
		{
		guint ro = 0;
		guint rc = 0;

		offset = tiff_table(data, len, offset, bo, tag, type, &ro, &rc);
		if (ro != 0)
			{
			*result_offset = ro;
			*result_count = rc;
			return TRUE;
			}
		}

	return FALSE;
}

static FormatRawEntry *format_raw_find(unsigned char *data, const guint len)
{
	gint n;
	gint tiff;
	guint make_count = 0;
	guint make_offset = 0;

	tiff = (len > 8 &&
		(memcmp(data, "II\x2a\x00", 4) == 0 ||
		 memcmp(data, "MM\x00\x2a", 4) == 0));

	n = 0;
	while (format_raw_list[n].magic_pattern)
		{
		FormatRawEntry *entry = &format_raw_list[n];

		switch (entry->magic_type)
			{
			case FORMAT_RAW_MATCH_MAGIC:
				if (entry->magic_length + entry->magic_offset <= len &&
				    memcmp(data + entry->magic_offset,
					   entry->magic_pattern, entry->magic_length) == 0)
					{
					return entry;
					}
				break;
			case FORMAT_RAW_MATCH_TIFF_MAKE:
				if (tiff &&
				    make_offset == 0 &&
				    !format_tiff_find_tag_data(data, len, 0x10f, EXIF_FORMAT_STRING,
							       &make_offset, &make_count))
					{
					tiff = FALSE;
					}
				if (make_offset != 0 &&
				    make_count >= entry->magic_offset + entry->magic_length &&
				    memcmp(entry->magic_pattern,
					   data + make_offset + entry->magic_offset, entry->magic_length) == 0)
					{
					return entry;
					}
				break;
			default:
				break;
			}
		n++;
		}

	return NULL;
}

static gint format_raw_parse(FormatRawEntry *entry,
			     unsigned char *data, const guint len,
			     guint *image_offset, guint *exif_offset)
{
	guint io = 0;
	guint eo = 0;
	gint found;

	if (!entry || !entry->func_parse) return FALSE;

	if (debug) printf("RAW using file parser for %s\n", entry->description);

	found = entry->func_parse(data, len, &io, &eo);

	if (!found ||
	    io >= len - 4 ||
	    eo >= len)
		{
		return FALSE;
		}

	if (image_offset) *image_offset = io;
	if (exif_offset) *exif_offset = eo;

	return TRUE;
}

gint format_raw_img_exif_offsets(unsigned char *data, const guint len,
				 guint *image_offset, guint *exif_offset)
{
	FormatRawEntry *entry;

	if (!data || len < 1) return FALSE;

	entry = format_raw_find(data, len);

	if (!entry || !entry->func_parse) return FALSE;

	return format_raw_parse(entry, data, len, image_offset, exif_offset);
}


gint format_raw_img_exif_offsets_fd(int fd, const gchar *path,
				    unsigned char *header_data, const guint header_len,
				    guint *image_offset, guint *exif_offset)
{
	FormatRawEntry *entry;
	void *map_data = NULL;
	size_t map_len = 0;
	struct stat st;
	gint success;

	if (!header_data || fd < 0) return FALSE;

	/* given image pathname, first do simple (and fast) file extension test */
	if (path)
		{
		const gchar *ext;
		gint match = FALSE;
		gint i;

		ext = strrchr(path, '.');
		if (!ext) return FALSE;
		ext++;

		i = 0;
		while (!match && format_raw_list[i].magic_pattern)
			{
			if (format_raw_list[i].extension &&
			    strcasecmp(format_raw_list[i].extension, ext) == 0)
				{
				match = TRUE;
				}
			i++;
			}

		if (!match) return FALSE;

		if (debug) printf("RAW file parser extension match\n");
		}

	/* FIXME:
	 * when the target is a tiff file it should be mmaped prior to format_raw_find as
	 * the make field data may not always be within header_data + header_len
	 */ 
	entry = format_raw_find(header_data, header_len);

	if (!entry || !entry->func_parse) return FALSE;

	if (fstat(fd, &st) == -1)
		{
		printf("Failed to stat file %d\n", fd);
		return FALSE;
		}
	map_len = st.st_size;
	map_data = mmap(0, map_len, PROT_READ, MAP_PRIVATE, fd, 0);
	if (map_data == MAP_FAILED)
		{
		printf("Failed to mmap file %d\n", fd);
		return FALSE;
		}

	success = format_raw_parse(entry, map_data, map_len, image_offset, exif_offset);

	if (munmap(map_data, map_len) == -1)
		{
		printf("Failed to unmap file %d\n", fd);
		}

	if (success && image_offset)
		{
		if (lseek(fd, *image_offset, SEEK_SET) != *image_offset)
			{
			printf("Failed to seek to embedded image\n");

			*image_offset = 0;
			if (*exif_offset) *exif_offset = 0;
			success = FALSE;
			}
		}

	return success;
}


static FormatExifEntry *format_exif_makernote_find(ExifData *exif, unsigned char *tiff,
						   guint offset, guint size)
{
	ExifItem *make;
	gint n;

	make = exif_get_item(exif, "Make");

	n = 0;
	while (format_exif_list[n].header_pattern)
		{
		switch (format_exif_list[n].header_type)
			{
			case FORMAT_EXIF_MATCH_MAKERNOTE:
				if (format_exif_list[n].header_length + offset < size &&
				    memcmp(tiff + offset, format_exif_list[n].header_pattern,
							  format_exif_list[n].header_length) == 0)
					{
					return &format_exif_list[n];
					}
				break;
			case FORMAT_EXIF_MATCH_MAKE:
				if (make &&
				    make->data_len >= format_exif_list[n].header_length &&
				    memcmp(make->data, format_exif_list[n].header_pattern,
						       format_exif_list[n].header_length) == 0)
					{
					return &format_exif_list[n];
					}
				break;
			}
		n++;
		}

	return FALSE;
}

gint format_exif_makernote_parse(ExifData *exif, unsigned char *tiff, guint offset,
				 guint size, ExifByteOrder bo)
{
	FormatExifEntry *entry;

	entry = format_exif_makernote_find(exif, tiff, offset, size);

	if (!entry || !entry->func_parse) return FALSE;

	if (debug) printf("EXIF using makernote parser for %s\n", entry->description);

	return entry->func_parse(exif, tiff, offset, size, bo);
}

