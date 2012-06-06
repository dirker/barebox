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
#include <module.h>

#include "nand.h"

static int nand_do_write_oob(struct mtd_info *mtd, loff_t to,
			     struct mtd_oob_ops *ops);

static int check_offs_len(struct mtd_info *mtd,
					loff_t ofs, uint64_t len)
{
	struct nand_chip *chip = mtd->priv;
	int ret = 0;

	/* Start address must align on block boundary */
	if (ofs & ((1 << chip->phys_erase_shift) - 1)) {
		pr_debug("%s: unaligned address\n", __func__);
		ret = -EINVAL;
	}

	/* Length must align on block boundary */
	if (len & ((1 << chip->phys_erase_shift) - 1)) {
		pr_debug("%s: length not block aligned\n", __func__);
		ret = -EINVAL;
	}

	return ret;
}

/**
 * nand_write_buf - [DEFAULT] write buffer to chip
 * @mtd:	MTD device structure
 * @buf:	data buffer
 * @len:	number of bytes to write
 *
 * Default write function for 8bit buswith
 */
/* static */ void nand_write_buf(struct mtd_info *mtd, const uint8_t *buf, int len)
{
	int i;
	struct nand_chip *chip = mtd->priv;

	for (i = 0; i < len; i++)
		writeb(buf[i], chip->IO_ADDR_W);
}

/**
 * nand_verify_buf - [DEFAULT] Verify chip data against buffer
 * @mtd: MTD device structure
 * @buf: buffer containing the data to compare
 * @len: number of bytes to compare
 *
 * Default verify function for 8bit buswidth.
 */
/* static */ int nand_verify_buf(struct mtd_info *mtd, const uint8_t *buf, int len)
{
	int i;
	struct nand_chip *chip = mtd->priv;

	for (i = 0; i < len; i++)
		if (buf[i] != readb(chip->IO_ADDR_R))
			return -EFAULT;
	return 0;
}

/**
 * nand_write_buf16 - [DEFAULT] write buffer to chip
 * @mtd:	MTD device structure
 * @buf:	data buffer
 * @len:	number of bytes to write
 *
 * Default write function for 16bit buswith
 */
/* static */ void nand_write_buf16(struct mtd_info *mtd, const uint8_t *buf, int len)
{
	int i;
	struct nand_chip *chip = mtd->priv;
	u16 *p = (u16 *) buf;
	len >>= 1;

	for (i = 0; i < len; i++)
		writew(p[i], chip->IO_ADDR_W);

}

/**
 * nand_verify_buf16 - [DEFAULT] Verify chip data against buffer
 * @mtd: MTD device structure
 * @buf: buffer containing the data to compare
 * @len: number of bytes to compare
 *
 * Default verify function for 16bit buswidth.
 */
/* static */ int nand_verify_buf16(struct mtd_info *mtd, const uint8_t *buf, int len)
{
	int i;
	struct nand_chip *chip = mtd->priv;
	u16 *p = (u16 *) buf;
	len >>= 1;

	for (i = 0; i < len; i++)
		if (p[i] != readw(chip->IO_ADDR_R))
			return -EFAULT;

	return 0;
}

/**
 * nand_block_bad - [DEFAULT] Read bad block marker from the chip
 * @mtd: MTD device structure
 * @ofs: offset from device start
 * @getchip: 0, if the chip is already selected
 *
 * Check, if the block is bad.
 */
/* static */ int nand_block_bad(struct mtd_info *mtd, loff_t ofs, int getchip)
{
	int page, chipnr, res = 0, i = 0;
	struct nand_chip *chip = mtd->priv;
	u16 bad;

	if (chip->bbt_options & NAND_BBT_SCANLASTPAGE)
		ofs += mtd->erasesize - mtd->writesize;

	page = (int)(ofs >> chip->page_shift) & chip->pagemask;

	if (getchip) {
		chipnr = (int)(ofs >> chip->chip_shift);

		/* Select the NAND device */
		chip->select_chip(mtd, chipnr);
	}

	do {
		if (chip->options & NAND_BUSWIDTH_16) {
			chip->cmdfunc(mtd, NAND_CMD_READOOB,
					chip->badblockpos & 0xFE, page);
			bad = cpu_to_le16(chip->read_word(mtd));
			if (chip->badblockpos & 0x1)
				bad >>= 8;
			else
				bad &= 0xFF;
		} else {
			chip->cmdfunc(mtd, NAND_CMD_READOOB, chip->badblockpos,
					page);
			bad = chip->read_byte(mtd);
		}

		if (likely(chip->badblockbits == 8))
			res = bad != 0xFF;
		else
			res = hweight8(bad) < chip->badblockbits;
		ofs += mtd->writesize;
		page = (int)(ofs >> chip->page_shift) & chip->pagemask;
		i++;
	} while (!res && i < 2 && (chip->bbt_options & NAND_BBT_SCAN2NDPAGE));

	return res;
}

/**
 * nand_default_block_markbad - [DEFAULT] mark a block bad
 * @mtd: MTD device structure
 * @ofs: offset from device start
 *
 * This is the default implementation, which can be overridden by a hardware
 * specific driver. We try operations in the following order, according to our
 * bbt_options (NAND_BBT_NO_OOB_BBM and NAND_BBT_USE_FLASH):
 *  (1) erase the affected block, to allow OOB marker to be written cleanly
 *  (2) update in-memory BBT
 *  (3) write bad block marker to OOB area of affected block
 *  (4) update flash-based BBT
 * Note that we retain the first error encountered in (3) or (4), finish the
 * procedures, and dump the error in the end.
*/
/* static */ int nand_default_block_markbad(struct mtd_info *mtd, loff_t ofs)
{
	struct nand_chip *chip = mtd->priv;
	uint8_t buf[2] = { 0, 0 };
	int block, res, ret = 0, i = 0;
	int write_oob = !(chip->bbt_options & NAND_BBT_NO_OOB_BBM);

	if (write_oob) {
		struct erase_info einfo;

		/* Attempt erase before marking OOB */
		memset(&einfo, 0, sizeof(einfo));
		einfo.mtd = mtd;
		einfo.addr = ofs;
		einfo.len = 1 << chip->phys_erase_shift;
		nand_erase_nand(mtd, &einfo, 0);
	}

	/* Get block number */
	block = (int)(ofs >> chip->bbt_erase_shift);
	/* Mark block bad in memory-based BBT */
	if (chip->bbt)
		chip->bbt[block >> 2] |= 0x01 << ((block & 0x03) << 1);

	/* Write bad block marker to OOB */
	if (write_oob) {
		struct mtd_oob_ops ops;
		loff_t wr_ofs = ofs;

		ops.datbuf = NULL;
		ops.oobbuf = buf;
		ops.ooboffs = chip->badblockpos;
		if (chip->options & NAND_BUSWIDTH_16) {
			ops.ooboffs &= ~0x01;
			ops.len = ops.ooblen = 2;
		} else {
			ops.len = ops.ooblen = 1;
		}
		ops.mode = MTD_OPS_PLACE_OOB;

		/* Write to first/last page(s) if necessary */
		if (chip->bbt_options & NAND_BBT_SCANLASTPAGE)
			wr_ofs += mtd->erasesize - mtd->writesize;
		do {
			res = nand_do_write_oob(mtd, wr_ofs, &ops);
			if (!ret)
				ret = res;

			i++;
			wr_ofs += mtd->writesize;
		} while ((chip->bbt_options & NAND_BBT_SCAN2NDPAGE) && i < 2);
	}

	/* Update flash-based bad block table */
	if (chip->bbt_options & NAND_BBT_USE_FLASH) {
		res = nand_update_bbt(mtd, ofs);
		if (!ret)
			ret = res;
	}

	if (!ret)
		mtd->ecc_stats.badblocks++;

	return ret;
}

/**
 * nand_check_wp - [GENERIC] check if the chip is write protected
 * @mtd:	MTD device structure
 * Check, if the device is write protected
 *
 * The function expects, that the device is already selected
 */
static int nand_check_wp(struct mtd_info *mtd)
{
	struct nand_chip *chip = mtd->priv;

	/* Broken xD cards report WP despite being writable */
	if (chip->options & NAND_BROKEN_XD)
		return 0;

	/* Check the WP bit */
	chip->cmdfunc(mtd, NAND_CMD_STATUS, -1, -1);
	return (chip->read_byte(mtd) & NAND_STATUS_WP) ? 0 : 1;
}

/**
 * nand_write_oob_std - [REPLACEABLE] the most common OOB data write function
 * @mtd: mtd info structure
 * @chip: nand chip info structure
 * @page: page number to write
 */
/* static */ int nand_write_oob_std(struct mtd_info *mtd, struct nand_chip *chip,
			      int page)
{
	int status = 0;
	const uint8_t *buf = chip->oob_poi;
	int length = mtd->oobsize;

	chip->cmdfunc(mtd, NAND_CMD_SEQIN, mtd->writesize, page);
	chip->write_buf(mtd, buf, length);
	/* Send command to program the OOB data */
	chip->cmdfunc(mtd, NAND_CMD_PAGEPROG, -1, -1);

	status = chip->waitfunc(mtd, chip);

	return status & NAND_STATUS_FAIL ? -EIO : 0;
}

/**
 * nand_write_page_raw - [INTERN] raw page write function
 * @mtd: mtd info structure
 * @chip: nand chip info structure
 * @buf: data buffer
 *
 * Not for syndrome calculating ECC controllers, which use a special oob layout.
 */
/* static */ void nand_write_page_raw(struct mtd_info *mtd, struct nand_chip *chip,
				const uint8_t *buf)
{
	chip->write_buf(mtd, buf, mtd->writesize);
	chip->write_buf(mtd, chip->oob_poi, mtd->oobsize);
}

/**
 * nand_write_page - [REPLACEABLE] write one page
 * @mtd: MTD device structure
 * @chip: NAND chip descriptor
 * @buf: the data to write
 * @page: page number to write
 * @cached: cached programming
 * @raw: use _raw version of write_page
 */
/* static */ int nand_write_page(struct mtd_info *mtd, struct nand_chip *chip,
			   const uint8_t *buf, int page, int cached, int raw)
{
	int status;

	chip->cmdfunc(mtd, NAND_CMD_SEQIN, 0x00, page);

	if (unlikely(raw))
		chip->ecc.write_page_raw(mtd, chip, buf);
	else
		chip->ecc.write_page(mtd, chip, buf);

	/*
	 * Cached progamming disabled for now. Not sure if it's worth the
	 * trouble. The speed gain is not very impressive. (2.3->2.6Mib/s).
	 */
	cached = 0;

	if (!cached || !(chip->options & NAND_CACHEPRG)) {

		chip->cmdfunc(mtd, NAND_CMD_PAGEPROG, -1, -1);
		status = chip->waitfunc(mtd, chip);
		/*
		 * See if operation failed and additional status checks are
		 * available.
		 */
		if ((status & NAND_STATUS_FAIL) && (chip->errstat))
			status = chip->errstat(mtd, chip, FL_WRITING, status,
					       page);

		if (status & NAND_STATUS_FAIL)
			return -EIO;
	} else {
		chip->cmdfunc(mtd, NAND_CMD_CACHEDPROG, -1, -1);
		status = chip->waitfunc(mtd, chip);
	}

#ifdef CONFIG_MTD_NAND_VERIFY_WRITE
	/* Send command to read back the data */
	chip->cmdfunc(mtd, NAND_CMD_READ0, 0, page);

	if (chip->verify_buf(mtd, buf, mtd->writesize))
		return -EIO;
#endif
	return 0;
}

/**
 * nand_fill_oob - [INTERN] Transfer client buffer to oob
 * @mtd: MTD device structure
 * @oob: oob data buffer
 * @len: oob data write length
 * @ops: oob ops structure
 */
static uint8_t *nand_fill_oob(struct mtd_info *mtd, uint8_t *oob, size_t len,
			      struct mtd_oob_ops *ops)
{
	struct nand_chip *chip = mtd->priv;

	/*
	 * Initialise to all 0xFF, to avoid the possibility of left over OOB
	 * data from a previous OOB read.
	 */
	memset(chip->oob_poi, 0xff, mtd->oobsize);

	switch (ops->mode) {
	case MTD_OPS_PLACE_OOB:
	case MTD_OPS_RAW:
		memcpy(chip->oob_poi + ops->ooboffs, oob, len);
		return oob + len;

	case MTD_OPS_AUTO_OOB: {
		struct nand_oobfree *free = chip->ecc.layout->oobfree;
		uint32_t boffs = 0, woffs = ops->ooboffs;
		size_t bytes = 0;

		for (; free->length && len; free++, len -= bytes) {
			/* Write request not from offset 0? */
			if (unlikely(woffs)) {
				if (woffs >= free->length) {
					woffs -= free->length;
					continue;
				}
				boffs = free->offset + woffs;
				bytes = min_t(size_t, len,
					      (free->length - woffs));
				woffs = 0;
			} else {
				bytes = min_t(size_t, len, free->length);
				boffs = free->offset;
			}
			memcpy(chip->oob_poi + boffs, oob, bytes);
			oob += bytes;
		}
		return oob;
	}
	default:
		BUG();
	}
	return NULL;
}

#define NOTALIGNED(x)	(x & (chip->subpagesize - 1)) != 0

/**
 * nand_do_write_ops - [INTERN] NAND write with ECC
 * @mtd: MTD device structure
 * @to: offset to write to
 * @ops: oob operations description structure
 *
 * NAND write with ECC.
 */
/* static */ int nand_do_write_ops(struct mtd_info *mtd, loff_t to,
			     struct mtd_oob_ops *ops)
{
	int chipnr, realpage, page, blockmask, column;
	struct nand_chip *chip = mtd->priv;
	uint32_t writelen = ops->len;

	uint32_t oobwritelen = ops->ooblen;
	uint32_t oobmaxlen = ops->mode == MTD_OPS_AUTO_OOB ?
				mtd->oobavail : mtd->oobsize;

	uint8_t *oob = ops->oobbuf;
	uint8_t *buf = ops->datbuf;
	int ret, subpage;

	ops->retlen = 0;
	if (!writelen)
		return 0;

	/* Reject writes, which are not page aligned */
	if (NOTALIGNED(to) || NOTALIGNED(ops->len)) {
		pr_notice("%s: attempt to write non page aligned data\n",
			   __func__);
		return -EINVAL;
	}

	column = to & (mtd->writesize - 1);
	subpage = column || (writelen & (mtd->writesize - 1));

	if (subpage && oob)
		return -EINVAL;

	chipnr = (int)(to >> chip->chip_shift);
	chip->select_chip(mtd, chipnr);

	/* Check, if it is write protected */
	if (nand_check_wp(mtd))
		return -EIO;

	realpage = (int)(to >> chip->page_shift);
	page = realpage & chip->pagemask;
	blockmask = (1 << (chip->phys_erase_shift - chip->page_shift)) - 1;

	/* Invalidate the page cache, when we write to the cached page */
	if (to <= (chip->pagebuf << chip->page_shift) &&
	    (chip->pagebuf << chip->page_shift) < (to + ops->len))
		chip->pagebuf = -1;

	/* Don't allow multipage oob writes with offset */
	if (oob && ops->ooboffs && (ops->ooboffs + ops->ooblen > oobmaxlen))
		return -EINVAL;

	while (1) {
		int bytes = mtd->writesize;
		int cached = writelen > bytes && page != blockmask;
		uint8_t *wbuf = buf;

		/* Partial page write? */
		if (unlikely(column || writelen < (mtd->writesize - 1))) {
			cached = 0;
			bytes = min_t(int, bytes - column, (int) writelen);
			chip->pagebuf = -1;
			memset(chip->buffers->databuf, 0xff, mtd->writesize);
			memcpy(&chip->buffers->databuf[column], buf, bytes);
			wbuf = chip->buffers->databuf;
		}

		if (unlikely(oob)) {
			size_t len = min(oobwritelen, oobmaxlen);
			oob = nand_fill_oob(mtd, oob, len, ops);
			oobwritelen -= len;
		} else {
			/* We still need to erase leftover OOB data */
			memset(chip->oob_poi, 0xff, mtd->oobsize);
		}

		ret = chip->write_page(mtd, chip, wbuf, page, cached,
				       (ops->mode == MTD_OPS_RAW));
		if (ret)
			break;

		writelen -= bytes;
		if (!writelen)
			break;

		column = 0;
		buf += bytes;
		realpage++;

		page = realpage & chip->pagemask;
		/* Check, if we cross a chip boundary */
		if (!page) {
			chipnr++;
			chip->select_chip(mtd, -1);
			chip->select_chip(mtd, chipnr);
		}
	}

	ops->retlen = ops->len - writelen;
	if (unlikely(oob))
		ops->oobretlen = ops->ooblen;
	return ret;
}

/**
 * nand_write - [MTD Interface] NAND write with ECC
 * @mtd: MTD device structure
 * @to: offset to write to
 * @len: number of bytes to write
 * @retlen: pointer to variable to store the number of written bytes
 * @buf: the data to write
 *
 * NAND write with ECC.
 */
/* static */ int nand_write(struct mtd_info *mtd, loff_t to, size_t len,
			  size_t *retlen, const uint8_t *buf)
{
	struct mtd_oob_ops ops;
	int ret;

	ops.len = len;
	ops.datbuf = (uint8_t *)buf;
	ops.oobbuf = NULL;
	ops.mode = 0;
	ret = nand_do_write_ops(mtd, to, &ops);
	*retlen = ops.retlen;
	return ret;
}

/**
 * nand_do_write_oob - [MTD Interface] NAND write out-of-band
 * @mtd: MTD device structure
 * @to: offset to write to
 * @ops: oob operation description structure
 *
 * NAND write out-of-band.
 */
/* static */ int nand_do_write_oob(struct mtd_info *mtd, loff_t to,
			     struct mtd_oob_ops *ops)
{
	int chipnr, page, status, len;
	struct nand_chip *chip = mtd->priv;

	MTD_DEBUG(MTD_DEBUG_LEVEL3, "%s: to = 0x%08x, len = %i\n",
			 __func__, (unsigned int)to, (int)ops->ooblen);

	if (ops->mode == MTD_OPS_AUTO_OOB)
		len = chip->ecc.layout->oobavail;
	else
		len = mtd->oobsize;

	/* Do not allow write past end of page */
	if ((ops->ooboffs + ops->ooblen) > len) {
		MTD_DEBUG(MTD_DEBUG_LEVEL0, "%s: attempt to write past end of page\n",
				__func__);
		return -EINVAL;
	}

	if (unlikely(ops->ooboffs >= len)) {
		MTD_DEBUG(MTD_DEBUG_LEVEL0, "%s: attempt to start write outside oob\n",
				__func__);
		return -EINVAL;
	}

	/* Do not allow write past end of device */
	if (unlikely(to >= mtd->size ||
		     ops->ooboffs + ops->ooblen >
			((mtd->size >> chip->page_shift) -
			 (to >> chip->page_shift)) * len)) {
		MTD_DEBUG(MTD_DEBUG_LEVEL0, "%s: attempt to write beyond end of device\n",
				__func__);
		return -EINVAL;
	}

	chipnr = (int)(to >> chip->chip_shift);
	chip->select_chip(mtd, chipnr);

	/* Shift to get page */
	page = (int)(to >> chip->page_shift);

	/*
	 * Reset the chip. Some chips (like the Toshiba TC5832DC found in one
	 * of my DiskOnChip 2000 test units) will clear the whole data page too
	 * if we don't do this. I have no clue why, but I seem to have 'fixed'
	 * it in the doc2000 driver in August 1999.  dwmw2.
	 */
	chip->cmdfunc(mtd, NAND_CMD_RESET, -1, -1);

	/* Check, if it is write protected */
	if (nand_check_wp(mtd))
		return -EROFS;

	/* Invalidate the page cache, if we write to the cached page */
	if (page == chip->pagebuf)
		chip->pagebuf = -1;

	nand_fill_oob(mtd, ops->oobbuf, ops->ooblen, ops);

	if (ops->mode == MTD_OPS_RAW)
		status = chip->ecc.write_oob_raw(mtd, chip, page & chip->pagemask);
	else
		status = chip->ecc.write_oob(mtd, chip, page & chip->pagemask);

	if (status)
		return status;

	ops->oobretlen = ops->ooblen;

	return 0;
}

/**
 * nand_write_oob - [MTD Interface] NAND write data and/or out-of-band
 * @mtd: MTD device structure
 * @to: offset to write to
 * @ops: oob operation description structure
 */
/* static */ int nand_write_oob(struct mtd_info *mtd, loff_t to,
			  struct mtd_oob_ops *ops)
{
	int ret = -ENOSYS;

	ops->retlen = 0;

	/* Do not allow writes past end of device */
	if (ops->datbuf && (to + ops->len) > mtd->size) {
		MTD_DEBUG(MTD_DEBUG_LEVEL0, "%s: attempt to write beyond end of device\n",
				__func__);
		return -EINVAL;
	}

	switch (ops->mode) {
	case MTD_OPS_PLACE_OOB:
	case MTD_OPS_AUTO_OOB:
	case MTD_OPS_RAW:
		break;

	default:
		goto out;
	}

	if (!ops->datbuf)
		ret = nand_do_write_oob(mtd, to, ops);
	else
		ret = nand_do_write_ops(mtd, to, ops);

out:
	return ret;
}

/**
 * single_erase_cmd - [GENERIC] NAND standard block erase command function
 * @mtd: MTD device structure
 * @page: the page address of the block which will be erased
 *
 * Standard erase command for NAND chips.
 */
/* static */ void single_erase_cmd(struct mtd_info *mtd, int page)
{
	struct nand_chip *chip = mtd->priv;
	/* Send commands to erase a block */
	chip->cmdfunc(mtd, NAND_CMD_ERASE1, -1, page);
	chip->cmdfunc(mtd, NAND_CMD_ERASE2, -1, -1);
}

/**
 * multi_erase_cmd - [GENERIC] AND specific block erase command function
 * @mtd: MTD device structure
 * @page: the page address of the block which will be erased
 *
 * AND multi block erase command function. Erase 4 consecutive blocks.
 */
/* static */ void multi_erase_cmd(struct mtd_info *mtd, int page)
{
	struct nand_chip *chip = mtd->priv;
	/* Send commands to erase a block */
	chip->cmdfunc(mtd, NAND_CMD_ERASE1, -1, page++);
	chip->cmdfunc(mtd, NAND_CMD_ERASE1, -1, page++);
	chip->cmdfunc(mtd, NAND_CMD_ERASE1, -1, page++);
	chip->cmdfunc(mtd, NAND_CMD_ERASE1, -1, page);
	chip->cmdfunc(mtd, NAND_CMD_ERASE2, -1, -1);
}

/**
 * nand_erase - [MTD Interface] erase block(s)
 * @mtd: MTD device structure
 * @instr: erase instruction
 *
 * Erase one ore more blocks.
 */
/* static */ int nand_erase(struct mtd_info *mtd, struct erase_info *instr)
{
	return nand_erase_nand(mtd, instr, 0);
}

#define BBT_PAGE_MASK	0xffffff3f
/**
 * nand_erase_nand - [INTERN] erase block(s)
 * @mtd: MTD device structure
 * @instr: erase instruction
 * @allowbbt: allow erasing the bbt area
 *
 * Erase one ore more blocks.
 */
/* static */ int nand_erase_nand(struct mtd_info *mtd, struct erase_info *instr,
		    int allowbbt)
{
	int page, status, pages_per_block, ret, chipnr;
	struct nand_chip *chip = mtd->priv;
	loff_t rewrite_bbt[NAND_MAX_CHIPS] = {0};
	unsigned int bbt_masked_page = 0xffffffff;
	loff_t len;

	MTD_DEBUG(MTD_DEBUG_LEVEL3, "%s: start = 0x%012llx, len = %llu\n",
			__func__, (unsigned long long)instr->addr,
			(unsigned long long)instr->len);

	if (check_offs_len(mtd, instr->addr, instr->len))
		return -EINVAL;

	/* Shift to get first page */
	page = (int)(instr->addr >> chip->page_shift);
	chipnr = (int)(instr->addr >> chip->chip_shift);

	/* Calculate pages in each block */
	pages_per_block = 1 << (chip->phys_erase_shift - chip->page_shift);

	/* Select the NAND device */
	chip->select_chip(mtd, chipnr);

	/* Check, if it is write protected */
	if (nand_check_wp(mtd)) {
		MTD_DEBUG(MTD_DEBUG_LEVEL3, "%s: device is write protected!\n",
				__func__);
		instr->state = MTD_ERASE_FAILED;
		goto erase_exit;
	}

	/*
	 * If BBT requires refresh, set the BBT page mask to see if the BBT
	 * should be rewritten. Otherwise the mask is set to 0xffffffff which
	 * can not be matched. This is also done when the bbt is actually
	 * erased to avoid recursive updates.
	 */
	if (chip->options & BBT_AUTO_REFRESH && !allowbbt)
		bbt_masked_page = chip->bbt_td->pages[chipnr] & BBT_PAGE_MASK;

	/* Loop through the pages */
	len = instr->len;

	instr->state = MTD_ERASING;

	while (len) {
		/* Check if we have a bad block, we do not erase bad blocks! */
		if (nand_block_checkbad(mtd, ((loff_t) page) <<
					chip->page_shift, 0, allowbbt)) {
			pr_warn("%s: attempt to erase a bad block at page 0x%08x\n",
				    __func__, page);
			instr->state = MTD_ERASE_FAILED;
			goto erase_exit;
		}

		/*
		 * Invalidate the page cache, if we erase the block which
		 * contains the current cached page.
		 */
		if (page <= chip->pagebuf && chip->pagebuf <
		    (page + pages_per_block))
			chip->pagebuf = -1;

		chip->erase_cmd(mtd, page & chip->pagemask);

		status = chip->waitfunc(mtd, chip);

		/*
		 * See if operation failed and additional status checks are
		 * available
		 */
		if ((status & NAND_STATUS_FAIL) && (chip->errstat))
			status = chip->errstat(mtd, chip, FL_ERASING,
					       status, page);

		/* See if block erase succeeded */
		if (status & NAND_STATUS_FAIL) {
			MTD_DEBUG(MTD_DEBUG_LEVEL0, "%s: failed erase, page 0x%08x\n",
					__func__, page);
			instr->state = MTD_ERASE_FAILED;
			instr->fail_addr =
				((loff_t)page << chip->page_shift);
			goto erase_exit;
		}

		/*
		 * If BBT requires refresh, set the BBT rewrite flag to the
		 * page being erased.
		 */
		if (bbt_masked_page != 0xffffffff &&
		    (page & BBT_PAGE_MASK) == bbt_masked_page)
			    rewrite_bbt[chipnr] =
					((loff_t)page << chip->page_shift);

		/* Increment page address and decrement length */
		len -= (1 << chip->phys_erase_shift);
		page += pages_per_block;

		/* Check, if we cross a chip boundary */
		if (len && !(page & chip->pagemask)) {
			chipnr++;
			chip->select_chip(mtd, -1);
			chip->select_chip(mtd, chipnr);

			/*
			 * If BBT requires refresh and BBT-PERCHIP, set the BBT
			 * page mask to see if this BBT should be rewritten.
			 */
			if (bbt_masked_page != 0xffffffff &&
			    (chip->bbt_td->options & NAND_BBT_PERCHIP))
				bbt_masked_page = chip->bbt_td->pages[chipnr] &
					BBT_PAGE_MASK;
		}
	}
	instr->state = MTD_ERASE_DONE;

erase_exit:

	ret = instr->state == MTD_ERASE_DONE ? 0 : -EIO;

	/* Do call back function */
	if (!ret)
		mtd_erase_callback(instr);

	/*
	 * If BBT requires refresh and erase was successful, rewrite any
	 * selected bad block tables.
	 */
	if (bbt_masked_page == 0xffffffff || ret)
		return ret;

	for (chipnr = 0; chipnr < chip->numchips; chipnr++) {
		if (!rewrite_bbt[chipnr])
			continue;
		/* Update the BBT for chip */
		MTD_DEBUG(MTD_DEBUG_LEVEL0, "%s: nand_update_bbt (%d:0x%0llx 0x%0x)\n",
				__func__, chipnr, rewrite_bbt[chipnr],
				chip->bbt_td->pages[chipnr]);
		nand_update_bbt(mtd, rewrite_bbt[chipnr]);
	}

	/* Return more or less happy */
	return ret;
}

/**
 * nand_block_markbad - [MTD Interface] Mark block at the given offset as bad
 * @mtd: MTD device structure
 * @ofs: offset relative to mtd start
 */
/* static */ int nand_block_markbad(struct mtd_info *mtd, loff_t ofs)
{
	struct nand_chip *chip = mtd->priv;
	int ret;

	ret = nand_block_isbad(mtd, ofs);
	if (ret) {
		/* If it was bad already, return success and do nothing */
		if (ret > 0)
			return 0;
		return ret;
	}

	return chip->block_markbad(mtd, ofs);
}
