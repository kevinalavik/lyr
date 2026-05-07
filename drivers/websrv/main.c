#include <drv/driver.h>
#include <fs/vfs.h>
#include <lib/nanoprintf.h>
#include <lib/string.h>
#include <mm/heap.h>
#include <net/net.h>

#define WEB_BODY_INIT 16384
#define WEB_BODY_LIMIT (256 * 1024)
#define WEB_FILE_LIMIT (128 * 1024)
#define WEB_FORM_LIMIT (160 * 1024)

typedef struct htmlbuf {
	char *data;
	size_t len;
	size_t cap;
} htmlbuf_t;

static unsigned long long web_stat_requests;
static unsigned long long web_stat_gets;
static unsigned long long web_stat_posts;
static unsigned long long web_stat_saves;
static unsigned long long web_stat_home;
static unsigned long long web_stat_listings;
static unsigned long long web_stat_raw;
static unsigned long long web_stat_edits;
static unsigned long long web_stat_stats;
static unsigned long long web_stat_errors;
static unsigned long long web_stat_bytes_sent;

static int hb_init(htmlbuf_t *b, size_t cap)
{
	b->data = kzalloc(cap);
	if (!b->data)
		return VFS_ERR_NOMEM;

	b->len = 0;
	b->cap = cap;
	b->data[0] = 0;
	return VFS_OK;
}

static void hb_free(htmlbuf_t *b)
{
	if (b->data)
		kfree(b->data);

	b->data = NULL;
	b->len = 0;
	b->cap = 0;
}

static int hb_grow(htmlbuf_t *b, size_t need)
{
	if (need <= b->cap)
		return VFS_OK;

	size_t ncap = b->cap ? b->cap : WEB_BODY_INIT;

	while (ncap < need) {
		if (ncap >= WEB_BODY_LIMIT)
			return VFS_ERR_INVAL;
		ncap *= 2;
		if (ncap > WEB_BODY_LIMIT)
			ncap = WEB_BODY_LIMIT;
	}

	char *n = krealloc(b->data, ncap);
	if (!n)
		return VFS_ERR_NOMEM;

	b->data = n;
	b->cap = ncap;
	return VFS_OK;
}

static int hb_append_n(htmlbuf_t *b, const char *s, size_t len)
{
	int r = hb_grow(b, b->len + len + 1);
	if (r != VFS_OK)
		return r;

	memcpy(b->data + b->len, s, len);
	b->len += len;
	b->data[b->len] = 0;
	return VFS_OK;
}

static int hb_append(htmlbuf_t *b, const char *s)
{
	return hb_append_n(b, s, strlen(s));
}

static int hb_appendf(htmlbuf_t *b, const char *fmt, ...)
{
	char tmp[512];

	va_list ap;
	va_start(ap, fmt);
	int n = npf_vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);

	if (n < 0 || (size_t)n >= sizeof(tmp))
		return VFS_ERR_INVAL;

	return hb_append_n(b, tmp, (size_t)n);
}

static int hb_html_escape_n(htmlbuf_t *b, const char *s, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		switch (s[i]) {
		case '&':
			if (hb_append(b, "&amp;"))
				return VFS_ERR_INVAL;
			break;
		case '<':
			if (hb_append(b, "&lt;"))
				return VFS_ERR_INVAL;
			break;
		case '>':
			if (hb_append(b, "&gt;"))
				return VFS_ERR_INVAL;
			break;
		case '"':
			if (hb_append(b, "&quot;"))
				return VFS_ERR_INVAL;
			break;
		default:
			if (hb_append_n(b, &s[i], 1))
				return VFS_ERR_INVAL;
			break;
		}
	}

	return VFS_OK;
}

static int hb_html_escape(htmlbuf_t *b, const char *s)
{
	return hb_html_escape_n(b, s, strlen(s));
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int url_decode_component(const char *src, size_t len, char *dst,
								size_t cap, int plus)
{
	size_t pos = 0;

	for (size_t i = 0; i < len && pos + 1 < cap;) {
		if (src[i] == '%' && i + 2 < len) {
			int a = hexval(src[i + 1]);
			int b = hexval(src[i + 2]);

			if (a < 0 || b < 0)
				return VFS_ERR_INVAL;

			dst[pos++] = (char)((a << 4) | b);
			i += 3;
		} else if (src[i] == '+' && plus) {
			dst[pos++] = ' ';
			i++;
		} else {
			dst[pos++] = src[i++];
		}
	}

	dst[pos] = 0;

	if (pos + 1 == cap && len)
		return VFS_ERR_INVAL;

	return VFS_OK;
}

static int url_path_decode(const char *src, char *dst, size_t cap)
{
	size_t len = 0;

	while (src[len] && src[len] != ' ' && src[len] != '?')
		len++;

	if (url_decode_component(src, len, dst, cap, 0) != VFS_OK)
		return VFS_ERR_INVAL;

	if (!dst[0] || strstr(dst, ".."))
		return VFS_ERR_INVAL;

	return VFS_OK;
}

static const char *find_http_body(const char *req, size_t len, size_t *out_len)
{
	for (size_t i = 0; i + 3 < len; i++) {
		if (req[i] == '\r' && req[i + 1] == '\n' && req[i + 2] == '\r' &&
			req[i + 3] == '\n') {
			*out_len = len - (i + 4);
			return req + i + 4;
		}
	}

	*out_len = 0;
	return NULL;
}

static int parse_content_length(const char *req, size_t len, size_t *out)
{
	const char *key = "Content-Length:";
	size_t key_len = strlen(key);

	for (size_t i = 0; i + key_len < len; i++) {
		if (!strncmp(req + i, key, key_len)) {
			i += key_len;

			while (i < len && (req[i] == ' ' || req[i] == '\t'))
				i++;

			size_t n = 0;
			int any = 0;

			while (i < len && req[i] >= '0' && req[i] <= '9') {
				any = 1;
				n = n * 10 + (size_t)(req[i] - '0');
				i++;
			}

			if (!any)
				return VFS_ERR_INVAL;

			*out = n;
			return VFS_OK;
		}
	}

	return VFS_ERR_INVAL;
}

static int path_parent(const char *path, char *out, size_t cap)
{
	size_t len = strlen(path);

	if (len <= 1) {
		if (cap < 2)
			return VFS_ERR_INVAL;
		out[0] = '/';
		out[1] = 0;
		return VFS_OK;
	}

	size_t end = len;

	while (end > 1 && path[end - 1] == '/')
		end--;

	size_t slash = end;

	while (slash > 1 && path[slash - 1] != '/')
		slash--;

	if (slash <= 1) {
		if (cap < 2)
			return VFS_ERR_INVAL;
		out[0] = '/';
		out[1] = 0;
		return VFS_OK;
	}

	if (slash >= cap)
		return VFS_ERR_INVAL;

	memcpy(out, path, slash - 1);
	out[slash - 1] = 0;
	return VFS_OK;
}

static int join_path(const char *dir, const char *name, char *out, size_t cap)
{
	int n;

	if (!strcmp(dir, "/"))
		n = npf_snprintf(out, cap, "/%s", name);
	else
		n = npf_snprintf(out, cap, "%s/%s", dir, name);

	if (n < 0 || (size_t)n >= cap)
		return VFS_ERR_INVAL;

	return VFS_OK;
}

static int read_file_limited(const char *path, char **out, size_t *out_len,
							 int *truncated)
{
	vfs_file_t *f;
	int r = vfs_open(path, VFS_O_RDONLY, 0, &vfs_root_cred, &f);
	if (r != VFS_OK)
		return r;

	size_t cap = 4096;
	char *buf = kzalloc(cap + 1);
	if (!buf) {
		vfs_close(f);
		return VFS_ERR_NOMEM;
	}

	size_t total = 0;
	*truncated = 0;

	for (;;) {
		if (total == WEB_FILE_LIMIT) {
			*truncated = 1;
			break;
		}

		if (total == cap) {
			size_t ncap = cap * 2;

			if (ncap > WEB_FILE_LIMIT)
				ncap = WEB_FILE_LIMIT;

			char *n = krealloc(buf, ncap + 1);
			if (!n) {
				kfree(buf);
				vfs_close(f);
				return VFS_ERR_NOMEM;
			}

			buf = n;
			cap = ncap;
		}

		size_t got = 0;
		r = vfs_read(f, buf + total, cap - total, &got);
		if (r != VFS_OK) {
			kfree(buf);
			vfs_close(f);
			return r;
		}

		if (!got)
			break;

		total += got;
	}

	vfs_close(f);

	buf[total] = 0;
	*out = buf;
	*out_len = total;
	return VFS_OK;
}

static int write_file_all(const char *path, const char *data, size_t len)
{
	vfs_file_t *f;
	int r = vfs_open(path, VFS_O_WRONLY | VFS_O_TRUNC, 0, &vfs_root_cred, &f);
	if (r != VFS_OK)
		return r;

	size_t off = 0;

	while (off < len) {
		size_t written = 0;

		r = vfs_write(f, data + off, len - off, &written);
		if (r != VFS_OK) {
			vfs_close(f);
			return r;
		}

		if (!written) {
			vfs_close(f);
			return VFS_ERR_INVAL;
		}

		off += written;
	}

	vfs_close(f);
	return VFS_OK;
}

static int build_header(htmlbuf_t *b, const char *title)
{
	hb_append(b, "<!doctype html><html><head><meta charset=\"utf-8\">");
	hb_append(b, "<title>");
	hb_html_escape(b, title);
	hb_append(b, "</title></head><body>");
	hb_append(b, "<header><fieldset><legend>lyrOS websrv</legend>");
	hb_append(b, "<h1>lyrOS</h1><nav>");
	hb_append(b, "<a href=\"/\">home</a> | ");
	hb_append(b, "<a href=\"/fs/\">files</a> | ");
	hb_append(b, "<a href=\"/stats\">stats</a>");
	hb_append(b, "</nav></fieldset></header>");
	return VFS_OK;
}

static int build_footer(htmlbuf_t *b)
{
	hb_append(b, "<footer><fieldset><legend>footer</legend>");
	hb_append(b, "<p>lyrOS websrv</p>");
	hb_append(b, "</fieldset></footer></body></html>");
	return VFS_OK;
}

static int build_home(htmlbuf_t *b)
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

	build_header(b, "lyrOS");

	hb_append(b, "<main>");

	hb_append(b, "<fieldset><legend>banner</legend><pre>");
	hb_append(b, " _             ___  ____\n");
	hb_append(b, "| |_   _ _ __ / _ \\/ ___|\n");
	hb_append(b, "| | | | | '__| | | \\___ \\\n");
	hb_append(b, "| | |_| | |  | |_| |___) |\n");
	hb_append(b, "|_|\\__, |_|   \\___/|____/\n");
	hb_append(b, "   |___/\n");
	hb_append(b, "</pre></fieldset>");

	hb_append(b, "<fieldset><legend>network</legend><table border=\"1\">");
	hb_append(b, "<tr><th>key</th><th>value</th></tr>");
	hb_appendf(b, "<tr><td>default device</td><td>%s</td></tr>", dev_name);
	hb_appendf(b, "<tr><td>ip address</td><td>%s</td></tr>", ip);
	hb_appendf(b, "<tr><td>gateway</td><td>%s</td></tr>", gw);
	hb_appendf(b, "<tr><td>link</td><td>%s</td></tr>", link);
	hb_appendf(b, "<tr><td>network devices</td><td>%zu</td></tr>", devs);
	hb_append(b, "</table></fieldset>");

	hb_append(b, "<fieldset><legend>links</legend>");
	hb_append(b, "<p><a href=\"/fs/\">browse filesystem</a></p>");
	hb_append(b, "<p><a href=\"/stats\">server stats</a></p>");
	hb_append(b, "</fieldset>");

	hb_append(b, "</main>");

	build_footer(b);
	return VFS_OK;
}

static int build_listing(const char *path, htmlbuf_t *b)
{
	vfs_node_t *dir;
	int r = vfs_resolve(path, &vfs_root_cred, &dir);
	if (r != VFS_OK)
		return r;

	if (!VFS_S_ISDIR(dir->mode)) {
		vfs_node_release(dir);
		return VFS_ERR_NOTDIR;
	}

	size_t files = 0;
	size_t dirs = 0;
	size_t entries = 0;
	unsigned long long bytes = 0;

	build_header(b, "files");

	hb_append(b, "<main>");
	hb_append(b, "<fieldset><legend>directory</legend><h2>Index of ");
	hb_html_escape(b, path);
	hb_append(b, "</h2><p>");
	hb_append(b, "<a href=\"/fs/\">root</a>");

	if (strcmp(path, "/")) {
		char parent[512];
		if (path_parent(path, parent, sizeof(parent)) == VFS_OK) {
			hb_append(b, " | <a href=\"/fs");
			hb_html_escape(b, parent);
			hb_append(b, "\">parent</a>");
		}
	}

	hb_append(b, "</p></fieldset>");

	hb_append(b, "<fieldset><legend>entries</legend><table border=\"1\">");
	hb_append(
		b, "<tr><th>name</th><th>kind</th><th>size</th><th>actions</th></tr>");

	for (size_t i = 0;; i++) {
		vfs_dirent_t ent;
		if (vfs_readdir(dir, i, &ent) != VFS_OK)
			break;

		char full[512];
		if (join_path(path, ent.name, full, sizeof(full)) != VFS_OK)
			continue;

		int is_dir = VFS_S_ISDIR(ent.mode);

		entries++;

		if (is_dir) {
			dirs++;
		} else {
			files++;
			bytes += (unsigned long long)ent.size;
		}

		hb_append(b, "<tr><td>");
		hb_html_escape(b, ent.name);
		hb_append(b, "</td><td>");
		hb_append(b, is_dir ? "directory" : "file");
		hb_append(b, "</td><td>");
		hb_appendf(b, "%llu", (unsigned long long)ent.size);
		hb_append(b, "</td><td>");

		if (is_dir) {
			hb_append(b, "<a href=\"/fs");
			hb_html_escape(b, full);
			hb_append(b, "\">open</a>");
		} else {
			hb_append(b, "<a href=\"/raw");
			hb_html_escape(b, full);
			hb_append(b, "\">view</a> ");
			hb_append(b, "<a href=\"/edit");
			hb_html_escape(b, full);
			hb_append(b, "\">edit</a>");
		}

		hb_append(b, "</td></tr>");
	}

	hb_append(b, "</table></fieldset>");

	hb_append(b,
			  "<fieldset><legend>directory stats</legend><table border=\"1\">");
	hb_append(b, "<tr><th>metric</th><th>value</th></tr>");
	hb_appendf(b, "<tr><td>entries</td><td>%zu</td></tr>", entries);
	hb_appendf(b, "<tr><td>directories</td><td>%zu</td></tr>", dirs);
	hb_appendf(b, "<tr><td>files</td><td>%zu</td></tr>", files);
	hb_appendf(b, "<tr><td>listed file bytes</td><td>%llu</td></tr>", bytes);
	hb_appendf(b, "<tr><td>editor read limit</td><td>%u</td></tr>",
			   WEB_FILE_LIMIT);
	hb_appendf(b, "<tr><td>form limit</td><td>%u</td></tr>", WEB_FORM_LIMIT);
	hb_append(b, "</table>");
	hb_append(
		b,
		"<p>The editor refuses to save files that were truncated while loading.</p>");
	hb_append(b, "</fieldset>");

	hb_append(b, "</main>");

	vfs_node_release(dir);
	build_footer(b);
	return VFS_OK;
}

static int build_raw(const char *path, htmlbuf_t *b)
{
	char *buf = NULL;
	size_t len = 0;
	int truncated = 0;

	int r = read_file_limited(path, &buf, &len, &truncated);
	if (r != VFS_OK)
		return r;

	build_header(b, "view");

	hb_append(b, "<main><fieldset><legend>file</legend><h2>View ");
	hb_html_escape(b, path);
	hb_append(b, "</h2>");

	if (truncated)
		hb_append(b, "<p>Warning: file output was truncated.</p>");

	hb_append(b, "<pre>");
	hb_html_escape_n(b, buf, len);
	hb_append(b, "</pre></fieldset></main>");

	kfree(buf);
	build_footer(b);
	return VFS_OK;
}

static int build_edit(const char *path, htmlbuf_t *b)
{
	char *buf = NULL;
	size_t len = 0;
	int truncated = 0;

	int r = read_file_limited(path, &buf, &len, &truncated);
	if (r != VFS_OK)
		return r;

	build_header(b, "edit");

	hb_append(b, "<main><fieldset><legend>editor</legend><h2>Edit ");
	hb_html_escape(b, path);
	hb_append(b, "</h2>");

	hb_append(b, "<p>Editor read limit: ");
	hb_appendf(b, "%u", WEB_FILE_LIMIT);
	hb_append(b, " bytes.</p>");

	if (truncated) {
		hb_append(
			b,
			"<p>Warning: this file was truncated while loading. Saving is disabled.</p>");
		hb_append(b, "<pre>");
		hb_html_escape_n(b, buf, len);
		hb_append(b, "</pre></fieldset></main>");
		kfree(buf);
		build_footer(b);
		return VFS_OK;
	}

	hb_append(b, "<form method=\"POST\" action=\"/save\">");
	hb_append(b, "<input type=\"hidden\" name=\"path\" value=\"");
	hb_html_escape(b, path);
	hb_append(b, "\">");
	hb_append(b, "<p><textarea name=\"content\" rows=\"30\" cols=\"100\">");
	hb_html_escape_n(b, buf, len);
	hb_append(b, "</textarea></p>");
	hb_append(b, "<p><button type=\"submit\">save</button></p>");
	hb_append(b, "</form></fieldset></main>");

	kfree(buf);
	build_footer(b);
	return VFS_OK;
}

static int extract_field(const char *body, size_t len, const char *name,
						 char *out, size_t cap)
{
	size_t name_len = strlen(name);

	for (size_t i = 0; i < len;) {
		size_t key_start = i;

		while (i < len && body[i] != '=' && body[i] != '&')
			i++;

		if (i >= len || body[i] != '=')
			break;

		size_t key_len = i - key_start;
		size_t val_start = i + 1;
		size_t val_end = val_start;

		while (val_end < len && body[val_end] != '&')
			val_end++;

		if (key_len == name_len && !strncmp(body + key_start, name, name_len)) {
			return url_decode_component(body + val_start, val_end - val_start,
										out, cap, 1);
		}

		i = val_end + 1;
	}

	return VFS_ERR_INVAL;
}

static int handle_save(const void *req, size_t len, void *resp, size_t cap,
					   size_t *out_len)
{
	size_t declared_len = 0;
	int r = parse_content_length((const char *)req, len, &declared_len);
	if (r != VFS_OK)
		return r;

	if (declared_len > WEB_FORM_LIMIT)
		return VFS_ERR_INVAL;

	size_t body_len = 0;
	const char *body = find_http_body((const char *)req, len, &body_len);
	if (!body)
		return VFS_ERR_INVAL;

	if (body_len != declared_len)
		return VFS_ERR_INVAL;

	char path[512];

	r = extract_field(body, body_len, "path", path, sizeof(path));
	if (r != VFS_OK)
		return r;

	if (!path[0] || strstr(path, ".."))
		return VFS_ERR_INVAL;

	char *old = NULL;
	size_t old_len = 0;
	int old_truncated = 0;

	r = read_file_limited(path, &old, &old_len, &old_truncated);
	if (r != VFS_OK)
		return r;

	kfree(old);

	if (old_truncated)
		return VFS_ERR_INVAL;

	char *content = kzalloc(WEB_FORM_LIMIT + 1);
	if (!content)
		return VFS_ERR_NOMEM;

	r = extract_field(body, body_len, "content", content, WEB_FORM_LIMIT + 1);
	if (r != VFS_OK) {
		kfree(content);
		return r;
	}

	r = write_file_all(path, content, strlen(content));
	kfree(content);

	if (r != VFS_OK)
		return r;

	web_stat_saves++;

	int n = npf_snprintf(resp, cap,
						 "HTTP/1.0 303 See Other\r\n"
						 "Location: /edit%s\r\n"
						 "Content-Length: 0\r\n"
						 "Connection: close\r\n\r\n",
						 path);

	if (n < 0 || (size_t)n >= cap)
		return VFS_ERR_INVAL;

	*out_len = (size_t)n;
	return VFS_OK;
}

static int build_stats(htmlbuf_t *b)
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

	build_header(b, "stats");

	hb_append(b, "<main>");

	hb_append(
		b,
		"<fieldset><legend>network device summary</legend><table border=\"1\">");
	hb_append(b, "<tr><th>metric</th><th>value</th></tr>");
	hb_appendf(b, "<tr><td>network devices</td><td>%zu</td></tr>", devs);
	hb_appendf(b, "<tr><td>default device</td><td>%s</td></tr>", dev_name);
	hb_appendf(b, "<tr><td>default link</td><td>%s</td></tr>", link);
	hb_appendf(b, "<tr><td>default ipv4</td><td>%s</td></tr>", ip);
	hb_appendf(b, "<tr><td>default gateway</td><td>%s</td></tr>", gw);
	hb_append(b, "</table></fieldset>");

	hb_append(b,
			  "<fieldset><legend>http counters</legend><table border=\"1\">");
	hb_append(b, "<tr><th>metric</th><th>value</th></tr>");
	hb_appendf(b, "<tr><td>requests</td><td>%llu</td></tr>", web_stat_requests);
	hb_appendf(b, "<tr><td>GET requests</td><td>%llu</td></tr>", web_stat_gets);
	hb_appendf(b, "<tr><td>POST requests</td><td>%llu</td></tr>",
			   web_stat_posts);
	hb_appendf(b, "<tr><td>saves</td><td>%llu</td></tr>", web_stat_saves);
	hb_appendf(b, "<tr><td>home views</td><td>%llu</td></tr>", web_stat_home);
	hb_appendf(b, "<tr><td>directory listings</td><td>%llu</td></tr>",
			   web_stat_listings);
	hb_appendf(b, "<tr><td>raw file views</td><td>%llu</td></tr>",
			   web_stat_raw);
	hb_appendf(b, "<tr><td>edit views</td><td>%llu</td></tr>", web_stat_edits);
	hb_appendf(b, "<tr><td>stats views</td><td>%llu</td></tr>", web_stat_stats);
	hb_appendf(b, "<tr><td>errors</td><td>%llu</td></tr>", web_stat_errors);
	hb_appendf(b, "<tr><td>bytes sent</td><td>%llu</td></tr>",
			   web_stat_bytes_sent);
	hb_append(b, "</table></fieldset>");

	hb_append(b, "<fieldset><legend>limits</legend><table border=\"1\">");
	hb_append(b, "<tr><th>limit</th><th>bytes</th></tr>");
	hb_appendf(b, "<tr><td>initial body buffer</td><td>%u</td></tr>",
			   WEB_BODY_INIT);
	hb_appendf(b, "<tr><td>maximum body buffer</td><td>%u</td></tr>",
			   WEB_BODY_LIMIT);
	hb_appendf(b, "<tr><td>file read limit</td><td>%u</td></tr>",
			   WEB_FILE_LIMIT);
	hb_appendf(b, "<tr><td>form limit</td><td>%u</td></tr>", WEB_FORM_LIMIT);
	hb_append(b, "</table></fieldset>");

	hb_append(b, "</main>");

	build_footer(b);
	return VFS_OK;
}

static int build_error(htmlbuf_t *b, const char *title, const char *msg)
{
	build_header(b, title);
	hb_append(b, "<main><fieldset><legend>error</legend><h2>");
	hb_html_escape(b, title);
	hb_append(b, "</h2><p>");
	hb_html_escape(b, msg);
	hb_append(b, "</p><p><a href=\"/fs/\">files</a></p></fieldset></main>");
	build_footer(b);
	return VFS_OK;
}

static int send_html(htmlbuf_t *b, void *resp, size_t cap, size_t *out)
{
	int n = npf_snprintf(resp, cap,
						 "HTTP/1.0 200 OK\r\n"
						 "Content-Type: text/html; charset=utf-8\r\n"
						 "Content-Length: %zu\r\n"
						 "Connection: close\r\n\r\n",
						 b->len);

	if (n < 0 || (size_t)n + b->len >= cap)
		return VFS_ERR_INVAL;

	memcpy((char *)resp + n, b->data, b->len);
	*out = (size_t)n + b->len;
	web_stat_bytes_sent += *out;
	return VFS_OK;
}

static int send_raw(const void *data, size_t len, const char *content_type,
					void *resp, size_t cap, size_t *out)
{
	if (!content_type)
		content_type = "application/octet-stream";

	int n = npf_snprintf(resp, cap,
						 "HTTP/1.0 200 OK\r\n"
						 "Content-Type: %s\r\n"
						 "Content-Length: %zu\r\n"
						 "Connection: close\r\n\r\n",
						 content_type, len);

	if (n < 0 || (size_t)n + len >= cap)
		return VFS_ERR_INVAL;

	memcpy((char *)resp + n, data, len);

	*out = (size_t)n + len;
	web_stat_bytes_sent += *out;

	return VFS_OK;
}

static int websrv_handle(netdev_t *dev, uint32_t ip, uint16_t port,
						 const void *req, size_t len, void *resp, size_t cap,
						 size_t *out, void *ctx)
{
	(void)dev;
	(void)ip;
	(void)port;
	(void)ctx;

	char url[256] = "/";

	int is_get = len >= 4 && !strncmp((const char *)req, "GET ", 4);
	int is_post = len >= 5 && !strncmp((const char *)req, "POST ", 5);

	web_stat_requests++;

	if (is_get)
		web_stat_gets++;
	else if (is_post)
		web_stat_posts++;

	if (is_get) {
		if (url_path_decode((const char *)req + 4, url, sizeof(url)) !=
			VFS_OK) {
			url[0] = '/';
			url[1] = 0;
		}
	} else if (is_post) {
		if (url_path_decode((const char *)req + 5, url, sizeof(url)) !=
			VFS_OK) {
			url[0] = '/';
			url[1] = 0;
		}
	}

	if (is_post && !strncmp(url, "/save", 5))
		return handle_save(req, len, resp, cap, out);

	htmlbuf_t b;
	int r = hb_init(&b, WEB_BODY_INIT);
	if (r != VFS_OK)
		return r;

	if (!strcmp(url, "/")) {
		web_stat_home++;
		r = build_home(&b);
	} else if (!strcmp(url, "/stats")) {
		web_stat_stats++;
		r = build_stats(&b);
	} else if (!strncmp(url, "/fs", 3)) {
		web_stat_listings++;
		const char *path = url + 3;
		if (!*path)
			path = "/";
		r = build_listing(path, &b);
	} else if (!strncmp(url, "/raw", 4)) {
		web_stat_raw++;
		const char *path = url + 4;
		if (!*path)
			path = "/";
		r = build_raw(path, &b);
	} else if (!strncmp(url, "/edit", 5)) {
		web_stat_edits++;
		const char *path = url + 5;
		if (!*path)
			path = "/";
		r = build_edit(path, &b);
	} else if (!strncmp(url, "/alive", 6)) {
		web_stat_raw++;
		hb_free(&b);

		static const char alive_msg[] = "yes im alive\n";

		return send_raw(alive_msg, sizeof(alive_msg) - 1,
						"text/plain; charset=utf-8", resp, cap, out);
	} else {
		r = VFS_ERR_NOENT;
	}

	if (r != VFS_OK) {
		web_stat_errors++;
		b.len = 0;
		if (b.data)
			b.data[0] = 0;
		build_error(&b, "error", "request failed");
	}

	r = send_html(&b, resp, cap, out);
	hb_free(&b);
	return r;
}

static void websrv_thread(void *arg)
{
	(void)arg;

	for (;;) {
		net_poll_all();
		__asm__ volatile("pause");
	}
}

static int websrv_main(driver_t *d)
{
	int r = net_tcp_listen(6969, websrv_handle, d);
	if (r != VFS_OK)
		return r;

	r = driver_spawn_thread(d, "websrv", websrv_thread, d);
	if (r != VFS_OK)
		return r;

	driver_log(d, "info", "websrv online");
	return VFS_OK;
}

static const char *const websrv_imports[] = {
	"driver_log",	   "driver_spawn_thread",
	"kzalloc",		   "krealloc",
	"kfree",		   "memcpy",
	"strlen",		   "strcmp",
	"strncmp",		   "strstr",
	"net_default_dev", "net_ipv4_format",
	"netdev_count",	   "net_poll_all",
	"net_tcp_listen",  "npf_snprintf_",
	"npf_vsnprintf",   "vfs_open",
	"vfs_read",		   "vfs_write",
	"vfs_close",	   "vfs_readdir",
	"vfs_resolve",	   "vfs_node_release",
	"vfs_root_cred"
};

const driver_metadata_t lyr_driver_metadata = {
	.magic = DRIVER_MAGIC,
	.abi_version = DRIVER_ABI_VERSION,
	.name = "websrv",
	.entry = websrv_main,
	.imports = websrv_imports,
	.import_count = sizeof(websrv_imports) / sizeof(websrv_imports[0]),
};