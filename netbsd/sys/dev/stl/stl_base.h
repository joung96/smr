/*
 * file:        stl_base.h
 * description: data structures for STL
 */

#ifndef __STL_BASE_H__
#define __STL_BASE_H__

#include <sys/mutex.h>
#include <sys/condvar.h>

/*----------- Data Structures ------------*/

/* a band group is a self-sufficient STL with an LBA span and a set of
 * bands.
 */
struct group {
    int count[BAND_TYPE_MAX];
    int frontier;
    int frontier_offset;
    pba_t prev;                 /* previous header */
    int cleaning;
};

/* track write pointer and band type per band.
 */
struct band {
    uint16_t type;              /* BAND_TYPE_FREE, etc. */
    uint16_t dirty;
    uint32_t write_pointer;     /* next free sector */
};

/* the primary data structure. Forward and reverse maps, geometry,
 * band info, group into, etc.
 */
struct volume {
    void *map;
    struct smr *disk;
    int   band_size;
    int   n_bands;
    struct band *band;
    int   map_size;             /* number of rotating map bands */
    int   map_band;             /* current persistent map band */
    int   group_size;
    int   group_span;
    int   n_groups;
    struct group *groups;
    int   seq;
    pba_t base;
    int   count;
    pba_t map_prev;
    kmutex_t m;
    kcondvar_t  c;
    lba_t defrag_begin, defrag_end;
    struct timeval last_op;
    int exit_threads;
};

/* mapping entry. note that 'lba' and 'pba' are duplicates of the
 * fields stored by the code in stl_map.c
 * TODO - fix this.
 */
struct entry {
    lba_t    lba;
    pba_t    pba;
    int32_t  len;
    uint8_t  dirty;
};

/* prototypes */
void clean_all(struct volume *v);
enum ckpt {CKPT_FULL = 1,
	   CKPT_INCR = 0};
void checkpoint_volume(struct volume *v, enum ckpt type);
void defrag_group(struct volume *v, int g, int idle);

#endif
