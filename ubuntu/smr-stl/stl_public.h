/*
 */

#ifndef __STL_PUBLIC_H__
#define __STL_PUBLIC_H__

struct volume;
struct volume *init_volume(const char *dev);
void delete_volume(struct volume *v);
void host_write(struct volume *v, lba_t lba, const void *buf, int bytes);
void host_trim(struct volume *v, lba_t lba, int sectors);
void host_read(struct volume *v, lba_t lba, void *buf, int bytes);
int64_t volume_size(struct volume *v);

#endif
