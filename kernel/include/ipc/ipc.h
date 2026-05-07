#ifndef _LYR_IPC_IPC_H
#define _LYR_IPC_IPC_H

#include <stddef.h>
#include <stdint.h>
#include <errno.h>

#define IPC_NAME_MAX 31
#define IPC_SHM_NAME_MAX 31
#define IPC_OK 0
#define IPC_ERR_NOENT (-ENOENT)
#define IPC_ERR_NOMEM (-ENOMEM)
#define IPC_ERR_EXIST (-EEXIST)
#define IPC_ERR_INVAL (-EINVAL)

typedef enum {
	IPC_MSG_CALL = 1,
	IPC_MSG_NOTIFY = 2,
	IPC_MSG_SHM = 3,
} ipc_msg_kind_t;

typedef struct ipc_msg {
	ipc_msg_kind_t kind;
	uint32_t type;
	uint32_t flags;
	const void *in;
	size_t in_len;
	void *out;
	size_t out_len;
	size_t *actual;
} ipc_msg_t;

typedef int (*ipc_handler_t)(const ipc_msg_t *msg, void *ctx);

typedef struct ipc_shm {
	char name[IPC_SHM_NAME_MAX + 1];
	size_t size;
	void *data;
} ipc_shm_t;

int ipc_init(void);
int ipc_endpoint_register(const char *name, int32_t owner, ipc_handler_t handler,
						  void *ctx);
int ipc_endpoint_unregister(const char *name);
int ipc_call(const char *name, const ipc_msg_t *msg);
int ipc_notify(const char *name, uint32_t type, const void *in, size_t in_len);
int ipc_shm_create(const char *name, size_t size, ipc_shm_t **out);
int ipc_shm_open(const char *name, ipc_shm_t **out);
int ipc_shm_unlink(const char *name);
int ipc_snapshot(char *buf, size_t len);

#endif /* _LYR_IPC_IPC_H */
