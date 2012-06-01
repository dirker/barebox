#include <common.h>
#include <malloc.h>
#include <linux/list.h>
#include <linux/mtd/mtd.h>

#include "mtd.h"

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
