#include <common.h>
#include <errno.h>
#include <clock.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/nand.h>
#include <linux/err.h>
#include <linux/mtd/nand_ecc.h>
#include <asm/byteorder.h>
#include <io.h>
#include <malloc.h>

#include "nand.h"

/**
 * nand_read_page_hwecc - [REPLACEABLE] hardware ECC based page read function
 * @mtd: mtd info structure
 * @chip: nand chip info structure
 * @buf: buffer to store read data
 * @page: page number to read
 *
 * Not for syndrome calculating ECC controllers which need a special oob layout.
 */
/* static */ int nand_read_page_hwecc(struct mtd_info *mtd, struct nand_chip *chip,
				uint8_t *buf, int page)
{
	int i, eccsize = chip->ecc.size;
	int eccbytes = chip->ecc.bytes;
	int eccsteps = chip->ecc.steps;
	uint8_t *p = buf;
	uint8_t *ecc_calc = chip->buffers->ecccalc;
	uint8_t *ecc_code = chip->buffers->ecccode;
	uint32_t *eccpos = chip->ecc.layout->eccpos;

	for (i = 0; eccsteps; eccsteps--, i += eccbytes, p += eccsize) {
		chip->ecc.hwctl(mtd, NAND_ECC_READ);
		chip->read_buf(mtd, p, eccsize);
		chip->ecc.calculate(mtd, p, &ecc_calc[i]);
	}
	chip->read_buf(mtd, chip->oob_poi, mtd->oobsize);

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
 * nand_read_page_hwecc_oob_first - [REPLACEABLE] hw ecc, read oob first
 * @mtd: mtd info structure
 * @chip: nand chip info structure
 * @buf: buffer to store read data
 * @page: page number to read
 *
 * Hardware ECC for large page chips, require OOB to be read first. For this
 * ECC mode, the write_page method is re-used from ECC_HW. These methods
 * read/write ECC from the OOB area, unlike the ECC_HW_SYNDROME support with
 * multiple ECC steps, follows the "infix ECC" scheme and reads/writes ECC from
 * the data area, by overwriting the NAND manufacturer bad block markings.
 */
/* static */ int nand_read_page_hwecc_oob_first(struct mtd_info *mtd,
	struct nand_chip *chip, uint8_t *buf, int page)
{
	int i, eccsize = chip->ecc.size;
	int eccbytes = chip->ecc.bytes;
	int eccsteps = chip->ecc.steps;
	uint8_t *p = buf;
	uint8_t *ecc_code = chip->buffers->ecccode;
	uint32_t *eccpos = chip->ecc.layout->eccpos;
	uint8_t *ecc_calc = chip->buffers->ecccalc;

	/* Read the OOB area first */
	chip->cmdfunc(mtd, NAND_CMD_READOOB, 0, page);
	chip->read_buf(mtd, chip->oob_poi, mtd->oobsize);
	chip->cmdfunc(mtd, NAND_CMD_READ0, 0, page);

	for (i = 0; i < chip->ecc.total; i++)
		ecc_code[i] = chip->oob_poi[eccpos[i]];

	for (i = 0; eccsteps; eccsteps--, i += eccbytes, p += eccsize) {
		int stat;

		chip->ecc.hwctl(mtd, NAND_ECC_READ);
		chip->read_buf(mtd, p, eccsize);
		chip->ecc.calculate(mtd, p, &ecc_calc[i]);

		stat = chip->ecc.correct(mtd, p, &ecc_code[i], NULL);
		if (stat < 0)
			mtd->ecc_stats.failed++;
		else
			mtd->ecc_stats.corrected += stat;
	}
	return 0;
}

/**
 * nand_write_page_hwecc - [REPLACEABLE] hardware ECC based page write function
 * @mtd: mtd info structure
 * @chip: nand chip info structure
 * @buf: data buffer
 */
#ifdef CONFIG_MTD_WRITE
/* static */ void nand_write_page_hwecc(struct mtd_info *mtd, struct nand_chip *chip,
				  const uint8_t *buf)
{
	int i, eccsize = chip->ecc.size;
	int eccbytes = chip->ecc.bytes;
	int eccsteps = chip->ecc.steps;
	uint8_t *ecc_calc = chip->buffers->ecccalc;
	const uint8_t *p = buf;
	uint32_t *eccpos = chip->ecc.layout->eccpos;

	for (i = 0; eccsteps; eccsteps--, i += eccbytes, p += eccsize) {
		chip->ecc.hwctl(mtd, NAND_ECC_WRITE);
		chip->write_buf(mtd, p, eccsize);
		chip->ecc.calculate(mtd, p, &ecc_calc[i]);
	}

	for (i = 0; i < chip->ecc.total; i++)
		chip->oob_poi[eccpos[i]] = ecc_calc[i];

	chip->write_buf(mtd, chip->oob_poi, mtd->oobsize);
}
#endif

void nand_init_ecc_hw(struct mtd_info *mtd, struct nand_chip *chip)
{
	switch (chip->ecc.mode) {
	case NAND_ECC_HW_OOB_FIRST:
		/* Similar to NAND_ECC_HW, but a separate read_page handle */
		if (!chip->ecc.calculate || !chip->ecc.correct ||
		     !chip->ecc.hwctl) {
			pr_warn("No ECC functions supplied; "
				   "hardware ECC not possible\n");
			BUG();
		}
		if (!chip->ecc.read_page)
			chip->ecc.read_page = nand_read_page_hwecc_oob_first;

	case NAND_ECC_HW:
		/* Use standard hwecc read page function? */
		if (!chip->ecc.read_page)
			chip->ecc.read_page = nand_read_page_hwecc;
		if (!chip->ecc.write_page)
			chip->ecc.write_page = nand_write_page_hwecc;
		if (!chip->ecc.read_page_raw)
			chip->ecc.read_page_raw = nand_read_page_raw;
		if (!chip->ecc.write_page_raw)
			chip->ecc.write_page_raw = nand_write_page_raw;
		if (!chip->ecc.read_oob)
			chip->ecc.read_oob = nand_read_oob_std;
		if (!chip->ecc.write_oob)
			chip->ecc.write_oob = nand_write_oob_std;

	default:
		break;
	}
}
