/*
 *  Copyright (c) 2024 Infineon Technologies AG.
 *
 *  This file is part of TAS Client, an API for device access for Infineon's
 *  automotive MCUs.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *  ****************************************************************************************************************
 */

#pragma once

/* Standard includes */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

/*! \brief Infineon's MCU families */
enum aurix_device_family {
	AURIX_DF_UNKNOWN = 0,	/*!< \brief Unknown device family */
	AURIX_DF_TC1X = 0x0110,	/*!< \brief TC1x, RiderD, AudoNG, TC11, AudoMax */
	AURIX_DF_TC2X = 0x0120,	/*!< \brief TC2x */
	AURIX_DF_TC3X = 0x0130,	/*!< \brief TC3x */
	AURIX_DF_TC4X = 0x0140,	/*!< \brief TC4x */
};

/*! \brief List of device type identifiers (IEEE 1149.1 device ID) or JTAG IDs */
enum aurix_device_type {

	AURIX_DT_VERSION_MASK_OUT =
		0x0FFFFFFF,	/*!< \brief JTAG ID version nibble masked out */

	/* Museum */
	AURIX_DT_RIDERD = 0x00063083,
	AURIX_DT_RIDERD_A = 0x10063083,
	AURIX_DT_TC1766 = 0x000DB083,
	AURIX_DT_TC1766_B = 0x200DB083,
	AURIX_DT_TC1796 = 0x000B8083,
	AURIX_DT_TC1796_B = 0x200B8083,

	/* TC2x */
	AURIX_DT_TC21X = 0x00202083,
	AURIX_DT_TC21X_A = 0x10202083,

	AURIX_DT_TC22X = 0x00201083,
	AURIX_DT_TC22X_A = 0x10201083,

	AURIX_DT_TC23X = 0x00200083,
	AURIX_DT_TC23X_A = 0x10200083,

	AURIX_DT_TC26X = 0x001E8083,
	AURIX_DT_TC26X_A = 0x101E8083,
	AURIX_DT_TC26X_B = 0x201E8083,

	AURIX_DT_TC27X = 0x001DA083,
	AURIX_DT_TC2D5ED = 0x101DA083,
	AURIX_DT_TC27X_A = 0x201DA083,
	AURIX_DT_TC27X_B = 0x301DA083,
	AURIX_DT_TC27X_C = 0x401DA083,
	AURIX_DT_TC27X_D = 0x501DA083,

	AURIX_DT_TC29X = 0x001E9083,
	AURIX_DT_TC29X_A = 0x101E9083,
	AURIX_DT_TC29X_B = 0x201E9083,

	/* TC3x */
	AURIX_DT_TC33X = 0x0020B083,
	AURIX_DT_TC33X_A = 0x1020B083,

	AURIX_DT_TC33XE = 0x0020C083,
	AURIX_DT_TC33XE_A = 0x1020C083,

	AURIX_DT_TC35X = 0x0020A083,
	AURIX_DT_TC35X_A = 0x1020A083,

	AURIX_DT_TC36X = 0x00209083,
	AURIX_DT_TC36X_A = 0x10209083,

	AURIX_DT_TC37X = 0x00207083,
	AURIX_DT_TC37X_A = 0x10207083,

	AURIX_DT_TC37XE = 0x00208083,
	AURIX_DT_TC37XE_A = 0x10208083,

	AURIX_DT_TC38X = 0x00206083,
	AURIX_DT_TC38X_A = 0x10206083,

	AURIX_DT_TC3EX = 0x00215083,
	AURIX_DT_TC3EX_A = 0x10215083,

	AURIX_DT_TC39X = 0x00205083,
	AURIX_DT_TC39X_A = 0x10205083,
	AURIX_DT_TC39X_B = 0x20205083,

	AURIX_DT_DAPEA2G = 0x0000DA9E,	/* A2G DAPE connection */

	/* TC4x */
	AURIX_DT_TC41X = 0x00218083,
	AURIX_DT_TC41X_A = 0x10218083,

	AURIX_DT_TC42X = 0x00219083,
	AURIX_DT_TC42X_A = 0x10219083,

	AURIX_DT_TC44X = 0x0021A083,
	AURIX_DT_TC44X_A = 0x1021A083,

	AURIX_DT_TC45X = 0x0021B083,
	AURIX_DT_TC45X_A = 0x1021B083,

	AURIX_DT_TC46X = 0x0021C083,
	AURIX_DT_TC46X_A = 0x1021C083,

	AURIX_DT_TC48X = 0x0021D083,
	AURIX_DT_TC48X_A = 0x1021D083,

	AURIX_DT_TC49AA = 0x0021E083,	/* TC49xA */
	AURIX_DT_TC49AAA = 0x1021E083,

	AURIX_DT_TC49X = 0x0022B083,	/* TC49xN */
	AURIX_DT_TC49X_A = 0x1022B083,

	AURIX_DT_TC4DX = 0x00225083,
	AURIX_DT_TC4DX_A = 0x10225083,

	AURIX_DT_TC4RX = 0x00223083,
	AURIX_DT_TC4RX_A = 0x10223083,
};

/*! \brief Conversion from device type (JTAG ID) to a string */
/*! \param device_type device type or JTAG ID */
/*! \returns pointer to a c-string containing string representation of device */
/*! type */
static inline const char *aurix_get_device_name_str(uint32_t device_type)
{
	switch (device_type & AURIX_DT_VERSION_MASK_OUT) {

		case 0:
			return "no device";

		case AURIX_DT_TC33X:
			return "TC33x";
		case AURIX_DT_TC33XE:
			return "TC33xE";
		case AURIX_DT_TC35X:
			return "TC35x";
		case AURIX_DT_TC36X:
			return "TC36x";
		case AURIX_DT_TC37X:
			return "TC37x";
		case AURIX_DT_TC37XE:
			return "TC37xE";
		case AURIX_DT_TC38X:
			return "TC38x";
		case AURIX_DT_TC39X:
			return (device_type == AURIX_DT_TC39X_A) ? "TC39x_A" : "TC39x";
		case AURIX_DT_TC3EX:
			return "TC3Ex";
		case AURIX_DT_DAPEA2G:
			return "TC3x DAPE";

		case AURIX_DT_TC41X:
			return "TC41x";
		case AURIX_DT_TC42X:
			return "TC42x";
		case AURIX_DT_TC44X:
			return "TC44x";
		case AURIX_DT_TC45X:
			return "TC45x";
		case AURIX_DT_TC46X:
			return "TC46x";
		case AURIX_DT_TC48X:
			return "TC48x";
		case AURIX_DT_TC49AA:
			return "TC49xA";
		case AURIX_DT_TC49X:
			return "TC49x";
		case AURIX_DT_TC4DX:
			return "TC4Dx";
		case AURIX_DT_TC4RX:
			return "TC4RxA";

		case AURIX_DT_TC21X:
			return "TC21x";
		case AURIX_DT_TC22X:
			return "TC22x";
		case AURIX_DT_TC23X:
			return "TC23x";
		case AURIX_DT_TC26X:
			return "TC26x";
		case AURIX_DT_TC27X:
			return "TC27x";
		case AURIX_DT_TC29X:
			return "TC29x";

		case AURIX_DT_RIDERD:
			return "RiderD";
		case AURIX_DT_TC1766:
			return "TC1766";
		case AURIX_DT_TC1796:
			return "TC1796";

		default:
			return "UNKNOWN";
	}
}

/*! \brief Performs a check if device type falls into TC1x family */
/*! \returns \c true if yes, otherwise \c false */
static inline bool aurix_df_check_if_tc1x(uint32_t device_type)
{
	switch (device_type & AURIX_DT_VERSION_MASK_OUT) {
		case AURIX_DT_RIDERD:
		case AURIX_DT_TC1766:
		case AURIX_DT_TC1796:
			return true;
		default:
			return false;
	}
}

/*! \brief Performs a check if device type falls into TC2x family */
/*! \returns \c true if yes, otherwise \c false */
static inline bool aurix_df_check_if_tc2x(uint32_t device_type)
{
	switch (device_type & AURIX_DT_VERSION_MASK_OUT) {
		case AURIX_DT_TC21X:
		case AURIX_DT_TC22X:
		case AURIX_DT_TC23X:
		case AURIX_DT_TC26X:
		case AURIX_DT_TC27X:
		case AURIX_DT_TC29X:
			return true;
		default:
			return false;
	}
}

/*! \brief Performs a check if device type falls into TC3x family */
/*! \returns \c true if yes, otherwise \c false */
static inline bool aurix_df_check_if_tc3x(uint32_t device_type)
{
	switch (device_type & AURIX_DT_VERSION_MASK_OUT) {
		case AURIX_DT_TC33X:
		case AURIX_DT_TC33XE:
		case AURIX_DT_TC35X:
		case AURIX_DT_TC36X:
		case AURIX_DT_TC37X:
		case AURIX_DT_TC37XE:
		case AURIX_DT_TC38X:
		case AURIX_DT_TC3EX:
		case AURIX_DT_TC39X:
		case AURIX_DT_DAPEA2G:
			return true;
		default:
			return false;
	}
}

/*! \brief Performs a check if device type falls into TC4x family */
/*! \returns \c true if yes, otherwise \c false */
static inline bool aurix_df_check_if_tc4x(uint32_t device_type)
{
	switch (device_type & AURIX_DT_VERSION_MASK_OUT) {
		case AURIX_DT_TC41X:
		case AURIX_DT_TC42X:
		case AURIX_DT_TC44X:
		case AURIX_DT_TC45X:
		case AURIX_DT_TC46X:
		case AURIX_DT_TC48X:
		case AURIX_DT_TC49AA:
		case AURIX_DT_TC49X:
		case AURIX_DT_TC4DX:
		case AURIX_DT_TC4RX:
			return true;
		default:
			return false;
	}
}

/*! \brief Get device family based on device type */
/*! \returns device family identifier */
static inline enum aurix_device_family aurix_get_device_family(uint32_t device_type)
{
	if (aurix_df_check_if_tc4x(device_type))
		return AURIX_DF_TC4X;
	if (aurix_df_check_if_tc3x(device_type))
		return AURIX_DF_TC3X;
	if (aurix_df_check_if_tc2x(device_type))
		return AURIX_DF_TC2X;
	if (aurix_df_check_if_tc1x(device_type))
		return AURIX_DF_TC1X;
	return AURIX_DF_UNKNOWN;
}

/*! \brief Performs a check if device family is an AURIX family */
/*! \returns \c true if yes, otherwise \c false */
static inline bool aurix_device_family_is_aurix(enum aurix_device_family device_family)
{
	return ((device_family == AURIX_DF_TC4X) ||
	       (device_family == AURIX_DF_TC3X) || (device_family == AURIX_DF_TC2X));
}

/*! \brief Performs a check if device type falls into an AURIX family */
/*! \returns \c true if yes, otherwise \c false */
static inline bool aurix_device_type_is_aurix(uint32_t device_type)
{
	return aurix_device_family_is_aurix(aurix_get_device_family(device_type));
}

/*! \brief Conversion from device fmaily to a string */
/*! \param device_family device family */
/*! \returns pointer to a c-string containing string representation of device */
/*! family */
static inline const char *aurix_get_device_family_str(enum aurix_device_family device_family)
{
	switch (device_family) {
		case AURIX_DF_TC4X:
			return "TC4x";
		case AURIX_DF_TC3X:
			return "TC3x";
		case AURIX_DF_TC2X:
			return "TC2x";
		case AURIX_DF_TC1X:
			return "TC1x";
		default:
			return "UNKNOWN";
	}
}
