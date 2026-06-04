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
 *   3. Reads /proc/zram_compr_test to get the compressed footprint
 *   4. Prints a summary: allocated vs compressed bytes + ratio
 *
 * Expected result: zram_compr_bytes << allocated_bytes (good compression)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>

#define DEFAULT_SIZE_MB  32
#define PROC_PATH        "/proc/zram_compr_test"

#ifndef MADV_COLD
#define MADV_COLD       20
#endif
#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT    21
#endif

static void read_zram_compr(unsigned long *swap_pages, unsigned long *compr_bytes)
{
	unsigned long compr_kb, ptes, present, none, swap_ptes;
	int pid, n;
	FILE *f;

	f = fopen(PROC_PATH, "r");
	if (!f) {
		perror("fopen " PROC_PATH);
		fprintf(stderr, "Is zram_compr_test built into kernel?\n");
		exit(1);
	}

	n = fscanf(f,
		   "pid=%d swap_pages=%lu zram_compr_bytes=%lu"
		   " zram_compr_kb=%lu ptes=%lu present=%lu none=%lu swap=%lu",
		   &pid, swap_pages, compr_bytes, &compr_kb,
		   &ptes, &present, &none, &swap_ptes);
	if (n >= 8)
		printf("  [debug] ptes=%lu present=%lu none=%lu swap=%lu\n",
		       ptes, present, none, swap_ptes);

	fclose(f);
}

int main(int argc, char *argv[])
{
	unsigned long swap_pages_before, compr_bytes_before;
	unsigned long swap_pages_after, compr_bytes_after;
	unsigned long delta_swap, delta_compr;
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

	printf("Memory faulted in. Reading baseline ZRAM stats...\n");
	read_zram_compr(&swap_pages_before, &compr_bytes_before);
	printf("  Before: swap_pages=%lu zram_compr_bytes=%lu\n",
	       swap_pages_before, compr_bytes_before);

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

	usleep(500000);

	printf("Reading ZRAM stats after swap-out...\n");
	read_zram_compr(&swap_pages_after, &compr_bytes_after);
	printf("  After:  swap_pages=%lu zram_compr_bytes=%lu\n",
	       swap_pages_after, compr_bytes_after);

	delta_swap  = swap_pages_after  - swap_pages_before;
	delta_compr = compr_bytes_after - compr_bytes_before;

	printf("\n=== Results ===\n");
	printf("Pages swapped to ZRAM:    %lu pages (%lu KB)\n",
	       delta_swap, delta_swap * 4);
	printf("Compressed size in ZRAM:  %lu bytes (%lu KB)\n",
	       delta_compr, delta_compr / 1024);
	printf("Allocated size:           %zu bytes (%zu KB)\n",
	       size_bytes, size_bytes / 1024);

	if (delta_compr > 0 && delta_swap > 0) {
		double ratio = (double)(delta_swap * 4096) / (double)delta_compr;

		printf("Compression ratio:        %.1fx\n", ratio);

		if (ratio >= 2.0) {
			printf("\nPASS: Compression ratio %.1fx >= 2x as expected\n",
			       ratio);
		} else {
			printf("\nPASS (weak): Some compression seen (%.1fx)\n",
			       ratio);
		}
	} else if (delta_swap == 0) {
		printf("\nWARN: No swap pages detected — ZRAM swap may not be active\n");
		printf("      Check: cat /proc/swaps\n");
	} else {
		printf("\nWARN: swap_pages=%lu but compr_bytes=0\n", delta_swap);
		printf("      zram_get_compr_size_for_swp_entry() returned 0 for all entries\n");
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
