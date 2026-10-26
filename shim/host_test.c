/* Render the console on the host:
 *   ./out/host_test            -> out/host.ppm (fixed layout)
 *   ./out/host_test FILE.dtb   -> out/probe.ppm (live-FDT bring-up report)
 */
#include <stdio.h>
#include <stdlib.h>
#include "console.h"
#include "probe.h"

static uint32_t host_fb[FB_H * FB_STRIDE / 4];

static void write_ppm(const char *path)
{
	FILE *f = fopen(path, "wb");
	if (f == NULL) {
		perror(path);
		return;
	}
	fprintf(f, "P6 %d %d 255\n", FB_W, FB_H);
	for (int i = 0; i < FB_W * FB_H; i++) {
		unsigned char px[3] = { host_fb[i] >> 16, host_fb[i] >> 8, host_fb[i] };
		fwrite(px, 1, 3, f);
	}
	fclose(f);
	printf("wrote %s (%dx%d)\n", path, FB_W, FB_H);
}

static int render_dtb(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		perror(path);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *buf = malloc(n > 0 ? (size_t)n : 1);
	if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n) {
		fprintf(stderr, "%s: short read\n", path);
		fclose(f);
		return 1;
	}
	fclose(f);
	con_fb = host_fb;
	con_clear(BLUE);
	con_str("SHIM", "sweet milestone 4: xnu loader");
	con_str("BOARD", BOARD_NAME);
	con_str("LOADER", "no SWEETXNU payload");
	con_skip();
	probe_report((uintptr_t)buf);
	write_ppm("out/probe.ppm");
	free(buf);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc > 1) {
		return render_dtb(argv[1]);
	}
	con_fb = host_fb;
	con_clear(RED);
	con_str("SHIM", "sweet milestone 2");
	con_hex("MMFR0", 0x0000000000101122ULL);
	con_hex2("PSCI", "ver=", 0x10001, "smccc=", 0x10002);
	con_hex2("CPU7", "ret=", 0, "mpidr=", 0x81000700);
	con_clear(BLUE);
	con_str("PANIC", "sync exception");
	con_hex("ESR", 0x96000010);
	write_ppm("out/host.ppm");
	return 0;
}
