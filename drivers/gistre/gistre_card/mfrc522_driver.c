#include "mfrc522_driver.h"

#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "commands/command.h"

static int major;
static struct mfrc522_driver_dev *mfrc522_driver_dev;

int mfrc522_driver_open(struct inode *inode, struct file *file) {
	unsigned int i_major = imajor(inode);
	unsigned int i_minor = iminor(inode);
	if (i_major != major) {
		pr_err("Invalid major %d, expected %d\n", i_major, major);
		return -EINVAL;
	}

    pr_debug("mfrc_open: major '%u', minor '%u'\n", i_major, i_minor);
    file->private_data = mfrc522_driver_dev;

	return 0;
}

int mfrc522_driver_release(struct inode *inode /* unused */,
        struct file *file /* unused */) {

	return 0;
}

ssize_t mfrc522_driver_read(struct file *file, char __user *buf,
        size_t len, loff_t *off /* unused */) {
	struct mfrc522_driver_dev *dev;
    dev = file->private_data;

    // check if data exists
    if (!dev->contains_data) {
        pr_warn("\nNo data in internal buffer\n");
        return 0;
    }

    // Copying our internal buffer (int *) into a string (char *)
    char data[INTERNAL_BUFFER_SIZE + 1];
    memset(data, 0, INTERNAL_BUFFER_SIZE + 1);
    int i = 0;
    while (i < INTERNAL_BUFFER_SIZE)
    {
        data[i] = dev->data[i];
        i++;
    }

    // Flush internal buffer
    if (copy_to_user(buf, data, INTERNAL_BUFFER_SIZE + 1)) {
        pr_err("Failed to copy data to user\n");
        return -EFAULT;
    }

    // Reset internal buffer
    memset(dev->data, 0, INTERNAL_BUFFER_SIZE + 1);
    dev->contains_data = false;

    return INTERNAL_BUFFER_SIZE;
}

ssize_t mfrc522_driver_write(struct file *file, const char __user *user_buf,
        size_t len, loff_t *off /* unused */) {
	struct mfrc522_driver_dev *dev;
    dev = file->private_data;
    
    char buff[MAX_ACCEPTED_COMMAND_SIZE + 1];

    memset(buff, 0, MAX_ACCEPTED_COMMAND_SIZE + 1);

    if (copy_from_user(buff, user_buf, MAX_ACCEPTED_COMMAND_SIZE)) {
        pr_err("Failed to copy user");
        return -EFAULT;
    }

    struct command *command = parse_command(buff);
    if (command == NULL) {
        return -EFAULT;
    }
    
	if (process_command(command, dev) < 0) {
        return -EFAULT;
    }

    return len;
}

/*
 *  Init & Exit
 */

static struct file_operations mfrc_fops = {
	.owner   = THIS_MODULE,
    .read    = mfrc522_driver_read,
    .write   = mfrc522_driver_write,
    .open    = mfrc522_driver_open,
    .release = mfrc522_driver_release
	/* Only use the kernel's defaults */
};

__exit
static void mfrc522_driver_exit(void) {

	dev_t dev;

	/* Unregister char device */
	cdev_del(&mfrc522_driver_dev->cdev);
	pr_debug("Unregistered char device\n");

	/* Free mfrc522_driver_dev structure */
	kfree(mfrc522_driver_dev);
	pr_debug("Freed struct mfrc522_driver_dev\n");

	/* Release major */
	dev = MKDEV(major, 0);
	unregister_chrdev_region(dev, 1);
	pr_debug("Released major %d\n", major);

    pr_info("Stopping driver support for MFRC_522 card\n");
}

__init
static int mfrc522_driver_init(void) {
    pr_info("Hello, GISTRE card !\n");
	dev_t dev;
	int ret;
	const char devname[] = "mfrc0";

	/* Allocate major */
	ret = alloc_chrdev_region(&dev, 0, 1, devname);
	if (ret < 0) {
		pr_err("Failed to allocate major\n");
		return 1;
	}
	else {
		major = MAJOR(dev);
		pr_info("Got major %d\n", major);
	}

	/* Allocate our device structure */
	mfrc522_driver_dev = kmalloc(sizeof(*mfrc522_driver_dev), GFP_KERNEL);
	if (! mfrc522_driver_dev) {
		pr_err("Failed to allocate struct mfrc522_driver_dev\n");
		return -ENOMEM;
	}
	else {
		pr_debug("Allocated struct mfrc522_driver_dev\n");
	}

    // Enable error logs by default
    mfrc522_driver_dev->log_level = LOG_ERROR;

	/* Register char device */
	mfrc522_driver_dev->cdev.owner = THIS_MODULE;
	cdev_init(&mfrc522_driver_dev->cdev, &mfrc_fops);

	ret = cdev_add(&mfrc522_driver_dev->cdev, dev, 1);
	if (ret < 0) {
		pr_err("Failed to register char device\n");
		return -ENOMEM;
	}
	else {
		pr_debug("Registered char device\n");
	}

	return 0;
}

module_init(mfrc522_driver_init);
module_exit(mfrc522_driver_exit);