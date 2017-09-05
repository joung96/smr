#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>

#include "stl2.h"

int main(int argc, char **argv)
{
    int do_create = 1;
    //int band_size = 4*1024;    /* 16MB */
    int band_size = 1024;       /* 4MB */
    int group_size = 128;       /* 512MB */
    //int group_span = 940000;   /* 90% */
    int group_span = 118000;   /* 90% */
    int map_size = 20;
    int map_band = 1;
    int map_offset = 0;
    int n_groups = 2;
    int n_bands = 1 + map_size + n_groups * group_size;

    if (strcmp(argv[1], "-q") == 0) {
        argv++;
        argc--;
        do_create = 0;
    }

    int i, fd = open(argv[1], O_WRONLY|O_CREAT, 0777);
    void *buf = calloc(SECTOR_SIZE, 1);
    if (fd < 0)
        perror("create"), exit(1);
    if (do_create)
        for (i = 0; i < band_size * n_bands; i++)
            write(fd, buf, SECTOR_SIZE);
    
    struct superblock sb = {
        .magic = STL_MAGIC, .disk_size = n_bands * band_size,
        .n_bands = n_bands, .band_size = band_size, .group_size = group_size,
        .group_span = group_span, .n_groups = n_groups, .map_size = map_size};
    memcpy(buf, &sb, sizeof(sb));

    lseek(fd, 0, SEEK_SET);
    write(fd, buf, SECTOR_SIZE);

    int *type = calloc(n_bands * sizeof(int), 1);
    for (i = 0; i < n_groups; i++) {
        int j = 1 + map_size + i*group_size;
        type[j++] = BAND_TYPE_1x | BAND_CURRENT;
        type[j++] = BAND_TYPE_16x | BAND_CURRENT;
        type[j++] = BAND_TYPE_128x | BAND_CURRENT;
        type[j++] = BAND_TYPE_1024x | BAND_CURRENT;
        type[j++] = BAND_TYPE_SMALL | BAND_CURRENT;
    }

    int len = n_bands * sizeof(struct band_record) + SECTOR_SIZE - 1;
    struct band_record *br = calloc(len, 1);
    for (i = 0; i < n_bands; i++) {
        br[i].band = i;
        br[i].pointer = 0;
        br[i].type = type[i];
    }

    int n_sectors = len / SECTOR_SIZE;
    br[1].pointer = n_sectors+3;

    struct header h = {
        .magic = STL_MAGIC, .seq = 0, .type = RECORD_BAND, .local_records = 0,
        .records = n_bands, .next = {.band = 1, .offset = n_sectors+1},
        .prev = {.band=1,.offset=0}, .base = {.band = 1, .offset = 0}};
    memset(buf, 0, SECTOR_SIZE);
    memcpy(buf, &h, sizeof(h));

    lseek(fd, band_size*SECTOR_SIZE, SEEK_SET);
    write(fd, buf, SECTOR_SIZE);

    write(fd, br, n_sectors * SECTOR_SIZE);

    h.seq = 1;
    h.records = 0;
    h.next.offset++;
    memcpy(buf, &h, sizeof(h));
    write(fd, buf, SECTOR_SIZE);

    close(fd);
    return 0;
}

// dd of=image.img conv=notrunc if=/dev/zero bs=4k count=8k
