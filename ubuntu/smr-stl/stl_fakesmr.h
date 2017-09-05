/*
 * file:        stl_fakesmr.h
 * description: fake SMR functions for image files and conventional disks
 */

#ifndef __STL_FAKESMR_H__
#define __STL_FAKESMR_H__

struct smr;
int smr_n_bands(struct smr *dev);
int smr_band_size(struct smr *dev);
int smr_write_pointer(struct smr *dev, int band);
int smr_init(char *dev, int band_size);
struct smr *smr_open(const char *name);
void smr_close(struct smr *dev);
void smr_read(struct smr *dev, unsigned band, unsigned offset, void *buf,
              unsigned n_sectors);
void smr_write(struct smr *dev, unsigned band, unsigned offset, const void *buf,
               unsigned n_sectors);
void smr_reset_pointer(struct smr *dev, unsigned band);
void smr_reset_all(struct smr *dev);

#endif
