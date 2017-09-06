/*
 * file:        stl_smr.h
 * description: SMR interface used by fake and real SMR
 */

#ifndef _DEV_STL_SMR_H
#define _DEV_STL_SMR_H

#include <dev/stl/stl.h>
#include <sys/mutex.h>
#include <sys/condvar.h>

#define BANDS_PER_SECTOR (SECTOR_SIZE/sizeof(int))

struct smr;
struct smr_op {
    lba_t lba;
    int band;
    int offset;
    int len;
    union {
	const void *w_buf;
	void *r_buf;
    } rw;
};

int smr_n_bands(struct smr *dev);
int smr_band_size(struct smr *dev);
int smr_write_pointer(struct smr *dev, int band);
int smr_open(struct vnode *vn, struct smr *dev);
void smr_close(struct smr *dev);
int smr_write_bp(struct smr *dev, struct buf *bp);
void smr_read_bp(struct smr *dev, struct buf *bp);
void smr_read(struct smr *dev, unsigned band, unsigned offset, void *buf,
              unsigned n_sectors);
void smr_read_multi(struct smr *dev, struct smr_op *ops, int opcount);
void smr_write(struct smr *dev, unsigned band, unsigned offset,
               const void *buf, unsigned n_sectors);
void smr_write_multi(struct smr *dev, struct smr_op *ops, int opcount);
int smr_reset_pointer(struct smr *dev, unsigned band);
int smr_reset_all(struct smr *dev);
void smr_flush(struct smr *dev);
//void smr_set_pointer(struct smr *dev, unsigned band, unsigned offset);

struct smr {
    struct vnode *vn;
    int n_bands;
    int band_size;
    int *write_pointers;
    kmutex_t m;
    kcondvar_t  c;
    int wp_sectors;             /* fakeSMR only */
};

#endif
