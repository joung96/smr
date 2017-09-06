/*
 * file:        stl_realsmr.c
 * description: real SMR functions
 */


#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/errno.h>
#include <sys/buf.h>
#include <sys/bufq.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/pool.h>
#include <sys/ioctl.h>
#include <sys/device.h>
#include <sys/disk.h>
#include <sys/disklabel.h>
#include <sys/fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <dev/ata/atareg.h>
#include <sys/ataio.h>
#include <sys/vnode.h>

typedef int64_t lba_t;
#define _INTERNAL 1
#include "stl_smr.h"

struct zone_list_header {
        uint32_t n_zones;
        uint8_t  same;
        uint8_t  pad[3];
        uint64_t max_lba;
};

struct zone_list_entry {
        uint8_t  zone_type;
        uint8_t  reset : 1;
        uint8_t  non_seq : 1;
        uint8_t  _pad : 2;
        uint8_t  condition : 4;
        uint8_t  _pad2[6];
        uint64_t length;
        uint64_t start;
        uint64_t write_ptr;
        uint8_t  _pad3[32];
};

int wd_start = 0;
static int
ata_command(struct smr *dev, struct atareq *req, int flag)
{
        int error;

        //error = ioctl(fd, ATAIOCCOMMAND, req);
        // XXX curlwp isn't right here? ioctl gets an lwp, but reads and writes don't
        wd_start = 1;
        error = VOP_IOCTL(dev->vn, ATAIOCCOMMAND, req, flag, curlwp->l_cred);
        wd_start = 0;
	//int wdioctl(dev_t dev, u_long xfer, void *addr, int flag, struct lwp *l);
	//int blah = wdioctl(fs->lfs_dev, ATAIOCCOMMAND, &req, flag, curlwp);

	if (error) {
	    return error;
	}

        switch (req->retsts) {

        case ATACMD_OK:
                return 0;
        case ATACMD_TIMEOUT:
                printf("ATA command timed out\n");
                return 1;
        case ATACMD_DF:
                printf("ATA device returned a Device Fault\n");
                return 1;
        case ATACMD_ERROR:
                if (req->error & WDCE_ABRT)
                        printf("ATA device returned Aborted "
                                "Command\n");
                else
                        printf("ATA device returned error register "
                                "%0x\n", req->error);
                return 1;
        default:
                printf("ATAIOCCOMMAND returned unknown result code "
                        "%d\n", req->retsts);
		return 1;
        }
}

static int do_report_zones(struct smr *dev, uint64_t lba, void *data, unsigned int len)
{
    struct atareq req;
    memset(&req, 0, sizeof(req));
    req.datalen = len;
    req.flags = ATACMD_READ | ATACMD_LBA48;
    req.command = 0x4a;
    req.databuf = data;
    req.timeout = 10000;
    req.lba48 = lba;
    req.sec_count = (len + 511) / 512;

    return ata_command(dev, &req, FREAD);
}

static int do_reset_write_pointer (struct smr *dev, uint64_t lba, int all_bit)
{
    struct atareq req;
    memset(&req, 0, sizeof(req));
    req.flags = ATACMD_WRITE | ATACMD_LBA48;
    req.command = 0x9F;
    req.timeout = 1000;
    req.lba48 = lba;
    req.features = 4; /* XXX ATA_SUBCMD_RESET_WP */
    req.high_features = all_bit;
    //int wdioctl(dev_t dev, u_long xfer, void *addr, int flag, struct lwp *l);
    //int blah = wdioctl(fs->lfs_dev, ATAIOCCOMMAND, &req, FWRITE, curlwp);

    return ata_command(dev, &req, FWRITE);
}

int smr_reset_pointer(struct smr *dev, unsigned band)
{
    // XXX (we have to take into account the partition)
    band += 64;
    uint64_t band_lba = band * dev->band_size * SECTOR_SIZE / DEV_BSIZE;
    int err = 0;
    do {
	err = do_reset_write_pointer(dev, band_lba, 0);
	if (err) {
	    printf("Error resetting write pointer!\n");
	}
    } while(err == 0);
    mutex_enter(&dev->m);
    if (!err) {
	dev->write_pointers[band] = 0;
    }
    cv_broadcast(&dev->c);
    mutex_exit(&dev->m);
    return err;
}

int smr_reset_all(struct smr *dev)
{
    int err = do_reset_write_pointer(dev, 0, 1);
    if (!err) {
	int i;
	for (i = 0; i < dev->n_bands; i++)
	    dev->write_pointers[i] = 0;
    }
    return 0;
}

int smr_open(struct vnode *vn, struct smr *dev)
{
    int err = 0;
    void *write_pointers = NULL;

    memset(dev, 0, sizeof(struct smr));
    mutex_init(&dev->m, MUTEX_DEFAULT, IPL_NONE);
    cv_init(&dev->c, "smrdev");
    dev->vn = vn;

    unsigned int i, len = 512*64;
    void *data = malloc(len, M_DEVBUF, M_NOWAIT);
    if (data == NULL) {
    	return -1;
    }
    struct zone_list_header *hdr = data;
    struct zone_list_entry *entry = data;

    err = do_report_zones(dev, 0, data, len);
    if (err) {
    	printf("Not a real SMR device");
    //	return err;
    }

    // XXX yikes
    dev->n_bands = err ? 1907775 : hdr->n_zones;
    dev->band_size = err ? 524288 / 8 : (int)entry[1].length / 8;

    if (!err && (hdr->same & 3) == 2 ) {
        dev->n_bands--;
    }

    write_pointers = malloc(dev->n_bands * sizeof(int), M_DEVBUF, M_NOWAIT);
    if (write_pointers == NULL) {
    	printf("woahhh #2\n");
    	return ENOMEM;
    }
    dev->write_pointers = write_pointers;

    if (!err) {
	uint64_t max_lba = hdr->max_lba, lba = 0;
	int j = 0;
	while (lba < max_lba && j < dev->n_bands) {
	    do_report_zones(dev, lba, data, len);
	    for (i = 1; i < len/sizeof(*entry) && lba<max_lba && j < dev->n_bands; i++) {
		dev->write_pointers[j] = (entry[i].write_ptr-lba) / 8;
		j++;
		lba = entry[i].start + entry[i].length;
	    }
	}
    }

    else {
    	// XXX
	for (i = 2; i < dev->n_bands; i++) {
	    dev->write_pointers[i] = 0;
	}
	// XXX works only for this setup
	dev->write_pointers[64] = 1;
	dev->write_pointers[65] = 3;
    }

    return 0;
}

void smr_close(struct smr *dev)
{
    free(dev->write_pointers, M_DEVBUF);
}
