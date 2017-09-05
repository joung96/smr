/*
 * file:        stl_base.c
 * description: basic shingle translation layer logic
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>

/* Apple OSX rbtree implementation
 */
#include "rbtree.h"

#include "stl.h"
#include "stl_map.h"
#include "stl_fakesmr.h"
#include "stl_base.h"

#define max(a, b) (((a) > (b)) ? (a) : (b))
#define min(a, b) (((a) < (b)) ? (a) : (b))

/* todo - sequence number arithmetic with signed sequence #s?
 * or just use 64-bit sequence number?
 */

/*---------- Prototypes -------------*/

static void do_write(struct volume *v, int group, lba_t lba,
                     const void *buf, int sectors, int prio);

/*----------- Helper functions for band/offset PBAs ---------------*/

int64_t pba2long(struct volume *v, pba_t pba)
{
    return (int64_t)pba.band * v->band_size + pba.offset;
}

pba_t long2pba(struct volume *v,  int64_t addr)
{
    return (struct pba){.band = addr / v->band_size,
            .offset = addr % v->band_size};
}


#define PBA_NEXT (struct pba){.band = 0xFFFFFFFF, .offset=0xFFFFFFFF}


/* Update mapping. Removes any total overlaps, edits any partial
 * overlaps, adds new extent to forward and reverse map.
 */
static void update_range(struct volume *v, pba_t location, lba_t lba, int len,
                         pba_t pba, uint32_t seq)
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

            pba_t new_pba = pba_add(e->pba, (e->len - new_len));
            struct entry *_new2 = stl_map_entry(sizeof(struct entry));
            *_new2 = (struct entry){.lba = lba+len, .pba = new_pba,
                                    .len = new_len, .seq = seq, .dirty = 1};

            printf("split %d,+%d -> %d.%d into ",
                   (int)e->lba, e->len, e->pba.band, e->pba.offset);
                   
            e->len = lba - e->lba; /* do this *before* inserting below */
            printf("%d,+%d -> %d.%d %d,+%d -> %d.%d\n",
                   (int)e->lba, e->len, e->pba.band, e->pba.offset,
                   (int)_new2->lba, _new2->len, _new2->pba.band, _new2->pba.offset);
            stl_map_update(e, e->lba, e->pba, e->len);
            e->dirty = 1;
            e->seq = seq;
            assert(e->len > 0);
            stl_map_insert(v->map, _new2, _new2->lba, _new2->pba, _new2->len);
            e = _new2;
        }
        /* [------------]
         *        [+++++++++]        -> [------][+++++++++]
         */
        else if (e->lba < lba) {
            printf("overlap tail %d,+%d -> %d.%d becomes", (int)e->lba, e->len,
                   e->pba.band, e->pba.offset);
            e->len = lba - e->lba;
            printf(" %d,+%d -> %d.%d\n", (int)e->lba, e->len, 
                   e->pba.band, e->pba.offset);
            stl_map_update(e, e->lba, e->pba, e->len);
            assert(e->len > 0);
            e->dirty = 1;
            e->seq = seq;
            e = stl_map_lba_iterate(v->map, e);
        }
        /*          [------]
         *   [+++++++++++++++]        -> [+++++++++++++++]
         */
        while (e != NULL && e->lba+e->len <= lba+len) {
            printf("overwriting %d,+%d -> %d.%d\n",  (int)e->lba, e->len,
                   e->pba.band, e->pba.offset);
            struct entry *tmp = stl_map_lba_iterate(v->map, e);
            stl_map_remove(v->map, e);
            e = tmp;
        }
        /*          [------]
         *   [+++++++++]        -> [++++++++++][---]
         */
        if (e != NULL && lba+len > e->lba) {
            printf("overlap head %d,+%d -> %d.%d becomes", (int)e->lba, e->len,
                   e->pba.band, e->pba.offset);
            int n = (lba+len)-e->lba;
            e->lba += n;
            e->pba.offset += n;
            e->len -= n;
            printf(" %d,+%d -> %d.%d\n", (int)e->lba, e->len, 
                   e->pba.band, e->pba.offset);
            stl_map_update(e, e->lba, e->pba, e->len);
            assert(e->len > 0);
            e->seq = seq;
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
                               .seq = seq, .dirty = 1};
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
    update_range(v, location, m->lba, m->len, m->pba, seq);
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
    v->band[i].seq = seq;
    if (b->type == BAND_TYPE_FRONTIER) { 
        v->groups[group_of(v, i)].frontier = i;
        v->groups[group_of(v, i)].frontier_offset = b->write_pointer; 
    }
}

static pba_t read_records(struct volume *v, pba_t location)
{
    struct header *h = v->buf;
    smr_read(v->disk, location.band, location.offset, v->buf, 1);
    assert(h->magic == STL_MAGIC);

    int i;
    uint32_t seq = h->seq;
    struct header h0 = *h;      /* saved copy */

    int nsectors = h0.next.offset - location.offset - 1;
    void *buf = NULL;

    if (h0.records > 0)
        buf = valloc(nsectors * SECTOR_SIZE);

    if (h0.type == RECORD_BAND) {
        struct band_record *r = (void*)(h+1);
        for (i = 0; i < h->local_records; i++)
            read_band_record(v, &r[i], seq);
        if (nsectors != 0){
            smr_read(v->disk, location.band, location.offset+1, buf, nsectors);
        }
        int hmax = nsectors * SECTOR_SIZE / sizeof(struct band_record);
        r = buf;
        for (i = 0; i < h0.records && i < hmax; i++)
            read_band_record(v, &r[i], seq);
    }
    if (h0.type == RECORD_MAP) {
        struct map_record *m = (void*)(h+1);
        for (i = 0; i < h->local_records; i++)
            read_map_record(v, location, &m[i], seq);
        if(nsectors>0){
            smr_read(v->disk, location.band, location.offset+1, buf, nsectors);
        }
        int hmax = nsectors * SECTOR_SIZE / sizeof(struct map_record);
        m = buf;
        for (i = 0; i < h0.records && i < hmax; i++)
            read_map_record(v, location, &m[i], seq);
    }

    if (buf)
        free(buf);
    v->map_prev = location;
    return h0.next;
}

struct data_pkt {
    struct header h;
    struct map_record map;
};

static int chase_frontiers(struct volume *v)
{
    struct header *h = v->buf;
    int i, max_seq, dirty = 0;
    
    for (i = 0; i < v->n_groups; i++) {
        int f = v->groups[i].frontier;
        if (v->band[f].write_pointer == 0) {
            continue;
        }
        int offset = v->groups[i].frontier_offset;
        smr_read(v->disk, f, offset, v->buf, 1);
        while (h->next.offset < v->band[h->next.band].write_pointer){
            smr_read(v->disk, h->next.band, h->next.offset, v->buf, 1);
            if (h->local_records > 0) {
                 read_map_record(v, PBA_NULL, (struct map_record*) (h+1), h->seq);
            }
            if (h->seq > max_seq) {
                max_seq = h->seq;
            }
        }
    }
    v->seq = max_seq;
    return 1;
}

/* track them down in order of sequence number and apply them.
 */
// static int chase_frontiers(struct volume *v)
// {
//     struct header *h = v->buf;
//     struct data_pkt *hdrs = malloc(v->n_groups*sizeof(*hdrs));
//     int i, n_valid, dirty = 0, *seq = malloc(v->n_groups*sizeof(int));

//     for (i = n_valid = 0; i < v->n_groups; i++) {
//         int f = v->groups[i].frontier;
//         if (v->band[f].write_pointer == 0)
//             continue;
//         int offset = v->band[f].write_pointer - 1;
//         smr_read(v->disk, f, offset, v->buf, 1);
//         if (h->magic == STL_MAGIC) {
//             memcpy(&hdrs[n_valid], h, sizeof(hdrs[0]));
//             seq[n_valid++] = h->seq;
//         }
//     }

//     while (n_valid > 0) {
//         i = find_min(seq, n_valid);
//         assert(seq[i] == hdrs[i].h.seq);
//         if (hdrs[i].h.seq >= v->seq) {
//             dirty = 1;
//             if (hdrs[i].h.local_records > 0) 
//                 read_map_record(v, PBA_NULL, &hdrs[i].map, seq[i]);
//             v->seq = seq[i];
//             pba_t next = hdrs[i].h.next;
//             // v->band[next.band].write_pointer = next.offset;
//         }

//         /* try to grab the next sector; if not there, then pull out of
//          * list. note that this will need fixing for real SMR host-aware.
//          */

    
//         int this_band = hdrs[i].h.next.band;
//         if (hdrs[i].h.next.offset < v->band[this_band].write_pointer){
//             smr_read(v->disk, hdrs[i].h.next.band, hdrs[i].h.next.offset, v->buf, 1);
//         } else{
//             break;
//         }

        
//         if (h->magic != STL_MAGIC || h->seq < seq[i]) {
//             /*
//              * stupid C data structures...
//              */
//             int n = n_valid - i - 1;
//             memmove(&hdrs[i], &hdrs[i+1], n * sizeof(hdrs[0]));
//             memmove(&seq[i], &seq[i+1], n * sizeof(seq[0]));
//             n_valid--;
//         }
//         else {
//             memcpy(&hdrs[i], h, sizeof(hdrs[n_valid]));
//             seq[i] = h->seq;
//             printf("updating wp[%d] -> %d\n", this_band, h->next.offset);
//             //v->band[this_band].write_pointer = h->next.offset;
//         }
//     }
//     free(hdrs);
//     free(seq);
//     return dirty;
// }


struct volume *init_volume(const char *dev)
{
    int i, j, k, seq, m;
    struct volume *v = calloc(sizeof(*v), 1);
    v->buf = valloc(SECTOR_SIZE);

    v->map = stl_map_init();

    if ((v->disk = smr_open(dev)) == NULL)
        return NULL;

    smr_read(v->disk, 0, 0, v->buf, 1); /* read 1 sector */
    struct superblock *sb = v->buf;
    assert(sb->magic == STL_MAGIC);

    /* note that we assume all bands are equal-sized. (ignore last one?)
     */
    v->band_size = smr_band_size(v->disk);
    v->n_bands = smr_n_bands(v->disk);
    v->band = calloc(v->n_bands * sizeof(v->band[0]), 1);
    for (i = 0; i < v->n_bands; i++)
        v->band[i].write_pointer = smr_write_pointer(v->disk, i);
    
    v->map_size = sb->map_size;
    v->group_size = sb->group_size;
    v->group_span = sb->group_span;
    v->n_groups = sb->n_groups;
    v->groups = calloc(v->n_groups * sizeof(*v->groups), 1);

    /* Find the current map band - i.e. the one starting with the
     * highest sequence number.
     */
    struct header *h = v->buf;
    for (i = 1, seq = -1; i <= v->map_size; i++) {
        if (v->band[i].write_pointer == 0)
            continue;
        smr_read(v->disk, i, 0, v->buf, 1);
        if (h->magic == STL_MAGIC && (int)h->seq > seq) {
            seq = h->seq;
            m = i;
        }
    }
    assert(seq > -1);

    /* Find the last record in the current map band by searching backwards from
     * write_pointer-1.  Offset of last legal header will be in 'i'. 
     */
    v->map_band = m;
    for (i = v->band[m].write_pointer - 1; i > 0; i--) {
       
        smr_read(v->disk, m, i, v->buf, 1);
    
        if (h->magic == STL_MAGIC && h->seq >= seq && h->next.band == m)
            break;
    }
    assert(i != -1);
    // ->band[v->map_band].write_pointer = i;
    v->seq = h->seq+1;
    
    /* now read forward from earliest relevant record to the end of
     * the checkpointed map and band information. Set the volume base
     * accordingly.
     */
    smr_read(v->disk, m, i, v->buf, 1);
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
            v->groups[i].count[type]++;
        }
    }

    if (chase_frontiers(v))
        checkpoint_volume(v);
    return v;
}

int64_t volume_size(struct volume *v)
{
    return v->n_groups * v->group_span * SECTOR_SIZE;
}

void delete_volume(struct volume *v)
{
    stl_map_destroy(v->map);
    smr_close(v->disk);
    free(v->buf);
    free(v->band);
    free(v->groups);
}

/* ---------- Cleaning ----------- */

/* clean a single group. Returns true if cleaning was performed.
 */
static int clean_group(struct volume *v, int g, int minfree, int prio)
{
    int i, nfree, made_changes = 0, iters = 0;
    int base = g * v->group_size + v->map_size + 1;

    /* are there enough free bands?
     */
    for (i = nfree = 0; i < v->group_size; i++)
        if (v->band[base+i].type == BAND_TYPE_FREE)
            nfree++;

    while (v->groups[g].count[BAND_TYPE_FREE] <= minfree) {
        printf("CLEANING %d: free = %d iter %d\n", g,
               v->groups[g].count[BAND_TYPE_FREE], ++iters);
	checkpoint_volume(v);
        made_changes = 1;

        assert(iters < 10);

        /* find the cleaning cost of each band, by counting:
         * - total data (sectors) in band
         * - approximate seeks to clean band (cap at ~#tracks/band)
         */
#define SECTORS_PER_SEEK 128
        int *seeks = calloc(v->group_size * sizeof(int), 1);
        int *sectors = calloc(v->group_size * sizeof(int), 1);
        //int seeks[v->group_size], sectors[v->group_size];
        //for (i = 0; i < v->group_size; i++)
        //seeks[i] = sectors[i] = 0;

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
        assert(min_band >= 0);

        /* read all the valid extents from this band
         */
        int band = min_band + base, n_sectors = sectors[min_band];
        int n_extents = seeks[min_band];
        void *buf = valloc(n_sectors * SECTOR_SIZE);
        int *len = calloc(n_extents * sizeof(int), 1);
        int64_t *lba = calloc(n_extents * sizeof(int64_t), 1);
        //int   len[n_extents];
        //lba_t lba[n_extents];
        void *ptr = buf;

        printf("PICKED %d - %d sectors\n", band, n_sectors);

        pba_t _pba[n_extents];
        begin = mkpba(band, 0);
        e = stl_map_pba_geq(v->map, begin);
        for (i = 0; e != NULL && e->pba.band == band; i++) {
            smr_read(v->disk, e->pba.band, e->pba.offset, ptr, e->len);
            len[i] = e->len;
            lba[i] = e->lba;
            _pba[i] = e->pba;
            ptr += (e->len * SECTOR_SIZE);
            struct entry *tmp = stl_map_pba_iterate(v->map, e);
            stl_map_remove(v->map, e);
            e = tmp;
        }

        /* Now re-write them
         */
        for (i = 0, ptr = buf; i < n_extents; i++) {
            printf("moving %d from %d.%d\n", (int)lba[i], _pba[i].band,
                   _pba[i].offset);
            do_write(v, g, lba[i], ptr, len[i], prio);
            ptr += len[i] * SECTOR_SIZE;
        }

        int type = v->band[band].type;
        v->groups[g].count[type]--;
        v->groups[g].count[BAND_TYPE_FREE]++;
        v->band[band].type = BAND_TYPE_FREE;
        v->band[band].dirty = 1;
        v->band[band].write_pointer = 0;

        free(buf);
        free(seeks); free(sectors);
        free(len); free(lba);
        nfree++;
    }
    return made_changes;
}

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

void clean_all(struct volume *v)
{
    int g, dirty;
    for (g = dirty = 0; g < v->n_groups; g++)
        dirty = dirty || clean_group(v, g, MINFREE_BG, PRIO_NORM);
    if (dirty)
        checkpoint_volume(v);
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
            if (n >= threshold)
                return j;
            n++;
        }
    }
    return -1;
}

/* assemble the header for a write packet into v->buf and write it at
 * the head of 'band'. Updates sequence#.
 * use the same header (with modifications to 'prev' and 'next') at tail.
 */
static void do_write_hdr(struct volume *v, pba_t here, pba_t prev,
                  pba_t next, int band, void *map, int n_records)
{
    v->band[band].seq = v->seq;
    memset(v->buf, 0, SECTOR_SIZE);
    struct header *h = v->buf;
    printf("do_write_hdr: %d.%d (%d.%d)\n", here.band, here.offset,
           next.band, next.offset);
    *h = (struct header){.magic = STL_MAGIC, .seq = v->seq++,
                         .type = RECORD_DATA, .local_records = n_records,
                         .records = 0, .prev = prev, .next = next,
                         .base = v->base};
    memcpy(h+1, map, sizeof(struct map_record)*n_records);
    smr_write(v->disk, here.band, here.offset, h, 1);
}

/* allocate a PBA extent. Updates the band map by advancing the write
 * pointer by 'len' sectors, marking it dirty, and setting sequence
 * number. Priority is passed down to find_free_band to avoid deadlock
 * on forced cleaning.
 */
static pba_t alloc_extent(struct volume *v, int g, int len, 
                    int prio, int *plen)
{
    struct group *gr = v->groups + g;
    int left, b;
top:
    b = gr->frontier;
    left = v->band_size - v->band[b].write_pointer;
    pba_t here = {.band = b, .offset = v->band[b].write_pointer};
    if (left < 8) {
        pba_t prev = {.band = b, .offset = v->band[b].write_pointer-1};
        int b2 = find_free_band(v, g, prio);
        if (b2 == -1) {
            if (clean_group(v, g, MINFREE_FG, PRIO_HIGH))
                checkpoint_volume(v);
            goto top;
        }
        assert(b2 != -1);
        gr->count[BAND_TYPE_FREE]--;
        assert(v->band[b2].write_pointer == 0);
        pba_t next = {.band = b2, .offset = 0};

        gr->count[BAND_TYPE_FULL]++;
        v->band[b].type = BAND_TYPE_FULL;
        v->band[b].dirty = 1;
        v->band[b].write_pointer++;
        v->band[b].seq = v->seq;

        do_write_hdr(v, here, prev, next, 0, 0, 0); /* increments v->seq */

        v->band[b2].type = BAND_TYPE_FRONTIER;
        v->band[b2].dirty = 1;
        v->band[b2].seq = v->seq;

        gr->frontier = b2;
        b = b2;
        here = next;
        left = v->band_size;
        //checkpoint_volume(v);
    }

    /* caller is responsible for updating write_pointer
     */
    len = min(left-2, len);
    *plen = len;
    printf("alloc: returning %d.%d %d\n", b, v->band[b].write_pointer, len);
    return here;
}

#warning FIXME: fix to work better with writev / aio

/* actually perform a write, wrapped with DATA records. 'prio' is used
 * to ensure that writes for forced cleaning can grab the last free
 * band.
 *
 * TODO - have alloc_extent return a length, and iterate. That way we
 * don't wast space at the end of a band.
 */
static void do_write(struct volume *v, int group, lba_t lba,
                     const void *buf, int sectors, int prio)
{
    int alloced = 0;
    while (sectors > 0) {
        pba_t pba = alloc_extent(v, group, sectors+2, prio, &alloced);
        int _sectors = alloced-2;
        struct map_record map = {.lba = lba, .pba = pba_add(pba,1),
                                 .len = _sectors};

        /* map entries haven't been logged yet, so location=PBA_NULL
         */
        update_range(v, PBA_NULL, lba, _sectors, pba_add(pba, 1), v->seq);

        do_write_hdr(v,
                     pba,                      /* location */
                     pba_add(pba, -1),         /* prev */
                     pba_add(pba, _sectors+1), /* next */
                     pba.band,
                     0, 0);                    /* no map entry */
        smr_write(v->disk, pba.band, pba.offset+1, buf, _sectors);
        do_write_hdr(v,
                     pba_add(pba, 1+_sectors),    /* location */
                     pba,                         /* prev */
                     pba_add(pba, _sectors+2),    /* next */
                     pba.band,
                     &map, 1);                    /* one map entry */

        v->band[pba.band].write_pointer += alloced;
        v->band[pba.band].dirty = 1;
        v->band[pba.band].seq = v->seq;

        sectors -= _sectors;
        lba += _sectors;
        buf += _sectors * SECTOR_SIZE;
    }
}

void host_write(struct volume *v, lba_t lba, const void *buf, int bytes)
{
    assert(bytes % SECTOR_SIZE == 0);
    int sectors = bytes / SECTOR_SIZE;
    assert(lba + sectors <= v->n_groups * v->group_span);

    /* internal writes can't span a group boundary
     */
    while (sectors > 0) {
        int group = lba / v->group_span;
        int _sectors = min(sectors, (group+1) * v->group_span - lba);
        do_write(v, group, lba, buf, _sectors, PRIO_NORM);
        lba += _sectors;
        sectors -= _sectors;
        buf += (_sectors*SECTOR_SIZE);
    }

    if (v->seq - v->oldest_seq > 2000) /* checkpoint every 1000 writes */
        checkpoint_volume(v);
}

#warning FIXME: test TRIM support

void host_trim(struct volume *v, lba_t lba, int sectors)
{
    assert(lba + sectors <= v->n_groups * v->group_span);

    /* internal ops can't span a group boundary
     */
    while (sectors > 0) {
        pba_t null_pba = {.band = -1, .offset = -1};
        int group = lba / v->group_span;
        int _sectors = min(sectors, (group+1) * v->group_span - lba);

        /* map entries haven't been logged yet, so location=PBA_NULL
         */
        update_range(v, PBA_NULL, lba, _sectors, null_pba, v->seq++);
        lba += _sectors;
        sectors -= _sectors;
    }
}

/*----------- Read logic --------------*/

void host_read(struct volume *v, lba_t lba, void *buf, int bytes)
{
    assert(bytes % SECTOR_SIZE == 0);
    int sectors = bytes / SECTOR_SIZE;
    assert(lba + sectors <= v->n_groups * v->group_span);

    for (sectors = bytes / SECTOR_SIZE; sectors > 0; ) {
        struct entry *e = stl_map_lba_geq(v->map, lba);
        if (e == NULL || e->lba >= lba + sectors) {
            memset(buf, 17, bytes);
            return;
        }
        int offset = max(lba - e->lba, -sectors), len = -offset;
        if (offset < 0) {
            memset(buf, 0, len*SECTOR_SIZE);
            sectors -= len;
            lba += len;
            buf += len * SECTOR_SIZE;
            offset = 0;
        }
        len = min(sectors, e->len - offset);
        if (len > 0)
            smr_read(v->disk, e->pba.band, e->pba.offset+offset, buf, len);
        sectors -= len;
        lba += len;
    }
}

/*----------- Map checkpointing --------------*/

/* note - a useful invariant is that accesses to map bands *never* use
 * the write pointers.
 */

 void write_meta(struct volume *v, int type, int n_records, pba_t next)
{
    assert(v->band[v->map_band].write_pointer == smr_write_pointer(v->disk, v->map_band));
    struct header *h = v->buf;
    memset(v->buf, 0, SECTOR_SIZE);
    pba_t location = mkpba(v->map_band, v->band[v->map_band].write_pointer++);
    if (pba_eq(next, PBA_NEXT))
        next = mkpba(v->map_band, v->band[v->map_band].write_pointer);
    *h = (struct header){.magic = STL_MAGIC, .seq = v->seq++,
                         .type = type, .local_records = 0,
                         .records = n_records, .prev = v->map_prev,
                         .next = next, .base = v->base};
    // location.offset += 1;
    smr_write(v->disk, location.band, location.offset, h, 1);
    v->map_prev = location;
}

const int map_per_sector = SECTOR_SIZE / sizeof(struct map_record);
const int band_per_sector = SECTOR_SIZE / sizeof(struct band_record);

#warning FIXME: allow sector-at-a-time processing
#warning FIXME: see checkpoint size calculations - notes 5/9/15
/* write map updates and then band updates into map band.
 */
void checkpoint_volume(struct volume *v)
{
    /* calculate previous entry
     */
    pba_t next;

    /* make sure there's enough room in the current map band. Note that we 
     * don't bother to checkpoint the write pointers for map bands
     */
    int i = v->map_band;
    int next_band = (i >= v->map_size) ? 1 : i+1;
    int needed = 2 + 20 + 2 + v->n_bands/band_per_sector + 1;
    if (v->band[i].write_pointer + needed >= v->band_size) {
        write_meta(v, RECORD_NULL,
                   0,                        /* n_records */
                   mkpba(next_band, 0));       /* next */
        v->map_band = next_band;
        v->band[i].write_pointer = 0;
    }

    int min_seq = v->seq;
    
    /* checkpoint map entries. always checkpoint the dirty ones.
     */
    int n_records, n_map = stl_map_count(v->map);
    pba_t location = mkpba(v->map_band, v->band[i].write_pointer);
    uint32_t cutoff = v->oldest_seq;

    /* also roll map updates forward. Assuming each sequence
     * number causes one map update, if we have N map entries and the
     * oldest map entry is 2N sequence numbers ago, it's too old. Roll
     * forward in units of about 10 sectors for efficiency.
     * 'cutoff' is the oldest sequence number that won't get rolled
     * forward. 
     */
    if (v->seq - v->oldest_seq > 2*n_map) 
        cutoff = v->oldest_seq + 10 * map_per_sector;

    /* write out all the band records that are dirty or older than the
     * cutoff.
     */
    struct band_record *bands = valloc(10 * SECTOR_SIZE);
    struct band *b = v->band;
    memset(bands, 0, 10*SECTOR_SIZE);

    for (i = 1+v->map_size, n_records = 0; i < v->n_bands; i++)
        if (b[i].dirty || b[i].seq < cutoff) {
            bands[n_records++] = (struct band_record)
                {.band = i, .type = b[i].type, .write_pointer = b[i].write_pointer};
            b[i].dirty = 0;
            b[i].seq = v->seq;
        }
        else if (b[i].seq < min_seq)
            min_seq = b[i].seq;

    assert(n_records < 10 * band_per_sector);
    if (n_records > 0) {
        int n_sectors = (n_records + band_per_sector - 1) / band_per_sector;
        next = mkpba(v->map_band, v->band[v->map_band].write_pointer+1+n_sectors);

        write_meta(v, RECORD_BAND, n_records, next);
        smr_write(v->disk, v->map_band, v->band[v->map_band].write_pointer, bands, n_sectors);
        v->band[v->map_band].write_pointer += n_sectors;
        write_meta(v, RECORD_BAND, 0 /*n_records*/, PBA_NEXT);
    }
    free(bands);

    /* gather the oldest entries into a checkpoint, and keep track
     * of the next-oldest entry
     */
    struct map_record *map = valloc(20 * SECTOR_SIZE);
    memset(map, 0, 20*SECTOR_SIZE);

    struct entry *e, *min_e = NULL;
    n_records = 0;
    e = stl_map_lba_iterate(v->map, NULL);
    while (e != NULL) {
        struct entry *tmp = stl_map_lba_iterate(v->map, e);
        if (e->seq < cutoff || e->dirty) {
            map[n_records++] = (struct map_record)
                {.lba = e->lba, .pba = e->pba, .len = e->len};
            if (pba_eq(e->pba, PBA_INVALID)) /* TRIM gets logged once */
                stl_map_remove(v->map, e);   /* and then removed */
            else {
                e->seq = v->seq;
                assert(location.band != 0);
                e->location = location;
                e->dirty = 0;
            }
        } else if (e->seq < min_seq) {
            min_seq = e->seq;
            min_e = e;
        }
        e = tmp;
    }

    /* next-oldest becomes the new base. (unless there are older
     * bands, in which case we don't update the base)
     */
#warning FIXME: what does 'older bands' mean?
    assert(n_records < 20 * map_per_sector);
    if (min_e != NULL) {
        v->oldest_seq = min_seq;
        v->base = min_e->location;
    }

    /* don't write anything if nothing changed.
     */
    if (n_records > 0) {
        printf("checkpoint at %d\n", v->seq);
        for (i = 0; i < n_records; i++)
            printf(" %d,+%d -> %d.%d\n", (int)map[i].lba, map[i].len,
                   map[i].pba.band, map[i].pba.offset);
        int n_sectors = (n_records + map_per_sector - 1) / map_per_sector;
        next =  mkpba(v->map_band, v->band[v->map_band].write_pointer+n_sectors+1);

        write_meta(v, RECORD_MAP, n_records, next);
        smr_write(v->disk, v->map_band, v->band[v->map_band].write_pointer, map, n_sectors);
        v->band[v->map_band].write_pointer += n_sectors;
        write_meta(v, RECORD_MAP, 0 /* n_records */, PBA_NEXT);
    }
    free(map);
}

/*------------ the rest --------------*/


