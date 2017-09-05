/*
 * file:        dumpstl.c
 * description: print out contents of disk image.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>

#include "stl2.h"

char buf[10*SECTOR_SIZE];

int n_bands;
int band_size;
int group_size;
int group_span;
int n_groups;
int map_size;

int addr(int band, int offset)
{
    return (band*band_size + offset)*SECTOR_SIZE;
}

char *record_type(unsigned t)
{
    char *names[] = {"*BAD*", "BAND", "MAP", "DATA", "NULL"};
    if (t > RECORD_NULL)
        return "*BAD*";
    return names[t];
}
    
int num_sectors(int type, int count)
{
    if (type == RECORD_MAP) {
        int map_per_sector = SECTOR_SIZE / sizeof(struct map_record);
        return (count + map_per_sector - 1) / map_per_sector;
    }
    if (type == RECORD_BAND) {
        int band_per_sector = SECTOR_SIZE / sizeof(struct band_record);
        return (count + band_per_sector - 1) / band_per_sector;
    }
    printf("ERROR: external records in non-MAP non-BAND entry\n");
    return 0;
}

int main(int argc, char **argv)
{
    int fd = open(argv[1], O_RDONLY);
    read(fd, buf, sizeof(buf));
    struct superblock *sb = (void*)buf;

    if (sb->magic != STL_MAGIC)
        printf("bad magic\n"), exit(1);
    
    printf("%d sectors\n %d bands of %d sectors\nmap size %d\n",
           (int)sb->disk_size, sb->n_bands, sb->band_size, sb->map_size);
    printf("%d groups of %d bands\n", sb->n_groups, sb->group_size);
    n_bands = sb->n_bands;
    band_size = sb->band_size;
    group_size = sb->group_size;
    group_span = sb->group_span;
    n_groups = sb->n_groups;
    map_size = sb->map_size;

    char flags[n_bands];
    memset(flags, 0, sizeof(flags));
    
    int i, j, k, l;
    struct header *h = (void*)buf;
    for (i = 1; i < map_size+1; i++) {
        j = 0;
        while (1) {
            if (j == 0)
                printf("band %d:\n", i);
            lseek(fd, addr(i, j), SEEK_SET);
            read(fd, buf, sizeof(buf));
            if (h->magic != STL_MAGIC) {
                break;
            }
            printf(" %d: %s seq %d local %d extern %d prev %d.%d next %d.%d"
                   " base %d.%d\n", j, record_type(h->type), h->seq,
                   h->local_records, h->records, h->prev.band, h->prev.offset,
                   h->next.band, h->next.offset, h->base.band, h->base.offset);
            struct map_record *lm = (void*)&h->data;
            for (k = 0; k < h->local_records; k++) 
                printf("   %d,+%d -> %d.%d\n", (int)lm[k].lba, lm[k].len,
                       lm[k].pba.band, lm[k].pba.offset);

            if (h->next.band != i) {
                if (h->records > 0)
                    printf("ERROR: external records at end of band\n");
                break;
            }

            int nrecords = h->records, offset = h->next.offset;
            int type = h->type;

            if (nrecords > 0) {
                int len = SECTOR_SIZE * (offset - (j+1));
                lseek(fd, addr(i, j+1), SEEK_SET);
                read(fd, buf, len);
            }

            struct map_record *m = (void*)buf;
            struct band_record *b = (void*)buf;
            
            if (type == RECORD_MAP) {
                for (l = 0; l < nrecords; l++)
                    printf("   %d,+%d -> %d.%d\n", (int)m[l].lba,
                           m[l].len, m[l].pba.band, m[l].pba.offset);
            }
            else if (type == RECORD_BAND) {
                for (k = 0; k < nrecords; k++) {
                    if (b[k].type != BAND_TYPE_FREE)
                        printf("%d: %d %d\n", b[k].band, b[k].type, b[k].pointer);
                }
            }
            else if (type == RECORD_DATA) {
            }

            j = offset;
        }
    }

    /* now read the data bands */
    for (i = 1+map_size; i < n_bands; i++) {
        j = 0;
        while (1) {
            lseek(fd, addr(i, j), SEEK_SET);
            read(fd, buf, sizeof(buf));
            if (h->magic != STL_MAGIC) {
                break;
            }
            if (j == 0)
                printf("band %d:\n", i);
            printf(" %d: %s seq %d local %d prev %d.%d next %d.%d\n",
                   j, record_type(h->type), h->seq, h->local_records, 
                   h->prev.band, h->prev.offset, h->next.band, h->next.offset);
            struct map_record *lm = (void*)&h->data;
            for (k = 0; k < h->local_records; k++) 
                printf("   %d,+%d -> %d.%d\n", (int)lm[k].lba, lm[k].len,
                       lm[k].pba.band, lm[k].pba.offset);
            j = h->next.offset;
            if (h->next.band != i)
                break;
        }
    }
    
    return 0;
}
