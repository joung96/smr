/*
 * file:        stl.h
 * description: definitions for shingle translation layer
 */
#ifndef __STL_H__
#define __STL_H__

#define STL_MAGIC 0x4c54537f
#define SECTOR_SIZE 4096ULL

/* address types for logical and physical addresses.
 */
typedef int64_t lba_t;

#define LBA_INVALID (lba_t)-1

typedef struct pba {
    int32_t band;
    int32_t offset;
} pba_t;

static inline pba_t pba_add(pba_t pba, int delta) {
    return (struct pba){.band=pba.band, .offset=pba.offset+delta};
}

static inline int pba_eq(pba_t p1, pba_t p2) {
    return p1.band == p2.band && p1.offset == p2.offset;
}

static inline pba_t mkpba(int band, int offset) {
    return (struct pba){.band = band, .offset = offset};
}

#define PBA_INVALID mkpba(-1,-1)
#define PBA_NULL mkpba(0,0)

/* the superblock goes at the beginning of band 0
 * map info is in bands 1..map_size
 * group 0 starts at band map_size+1 
 */
struct superblock {
    uint32_t magic;
    uint32_t n_bands;
    uint64_t disk_size;         /* 4K sectors */
    uint32_t band_size;         /* 4K sectors */
    uint32_t group_size;        /* in bands */
    uint32_t group_span;        /* in 4K sectors */
    uint32_t n_groups;
    uint32_t map_size;          /* in bands */
};

/* any data or metadata written is wrapped by sectors in this format.
 */
struct header {
    uint32_t magic;
    uint32_t seq;
    uint16_t type;
    uint16_t local_records;     /* in data[] */
    uint32_t records;           /* in following sectors */
    pba_t    next;
    pba_t    prev;
    pba_t    base;              /* only for MAP / BAND? */
                                /* following: MAP,DATA: map_record */
                                /* BAND: band_record */
};

/* header->type determines how to interpret local or external records
 */
enum {
    RECORD_BAND=1,
    RECORD_MAP=2,
    RECORD_DATA=3,
    RECORD_NULL=4
};

/* type=RECORD_BAND. Just indicates the band type.
 */
struct band_record {
    uint32_t band;
    uint32_t type;
    int write_pointer;
} __attribute__((packed));

/* type==RECORD_MAP, type=RECORD_DATA (only in local_records)
 */
struct map_record {
    int64_t  lba;
    pba_t    pba;
    int32_t  len;
};

/* not sure if band type goes here - it's more specific to the STL.
 */
//#define BAND_CURRENT 0x80
enum band_type {
    BAND_TYPE_FREE,
    BAND_TYPE_FULL,
    BAND_TYPE_FRONTIER,
    BAND_TYPE_MAX
};

#endif
