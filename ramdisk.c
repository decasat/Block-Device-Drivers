/********************************************************************************
 * RAM DISK DRIVER								*
 * Author : Sateesh Kumar G.							*
 * Useage:	1) make								*
 *		2) insmod ramdisk.ko						*
 *		3) mkfs.ext2 /dev/skg						* 		 
 *		4) mount -t ext2 /dev/skg /mnt					*
 *		5) cd /mnt							*
 *		6) touch testfile						*
 *		7) mkdir mydir							*
 *		8) try out all file operations					*
 *		9) cd ~/							*
 *		10) umount /dev/skg						*
 *		11) rmmod ramdisk						*
 *
 ********************************************************************************/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/genhd.h> // For basic block driver framework
#include <linux/blkdev.h> // For at least, struct block_device_operations
#include <linux/hdreg.h> // For struct hd_geometry
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <linux/string.h>  // string operations


#define SKG_FIRST_MINOR 0
#define SKG_MINOR_CNT 1
#define SKG_SECTOR_SIZE 512
#define SKG_DEVICE_SIZE 1024 /* sectors */
/* So, total device size = 1024 * 512 bytes = 512 KiB */

static u_int skg_major = 0;



/* Array where the disk stores its data */
static u8 *mydisk;


/* 
 * The internal structure representation of our Device
 */
static struct myprivDevice
{
	unsigned int size; 			/* Size is the size of the device (in sectors) */
	spinlock_t lock; 			/* For exclusive access to our request queue */
	struct request_queue *skg_queue; 	/* Our request queue */
	struct gendisk *skg_disk; 		/* This is kernel's representation of an individual disk device */
} Device;

static int skg_open(struct block_device *bdev, fmode_t mode)
{
	unsigned unit = iminor(bdev->bd_inode);

	printk(KERN_INFO "skg: Device is opened\n");
	printk(KERN_INFO "skg: Inode number is %d\n", unit);

	if (unit > SKG_MINOR_CNT)
		return -ENODEV;
	return 0;
}

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3,10,0))
static int skg_close(struct gendisk *disk, fmode_t mode)
{
	printk(KERN_INFO "skg: Device is closed\n");
	return 0;
}
#else
static void skg_close(struct gendisk *disk, fmode_t mode)
{
	printk(KERN_INFO "skg: Device is closed\n");
}
#endif

static int skg_getgeo(struct block_device *bdev, struct hd_geometry *geo)
{
	geo->heads = 1; 
	geo->cylinders = 32;
	geo->sectors = 32;
	geo->start = 0;
	return 0;
}

/* 
 * Actual Data transfer done here
 */
static int skg_transfer(struct request *req)
{

	int dir = rq_data_dir(req);
	sector_t start_sector = blk_rq_pos(req);
	unsigned int sector_cnt = blk_rq_sectors(req);

	#if (LINUX_VERSION_CODE < KERNEL_VERSION(3,14,0))
	#define BV_PAGE(bv) ((bv)->bv_page)
	#define BV_OFFSET(bv) ((bv)->bv_offset)
	#define BV_LEN(bv) ((bv)->bv_len)
		struct bio_vec *bv;
	#else
	#define BV_PAGE(bv) ((bv).bv_page)
	#define BV_OFFSET(bv) ((bv).bv_offset)
	#define BV_LEN(bv) ((bv).bv_len)
		struct bio_vec bv;
	#endif
	struct req_iterator iter;

	sector_t sector_offset;
	unsigned int sectors;
	u8 *buffer;

	int ret = 0;


	sector_offset = 0;
	rq_for_each_segment(bv, req, iter)
	{
		buffer = page_address(BV_PAGE(bv)) + BV_OFFSET(bv);
		if (BV_LEN(bv) % SKG_SECTOR_SIZE != 0)
		{
			printk(KERN_ERR "skg: Should never happen: "
				"bio size (%d) is not a multiple of SKG_SECTOR_SIZE (%d).\n"
				"This may lead to data truncation.\n",
				BV_LEN(bv), SKG_SECTOR_SIZE);
			ret = -EIO;
		}
		sectors = BV_LEN(bv) / SKG_SECTOR_SIZE;
		printk(KERN_DEBUG "skg: Start Sector: %lld, Sector Offset: %lld; Buffer: %p; Length: %u sectors\n",
			start_sector, sector_offset, buffer, sectors);
		if (dir == WRITE) /* Write to the device */
		{

			 memcpy(mydisk + (start_sector + sector_offset) * SKG_SECTOR_SIZE, buffer, sectors * SKG_SECTOR_SIZE);

		}

		else /* Read from the device */
		{
			 memcpy(buffer, mydisk + (start_sector + sector_offset) * SKG_SECTOR_SIZE, sectors * SKG_SECTOR_SIZE);

		}
		sector_offset += sectors;
	}
	if (sector_offset != sector_cnt)
	{
		printk(KERN_ERR "skg: bio info doesn't match with the request info");
		ret = -EIO;
	}

	return ret;
}
	
/*
 * Represents a block I/O request for us to execute
 */
static void skg_request(struct request_queue *q)
{
	struct request *req;
	int ret;

	/* Gets the current request from the dispatch queue */
	while ((req = blk_fetch_request(q)) != NULL)
	{
#if 0
		/*
		 * This function tells us whether we are looking at a filesystem request
		 * - one that moves block of data
		 */
		if (!blk_fs_request(req))
		{
			printk(KERN_NOTICE "skg: Skip non-fs request\n");
			/* We pass 0 to indicate that we successfully completed the request */
			__blk_end_request_all(req, 0);
			//__blk_end_request(req, 0, blk_rq_bytes(req));
			continue;
		}
#endif
		ret = skg_transfer(req);
		__blk_end_request_all(req, ret);
	}
}

/* 
 * These are the file operations that performed on the ram block device
 */
static struct block_device_operations skg_fops =
{
	.owner = THIS_MODULE,
	.open = skg_open,
	.release = skg_close,
	.getgeo = skg_getgeo,
};
	
/* 
 * In init:
 * Create request queue, register with block layer, register with vfs 
 */
static int __init skg_init(void)
{
	//int ret=0;

	/* Set up our RAM Device */


	mydisk = vmalloc(SKG_DEVICE_SIZE * SKG_SECTOR_SIZE);

	Device.size = SKG_DEVICE_SIZE;

	/* Get Registered */
	skg_major = register_blkdev(skg_major, "skgdisk");
	if (skg_major <= 0)
	{
		printk(KERN_ERR "skg: Unable to get Major Number\n");
		  vfree(mydisk);
		return -EBUSY;
	}

	/* Get a request queue (here queue is created) */
	spin_lock_init(&Device.lock);
	Device.skg_queue = blk_init_queue(skg_request, &Device.lock);
	if (Device.skg_queue == NULL)
	{
		printk(KERN_ERR "skg: blk_init_queue failure\n");
		unregister_blkdev(skg_major, "skgdisk");
		vfree(mydisk);
		return -ENOMEM;
	}
	
	/*
	 * Add the gendisk structure
	 */
	Device.skg_disk = alloc_disk(SKG_MINOR_CNT);
	if (!Device.skg_disk)
	{
		printk(KERN_ERR "skg: alloc_disk failure\n");
		blk_cleanup_queue(Device.skg_queue);
		unregister_blkdev(skg_major, "skgdisk");
		  vfree(mydisk);
		return -ENOMEM;
	}

 	/* Setting the major number */
	Device.skg_disk->major = skg_major;

  	/* Setting the first mior number */
	Device.skg_disk->first_minor = SKG_FIRST_MINOR;

 	/* Initializing the device operations */
	Device.skg_disk->fops = &skg_fops;

 	/* Driver-specific own internal data */
	Device.skg_disk->private_data = &Device;
	Device.skg_disk->queue = Device.skg_queue;
	sprintf(Device.skg_disk->disk_name, "skgdisk");

	/* Setting the capacity of the device in its gendisk structure */
	set_capacity(Device.skg_disk, Device.size);

	/* Adding the disk to the system */
	add_disk(Device.skg_disk);

	/* Now the disk is "live" */
	printk(KERN_INFO "skg: Ram Block driver initialised (%d sectors; %d bytes)\n",
		Device.size, Device.size * SKG_SECTOR_SIZE);

	return 0;
}

/*
 * This is the unregistration and uninitialization section of the ram block
 * device driver
 */
static void __exit skg_cleanup(void)
{
	del_gendisk(Device.skg_disk);
	put_disk(Device.skg_disk);
	blk_cleanup_queue(Device.skg_queue);
	unregister_blkdev(skg_major, "skgdisk");
	vfree(mydisk);
}

module_init(skg_init);
module_exit(skg_cleanup);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SATEESHKG");
