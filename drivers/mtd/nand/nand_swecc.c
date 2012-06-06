#include <common.h>
#include <errno.h>
#include <clock.h>
#include <linux/err.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/nand.h>
#include <linux/mtd/nand_ecc.h>
#include <linux/mtd/nand_bch.h>
#include <asm/byteorder.h>
#include <io.h>
#include <malloc.h>

#include "nand.h"

/**
 * nand_read_page_swecc - [REPLACEABLE] software ECC based page read function
 * @mtd: mtd info structure
 * @chip: nand chip info structure
 * @buf: buffer to store read data
 * @page: page number to read
 */
static int nand_read_page_swecc(struct mtd_info *mtd, struct nand_chip *chip,
				uint8_t *buf, int page)
{
	int i, eccsize = chip->ecc.size;
	int eccbytes = chip->ecc.bytes;
	int eccsteps = chip->ecc.steps;
	uint8_t *p = buf;
	uint8_t *ecc_calc = chip->buffers->ecccalc;
	uint8_t *ecc_code = chip->buffers->ecccode;
	uint32_t *eccpos = chip->ecc.layout->eccpos;

	chip->ecc.read_page_raw(mtd, chip, buf, page);

	for (i = 0; eccsteps; eccsteps--, i += eccbytes, p += eccsize)
		chip->ecc.calculate(mtd, p, &ecc_calc[i]);

	for (i = 0; i < chip->ecc.total; i++)
		ecc_code[i] = chip->oob_poi[eccpos[i]];

	eccsteps = chip->ecc.steps;
	p = buf;

	for (i = 0 ; eccsteps; eccsteps--, i += eccbytes, p += eccsize) {
		int stat;

		stat = chip->ecc.correct(mtd, p, &ecc_code[i], &ecc_calc[i]);
		if (stat < 0)
			mtd->ecc_stats.failed++;
		else
			mtd->ecc_stats.corrected += stat;
	}
	return 0;
}

/**
 * nand_write_page_swecc - [REPLACEABLE] software ECC based page write function
 * @mtd: mtd info structure
 * @chip: nand chip info structure
 * @buf: data buffer
 */
#ifdef CONFIG_MTD_WRITE
static void nand_write_page_swecc(struct mtd_info *mtd, struct nand_chip *chip,
				  const uint8_t *buf)
{
	int i, eccsize = chip->ecc.size;
	int eccbytes = chip->ecc.bytes;
	int eccsteps = chip->ecc.steps;
	uint8_t *ecc_calc = chip->buffers->ecccalc;
	const uint8_t *p = buf;
	uint32_t *eccpos = chip->ecc.layout->eccpos;

	/* Software ECC calculation */
	for (i = 0; eccsteps; eccsteps--, i += eccbytes, p += eccsize)
		chip->ecc.calculate(mtd, p, &ecc_calc[i]);

	for (i = 0; i < chip->ecc.total; i++)
		chip->oob_poi[eccpos[i]] = ecc_calc[i];

	chip->ecc.write_page_raw(mtd, chip, buf);
}
#endif

void nand_init_ecc_soft(struct mtd_info *mtd, struct nand_chip *chip)
{
	switch (chip->ecc.mode) {
	case NAND_ECC_SOFT:
		chip->ecc.calculate = nand_calculate_ecc;
		chip->ecc.correct = nand_correct_data;
		chip->ecc.read_page = nand_read_page_swecc;
		chip->ecc.read_subpage = nand_read_subpage;
		chip->ecc.read_page_raw = nand_read_page_raw;
#ifdef CONFIG_NAND_READ_OOB
		chip->ecc.read_oob = nand_read_oob_std;
#endif
#ifdef CONFIG_MTD_WRITE
		chip->ecc.write_page = nand_write_page_swecc;
		chip->ecc.write_page_raw = nand_write_page_raw;
		chip->ecc.write_oob = nand_write_oob_std;
#endif
		if (!chip->ecc.size)
			chip->ecc.size = 256;
		chip->ecc.bytes = 3;
		chip->ecc.strength = 1;
		break;

	case NAND_ECC_SOFT_BCH:
		if (!mtd_nand_has_bch()) {
			pr_warn("CONFIG_MTD_ECC_BCH not enabled\n");
			BUG();
		}
		chip->ecc.calculate = nand_bch_calculate_ecc;
		chip->ecc.correct = nand_bch_correct_data;
		chip->ecc.read_page = nand_read_page_swecc;
		chip->ecc.read_subpage = nand_read_subpage;
		chip->ecc.read_page_raw = nand_read_page_raw;
#ifdef CONFIG_NAND_READ_OOB
		chip->ecc.read_oob = nand_read_oob_std;
#endif
#ifdef CONFIG_MTD_WRITE
		chip->ecc.write_page = nand_write_page_swecc;
		chip->ecc.write_page_raw = nand_write_page_raw;
		chip->ecc.write_oob = nand_write_oob_std;
#endif
		/*
		 * Board driver should supply ecc.size and ecc.bytes values to
		 * select how many bits are correctable; see nand_bch_init()
		 * for details. Otherwise, default to 4 bits for large page
		 * devices.
		 */
		if (!chip->ecc.size && (mtd->oobsize >= 64)) {
			chip->ecc.size = 512;
			chip->ecc.bytes = 7;
		}
		chip->ecc.priv = nand_bch_init(mtd,
					       chip->ecc.size,
					       chip->ecc.bytes,
					       &chip->ecc.layout);
		if (!chip->ecc.priv) {
			pr_warn("BCH ECC initialization failed!\n");
			BUG();
		}
		chip->ecc.strength =
			chip->ecc.bytes*8 / fls(8*chip->ecc.size);
		break;
	
	default:
		break;
	}
}
