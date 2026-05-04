#ifndef _LYR_DRV_DRIVER_H
#define _LYR_DRV_DRIVER_H

#include <stddef.h>
#include <stdint.h>

#define DRIVER_NAME_MAX 31
#define DRIVER_ENTRY_MAX 31
#define DRIVER_PATH_MAX 95
#define DRIVER_MAGIC 0x4C445256u /* LDRV */
#define DRIVER_ABI_VERSION 1

typedef struct driver {
	char name[DRIVER_NAME_MAX + 1];
	char image_path[DRIVER_PATH_MAX + 1];
	int32_t pid;
	int status;
	void *process;
	void *image;
	size_t image_size;
	const struct driver_metadata *metadata;
} driver_t;

typedef int (*driver_entry_t)(driver_t *driver);
typedef void (*driver_thread_entry_t)(void *);

typedef struct driver_metadata {
	uint32_t magic;
	uint32_t abi_version;
	const char *name;
	driver_entry_t entry;
	void (*exit)(driver_t *driver);
	const char *const *exports;
	size_t export_count;
	const char *const *imports;
	size_t import_count;
} driver_metadata_t;

int driver_manager_init(void);
void driver_log(driver_t *driver, const char *level, const char *message);
int driver_spawn_thread(driver_t *driver, const char *name,
						driver_thread_entry_t entry, void *arg);

#endif /* _LYR_DRV_DRIVER_H */
