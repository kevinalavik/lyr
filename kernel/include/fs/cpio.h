#ifndef _LYR_FS_CPIO_H
#define _LYR_FS_CPIO_H

#include <stddef.h>
#include <stdint.h>

int cpio_newc_extract(const void *archive, size_t size, size_t *entries_out);

#endif /* _LYR_FS_CPIO_H */
