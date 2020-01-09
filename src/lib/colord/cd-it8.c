/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2012-2014 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU Lesser General Public License Version 2.1
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

/**
 * SECTION:cd-it8
 * @short_description: Read and write IT8 color sample exchange files
 *
 * This object represents .ti1 and .ti3 files which can contain raw
 * or normalized sample data.
 */

#include "config.h"

#include <glib.h>
#include <lcms2.h>
#include <stdlib.h>
#include <string.h>

#include "cd-it8.h"
#include "cd-cleanup.h"
#include "cd-color.h"
#include "cd-context-lcms.h"

static void	cd_it8_class_init	(CdIt8Class	*klass);
static void	cd_it8_init		(CdIt8		*it8);
static void	cd_it8_finalize		(GObject	*object);

#define CD_IT8_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), CD_TYPE_IT8, CdIt8Private))

/**
 * CdIt8Private:
 *
 * Private #CdIt8 data
 **/
struct _CdIt8Private
{
	CdIt8Kind		 kind;
	cmsContext		 context_lcms;
	CdMat3x3		 matrix;
	gboolean		 normalized;
	gboolean		 spectral;
	gboolean		 enable_created;
	gchar			*instrument;
	gchar			*reference;
	gchar			*originator;
	gchar			*title;
	GPtrArray		*array_spectra;
	GPtrArray		*array_rgb;
	GPtrArray		*array_xyz;
	GPtrArray		*options;
};

enum {
	PROP_0,
	PROP_KIND,
	PROP_INSTRUMENT,
	PROP_REFERENCE,
	PROP_NORMALIZED,
	PROP_ORIGINATOR,
	PROP_TITLE,
	PROP_SPECTRAL,
	PROP_LAST
};

G_DEFINE_TYPE (CdIt8, cd_it8, G_TYPE_OBJECT)

/**
 * cd_it8_error_quark:
 *
 * Return value: An error quark.
 *
 * Since: 0.1.0
 **/
GQuark
cd_it8_error_quark (void)
{
	static GQuark quark = 0;
	if (!quark) {
		quark = g_quark_from_static_string ("cd_it8_error");
	}
	return quark;
}

/**
 * _cmsIT8GetPropertyDbl:
 *
 * This gets a property ensuring the decimal point is '.' rather than what is
 * specified in LC_NUMERIC
 **/
static gdouble
_cmsIT8GetPropertyDbl (cmsHANDLE it8_lcms, const gchar *key)
{
	const gchar *value;
	value = cmsIT8GetProperty (it8_lcms, key);
	if (value == NULL)
		return -1;
	return g_ascii_strtod (value, NULL);
}

/**
 * _cmsIT8GetPropertyInt:
 **/
static guint
_cmsIT8GetPropertyInt (cmsHANDLE it8_lcms, const gchar *key)
{
	const gchar *value;
	guint64 tmp;

	value = cmsIT8GetProperty (it8_lcms, key);
	if (value == NULL)
		return 0;
	tmp = g_ascii_strtoull (value, NULL, 10);
	if (tmp > G_MAXUINT)
		return 0;

	return tmp;
}

/**
 * _cmsIT8GetDataRowColDbl:
 *
 * This gets a data value ensuring the decimal point is '.' rather than what is
 * specified in LC_NUMERIC
 **/
static gdouble
_cmsIT8GetDataRowColDbl (cmsHANDLE it8_lcms, gint row, gint col)
{
	const char *value;
	value = cmsIT8GetDataRowCol (it8_lcms, row, col);
	if (value == NULL)
		return -1;
	return g_ascii_strtod (value, NULL);
}

/**
 * _cmsIT8WriteFloat:
 *
 * We can't use g_ascii_dtostr() as this produces very inefficient
 * strings for storage. Instead write the string with 13 decimal places
 * and then manually remove trailing zeros more than one as ArgyllCMS
 * doesn't support integers as floats (!)
 **/
static void
_cmsIT8WriteFloat (gchar *buffer, gsize buffer_size, gdouble value)
{
	guint i;
	memset (buffer, '\0', buffer_size);
	g_ascii_formatd (buffer, buffer_size, "%.12f", value);
	for (i = G_ASCII_DTOSTR_BUF_SIZE - 1; i > 2; i--) {
		if (buffer[i] == '\0')
			continue;
		if (buffer[i] != '0')
			break;
		if (buffer[i-1] == '.')
			break;
		buffer[i] = '\0';
	}
}

/**
 * _cmsIT8SetPropertyDbl:
 *
 * This sets a property ensuring the decimal point is '.' rather than what is
 * specified in LC_NUMERIC
 **/
static void
_cmsIT8SetPropertyDbl (cmsHANDLE it8_lcms, const gchar *key, gdouble value)
{
	gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];
	_cmsIT8WriteFloat (buffer, G_ASCII_DTOSTR_BUF_SIZE, value);
	cmsIT8SetPropertyUncooked (it8_lcms, key, buffer);
}

/**
 * _cmsIT8SetPropertyInt:
 **/
static void
_cmsIT8SetPropertyInt (cmsHANDLE it8_lcms, const gchar *key, gint value)
{
	gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];
	g_ascii_formatd (buffer, G_ASCII_DTOSTR_BUF_SIZE, "%.0f", value);
	cmsIT8SetPropertyUncooked (it8_lcms, key, buffer);
}

/**
 * _cmsIT8SetDataRowColDbl:
 *
 * This sets a data value ensuring the decimal point is '.' rather than what is
 * specified in LC_NUMERIC
 **/
static void
_cmsIT8SetDataRowColDbl (cmsHANDLE it8_lcms, gint row, gint col, gdouble value)
{
	gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];
	_cmsIT8WriteFloat (buffer, G_ASCII_DTOSTR_BUF_SIZE, value);
	cmsIT8SetDataRowCol (it8_lcms, row, col, buffer);
}

/**
 * cd_it8_set_matrix:
 * @it8: a #CdIt8 instance.
 * @matrix: a #CdMat3x3.
 *
 * Set the calibration matrix in the it8 file.
 *
 * Since: 0.1.20
 **/
void
cd_it8_set_matrix (CdIt8 *it8, const CdMat3x3 *matrix)
{
	g_return_if_fail (CD_IS_IT8 (it8));
	cd_mat33_copy (matrix, &it8->priv->matrix);
}

/**
 * cd_it8_get_matrix:
 * @it8: a #CdIt8 instance.
 *
 * Gets the calibration matrix in the it8 file.
 *
 * Return value: a #CdMat3x3.
 *
 * Since: 0.1.20
 **/
const CdMat3x3 *
cd_it8_get_matrix (CdIt8 *it8)
{
	g_return_val_if_fail (CD_IS_IT8 (it8), NULL);
	return &it8->priv->matrix;
}

/**
 * cd_it8_set_kind:
 * @it8: a #CdIt8 instance.
 * @kind: a #CdIt8Kind, e.g %CD_IT8_KIND_TI3.
 *
 * Set the kind of IT8 file.
 *
 * Since: 0.1.20
 **/
void
cd_it8_set_kind (CdIt8 *it8, CdIt8Kind kind)
{
	g_return_if_fail (CD_IS_IT8 (it8));
	it8->priv->kind = kind;
}

/**
 * cd_it8_get_kind:
 * @it8: a #CdIt8 instance.
 *
 * Gets the kind of IT8 file.
 *
 * Return value: a #CdIt8Kind, e.g %CD_IT8_KIND_TI3.
 *
 * Since: 0.1.20
 **/
CdIt8Kind
cd_it8_get_kind (CdIt8 *it8)
{
	g_return_val_if_fail (CD_IS_IT8 (it8), 0);
	return it8->priv->kind;
}

/**
 * cd_it8_parse_luminance:
 **/
static gboolean
cd_it8_parse_luminance (const gchar *text, CdColorXYZ *xyz, GError **error)
{
	_cleanup_strv_free_ gchar **split = NULL;

	split = g_strsplit (text, " ", -1);
	if (g_strv_length (split) != 3) {
		g_set_error (error,
			     CD_IT8_ERROR,
			     CD_IT8_ERROR_INVALID_FORMAT,
			     "LUMINANCE_XYZ_CDM2 format invalid: %s",
			     text);
		return FALSE;
	}

	xyz->X = g_ascii_strtod (split[0], NULL);
	xyz->Y = g_ascii_strtod (split[1], NULL);
	xyz->Z = g_ascii_strtod (split[2], NULL);
	return TRUE;
}

/**
 * cd_it8_get_originator:
 * @it8: a #CdIt8 instance.
 *
 * Gets the file orginator.
 *
 * Return value: The originator, or %NULL if unset
 *
 * Since: 0.1.20
 **/
const gchar *
cd_it8_get_originator (CdIt8 *it8)
{
	g_return_val_if_fail (CD_IS_IT8 (it8), NULL);
	return it8->priv->originator;
}

/**
 * cd_it8_get_title:
 * @it8: a #CdIt8 instance.
 *
 * Gets the file title.
 *
 * Return value: The title, or %NULL if unset
 *
 * Since: 0.1.20
 **/
const gchar *
cd_it8_get_title (CdIt8 *it8)
{
	g_return_val_if_fail (CD_IS_IT8 (it8), NULL);
	return it8->priv->title;
}

/**
 * cd_it8_get_instrument:
 * @it8: a #CdIt8 instance.
 *
 * Gets the instrument the file was created by.
 *
 * Return value: The instrument, or %NULL if unset
 *
 * Since: 0.1.20
 **/
const gchar *
cd_it8_get_instrument (CdIt8 *it8)
{
	g_return_val_if_fail (CD_IS_IT8 (it8), NULL);
	return it8->priv->instrument;
}

/**
 * cd_it8_get_reference:
 * @it8: a #CdIt8 instance.
 *
 * Gets the reference the file was created against.
 *
 * Return value: The reference, or %NULL if unset
 *
 * Since: 0.1.20
 **/
const gchar *
cd_it8_get_reference (CdIt8 *it8)
{
	g_return_val_if_fail (CD_IS_IT8 (it8), NULL);
	return it8->priv->reference;
}

/**
 * cd_it8_get_enable_created:
 * @it8: a #CdIt8 instance.
 *
 * Gets if the 'CREATED' attribute will be written. This is typically only
 * set in the self test programs.
 *
 * Return value: The reference, or %NULL if unset
 *
 * Since: 0.1.33
 **/
gboolean
cd_it8_get_enable_created (CdIt8 *it8)
{
	g_return_val_if_fail (CD_IS_IT8 (it8), FALSE);
	return it8->priv->enable_created;
}

/**
 * cd_it8_get_normalized:
 * @it8: a #CdIt8 instance.
 *
 * Gets if the data should be written normlaised to y=100.
 *
 * Return value: %TRUE if the data should be normalised.
 *
 * Since: 0.1.20
 **/
gboolean
cd_it8_get_normalized (CdIt8 *it8)
{
	g_return_val_if_fail (CD_IS_IT8 (it8), FALSE);
	return it8->priv->normalized;
}

/**
 * cd_it8_get_spectral:
 * @it8: a #CdIt8 instance.
 *
 * Gets if the data is spectral or XYZ.
 *
 * Return value: %TRUE if the data is in spectral bands.
 *
 * Since: 0.1.20
 **/
gboolean
cd_it8_get_spectral (CdIt8 *it8)
{
	g_return_val_if_fail (CD_IS_IT8 (it8), FALSE);
	return it8->priv->spectral;
}

/**
 * cd_it8_load_ti1_cal:
 **/
static gboolean
cd_it8_load_ti1_cal (CdIt8 *it8, cmsHANDLE it8_lcms, GError **error)
{
	CdColorRGB *rgb;
	CdColorXYZ *xyz;
	const gchar *tmp;
	guint i;
	guint number_of_sets = 0;

	tmp = cmsIT8GetProperty (it8_lcms, "COLOR_REP");
	if (g_strcmp0 (tmp, "RGB") != 0) {
		g_set_error (error,
			     CD_IT8_ERROR,
			     CD_IT8_ERROR_INVALID_FORMAT,
			     "Invalid data format: %s", tmp);
		return FALSE;
	}

	/* copy out data entries */
	number_of_sets = _cmsIT8GetPropertyInt (it8_lcms, "NUMBER_OF_SETS");
	if (number_of_sets == 0) {
		g_set_error_literal (error,
				     CD_IT8_ERROR,
				     CD_IT8_ERROR_INVALID_FORMAT,
				     "Invalid format, NUMBER_OF_SETS required");
		return FALSE;
	}

	for (i = 0; i < number_of_sets; i++) {
		rgb = cd_color_rgb_new ();
		rgb->R = _cmsIT8GetDataRowColDbl (it8_lcms, i, 1);
		rgb->G = _cmsIT8GetDataRowColDbl (it8_lcms, i, 2);
		rgb->B = _cmsIT8GetDataRowColDbl (it8_lcms, i, 3);

		/* ti1 files don't have NORMALIZED_TO_Y_100 so guess on
		 * the asumption the first patch isn't black */
		if (rgb->R > 1.0 || rgb->G > 1.0 || rgb->B > 1.0)
			it8->priv->normalized = TRUE;
		if (it8->priv->normalized) {
			rgb->R /= 100.0f;
			rgb->G /= 100.0f;
			rgb->B /= 100.0f;
		}
		g_ptr_array_add (it8->priv->array_rgb, rgb);
		xyz = cd_color_xyz_new ();
		cd_color_xyz_set (xyz, 0.0, 0.0, 0.0);
		g_ptr_array_add (it8->priv->array_xyz, xyz);
	}
	return TRUE;
}

/**
 * cd_it8_load_ti3:
 **/
static gboolean
cd_it8_load_ti3 (CdIt8 *it8, cmsHANDLE it8_lcms, GError **error)
{
	CdColorRGB *rgb;
	CdColorXYZ luminance;
	CdColorXYZ *xyz;
	const gchar *tmp;
	gboolean scaled_to_y100 = FALSE;
	guint i;
	guint number_of_sets = 0;

	tmp = cmsIT8GetProperty (it8_lcms, "COLOR_REP");
	if (g_strcmp0 (tmp, "RGB_XYZ") != 0) {
		g_set_error (error,
			     CD_IT8_ERROR,
			     CD_IT8_ERROR_INVALID_FORMAT,
			     "Invalid data format: %s", tmp);
		return FALSE;
	}

	/* if normalized, then scale back up */
	tmp = cmsIT8GetProperty (it8_lcms, "NORMALIZED_TO_Y_100");
	if (g_strcmp0 (tmp, "YES") == 0) {
		scaled_to_y100 = TRUE;
		tmp = cmsIT8GetProperty (it8_lcms, "LUMINANCE_XYZ_CDM2");
		if (!cd_it8_parse_luminance (tmp, &luminance, error))
			return FALSE;
	} else {
		cd_color_xyz_set (&luminance, 1.f, 1.f, 1.f);
	}

	/* set spectral flag */
	tmp = cmsIT8GetProperty (it8_lcms, "INSTRUMENT_TYPE_SPECTRAL");
	cd_it8_set_spectral (it8, g_strcmp0 (tmp, "YES") == 0);

	/* set instrument */
	cd_it8_set_instrument (it8, cmsIT8GetProperty (it8_lcms, "TARGET_INSTRUMENT"));

	/* copy out data entries */
	number_of_sets = _cmsIT8GetPropertyInt (it8_lcms, "NUMBER_OF_SETS");
	if (number_of_sets == 0) {
		g_set_error_literal (error,
				     CD_IT8_ERROR,
				     CD_IT8_ERROR_INVALID_FORMAT,
				     "Invalid format, NUMBER_OF_SETS required");
		return FALSE;
	}
	for (i = 0; i < number_of_sets; i++) {
		rgb = cd_color_rgb_new ();
		rgb->R = _cmsIT8GetDataRowColDbl (it8_lcms, i, 1);
		rgb->G = _cmsIT8GetDataRowColDbl (it8_lcms, i, 2);
		rgb->B = _cmsIT8GetDataRowColDbl (it8_lcms, i, 3);
		if (scaled_to_y100) {
			rgb->R /= 100.0f;
			rgb->G /= 100.0f;
			rgb->B /= 100.0f;
		}
		g_ptr_array_add (it8->priv->array_rgb, rgb);
		xyz = cd_color_xyz_new ();
		xyz->X = _cmsIT8GetDataRowColDbl (it8_lcms, i, 4);
		xyz->Y = _cmsIT8GetDataRowColDbl (it8_lcms, i, 5);
		xyz->Z = _cmsIT8GetDataRowColDbl (it8_lcms, i, 6);
		if (scaled_to_y100) {
			xyz->X /= 100.0f;
			xyz->Y /= 100.0f;
			xyz->Z /= 100.0f;
			xyz->X *= luminance.X;
			xyz->Y *= luminance.Y;
			xyz->Z *= luminance.Z;
		}
		g_ptr_array_add (it8->priv->array_xyz, xyz);
	}
	return TRUE;
}

/**
 * cd_it8_load_ccmx:
 **/
static gboolean
cd_it8_load_ccmx (CdIt8 *it8, cmsHANDLE it8_lcms, GError **error)
{
	const gchar *tmp;

	/* check color format */
	tmp = cmsIT8GetProperty (it8_lcms, "COLOR_REP");
	if (g_strcmp0 (tmp, "XYZ") != 0) {
		g_set_error (error,
			     CD_IT8_ERROR,
			     CD_IT8_ERROR_INVALID_FORMAT,
			     "Invalid CCMX data format: %s", tmp);
		return FALSE;
	}

	/* set instrument */
	cd_it8_set_instrument (it8, cmsIT8GetProperty (it8_lcms, "INSTRUMENT"));

	/* just load the matrix */
	it8->priv->matrix.m00 = _cmsIT8GetDataRowColDbl (it8_lcms, 0, 0);
	it8->priv->matrix.m01 = _cmsIT8GetDataRowColDbl (it8_lcms, 0, 1);
	it8->priv->matrix.m02 = _cmsIT8GetDataRowColDbl (it8_lcms, 0, 2);
	it8->priv->matrix.m10 = _cmsIT8GetDataRowColDbl (it8_lcms, 1, 0);
	it8->priv->matrix.m11 = _cmsIT8GetDataRowColDbl (it8_lcms, 1, 1);
	it8->priv->matrix.m12 = _cmsIT8GetDataRowColDbl (it8_lcms, 1, 2);
	it8->priv->matrix.m20 = _cmsIT8GetDataRowColDbl (it8_lcms, 2, 0);
	it8->priv->matrix.m21 = _cmsIT8GetDataRowColDbl (it8_lcms, 2, 1);
	it8->priv->matrix.m22 = _cmsIT8GetDataRowColDbl (it8_lcms, 2, 2);
	return TRUE;
}

/**
 * cd_it8_load_ccss_spect:
 **/
static gboolean
cd_it8_load_ccss_spect (CdIt8 *it8, cmsHANDLE it8_lcms, GError **error)
{
	CdSpectrum *spectrum;
	const gchar *tmp;
	gboolean has_index;
	gdouble spectral_end;
	gdouble spectral_norm;
	gdouble spectral_start;
	guint i;
	guint j;
	guint number_of_fields;
	guint number_of_sets;
	guint spectral_bands;

	/* get spectra endpoints */
	tmp = cmsIT8GetProperty (it8_lcms, "SPECTRAL_START_NM");
	if (tmp == NULL) {
		g_set_error_literal (error,
				     CD_IT8_ERROR,
				     CD_IT8_ERROR_INVALID_FORMAT,
				     "Invalid format, SPECTRAL_START_NM required");
		return FALSE;
	}
	spectral_start = _cmsIT8GetPropertyDbl (it8_lcms, "SPECTRAL_START_NM");
	spectral_end = _cmsIT8GetPropertyDbl (it8_lcms, "SPECTRAL_END_NM");
	if (spectral_end == 0) {
		g_set_error_literal (error,
				     CD_IT8_ERROR,
				     CD_IT8_ERROR_INVALID_FORMAT,
				     "Invalid format, SPECTRAL_END_NM required");
		return FALSE;
	}

	/* get number of bands */
	spectral_bands = _cmsIT8GetPropertyInt (it8_lcms, "SPECTRAL_BANDS");
	if (spectral_bands == 0) {
		g_set_error_literal (error,
				     CD_IT8_ERROR,
				     CD_IT8_ERROR_INVALID_FORMAT,
				     "Invalid format: SPECTRAL_BANDS required");
		return FALSE;
	}
	if (spectral_bands < 2 || spectral_bands > 16384) {
		g_set_error (error,
			     CD_IT8_ERROR,
			     CD_IT8_ERROR_INVALID_FORMAT,
			     "Invalid CCSS spectral size: %i", spectral_bands);
		return FALSE;
	}

	/* get spectral norm */
	spectral_norm = _cmsIT8GetPropertyDbl (it8_lcms, "SPECTRAL_NORM");
	if (spectral_norm < 0.f)
		spectral_norm = 1.f;

	/* ArgyllCMS seems to support an index in the CCSS file, and not in the
	 * SPECT or CMF but like any good library support each mode */
	number_of_fields = _cmsIT8GetPropertyInt (it8_lcms, "NUMBER_OF_FIELDS");
	if (number_of_fields == 0) {
		g_set_error_literal (error,
				     CD_IT8_ERROR,
				     CD_IT8_ERROR_INVALID_FORMAT,
				     "Invalid format, NUMBER_OF_FIELDS required");
		return FALSE;
	}
	if (spectral_bands == number_of_fields) {
		has_index = FALSE;
	} else if (spectral_bands + 1 == number_of_fields) {
		has_index = TRUE;
	} else {
		g_set_error (error,
			     CD_IT8_ERROR,
			     CD_IT8_ERROR_INVALID_FORMAT,
			     "Invalid CCSS: bands = %i, fields = %i",
			     spectral_bands, number_of_fields);
		return FALSE;
	}

	/* read out the arrays of data */
	number_of_sets = _cmsIT8GetPropertyInt (it8_lcms, "NUMBER_OF_SETS");
	if (number_of_sets == 0) {
		g_set_error_literal (error,
				     CD_IT8_ERROR,
				     CD_IT8_ERROR_INVALID_FORMAT,
				     "Invalid format, NUMBER_OF_SETS required");
		return FALSE;
	}
	for (j = 0; j < (guint) number_of_sets; j++) {
		spectrum = cd_spectrum_sized_new (spectral_bands);
		if (has_index) {
			cd_spectrum_set_id (spectrum,
					    cmsIT8GetDataRowCol(it8_lcms, j, 0));
		} else {
			_cleanup_free_ gchar *label = NULL;
			label = g_strdup_printf ("%i", j + 1);
			cd_spectrum_set_id (spectrum, label);
		}
		for (i = has_index; i < number_of_fields; i++) {
			cd_spectrum_add_value (spectrum,
					      _cmsIT8GetDataRowColDbl (it8_lcms, j, i));
		}
		cd_spectrum_set_start (spectrum, spectral_start);
		cd_spectrum_set_end (spectrum, spectral_end);
		cd_spectrum_set_norm (spectrum, spectral_norm);
		cd_it8_add_spectrum (it8, spectrum);
		cd_spectrum_free (spectrum);
	}
	return TRUE;
}

/**
 * cd_it8_load_cmf:
 **/
static gboolean
cd_it8_load_cmf (CdIt8 *it8, cmsHANDLE it8_lcms, GError **error)
{
	CdSpectrum *spectrum;
	gdouble spectral_end;
	gdouble spectral_norm;
	gdouble spectral_start;
	guint i;
	guint j;
	guint number_of_fields;
	guint number_of_sets;
	guint spectral_bands;

	/* get spectra endpoints */
	spectral_start = _cmsIT8GetPropertyDbl (it8_lcms, "SPECTRAL_START_NM");
	if (spectral_start == 0) {
		g_set_error_literal (error,
				     CD_IT8_ERROR,
				     CD_IT8_ERROR_INVALID_FORMAT,
				     "Invalid format, SPECTRAL_START_NM required");
		return FALSE;
	}
	spectral_end = _cmsIT8GetPropertyDbl (it8_lcms, "SPECTRAL_END_NM");
	if (spectral_end == 0) {
		g_set_error_literal (error,
				     CD_IT8_ERROR,
				     CD_IT8_ERROR_INVALID_FORMAT,
				     "Invalid format, SPECTRAL_END_NM required");
		return FALSE;
	}

	/* get number of bands */
	spectral_bands = _cmsIT8GetPropertyInt (it8_lcms, "SPECTRAL_BANDS");
	if (spectral_bands == 0) {
		g_set_error_literal (error,
				     CD_IT8_ERROR,
				     CD_IT8_ERROR_INVALID_FORMAT,
				     "Invalid format, SPECTRAL_BANDS required");
		return FALSE;
	}
	if (spectral_bands < 2 || spectral_bands > 16384) {
		g_set_error (error,
			     CD_IT8_ERROR,
			     CD_IT8_ERROR_INVALID_FORMAT,
			     "Invalid CCSS spectral size: %i", spectral_bands);
		return FALSE;
	}

	/* get spectral norm */
	spectral_norm = _cmsIT8GetPropertyDbl (it8_lcms, "SPECTRAL_NORM");
	if (spectral_norm < 0.f)
		spectral_norm = 1.f;

	/* CMF files are un-indexed and implicitly XYZ */
	number_of_fields = _cmsIT8GetPropertyInt (it8_lcms, "NUMBER_OF_FIELDS");
	if (number_of_fields == 0) {
		g_set_error_literal (error,
				     CD_IT8_ERROR,
				     CD_IT8_ERROR_INVALID_FORMAT,
				     "Invalid format, NUMBER_OF_FIELDS required");
		return FALSE;
	}
	if (spectral_bands != number_of_fields) {
		g_set_error (error,
			     CD_IT8_ERROR,
			     CD_IT8_ERROR_INVALID_FORMAT,
			     "Invalid CMF: bands != fields (%i, %i)",
			     spectral_bands, number_of_fields);
		return FALSE;
	}

	/* read out the arrays of data */
	number_of_sets = _cmsIT8GetPropertyInt (it8_lcms, "NUMBER_OF_SETS");
	if (number_of_sets != 3) {
		g_set_error (error,
			     CD_IT8_ERROR,
			     CD_IT8_ERROR_INVALID_FORMAT,
			     "Invalid CMF: sets != 3 (%i)",
			     number_of_sets);
		return FALSE;
	}
	for (j = 0; j < (guint) number_of_sets; j++) {
		spectrum = cd_spectrum_sized_new (spectral_bands);
		if (j == 0)
			cd_spectrum_set_id (spectrum, "X");
		else if (j == 1)
			cd_spectrum_set_id (spectrum, "Y");
		else
			cd_spectrum_set_id (spectrum, "Z");
		for (i = 0; i < number_of_fields; i++) {
			cd_spectrum_add_value (spectrum,
					      _cmsIT8GetDataRowColDbl (it8_lcms, j, i));
		}
		cd_spectrum_set_start (spectrum, spectral_start);
		cd_spectrum_set_end (spectrum, spectral_end);
		cd_spectrum_set_norm (spectrum, spectral_norm);
		cd_it8_add_spectrum (it8, spectrum);
		cd_spectrum_free (spectrum);
	}
	return TRUE;
}

/**
 * cd_it8_has_option:
 * @it8: a #CdIt8 instance.
 * @option: a option, e.g. "TYPE_CRT"
 *
 * Finds an option in the file.
 *
 * Return value: %TRUE if the option is set
 *
 * Since: 0.1.20
 **/
gboolean
cd_it8_has_option (CdIt8 *it8, const gchar *option)
{
	const gchar *tmp;
	guint i;

	g_return_val_if_fail (CD_IS_IT8 (it8), FALSE);
	g_return_val_if_fail (option != NULL, FALSE);

	for (i = 0; i < it8->priv->options->len; i++) {
		tmp = g_ptr_array_index (it8->priv->options, i);
		if (g_strcmp0 (tmp, option) == 0)
			return TRUE;
	}
	return FALSE;
}

/**
 * cd_it8_load_from_data:
 * @it8: a #CdIt8 instance.
 * @data: text data
 * @size: the size of text data
 * @error: a #GError, or %NULL
 *
 * Loads a it8 file from data.
 *
 * Return value: %TRUE if a valid it8 file was read.
 *
 * Since: 0.1.20
 **/
gboolean
cd_it8_load_from_data (CdIt8 *it8,
		       const gchar *data,
		       gsize size,
		       GError **error)
{
	cmsHANDLE it8_lcms = NULL;
	const gchar *tmp;
	gboolean ret = TRUE;
	gchar **props = NULL;
	guint i;
	_cleanup_error_free_ GError *error_local = NULL;

	g_return_val_if_fail (CD_IS_IT8 (it8), FALSE);
	g_return_val_if_fail (data != NULL, FALSE);
	g_return_val_if_fail (size > 0, FALSE);

	/* clear old data */
	g_ptr_array_set_size (it8->priv->array_rgb, 0);
	g_ptr_array_set_size (it8->priv->array_xyz, 0);
	g_ptr_array_set_size (it8->priv->options, 0);
	cd_mat33_clear (&it8->priv->matrix);

	/* load the it8 data */
	it8_lcms = cmsIT8LoadFromMem (it8->priv->context_lcms, (void *) data, size);
	if (it8_lcms == NULL) {
		ret = cd_context_lcms_error_check (it8->priv->context_lcms, &error_local);
		if (!ret) {
			g_set_error_literal (error,
					     CD_IT8_ERROR,
					     CD_IT8_ERROR_FAILED,
					     error_local->message);
			goto out;
		}
		ret = FALSE;
		g_set_error_literal (error,
				     CD_IT8_ERROR,
				     CD_IT8_ERROR_FAILED,
				     "Failed to load but no error set");
		goto out;
	}

	/* add options */
	cmsIT8EnumProperties (it8_lcms, &props);
	for (i = 0; props[i] != NULL; i++) {
		if (g_str_has_prefix (props[i], "TYPE_"))
			cd_it8_add_option (it8, props[i]);
	}

	/* get sheet type */
	tmp = cmsIT8GetSheetType (it8_lcms);
	if (g_str_has_prefix (tmp, "CTI1")) {
		cd_it8_set_kind (it8, CD_IT8_KIND_TI1);
	} else if (g_str_has_prefix (tmp, "CTI3")) {
		cd_it8_set_kind (it8, CD_IT8_KIND_TI3);
	} else if (g_str_has_prefix (tmp, "CCMX")) {
		cd_it8_set_kind (it8, CD_IT8_KIND_CCMX);
	} else if (g_str_has_prefix (tmp, "CCSS")) {
		cd_it8_set_kind (it8, CD_IT8_KIND_CCSS);
	} else if (g_str_has_prefix (tmp, "CMF")) {
		cd_it8_set_kind (it8, CD_IT8_KIND_CMF);
	} else if (g_str_has_prefix (tmp, "SPECT")) {
		cd_it8_set_kind (it8, CD_IT8_KIND_SPECT);
	} else if (g_str_has_prefix (tmp, "CAL")) {
		cd_it8_set_kind (it8, CD_IT8_KIND_CAL);
	} else {
		ret = FALSE;
		g_set_error (error,
			     CD_IT8_ERROR,
			     CD_IT8_ERROR_UNKNOWN_KIND,
			     "Unknown sheet type: %s", tmp);
		goto out;
	}

	/* get ti1 and ti3 specific data */
	switch (it8->priv->kind) {
	case CD_IT8_KIND_TI1:
	case CD_IT8_KIND_CAL:
		ret = cd_it8_load_ti1_cal (it8, it8_lcms, error);
		if (!ret)
			goto out;
		break;
	case CD_IT8_KIND_TI3:
		ret = cd_it8_load_ti3 (it8, it8_lcms, error);
		if (!ret)
			goto out;
		break;
	case CD_IT8_KIND_CCMX:
		ret = cd_it8_load_ccmx (it8, it8_lcms, error);
		if (!ret)
			goto out;
		break;
	case CD_IT8_KIND_CCSS:
	case CD_IT8_KIND_SPECT:
		ret = cd_it8_load_ccss_spect (it8, it8_lcms, error);
		if (!ret)
			goto out;
		break;
	case CD_IT8_KIND_CMF:
		ret = cd_it8_load_cmf (it8, it8_lcms, error);
		if (!ret)
			goto out;
		break;
	default:
		break;
	}

	/* set common bits */
	cd_it8_set_title (it8, cmsIT8GetProperty (it8_lcms, "DISPLAY"));
	cd_it8_set_originator (it8, cmsIT8GetProperty (it8_lcms, "ORIGINATOR"));
	cd_it8_set_reference (it8, cmsIT8GetProperty (it8_lcms, "REFERENCE"));
out:
	if (it8_lcms != NULL)
		cmsIT8Free (it8_lcms);
	return ret;
}

/**
 * cd_it8_load_from_file:
 * @it8: a #CdIt8 instance.
 * @file: a #GFile
 * @error: a #GError, or %NULL
 *
 * Loads a it8 file from disk.
 *
 * Return value: %TRUE if a valid it8 file was read.
 *
 * Since: 0.1.20
 **/
gboolean
cd_it8_load_from_file (CdIt8 *it8, GFile *file, GError **error)
{
	gsize size = 0;
	_cleanup_free_ gchar *data = NULL;

	g_return_val_if_fail (CD_IS_IT8 (it8), FALSE);
	g_return_val_if_fail (G_IS_FILE (file), FALSE);

	/* load file */
	if (!g_file_load_contents (file, NULL, &data, &size, NULL, error))
		return FALSE;

	/* load data */
	if (!cd_it8_load_from_data (it8, data, size, error))
		return FALSE;
	return TRUE;
}

/**
 * cd_it8_color_match:
 **/
static gboolean
cd_it8_color_match (CdColorRGB *rgb, gdouble r, gdouble g, gdouble b)
{
	if (ABS (rgb->R - r) > 0.01f)
		return FALSE;
	if (ABS (rgb->G - g) > 0.01f)
		return FALSE;
	if (ABS (rgb->B - b) > 0.01f)
		return FALSE;
	return TRUE;
}

/**
 * cd_it8_convert_xyz_to_string:
 **/
static gchar *
cd_it8_convert_xyz_to_string (CdColorXYZ *src)
{
	gchar buffer[3][G_ASCII_DTOSTR_BUF_SIZE];
	g_ascii_dtostr (buffer[0], G_ASCII_DTOSTR_BUF_SIZE, src->X);
	g_ascii_dtostr (buffer[1], G_ASCII_DTOSTR_BUF_SIZE, src->Y);
	g_ascii_dtostr (buffer[2], G_ASCII_DTOSTR_BUF_SIZE, src->Z);
	return g_strdup_printf ("%s %s %s", buffer[0], buffer[1], buffer[2]);
}

/**
 * cd_it8_save_to_file_ti1_ti3:
 **/
static gboolean
cd_it8_save_to_file_ti1_ti3 (CdIt8 *it8, cmsHANDLE it8_lcms, GError **error)
{
	CdColorRGB *rgb_tmp;
	CdColorXYZ lumi_xyz;
	CdColorXYZ *xyz_tmp;
	gboolean is_white;
	gdouble normalize = 0.0f;
	guint i;
	guint luminance_samples = 0;
	_cleanup_free_ gchar *lumi_str = NULL;

	/* calculate the absolute XYZ in candelas per meter squared */
	cd_color_xyz_clear (&lumi_xyz);
	if (it8->priv->normalized) {
		for (i = 0; i < it8->priv->array_rgb->len; i++) {
			rgb_tmp = g_ptr_array_index (it8->priv->array_rgb, i);

			/* is this 100% white? */
			is_white = cd_it8_color_match (rgb_tmp, 1.0f, 1.0f, 1.0f);
			if (!is_white)
				continue;
			luminance_samples++;
			xyz_tmp = g_ptr_array_index (it8->priv->array_xyz, i);
			lumi_xyz.X += xyz_tmp->X;
			lumi_xyz.Y += xyz_tmp->Y;
			lumi_xyz.Z += xyz_tmp->Z;
			if (xyz_tmp->Y > normalize)
				normalize = xyz_tmp->Y;
		}
		if (luminance_samples == 0) {
			g_set_error_literal (error,
					     CD_IT8_ERROR,
					     CD_IT8_ERROR_FAILED,
					     "Failed to find any white samples");
			return FALSE;
		}
		lumi_xyz.X /= luminance_samples;
		lumi_xyz.Y /= luminance_samples;
		lumi_xyz.Z /= luminance_samples;

		/* scale all the readings to 100 */
		normalize = 100.0f / normalize;
	}
	lumi_str = cd_it8_convert_xyz_to_string (&lumi_xyz);

	/* write data */
	if (it8->priv->kind == CD_IT8_KIND_TI1) {
		cmsIT8SetSheetType (it8_lcms, "CTI1   ");
		cmsIT8SetPropertyStr (it8_lcms, "DESCRIPTOR",
				      "Calibration Target chart information 1");
	} else if (it8->priv->kind == CD_IT8_KIND_TI3) {
		cmsIT8SetSheetType (it8_lcms, "CTI3   ");
		cmsIT8SetPropertyStr (it8_lcms, "DESCRIPTOR",
				      "Calibration Target chart information 3");
	}
	if (it8->priv->kind == CD_IT8_KIND_TI3) {
		cmsIT8SetPropertyStr (it8_lcms, "DEVICE_CLASS",
				      "DISPLAY");
	}
	cmsIT8SetPropertyStr (it8_lcms, "COLOR_REP", "RGB_XYZ");
	if (it8->priv->instrument != NULL) {
		cmsIT8SetPropertyStr (it8_lcms, "TARGET_INSTRUMENT",
				      it8->priv->instrument);
	}
	cmsIT8SetPropertyStr (it8_lcms, "INSTRUMENT_TYPE_SPECTRAL",
			      it8->priv->spectral ? "YES" : "NO");
	if (it8->priv->normalized) {
		cmsIT8SetPropertyStr (it8_lcms, "NORMALIZED_TO_Y_100", "YES");
		cmsIT8SetPropertyStr (it8_lcms, "LUMINANCE_XYZ_CDM2", lumi_str);
	} else {
		cmsIT8SetPropertyStr (it8_lcms, "NORMALIZED_TO_Y_100", "NO");
	}
	_cmsIT8SetPropertyInt (it8_lcms, "NUMBER_OF_FIELDS", 7);
	_cmsIT8SetPropertyInt (it8_lcms, "NUMBER_OF_SETS", it8->priv->array_rgb->len);
	cmsIT8SetDataFormat (it8_lcms, 0, "SAMPLE_ID");
	cmsIT8SetDataFormat (it8_lcms, 1, "RGB_R");
	cmsIT8SetDataFormat (it8_lcms, 2, "RGB_G");
	cmsIT8SetDataFormat (it8_lcms, 3, "RGB_B");
	cmsIT8SetDataFormat (it8_lcms, 4, "XYZ_X");
	cmsIT8SetDataFormat (it8_lcms, 5, "XYZ_Y");
	cmsIT8SetDataFormat (it8_lcms, 6, "XYZ_Z");

	/* write to the it8 file */
	for (i = 0; i < it8->priv->array_rgb->len; i++) {
		rgb_tmp = g_ptr_array_index (it8->priv->array_rgb, i);
		xyz_tmp = g_ptr_array_index (it8->priv->array_xyz, i);

		_cmsIT8SetDataRowColDbl(it8_lcms, i, 0, i + 1);
		if (it8->priv->normalized) {
			_cmsIT8SetDataRowColDbl(it8_lcms, i, 1, rgb_tmp->R * 100.0f);
			_cmsIT8SetDataRowColDbl(it8_lcms, i, 2, rgb_tmp->G * 100.0f);
			_cmsIT8SetDataRowColDbl(it8_lcms, i, 3, rgb_tmp->B * 100.0f);
			_cmsIT8SetDataRowColDbl(it8_lcms, i, 4, xyz_tmp->X * normalize);
			_cmsIT8SetDataRowColDbl(it8_lcms, i, 5, xyz_tmp->Y * normalize);
			_cmsIT8SetDataRowColDbl(it8_lcms, i, 6, xyz_tmp->Z * normalize);
		} else {
			_cmsIT8SetDataRowColDbl(it8_lcms, i, 1, rgb_tmp->R);
			_cmsIT8SetDataRowColDbl(it8_lcms, i, 2, rgb_tmp->G);
			_cmsIT8SetDataRowColDbl(it8_lcms, i, 3, rgb_tmp->B);
			_cmsIT8SetDataRowColDbl(it8_lcms, i, 4, xyz_tmp->X);
			_cmsIT8SetDataRowColDbl(it8_lcms, i, 5, xyz_tmp->Y);
			_cmsIT8SetDataRowColDbl(it8_lcms, i, 6, xyz_tmp->Z);
		}
	}
	return TRUE;
}

/**
 * cd_it8_save_to_file_cal:
 **/
static gboolean
cd_it8_save_to_file_cal (CdIt8 *it8, cmsHANDLE it8_lcms, GError **error)
{
	CdColorRGB *rgb_tmp;
	gboolean ret = TRUE;
	guint i;

	/* write data */
	cmsIT8SetSheetType (it8_lcms, "CAL    ");
	cmsIT8SetPropertyStr (it8_lcms, "DESCRIPTOR",
			      "Device Calibration Curves");
	cmsIT8SetPropertyStr (it8_lcms, "DEVICE_CLASS", "DISPLAY");
	cmsIT8SetPropertyStr (it8_lcms, "COLOR_REP", "RGB");
	if (it8->priv->instrument != NULL) {
		cmsIT8SetPropertyStr (it8_lcms, "TARGET_INSTRUMENT",
				      it8->priv->instrument);
	}
	_cmsIT8SetPropertyInt (it8_lcms, "NUMBER_OF_FIELDS", 4);
	_cmsIT8SetPropertyInt (it8_lcms, "NUMBER_OF_SETS", it8->priv->array_rgb->len);
	cmsIT8SetDataFormat (it8_lcms, 0, "RGB_I");
	cmsIT8SetDataFormat (it8_lcms, 1, "RGB_R");
	cmsIT8SetDataFormat (it8_lcms, 2, "RGB_G");
	cmsIT8SetDataFormat (it8_lcms, 3, "RGB_B");

	/* write to the it8 file */
	for (i = 0; i < it8->priv->array_rgb->len; i++) {
		rgb_tmp = g_ptr_array_index (it8->priv->array_rgb, i);
		_cmsIT8SetDataRowColDbl(it8_lcms, i, 0, 1.0f / (gdouble) (it8->priv->array_rgb->len - 1) * (gdouble) i);
		_cmsIT8SetDataRowColDbl(it8_lcms, i, 1, rgb_tmp->R);
		_cmsIT8SetDataRowColDbl(it8_lcms, i, 2, rgb_tmp->G);
		_cmsIT8SetDataRowColDbl(it8_lcms, i, 3, rgb_tmp->B);
	}

	return ret;
}

/**
 * cd_it8_save_to_file_ccmx:
 **/
static gboolean
cd_it8_save_to_file_ccmx (CdIt8 *it8, cmsHANDLE it8_lcms, GError **error)
{
	gboolean ret = TRUE;

	cmsIT8SetSheetType (it8_lcms, "CCMX   ");
	cmsIT8SetPropertyStr (it8_lcms, "DESCRIPTOR",
			      "Device Correction Matrix");

	cmsIT8SetPropertyStr (it8_lcms, "COLOR_REP", "XYZ");
	_cmsIT8SetPropertyInt (it8_lcms, "NUMBER_OF_FIELDS", 3);
	_cmsIT8SetPropertyInt (it8_lcms, "NUMBER_OF_SETS", 3);
	cmsIT8SetDataFormat (it8_lcms, 0, "XYZ_X");
	cmsIT8SetDataFormat (it8_lcms, 1, "XYZ_Y");
	cmsIT8SetDataFormat (it8_lcms, 2, "XYZ_Z");

	/* save instrument */
	if (it8->priv->instrument != NULL) {
		cmsIT8SetPropertyStr (it8_lcms, "INSTRUMENT",
				      it8->priv->instrument);
	}

	/* just save the matrix */
	_cmsIT8SetDataRowColDbl (it8_lcms, 0, 0, it8->priv->matrix.m00);
	_cmsIT8SetDataRowColDbl (it8_lcms, 0, 1, it8->priv->matrix.m01);
	_cmsIT8SetDataRowColDbl (it8_lcms, 0, 2, it8->priv->matrix.m02);
	_cmsIT8SetDataRowColDbl (it8_lcms, 1, 0, it8->priv->matrix.m10);
	_cmsIT8SetDataRowColDbl (it8_lcms, 1, 1, it8->priv->matrix.m11);
	_cmsIT8SetDataRowColDbl (it8_lcms, 1, 2, it8->priv->matrix.m12);
	_cmsIT8SetDataRowColDbl (it8_lcms, 2, 0, it8->priv->matrix.m20);
	_cmsIT8SetDataRowColDbl (it8_lcms, 2, 1, it8->priv->matrix.m21);
	_cmsIT8SetDataRowColDbl (it8_lcms, 2, 2, it8->priv->matrix.m22);

	return ret;
}

/**
 * cd_it8_save_to_file_cmf:
 **/
static gboolean
cd_it8_save_to_file_cmf (CdIt8 *it8, cmsHANDLE it8_lcms, GError **error)
{
	CdSpectrum *spectrum;
	guint i;
	guint j;
	guint number_of_sets;
	guint spectral_bands;

	cmsIT8SetSheetType (it8_lcms, "CMF    ");
	cmsIT8SetPropertyStr (it8_lcms, "DESCRIPTOR",
			      "Color Match Function");

	/* check data is valid */
	number_of_sets = it8->priv->array_spectra->len;
	if (number_of_sets != 3) {
		g_set_error_literal (error,
				     CD_IT8_ERROR,
				     CD_IT8_ERROR_INVALID_FORMAT,
				     "Cannot write CMF: XYZ data required");
		return FALSE;
	}

	/* all the arrays have to have the same length */
	spectrum = g_ptr_array_index (it8->priv->array_spectra, 0);
	spectral_bands = cd_spectrum_get_size (spectrum);
	_cmsIT8SetPropertyDbl (it8_lcms, "SPECTRAL_START_NM", cd_spectrum_get_start (spectrum));
	_cmsIT8SetPropertyDbl (it8_lcms, "SPECTRAL_END_NM", cd_spectrum_get_end (spectrum));
	_cmsIT8SetPropertyInt (it8_lcms, "SPECTRAL_BANDS", spectral_bands);
	_cmsIT8SetPropertyDbl (it8_lcms, "SPECTRAL_NORM", cd_spectrum_get_norm (spectrum));
	_cmsIT8SetPropertyInt (it8_lcms, "NUMBER_OF_FIELDS", spectral_bands);

	/* set DATA_FORMAT (using an ID if there are more than one spectra */
	spectrum = g_ptr_array_index (it8->priv->array_spectra, 0);
	for (i = 0; i < spectral_bands; i++) {
		_cleanup_free_ gchar *label = NULL;
		label = g_strdup_printf ("SPEC_%.0f",
					 cd_spectrum_get_wavelength (spectrum, i));
		cmsIT8SetDataFormat (it8_lcms, i, label);
	}

	/* set DATA */
	_cmsIT8SetPropertyInt (it8_lcms, "NUMBER_OF_SETS", number_of_sets);
	for (j = 0; j < number_of_sets; j++) {
		spectrum = g_ptr_array_index (it8->priv->array_spectra, j);
		for (i = 0; i < spectral_bands; i++) {
			if (it8->priv->normalized) {
				_cmsIT8SetDataRowColDbl (it8_lcms, j, i,
							 cd_spectrum_get_value (spectrum, i));
			} else {
				_cmsIT8SetDataRowColDbl (it8_lcms, j, i,
							 cd_spectrum_get_value_raw (spectrum, i));
			}
		}
	}
	return TRUE;
}

/**
 * cd_it8_save_to_file_ccss_sp:
 **/
static gboolean
cd_it8_save_to_file_ccss_sp (CdIt8 *it8, cmsHANDLE it8_lcms, GError **error)
{
	CdSpectrum *spectrum;
	gboolean has_index = FALSE;
	guint i;
	guint j;
	guint number_of_sets;
	guint spectral_bands;

	switch (it8->priv->kind) {
	case CD_IT8_KIND_CCSS:
		cmsIT8SetSheetType (it8_lcms, "CCSS   ");
		cmsIT8SetPropertyStr (it8_lcms, "DESCRIPTOR",
				      "Colorimeter Calibration Spectral Set");
		break;
	case CD_IT8_KIND_CMF:
		cmsIT8SetSheetType (it8_lcms, "CMF    ");
		cmsIT8SetPropertyStr (it8_lcms, "DESCRIPTOR",
				      "Color Match Function");
		break;
	case CD_IT8_KIND_SPECT:
		cmsIT8SetSheetType (it8_lcms, "SPECT  ");
		cmsIT8SetPropertyStr (it8_lcms, "DESCRIPTOR",
				      "Spectral Power");
		break;
	default:
		break;
	}

	/* check data is valid */
	number_of_sets = it8->priv->array_spectra->len;
	if (number_of_sets == 0) {
		g_set_error_literal (error,
				     CD_IT8_ERROR,
				     CD_IT8_ERROR_INVALID_FORMAT,
				     "Cannot write CCSS: spectral data required");
		return FALSE;
	}
	if (number_of_sets > 1)
		has_index = TRUE;

	/* all the arrays have to have the same length */
	spectrum = g_ptr_array_index (it8->priv->array_spectra, 0);
	spectral_bands = cd_spectrum_get_size (spectrum);
	_cmsIT8SetPropertyDbl (it8_lcms, "SPECTRAL_START_NM", cd_spectrum_get_start (spectrum));
	_cmsIT8SetPropertyDbl (it8_lcms, "SPECTRAL_END_NM", cd_spectrum_get_end (spectrum));
	_cmsIT8SetPropertyInt (it8_lcms, "SPECTRAL_BANDS", spectral_bands);
	_cmsIT8SetPropertyInt (it8_lcms, "NUMBER_OF_FIELDS", spectral_bands + has_index);
	if (it8->priv->normalized)
		_cmsIT8SetPropertyDbl (it8_lcms, "SPECTRAL_NORM", cd_spectrum_get_norm (spectrum));

	/* set DATA_FORMAT (using an ID if there are more than one spectra */
	if (has_index)
		cmsIT8SetDataFormat (it8_lcms, 0, "SAMPLE_ID");
	spectrum = g_ptr_array_index (it8->priv->array_spectra, 0);
	for (i = 0; i < spectral_bands; i++) {
		_cleanup_free_ gchar *label = NULL;
		/* there are more spectral bands than integers between the
		 * start and stop wavelengths */
		if ((cd_spectrum_get_end (spectrum) -
		     cd_spectrum_get_start (spectrum)) < spectral_bands) {
			label = g_strdup_printf ("SPEC_%.0f",
						 cd_spectrum_get_wavelength (spectrum, i) * 1000.f);
		} else {
			label = g_strdup_printf ("SPEC_%.0f",
						 cd_spectrum_get_wavelength (spectrum, i));
		}
		cmsIT8SetDataFormat (it8_lcms, i + has_index, label);
	}

	/* set DATA */
	_cmsIT8SetPropertyInt (it8_lcms, "NUMBER_OF_SETS", number_of_sets);
	for (j = 0; j < number_of_sets; j++) {
		spectrum = g_ptr_array_index (it8->priv->array_spectra, j);
		if (has_index) {
			cmsIT8SetDataRowCol (it8_lcms, j, 0,
					     cd_spectrum_get_id (spectrum));
		}
		for (i = 0; i < spectral_bands; i++) {
			if (it8->priv->normalized) {
				_cmsIT8SetDataRowColDbl (it8_lcms, j, i + has_index,
							 cd_spectrum_get_value (spectrum, i));
			} else {
				_cmsIT8SetDataRowColDbl (it8_lcms, j, i + has_index,
							 cd_spectrum_get_value_raw (spectrum, i));
			}
		}
	}
	return TRUE;
}

/**
 * cd_it8_save_to_data:
 * @it8: a #CdIt8 instance.
 * @data: a pointer to returned data
 * @size: size of @data
 * @error: a #GError, or %NULL
 *
 * Saves a it8 file to an area of memory.
 *
 * Return value: %TRUE if it8 file was saved.
 *
 * Since: 0.1.26
 **/
gboolean
cd_it8_save_to_data (CdIt8 *it8,
		     gchar **data,
		     gsize *size,
		     GError **error)
{
	cmsHANDLE it8_lcms = NULL;
	const gchar *tmp;
	gboolean ret;
	GDateTime *datetime = NULL;
	cmsUInt32Number size_tmp = 0;
	guint i;
	_cleanup_free_ gchar *data_tmp = NULL;
	_cleanup_free_ gchar *date_str = NULL;

	g_return_val_if_fail (CD_IS_IT8 (it8), FALSE);

	/* set common data */
	it8_lcms = cmsIT8Alloc (it8->priv->context_lcms);
	if (it8->priv->title != NULL) {
		cmsIT8SetPropertyStr (it8_lcms, "DISPLAY",
				      it8->priv->title);
	}
	if (it8->priv->originator != NULL) {
		cmsIT8SetPropertyStr (it8_lcms, "ORIGINATOR",
				      it8->priv->originator);
	}
	if (it8->priv->reference != NULL) {
		cmsIT8SetPropertyStr (it8_lcms, "REFERENCE",
				      it8->priv->reference);
	}

	/* set time and date in crazy ArgllCMS format, e.g.
	 * 'Wed Dec 19 18:47:57 2012' */
	if (it8->priv->enable_created) {
		datetime = g_date_time_new_now_local ();
		date_str = g_date_time_format (datetime, "%a %b %d %H:%M:%S %Y");
		cmsIT8SetPropertyStr (it8_lcms, "CREATED", date_str);
	}

	/* set ti1 and ti3 specific data */
	switch (it8->priv->kind) {
	case CD_IT8_KIND_TI1:
	case CD_IT8_KIND_TI3:
		ret = cd_it8_save_to_file_ti1_ti3 (it8, it8_lcms, error);
		if (!ret)
			goto out;
		break;
	case CD_IT8_KIND_CAL:
		ret = cd_it8_save_to_file_cal (it8, it8_lcms, error);
		if (!ret)
			goto out;
		break;
	case CD_IT8_KIND_CCMX:
		ret = cd_it8_save_to_file_ccmx (it8, it8_lcms, error);
		if (!ret)
			goto out;
		break;
	case CD_IT8_KIND_CMF:
		ret = cd_it8_save_to_file_cmf (it8, it8_lcms, error);
		if (!ret)
			goto out;
		break;
	case CD_IT8_KIND_CCSS:
	case CD_IT8_KIND_SPECT:
		ret = cd_it8_save_to_file_ccss_sp (it8, it8_lcms, error);
		if (!ret)
			goto out;
		break;
	default:
		break;
	}

	/* save any options */
	for (i = 0; i < it8->priv->options->len; i++) {
		tmp = g_ptr_array_index (it8->priv->options, i);
		cmsIT8SetPropertyStr (it8_lcms, tmp, "YES");
	}

	/* write the file */
	ret = cmsIT8SaveToMem (it8_lcms, NULL, &size_tmp);
	g_assert (ret);
	data_tmp = g_malloc (size_tmp);
	ret = cmsIT8SaveToMem (it8_lcms, data_tmp, &size_tmp);
	g_assert (ret);

	/* save for caller */
	if (data != NULL)
		*data = g_strdup (data_tmp);

	/* LCMS alocates an extra byte for the '\0' byte */
	if (size != NULL)
		*size = size_tmp - 1;
out:
	if (it8_lcms != NULL)
		cmsIT8Free (it8_lcms);
	if (datetime != NULL)
		g_date_time_unref (datetime);
	return ret;
}

/**
 * cd_it8_save_to_file:
 * @it8: a #CdIt8 instance.
 * @file: a #GFile
 * @error: a #GError, or %NULL
 *
 * Saves a it8 file to disk
 *
 * Return value: %TRUE if it8 file was saved.
 *
 * Since: 0.1.20
 **/
gboolean
cd_it8_save_to_file (CdIt8 *it8, GFile *file, GError **error)
{
	gsize size = 0;
	_cleanup_free_ gchar *data = NULL;

	g_return_val_if_fail (CD_IS_IT8 (it8), FALSE);
	g_return_val_if_fail (G_IS_FILE (file), FALSE);

	/* get data */
	if (!cd_it8_save_to_data (it8, &data, &size, error))
		return FALSE;

	/* save file */
	return g_file_replace_contents (file, data, size, NULL,
					FALSE, G_FILE_CREATE_NONE,
					NULL, NULL, error);
}

/**
 * cd_it8_add_option:
 * @it8: a #CdIt8 instance.
 * @option: A IT8 option, e.g. "TYPE_LCD"
 *
 * Sets any extra options that have to be set in the CCMX file
 *
 * Since: 0.1.20
 **/
void
cd_it8_add_option (CdIt8 *it8, const gchar *option)
{
	g_return_if_fail (CD_IS_IT8 (it8));
	g_ptr_array_add (it8->priv->options, g_strdup (option));
}

/**
 * cd_it8_set_normalized:
 * @it8: a #CdIt8 instance.
 * @normalized: If the data is normalized
 *
 * Sets if normalized data should be written to the .it8 file.
 *
 * Since: 0.1.20
 **/
void
cd_it8_set_normalized (CdIt8 *it8, gboolean normalized)
{
	g_return_if_fail (CD_IS_IT8 (it8));
	it8->priv->normalized = normalized;
}

/**
 * cd_it8_set_spectral:
 * @it8: a #CdIt8 instance.
 * @spectral: If the data is spectral
 *
 * Sets if spectral data should be written to the .it8 file.
 *
 * Since: 0.1.20
 **/
void
cd_it8_set_spectral (CdIt8 *it8, gboolean spectral)
{
	g_return_if_fail (CD_IS_IT8 (it8));
	it8->priv->spectral = spectral;
}

/**
 * cd_it8_set_originator:
 * @it8: a #CdIt8 instance.
 * @originator: the program name, e.g. "gcm-calibrate"
 *
 * Sets the program name that created the .it8 file
 *
 * Since: 0.1.20
 **/
void
cd_it8_set_originator (CdIt8 *it8, const gchar *originator)
{
	g_return_if_fail (CD_IS_IT8 (it8));

	g_free (it8->priv->originator);
	it8->priv->originator = g_strdup (originator);
}

/**
 * cd_it8_set_title:
 * @it8: a #CdIt8 instance.
 * @title: the title name, e.g. "Factory calibration"
 *
 * Sets the display name for the file.
 *
 * Since: 0.1.20
 **/
void
cd_it8_set_title (CdIt8 *it8, const gchar *title)
{
	g_return_if_fail (CD_IS_IT8 (it8));

	g_free (it8->priv->title);
	it8->priv->title = g_strdup (title);
}

/**
 * cd_it8_set_instrument:
 * @it8: a #CdIt8 instance.
 * @instrument: the instruemnt name, e.g. "huey"
 *
 * Sets the measuring instrument that created the .it8 file
 *
 * Since: 0.1.20
 **/
void
cd_it8_set_instrument (CdIt8 *it8, const gchar *instrument)
{
	g_return_if_fail (CD_IS_IT8 (it8));

	g_free (it8->priv->instrument);
	it8->priv->instrument = g_strdup (instrument);
}

/**
 * cd_it8_set_reference:
 * @it8: a #CdIt8 instance.
 * @reference: the instruemnt name, e.g. "colormunki"
 *
 * Sets the reference that as used to create the .it8 reference
 *
 * Since: 0.1.20
 **/
void
cd_it8_set_reference (CdIt8 *it8, const gchar *reference)
{
	g_return_if_fail (CD_IS_IT8 (it8));

	g_free (it8->priv->reference);
	it8->priv->reference = g_strdup (reference);
}

/**
 * cd_it8_set_enable_created:
 * @it8: a #CdIt8 instance.
 * @enable_created: Is 'CREATED' should be written
 *
 * Sets if the 'CREATED' attribute should be written. This is mainly useful
 * in the self test programs where we want to string compare the output data
 * with a known reference.
 *
 * Since: 0.1.33
 **/
void
cd_it8_set_enable_created (CdIt8 *it8, gboolean enable_created)
{
	g_return_if_fail (CD_IS_IT8 (it8));
	it8->priv->enable_created = enable_created;
}

/**
 * cd_it8_add_data:
 * @it8: a #CdIt8 instance.
 * @rgb: a #CdColorRGB, or %NULL
 * @xyz: a #CdColorXYZ, or %NULL
 *
 * Adds a reading to this object. If either of @rgb or @xyz is NULL then
 * a black reading (0.0, 0.0, 0.0) is added instead.
 *
 * Since: 0.1.20
 **/
void
cd_it8_add_data (CdIt8 *it8, const CdColorRGB *rgb, const CdColorXYZ *xyz)
{
	CdColorRGB *rgb_tmp;
	CdColorXYZ *xyz_tmp;

	g_return_if_fail (CD_IS_IT8 (it8));

	/* add RGB */
	if (rgb != NULL) {
		rgb_tmp = cd_color_rgb_dup (rgb);
	} else {
		rgb_tmp = cd_color_rgb_new ();
		cd_color_rgb_set (rgb_tmp, 0.0f, 0.0f, 0.0f);
	}
	g_ptr_array_add (it8->priv->array_rgb, rgb_tmp);

	/* add XYZ */
	if (xyz != NULL) {
		xyz_tmp = cd_color_xyz_dup (xyz);
	} else {
		xyz_tmp = cd_color_xyz_new ();
		cd_color_xyz_set (xyz_tmp, 0.0f, 0.0f, 0.0f);
	}
	g_ptr_array_add (it8->priv->array_xyz, xyz_tmp);
}

/**
 * cd_it8_get_data_size:
 * @it8: a #CdIt8 instance.
 *
 * Gets the data size.
 *
 * Return value: The number of RGB-XYZ readings in this object.
 *
 * Since: 0.1.20
 **/
guint
cd_it8_get_data_size (CdIt8 *it8)
{
	g_return_val_if_fail (CD_IS_IT8 (it8), G_MAXUINT);
	return it8->priv->array_xyz->len;
}

/**
 * cd_it8_get_data_item:
 * @it8: a #CdIt8 instance.
 * @idx: the item index
 * @rgb: the returned RGB value
 * @xyz: the returned XYZ value
 *
 * Gets a specific bit of data from this object.
 * The returned data are absolute readings and are not normalised.
 *
 * Return value: %TRUE if the index existed.
 *
 * Since: 0.1.20
 **/
gboolean
cd_it8_get_data_item (CdIt8 *it8, guint idx, CdColorRGB *rgb, CdColorXYZ *xyz)
{
	const CdColorRGB *rgb_tmp;
	const CdColorXYZ *xyz_tmp;

	g_return_val_if_fail (CD_IS_IT8 (it8), FALSE);

	if (idx > it8->priv->array_xyz->len)
		return FALSE;
	if (rgb != NULL) {
		rgb_tmp = g_ptr_array_index (it8->priv->array_rgb, idx);
		cd_color_rgb_copy (rgb_tmp, rgb);
	}
	if (xyz != NULL) {
		xyz_tmp = g_ptr_array_index (it8->priv->array_xyz, idx);
		cd_color_xyz_copy (xyz_tmp, xyz);
	}
	return TRUE;
}

/**
 * cd_it8_get_xyz_for_rgb:
 * @it8: a #CdIt8 instance.
 * @R: the red value
 * @G: the green value
 * @B: the blue value
 * @delta: the smallest difference between colors, e.g. 0.01f
 *
 * Gets the XYZ value for a specific RGB value.
 *
 * Return value: (transfer none): A CdColorXYZ, or %NULL if the sample does not exist.
 *
 * Since: 1.2.6
 **/
CdColorXYZ *
cd_it8_get_xyz_for_rgb (CdIt8 *it8, gdouble R, gdouble G, gdouble B, gdouble delta)
{
	CdColorXYZ *xyz_tmp;
	guint i;
	const CdColorRGB *rgb_tmp;

	g_return_val_if_fail (CD_IS_IT8 (it8), FALSE);

	for (i = 0; i < it8->priv->array_xyz->len; i++) {
		rgb_tmp = g_ptr_array_index (it8->priv->array_rgb, i);
		if (ABS (rgb_tmp->R - R) > delta)
			continue;
		if (ABS (rgb_tmp->G - G) > delta)
			continue;
		if (ABS (rgb_tmp->B - B) > delta)
			continue;
		xyz_tmp = g_ptr_array_index (it8->priv->array_xyz, i);
		return xyz_tmp;
	}
	return NULL;
}

/**
 * cd_it8_set_spectrum_array:
 * @it8: a #CdIt8 instance.
 * @data: (transfer container) (element-type CdSpectrum): the spectral data
 *
 * Set the spectral data
 *
 * Since: 1.1.6
 **/
void
cd_it8_set_spectrum_array (CdIt8 *it8, GPtrArray *data)
{
	g_return_if_fail (CD_IS_IT8 (it8));
	g_ptr_array_unref (it8->priv->array_spectra);
	it8->priv->array_spectra = g_ptr_array_ref (data);
}

/**
 * cd_it8_add_spectrum:
 * @it8: a #CdIt8 instance.
 * @spectrum: the spectral data
 *
 * Adds a spectrum to the spectral array.
 *
 * Since: 1.1.6
 **/
void
cd_it8_add_spectrum (CdIt8 *it8, CdSpectrum *spectrum)
{
	const gchar *id;
	CdSpectrum *tmp;

	g_return_if_fail (CD_IS_IT8 (it8));

	/* remove any existing spectra with this same ID */
	id = cd_spectrum_get_id (spectrum);
	if (id != NULL) {
		tmp = cd_it8_get_spectrum_by_id (it8, id);
		if (tmp != NULL)
			g_ptr_array_remove (it8->priv->array_spectra, tmp);
	}

	/* add this */
	g_ptr_array_add (it8->priv->array_spectra, cd_spectrum_dup (spectrum));
}

/**
 * cd_it8_get_spectrum_array:
 * @it8: a #CdIt8 instance.
 *
 * Gets the spectral data of IT8 file.
 *
 * Return value: (transfer container) (element-type CdSpectrum): spectral data
 *
 * Since: 1.1.6
 **/
GPtrArray *
cd_it8_get_spectrum_array (CdIt8 *it8)
{
	g_return_val_if_fail (CD_IS_IT8 (it8), NULL);
	return g_ptr_array_ref (it8->priv->array_spectra);
}

/**
 * cd_it8_get_spectrum_by_id:
 * @it8: a #CdIt8 instance.
 * @id: the spectrum ID value
 *
 * Gets a specific spectrum in an IT8 file.
 *
 * Return value: (transfer none): spectrum, or %NULL
 *
 * Since: 1.1.6
 **/
CdSpectrum *
cd_it8_get_spectrum_by_id (CdIt8 *it8, const gchar *id)
{
	CdSpectrum *tmp;
	guint i;

	g_return_val_if_fail (CD_IS_IT8 (it8), NULL);
	g_return_val_if_fail (id != NULL, NULL);

	for (i = 0; i < it8->priv->array_spectra->len; i++) {
		tmp = g_ptr_array_index (it8->priv->array_spectra, i);
		if (g_strcmp0 (cd_spectrum_get_id (tmp), id) == 0)
			return tmp;
	}
	return NULL;
}

/**********************************************************************/

/*
 * cd_it8_get_property:
 */
static void
cd_it8_get_property (GObject *object,
		     guint prop_id,
		     GValue *value,
		     GParamSpec *pspec)
{
	CdIt8 *it8 = CD_IT8 (object);

	switch (prop_id) {
	case PROP_KIND:
		g_value_set_uint (value, it8->priv->kind);
		break;
	case PROP_NORMALIZED:
		g_value_set_boolean (value, it8->priv->normalized);
		break;
	case PROP_ORIGINATOR:
		g_value_set_string (value, it8->priv->originator);
		break;
	case PROP_TITLE:
		g_value_set_string (value, it8->priv->title);
		break;
	case PROP_INSTRUMENT:
		g_value_set_string (value, it8->priv->instrument);
		break;
	case PROP_REFERENCE:
		g_value_set_string (value, it8->priv->reference);
		break;
	case PROP_SPECTRAL:
		g_value_set_boolean (value, it8->priv->spectral);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

/**
 * cd_it8_set_property:
 **/
static void
cd_it8_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
	CdIt8 *it8 = CD_IT8 (object);

	switch (prop_id) {
	case PROP_KIND:
		it8->priv->kind = g_value_get_uint (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

/*
 * cd_it8_class_init:
 */
static void
cd_it8_class_init (CdIt8Class *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->get_property = cd_it8_get_property;
	object_class->set_property = cd_it8_set_property;
	object_class->finalize = cd_it8_finalize;

	/**
	 * CdIt8:kind:
	 *
	 * The kind of IT8 file.
	 *
	 * Since: 0.1.20
	 **/
	g_object_class_install_property (object_class,
					 PROP_KIND,
					 g_param_spec_uint ("kind",
							    NULL, NULL,
							    0, G_MAXUINT, 0,
							    G_PARAM_READWRITE));

	/**
	 * CdIt8:normalized:
	 *
	 * If the results file is normalized.
	 *
	 * Since: 0.1.20
	 **/
	g_object_class_install_property (object_class,
					 PROP_NORMALIZED,
					 g_param_spec_boolean ("normalized",
							       NULL, NULL,
							       FALSE,
							       G_PARAM_READABLE));

	/**
	 * CdIt8:originator:
	 *
	 * The framework that created the results, e.g. "cd-self-test"
	 *
	 * Since: 0.1.20
	 **/
	g_object_class_install_property (object_class,
					 PROP_ORIGINATOR,
					 g_param_spec_string ("originator",
							      NULL, NULL,
							      NULL,
							      G_PARAM_READABLE));

	/**
	 * CdIt8:title:
	 *
	 * The file title, e.g. "Factor calibration".
	 *
	 * Since: 0.1.20
	 **/
	g_object_class_install_property (object_class,
					 PROP_TITLE,
					 g_param_spec_string ("title",
							      NULL, NULL,
							      NULL,
							      G_PARAM_READABLE));

	/**
	 * CdIt8:instrument:
	 *
	 * The instrument that created the results, e.g. "huey"
	 *
	 * Since: 0.1.20
	 **/
	g_object_class_install_property (object_class,
					 PROP_INSTRUMENT,
					 g_param_spec_string ("instrument",
							      NULL, NULL,
							      NULL,
							      G_PARAM_READABLE));

	/**
	 * CdIt8:reference:
	 *
	 * The reference that created the results, e.g. "colormunki"
	 *
	 * Since: 0.1.20
	 **/
	g_object_class_install_property (object_class,
					 PROP_REFERENCE,
					 g_param_spec_string ("reference",
							      NULL, NULL,
							      NULL,
							      G_PARAM_READABLE));

	/**
	 * CdIt8:spectral:
	 *
	 * If the results file is spectral.
	 *
	 * Since: 0.1.20
	 **/
	g_object_class_install_property (object_class,
					 PROP_SPECTRAL,
					 g_param_spec_boolean ("spectral",
							       NULL, NULL,
							       FALSE,
							       G_PARAM_READABLE));

	g_type_class_add_private (klass, sizeof (CdIt8Private));
}

/*
 * cd_it8_init:
 */
static void
cd_it8_init (CdIt8 *it8)
{
	it8->priv = CD_IT8_GET_PRIVATE (it8);
	it8->priv->context_lcms = cd_context_lcms_new ();

	cd_mat33_clear (&it8->priv->matrix);
	it8->priv->array_rgb = g_ptr_array_new_with_free_func ((GDestroyNotify) cd_color_rgb_free);
	it8->priv->array_xyz = g_ptr_array_new_with_free_func ((GDestroyNotify) cd_color_xyz_free);
	it8->priv->array_spectra = g_ptr_array_new_with_free_func ((GDestroyNotify) cd_spectrum_free);
	it8->priv->options = g_ptr_array_new_with_free_func (g_free);
	it8->priv->enable_created = TRUE;

	/* ensure the remote errors are registered */
	cd_it8_error_quark ();
}

/*
 * cd_it8_finalize:
 */
static void
cd_it8_finalize (GObject *object)
{
	CdIt8 *it8 = CD_IT8 (object);

	g_return_if_fail (CD_IS_IT8 (object));

	cd_context_lcms_free (it8->priv->context_lcms);
	g_ptr_array_unref (it8->priv->array_spectra);
	g_ptr_array_unref (it8->priv->array_rgb);
	g_ptr_array_unref (it8->priv->array_xyz);
	g_ptr_array_unref (it8->priv->options);
	g_free (it8->priv->originator);
	g_free (it8->priv->title);
	g_free (it8->priv->instrument);
	g_free (it8->priv->reference);

	G_OBJECT_CLASS (cd_it8_parent_class)->finalize (object);
}

/**
 * cd_it8_new:
 *
 * Creates a new #CdIt8 object.
 *
 * Return value: a new CdIt8 object.
 *
 * Since: 0.1.20
 **/
CdIt8 *
cd_it8_new (void)
{
	CdIt8 *it8;
	it8 = g_object_new (CD_TYPE_IT8, NULL);
	return CD_IT8 (it8);
}

/**
 * cd_it8_new_with_kind:
 * @kind: a #CdIt8Kind, e.g %CD_IT8_KIND_TI3.
 *
 * Creates a new #CdIt8 object.
 *
 * Return value: a new CdIt8 object.
 *
 * Since: 0.1.20
 **/
CdIt8 *
cd_it8_new_with_kind (CdIt8Kind kind)
{
	CdIt8 *it8;
	it8 = g_object_new (CD_TYPE_IT8,
			    "kind", kind,
			    NULL);
	return CD_IT8 (it8);
}

