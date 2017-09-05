/*
 * file:        stl_map.h
 * description: LBA->PBA and PBA->LBA map implementation
 */

#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include "rbtree.h"
#include "stl.h"
#include "stl_map.h"

/* 'location' is the header of the checkpoint packet that this entry
 * was last written in. Not valid if the entry is dirty.
 */

struct entry {
    rb_node_t  rb_fwd;          /* LBA -> PBA map */
    rb_node_t  rb_rev;          /* PBA -> LBA map */
    int64_t    lba;
    struct pba pba;
    int        len;
};

struct map_pair {
    rb_tree_t fmap;
    rb_tree_t rmap;
};

/* returns equal if the LBA intervals overlap
 */
static signed int compare_nodes_fwd(void *ctx, const void *n1, const void *n2)
{
    const struct entry *e1 = n1, *e2 = n2;
    if (e1->lba + e1->len <= e2->lba)
        return -1;
    if (e1->lba >= e2->lba + e2->len)
        return 1;
    return 0;
}

/* returns equal if the key (lba) is contained in the interval
 */
static signed int compare_key_fwd(void *ctx, const void *n1, const void *key)
{
    const struct entry *e = n1;
    const int32_t *p = key;

    if (e->lba + e->len <= *p)
        return -1;
    if (e->lba > *p)
        return 1;
    return 0;
}

static rb_tree_ops_t fwd_ops = {
    .rbto_compare_nodes = compare_nodes_fwd,
    .rbto_compare_key = compare_key_fwd,
    .rbto_node_offset = 0,
    .rbto_context = 0
};

/* returns equal if the PBA intervals start at the same address.
 */
static signed int compare_nodes_rev(void *ctx, const void *n1, const void *n2)
{
    const struct entry *e1 = n1, *e2 = n2;

    if (e1->pba.band != e2->pba.band)
        return e1->pba.band - e2->pba.band;
    return e1->pba.offset - e2->pba.offset;
    
#if 0
    if (e1->pba.offset + e1->len <= e2->pba.offset)
        return -1;
    if (e1->pba.offset >= e2->pba.offset + e2->len)
        return 1;

//    assert(0);                  /* should never happen */
    return 0;        /* race - should only happen when reading maps */
#endif
}

/* returns equal if the key (PBA) is contained in the interval.
 * note - this function is only used when called by stl_map_pba_geq.
 * the compare_nodes function is used for insertions, which does a
 * point instead of range comparison 
 */
static signed int compare_key_rev(void *ctx, const void *n1, const void *key)
{
    const struct entry *e = n1;
    const struct pba *p = key;

    if (e->pba.band != p->band)
        return e->pba.band - p->band;
    if (e->pba.offset + e->len <= p->offset)
        return -1;
    if (e->pba.offset > p->offset)
        return 1;
    return 0;
}

static rb_tree_ops_t rev_ops = {
    .rbto_compare_nodes = compare_nodes_rev,
    .rbto_compare_key = compare_key_rev,
    .rbto_node_offset = sizeof(rb_node_t),
    .rbto_context = 0
};

/* ---------- PUBLIC FUNCTIONS ----------- */

/* initialize a forward and reverse map pair.
 */
void *stl_map_init(void)
{
    struct map_pair *maps = malloc(sizeof(*maps));
    rb_tree_init(&maps->fmap, &fwd_ops);
    rb_tree_init(&maps->rmap, &rev_ops);
    return maps;
}

/* free a map pair and all the entries in it. note that entries are in
 * both, so we just need to iterate through one.
 */
void stl_map_destroy(void *_maps)
{
    struct map_pair *maps = _maps;
    struct entry *e = rb_tree_iterate(&maps->fmap, NULL, RB_DIR_RIGHT);
    while (e != NULL) {
        struct entry *tmp = rb_tree_iterate(&maps->fmap, e, RB_DIR_RIGHT);
        rb_tree_remove_node(&maps->fmap, e);
        rb_tree_remove_node(&maps->rmap, e);
        free(e);
        e = tmp;
    }
    free(maps);
}

/* create a map entry with 'len' extra bytes for user data
 */    
void *stl_map_entry(int len)
{
    struct entry *e = malloc(sizeof(*e) + len);
    return e+1;
}

/* insert map entry at lba..+len and pba..+len in the two maps
 */
void stl_map_insert(void *_maps, void *_e, lba_t lba, pba_t pba, int len)
{
    struct map_pair *maps = _maps;
    struct entry *e = _e;
    if (e != NULL) e--;
    e->lba = lba;
    e->pba = pba;
    e->len = len;
    rb_tree_insert_node(&maps->fmap, e);
    struct entry *tmp = rb_tree_insert_node(&maps->rmap, e);
    assert(tmp == e);
}

/* remove and free a map entry.
 */
void stl_map_remove(void *_maps, void *_e)
{
    struct map_pair *maps = _maps;
    struct entry *e = _e;
    if (e != NULL) e--;
    rb_tree_remove_node(&maps->fmap, e);
    rb_tree_remove_node(&maps->rmap, e);
    free(e);
}

/* update a map entry. Don't change the order of entries or things
 * will break.
 */
void stl_map_update(void *_e, lba_t lba, pba_t pba, int len)
{
    struct entry *e = _e;
    if (e != NULL) e--;
    e->lba = lba;
    e->pba = pba;
    e->len = len;
}

/* find a map entry containing 'lba' or the next higher entry.
 */
void *stl_map_lba_geq(void *_maps, lba_t lba)
{
    struct map_pair *maps = _maps;
    struct entry *e = rb_tree_find_node_geq(&maps->fmap, &lba);
    return (e == NULL) ? NULL : e+1;
}

/* ditto for PBA
 */
void *stl_map_pba_geq(void *_maps, pba_t pba)
{
    struct map_pair *maps = _maps;
    struct entry *e = rb_tree_find_node_geq(&maps->rmap, &pba);
    return (e == NULL) ? NULL : e+1;
}

/* right-hand iterator - LBA mapping
 */
void *stl_map_lba_iterate(void *_maps, void *_e)
{
    struct map_pair *maps = _maps;
    struct entry *e = _e;
    if (e != NULL) e--;
    e = rb_tree_iterate(&maps->fmap, e, RB_DIR_RIGHT);
    return (e == NULL) ? NULL : e+1;
}

/* right-hand iterator - PBA mapping
 */
void *stl_map_pba_iterate(void *_maps, void *_e)
{
    struct map_pair *maps = _maps;
    struct entry *e = _e;
    if (e != NULL) e--;
    e = rb_tree_iterate(&maps->rmap, e, RB_DIR_RIGHT);
    return (e == NULL) ? NULL : e+1;
}

int  stl_map_count(void *_maps)
{
    struct map_pair *maps = _maps;
    return rb_tree_count(&maps->fmap);
}

