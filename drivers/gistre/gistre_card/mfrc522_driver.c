#include "mfrc522_driver.h"

#include <linux/errno.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/of.h>

#include "../mfrc522.h"

#include "commands/command.h"
#include "commands/utils.h"

MODULE_SOFTDEP("pre: mfrc522_emu");


#define MAX_PARAM_SIZE 50


static int nb_devices = 1;
module_param(nb_devices, int, S_IRUGO);

static bool quiet;
module_param(quiet, bool, S_IRUGO);

static char starting_debug_levels[MAX_PARAM_SIZE];
module_param_string(debug, starting_debug_levels, MAX_PARAM_SIZE, S_IRUGO);


static int major;
static struct mfrc522_driver_dev **mfrc522_driver_devs;


int mfrc522_driver_open(struct inode *inode, struct file *file)
{
	struct mfrc522_driver_dev *mfrc522_driver_dev;
	unsigned int i_major = imajor(inode);
	unsigned int i_minor = iminor(inode);

	if (i_major != major) {
		LOG("open: invalid major, got %d but expected %d, exiting",
			LOG_ERROR, LOG_ERROR, i_major, major);
		return -EINVAL;
	}

	LOG("open: major '%u', minor '%u'", LOG_EXTRA,
		mfrc522_driver_devs[i_minor]->log_level, i_major, i_minor);

	mfrc522_driver_dev = container_of(inode->i_cdev,
					  struct mfrc522_driver_dev, cdev);
	if (file->private_data != mfrc522_driver_dev) {
		file->private_data = mfrc522_driver_dev;
		return 0;
	} else
		return -EBUSY;
}

int mfrc522_driver_release(struct inode *inode,
	struct file *file /* unused */) {
	unsigned int i_major = imajor(inode);
	unsigned int i_minor = iminor(inode);

	if (i_major != major) {
		LOG("release: invalid major, got %d but expected %d, exiting",
			LOG_ERROR, LOG_ERROR, i_major, major);
		return -EINVAL;
	}

	LOG("release: major '%u', minor '%u'", LOG_EXTRA,
		mfrc522_driver_devs[i_minor]->log_level, i_major, i_minor);

	return 0;
}

ssize_t mfrc522_driver_read(struct file *file, char __user *buf,
	size_t len, loff_t *off /* unused */) {
	struct mfrc522_driver_dev *mfrc522_driver_dev;
	struct mfrc522_driver_data *driver_data;
	char data[INTERNAL_BUFFER_SIZE + 1];
	int i = 0;

	mfrc522_driver_dev = file->private_data;
	driver_data = mfrc522_driver_dev->virtual_dev->driver_data;

	// check if data exists
	if (!mfrc522_driver_dev->contains_data) {
		LOG("read: no data to read from internal buffer",
			LOG_WARN, mfrc522_driver_dev->log_level);
		return 0;
	}

	// Copying our internal buffer (int *) into a string (char *)
	memset(data, 0, INTERNAL_BUFFER_SIZE + 1);
	while (i < INTERNAL_BUFFER_SIZE) {
		data[i] = mfrc522_driver_dev->data[i];
		i++;
	}

	// Flush internal buffer
	if (copy_to_user(buf, data, INTERNAL_BUFFER_SIZE + 1)) {
		LOG("read: failed to copy data to user",
			LOG_ERROR, mfrc522_driver_dev->log_level);
		return -EFAULT;
	}

	// Reset internal buffer
	memset(mfrc522_driver_dev->data, 0, INTERNAL_BUFFER_SIZE + 1);
	mfrc522_driver_dev->contains_data = false;
	driver_data->bytes_read += 25;

	return INTERNAL_BUFFER_SIZE;
}

ssize_t mfrc522_driver_write(struct file *file, const char __user *user_buf,
	size_t len, loff_t *off /* unused */) {
	struct mfrc522_driver_dev *mfrc522_driver_dev;
	struct mfrc522_driver_data *driver_data;
	char buff[MAX_ACCEPTED_COMMAND_SIZE + 1];
	struct command *command;
	int res;

	mfrc522_driver_dev = file->private_data;
	driver_data = mfrc522_driver_dev->virtual_dev->driver_data;

	memset(buff, 0, MAX_ACCEPTED_COMMAND_SIZE + 1);

	if (copy_from_user(buff, user_buf, MAX_ACCEPTED_COMMAND_SIZE)) {
		LOG("write: failed to copy data from user",
			LOG_ERROR, mfrc522_driver_dev->log_level);
		return -EFAULT;
	}

	command = parse_command(buff, mfrc522_driver_dev->log_level);

	if (command == NULL)
		return -EFAULT;

	res = process_command(command, mfrc522_driver_dev);

	if (res < 0) {
		command_free(command);
		return -EFAULT;
	}
	if (command->command_type == COMMAND_WRITE)
		driver_data->bytes_written += 25;

	command_free(command);
	return len;
}

// Our own class

static struct class mfrc522_driver_class = {
	.name = "mfrc522_driver",
	.owner = THIS_MODULE,
};

/*
 * Device-specific attributes start here.
 */

static ssize_t bits_read_show(struct device *dev,
	struct device_attribute *attr, char *buf) {
	int ret;
	struct mfrc522_driver_data *dd;

	dd = (struct mfrc522_driver_data *) dev->driver_data;
	ret = snprintf(buf,
			8
			/* 32-bit number + \n */,
			"%u\n",
			/* must be multiplied by 8 since it is asked to display the number of bits */
			dd->bytes_read * 8);
	if (ret < 0) {
			pr_err("Failed to show nb_reads\n");
	}
	return ret;
}
/* Generates dev_attr_nb_reads */
DEVICE_ATTR_RO(bits_read);

static ssize_t bits_written_show(struct device *dev,
	struct device_attribute *attr, char *buf) {
	int ret;
	struct mfrc522_driver_data *dd;

	dd = (struct mfrc522_driver_data *) dev->driver_data;
	ret = snprintf(buf,
			8 /* 32-bit number + \n */,
			"%u\n",
			/* must be multiplied by 8 since it is asked to display the number of bits */
			dd->bytes_written * 8);
	if (ret < 0) {
			pr_err("Failed to show nb_writes\n");
	}
	return ret;
}

/* Generates dev_attr_nb_writes */
DEVICE_ATTR_RO(bits_written);

static struct attribute *mfrc522_driver_attrs[] = {
	&dev_attr_bits_read.attr,
	&dev_attr_bits_written.attr,
	NULL,
};

static const struct attribute_group mfrc522_driver_group = {
	.attrs = mfrc522_driver_attrs,
	/* is_visible() == NULL <==> always visible */
};

static const struct attribute_group *mfrc522_driver_groups[] = {
	&mfrc522_driver_group,
	NULL
};

static const struct file_operations mfrc522_driver_fops = {
	.owner	= THIS_MODULE,
	.read	= mfrc522_driver_read,
	.write	= mfrc522_driver_write,
	.open	= mfrc522_driver_open,
	.release = mfrc522_driver_release
	/* Only use the kernel's defaults */
};

static void mfrc522_driver_destroy_sysfs(struct mfrc522_driver_dev **mfrc522_driver_dev,
	size_t nb_devices) {

	size_t i;

	for (i = 0; i < nb_devices; ++i) {
			kfree(mfrc522_driver_dev[i]->virtual_dev->driver_data);
			device_destroy(&mfrc522_driver_class, MKDEV(major, i));
	}
	class_unregister(&mfrc522_driver_class);
}

static void mfrc522_driver_delete_devices(size_t count)
{
	size_t i;

	for (i = 0; i < count; i++) {
		cdev_del(&(mfrc522_driver_devs[i])->cdev);
		kfree(mfrc522_driver_devs[i]);
	}
	kfree(mfrc522_driver_devs);
}

__exit
static void mfrc522_driver_exit(void)
{
	int i;
	int log_level = 0;

	for (i = 0; i < nb_devices; i++)
		log_level |= mfrc522_driver_devs[i]->log_level;

	mfrc522_driver_destroy_sysfs(mfrc522_driver_devs, nb_devices);
	mfrc522_driver_delete_devices(nb_devices);

	unregister_chrdev_region(MKDEV(major, 0), nb_devices);
	LOG("Released major %d", LOG_EXTRA, log_level, major);

	LOG("Stopping driver support for MFRC_522 card", LOG_INFO, LOG_INFO);
}

/**
 * Create the whole file hierarchy under /sys/class/statistics/.
 * Character devices setup must have already been completed.
 * @param mfrc522_drivers_dev: the structure that will contain all devices
 */
static int mfrc522_driver_create_sysfs(struct mfrc522_driver_dev **mfrc522_drivers_dev)
{
	int ret;
	struct device *virtual_dev;
	struct mfrc522_driver_data *data;
	size_t i;

	ret = class_register(&mfrc522_driver_class);
	if (ret < 0) {
			ret = 1;
			goto sysfs_end;
	}

	for (i = 0; i < nb_devices; ++i) {
			/* Create device with all its attributes */
			virtual_dev = device_create_with_groups(&mfrc522_driver_class, NULL,
					MKDEV(major, i), NULL /* No private data */,
					mfrc522_driver_groups, "mfrc%zu", i);
			if (IS_ERR(virtual_dev)) {
					ret = 1;
					goto sysfs_cleanup;
			}

			/* Store access to the device. As we're the one creating it,
			 * we take advantage if this direct access to struct device.
			 * Note that on regular scenarios, this is not the case;
			 * "struct device" and "struct cdev" are disjoint. */
			mfrc522_drivers_dev[i]->virtual_dev = virtual_dev;

			data = kmalloc(sizeof(struct mfrc522_driver_data), GFP_KERNEL);
			if (!data) {
					device_destroy(&mfrc522_driver_class, MKDEV(major, i));
					ret = 1;
					goto sysfs_cleanup;
			}
			data->bytes_read = 0;
			data->bytes_written = 0;
			// Stock our data to retrieve them later
			mfrc522_drivers_dev[i]->virtual_dev->driver_data = (void *)data;
	}

	goto sysfs_end;

sysfs_cleanup:
	mfrc522_driver_destroy_sysfs(mfrc522_drivers_dev, i);
sysfs_end:
	return ret;
}


static void mfrc522_driver_init_dev(struct mfrc522_driver_dev *dev)
{
	if (quiet)
		dev->log_level = LOG_NONE;
	else if (strlen(starting_debug_levels) == 0) /* Enable error logs by default */
		dev->log_level = LOG_ERROR;
	else
		dev->log_level = process_logs_module_param(starting_debug_levels);
	dev->cdev.owner = THIS_MODULE;
	cdev_init(&dev->cdev, &mfrc522_driver_fops);
	dev->card_dev = mfrc522_find_dev();
	dev->virtual_dev = NULL;
}

static int mfrc522_driver_setup_dev(size_t i)
{
	int ret;

	/* Allocate our device structure */
	mfrc522_driver_devs[i] = kmalloc(sizeof(*mfrc522_driver_devs[i]), GFP_KERNEL);
	if (!mfrc522_driver_devs[i]) {
		LOG("init: failed to allocate struct mfrc522_driver_dev",
			LOG_ERROR, LOG_ERROR);
		return 1;
	}

	mfrc522_driver_init_dev(mfrc522_driver_devs[i]);

	ret = cdev_add(&(mfrc522_driver_devs[i])->cdev, MKDEV(major, i), 1);
	if (ret < 0) {
		LOG("init: failed to add device", LOG_ERROR,
			mfrc522_driver_devs[i]->log_level);
		kfree(mfrc522_driver_devs[i]);
		return 1;
	}

	return 0;
}

__init
static int mfrc522_driver_init(void)
{
	dev_t dev;
	int ret = 0;
	size_t devices_set_up = 0;
	size_t i;

	printk(KERN_CONT "Hello, GISTRE card !\n"); /* to make sure it will work with the testsuite */
	if (nb_devices < 0) {
		LOG("Trying to create a negative number of device, aborting.", LOG_ERROR, LOG_ERROR);
		return 1;
	}

	/* Allocate major */
	ret = alloc_chrdev_region(&dev, 0, nb_devices, "mfrc");
	if (ret < 0)
		return ret;

	major = MAJOR(dev);
	LOG("Got major %d for driver support for MRFC_522 card",
		LOG_INFO, LOG_INFO, major);

	mfrc522_driver_devs = kmalloc_array(nb_devices,
						sizeof(struct mfrc522_driver_dev *), GFP_KERNEL);
	for (i = 0; i < nb_devices; i++) {
		if (mfrc522_driver_setup_dev(i))
			goto init_cleanup;

		devices_set_up++;
	}

	if (mfrc522_driver_create_sysfs(mfrc522_driver_devs)) {
		LOG("Failed to create class", LOG_ERROR, LOG_ERROR);
		ret = -ENOMEM;
		goto init_cleanup;
	}

	struct device_node *dev_node = of_find_node_by_name(NULL, "mfrc522_emu");
	u32 version;
	int check_property = of_property_read_u32(dev_node, "version", &version);
	if (check_property)
		LOG("version property not found (%d)", LOG_WARN, LOG_WARN, check_property);
	else
		LOG("version: %u", LOG_INFO, LOG_INFO, version);

	LOG("init: %d devices successfully initialized",
		LOG_INFO, LOG_INFO, nb_devices);
	goto init_end;

init_cleanup:
	mfrc522_driver_delete_devices(devices_set_up);
	unregister_chrdev_region(MKDEV(major, 0), nb_devices);
init_end:
	return ret;
}

module_init(mfrc522_driver_init);
module_exit(mfrc522_driver_exit);
