#include <ipc/ipc.h>
#include <debug/log.h>

int ipc_unix_init(void);
int ipc_shm_init(void);

int ipc_init(void)
{
	int r = ipc_unix_init();
	if (r != IPC_OK)
		return r;
	r = ipc_shm_init();
	if (r != IPC_OK)
		return r;

	log_info("ipc", "namespaces mounted at /dev/ipc and /dev/shm");
	return IPC_OK;
}
