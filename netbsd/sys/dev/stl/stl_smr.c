#define __need_IOV_MAX
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/buf.h>
#include <sys/vnode.h>

typedef int64_t lba_t;
#define _INTERNAL 1
#include "stl_smr.h"

#define max(a, b) (((a) > (b)) ? (a) : (b))
#define min(a, b) (((a) < (b)) ? (a) : (b))

int smr_n_bands(struct smr *dev)
{
    // XXX partition
    return dev->n_bands;
}

int smr_band_size(struct smr *dev)
{
    return dev->band_size;
}

int smr_write_pointer(struct smr *dev, int band)
{
    // XXX partition
    band += 64;
    assert(band >= 0 && band < dev->n_bands);

    return dev->write_pointers[band];
}

static int iov_sum(const struct iovec *iov, int iovcnt)
{
    int i, sum;
    for (i = sum = 0; i < iovcnt; i++)
        sum += iov[i].iov_len;
    return sum / SECTOR_SIZE;
}

static void smr_readv(struct smr *dev, unsigned band, unsigned offset,
                struct iovec *iov, int iovcnt)
{
    // XXX ROB can do like cgd and optimize this
    struct buf *bp = getiobuf(dev->vn, true);

    int n_sectors = iov_sum(iov, iovcnt);
    assert(band < dev->n_bands && offset+n_sectors <= dev->band_size);

    off_t position = (dev->band_size * (off_t)band + offset) * SECTOR_SIZE / DEV_BSIZE;

    bp->b_data = iov[0].iov_base;
    bp->b_flags = B_READ;
    bp->b_cflags = BC_BUSY;
    bp->b_blkno = position;
    bp->b_bcount = bp->b_resid = n_sectors * SECTOR_SIZE;
    //bp->b_proc = curproc;
    BIO_SETPRIO(bp, BPRIO_DEFAULT);

    VOP_STRATEGY(dev->vn, bp);
    int error = biowait(bp);
    (void) error;

    putiobuf(bp);
}

void smr_read_bp(struct smr *dev, struct buf *bp) {
    VOP_STRATEGY(dev->vn, bp);
}

int smr_write_bp(struct smr *dev, struct buf *bp) {
    unsigned n_sectors = (bp->b_bcount + SECTOR_SIZE - 1) / SECTOR_SIZE;
    uint64_t band = bp->b_blkno * DEV_BSIZE / SECTOR_SIZE;
    uint64_t offset = band % dev->band_size;
    band /= dev->band_size;

    // XXX partition
    band += 64;

    mutex_enter(&dev->m);
    while (offset != dev->write_pointers[band]) {
    	printf("waiting: %"PRIu64" %"PRIu64"\n", band, offset);
        cv_wait(&dev->c, &dev->m);
    }
    (void)offset;
    mutex_exit(&dev->m);

    void (*iod)(struct buf *) = bp->b_iodone;
    bp->b_iodone = NULL;

    int ret = VOP_STRATEGY(dev->vn, bp);
    // XXX dunno what causes this? is it that it's not async?
    if (ret == EINPROGRESS) {
    	ret = 0;
    }
    if (biowait(bp)) {
    	//printf("berror %d\n", bp->b_error);
    }
    iod(bp);

    mutex_enter(&dev->m);
    dev->write_pointers[band] += n_sectors;
    cv_broadcast(&dev->c); /* thunder the herd */
    mutex_exit(&dev->m);

    return ret;
}

void smr_read(struct smr *dev, unsigned band, unsigned offset, void *buf,
                unsigned n_sectors)
{
    // XXX reads should be protected like writes? or does it not matter...
    struct iovec v = {.iov_base = (void*)buf, .iov_len = n_sectors*SECTOR_SIZE};
    return smr_readv(dev, band, offset, &v, 1);
}

void smr_read_multi(struct smr *dev, struct smr_op *ops, int opcount)
{
    struct iovec iov[opcount];
    int i;
    for (i = 0; i < opcount; i++) {
	iov[i] = (struct iovec){.iov_base = ops[i].rw.r_buf,
				.iov_len = ops[i].len * SECTOR_SIZE};
	// XXX fix later
	smr_readv(dev, ops[i].band, ops[i].offset, &iov[i], 1);
	//if (i > 0) {
	//    printf("assert %d %d %d %d %d\n", ops[i].band, ops[i-1].band, ops[i].offset, ops[i-1].offset, ops[i-1].len);
	//    assert(ops[i].band == ops[i-1].band &&
	//	   ops[i].offset == ops[i-1].offset + ops[i-1].len);
	//}
    }
    //smr_readv(dev, ops[0].band, ops[0].offset, iov, opcount);
}

static void smr_writev(struct smr *dev, unsigned band, unsigned offset,
                struct iovec *iov, int iovcnt)
{
    // XXX ROB can do like cgd and optimize this
    struct buf *bp = getiobuf(dev->vn, true);

    int n_sectors = iov_sum(iov, iovcnt);
    assert(band < dev->n_bands && offset+n_sectors <= dev->band_size);

    off_t position = (dev->band_size * (off_t)band + offset) * SECTOR_SIZE / DEV_BSIZE;

    bp->b_data = iov[0].iov_base;
    bp->b_flags = B_WRITE;
    bp->b_blkno = position;
    bp->b_bcount = bp->b_resid = n_sectors * SECTOR_SIZE;
    //bp->b_proc = curproc;
    BIO_SETPRIO(bp, BPRIO_DEFAULT);
    SET(bp->b_cflags, BC_BUSY);

    /* re-order any threads which are trying to write out-of-order. Use
	* simple thundering-herd logic and do the write under the lock.
	*/
	// could have a lock per band

    // XXX partition
    band += 64;

    mutex_enter(&dev->m);
    while (offset != dev->write_pointers[band]) {
    	printf("waiting: %u %u\n", band, offset);
        cv_wait(&dev->c, &dev->m);
    }
    mutex_exit(&dev->m);


    // since we're doing a write...
    mutex_enter(bp->b_vp->v_interlock);
    bp->b_vp->v_numoutput++;
    mutex_exit(bp->b_vp->v_interlock);

    VOP_STRATEGY(dev->vn, bp);

    int error = biowait(bp);
    (void) error;

    mutex_enter(&dev->m);
    dev->write_pointers[band] += n_sectors;
    cv_broadcast(&dev->c); /* thunder the herd */
    mutex_exit(&dev->m);

    putiobuf(bp);
}

void smr_write(struct smr *dev, unsigned band, unsigned offset, const void *buf,
                unsigned n_sectors)
{
    struct iovec v = {.iov_base = __UNCONST(buf), .iov_len = n_sectors*SECTOR_SIZE};
    return smr_writev(dev, band, offset, &v, 1);
}

void smr_write_multi(struct smr *dev, struct smr_op *ops, int opcount)
{
    struct iovec iov[opcount];
    int i;
    for (i = 0; i < opcount; i++) {
	iov[i] = (struct iovec){.iov_base = __UNCONST(ops[i].rw.w_buf),
				.iov_len = ops[i].len * SECTOR_SIZE};
	if (i > 0)
	    assert(ops[i].band == ops[i-1].band &&
		   ops[i].offset == ops[i-1].offset + ops[i-1].len);
    }
    smr_writev(dev, ops[0].band, ops[0].offset, iov, opcount);
}

/* causes underlying driver to issue FLUSH or SYNCHRONIZE CACHE
 */
void smr_flush(struct smr *dev)
{
    //fsync(dev->fd);
    // XXX ROB
    return;
}
