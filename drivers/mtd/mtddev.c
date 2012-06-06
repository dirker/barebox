/*
 * (C) Copyright 2005
 * 2N Telekomunikace, a.s. <www.2n.cz>
 * Ladislav Michl <michl@2n.cz>
 *
 * See file CREDITS for list of people who contributed to this
 * project.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <common.h>
#include <linux/mtd/nand.h>
#include <linux/mtd/mtd.h>
#include <init.h>
#include <xfuncs.h>
#include <driver.h>
#include <malloc.h>
#include <ioctl.h>
#include <nand.h>
#include <errno.h>

#include "mtd.h"

struct mtddev {
	struct cdev cdev;
	struct mtd_info *mtd;
};

static ssize_t mtddev_read(struct cdev *cdev, void* buf, size_t count,
			  ulong offset, ulong flags)
{
	struct mtd_info *mtd = cdev->priv;
	size_t retlen;
	int ret;

	debug("mtd_read: 0x%08lx 0x%08x\n", offset, count);

	ret = mtd_read(mtd, offset, count, &retlen, buf);

	if(ret) {
		printf("err %d\n", ret);
		return ret;
	}
	return retlen;
}

#define NOTALIGNED(x) (x & (mtd->writesize - 1)) != 0
#define MTDPGALG(x) ((x) & ~(mtd->writesize - 1))

#ifdef CONFIG_MTD_WRITE
static int all_ff(const void *buf, int len)
{
	int i;
	const uint8_t *p = buf;

	for (i = 0; i < len; i++)
		if (p[i] != 0xFF)
			return 0;
	return 1;
}

static ssize_t mtddev_write(struct cdev* cdev, const void *buf, size_t _count,
			  ulong offset, ulong flags)
{
	struct mtd_info *mtd = cdev->priv;
	size_t retlen, now;
	int ret = 0;
	void *wrbuf = NULL;
	size_t count = _count;

	if (NOTALIGNED(offset)) {
		printf("offset 0x%0lx not page aligned\n", offset);
		return -EINVAL;
	}

	dev_dbg(cdev->dev, "write: 0x%08lx 0x%08x\n", offset, count);
	while (count) {
		now = count > mtd->writesize ? mtd->writesize : count;

		if (NOTALIGNED(now)) {
			dev_dbg(cdev->dev, "not aligned: %d %ld\n",
				mtd->writesize,
				(offset % mtd->writesize));
			wrbuf = xmalloc(mtd->writesize);
			memset(wrbuf, 0xff, mtd->writesize);
			memcpy(wrbuf + (offset % mtd->writesize), buf, now);
			if (!all_ff(wrbuf, mtd->writesize))
				ret = mtd_write(mtd, MTDPGALG(offset),
						  mtd->writesize, &retlen,
						  wrbuf);
			free(wrbuf);
		} else {
			if (!all_ff(buf, mtd->writesize))
				ret = mtd_write(mtd, offset, now, &retlen,
						  buf);
			dev_dbg(cdev->dev,
				"offset: 0x%08lx now: 0x%08x retlen: 0x%08x\n",
				offset, now, retlen);
		}
		if (ret)
			goto out;

		offset += now;
		count -= now;
		buf += now;
	}

out:
	return ret ? ret : _count;
}
#endif

int mtddev_ioctl(struct cdev *cdev, int request, void *buf)
{
	int ret = 0;
	struct mtd_info *mtd = cdev->priv;
	struct mtd_info_user *user = buf;
#if (defined(CONFIG_NAND_ECC_HW) || defined(CONFIG_NAND_ECC_SOFT))
	struct mtd_ecc_stats *ecc = buf;
#endif
	struct region_info_user *reg = buf;

	switch (request) {
	case MEMGETBADBLOCK:
		dev_dbg(cdev->dev, "MEMGETBADBLOCK: 0x%08lx\n", (off_t)buf);
		ret = mtd_block_isbad(mtd, (off_t)buf);
		break;
#ifdef CONFIG_MTD_WRITE
	case MEMSETBADBLOCK:
		dev_dbg(cdev->dev, "MEMSETBADBLOCK: 0x%08lx\n", (off_t)buf);
		ret = mtd_block_markbad(mtd, (off_t)buf);
		break;
#endif
	case MEMGETINFO:
		user->type	= mtd->type;
		user->flags	= mtd->flags;
		user->size	= mtd->size;
		user->erasesize	= mtd->erasesize;
		user->oobsize	= mtd->oobsize;
		user->mtd	= mtd;
		break;
#if (defined(CONFIG_NAND_ECC_HW) || defined(CONFIG_NAND_ECC_SOFT))
	case ECCGETSTATS:
		ecc->corrected = mtd->ecc_stats.corrected;
		ecc->failed = mtd->ecc_stats.failed;
		ecc->badblocks = mtd->ecc_stats.badblocks;
		ecc->bbtblocks = mtd->ecc_stats.bbtblocks;
		break;
#endif
	case MEMGETREGIONINFO:
		if (cdev->mtd) {
			reg->offset = cdev->offset;
			reg->erasesize = cdev->mtd->erasesize;
			reg->numblocks = cdev->size/reg->erasesize;
			reg->regionindex = cdev->mtd->index;
		}
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

#ifdef CONFIG_MTD_WRITE
static ssize_t mtddev_erase(struct cdev *cdev, size_t count, unsigned long offset)
{
	struct mtd_info *mtd = cdev->priv;
	struct erase_info erase;
	int ret;

	memset(&erase, 0, sizeof(erase));
	erase.mtd = mtd;
	erase.addr = offset;
	erase.len = mtd->erasesize;

	while (count > 0) {
		dev_dbg(cdev->dev, "erase %llu %llu\n", erase.addr, erase.len);

		ret = mtd_block_isbad(mtd, erase.addr);
		if (ret > 0) {
			printf("Skipping bad block at %#0llx\n", erase.addr);
		} else {
			ret = mtd_erase(mtd, &erase);
			if (ret)
				return ret;
		}

		erase.addr += mtd->erasesize;
		count -= count > mtd->erasesize ? mtd->erasesize : count;
	}

	return 0;
}
#endif

static struct file_operations mtddev_ops = {
	.read   = mtddev_read,
#ifdef CONFIG_MTD_WRITE
	.write  = mtddev_write,
	.erase  = mtddev_erase,
#endif
	.ioctl  = mtddev_ioctl,
	.lseek  = dev_lseek_default,
};

static int mtddev_add(struct mtd_info *mtd, char *devname)
{
	struct mtddev *mtddev;

	mtddev = xzalloc(sizeof(*mtddev));
	mtddev->mtd = mtd;

	mtddev->cdev.ops = &mtddev_ops;
	mtddev->cdev.size = mtd->size;
	mtddev->cdev.name = asprintf("%s%d", devname, mtd->class_dev.id);
	mtddev->cdev.priv = mtd;
	mtddev->cdev.dev = &mtd->class_dev;
	mtddev->cdev.mtd = mtd;

	devfs_create(&mtddev->cdev);
	return 0;
}

static struct mtddev_hook mtddev_hook = {
	.add_mtd_device = mtddev_add,
};

static int __init mtddev_init(void)
{
	mtdcore_add_hook(&mtddev_hook);
	return 0;
}
coredevice_initcall(mtddev_init);
