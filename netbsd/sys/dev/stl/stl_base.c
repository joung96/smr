/*
 * file:        stl_base.c
 * description: basic shingle translation layer logic
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
#include <sys/namei.h> /* for pathbuf */
#include <sys/vnode.h>
#include <sys/conf.h>
#include <sys/syslog.h>
#include <sys/time.h>
#include <sys/sched.h>
#include <sys/kthread.h>

/* Apple OSX rbtree implementation
 */
#include <sys/rbtree.h>

#include "stl.h"
#include "stl_map.h"
#include "stl_smr.h"
#include "stl_base.h"

#define max(a, b) (((a) > (b)) ? (a) : (b))
#define min(a, b) (((a) < (b)) ? (a) : (b))

#ifdef STL_DEBUG
#define DEBUG_PRINT(...) printf(__VA_ARGS__ )
#else
#define DEBUG_PRINT(...) do{ } while ( false )
#endif

/* There are two levels of cleaning and band allocation.
 * - background cleaning - clean until MINFREE_BG bands are free,
 *   allocate extents at normal priority. (clean if 1 free)
 * - forced cleaning - clean until MINFREE_PRIO bands are free,
 *   allocate at high priority. (i.e. can allocate the last band that
 *   is not touched by normal priority allocation)
 */
#define MINFREE_FG 2
#define MINFREE_BG 4
#define PRIO_NORM 0
#define PRIO_HIGH 1

/* todo - sequence number arithmetic with signed sequence #s?
 * or just use 64-bit sequence number?
 */

/*---------- Prototypes -------------*/

static pba_t alloc_extent(struct volume *v, int g, int len,
                    int prio, int *plen);

static int do_write(struct volume *v, int group, struct buf *bp, int prio);

static void do_write_header_done(struct buf *bp);

static int clean_group(struct volume *v, int g, int minfree, int prio, bool lock_held);

static int _stl_write(struct volume *v, struct buf *bp);

#define PBA_NEXT (struct pba){.band = 0xFFFFFFFF, .offset=0xFFFFFFFF}


/* Update mapping. Removes any total overlaps, edits any partial
 * overlaps, adds new extent to forward and reverse map.
 */
static void update_range(struct volume *v, pba_t location, lba_t lba, int len,
                         pba_t pba)
{
    assert(pba.offset != 0);
    assert(len > 0);

    struct entry *e = stl_map_lba_geq(v->map, lba);
    if (e != NULL) {
        /* [----------------------]        e     new     new2
         *        [++++++]           -> [-----][+++++][--------]
         */
        if (e->lba < lba && e->lba+e->len > lba+len) {
            int64_t new_lba = lba+len;
            int new_len = e->lba + e->len - new_lba;
            assert(new_len > 0);

            pba_t new_pba = e->pba;
            if (!pba_eq(e->pba, PBA_INVALID))
                new_pba = pba_add(e->pba, (e->len - new_len));
            struct entry *_new2 = stl_map_entry(sizeof(struct entry));
            *_new2 = (struct entry){.lba = lba+len, .pba = new_pba,
                                    .len = new_len, .dirty = 1};

            DEBUG_PRINT("split %d,+%d -> %d.%d into ",
                  (int)e->lba, e->len, e->pba.band, e->pba.offset);

            e->len = lba - e->lba; /* do this *before* inserting below */
            DEBUG_PRINT("%d,+%d -> %d.%d %d,+%d -> %d.%d\n",
                  (int)e->lba, e->len, e->pba.band, e->pba.offset,
                 (int)_new2->lba, _new2->len, _new2->pba.band, _new2->pba.offset);
            stl_map_update(e, e->lba, e->pba, e->len);
            e->dirty = 1;
            assert(e->len > 0);
            stl_map_insert(v->map, _new2, _new2->lba, _new2->pba, _new2->len);
            e = _new2;
        }
        /* [------------]
         *        [+++++++++]        -> [------][+++++++++]
         */
        else if (e->lba < lba) {
            DEBUG_PRINT("overlap tail %d,+%d -> %d.%d becomes", (int)e->lba, e->len,
                  e->pba.band, e->pba.offset);
            e->len = lba - e->lba;
            DEBUG_PRINT(" %d,+%d -> %d.%d\n", (int)e->lba, e->len,
                  e->pba.band, e->pba.offset);
            stl_map_update(e, e->lba, e->pba, e->len);
            assert(e->len > 0);
            e->dirty = 1;
            e = stl_map_lba_iterate(v->map, e);
        }
        /*          [------]
         *   [+++++++++++++++]        -> [+++++++++++++++]
         */
        while (e != NULL && e->lba+e->len <= lba+len) {
            DEBUG_PRINT("overwriting %d,+%d -> %d.%d\n",  (int)e->lba, e->len,
                  e->pba.band, e->pba.offset);
            struct entry *tmp = stl_map_lba_iterate(v->map, e);
            stl_map_remove(v->map, e);
            e = tmp;
        }
        /*          [------]
         *   [+++++++++]        -> [++++++++++][---]
         */
        if (e != NULL && lba+len > e->lba) {
            DEBUG_PRINT("overlap head %d,+%d -> %d.%d becomes", (int)e->lba, e->len,
                  e->pba.band, e->pba.offset);
            int n = (lba+len)-e->lba;
            e->lba += n;
            if (!pba_eq(e->pba, PBA_INVALID))
                e->pba.offset += n;
            e->len -= n;
            DEBUG_PRINT(" %d,+%d -> %d.%d\n", (int)e->lba, e->len,
                  e->pba.band, e->pba.offset);
            stl_map_update(e, e->lba, e->pba, e->len);
            assert(e->len > 0);
            e->dirty = 1;
        }
    }

    /* When TRIM is passed down from host_trim, location=0,0 - set it
     * in the map, where it gets logged by checkpoint and then
     * removed from the map. When we read it on startup, location!=0,0
     * - all the work was done above by clearing the LBA range.
     */
    if (!pba_eq(pba, PBA_INVALID) || pba_eq(location, PBA_NULL)) {
        struct entry *_new = stl_map_entry(sizeof(*_new));
        *_new = (struct entry){.lba = lba, .pba = pba, .len = len,
                               .dirty = 1};
        stl_map_insert(v->map, _new, lba, pba, len);
    }
}

/*------------------- Initialization ---------------------*/

/* note that this works perfectly for TRIM logging, as well.
 */
static void read_map_record(struct volume *v, pba_t location,
                            struct map_record *m, uint32_t seq)
{
    assert(m->pba.band < v->n_bands &&
           (m->pba.offset+m->len) <= v->band_size &&
           (m->lba+m->len) <= v->group_span*v->n_groups);
    /* tag range with location it's checkpointed in */
    update_range(v, location, m->lba, m->len, m->pba);
}

static int group_of(struct volume *v, int band)
{
    return (band - 1 - v->map_size) / v->group_size;
}

static void read_band_record(struct volume *v, struct band_record *b,
                             uint32_t seq)
{
    assert(b->band < v->n_bands && b->type < BAND_TYPE_MAX);
    int i = b->band;
    v->band[i].type = b->type;
    v->band[i].dirty = 0;
    if (b->type == BAND_TYPE_FRONTIER) {
        v->groups[group_of(v, i)].frontier = i;
        v->groups[group_of(v, i)].frontier_offset = max(0, b->write_pointer - 1);
    }
}

static pba_t read_records(struct volume *v, pba_t location)
{
    void *buffer = NULL;
    buffer = malloc(SECTOR_SIZE, M_DEVBUF, M_WAITOK);
    memset(buffer, 0, SECTOR_SIZE);
    struct header *h = buffer;

    // XXX only during stlopen, so it's fine for now
    smr_read(v->disk, location.band, location.offset, buffer, 1);
    assert(h->magic == STL_MAGIC2);

    int i;
    uint32_t seq = h->seq;
    struct header h0 = *h;      /* saved copy */

    int nsectors = h0.next.offset - location.offset - 1;
    void *buf = NULL;

    if (nsectors > 0) {
        buf = malloc(nsectors * SECTOR_SIZE, M_DEVBUF, M_WAITOK);
        memset(buf, 0, nsectors * SECTOR_SIZE);
    }

    if (h0.type == RECORD_BAND) {
        struct band_record *r = (void*)(h+1);
        for (i = 0; i < h->local_records; i++)
            read_band_record(v, &r[i], seq);
        if (nsectors != 0) {
            smr_read(v->disk, location.band, location.offset+1, buf, nsectors);
            int hmax = nsectors * SECTOR_SIZE / sizeof(struct band_record);
            r = buf;
            for (i = 0; i < h0.records && i < hmax; i++)
                read_band_record(v, &r[i], seq);
        }
    }
    if (h0.type == RECORD_MAP) {
        struct map_record *m = (void*)(h+1);
        for (i = 0; i < h->local_records; i++)
            read_map_record(v, location, &m[i], seq);
        if (nsectors>0) {
            smr_read(v->disk, location.band, location.offset+1, buf, nsectors);
            int hmax = nsectors * SECTOR_SIZE / sizeof(struct map_record);
            m = buf;
            for (i = 0; i < h0.records && i < hmax; i++)
                read_map_record(v, location, &m[i], seq);
        }
    }

    if (buf)
        free(buf, M_DEVBUF);
    if (buffer)
        free(buffer, M_DEVBUF);
    v->map_prev = location;
    return h0.next;
}


#if 0
static int valid_pba(struct volume *v, pba_t pba)
{
    if (pba.band < 0 || pba.offset < 0)
        return 0;
    return pba.band < v->n_bands && pba.offset < v->band_size;
}
#endif

/* locking: not needed, since init_volume hasn't returned yet
 */
static int chase_frontiers(struct volume *v)
{
    void *buffer = NULL;
    buffer = malloc(SECTOR_SIZE, M_DEVBUF, M_WAITOK);
    memset(buffer, 0, SECTOR_SIZE);
    struct header *h = buffer;
    int i, max_seq = 0;

    for (i = 0; i < v->n_groups; i++) {
        int f = v->groups[i].frontier;
        if (v->band[f].write_pointer == 0) {
            continue;
        }
        int offset = v->groups[i].frontier_offset;
        if (offset >= v->band[f].write_pointer)
            continue;

        smr_read(v->disk, f, offset, buffer, 1);
        assert(h->magic == STL_MAGIC2);

        /* we really should try to handle bad data here, but it's hard...
         */
        while (h->next.offset < v->band[h->next.band].write_pointer){
            v->groups[i].prev = mkpba(f, offset);
            if (f != h->next.band) {
                assert(v->band[f].type == BAND_TYPE_FRONTIER);
                v->groups[i].count[BAND_TYPE_FULL]++;
		assert(v->groups[i].count[BAND_TYPE_FULL] <= v->group_size);

                v->groups[i].count[BAND_TYPE_FREE]--;
		assert(v->groups[i].count[BAND_TYPE_FREE] >= 0);

                v->band[f].type = BAND_TYPE_FULL;
                f = h->next.band;
                v->band[f].type = BAND_TYPE_FRONTIER;
                v->groups[i].frontier = f;
            }
            smr_read(v->disk, h->next.band, h->next.offset, buffer, 1);

            if (h->local_records > 0) {
                 read_map_record(v, PBA_NULL, (struct map_record*) (h+1), h->seq);
            }
            if (h->seq > max_seq) {
                max_seq = h->seq;
            }
        }
        if (f != h->next.band) {
            assert(v->band[f].type == BAND_TYPE_FRONTIER);
            v->groups[i].count[BAND_TYPE_FULL]++;
	    assert(v->groups[i].count[BAND_TYPE_FULL] <= v->group_size);

            v->groups[i].count[BAND_TYPE_FREE]--;
	    assert(v->groups[i].count[BAND_TYPE_FREE] >= 0);

            v->band[f].type = BAND_TYPE_FULL;
            f = h->next.band;
            v->band[f].type = BAND_TYPE_FRONTIER;
            v->groups[i].frontier = f;
        }
        //v->groups[i].frontier_offset = h->next.offset;
        v->groups[i].frontier_offset = v->band[f].write_pointer;
    }
    v->seq = max(v->seq, max_seq);
    if (buffer)
        free(buffer, M_DEVBUF);
    return 1;
}

static void defrag_thread(void *ctx)
{
    struct volume *v = ctx;
    struct timeval tv;
    int i, g = 0, ckpt = 0;

    while (false && __predict_true(!v->exit_threads)) {
    	yield();
	getmicrouptime(&tv);

	int64_t time_elapsed = (tv.tv_sec - v->last_op.tv_sec) * 1000000;
	time_elapsed += tv.tv_usec - v->last_op.tv_usec;

	// clean if we've been idle for a second
	int idle = time_elapsed > 1000000;

	mutex_enter(&v->m);
	off_t extents = stl_map_count(v->map);
	mutex_exit(&v->m);
	off_t vsize = v->n_groups * v->group_span; /* volume size in sectors */

        if (v->count > 2000 || (idle && v->count > 0)) {
            int type = CKPT_INCR;
            if (++ckpt > 10) {
                ckpt = 0;
                type = CKPT_FULL;
            }
            checkpoint_volume(v, type);
            v->count = 0;
        }

	/* target is 1 extent per 6MB or less, 6MB = 1536 sectors*/
	int n = (extents * 1536ULL) / vsize;

        if (!idle)
            n = n/2;
        for (i = 0; i < n; i++) {
            defrag_group(v, g++, idle);
            if (g >= v->n_groups)
                g = 0;
        }
    }
    kthread_exit(0);
}

static void cleaning_thread(void *ctx)
{
    int i;
    struct volume *v = ctx;
    struct timeval tv;

    // XXX some kind of race condition here... we hit an issue with stl_map_remove
    // via stl_write->alloc_extent->clean_group->stl_map_remove
    while (false && __predict_true(!v->exit_threads)) {
    	yield();
	getmicrouptime(&tv);

	int64_t time_elapsed = (tv.tv_sec - v->last_op.tv_sec) * 3000000;
	time_elapsed += tv.tv_usec - v->last_op.tv_usec;

	// clean if we've been idle for a second
	if (time_elapsed > 3000000) {
	    int min_i = -1, min_free = v->group_size+1;

	    for (i = 0; i < v->n_groups; i++) {
		int n = v->groups[i].count[BAND_TYPE_FREE];
		if (n < min_free) {
		    min_free = n;
		    min_i = i;
		}
	    }

	    assert(min_i >= 0);

	    // can't clean if everything's free (-1 for the frontier band)
	    if (min_free < v->group_size - 1) {
		clean_group(v, min_i, MINFREE_BG, PRIO_NORM, false);
	    }

	    // to avoid cleaning again immediately, consider this an operation
	    getmicrouptime(&v->last_op);
	}
    }

    // XXX not particularly clean, but fine for now
    //free(v, M_DEVBUF);


    kthread_exit(0);
}
//
//static int
//smr_read_helper(struct smr *dev, unsigned band, unsigned offset, void *buf,
//                unsigned n_sectors)
//{
//    struct iovec v = {.iov_base = (void*)buf, .iov_len = n_sectors*SECTOR_SIZE};
//    return smr_readv(dev, band, offset, &v, 1);
//}
//    /* break large writes into sections.
//     */
//    // XXX ROB
//    // XXX ROB can do like cgd and optimize this
//    struct buf *bp = getiobuf(dev->vn, true);
//
//    int n_sectors = iov_sum(iov, iovcnt);
//    assert(band < dev->n_bands && offset+n_sectors <= dev->band_size);
//
//    off_t position = (dev->band_size * (off_t)band + offset) * SECTOR_SIZE / 512;
//    //position += 524288;
//
//    bp->b_data = iov[0].iov_base;
//    bp->b_flags = B_READ;
//    bp->b_blkno = position;
//    bp->b_bcount = bp->b_resid = n_sectors * SECTOR_SIZE;
//    bp->b_proc = curproc;
//    BIO_SETPRIO(bp, BPRIO_DEFAULT);
//    SET(bp->b_cflags, BC_BUSY);
//
//    //int val = preadv(dev->fd, iov, iovcnt, position);
//
//    VOP_STRATEGY(dev->vn, bp);
//    int error = biowait(bp);
//    (void) error;
//
//    putiobuf(bp);
//}

struct volume *stl_open(struct smr *dev)
{
    int i, j, k, seq, m = 0;
    struct volume *v = malloc(sizeof(*v), M_DEVBUF, M_WAITOK);
    memset(v, 0x0, sizeof(*v));

    void *buffer = NULL;
    buffer = malloc(SECTOR_SIZE, M_DEVBUF, M_WAITOK);
    memset(buffer, 0, SECTOR_SIZE);

    mutex_init(&v->m, MUTEX_DEFAULT, IPL_NONE);
    cv_init(&v->c, "stlcond");
    v->defrag_begin = v->defrag_end = -1;
    v->map = stl_map_init();
    v->disk = dev;

    smr_read(v->disk, 0, 0, buffer, 1); /* read 1 sector */
    struct superblock *sb = buffer;
    // XXX cleanup
    if (sb->magic != STL_MAGIC) {
    	printf("Superblock magic number is wrong!\n");
    	return NULL;
    }

    /* note that we assume all bands are equal-sized. (ignore last one?)
     */
    v->band_size = smr_band_size(v->disk);

    // XXX this should be based on partition size
    v->n_bands = 256;//smr_n_bands(v->disk);
    v->band = malloc(v->n_bands * sizeof(v->band[0]), M_DEVBUF, M_WAITOK);
    memset(v->band, 0x0, v->n_bands * sizeof(v->band[0]));

    for (i = 0; i < v->n_bands; i++)
        v->band[i].write_pointer = smr_write_pointer(v->disk, i);

    v->map_size = sb->map_size;
    v->group_size = sb->group_size;
    v->group_span = sb->group_span;
    v->n_groups = sb->n_groups;
    v->groups = malloc(v->n_groups * sizeof(*v->groups), M_DEVBUF, M_WAITOK);
    memset(v->groups, 0x0, v->n_groups * sizeof(*v->groups));

    /* Find the current map band - i.e. the one starting with the
     * highest sequence number.
     */
    struct header *h = buffer;
    for (i = 1, seq = -1; i <= v->map_size; i++) {
    	printf("checking: %d %d\n", i, v->band[i].write_pointer);
        if (v->band[i].write_pointer == 0)
            continue;
        smr_read(v->disk, i, 0, buffer, 1);
        if (h->magic == STL_MAGIC2 && (int)h->seq > seq) {
            seq = h->seq;
            m = i;
        }
    }

    // not formatted correctly
    if (seq == -1 || m == 0) {
    	printf("Not formatted correctly");
    	return NULL;
    }

    /* Find the last record in the current map band by searching backwards from
     * write_pointer-1.  Offset of last legal header will be in 'i'.
     */
    v->map_band = m;
    for (i = v->band[m].write_pointer - 1; i > 0; i--) {

        smr_read(v->disk, m, i, buffer, 1);

        if (h->magic == STL_MAGIC2 && h->seq >= seq && h->next.band == m)
            break;
    }
    assert(i != -1);
    // ->band[v->map_band].write_pointer = i;
    v->seq = h->seq+1;

    /* now read forward from earliest relevant record to the end of
     * the checkpointed map and band information. Set the volume base
     * accordingly.
     */
    smr_read(v->disk, m, i, buffer, 1);
    pba_t base = v->base = h->base;
    seq = h->seq;
    while (base.band != m || base.offset != i)
        base = read_records(v, base);

    /* now we have the band map and the exception map as of the last
     * checkpoint. For now assume that drive is properly checkpointed.
     * In the future, log recovery proceeds in breadth-first / seq#
     * order starting with the tail of all the bands marked 'current'.
     */

    /* recover the state of all of the band groups.
     */
    for (i = 0, j = 1+v->map_size; i < v->n_groups; i++) {
        for (k = 0; k < v->group_size; j++, k++) {
            int type = v->band[j].type;
            if (type == BAND_TYPE_FRONTIER)
                v->groups[i].frontier = j;
            if (type == BAND_TYPE_FREE && v->band[j].write_pointer != 0) {
                smr_reset_pointer(v->disk, j);
                v->band[j].write_pointer = 0;
            }
            v->groups[i].count[type]++;
	    assert(v->groups[i].count[type] <= v->group_size);
        }
    }

    if (chase_frontiers(v)) {
        checkpoint_volume(v, CKPT_FULL);
    }
    if (buffer)
        free(buffer, M_DEVBUF);


    kthread_create(PRI_NONE, KTHREAD_MPSAFE, NULL, cleaning_thread, v, NULL, "stl cleaner");

    kthread_create(PRI_NONE, KTHREAD_MPSAFE, NULL, defrag_thread, v, NULL, "stl defragger");

    printf("STL Opened");

    return v;
}


/* Get size of unopened volume in 4K sectors - analogous to stat(2).st_size
 * note that this will not replay the log or write to the volume in any way.
 */
//off_t stl_size(const char *name)
//{
//    lba_t val = -1;
//    struct smr *disk;
//    if ((disk = smr_open(name)) == NULL)
//        return -1;
//
//    struct superblock *sb = malloc(SECTOR_SIZE, M_DEVBUF, M_WAITOK);
//    smr_read(disk, 0, 0, sb, 1); /* read 1 sector */
//
//    if (sb->magic == STL_MAGIC)
//        val = sb->group_span * sb->n_groups;
//
//    free(sb, M_DEVBUF);
//    smr_close(disk);
//
//    return val;
//}


/* Get size of opened volume in 4K sectors - analogous to fstat(2).st_size
 */
off_t stl_vsize(struct volume *v)
{
    return v->n_groups * v->group_span;
}


void stl_close(struct volume *v)
{
    // XXX not synchronized, though not horrible?
    printf("Closing %p\n", v);
    v->exit_threads += 1;
    //stl_map_destroy(v->map);
    //// shouldn't close this?
    //// smr_close(v->disk);
    //free(v->band, M_DEVBUF);
    //free(v->groups, M_DEVBUF);
    //cv_destroy(&v->c);
    //mutex_destroy(&v->m);
}

/* ---------- Cleaning ----------- */

static int cmp_ops_pba(const void *arg1, const void *arg2)
{
    const struct smr_op *op1 = arg1;
    const struct smr_op *op2 = arg2;
    if (op1->band != op2->band)
        return op1->band - op2->band;
    return op1->offset - op2->offset;
}

static int cmp_ops_lba(const void *arg1, const void *arg2)
{
    const struct smr_op *op1 = arg1;
    const struct smr_op *op2 = arg2;
    return op1->lba - op2->lba;
}

/* find the densest 32MB LBA region and re-write it. If typical track
 * is 1.6MB, then one extent per 8MB gives a read slowdown of <=17%.
 * note that the chunk size is 3 sectors less than 8MB so 32 of them fit
 * cleanly into a 256MB band.
 */
void defrag_group(struct volume *v, int g, int idle)
{
    lba_t begin = g * v->group_span;
    int chunksz = 8*1024*1024 / SECTOR_SIZE - 3; /* magic number warning */
    int n_regions = (v->group_span + chunksz - 1) / chunksz;
    int extents[n_regions], mass[n_regions];

    memset(extents, 0, sizeof(extents));
    memset(mass, 0, sizeof(mass));

    mutex_enter(&v->m);
    struct entry *e = stl_map_lba_geq(v->map, begin);
    while (e != NULL && e->lba < begin + v->group_span) {
        int i = (e->lba - begin) / chunksz;
        extents[i]++;
        int len = min(e->len, ((lba_t)(i + 1) * chunksz) - e->lba);
        mass[i] += len;
        int i2 = (e->lba + e->len - begin) / chunksz;
        if (i2 != i)
            extents[i2]++;
        e = stl_map_lba_iterate(v->map, e);
    }

    int max = 0, k = 0, i;
    for (i = 0; i < n_regions; i++) {
        if (extents[i] > max) {
            max = extents[i];
            k = i;
        }
    }

    if (max < 4 || (idle && mass[k] < chunksz/8) ||
        (!idle && mass[k] < chunksz/4)) { /* magic number alert */
        mutex_exit(&v->m);
        return;
    }

    printf("coalesce group %d : %d extents %d sectors %d\n",
           g, max, mass[k], stl_map_count(v->map));

    /* don't go off the end of the group.
     */
    lba_t end = begin + (k+1)*chunksz;
    if (end > begin + v->group_span)
        end = begin + v->group_span;
    begin = begin + k*chunksz;

    void *buf = malloc(chunksz*SECTOR_SIZE, M_DEVBUF, M_WAITOK);
    memset(buf, 0, chunksz*SECTOR_SIZE);
    struct smr_op ops[max+100];

    for (i = 0, e = stl_map_lba_geq(v->map, begin);
         e != NULL && e->lba < end;
         e = stl_map_lba_iterate(v->map, e)) {

        /* skip negative entries for TRIM.
         */
        if (pba_eq(PBA_INVALID, e->pba))
            continue;

        /* consider the case:
         *  +----- extent --------+
         *                    ^------- 8MB chunk -----^
         *  |<..... skip ....>|
         */
        lba_t _lba = max(begin, e->lba);
        lba_t skip = _lba - e->lba;
        ops[i].lba = _lba;
        ops[i].rw.r_buf = (char *)buf + (_lba - begin)*SECTOR_SIZE;
        ops[i].band = e->pba.band;
        ops[i].offset = e->pba.offset + skip;
        ops[i].len = e->len - skip;
        if (e->lba + e->len > end)
            ops[i].len = end - e->lba;
        i++;                    /* only if we didn't skip above */
    }

    /* only block reads/writes that interfere with this range
     */
    v->defrag_begin = begin;
    v->defrag_end = end;
    mutex_exit(&v->m);

    struct smr_op op;
    kheapsort(ops, i, sizeof(ops[0]), cmp_ops_pba, &op);

    smr_read_multi(v->disk, ops, i);


    struct buf *bp = getiobuf(v->disk->vn, true);
    bp->b_data = buf;
    bp->b_flags = B_WRITE;
    bp->b_cflags = BC_BUSY;
    // XXX if this is in terms of sectors, don't even need _stl_write
    bp->b_blkno = begin * SECTOR_SIZE / DEV_BSIZE;
    bp->b_bcount = (end - begin) * SECTOR_SIZE;
    // XXX semantically wrong, but does the right thing
    bp->b_iodone = do_write_header_done;
    bp->b_prio = BPRIO_DEFAULT;

    struct vnode *vp = bp->b_vp;
    mutex_enter(vp->v_interlock);
    vp->v_numoutput++;
    mutex_exit(vp->v_interlock);

    _stl_write(v, bp);

    mutex_enter(&v->m);
    v->defrag_begin = v->defrag_end = -1;
    cv_broadcast(&v->c);
    mutex_exit(&v->m);
}

static void do_write_multi(struct volume *v, int group,
                           struct smr_op *ops, int n_ops, int prio);


/* clean a single group. Returns true if cleaning was performed.
 */
static int clean_group(struct volume *v, int g, int minfree, int prio, bool lock_held)
{
    int i, nfree, made_changes = 0, iters = 0;
    int base = g * v->group_size + v->map_size + 1;
    int ret = 0;


    /* are there enough free bands?
     */
    if (!lock_held) {
	mutex_enter(&v->m); /* vvvvvvvvvvvvvvvvvvvvvv */
    }
    v->groups[g].cleaning = 1;

    for (i = nfree = 0; i < v->group_size; i++)
        if (v->band[base+i].type == BAND_TYPE_FREE)
            nfree++;

    assert(nfree == v->groups[g].count[BAND_TYPE_FREE]);

    int *seeks = malloc(v->group_size * sizeof(int), M_DEVBUF, M_WAITOK);
    int *sectors = malloc(v->group_size * sizeof(int), M_DEVBUF, M_WAITOK);
    while (v->groups[g].count[BAND_TYPE_FREE] <= minfree) {

        DEBUG_PRINT("CLEANING %d: free = %d iter %d\n", g,
               v->groups[g].count[BAND_TYPE_FREE], ++iters);

        // XXX we didn't necessarily actually clean anything?
        made_changes = 1;

	// XXX hard-coded... out of space
	if (iters >= 10) {
	    printf("Uh oh... out of space?\n");
	    ret = -1;
	    break;
	}

        /* find the cleaning cost of each band, by counting:
         * - total data (sectors) in band
         */
#define SECTORS_PER_SEEK 128
        memset(seeks, 0, v->group_size * sizeof(int));
        memset(sectors, 0, v->group_size * sizeof(int));

        pba_t begin = mkpba(base, 0);
        struct entry *e = stl_map_pba_geq(v->map, begin);

        while (e != NULL && e->pba.band < base+v->group_size) {
            i = e->pba.band - base;
            assert(i >= 0 && i < v->group_size);
            seeks[i]++;
            sectors[i] += e->len;
            e = stl_map_pba_iterate(v->map, e);
        }

        /* mark all the free bands so we don't try to clean them
         */
        for (i = 0; i < v->group_size; i++)
            if (v->band[i+base].type == BAND_TYPE_FREE ||
                v->band[i+base].type == BAND_TYPE_FRONTIER)
                sectors[i] = 1e9;

        int min_cost = 1e9, min_band = -1;
        for (i = 0; i < v->group_size; i++) {
#if 0
            int s = min(seeks[i], 1 + v->band_size / 512);
            if (iters > 1)
                s = 0;
            int cost = s*SECTORS_PER_SEEK + sectors[i];
#endif
            int cost = sectors[i];
            if (cost < min_cost) {
                min_cost = cost;
                min_band = i;
            }
        }
        // XXX this was an assert, but maybe something was
        // cleaned before we got the chance, which is fine?
        if (min_band < 0) {
            ret = -1;
            break;
	}

        /* read all the valid extents from this band
         */
        int band = min_band + base;
        int n_extents = seeks[min_band];

        //DEBUG_PRINT("PICKED %d - %d sectors\n", band, n_sectors);

	if (n_extents > 0) {
	    struct smr_op *ops = malloc(n_extents * sizeof(*ops), M_DEVBUF, M_WAITOK);
	    if (ops == NULL) {
	    	panic("oh no!");
	    }
	    begin = mkpba(band, 0);
	    e = stl_map_pba_geq(v->map, begin);
	    for (i = 0; e != NULL && e->pba.band == band; i++) {
	        //DEBUG_PRINT("moving %d.%d -> %d+%d\n", e->pba.band,
	        //	    e->pba.offset, (int)e->lba, e->len);
	        // XXX repeated mallocs are gross. use a pool?
	        ops[i] = (struct smr_op){
	            .lba = e->lba, .band = e->pba.band,
	            .offset = e->pba.offset, .len = e->len, .rw.r_buf = malloc(e->len * SECTOR_SIZE, M_DEVBUF, M_WAITOK)};
	        if (ops[i].rw.r_buf == NULL) {
	            panic("malloc failed!!\n");
		}
	        struct entry *tmp = stl_map_pba_iterate(v->map, e);
	        stl_map_remove(v->map, e);
	        e = tmp;
	        // XXX ugh, use read_multi
		mutex_exit(&v->m); /* ^^^^^^^^^^^^^^^^^^^^^^ */
	        smr_read(v->disk, ops[i].band, ops[i].offset, ops[i].rw.r_buf, ops[i].len);
		mutex_enter(&v->m); /* ^^^^^^^^^^^^^^^^^^^^^^ */
	    }

	    //printf("1\n");
	    // XXX what if we try to read te removed stuff from the map?

	    // XXX ROB errrr can we let go from alloc_extent like this?
	    //if (!lock_held)
		mutex_exit(&v->m); /* ^^^^^^^^^^^^^^^^^^^^^^ */
	    //}

	    assert(i == n_extents);

	    // XXX the ops skip over headers, so read_multi kept asserting?
	    //smr_read_multi(v->disk, ops, n_extents);
	    struct smr_op op;
	    kheapsort(ops, i, sizeof(ops[0]), cmp_ops_lba, &op);

	    // XXX not currently freeing buffer!
	    /* Now re-write them
	     */
	    do_write_multi(v, g, ops, i, prio);
	    //if (!lock_held) {
		mutex_enter(&v->m); /* vvvvvvvvvvvvvvvvvvvvvv */
	    //}
	    int k;
	    for (k = 0; k < i; k++) {
	    	free(ops[k].rw.r_buf, M_DEVBUF);
	    }
	    free(ops, M_DEVBUF);
            //free(buf, M_DEVBUF);
	}

        int type = v->band[band].type;

        v->groups[g].count[type]--;
	assert(v->groups[g].count[type] >= 0);
        v->groups[g].count[BAND_TYPE_FREE]++;

	assert(v->groups[g].count[BAND_TYPE_FREE] <= v->group_size);


	// find free band will do this
        smr_reset_pointer(v->disk, band);
        v->band[band].type = BAND_TYPE_FREE;
        v->band[band].dirty = 1;
        v->band[band].write_pointer = 0;

        nfree++;
    }

    v->groups[g].cleaning = 0;
    cv_broadcast(&v->c);
    if (!lock_held) {
	mutex_exit(&v->m);      /* ^^^^^^^^^^^^^^^^^^^^^^ */
    }
    free(seeks, M_DEVBUF);
    free(sectors, M_DEVBUF);

    // XXX hmm
    if (ret < 0) {
    	return ret;
    }
    return made_changes;
}

void clean_all(struct volume *v)
{
    int g, dirty;
    for (g = dirty = 0; g < v->n_groups; g++)
        dirty = dirty || clean_group(v, g, MINFREE_BG, PRIO_NORM, false);
    if (dirty)
        v->count = 3000;        /* force checkpoint */
}

/*------------ Write, allocate -----------*/

/* Find a free band in group 'g'.
 * Only grab the last band if 'prio' is true.
 */
static int find_free_band(struct volume *v, int g, int prio)
{
    int threshold = prio ? 0 : MINFREE_FG;
    int i, j = 1 + v->map_size + g*v->group_size, n;
    for (i = n = 0; i < v->group_size; i++, j++) {
        if (v->band[j].type == BAND_TYPE_FREE) {
            if (n >= threshold) {
                smr_reset_pointer(v->disk, j);
                return j;
            }
            n++;
        }
    }
    return -1;
}

/* assemble the header for a write packet into buffer
 */
static void fill_write_hdr(int seq, pba_t here, pba_t prev, pba_t next,
                           pba_t base, void *map, int n_records, void *buffer)
{
    memset(buffer, 0, SECTOR_SIZE);
    struct header *h = buffer;
    DEBUG_PRINT("fill_write_hdr: %d.%d (%d.%d)\n", here.band, here.offset,
          next.band, next.offset);
    *h = (struct header){.magic = STL_MAGIC2, seq,
                         .type = RECORD_DATA, .local_records = n_records,
                         .records = 0, .prev = prev, .next = next,
                         .base = base};
    memcpy(h+1, map, sizeof(struct map_record)*n_records);
}

static void do_write_header_done(struct buf *bp) {
    free(bp->b_data, M_DEVBUF);
    putiobuf(bp);
}

static int do_write_header(struct volume *v, struct buf *bp, int band, int offset, struct header *h) {

    struct buf *nbp = getiobuf(v->disk->vn, true);
    nbp->b_data = h;

    // XXX B_ASYNC?
    nbp->b_flags = B_WRITE;
    nbp->b_cflags = BC_BUSY;

    nbp->b_blkno = ((band * (off_t) v->band_size + offset) * SECTOR_SIZE) / DEV_BSIZE;
    nbp->b_bcount = SECTOR_SIZE;
    nbp->b_iodone = do_write_header_done;
    //BIO_COPYPRIO(nbp, bp);
    BIO_SETPRIO(nbp, BPRIO_DEFAULT);

    struct vnode *vp = nbp->b_vp;
    mutex_enter(vp->v_interlock);
    vp->v_numoutput++;
    mutex_exit(vp->v_interlock);

    // XXX hackish workaround to the fact that we don't want to submit this
    // buffer until we've used it to set up the trailing header
    int err = 0;
    if (bp != NULL) {
	err = smr_write_bp(v->disk, bp);
    }

    if (!err) {
	err = smr_write_bp(v->disk, nbp);
    }

    return err;
}

/* allocate a PBA extent. Updates the band map by advancing the write
 * pointer by 'len' sectors, marking it dirty, and setting sequence
 * number. Priority is passed down to find_free_band to avoid deadlock
 * on forced cleaning.
 *
 * locking: v->m is held when calling this.
 */
static pba_t alloc_extent(struct volume *v, int g, int len,
                    int prio, int *plen)
{
    struct group *gr = v->groups + g;
    int left, b;
    int err = 0;
    pba_t here;

    // XXX hard-coded, num_retries
    int tries = 8;
    while (tries > 0) {
	b = gr->frontier;
	left = v->band_size - v->band[b].write_pointer;
	here = (pba_t){.band = b, .offset = v->band[b].write_pointer};

	// XXX hard coded?
	if (left < 8) {
	    int b2 = find_free_band(v, g, prio);
	    if (b2 == -1) {
	    	err = clean_group(v, g, MINFREE_FG, PRIO_HIGH, true);
	    	// retry
	    	if (err == 0) {
		    v->count = 3000; /* force checkpoint */
		    continue;
		}
		// error
		else if (err < 0) {
		    printf("clean group returned error\n");
		    *plen = err;
		    return here;
		}
		else {
		    tries--;
		    continue;
		}
	    }
	    assert(b2 != -1);
	    gr->count[BAND_TYPE_FREE]--;
	    assert(gr->count[BAND_TYPE_FREE] >= 0);

	    assert(v->band[b2].write_pointer == 0);
	    pba_t next = {.band = b2, .offset = 0};

	    gr->count[BAND_TYPE_FULL]++;
	    assert(gr->count[BAND_TYPE_FULL] <= v->group_size);

	    v->band[b].type = BAND_TYPE_FULL;
	    v->band[b].dirty = 1;

	    void *buffer = malloc(SECTOR_SIZE, M_DEVBUF, M_WAITOK);
	    fill_write_hdr(v->seq++, here, v->groups[g].prev, next, v->base,
			NULL /* no map entry */, 0, buffer);
	    v->groups[g].prev = here;

	    err = do_write_header(v, NULL, here.band, here.offset, buffer);

	    // XXX weird error handling
	    if (err) {
		//printf("uh oh\n");
		*plen = -err;
		return here;
	    }

	    v->band[b].write_pointer++;

	    v->band[b2].type = BAND_TYPE_FRONTIER;
	    v->band[b2].dirty = 1;

	    gr->frontier = b2;
	    b = b2;
	    here = next;
	    left = v->band_size;
	}

	break;
    }

    if (tries == 0) {
    	// printf("uh oh\n");
	*plen = -1;
	return here;
    }

    /* caller is responsible for updating write_pointer
     */
    len = min(left-2, len);
    *plen = len;
    DEBUG_PRINT("alloc: returning %d.%d %d\n", b, v->band[b].write_pointer, len);
    return here;
}

static int sum_ops(struct smr_op *ops, int n_ops)
{
    int i, sum;
    for (i = sum = 0; i < n_ops; i++)
        sum += ops[i].len;
    return sum;
}

/* this is only used for cleaning, so we don't need to include the map
 * in the trailer - we're going to checkpoint this immediately anyway.
 */
static void do_write_multi(struct volume *v, int group,
                           struct smr_op *ops, int n_ops, int prio)
{
    void *buf1 = malloc(SECTOR_SIZE*2, M_DEVBUF, M_WAITOK), *buf2 = (char *)buf1+SECTOR_SIZE;
    struct smr_op *_ops = malloc(sizeof(*_ops)*(n_ops+2), M_DEVBUF, M_WAITOK);

    int alloced = 0, sectors = sum_ops(ops, n_ops);

    int i = 0; /* ops[i] */
    int borrowed = 0;
    while (sectors > 0) {
        mutex_enter(&v->m); /* vvvvvvvvvvvvvvvvvvvvvv */
        pba_t pba = alloc_extent(v, group, sectors+2, prio, &alloced);
        int _sectors = alloced-2, mapped = 0;

        struct smr_op *p = _ops;
        *p++ = (struct smr_op){.band = pba.band, .
                               offset = pba.offset, .len = 1, .rw.r_buf = buf1};

        pba_t here = pba_add(pba, 1);
        while (i < n_ops && mapped < _sectors) {
            *p++ = (struct smr_op){.lba = ops[i].lba + borrowed,
            .band = here.band, .offset = here.offset,
                                   .len = borrowed ? borrowed : ops[i].len,
                                   .rw.r_buf = (char *)ops[i].rw.r_buf + (borrowed*SECTOR_SIZE)};
            here.offset += borrowed ? borrowed : ops[i].len - borrowed;
            mapped += borrowed ? borrowed : ops[i].len - borrowed;
            borrowed = 0;
            i++;
        }
        borrowed = mapped - _sectors;
        if (borrowed > 0) {
            here.offset -= borrowed;
            p[-1].len -= borrowed;
            i--;                /* revisit this op in the next write */
        }
        *p++ = (struct smr_op){.band = here.band,
                               .offset = here.offset,
                               .len = 1, .rw.r_buf = buf2};
        int n = p - _ops;

        /* map entries haven't been logged yet, so location=PBA_NULL
         */

        fill_write_hdr(v->seq++, pba, v->groups[group].prev,
                       pba_add(pba, _sectors+1), v->base,
                       NULL /* no map entry */, 0, buf1);

        v->groups[group].prev = pba;

        fill_write_hdr(v->seq++, pba_add(pba, 1+_sectors),
                       v->groups[group].prev, pba_add(pba, _sectors+2),
                       v->base, NULL /* no map entry */, 0, buf2);
        v->groups[group].prev = pba_add(pba, 1+_sectors);

        v->band[pba.band].write_pointer += alloced;
        v->band[pba.band].dirty = 1;

        mutex_exit(&v->m); /* ^^^^^^^^^^^^^^^^^^^^^ */

	// XXX bleck, write_multi
	int k;
	for (k = 0; k < n; k++) {
	    smr_write(v->disk, _ops[k].band, _ops[k].offset, _ops[k].rw.w_buf, _ops[k].len);
	}

        mutex_enter(&v->m); /* vvvvvvvvvvvvvvvvvvvvvv */
        for (k = 1; k < n-1; k++) {
            update_range(v, PBA_NULL, _ops[k].lba, _ops[k].len,
                         mkpba(_ops[k].band, _ops[k].offset));
	}
        mutex_exit(&v->m); /* ^^^^^^^^^^^^^^^^^^^^^ */
        sectors -= _sectors;
    }
    free(buf1, M_DEVBUF);
    free(_ops, M_DEVBUF);
}

/* actually perform a write, wrapped with DATA records. 'prio' is used
 * to ensure that writes for forced cleaning can grab the last free
 * band.
 *
 */
static int do_write(struct volume *v, int group,
                     struct buf *bp, int prio)
{
    // only aligned writes should be passed here
    assert(bp->b_bcount % SECTOR_SIZE == 0);
    assert((bp->b_blkno * DEV_BSIZE) % SECTOR_SIZE == 0);

    int alloced = 0;
    int err;
    unsigned sectors = bp->b_bcount / SECTOR_SIZE;
    lba_t lba = bp->b_blkno * DEV_BSIZE / SECTOR_SIZE;
    uint64_t bp_offset = 0;

    // need to set this for nestedbufio
    bp->b_resid = bp->b_bcount;

    while (sectors > 0) {
        mutex_enter(&v->m); /* vvvvvvvvvvvvvvvvvvvvvv */
        while (v->groups[group].cleaning) /* wait for cleaning */ {
            cv_wait(&v->c, &v->m);
	}

        pba_t pba = alloc_extent(v, group, sectors+2, prio, &alloced);

	// alloc_extent erred
	if (alloced < 0) {
	    mutex_exit(&v->m);
	    return -alloced;
	}

        int _sectors = alloced-2;
        struct map_record map = {.lba = lba, .pba = pba_add(pba,1),
                                 .len = _sectors};

        /* map entries haven't been logged yet, so location=PBA_NULL
         */

	// should use pool!
	void *buf1 = malloc(SECTOR_SIZE, M_DEVBUF, M_WAITOK);
	void *buf2 = malloc(SECTOR_SIZE, M_DEVBUF, M_WAITOK);
        fill_write_hdr(v->seq++, pba, v->groups[group].prev,
                       pba_add(pba, _sectors+1), v->base,
                       NULL /* no map entry */, 0, buf1);

        v->groups[group].prev = pba;

        fill_write_hdr(v->seq++, pba_add(pba, 1+_sectors),
                       v->groups[group].prev, pba_add(pba, _sectors+2),
                       v->base, &map, 1, buf2);
        v->groups[group].prev = pba_add(pba, 1+_sectors);

        v->band[pba.band].write_pointer += alloced;
        v->band[pba.band].dirty = 1;

        mutex_exit(&v->m); /* ^^^^^^^^^^^^^^^^^^^^^ */

        int b = pba.band, o = pba.offset;

    	struct buf *nestbuf = getiobuf(v->disk->vn, true);
	nestiobuf_setup(bp, nestbuf, bp_offset, _sectors * SECTOR_SIZE);
    	//nestbuf->b_iodone = io_test2;
	nestbuf->b_blkno = ((b * (off_t) v->band_size + o + 1) * SECTOR_SIZE) / DEV_BSIZE;
	nestbuf->b_flags &= ~B_ASYNC;


        if ((err = do_write_header(v, NULL, b, o, buf1)) < 0 ||
	    (err = do_write_header(v, nestbuf, b, o+1+_sectors, buf2)) < 0) {
            return err;
	}

        mutex_enter(&v->m); /* vvvvvvvvvvvvvvvvvvvvvv */
        update_range(v, PBA_NULL, lba, _sectors, pba_add(pba, 1));
        mutex_exit(&v->m); /* ^^^^^^^^^^^^^^^^^^^^^ */

	sectors -= _sectors;
	lba += _sectors;
	bp_offset += _sectors * SECTOR_SIZE;
    }

    return 0;
}

struct stl_unaligned_io {
    struct buf *bp;
    int bytes;
};

static void stl_unaligned_done(struct buf *nbp)
{
    struct stl_unaligned_io *io = nbp->b_private;

    // XXX an error is theoretically possible here. base it
    // on nestiobuf_iodone?
    nestiobuf_done(io->bp, io->bytes, 0);
    free(io, M_DEVBUF);
    free(nbp->b_data, M_DEVBUF);
    putiobuf(nbp);
}


// XXX can err?
static int stl_handle_unaligned(struct volume *v, struct buf *bp, void *newbuf, int bytes)
{
    // absurd way to get right block number for the write
    int64_t byteno = bp->b_blkno * DEV_BSIZE / SECTOR_SIZE * SECTOR_SIZE;
    uint64_t group_bytes = v->group_span * SECTOR_SIZE;

    // XXX can theoretically reuse?
    struct buf *nbp = getiobuf(v->disk->vn, true);
    nbp->b_data = newbuf;
    nbp->b_flags = bp->b_flags;
    nbp->b_oflags = bp->b_oflags;
    nbp->b_cflags = bp->b_cflags;
    nbp->b_proc = bp->b_proc;
    nbp->b_blkno = byteno / DEV_BSIZE;
    nbp->b_bcount = SECTOR_SIZE;
    nbp->b_iodone = stl_unaligned_done;;
    SET(nbp->b_cflags, BC_BUSY);

    BIO_COPYPRIO(nbp, bp);
    struct vnode *vp = nbp->b_vp;
    mutex_enter(vp->v_interlock);
    vp->v_numoutput++;
    mutex_exit(vp->v_interlock);

    struct stl_unaligned_io *io = malloc(sizeof(*io), M_DEVBUF, M_WAITOK);
    io->bp = bp;
    io->bytes = bytes;
    nbp->b_private = io;

    int group = byteno / group_bytes;
    return do_write(v, group, nbp, PRIO_NORM);
}

/* FIXME - one of our test systems (pjd-fx) splits large writes into units
 * of 1022 sectors, or 255.5 4K blocks, resulting in zones converting to
 * non-sequential. Current limit in this code is 120 4K data blocks, plus 2
 * headers = 122 4K blocks or 976 sectors.
 * This should be parameterized or better yet discovered to match the
 * hardware limitations.
 */
static int _stl_write(struct volume *v, struct buf *bp)
{
    //printf("test1 %p test2 %p unaligned %p\n", io_test1, io_test2, stl_unaligned_done);
    /* internal writes can't span a group boundary
     */
     // XXX overflow issues here?
    int bcount = bp->b_bcount;
    int bp_offset = 0;
    int64_t byteno = bp->b_blkno * DEV_BSIZE;
    int64_t group_bytes = v->group_span * SECTOR_SIZE;
    int err;

    // need to set this for nestedbufio
    bp->b_resid = bcount;

    // we're starting partway into a sector
    if (byteno % SECTOR_SIZE != 0) {
    	int bytes = min(SECTOR_SIZE - byteno % SECTOR_SIZE, bcount);

	void *newbuf = malloc(SECTOR_SIZE, M_DEVBUF, M_WAITOK);
	memset(newbuf, 0, SECTOR_SIZE - bytes);
	memcpy((char *)newbuf + byteno % SECTOR_SIZE, bp->b_data, bytes);

    	if ((err = stl_handle_unaligned(v, bp, newbuf, bytes)) < 0) {
    	    return err;
	}

	byteno += bytes;
	bcount -= bytes;
	bp_offset += bytes;
    }


    while (bcount >= SECTOR_SIZE) {
        int group = byteno / group_bytes;
    	uint64_t _sectors = bcount / SECTOR_SIZE;
    	uint64_t sectors_remaining = (group + 1) * v->group_span - byteno / SECTOR_SIZE;
    	_sectors = min(_sectors, sectors_remaining);
        uint64_t bytes = _sectors * SECTOR_SIZE;

    	struct buf *nestbuf = getiobuf(v->disk->vn, true);
    	nestbuf->b_blkno = byteno / DEV_BSIZE;
	nestiobuf_setup(bp, nestbuf, bp_offset, bytes);
	nestbuf->b_flags &= ~B_ASYNC;
        if ((err = do_write(v, group, nestbuf, PRIO_NORM)) < 0) {
    	    return err;
	}

	byteno += bytes;
	bp_offset += bytes;
	bcount -= bytes;
    }

    if (bcount > 0) {
	void *newbuf = malloc(SECTOR_SIZE, M_DEVBUF, M_WAITOK);
	memcpy(newbuf, (char *)bp->b_data + bp_offset, bcount);
	memset((char *)newbuf + bcount, 0, SECTOR_SIZE - bcount);

	if ((err = stl_handle_unaligned(v, bp, newbuf, bcount)) < 0) {
    	    return err;
	}
    }

    return 0;
}

int stl_write(struct volume *v, struct buf *bp)
{
    lba_t lba = bp->b_blkno * DEV_BSIZE / SECTOR_SIZE;
    unsigned bcount = bp->b_bcount;
    int sectors = (bcount + SECTOR_SIZE - 1) / SECTOR_SIZE;

    if (lba + sectors > v->n_groups * v->group_span) {
	// the request goes past the end of the STL
    	return EINVAL;
    }

    DEBUG_PRINT("host_write %" PRId64 " %d\n", lba, sectors);

    getmicrouptime(&v->last_op);

    /* block if there's an interfering defrag operation
     */
    mutex_enter(&v->m);
    while (max(v->defrag_begin, lba) < min(v->defrag_end, lba+sectors))
        cv_wait(&v->c, &v->m);
    v->count++;                 /* and increment write count */
    mutex_exit(&v->m);

    getmicrouptime(&v->last_op);

    return _stl_write(v, bp);
}


// XXX do ffs or lfs have trim support?
void stl_trim(struct volume *v, lba_t lba, int sectors)
{
    assert(lba + sectors <= v->n_groups * v->group_span);

    DEBUG_PRINT("host_trim %" PRId64 " %d\n", lba, sectors);

    /* internal ops can't span a group boundary
     */
    while (sectors > 0) {
        pba_t null_pba = {.band = -1, .offset = -1};
        int group = lba / v->group_span;
        int _sectors = min(sectors, (group+1) * v->group_span - lba);

        /* map entries haven't been logged yet, so location=PBA_NULL
         */
        mutex_enter(&v->m); /* vvvvvvvvvvvvvvvvvvvvvv */
        update_range(v, PBA_NULL, lba, _sectors, null_pba);
        mutex_exit(&v->m); /* ^^^^^^^^^^^^^^^^^^^^^ */
        lba += _sectors;
        sectors -= _sectors;
    }
}

void stl_flush(struct volume *v)
{
    smr_flush(v->disk);
}

/*----------- Read logic --------------*/

//void stl_read(struct volume *v, lba_t lba, void *buf, int sectors)
int stl_read(struct volume *v, struct buf *bp)
{
    lba_t lba = bp->b_blkno * DEV_BSIZE / SECTOR_SIZE;
    int bcount = bp->b_bcount;
    int sectors = (bcount + SECTOR_SIZE - 1) / SECTOR_SIZE;

    if (lba + sectors > v->n_groups * v->group_span) {
    	printf("Read larger than device\n");
    	return EINVAL;
    }

    DEBUG_PRINT("host_read %" PRId64 " %d\n", lba, sectors);
    DEBUG_PRINT("host_read2 %" PRId64 " %u\n", bp->b_blkno, bcount);


    getmicrouptime(&v->last_op);

    // just deal with everything in bytes
    uint64_t bp_offset = 0;
    int64_t byteno = bp->b_blkno * DEV_BSIZE;

    // need to set this for nestedbufio
    bp->b_resid = bcount;

    while (bcount > 0) {

    	lba = byteno / SECTOR_SIZE;

	// XXX needed to add this?
        mutex_enter(&v->m);   /* vvvvvvvvvvvvvvvvvvvv */
        int group = lba / v->group_span;
        while (v->groups[group].cleaning) /* wait for cleaning */ {
            cv_wait(&v->c, &v->m);
	}
        struct entry *e = stl_map_lba_geq(v->map, lba);
        mutex_exit(&v->m); /* ^^^^^^^^^^^^^^^^^^^^ */

	lba_t e_lba;
	int32_t e_len;
	if (e != NULL) {
	    e_lba = e->lba * SECTOR_SIZE;
	    e_len = e->len * SECTOR_SIZE;
	}


	// finish off the rest of the buf
        if (e == NULL || e_lba >= byteno + bcount) {
            memset((char *)bp->b_data + bp_offset, 0, bcount);
            nestiobuf_done(bp, bcount, 0);
            return 0;
        }

        int offset = max(byteno - e_lba, -bcount), len = -offset;

        if (offset < 0) {
            memset((char *)bp->b_data + bp_offset, 0, len);
            nestiobuf_done(bp, len, 0);
            bcount -= len;
            byteno += len;
            bp_offset += len;
            offset = 0;
        }

        len = min(bcount, e_len - offset);
        if (len > 0) {
            if (pba_eq(e->pba, PBA_INVALID)) {
                memset((char *)bp->b_data + bp_offset, 0, len);
                nestiobuf_done(bp, len, 0);
	    }
            else {
                assert(e->pba.band < v->n_bands && e->pba.offset * SECTOR_SIZE + offset <= v->band_size * SECTOR_SIZE);

            	struct buf *nestbuf = getiobuf(v->disk->vn, true);
                nestiobuf_setup(bp, nestbuf, bp_offset, len);
		nestbuf->b_blkno = (((v->band_size * (off_t)e->pba.band + e->pba.offset) * SECTOR_SIZE) + offset) / DEV_BSIZE;
                smr_read_bp(v->disk, nestbuf);
	    }
        }
        bcount -= len;
        byteno += len;
        bp_offset += len;
    }

    getmicrouptime(&v->last_op);
    return 0;
}

/*----------- Map checkpointing --------------*/

static pba_t fill_meta(int type, int n_records, pba_t location, pba_t prev,
		       pba_t next, pba_t base, int seq, void *buffer)
{
    memset(buffer, 0, SECTOR_SIZE);
    struct header *h = buffer;
    if (pba_eq(next, PBA_NEXT))
	next = mkpba(location.band, location.offset+1);
    DEBUG_PRINT("fill_meta: %d %d base %d.%d\n", type, n_records,
                base.band, base.offset);
    *h = (struct header){.magic = STL_MAGIC2, .seq = seq,
                         .type = type, .local_records = 0,
                         .records = n_records, .prev = prev,
                         .next = next, .base = base};
    return location;
}

static const int map_per_sector = SECTOR_SIZE / sizeof(struct map_record);
static const int band_per_sector = SECTOR_SIZE / sizeof(struct band_record);


static void check_for_space(struct volume *v, int n_sectors)
{
    /* make sure there's enough room in the current map band.
     */
    int i = v->map_band;
    int next_band = (i >= v->map_size) ? 1 : i+1;
    int needed = n_sectors + 2;

    /* sufficiently rare case that we can just lock the whole thing
     */
    mutex_enter(&v->m);  /* vvvvvvvvvvvvvvvvvvvv */
    if (v->band[i].write_pointer + needed >= v->band_size) {
	void *buffer = malloc(SECTOR_SIZE, M_DEVBUF, M_WAITOK);

	v->map_prev = fill_meta(RECORD_NULL, 0,
				mkpba(i, v->band[i].write_pointer),
				v->map_prev, mkpba(next_band, 0),
				v->base, v->seq++, buffer);
	// XXX translate this to a bp call?
	smr_write(v->disk, i, v->band[i].write_pointer, buffer, 1);

        v->map_band = next_band;
        v->band[next_band].write_pointer = 0;
        smr_reset_pointer(v->disk, next_band);
	free(buffer, M_DEVBUF);
    }
    mutex_exit(&v->m); /* ^^^^^^^^^^^^^^^^^^^^ */
}

/* write map updates and then band updates into map band.
 */

/* this works even for non-power-of-2 sizes */
#define ROUND_UP(n, size) (((n+size-1)/size)*size)

// first do full checkpoint
void checkpoint_volume(struct volume *v, enum ckpt type)
{
    void *buffer1 = malloc(SECTOR_SIZE, M_DEVBUF, M_WAITOK);
    void *buffer2 = malloc(SECTOR_SIZE, M_DEVBUF, M_WAITOK);
    int i, n_records, m = v->map_band;
    pba_t new_base = PBA_INVALID;

    DEBUG_PRINT("checkpoint_volume %d\n", v->count);

    /* write out all the band records
     */
    int band_len = ROUND_UP(v->n_bands * sizeof(struct band_record),
			    SECTOR_SIZE);
    struct band_record *bands = malloc(band_len, M_DEVBUF, M_WAITOK);
    memset(bands, 0, band_len);

    mutex_enter(&v->m);  /* vvvvvvvvvvvvvvvvvvvv */
    for (i = 1+v->map_size, n_records = 0; i < v->n_bands; i++)
        if (type == CKPT_FULL || v->band[i].dirty) {
	    bands[n_records++] = (struct band_record)
                {.band = i, .type = v->band[i].type,
		 .write_pointer = v->band[i].write_pointer};
            v->band[i].dirty = 0;
        }
    mutex_exit(&v->m); /* ^^^^^^^^^^^^^^^^^^^^ */

    if (n_records > 0) {
        int n_sectors = (n_records * sizeof(struct band_record) +
			 SECTOR_SIZE - 1) / SECTOR_SIZE;
        check_for_space(v, n_sectors);


	m = v->map_band;
        new_base = mkpba(m, v->band[m].write_pointer);
        pba_t next = mkpba(m, (v->band[m].write_pointer + 1 + n_sectors));

	v->map_prev = fill_meta(RECORD_BAND, n_records,
				mkpba(m, v->band[m].write_pointer),
				v->map_prev, next, v->base, v->seq++, buffer1);
	smr_write(v->disk, m, v->band[m].write_pointer++, buffer1, 1);

        smr_write(v->disk, m, v->band[m].write_pointer, bands, n_sectors);
        v->band[m].write_pointer += n_sectors;

	v->map_prev = fill_meta(RECORD_BAND, 0,
				mkpba(m, v->band[m].write_pointer),
				v->map_prev, PBA_NEXT, v->base, v->seq++,
				buffer1);
	smr_write(v->disk, m, v->band[m].write_pointer++, buffer1, 1);
    }

    free(bands, M_DEVBUF);

    /* checkpoint map entries. always checkpoint the dirty ones.
     */
    mutex_enter(&v->m);  /* vvvvvvvvvvvvvvvvvvvv */

    int n_map = stl_map_count(v->map); /* lock ALL accesses to map */
    int map_len = ROUND_UP(n_map * sizeof(struct map_record), SECTOR_SIZE);
    struct map_record *map = malloc(map_len, M_DEVBUF, M_WAITOK);
    memset(map, 0, map_len);

    m = v->map_band;

    struct entry *e = stl_map_lba_iterate(v->map, NULL);
    n_records = 0;
    while (e != NULL) {
        struct entry *tmp = stl_map_lba_iterate(v->map, e);
        if (type == CKPT_FULL || e->dirty) {
            map[n_records++] = (struct map_record)
                {.lba = e->lba, .pba = e->pba, .len = e->len};
	    e->dirty = 0;
            if (pba_eq(e->pba, PBA_INVALID)) {/* TRIM gets logged once */
                stl_map_remove(v->map, e);   /* and then removed */
            }
	}
        e = tmp;
    }
    mutex_exit(&v->m); /* ^^^^^^^^^^^^^^^^^^^^ */

    /* don't write anything if nothing changed.
     */
    if (n_records > 0) {
        DEBUG_PRINT("checkpoint at %d\n", v->seq);
        for (i = 0; i < n_records; i++)
            DEBUG_PRINT(" %d,+%d -> %d.%d\n", (int)map[i].lba, map[i].len,
                  map[i].pba.band, map[i].pba.offset);
        int n_sectors = (n_records * sizeof(struct map_record) +
			 SECTOR_SIZE - 1) / SECTOR_SIZE;

        DEBUG_PRINT("checkpoint map %s (%d) at %d\n", type == CKPT_FULL ? "full" : "incr", n_records, v->seq);
        check_for_space(v, n_sectors);

	m = v->map_band;
        pba_t next =  mkpba(m, (v->band[m].write_pointer + n_sectors + 1));
        struct smr_op ops[3];

        mutex_enter(&v->m); /* vvvvvvvvvvvvvvvvvvvv */
	v->map_prev = fill_meta(RECORD_MAP, n_records,
				mkpba(m, v->band[m].write_pointer),
				v->map_prev, next, v->base, v->seq++, buffer1);
        ops[0] = (struct smr_op){.band = m,
                                 .offset = v->band[m].write_pointer,
                                 .rw.r_buf = buffer1, .len = 1};
        v->band[m].write_pointer++;

        ops[1] = (struct smr_op){.band = m,
                                 .offset = v->band[m].write_pointer,
                                 .rw.r_buf = map, .len = n_sectors};
        v->band[m].write_pointer += n_sectors;

        if (type == CKPT_FULL) {
            DEBUG_PRINT("base %d.%d -> %d.%d\n", v->base.band, v->base.offset,
                        new_base.band, new_base.offset);
            v->base = new_base;
        }

	v->map_prev = fill_meta(RECORD_MAP, 0,
				mkpba(m, v->band[m].write_pointer),
				v->map_prev, PBA_NEXT, v->base, v->seq++,
				buffer2);

	// XXX ROB -- conversion to write_bp in progress
	//do_write_header(v, bp, b, o, buffer1, false);
        ops[2] = (struct smr_op){.band = m,
                                 .offset = v->band[m].write_pointer,
                                 .rw.r_buf = buffer2, .len = 1};
        v->band[m].write_pointer++;
        mutex_exit(&v->m); /* ^^^^^^^^^^^^^^^^^^^^ */
        smr_write_multi(v->disk, ops, 3);
    }
    free(map, M_DEVBUF);

    free(buffer1, M_DEVBUF);
    free(buffer2, M_DEVBUF);
}

/*------------ the rest --------------*/
