/*
 * file:        stl-plugin.c
 * description: NBD plugin for shingled translation layer
 */

#include <stdlib.h>
#include <string.h>
#include <nbdkit-plugin.h>
#include <assert.h>
#include "stl.h"
#include "stl_public.h"

#define THREAD_MODEL NBDKIT_THREAD_MODEL_SERIALIZE_ALL_REQUESTS
#define max(a, b) (((a) > (b)) ? (a) : (b))
#define min(a, b) (((a) < (b)) ? (a) : (b))

void *smr_dev;

int stlplugin_config(const char *key, const char *value)
{
    if (!strcmp(key, "device")) {
        if (!(smr_dev = init_volume(value))) {
            nbdkit_error("failed to open %s\n", value);
            return -1;
        }
        return 1;
    }
    else {
        nbdkit_error("bad option: %s=%s\n", key, value);
        return -1;
    }
}

int stlplugin_config_complete(void)
{
    if (!smr_dev) {
        nbdkit_error("no device specified\n");
        return -1;
    }
    return 1;
}

void *stlplugin_open(int readonly)
{
    return smr_dev;
}

void stlplugin_close(void *handle)
{
}

int64_t stlplugin_get_size(void *handle)
{
    return volume_size(smr_dev);
}

int stlplugin_is_rotational(void *handle)
{
    return 1;
}

int stlplugin_can_trim(void *handle)
{
    return 0;
}

int stlplugin_can_flush (void *handle)
{
    return 1;
}

int stlplugin_flush (void *handle)
{
    return 0;
}

int stlplugin_pread(void *handle, void *buf, uint32_t count, uint64_t offset)
{
    // assert((offset & 511) == 0); 
    // assert(((long long)buf & 511) == 0);
    if ((offset & 4095) != 0) {
        nbdkit_debug("unaligned READ\n");
        void *tmp = valloc(4096);
        host_read(smr_dev, offset/4096, tmp, 4096);
        int i = offset % 4096, len = min(4096 - i, count);
        memcpy(buf, tmp+i, len);
        buf += len;
        offset += len;
        count -= len;
        free(tmp);
    }

    host_read(smr_dev, offset/4096, buf, count & ~4095);

    if (count % 4096 != 0) {
        nbdkit_debug("unaligned READ length\n");
        int n = (count/4096) * 4096;
        buf += n;
        offset += n;
        count = count % 4096;
        printf("before allocation \n");
        void *tmp = valloc(4096);
        printf("after allocation \n");
        host_read(smr_dev, offset/4096, tmp, 4096);
        memcpy(buf, tmp, count);
        free(tmp);
    }

    return 0;
}

int stlplugin_pwrite(void *handle, const void *buf, uint32_t count,
                     uint64_t offset)
{
    // assert((offset & 511) == 0);
    // assert(((long long)buf & 511) == 0);

    if ((offset & 4095) != 0) {
        nbdkit_debug("unaligned WRITE\n");
        void *tmp = valloc(4096);
        host_read(smr_dev, offset/4096, tmp, 4096);
        int i = offset % 4096, len = min(4096 - i, count);
        memcpy(tmp+i, buf, len);
        host_write(smr_dev, offset/4096, tmp, 4096);
        buf += len;
        offset += len;
        count -= len;
        free(tmp);
    }

    host_write(smr_dev, offset/4096, buf, count & ~4095);

    if (count % 4096 != 0) {
        nbdkit_debug("unaligned WRITE length\n");
        int n = (count/4096) * 4096;
        buf += n;
        offset += n;
        count = count % 4096;
        void *tmp = valloc(4096);
        host_read(smr_dev, offset/4096, tmp, 4096);
        memcpy(tmp, buf, count);
        host_write(smr_dev, offset/4096, tmp, 4096);
        free(tmp);
    }

    return 0;
}


int stlplugin_trim(void *handle, uint32_t count, uint64_t offset)
{
    int n = offset % 4096;
    offset += n;
    count -= n;
    if (n != 0 || count % 4096 != 0)
        nbdkit_debug("unaligned TRIM\n");

    count = count % 4096;
    host_trim(smr_dev, offset / 4096, count / 4096);

    return 0;
}

static struct nbdkit_plugin plugin = {
  .name              = "STLplugin",
  .config            = stlplugin_config,
  .config_complete   = stlplugin_config_complete,
  .open              = stlplugin_open,
  .close             = stlplugin_close,
  .is_rotational     = stlplugin_is_rotational,
  .can_trim          = stlplugin_can_trim,
  .get_size          = stlplugin_get_size,
  .pread             = stlplugin_pread,
  .pwrite            = stlplugin_pwrite,
  .trim              = stlplugin_trim,
  .can_flush         = stlplugin_can_flush,
  .flush             = stlplugin_flush
};

NBDKIT_REGISTER_PLUGIN(plugin)
