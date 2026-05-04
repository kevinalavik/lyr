#include <init/init.h>
#include <debug/log.h>
#include <fs/vfs.h>
#include <lib/string.h>
#include <mm/heap.h>
#include <net/net.h>
#include <sched/sched.h>
#include <util/kprintf.h>

#define ANSI_GRAY "\x1b[38;2;120;120;120m"
#define ANSI_RESET "\x1b[0m"
#define INIT_LOG(fmt, ...) \
	kprintf(ANSI_GRAY "init: " fmt ANSI_RESET, ##__VA_ARGS__)

static char fs_type_char(vfs_mode_t mode)
{
	if (VFS_S_ISDIR(mode))
		return 'd';
	if (VFS_S_ISCHR(mode))
		return 'c';
	if (VFS_S_ISREG(mode))
		return '-';
	return '?';
}

static void fs_mode_string(vfs_mode_t mode, char out[11])
{
	out[0] = fs_type_char(mode);
	out[1] = (mode & VFS_S_IRUSR) ? 'r' : '-';
	out[2] = (mode & VFS_S_IWUSR) ? 'w' : '-';
	out[3] = (mode & VFS_S_IXUSR) ? 'x' : '-';
	out[4] = (mode & VFS_S_IRGRP) ? 'r' : '-';
	out[5] = (mode & VFS_S_IWGRP) ? 'w' : '-';
	out[6] = (mode & VFS_S_IXGRP) ? 'x' : '-';
	out[7] = (mode & VFS_S_IROTH) ? 'r' : '-';
	out[8] = (mode & VFS_S_IWOTH) ? 'w' : '-';
	out[9] = (mode & VFS_S_IXOTH) ? 'x' : '-';
	out[10] = '\0';
}

static int fs_join_path(const char *parent, const char *name, char *out,
						size_t out_len)
{
	size_t parent_len = strlen(parent);
	size_t name_len = strlen(name);
	int need_slash = !(parent_len == 1 && parent[0] == '/');
	size_t total = parent_len + (need_slash ? 1 : 0) + name_len;
	if (total + 1 > out_len)
		return VFS_ERR_NAMETOOLONG;

	memcpy(out, parent, parent_len);
	size_t pos = parent_len;
	if (need_slash)
		out[pos++] = '/';
	memcpy(out + pos, name, name_len);
	out[pos + name_len] = '\0';
	return VFS_OK;
}

__attribute__((unused)) static void fs_list_recursive(const char *path)
{
	vfs_stat_t st;
	int r = vfs_stat(path, &vfs_root_cred, &st);
	if (r != VFS_OK) {
		kprintf("? %s status=%d\n", path, r);
		return;
	}

	char mode[11];
	fs_mode_string(st.mode, mode);
	kprintf("%-10s %3u %5u:%-5u %10llu %04o %s\n", mode, st.nlink, st.uid,
			st.gid, st.size, st.mode & VFS_S_PERM, path);

	if (!VFS_S_ISDIR(st.mode))
		return;

	vfs_node_t *dir = NULL;
	r = vfs_resolve(path, &vfs_root_cred, &dir);
	if (r != VFS_OK) {
		kprintf("! cannot open dir %s status=%d\n", path, r);
		return;
	}

	for (size_t i = 0;; i++) {
		vfs_dirent_t ent;
		r = vfs_readdir(dir, i, &ent);
		if (r == VFS_ERR_NOENT)
			break;
		if (r != VFS_OK) {
			kprintf("! readdir %s[%zu] status=%d\n", path, i, r);
			break;
		}

		char child_path[256];
		r = fs_join_path(path, ent.name, child_path, sizeof(child_path));
		if (r != VFS_OK) {
			kprintf("! path too long under %s/%s\n", path, ent.name);
			continue;
		}
		fs_list_recursive(child_path);
	}

	vfs_node_release(dir);
	kprintf("\n");
}

static size_t ipv4_prefix_len(uint32_t mask)
{
	size_t n = 0;
	while (mask & 0x80000000u) {
		n++;
		mask <<= 1;
	}
	return n;
}

static void init_print_ip_addr(void)
{
	size_t idx = 1;
	for (netdev_t *dev = net_first_dev(); dev; dev = dev->next, idx++) {
		uint32_t broadcast = dev->ipv4_addr | ~dev->ipv4_netmask;
		char ip[24];
		char brd[24];
		net_ipv4_format(dev->ipv4_addr, ip, sizeof(ip));
		net_ipv4_format(broadcast, brd, sizeof(brd));

		kprintf("%zu: %s: <%s,BROADCAST,MULTICAST> mtu %u\n", idx, dev->name,
				dev->link_up ? "UP,LOWER_UP" : "DOWN", dev->mtu);
		kprintf(
			"    link/ether %02x:%02x:%02x:%02x:%02x:%02x brd ff:ff:ff:ff:ff:ff\n",
			dev->mac[0], dev->mac[1], dev->mac[2], dev->mac[3], dev->mac[4],
			dev->mac[5]);
		if (dev->ipv4_addr) {
			kprintf("    inet %s/%zu brd %s scope global %s%s\n", ip,
					ipv4_prefix_len(dev->ipv4_netmask), brd,
					dev->dhcp_configured ? "dynamic " : "", dev->name);
			kprintf("       valid_lft forever preferred_lft forever\n");
		}
	}
}

static void init_fetch_site(const char *host)
{
	char *http = kzalloc(4096);
	if (!http) {
		kprintf("init: HTTP GET http://%s/ skipped: no memory\n", host);
		return;
	}

	size_t done = 0;
	net_http_response_t res;
	memset(&res, 0, sizeof(res));
	kprintf("HTTP GET http://%s/\n", host);
	int r = net_http_get(host, "/", http, 4095, &done, &res, 5000);
	if (r == VFS_OK) {
		http[done < 4095 ? done : 4095] = '\0';
		kprintf("HTTP status=%u bytes=%zu body=%zu\n", res.status, done,
				res.body_len);
		kprintf("%s\n", http);
	} else {
		kprintf("HTTP fetch %s failed status=%d\n", host, r);
	}
	kfree(http);
}

#define CAT_FILE(path_)                                                        \
	do {                                                                       \
		vfs_file_t *file__ = NULL;                                             \
		int r__ = vfs_open((path_), VFS_O_RDONLY, 0, &vfs_root_cred, &file__); \
		if (r__ != VFS_OK) {                                                   \
			kprintf("cat: failed to open %s status=%d\n", (path_), r__);       \
			break;                                                             \
		}                                                                      \
                                                                               \
		size_t cap__ = file__->node->size;                                     \
		if (cap__ == 0 || VFS_S_ISCHR(file__->node->mode))                     \
			cap__ = 4096;                                                      \
                                                                               \
		char *buf__ = kzalloc(cap__ + 1);                                      \
		if (!buf__) {                                                          \
			kprintf("cat: failed to allocate buffer for %s\n", (path_));       \
			vfs_close(file__);                                                 \
			break;                                                             \
		}                                                                      \
                                                                               \
		size_t done__ = 0;                                                     \
		r__ = vfs_read(file__, buf__, cap__, &done__);                         \
		vfs_close(file__);                                                     \
		if (r__ != VFS_OK) {                                                   \
			kprintf("cat: failed to read %s status=%d\n", (path_), r__);       \
			kfree(buf__);                                                      \
			break;                                                             \
		}                                                                      \
                                                                               \
		buf__[done__] = '\0';                                                  \
		kprintf("%s", buf__);                                                  \
		kfree(buf__);                                                          \
	} while (0)

#define FETCH_SITE(host_) init_fetch_site((host_))

__attribute__((unused)) static void init_play_starwars_telnet(void)
{
	char *buf = kzalloc(4097);
	if (!buf) {
		INIT_LOG("starwars: no memory\n");
		return;
	}

	INIT_LOG("starwars: connecting to starwarstel.net:23\n");

	net_tcp_conn_t *conn = NULL;
	int r = net_tcp_connect("starwarstel.net", 23, &conn, 15000);
	if (r != VFS_OK) {
		INIT_LOG("starwars: connect failed status=%d\n", r);
		kfree(buf);
		return;
	}

	INIT_LOG("starwars: connected\n");

	for (;;) {
		size_t got = 0;

		r = net_tcp_recv(conn, buf, 4096, &got, 60000);
		if (r != VFS_OK || got == 0) {
			INIT_LOG("starwars: recv stopped status=%d got=%zu\n", r, got);
			break;
		}

		buf[got] = '\0';
		kprintf("\x1b[2J\x1b[H%s", buf);
	}

	net_tcp_close(conn);
	kfree(buf);
}

void init_proc_entry(void *arg)
{
	(void)arg;

	CAT_FILE("/etc/banner");
	kprintf("\n");

	INIT_LOG("Hello, World!\n");
	INIT_LOG("motd\n");
	CAT_FILE("/etc/motd");
	INIT_LOG("netdevs\n");
	CAT_FILE("/dev/net/devices");
	INIT_LOG("net routes\n");
	CAT_FILE("/dev/net/routes");
	INIT_LOG("ip addr\n");
	init_print_ip_addr();

	{
		netdev_t *def = net_default_dev();
		if (def) {
			char ip[24];
			char gw[24];
			char mask[24];
			net_ipv4_format(def->ipv4_addr, ip, sizeof(ip));
			net_ipv4_format(def->ipv4_gateway, gw, sizeof(gw));
			net_ipv4_format(def->ipv4_netmask, mask, sizeof(mask));
			INIT_LOG(
				"default netdev=%s ip=%s netmask=%s gateway=%s dns=%u.%u.%u.%u link=%s\n",
				def->name, ip, mask, gw, (def->dns_server >> 24) & 0xff,
				(def->dns_server >> 16) & 0xff, (def->dns_server >> 8) & 0xff,
				def->dns_server & 0xff, def->link_up ? "up" : "down");
		} else {
			INIT_LOG("default netdev: none\n");
		}

		uint32_t probe_ip = net_ipv4(8, 8, 8, 8);
		uint32_t next_hop = 0;
		netdev_t *route_dev = net_route(probe_ip, &next_hop);
		char dst[24];
		char hop[24];
		net_ipv4_format(probe_ip, dst, sizeof(dst));
		net_ipv4_format(next_hop, hop, sizeof(hop));
		INIT_LOG("route lookup dst=%s dev=%s next_hop=%s\n", dst,
				 route_dev ? route_dev->name : "none", hop);
	}

#if _DEBUG
	{
		vfs_node_t *dir = NULL;
		int r = vfs_resolve("/dev/net", &vfs_root_cred, &dir);
		if (r == VFS_OK) {
			for (size_t i = 0;; i++) {
				vfs_dirent_t ent;
				r = vfs_readdir(dir, i, &ent);
				if (r == VFS_ERR_NOENT)
					break;
				if (r != VFS_OK) {
					INIT_LOG("net: readdir /dev/net[%zu] status=%d\n", i, r);
					break;
				}

				size_t name_len = strlen(ent.name);
				if (name_len < 6 ||
					strcmp(ent.name + name_len - 5, ".info") != 0)
					continue;

				char path[128];
				r = fs_join_path("/dev/net", ent.name, path, sizeof(path));
				if (r != VFS_OK)
					continue;

				INIT_LOG("%s:\n", path);
				CAT_FILE(path);
				kprintf("\n");
			}
			vfs_node_release(dir);
		} else {
			INIT_LOG("net: failed to open /dev/net status=%d\n", r);
		}
	}

	INIT_LOG("filesystem tree\n");
	kprintf("%-10s %3s %11s %10s %4s %s\n", "mode", "lnk", "uid:gid", "size",
			"perm", "path");
	fs_list_recursive("/");
#endif

#if _DEBUG
	{
		vfs_node_t *dir = NULL;
		int r = vfs_resolve("/dev/pci", &vfs_root_cred, &dir);
		if (r != VFS_OK) {
			INIT_LOG("pci: failed to open /dev/pci status=%d\n", r);
			return;
		}

		for (size_t i = 0;; i++) {
			vfs_dirent_t ent;
			r = vfs_readdir(dir, i, &ent);
			if (r == VFS_ERR_NOENT)
				break;
			if (r != VFS_OK) {
				INIT_LOG("pci: readdir /dev/pci[%zu] status=%d\n", i, r);
				break;
			}

			if (!strcmp(ent.name, "devices"))
				continue;

			char path[128];
			r = fs_join_path("/dev/pci", ent.name, path, sizeof(path));
			if (r != VFS_OK)
				continue;

			vfs_stat_t st;
			r = vfs_stat(path, &vfs_root_cred, &st);
			if (r != VFS_OK || !VFS_S_ISDIR(st.mode))
				continue;

			char info_path[160];
			r = fs_join_path(path, "info", info_path, sizeof(info_path));
			if (r != VFS_OK)
				continue;

			INIT_LOG("%s:\n", info_path);
			CAT_FILE(info_path);
			kprintf("\n");
		}

		vfs_node_release(dir);
	}
#endif

	{
		uint32_t target = net_ipv4(8, 8, 8, 8);
		const uint16_t ident = 0x4c59;
		const uint16_t count = 16;
		uint16_t sent = 0;
		uint16_t received = 0;
		uint64_t min_ms = (uint64_t)-1;
		uint64_t max_ms = 0;
		uint64_t total_ms = 0;
		char target_ip[24];

		net_ipv4_format(target, target_ip, sizeof(target_ip));
		INIT_LOG("PING %s: 40 data bytes\n", target_ip);
		for (uint16_t seq = 1; seq <= count; seq++) {
			net_ping_result_t reply;
			sent++;
			int r = net_ping_echo(target, ident, seq, 1500, &reply);
			if (r == VFS_OK) {
				received++;
				if (reply.time_ms < min_ms)
					min_ms = reply.time_ms;
				if (reply.time_ms > max_ms)
					max_ms = reply.time_ms;
				total_ms += reply.time_ms;
				char reply_ip[24];
				net_ipv4_format(reply.src_ip, reply_ip, sizeof(reply_ip));
				INIT_LOG("%u bytes from %s: icmp_seq=%u ttl=%u time=%llums\n",
						 reply.bytes, reply_ip, reply.seq, reply.ttl,
						 (unsigned long long)reply.time_ms);
			} else {
				INIT_LOG("Request timeout for icmp_seq %u (status=%d)\n", seq,
						 r);
			}
		}

		uint16_t lost = sent - received;
		uint16_t loss = sent ? (uint16_t)((lost * 100) / sent) : 0;
		INIT_LOG("--- %s ping statistics ---\n", target_ip);
		INIT_LOG("%u packets transmitted, %u received, %u%% packet loss\n",
				 sent, received, loss);
		if (received) {
			INIT_LOG("round-trip min/avg/max = %llu/%llu/%llums\n",
					 (unsigned long long)min_ms,
					 (unsigned long long)(total_ms / received),
					 (unsigned long long)max_ms);
		}
	}

	FETCH_SITE("127.0.0.1");
	FETCH_SITE("example.com");

	/* uncomment for a supprise :^) */
	// init_play_starwars_telnet();

	INIT_LOG("no shell, exiting.\n");
	sched_thread_exit(0);
}
