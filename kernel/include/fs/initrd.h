#ifndef _LYR_FS_INITRD_H
#define _LYR_FS_INITRD_H

#include <limine.h>

int initrd_load_from_limine(struct limine_module_response *modules);

#endif /* _LYR_FS_INITRD_H */
