/*
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <getopt.h>
#include <string.h>

#include "stl.h"
#include "stl_fakesmr.h"

struct option opts[] = {
    {.name = "group-bands",       required_argument, 0, 'g'},
    {.name = "over-provisioning", required_argument, 0, 'o'},
    {.name = "map-bands",         required_argument, 0, 'm'},
    {0,                           0,                 0,  0}
};

int group_bands = 0;
double over_provisioning = 0.0;
int map_bands = 0;

void usage(char *cmd)
{
    int i;
    fprintf(stderr, "usage: %s [options] <device>\n options = ", cmd);
    char *s = "";
    for (i = 0; opts[i].name; i++) {
        printf("%s-%s\n", s, opts[i].name);
        s = "           ";
    }
    exit(1);
}

int main(int argc, char **argv)
{
    int c, opt_index;

    while ((c = getopt_long_only(argc, argv, "", opts, &opt_index)) != -1) {
        switch (opts[opt_index].val) {
        case 'g':
            group_bands = atoi(optarg);
            break;
        case 'o':
            over_provisioning = atof(optarg);
            break;
        case 'm':
            map_bands = atoi(optarg);
            break;
        }
    }
    
    if (optind != argc-1 || !group_bands || !map_bands || !over_provisioning)
        usage(argv[0]);
    char *name = argv[optind];

    struct smr *dev = smr_open(name);
    if (!dev)
        exit(1);

    int n_bands = smr_n_bands(dev);
    int band_size = smr_band_size(dev);

    printf("n_bands: %d\n", n_bands);
    printf("band_size: %d (%dM)\n", band_size, band_size/256);
    
    int n_groups = (n_bands - 1 - map_bands) / group_bands;
    int group_span = (group_bands * band_size) / over_provisioning;
    n_bands = 1 + map_bands + (group_bands * n_groups);

    printf("%d groups: %d bands, %dMB each\n", n_groups, group_bands,
           band_size/256);
    printf("logical capacity: %dMB\n", n_groups * group_span / 256);
    
    smr_reset_all(dev);
    
    struct superblock sb = {
        .magic = STL_MAGIC, .disk_size = n_bands * band_size,
        .n_bands = n_bands, .band_size = band_size, .group_size = group_bands,
        .group_span = group_span, .n_groups = n_groups, .map_size = map_bands};
    void *buf = valloc(4096);
    memcpy(buf, &sb, sizeof(sb));

    smr_write(dev, 0, 0, buf, 1);

    int i, offset = 1 + map_bands;
    int sectors = (n_bands * sizeof(struct band_record) + 4095) / 4096;
    struct band_record *bands = valloc(sectors * 4096);
    memset(bands, 0, sectors*4096);

    for (i = offset; i < n_bands; i++)
        bands[i] = (struct band_record){.band = i, .type = BAND_TYPE_FREE};

    for (i = 0; i < n_groups; i++) {
        int j = i*group_bands + offset;
        bands[j] = (struct band_record){.band = j, .type = BAND_TYPE_FRONTIER};
    }

    /* write band record in band 1, offset 0, seqs 0 and 1 */
    int next_offset = 1 + sectors;
    struct header h1 = {
        .magic = STL_MAGIC, .seq = 0, .type = RECORD_BAND, .local_records = 0,
        .records = n_bands, .next = mkpba(1, next_offset), .prev = mkpba(1, 0),
        .base = mkpba(1, 0)};

    struct header h2 = {
        .magic = STL_MAGIC, .seq = 1, .type = RECORD_BAND, .local_records = 0,
        .records = 0, .next = mkpba(1, next_offset+1), .prev = mkpba(1, 0),
        .base = mkpba(1, 0)};

    memset(buf, 0, 4096);
    memcpy(buf, &h1, sizeof(h1));
    smr_write(dev, 1, 0, buf, 1);

    smr_write(dev, 1, 1, bands, sectors);

    memcpy(buf, &h2, sizeof(h2));
    smr_write(dev, 1, sectors+1, buf, 1);

    smr_close(dev);

    return 0;
}
   
