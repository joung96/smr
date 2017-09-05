/*
 * file:        stl_base.h
 * description: data structures for STL
 */

#ifndef __STL_BASE_H__
#define __STL_BASE_H__

/*----------- Data Structures ------------*/

/* a band group is a self-sufficient STL with an LBA span and a set of
 * bands.
 */
struct group {
    int count[BAND_TYPE_MAX];
    int frontier;
    int frontier_offset;
};

/* track write pointer and band type per band. 
 */
struct band {
    uint16_t type;              /* BAND_TYPE_FREE, etc. */
    uint16_t dirty;
    uint32_t write_pointer;     /* next free sector */
    uint32_t seq;               /* of last write it's persisted in */
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
    // int   map_offset;           /* and offset */
    int   group_size;
    int   group_span;
    int   n_groups;
    struct group *groups;
    void *buf;                  /* temporary buffer */
    int   seq;
    pba_t base;                 
    int   oldest_seq;
    pba_t map_prev;
};

/* mapping entry. note that 'lba' and 'pba' are duplicates of the
 * fields stored by the code in stl_map.c
 * TODO - fix this.
 */
struct entry {
    lba_t    lba;
    pba_t    pba;
    pba_t    location;
    int32_t  len;
    uint32_t seq;
    uint8_t  dirty;
};

/* prototypes */
void clean_all(struct volume *v);
void checkpoint_volume(struct volume *v);

#endif
