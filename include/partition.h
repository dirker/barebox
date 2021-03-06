#ifndef __PARTITION_H
#define __PARTITION_H

struct device_d;

struct partition {
        int num;

	int flags;

        unsigned long offset;

        struct device_d *physdev;
        struct device_d device;

        char name[16];
	struct cdev cdev;
};

#endif /* __PARTITION_H */

