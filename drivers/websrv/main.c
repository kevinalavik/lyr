#include <drv/driver.h>
#include <fs/vfs.h>
#include <lib/nanoprintf.h>
#include <lib/string.h>
#include <net/net.h>

static int append(char *out, size_t cap, size_t *pos, const char *s)
{
	size_t len = strlen(s);
	if (*pos + len >= cap)
		return VFS_ERR_INVAL;
	memcpy(out + *pos, s, len);
	*pos += len;
	out[*pos] = '\0';
	return VFS_OK;
}

static int appendf(char *out, size_t cap, size_t *pos, const char *fmt, ...)
{
	char tmp[512];
	va_list ap;
	va_start(ap, fmt);
	int n = npf_vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= sizeof(tmp))
		return VFS_ERR_INVAL;
	return append(out, cap, pos, tmp);
}

static int html_escape(char *out, size_t cap, size_t *pos, const char *s)
{
	for (; *s; s++) {
		switch (*s) {
		case '&':
			if (append(out, cap, pos, "&amp;"))
				return VFS_ERR_INVAL;
			break;
		case '<':
			if (append(out, cap, pos, "&lt;"))
				return VFS_ERR_INVAL;
			break;
		case '>':
			if (append(out, cap, pos, "&gt;"))
				return VFS_ERR_INVAL;
			break;
		case '"':
			if (append(out, cap, pos, "&quot;"))
				return VFS_ERR_INVAL;
			break;
		default: {
			char c[2] = { *s, 0 };
			if (append(out, cap, pos, c))
				return VFS_ERR_INVAL;
		}
		}
	}
	return VFS_OK;
}

static int html_escape_n(char *out, size_t cap, size_t *pos, const char *s,
						 size_t len)
{
	for (size_t i = 0; i < len; i++) {
		switch (s[i]) {
		case '&':
			if (append(out, cap, pos, "&amp;"))
				return VFS_ERR_INVAL;
			break;
		case '<':
			if (append(out, cap, pos, "&lt;"))
				return VFS_ERR_INVAL;
			break;
		case '>':
			if (append(out, cap, pos, "&gt;"))
				return VFS_ERR_INVAL;
			break;
		case '"':
			if (append(out, cap, pos, "&quot;"))
				return VFS_ERR_INVAL;
			break;
		default: {
			char c[2] = { s[i], 0 };
			if (append(out, cap, pos, c))
				return VFS_ERR_INVAL;
			break;
		}
		}
	}

	return VFS_OK;
}

static int url_path_decode(const char *src, char *dst, size_t cap)
{
	size_t pos = 0;

	while (*src && *src != ' ' && *src != '?' && pos + 1 < cap) {
		if (*src == '%' && src[1] && src[2]) {
			dst[pos++] = '_';
			src += 3;
		} else {
			dst[pos++] = *src++;
		}
	}

	dst[pos] = 0;

	if (!dst[0] || strstr(dst, ".."))
		return VFS_ERR_INVAL;

	return VFS_OK;
}

static int append_css(char *body, size_t cap, size_t *pos)
{
	return append(
		body, cap, pos,
		"<style>"
		":root{color-scheme:dark;--bg:#070b10;--panel:#0a1511;--panel2:#0d2018;--fg:#d6ffe7;--hot:#53ff9f;--cyan:#6ee7ff;--dim:#7d988a;--line:#1f4d38;--bad:#ff6b6b}"
		"*{box-sizing:border-box}"
		"body{margin:0;background:radial-gradient(circle at 50% -10%,#183b2a 0,#070b10 38rem);color:var(--fg);font:15px/1.45 ui-monospace,SFMono-Regular,Menlo,monospace}"
		"body:before{content:\"\";position:fixed;inset:0;pointer-events:none;background:repeating-linear-gradient(0deg,rgba(255,255,255,.035) 0 1px,transparent 1px 4px);mix-blend-mode:overlay}"
		"main{max-width:1100px;margin:0 auto;padding:32px 16px 48px}"
		".term{border:1px solid var(--line);background:rgba(4,12,9,.9);box-shadow:0 0 42px rgba(83,255,159,.13),inset 0 0 32px rgba(83,255,159,.05)}"
		".bar{display:flex;gap:8px;align-items:center;border-bottom:1px solid var(--line);padding:10px 14px;color:var(--dim)}"
		".dot{width:10px;height:10px;border-radius:50%;background:var(--hot);box-shadow:0 0 14px var(--hot)}"
		".screen{padding:24px}"
		"h1{font-size:34px;line-height:1;margin:0 0 8px;color:#fff}"
		"h2{margin:0 0 14px;color:#fff}"
		"p{max-width:75ch;color:#b9d7c5}"
		".logo{white-space:pre;color:var(--hot);text-shadow:0 0 14px rgba(83,255,159,.8);font-size:13px;margin:0 0 20px}"
		".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:12px;margin:22px 0}"
		".card{border:1px solid var(--line);padding:14px;background:rgba(13,32,24,.72)}"
		".k{display:block;color:var(--dim);font-size:12px;text-transform:uppercase}"
		".v{font-size:21px;color:var(--cyan);overflow-wrap:anywhere}"
		".btn,.mini{display:inline-block;border:1px solid var(--hot);background:var(--hot);color:#001b0c;font-weight:700;text-decoration:none}"
		".btn{margin-top:18px;padding:12px 16px}"
		".mini{padding:5px 8px;margin-right:6px;font-size:12px}"
		".ghost{background:transparent;color:var(--hot)}"
		"table{width:100%;border-collapse:collapse;margin-top:12px}"
		"th,td{padding:9px;border-bottom:1px solid #143424;text-align:left;vertical-align:middle}"
		"th{color:var(--dim);font-size:12px;text-transform:uppercase}"
		"a{color:var(--hot);text-decoration:none}"
		"a:hover{text-decoration:underline}"
		".size{text-align:right;color:var(--dim)}"
		".kind{color:var(--cyan)}"
		".path{color:var(--dim);margin-bottom:16px}"
		".modal{display:none;position:fixed;inset:0;background:rgba(0,0,0,.78);padding:28px;z-index:10}"
		".modal:target{display:block}"
		".modalbox{height:100%;border:1px solid var(--line);background:#050907;display:flex;flex-direction:column;box-shadow:0 0 35px rgba(83,255,159,.18)}"
		".modalbar{display:flex;justify-content:space-between;align-items:center;padding:10px 14px;border-bottom:1px solid var(--line);color:var(--dim)}"
		"iframe{flex:1;width:100%;border:0;background:#050907;color:#d6ffe7}"
		".close{color:var(--bad);font-weight:700}"
		"</style>");
}

/* ---------------- landing page ---------------- */

static int build_home(char *body, size_t cap, size_t *done)
{
	netdev_t *def = net_default_dev();
	char ip[24];
	char gw[24];

	const char *dev_name = "none";
	const char *link = "down";
	size_t devs = netdev_count();

	net_ipv4_format(0, ip, sizeof(ip));
	net_ipv4_format(0, gw, sizeof(gw));

	if (def) {
		dev_name = def->name;
		link = def->link_up ? "up" : "down";
		net_ipv4_format(def->ipv4_addr, ip, sizeof(ip));
		net_ipv4_format(def->ipv4_gateway, gw, sizeof(gw));
	}

	size_t pos = 0;

	append(
		body, cap, &pos,
		"<!doctype html><html><head><meta charset=\"utf-8\">"
		"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
		"<title>lyrOS live</title>");
	append_css(body, cap, &pos);

	append(
		body, cap, &pos,
		"</head><body><main><section class=\"term\">"
		"<div class=\"bar\"><span class=\"dot\"></span><span>lyrOS /dev/http0</span></div>"
		"<div class=\"screen\">"
		"<pre class=\"logo\">"
		" _             ___  ____\n"
		"| |_   _ _ __ / _ \\/ ___|\n"
		"| | | | | '__| | | \\___ \\\n"
		"| | |_| | |  | |_| |___) |\n"
		"|_|\\__, |_|   \\___/|____/\n"
		"   |___/</pre>");

	append(body, cap, &pos, "<div class=\"grid\">");
	appendf(
		body, cap, &pos,
		"<div class=\"card\"><span class=\"k\">default dev</span><span class=\"v\">%s</span></div>"
		"<div class=\"card\"><span class=\"k\">ip address</span><span class=\"v\">%s</span></div>"
		"<div class=\"card\"><span class=\"k\">gateway</span><span class=\"v\">%s</span></div>"
		"<div class=\"card\"><span class=\"k\">link</span><span class=\"v\">%s</span></div>"
		"<div class=\"card\"><span class=\"k\">netdevs</span><span class=\"v\">%zu</span></div>",
		dev_name, ip, gw, link, devs);
	append(body, cap, &pos, "</div>");

	append(
		body, cap, &pos,
		"<p>The filesystem browser exposes the mounted root tree. Directories can be opened, files can be downloaded, and small files can be previewed inline.</p>"
		"<a class=\"btn\" href=\"/fs/\">Goto file listing</a>"
		"</div></section></main></body></html>");

	*done = pos;
	return VFS_OK;
}

/* ---------------- directory listing ---------------- */

static int build_listing(const char *path, char *body, size_t cap, size_t *done)
{
	vfs_node_t *dir;
	int r = vfs_resolve(path, &vfs_root_cred, &dir);
	if (r != VFS_OK)
		return r;

	if (!VFS_S_ISDIR(dir->mode)) {
		vfs_node_release(dir);
		return VFS_ERR_NOTDIR;
	}

	size_t pos = 0;
	size_t file_count = 0;
	size_t dir_count = 0;
	unsigned long long total_size = 0;

	append(
		body, cap, &pos,
		"<!doctype html><html><head><meta charset=\"utf-8\">"
		"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
		"<title>lyrOS files</title>");
	append_css(body, cap, &pos);
	append(
		body, cap, &pos,
		"</head><body><main><section class=\"term\">"
		"<div class=\"bar\"><span class=\"dot\"></span><span>lyrOS file browser</span></div>"
		"<div class=\"screen\"><h2>Index of ");
	html_escape(body, cap, &pos, path);
	append(
		body, cap, &pos,
		"</h2><div class=\"path\"><a href=\"/\">home</a> / mounted root filesystem</div>");

	append(
		body, cap, &pos,
		"<div class=\"grid\" id=\"stats\">"
		"<div class=\"card\"><span class=\"k\">path</span><span class=\"v\">");
	html_escape(body, cap, &pos, path);
	append(
		body, cap, &pos,
		"</span></div>"
		"<div class=\"card\"><span class=\"k\">source</span><span class=\"v\">vfs</span></div>"
		"<div class=\"card\"><span class=\"k\">mode</span><span class=\"v\">browse</span></div>"
		"</div>");

	append(
		body, cap, &pos,
		"<table><tr><th>Name</th><th>Type</th><th>Action</th><th class=\"size\">Size</th></tr>");

	if (strcmp(path, "/") != 0)
		append(
			body, cap, &pos,
			"<tr><td><a href=\"/fs/\">..</a></td><td class=\"kind\">dir</td><td></td><td class=\"size\"></td></tr>");

	for (size_t i = 0;; i++) {
		vfs_dirent_t ent;
		if (vfs_readdir(dir, i, &ent) != VFS_OK)
			break;

		char full[512];
		npf_snprintf(full, sizeof(full), "%s%s%s",
					 strcmp(path, "/") == 0 ? "/" : path,
					 strcmp(path, "/") == 0 ? "" : "/", ent.name);

		int is_dir = VFS_S_ISDIR(ent.mode);
		if (is_dir)
			dir_count++;
		else {
			file_count++;
			total_size += (unsigned long long)ent.size;
		}

		append(body, cap, &pos, "<tr><td><a href=\"");
		append(body, cap, &pos, is_dir ? "/fs" : "/raw");
		html_escape(body, cap, &pos, full);
		append(body, cap, &pos, "\">");
		html_escape(body, cap, &pos, ent.name);
		append(body, cap, &pos, "</a></td><td class=\"kind\">");
		append(body, cap, &pos, is_dir ? "dir" : "file");
		append(body, cap, &pos, "</td><td>");

		if (is_dir) {
			append(body, cap, &pos, "<a class=\"mini\" href=\"/fs");
			html_escape(body, cap, &pos, full);
			append(body, cap, &pos, "\">Open</a>");
		} else {
			append(body, cap, &pos, "<a class=\"mini ghost\" href=\"#preview");
			html_escape(body, cap, &pos, full);
			append(body, cap, &pos, "\">Preview</a>");
			append(body, cap, &pos, "<a class=\"mini\" href=\"/raw");
			html_escape(body, cap, &pos, full);
			append(body, cap, &pos, "\">Download</a>");
		}

		appendf(body, cap, &pos, "</td><td class=\"size\">%llu</td></tr>",
				(unsigned long long)ent.size);

		if (!is_dir) {
			append(body, cap, &pos, "<div class=\"modal\" id=\"preview");
			html_escape(body, cap, &pos, full);
			append(
				body, cap, &pos,
				"\"><div class=\"modalbox\"><div class=\"modalbar\"><span>Preview: ");
			html_escape(body, cap, &pos, full);
			append(
				body, cap, &pos,
				"</span><a class=\"close\" href=\"#\">close</a></div><iframe src=\"/raw");
			html_escape(body, cap, &pos, full);
			append(body, cap, &pos, "\"></iframe></div></div>");
		}
	}

	appendf(
		body, cap, &pos,
		"</table><p class=\"path\">%zu directories, %zu files, %llu bytes listed. "
		"Preview is best for text-like files; binary files should be downloaded.</p>",
		dir_count, file_count, total_size);

	append(body, cap, &pos, "</div></section></main></body></html>");
	vfs_node_release(dir);

	*done = pos;
	return VFS_OK;
}

/* ---------------- file preview / download ---------------- */

static int handle_raw(const char *path, void *response, size_t cap,
					  size_t *out_len)
{
	vfs_file_t *f;
	size_t read = 0;
	char filebuf[8192];

	if (vfs_open(path, VFS_O_RDONLY, 0, &vfs_root_cred, &f) != VFS_OK)
		return VFS_ERR_NOENT;

	vfs_read(f, filebuf, sizeof(filebuf), &read);
	vfs_close(f);

	char body[16384];
	size_t body_len = 0;

	append(
		body, sizeof(body), &body_len,
		"<!doctype html><html><head><meta charset=\"utf-8\">"
		"<style>"
		":root{color-scheme:dark}"
		"html,body{margin:0;min-height:100%;background:#050907;color:#d6ffe7}"
		"body{font:13px/1.4 ui-monospace,SFMono-Regular,Menlo,monospace;padding:16px}"
		"pre{margin:0;white-space:pre-wrap;word-break:break-word}"
		"</style></head><body><pre>");

	html_escape_n(body, sizeof(body), &body_len, filebuf, read);

	append(body, sizeof(body), &body_len, "</pre></body></html>");

	int n = npf_snprintf(response, cap,
						 "HTTP/1.0 200 OK\r\n"
						 "Content-Type: text/html; charset=utf-8\r\n"
						 "Content-Length: %zu\r\n"
						 "Connection: close\r\n\r\n",
						 body_len);

	if (n < 0 || (size_t)n + body_len >= cap)
		return VFS_ERR_INVAL;

	memcpy((char *)response + n, body, body_len);
	*out_len = (size_t)n + body_len;
	return VFS_OK;
}

/* ---------------- handler ---------------- */

static int websrv_handle(netdev_t *dev, uint32_t rip, uint16_t rport,
						 const void *request, size_t request_len,
						 void *response, size_t cap, size_t *response_len,
						 void *ctx)
{
	(void)dev;
	(void)rip;
	(void)rport;
	(void)ctx;

	char url[256] = "/";
	if (request_len > 4)
		url_path_decode((const char *)request + 4, url, sizeof(url));

	char body[16384];
	size_t body_len = 0;
	int r = VFS_OK;

	if (!strcmp(url, "/")) {
		r = build_home(body, sizeof(body), &body_len);
	} else if (!strncmp(url, "/fs", 3)) {
		const char *p = url + 3;
		if (!*p)
			p = "/";
		r = build_listing(p, body, sizeof(body), &body_len);
	} else if (!strncmp(url, "/raw", 4)) {
		const char *p = url + 4;
		if (!*p)
			p = "/";
		return handle_raw(p, response, cap, response_len);
	} else {
		r = build_home(body, sizeof(body), &body_len);
	}

	if (r != VFS_OK) {
		body_len = 0;
		append(
			body, sizeof(body), &body_len,
			"<!doctype html><html><body><h1>404</h1><p>Not found.</p><p><a href=\"/fs/\">Back to files</a></p></body></html>");
	}

	int n = npf_snprintf(response, cap,
						 "HTTP/1.0 200 OK\r\n"
						 "Content-Type: text/html; charset=utf-8\r\n"
						 "Content-Length: %zu\r\n"
						 "Connection: close\r\n\r\n",
						 body_len);
	if (n < 0 || (size_t)n + body_len >= cap)
		return VFS_ERR_INVAL;

	memcpy((char *)response + n, body, body_len);
	*response_len = (size_t)n + body_len;
	return VFS_OK;
}

/* ---------------- thread + init ---------------- */

static void websrv_thread(void *arg)
{
	(void)arg;
	for (;;) {
		net_poll_all();
		__asm__ volatile("pause");
	}
}

static int websrv_main(driver_t *driver)
{
	int r = net_tcp_listen(80, websrv_handle, driver);
	if (r != VFS_OK)
		return r;

	r = driver_spawn_thread(driver, "websrv", websrv_thread, driver);
	if (r != VFS_OK)
		return r;

	driver_log(driver, "info", "websrv online");
	return VFS_OK;
}

static const char *const websrv_imports[] = {
	"driver_log",		"driver_spawn_thread",
	"memcpy",			"memmove",
	"net_default_dev",	"net_ipv4_format",
	"netdev_count",		"net_poll_all",
	"net_tcp_listen",	"npf_snprintf_",
	"npf_vsnprintf",	"strlen",
	"strcmp",			"strncmp",
	"strstr",			"vfs_open",
	"vfs_read",			"vfs_close",
	"vfs_readdir",		"vfs_resolve",
	"vfs_node_release", "vfs_root_cred"
};

const driver_metadata_t lyr_driver_metadata = {
	.magic = DRIVER_MAGIC,
	.abi_version = DRIVER_ABI_VERSION,
	.name = "websrv",
	.entry = websrv_main,
	.imports = websrv_imports,
	.import_count = sizeof(websrv_imports) / sizeof(websrv_imports[0]),
};