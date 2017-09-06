/*
 * file:        stl_map.h
 * description: prototypes for stl_map.c
 */
#ifndef __STL_MAP_H__
#define __STL_MAP_H__

void *stl_map_init(void);
void stl_map_destroy(void *_maps);
void *stl_map_entry(int len);
void stl_map_insert(void *_maps, void *_e, lba_t lba, pba_t pba, int len);
void stl_map_remove(void *_maps, void *_e);
void stl_map_update(void *_e, lba_t lba, pba_t pba, int len);
void *stl_map_lba_geq(void *_maps, lba_t lba);
void *stl_map_pba_geq(void *_maps, pba_t pba);
void *stl_map_lba_iterate(void *_maps, void *_e);
void *stl_map_pba_iterate(void *_maps, void *_e);
int  stl_map_count(void *_maps);

#endif
