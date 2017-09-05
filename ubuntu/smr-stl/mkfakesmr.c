/*
 * file:        mkfamesmr.c
 * description: initialize fake SMR drive or image
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <getopt.h>
#include "stl_fakesmr.h"

#define SECTOR_SIZE 4096
#define BANDS_PER_SECTOR (SECTOR_SIZE/sizeof(int))

int band_size = 128 * 1024 * 1024; /* 128MB */

struct option opts[] = {
    {.name = "bandsize", required_argument, 0, 'b'},
    {.name = "read",     no_argument,       0, 'r'},
    {0,                  0,                 0, 0}
};

off_t parseint(char *s)
{
    off_t val = strtol(s, &s, 0);
    if (toupper(*s) == 'G')
        val *= (1024*1024*1024);
    if (toupper(*s) == 'M')
        val *= (1024*1024);
    if (toupper(*s) == 'K')
        val *= 1024;
    return val;
}

int main(int argc, char **argv)
{
    int c, opt_index, do_read = 0;

    while ((c = getopt_long_only(argc, argv, "", opts, &opt_index)) != -1) {
        switch (opts[opt_index].val) {
        case 'b':
            band_size = parseint(optarg);
            break;
        case 'r':
            do_read = 1;
            break;
        }
    }

    if (optind != argc-1) {
        fprintf(stderr, "usage: %s <device>\n", argv[0]);
        exit(1);
    }
    char *name = argv[optind];

    if (do_read) {
        struct smr *dev;
        if ((dev = smr_open(name)) == NULL) 
            fprintf(stderr, "can't open %s\n", name);
        else {
            int i, n = smr_n_bands(dev);
            int s = smr_band_size(dev);
            printf("%d bands of %.1f MB\n", n, s / 256.0);
            for (i = 0; i < n; i++) {
                int m = smr_write_pointer(dev, i);
                if (m != 0)
                    printf("%d: %d\n", i, m);
            }
        }
    }
    else {
        int n = smr_init(name, band_size / 4096);
        printf("successfully initialized with %d bands\n", n);
    }
    return 0;
}
