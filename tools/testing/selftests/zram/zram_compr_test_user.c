// SPDX-License-Identifier: GPL-2.0-only
/*
 * zram_compr_test_user.c - Userspace test for per-process ZRAM compressed size
 *
 * Usage: zram_compr_test_user [SIZE_MB]
 *   SIZE_MB: amount of memory to allocate and swap out (default: 32)
 *
 * What it does:
 *   1. Allocates SIZE_MB of memory filled with compressible data
 *   2. Forces it to swap out via madvise(MADV_PAGEOUT)
 *   3. Reads /proc/self/smaps_rollup to get SwapPss and SwapCompressed
 *   4. Prints a summary: uncompressed vs compressed bytes + ratio
 *
 * Expected result: SwapCompressed << SwapPss (good compression)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>

#define DEFAULT_SIZE_MB  32

#ifndef MADV_COLD
#define MADV_COLD       20
#endif
#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT    21
#endif

int main(int argc, char *argv[])
{
	size_t size_mb = DEFAULT_SIZE_MB;
	size_t size_bytes;
	char *buf;
	int ret;

	if (argc > 1)
		size_mb = (size_t)atoi(argv[1]);
	if (size_mb == 0)
		size_mb = DEFAULT_SIZE_MB;

	size_bytes = size_mb * 1024 * 1024;

	printf("=== ZRAM Per-Process Compressed Size Test ===\n");
	printf("PID: %d\n", getpid());
	printf("Allocating %zu MB (%zu bytes) of compressible data...\n",
	       size_mb, size_bytes);

	buf = mmap(NULL, size_bytes, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (buf == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	/*
	 * Fill with a repeating pattern — highly compressible.
	 * LZO-rle (ZRAM default) will compress this to ~1/100 of original.
	 */
	memset(buf, 0xAB, size_bytes);
	for (size_t i = 0; i < size_bytes; i += 4096)
		buf[i] = (char)(i & 0xFF);

	printf("Marking pages cold via MADV_COLD...\n");
	ret = madvise(buf, size_bytes, MADV_COLD);
	if (ret < 0)
		perror("madvise(MADV_COLD) (non-fatal)");

	printf("Forcing %zu MB to swap out via MADV_PAGEOUT...\n", size_mb);
	ret = madvise(buf, size_bytes, MADV_PAGEOUT);
	if (ret < 0) {
		perror("madvise(MADV_PAGEOUT)");
		fprintf(stderr, "Kernel may not support MADV_PAGEOUT (need 5.4+)\n");
		munmap(buf, size_bytes);
		return 1;
	}

	/*
	 * Poll /proc/self/smaps_rollup until SwapCompressed > 0 or timeout.
	 * MADV_PAGEOUT moves pages to the swap cache first; the kernel writes
	 * them to ZRAM asynchronously.  Allow up to 10 seconds for writeback.
	 */
	printf("Waiting for pages to reach ZRAM (polling smaps_rollup)...\n");
	{
		FILE *f;
		long swap_pss_kb = 0, swap_compressed_kb = 0;
		char line[256];
		int retries = 20; /* 20 × 500 ms = 10 s max */

		do {
			usleep(500000);
			swap_pss_kb = 0;
			swap_compressed_kb = 0;

			f = fopen("/proc/self/smaps_rollup", "r");
			if (!f) {
				perror("fopen /proc/self/smaps_rollup");
				munmap(buf, size_bytes);
				return 1;
			}
			while (fgets(line, sizeof(line), f)) {
				if (sscanf(line, "SwapPss: %ld kB",
					   &swap_pss_kb) == 1)
					continue;
				if (sscanf(line, "SwapCompressed: %ld kB",
					   &swap_compressed_kb) == 1)
					continue;
			}
			fclose(f);
		} while (swap_compressed_kb <= 0 && --retries > 0);

		printf("SwapPss (uncompressed):      %ld kB\n", swap_pss_kb);
		printf("SwapCompressed (compressed): %ld kB\n", swap_compressed_kb);

		if (swap_compressed_kb <= 0) {
			fprintf(stderr,
				"FAIL: SwapCompressed = 0 after 10s (pages not reaching ZRAM)\n");
			munmap(buf, size_bytes);
			return 1;
		}
		if (swap_compressed_kb > swap_pss_kb) {
			fprintf(stderr, "FAIL: SwapCompressed %ld > SwapPss %ld\n",
				swap_compressed_kb, swap_pss_kb);
			munmap(buf, size_bytes);
			return 1;
		}
		{
			double ratio = (double)swap_pss_kb / swap_compressed_kb;

			printf("Compression ratio: %.1fx\n", ratio);
			if (ratio < 1.5) {
				fprintf(stderr,
					"FAIL: compression ratio %.1fx < 1.5x\n",
					ratio);
				munmap(buf, size_bytes);
				return 1;
			}
			printf("PASS: SwapCompressed=%ld kB, ratio=%.1fx\n",
			       swap_compressed_kb, ratio);
		}
	}

	printf("\nVerifying data integrity (reading back swapped pages)...\n");
	{
		unsigned long mismatches = 0;

		for (size_t i = 0; i < size_bytes; i += 4096) {
			if (buf[i] != (char)(i & 0xFF))
				mismatches++;
		}
		if (mismatches == 0)
			printf("Data integrity: PASS (all pages read back correctly)\n");
		else
			printf("Data integrity: FAIL (%lu pages corrupted)\n",
			       mismatches);
	}

	munmap(buf, size_bytes);
	return 0;
}
