/*
 * file:        stl_test.c
 * description: test interface for STL
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
 
#include "stl.h"
#include "stl_map.h"
#include "stl_base.h"
#include "stl_public.h"

void print_metadata(struct volume *v)
{
    int i, j;

    printf("bands:\n");
    for (i = 0; i < v->n_bands; ) {
        int type = v->band[i].type, ptr = v->band[i].write_pointer;
        for (j = i; (j+1 < v->n_bands && v->band[j+1].type == type
                     && v->band[j+1].write_pointer == ptr); j++);
        printf("%d-%d %x %s %d %d\n", i, j, v->band[i].type,
               v->band[i].dirty ? "DIRTY" : "",
               v->band[i].write_pointer, (int)v->band[i].seq);
        i = j+1;
    }

    struct entry *e = stl_map_lba_iterate(v->map, NULL);
    while (e != NULL) {
        printf("%d +%d -> %d.%d at %d.%d (%d%s)\n", (int)e->lba, e->len,
               e->pba.band, e->pba.offset, e->location.band, e->location.offset,
               e->seq, e->dirty ? " DIRTY" : "");
        e = stl_map_lba_iterate(v->map, e);
    }
}

void *cmdline_buf;

void cmd_clean(struct volume *v, int argc, char **argv)
{
    clean_all(v);
}

void cmd_checkpoint(struct volume *v, int argc, char **argv)
{
    checkpoint_volume(v);
}

void cmd_print(struct volume *v, int argc, char **argv)
{
    print_metadata(v);
}

void cmd_rprint(struct volume *v, int argc, char **argv)
{
    struct entry *e = stl_map_pba_iterate(v->map, NULL);
    while (e != NULL) {
        printf("%d.%d <- %d +%d (%d%s)\n", e->pba.band, e->pba.offset,
               (int)e->lba, e->len, e->seq, e->dirty ? " DIRTY" : "");
        e = stl_map_pba_iterate(v->map, e);
    }
}

void cmd_fprint(struct volume *v, int argc, char **argv)
{
    struct entry *e = stl_map_lba_iterate(v->map, NULL);
    while (e != NULL) {
        printf("%d +%d -> %d.%d (%d%s)\n", (int)e->lba, e->len,
               e->pba.band, e->pba.offset, e->seq, e->dirty ? " DIRTY" : "");
        e = stl_map_lba_iterate(v->map, e);
    }
}

void cmd_read(struct volume *v, int argc, char **argv)
{
    int lba = atoi(argv[1]), len = atoi(argv[2]);
    host_read(v, lba, cmdline_buf, len*SECTOR_SIZE);
}

int bad;
void cmd_verify(struct volume *v, int argc, char **argv)
{
    int lba = atoi(argv[1]), len = atoi(argv[2]);
    host_read(v, lba, cmdline_buf, len*SECTOR_SIZE);
    int i, val = atoi(argv[3]);
    unsigned char *ptr = cmdline_buf;
    for (i = 0; i < len; i++) 
        if (ptr[i*SECTOR_SIZE] != val) {
            printf("bad value %d (%d) at %d\n", ptr[i*SECTOR_SIZE], val, lba+i);
            bad++;
        }
}

void cmd_write(struct volume *v, int argc, char **argv)
{
    int lba = atoi(argv[1]), len = atoi(argv[2]), value = 0;
    if (argc > 3)
        value = atoi(argv[3]);
    memset(cmdline_buf, value, len*SECTOR_SIZE);
    host_write(v, lba, cmdline_buf, len*SECTOR_SIZE);
}

void map_overlap(struct volume *v)
{
    struct entry *e = stl_map_lba_iterate(v->map, NULL);
    int bad = 0;
    while (e != NULL) {
        struct entry *tmp = stl_map_lba_iterate(v->map, e);
        if (tmp && e->lba + e->len > tmp->lba) {
            printf("  error: %d+%d overlaps %d+%d\n", (int)e->lba, e->len,
                   (int)tmp->lba, tmp->len);
            bad = 1;
        }
        e = tmp;
    }
    if (!bad)
        printf("OK\n");
}

void rmap_overlap(struct volume *v)
{
    struct entry *e = stl_map_pba_iterate(v->map, NULL);
    int bad = 0;
    while (e != NULL) {
        struct entry *tmp = stl_map_pba_iterate(v->map, e);
        if (tmp && e->pba.band == tmp->pba.band &&
            e->pba.offset + e->len > tmp->pba.offset) {
            printf("  error: %d.%d+%d overlaps %d.%d+%d\n",
                   e->pba.band, e->pba.offset, e->len,
                   tmp->pba.band, tmp->pba.offset, tmp->len);
            bad = 1;
        }
        if (tmp) {
            if (e->pba.band == tmp->pba.band &&
                e->pba.offset + e->len > tmp->pba.offset) {
                printf("  error2: %d.%d+%d overlaps %d.%d+%d\n",
                       e->pba.band, e->pba.offset, e->len,
                       tmp->pba.band, tmp->pba.offset, tmp->len);
                bad = 1;
            }
        }
        e = tmp;
    }
    if (!bad)
        printf("OK\n");
}
    
void cmd_overlap(struct volume *v, int argc, char **argv)
{
    if (!strcmp(argv[1], "fwd"))
        map_overlap(v);
    else if (!strcmp(argv[1], "rev"))
        rmap_overlap(v);
    else
        printf("ERROR: bad command: overlap %s\n", argv[1]);
}

void cmd_break(struct volume *v, int argc, char **argv)
{
}

void cmd_trim(struct volume *v, int argc, char **argv)
{
    int lba = atoi(argv[1]), len = atoi(argv[2]);
    host_trim(v, lba, len);     /* len in sectors */
}

struct {
    char *cmd;
    void (*fn)(struct volume *, int, char **);
} cmdtable[] = {
    {.cmd = "clean", .fn=cmd_clean},
    {.cmd = "checkpoint", .fn=cmd_checkpoint},
    {.cmd = "print", .fn=cmd_print},
    {.cmd = "rprint", .fn=cmd_rprint},
    {.cmd = "fprint", .fn=cmd_fprint},
    {.cmd = "read", .fn=cmd_read},
    {.cmd = "verify", .fn=cmd_verify},
    {.cmd = "write", .fn=cmd_write},
    {.cmd = "break", .fn=cmd_break},
    {.cmd = "trim", .fn=cmd_trim},
    {.cmd = "overlap", .fn=cmd_overlap}
};

void verify_free(struct volume *v)
{
    int g, i, nf;
    for (g = 0; g < v->n_groups; g++) {
        int base = 1 + v->map_size + v->group_size * g;
        for (i = nf = 0; i < v->group_size; i++)
            if (v->band[base+i].type == BAND_TYPE_FREE)
                nf++;
        assert(nf == v->groups[g].count[BAND_TYPE_FREE]);
    }
}

int main(int argc, char **argv)
{
    struct volume *v = init_volume(argv[1]);
    print_metadata(v);

    char line[80];
    cmdline_buf = valloc(0x1000000); /* 16M */
    while (fgets(line, sizeof(line), stdin) != NULL) {
        char **ap, *av[10], *ptr;
        printf("> %s", line);
        if (line[0] == '#' || line[0] == '\n')
            continue;
        for (ap = av, ptr = line; (*ap = strsep(&ptr, " \t\n")) != NULL;)
            if (**ap != '\0')
                if (++ap >= &av[10])
                    break;
        int i, ac = ap-&av[0],
            n_cmds = sizeof(cmdtable)/sizeof(cmdtable[0]);
        if (ac == 0)
            continue;
        if (!strcmp(av[0], "quit"))
            break;
        if (!strcmp(av[0], "close"))
            delete_volume(v);
        else if (!strcmp(av[0], "open"))
            v = init_volume(argv[1]);
        else {
            for (i = 0; i < n_cmds; i++) 
                if (!strcmp(av[0], cmdtable[i].cmd)) {
                    cmdtable[i].fn(v, ac, av);
                    break;
                }
            if (i == n_cmds)
                printf("invalid command: %s\n", av[0]);
        }
        verify_free(v);
    }
    printf("verification errors: %d\n", bad);
    return 0;
}
