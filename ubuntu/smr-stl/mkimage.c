#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>

#include "stl2.h"
/*
band.sectors nn
map.bands nn
group.bands nn
group.span nn
group.num nn
create

band (n) nn (ptr) nn (type) nn
bands (n) nn (count) nn (ptr) nn (type) nn
map lba=... pba=x.y len=nn
dump map/bands x.y <- write records

header x.y seq=n type=n records=n prev=x.y next=x.y base=x.y
*/

int band_size;
int map_size;
int group_size;
int group_span;
int n_groups;
int64_t n_bands;

char _bands[4096*16];
struct band_record *bands = (void*)_bands, *band_base =(void*) _bands;

char _map[4096*16];
struct map_record *maps = (void*)_map, *map_base = (void*)_map;

int fd;

void parse_int(int argc, char **argv, void *ctx)
{
    int *ptr = ctx;
    *ptr = atoi(argv[1]);
}

void open_image(int argc, char **argv, void *ctx)
{
    int i, do_create = (int)ctx;
    char *file = argv[1];
    n_bands = group_size * n_groups + map_size + 1;
    
    if (do_create) {
        if ((fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0777)) < 0)
            perror("open failed"), exit(1);
        void *buf = calloc(4096, 1);
        printf("creating disk %d bands %d sectors\n",
               (int)n_bands, (int)(n_bands*band_size));
        for (i = 0; i < n_bands*band_size; i++)
            write(fd, buf, 4096);
    }
    else {
        if ((fd = open(file, O_WRONLY)) < 0)
            perror("open failed"), exit(1);
        off_t size = lseek(fd, 0, SEEK_END);
        if (size != 4096 * n_bands * band_size)
            printf("bad size\n"), exit(1);
    }
}

/* band (n) nn (ptr) nn (type) nn
 * bands (n) nn (count) nn (ptr) nn (type) nn
 */

void band_record(int argc, char **argv, void *ctx)
{
    int i, count = 1, band = atoi(argv[1]), ptr, type;
    if (!strcmp(argv[0], "bands")) {
        count = atoi(argv[2]);
        argv++;
    }
    ptr = atoi(argv[2]);
    type = atoi(argv[3]);

    for (i = 0; i < count; i++) 
        *bands++ = (struct band_record){.band = band+i,
                                        .pointer = ptr, .type = type};
}

struct pba atopba(const char *str)
{
    struct pba val;
    sscanf(str, "%d.%d", &val.band, &val.offset);
    return val;
}

/* map lba pba len
 */
void map_record(int argc, char **argv, void *ctx)
{
    int64_t lba = atoll(argv[1]);
    struct pba pba = atopba(argv[2]);
    int len = atoi(argv[3]);
    *maps++ = (struct map_record){.lba=lba, .pba=pba, .len=len};
}

int64_t pba2long(struct pba pba)
{
    return (int64_t)pba.band * band_size + pba.offset;
}

/* dump map x.y
 */
void dump_map(int argc, char **argv, void *ctx)
{
    int n_records = maps - map_base;
    int n_bytes = (char*)maps - (char*)map_base;
    int n_sectors = (n_bytes + 4095) / 4096;
    struct pba pba = atopba(argv[2]);
    printf("writing %d map records (%d sectors) at %d.%d\n",
           n_records, n_sectors, pba.band, pba.offset);
    
    lseek(fd, pba2long(pba) * SECTOR_SIZE, SEEK_SET);
    write(fd, map_base, n_sectors*4096);
    memset(map_base, 0, n_bytes);
    maps = map_base;
}

void dump_records(int argc, char **argv, void *ctx)
{
    if (!strcmp(argv[1], "map")) {
        dump_map(argc, argv, ctx);
        return;
    }
    int n_records = bands - band_base;
    int n_bytes = (char*)bands - (char*)band_base;
    int n_sectors = (n_bytes + 4095) / 4096;
    struct pba pba = atopba(argv[2]);
    printf("writing %d band records (%d sectors) at %d.%d\n",
           n_records, n_sectors, pba.band, pba.offset);
    
    lseek(fd, pba2long(pba) * SECTOR_SIZE, SEEK_SET);
    write(fd, band_base, n_sectors*4096);
    memset(band_base, 0, n_bytes);
    bands = band_base;
}

/* header x.y seq=n type=n records=n prev=x.y next=x.y base=x.y
 */
void write_header(int argc, char **argv, void *ctx)
{
    struct pba here = atopba(argv[1]), prev = atopba(argv[5]),
        next = atopba(argv[6]), base = atopba(argv[7]);
    int seq = atoi(argv[2]), type = atoi(argv[3]), n_records = atoi(argv[4]);

    struct header h =
        (struct header){.magic = STL_MAGIC, .seq = seq, .type = type,
                        .local_records = 0, .records = n_records,
                        .next = next, .prev = prev, .base = base};
    char buf[4096];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, &h, sizeof(h));

    off_t n = lseek(fd, pba2long(here) * SECTOR_SIZE, SEEK_SET);

    printf("writing header %d at %d.%d (%lld)\n", seq, here.band,
           here.offset, n);
    write(fd, buf, sizeof(buf));
}

/* like write_header, but dumps any map records into h->data
 *  mapheader x.y seq type prev.x next.y base.z
 */
void write_mapheader(int argc, char **argv, void *ctx)
{
    struct pba here = atopba(argv[1]), prev = atopba(argv[4]),
        next = atopba(argv[5]), base = atopba(argv[6]);
    int seq = atoi(argv[2]), type = atoi(argv[3]);
    int n_records = maps - map_base, bytes = (char*)maps - (char*)map_base;

    struct header h =
        (struct header){.magic = STL_MAGIC, .seq = seq, .type = type,
                        .local_records = n_records, .records = 0,
                        .next = next, .prev = prev, .base = base};
    char buf[4096];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, &h, sizeof(h));
    memcpy(&buf[sizeof(h)], map_base, bytes);
    maps = map_base;
    off_t n = lseek(fd, pba2long(here) * SECTOR_SIZE, SEEK_SET);

    printf("writing header %d with %d local records at %d.%d (%lld)\n",
           seq, n_records, here.band, here.offset, n);
    write(fd, buf, sizeof(buf));
}

void write_zeros(int argc, char **argv, void *ctx)
{
    struct pba here = atopba(argv[1]);
    int i, count = 1;
    char buf[4096];
    memset(buf, 0, sizeof(buf));
    off_t n = lseek(fd, pba2long(here) * SECTOR_SIZE, SEEK_SET);

    if (argc > 2)
        count = atoi(argv[2]);
    printf("zeroing %d.%d (%lld) %d sectors\n", here.band, here.offset,
           n, count);
    for (i = 0; i < count; i++)
        write(fd, buf, sizeof(buf));
}

void write_superblock(int argc, char **argv, void *ctx)
{
    struct superblock sb = {
        .magic = STL_MAGIC, .disk_size = n_bands * band_size,
        .n_bands = n_bands, .band_size = band_size, .group_size = group_size,
        .group_span = group_span, .n_groups = n_groups, .map_size = map_size};
    char buf[4096];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, &sb, sizeof(sb));

    lseek(fd, 0, SEEK_SET);
    write(fd, buf, SECTOR_SIZE);
}

void quit(int argc, char **argv, void *ctx)
{
    exit(0);
}

struct {
    char *cmd;
    void (*fn)(int argc, char **argv, void *ctx);
    void *ctx;
} cmdtable[] = {
    {.cmd = "band.sectors", .fn=parse_int, .ctx=&band_size},
    {.cmd = "map.bands", .fn=parse_int, .ctx=&map_size},
    {.cmd = "group.bands", .fn=parse_int, .ctx=&group_size},
    {.cmd = "group.span", .fn=parse_int, .ctx=&group_span},
    {.cmd = "group.num", .fn=parse_int, .ctx=&n_groups},
    {.cmd = "open", .fn=open_image, .ctx=0},
    {.cmd = "create", .fn=open_image, .ctx=(void*)1},
    {.cmd = "band", .fn=band_record},
    {.cmd = "bands", .fn=band_record},
    {.cmd = "map", .fn=map_record},
    {.cmd = "dump", .fn=dump_records},
    {.cmd = "superblock", .fn=write_superblock},
    {.cmd = "mapheader", .fn=write_mapheader},
    {.cmd = "header", .fn=write_header},
    {.cmd = "quit", .fn=quit},
    {.cmd = "zero", .fn=write_zeros}};


/* usage: mkimage <cmdfile>
 */
int main(int n, char **args)
{
    char line[80], *ptr;
    char **ap, *argv[10];
    FILE *fp = fopen(args[1], "r");
    
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (line[0] == '#' || line[0] == '\n')
            continue;
        printf("line %s", line);
        for (ap = argv, ptr = line; (*ap = strsep(&ptr, " \t\n")) != NULL;)
            if (**ap != '\0')
                if (++ap >= &argv[10])
                    break;
        int i, argc = ap-&argv[0],
            n_cmds = sizeof(cmdtable)/sizeof(cmdtable[0]);
        if (argc == 0)
            continue;
        for (i = 0; i < n_cmds; i++) 
            if (!strcmp(argv[0], cmdtable[i].cmd)) {
                cmdtable[i].fn(argc, argv, cmdtable[i].ctx);
                break;
            }
        if (i == n_cmds)
            printf("invalid command: %s\n", argv[0]);
    }
    close(fd);
}


