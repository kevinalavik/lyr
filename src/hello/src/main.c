#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <lyr/pci.h>

static int read_exact(FILE *f, void *buf, size_t len)
{
	unsigned char *p = buf;
	size_t done = 0;

	while (done < len) {
		size_t n = fread(p + done, 1, len - done, f);

		if (n == 0) {
			if (ferror(f))
				return -1;
			break;
		}

		done += n;
	}

	return done == len ? 1 : 0;
}

int main(void)
{
	FILE *f = fopen(LYR_PCI_MAP_PATH, "rb");
	if (!f) {
		fprintf(stderr, "failed to open %s: %s\n", LYR_PCI_MAP_PATH,
				strerror(errno));
		return 1;
	}

	printf("%-8s %-9s %-8s %-7s %-5s %-32s %-32s\n", "PCI-ADDR", "ID", "CLASS",
		   "PROG-IF", "BOUND", "DEVICE", "DRIVER");

	printf("%-8s %-9s %-8s %-7s %-5s %-32s %-32s\n", "--------", "---------",
		   "--------", "-------", "-----", "--------------------------------",
		   "--------------------------------");

	for (;;) {
		lyr_pcimap_t ent;
		int r = read_exact(f, &ent, sizeof(ent));

		if (r < 0) {
			fprintf(stderr, "failed to read %s: %s\n", LYR_PCI_MAP_PATH,
					strerror(errno));
			fclose(f);
			return 1;
		}

		if (r == 0)
			break;

		if (ent.abi_version != LYR_PCI_MAP_ABI_VERSION) {
			fprintf(stderr, "unsupported PCI map ABI version %u, expected %u\n",
					ent.abi_version, LYR_PCI_MAP_ABI_VERSION);
			fclose(f);
			return 1;
		}

		if (ent.entry_size != sizeof(ent)) {
			fprintf(stderr, "unsupported PCI map entry size %u, expected %zu\n",
					ent.entry_size, sizeof(ent));
			fclose(f);
			return 1;
		}

		printf("%02x:%02x.%u  "
			   "%04x:%04x "
			   "%02x:%02x    "
			   "%02x      "
			   "%-5s "
			   "%-32.*s "
			   "%-32.*s\n",
			   ent.bus, ent.slot, ent.function, ent.vendor_id, ent.device_id,
			   ent.class_code, ent.subclass, ent.prog_if,
			   ent.bound ? "yes" : "no", (int)sizeof(ent.device_name),
			   ent.device_name, (int)sizeof(ent.driver_name), ent.driver_name);
	}

	fclose(f);
	return 0;
}