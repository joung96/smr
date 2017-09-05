/*
 * file:        stl_fakesmr.c
 * description: fake SMR functions for image files and conventional disks
 */


#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#define __USE_GNU               /* for O_DIRECT */
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fs.h>

struct smr {
    int fd;
    int n_bands;
    int band_size;
    off_t wp_position;
    int wp_sectors;
    int *write_pointers;
};

struct fakeSMR_trailer {
    int n_bands;
    int band_size;
};

#define SECTOR_SIZE 4096
#define BANDS_PER_SECTOR (SECTOR_SIZE/sizeof(int))

int smr_n_bands(struct smr *dev)
{
    return dev->n_bands;
}

int smr_band_size(struct smr *dev)
{
    return dev->band_size;
}

int smr_write_pointer(struct smr *dev, int band)
{
    assert(band >= 0 && band < dev->n_bands);
    return dev->write_pointers[band];
}

/* initialze a fakeSMR device with a given band size, setting all
 * write pointers to zero. Returns the total number of bands. Used for
 * initial formatting only.
 */
int smr_init(char *dev, int band_size)
{
    struct stat sb;
    off_t len;
    int fd = open(dev, O_RDWR);
    if (fstat(fd, &sb) < 0)
        assert(0);
    if (S_ISREG(sb.st_mode)) {
        if ((len = lseek(fd, 0, SEEK_END)) < 0)
            assert(0);
        len = len / SECTOR_SIZE;
    }
#ifdef linux
    else if (S_ISBLK(sb.st_mode)) {
        if (ioctl(fd, BLKGETSIZE, &len) < 0)
            assert(0);
        len = len/8;
    }
#endif
    else {
        printf("bad file type: %x\n", sb.st_mode);
        assert(0);
    }
    int n_bands = len / band_size;
    int n_sectors = (n_bands + BANDS_PER_SECTOR - 1) / BANDS_PER_SECTOR;
    while (n_bands * band_size + n_sectors + 1 > len)
        n_bands--;

    struct fakeSMR_trailer *trail = calloc(sizeof(*trail), 1);
    trail->band_size = band_size;
    trail->n_bands = n_bands;
    //pwrite(fd, trail, sizeof(*trail), (len-1)*SECTOR_SIZE);
    /* changed to use lseek because of pwrite error
    */
    lseek(fd, (len-1)*SECTOR_SIZE, SEEK_SET);
    write(fd, trail, sizeof(*trail));

    void *buf = calloc(SECTOR_SIZE, 1);
    int i;
    lseek(fd, (len-n_sectors-1)*SECTOR_SIZE, SEEK_SET);
    for (i = 0; i < n_sectors; i++)
        write(fd, buf, SECTOR_SIZE);

    free(buf);
    free(trail);
    close(fd);

    return n_bands;
}

/* open the fake SMR device. Note that we mmap the write pointers at
 * the top of the file / device so that the kernel will (hopefully)
 * keep them up-to-date without a vast amount of overhead.
 */
void *smr_open(const char *name)
{
    off_t len;
    void *buf = NULL, *write_pointers = NULL;
    struct smr *dev = NULL;

    int fd = open(name, O_RDWR);
    if (fd < 0)
        goto bail;

    struct stat sb;
    if (fstat(fd, &sb) < 0)
        goto bail;
    if (S_ISREG(sb.st_mode)) {
        if ((len = lseek(fd, 0, SEEK_END)) < 0)
            goto bail;
        len = len/SECTOR_SIZE;
    }
#ifdef linux
    else if (S_ISBLK(sb.st_mode)) {
        if (ioctl(fd, BLKGETSIZE, &len) < 0)
            goto bail;
        len = len/8;
    }
#endif
    else {
        printf("bad file type: %x\n", sb.st_mode);
        goto bail;
    }

    buf = valloc(SECTOR_SIZE);
    if (pread(fd, buf, SECTOR_SIZE, (len-1)*SECTOR_SIZE) < 0)
        goto bail;

    struct fakeSMR_trailer *trail = buf;
    if ((dev = calloc(sizeof(*dev), 1)) == NULL)
        goto bail;
    dev->n_bands = trail->n_bands;
    dev->band_size = trail->band_size;

    int n_sectors = (dev->n_bands + BANDS_PER_SECTOR - 1) / BANDS_PER_SECTOR;
    if (len - n_sectors < dev->n_bands * dev->band_size) {
        printf("Invalid device: %d bands of %d, len %lld\n", dev->n_bands,
               dev->band_size, (long long)len);
        goto bail;
    }

    dev->wp_sectors = n_sectors;

    write_pointers = mmap(NULL, n_sectors * SECTOR_SIZE, PROT_READ|PROT_WRITE,
                          MAP_SHARED, fd, (len-1-n_sectors)*SECTOR_SIZE);
    if (write_pointers == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    if (write_pointers == NULL)
        goto bail;
    dev->write_pointers = write_pointers;

    /* O_DIRECT commented out--may use again later*/
    // int flags;
    // flags = fcntl(fd, F_GETFL, 0);
    // flags |= O_DIRECT;
    // fcntl(fd, F_SETFL, flags);

    dev->fd = fd;

    free(buf);

    return dev;

bail:
    if (buf)
        free(buf);
    if (dev)
        free(dev);
    perror("fakeSMR open failed");
    close(fd);
    return NULL;
}

void smr_close(struct smr *dev)
{
    munmap(dev->write_pointers, dev->wp_sectors*SECTOR_SIZE);
    close(dev->fd);
    free(dev);
}

/* read from a given band and offset. Note that unsigned values make
 * length checking a bit easier...
 */
void smr_read(struct smr *dev, unsigned band, unsigned offset, void *buf,
              unsigned n_sectors)
{
    assert(band < dev->n_bands && offset < dev->band_size &&
           offset+n_sectors <= dev->write_pointers[band]);
    // assert(((long long)buf & 511) == 0);   /* Used for O_DIRECT */
    off_t position = (dev->band_size * band + offset) * SECTOR_SIZE;
    int val = pread(dev->fd, buf, n_sectors*SECTOR_SIZE, position);
    assert(val > 0);
}

/* write to a given band and offset. Checks that SMR constraint is
 * being followed, and updates the write pointer.
 */
void smr_write(struct smr *dev, unsigned band, unsigned offset, const void *buf,
               unsigned n_sectors)
{
    assert(band < dev->n_bands && offset+n_sectors <= dev->band_size);
    assert(offset == dev->write_pointers[band]);
    // assert(((long long)buf & 511) == 0);	    /* Used for O_DIRECT */
    off_t position = (dev->band_size * band + offset) * SECTOR_SIZE;
    //int val = pwrite(dev->fd, buf, n_sectors*SECTOR_SIZE, position);
    /* changed to use lseek because of pwrite error
    */
    lseek(dev->fd, position, SEEK_SET);
    int val = write(dev->fd, buf, n_sectors*SECTOR_SIZE);
    assert(val > 0);
    dev->write_pointers[band] += n_sectors;
}

void smr_reset_pointer(struct smr *dev, unsigned band)
{
    assert(band < dev->n_bands);
    dev->write_pointers[band] = 0;
}

void smr_reset_all(struct smr *dev)
{
    int i;
    for (i = 0; i < dev->n_bands; i++)
        dev->write_pointers[i] = 0;
}
