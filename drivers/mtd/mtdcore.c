#include <common.h>
#include <malloc.h>
#include <linux/list.h>
#include <linux/mtd/mtd.h>

#include "mtd.h"

/*
 * Erase is an asynchronous operation.  Device drivers are supposed
 * to call instr->callback() whenever the operation completes, even
 * if it completes with a failure.
 * Callers are supposed to pass a callback function and wait for it
 * to be called before writing to the block.
 */
#ifdef CONFIG_MTD_WRITE
int mtd_erase(struct mtd_info *mtd, struct erase_info *instr)
{
	if (instr->addr > mtd->size || instr->len > mtd->size - instr->addr)
		return -EINVAL;
	if (!(mtd->flags & MTD_WRITEABLE))
		return -EROFS;
	instr->fail_addr = MTD_FAIL_ADDR_UNKNOWN;
	if (!instr->len) {
		instr->state = MTD_ERASE_DONE;
		mtd_erase_callback(instr);
		return 0;
	}
	return mtd->erase(mtd, instr);
}
EXPORT_SYMBOL_GPL(mtd_erase);
#endif

int mtd_read(struct mtd_info *mtd, loff_t from, size_t len, size_t *retlen,
	     u_char *buf)
{
	*retlen = 0;
	if (from < 0 || from > mtd->size || len > mtd->size - from)
		return -EINVAL;
	if (!len)
		return 0;
	return mtd->read(mtd, from, len, retlen, buf);
}
EXPORT_SYMBOL_GPL(mtd_read);

#ifdef CONFIG_MTD_WRITE
int mtd_write(struct mtd_info *mtd, loff_t to, size_t len, size_t *retlen,
	      const u_char *buf)
{
	*retlen = 0;
	if (to < 0 || to > mtd->size || len > mtd->size - to)
		return -EINVAL;
	if (!mtd->write || !(mtd->flags & MTD_WRITEABLE))
		return -EROFS;
	if (!len)
		return 0;
	return mtd->write(mtd, to, len, retlen, buf);
}
EXPORT_SYMBOL_GPL(mtd_write);
#endif

int mtd_block_isbad(struct mtd_info *mtd, loff_t ofs)
{
	if (!mtd->block_isbad)
		return 0;
	if (ofs < 0 || ofs > mtd->size)
		return -EINVAL;
	return mtd->block_isbad(mtd, ofs);
}
EXPORT_SYMBOL_GPL(mtd_block_isbad);

#ifdef CONFIG_MTD_WRITE
int mtd_block_markbad(struct mtd_info *mtd, loff_t ofs)
{
	if (!mtd->block_markbad)
		return -EOPNOTSUPP;
	if (ofs < 0 || ofs > mtd->size)
		return -EINVAL;
	if (!(mtd->flags & MTD_WRITEABLE))
		return -EROFS;
	return mtd->block_markbad(mtd, ofs);
}
EXPORT_SYMBOL_GPL(mtd_block_markbad);
#endif

static LIST_HEAD(mtd_register_hooks);

int add_mtd_device(struct mtd_info *mtd, char *devname)
{
	char str[16];
	struct mtddev_hook *hook;

	if (!devname)
		devname = "mtd";
	strcpy(mtd->class_dev.name, devname);
	mtd->class_dev.id = DEVICE_ID_DYNAMIC;
	register_device(&mtd->class_dev);

	if (IS_ENABLED(CONFIG_PARAMETER)) {
		sprintf(str, "%u", mtd->size);
		dev_add_param_fixed(&mtd->class_dev, "size", str);
		sprintf(str, "%u", mtd->erasesize);
		dev_add_param_fixed(&mtd->class_dev, "erasesize", str);
		sprintf(str, "%u", mtd->writesize);
		dev_add_param_fixed(&mtd->class_dev, "writesize", str);
		sprintf(str, "%u", mtd->oobsize);
		dev_add_param_fixed(&mtd->class_dev, "oobsize", str);
	}

	list_for_each_entry(hook, &mtd_register_hooks, hook)
		if (hook->add_mtd_device)
			hook->add_mtd_device(mtd, devname);

	return 0;
}

int del_mtd_device (struct mtd_info *mtd)
{
	struct mtddev_hook *hook;

	list_for_each_entry(hook, &mtd_register_hooks, hook)
		if (hook->del_mtd_device)
			hook->del_mtd_device(mtd);
	unregister_device(&mtd->class_dev);
	free(mtd->param_size.value);
	return 0;
}

void mtdcore_add_hook(struct mtddev_hook *hook)
{
	list_add(&hook->hook, &mtd_register_hooks);
}
