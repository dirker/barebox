/*
 * Copyright (C) 2009 Juergen Beisert, Pengutronix
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 *
 */

#ifdef CONFIG_S3C_NAND_BOOT
extern void s3c24x0_nand_load_image(void*, int, int);
#endif

/**
 * Locate the timing bits for the NFCONF register
 * @param setup is the TACLS clock count
 * @param access is the TWRPH0 clock count
 * @param hold is the TWRPH1 clock count
 *
 * @note A clock count of 0 means always 1 HCLK clock.
 * @note Clock count settings depend on the NAND flash requirements and the current HCLK speed
 */
#ifdef CONFIG_CPU_S3C2410
# define CALC_NFCONF_TIMING(setup, access, hold) \
	((setup << 8) + (access << 4) + (hold << 0))
#endif
#ifdef CONFIG_CPU_S3C2440
# define CALC_NFCONF_TIMING(setup, access, hold) \
	((setup << 12) + (access << 8) + (hold << 4))
#endif

/**
 * Define platform specific data for the NAND controller and its device
 */
struct s3c24x0_nand_platform_data {
	uint32_t nand_timing;	/**< value for the NFCONF register (timing bits only) */
	char flash_bbt;	/**< force a flash based BBT */
};

/**
 * @file
 * @brief Basic declaration to use the s3c24x0 NAND driver
 */
