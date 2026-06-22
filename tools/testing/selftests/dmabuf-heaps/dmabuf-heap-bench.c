// SPDX-License-Identifier: GPL-2.0
/*
 * dmabuf-heap-bench.c - DMA-buf heap allocation profiling benchmark
 *
 * Measures alloc/mmap/memset/free latency for each heap in /dev/dma_heap/
 * across multiple buffer sizes and heap_flags variants. Optionally measures
 * IOMMU map/unmap latency via vgem DRM device.
 *
 * Uses clock_gettime(CLOCK_MONOTONIC) for nanosecond-resolution timing.
 *
 * Output: TAP with ksft_print_msg() stats lines.
 * Each (heap, size, flags) triple is one kselftest test:
 *   PASS = allocation succeeded + data integrity check passed
 *   FAIL = allocation failed or integrity check failed
 *
 * IOMMU map/unmap (GROUP C) is measured via vgem DRM device, which exercises
 * the same dma_map_sgtable() -> iommu_map() kernel path as real GPU/camera
 * DMA-buf usage. Tests are skipped gracefully if vgem is unavailable.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>

#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <drm/drm.h>
#include "kselftest.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x)	(sizeof(x) / sizeof((x)[0]))
#endif

#define DEVPATH		"/dev/dma_heap"
#define BENCH_REPS	50
#define MAX_HEAPS	32
#define HEAP_NAME_LEN	64

/* Buffer sizes: covers small (order-0 only) through large (multi-hugepage) */
static const struct bench_size {
	size_t bytes;
	const char *label;
} bench_sizes[] = {
	{      4096, "4KB"  },
	{     65536, "64KB" },
	{    262144, "256KB"},
	{   1048576, "1MB"  },
	{   4194304, "4MB"  },
	{  16777216, "16MB" },
};

/* Flag variants: baseline + explicit hugepage hints */
static const struct bench_flags {
	const char *name;
	unsigned int val;
} flag_variants[] = {
	{ "none",       0                         },
	{ "HUGEPAGE",   DMA_HEAP_ALLOC_HUGEPAGE   },
	{ "NOHUGEPAGE", DMA_HEAP_ALLOC_NOHUGEPAGE },
};

struct bench_stats {
	double avg_us;
	double std_dev_us;
	long min_us;
	long max_us;
};

static long time_diff_us(const struct timespec *start,
			 const struct timespec *end)
{
	return (end->tv_sec - start->tv_sec) * 1000000L +
	       (end->tv_nsec - start->tv_nsec) / 1000L;
}

static void compute_stats(const long *samples, int n,
			  struct bench_stats *out)
{
	double sum = 0, sum_sq = 0;
	int i;

	out->min_us = LONG_MAX;
	out->max_us = 0;

	for (i = 0; i < n; i++) {
		sum += samples[i];
		sum_sq += (double)samples[i] * samples[i];
		if (samples[i] < out->min_us)
			out->min_us = samples[i];
		if (samples[i] > out->max_us)
			out->max_us = samples[i];
	}
	out->avg_us = sum / n;
	out->std_dev_us = sqrt(sum_sq / n - out->avg_us * out->avg_us);
}

static int dmabuf_heap_open(const char *name)
{
	char buf[256];
	int ret, fd;

	ret = snprintf(buf, sizeof(buf), "%s/%s", DEVPATH, name);
	if (ret < 0)
		ksft_exit_fail_msg("snprintf failed! %d\n", ret);

	fd = open(buf, O_RDWR);
	if (fd < 0)
		ksft_exit_fail_msg("open %s failed: %s\n", buf,
				   strerror(errno));
	return fd;
}

static int dmabuf_heap_alloc(int fd, size_t len, unsigned int flags,
			     int *dmabuf_fd)
{
	struct dma_heap_allocation_data data = {
		.len = len,
		.fd = 0,
		.fd_flags = O_RDWR | O_CLOEXEC,
		.heap_flags = flags,
	};
	int ret;

	if (!dmabuf_fd)
		return -EINVAL;

	ret = ioctl(fd, DMA_HEAP_IOCTL_ALLOC, &data);
	if (ret < 0)
		return ret;
	*dmabuf_fd = (int)data.fd;
	return ret;
}

static int dmabuf_sync(int fd, int start_stop)
{
	struct dma_buf_sync sync = {
		.flags = start_stop | DMA_BUF_SYNC_RW,
	};

	return ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

static int check_vgem(int fd)
{
	drm_version_t version = { 0 };
	char name[5];
	int ret;

	version.name_len = 4;
	version.name = name;

	ret = ioctl(fd, DRM_IOCTL_VERSION, &version);
	if (ret || version.name_len != 4)
		return 0;

	name[4] = '\0';
	return !strcmp(name, "vgem");
}

static int open_vgem(void)
{
	const char *drmstr = "/dev/dri/card";
	int i, fd;

	for (i = 0; i < 16; i++) {
		char name[80];

		snprintf(name, sizeof(name), "%s%u", drmstr, i);
		fd = open(name, O_RDWR);
		if (fd < 0)
			continue;
		if (!check_vgem(fd)) {
			close(fd);
			continue;
		}
		return fd;
	}
	return -1;
}

static int import_vgem_fd(int vgem_fd, int dma_buf_fd, uint32_t *handle)
{
	struct drm_prime_handle import_handle = {
		.fd = dma_buf_fd,
		.flags = 0,
		.handle = 0,
	};
	int ret;

	ret = ioctl(vgem_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &import_handle);
	if (ret == 0)
		*handle = import_handle.handle;
	return ret;
}

static void close_handle(int vgem_fd, uint32_t handle)
{
	struct drm_gem_close gem_close = {
		.handle = handle,
	};

	ioctl(vgem_fd, DRM_IOCTL_GEM_CLOSE, &gem_close);
}

static int discover_heaps(char heap_names[][HEAP_NAME_LEN], int max_heaps)
{
	struct dirent *dir;
	DIR *d;
	int n = 0;

	d = opendir(DEVPATH);
	if (!d)
		return 0;

	while ((dir = readdir(d)) && n < max_heaps) {
		if (!strncmp(dir->d_name, ".", 2))
			continue;
		if (!strncmp(dir->d_name, "..", 3))
			continue;
		strncpy(heap_names[n], dir->d_name, HEAP_NAME_LEN - 1);
		heap_names[n][HEAP_NAME_LEN - 1] = '\0';
		n++;
	}
	closedir(d);
	return n;
}

/*
 * bench_one - benchmark one (heap, size, flags) combination
 *
 * Runs BENCH_REPS iterations measuring:
 *   GROUP A: alloc_us       - ioctl(DMA_HEAP_IOCTL_ALLOC)
 *   GROUP B: mmap_us        - mmap()
 *            memset_us      - memset(0xa5, size) + DMA_BUF_SYNC
 *   GROUP C: iommu_map_us   - DRM_IOCTL_PRIME_FD_TO_HANDLE (vgem import)
 *            iommu_unmap_us - DRM_IOCTL_GEM_CLOSE (vgem close)
 *            (skipped if vgem_fd < 0)
 *   GROUP D: free_us        - close(dmabuf_fd)
 *
 * Data integrity check (rep 0 only):
 *   - Zero-check before memset (verifies heap zero-initialises buffers)
 *   - Verify 0xa5 after memset (catches memory aliasing bugs)
 *
 * Returns true if all allocations succeeded and integrity check passed.
 */
static bool bench_one(int heap_fd, const char *heap_name,
		      size_t size, const char *size_label,
		      const char *flag_name, unsigned int flags,
		      int vgem_fd)
{
	long alloc_s[BENCH_REPS], mmap_s[BENCH_REPS], memset_s[BENCH_REPS];
	long iommu_map_s[BENCH_REPS], iommu_unmap_s[BENCH_REPS];
	long free_s[BENCH_REPS];
	struct bench_stats alloc_st, mmap_st, memset_st, free_st;
	struct bench_stats iommu_map_st, iommu_unmap_st;
	struct timespec ts_start, ts_end;
	bool integrity_ok = true;
	unsigned char *c;
	uint32_t handle;
	size_t j;
	int i, dmabuf_fd, ret;
	void *p;

	for (i = 0; i < BENCH_REPS; i++) {
		/* GROUP A: Allocation */
		clock_gettime(CLOCK_MONOTONIC, &ts_start);
		ret = dmabuf_heap_alloc(heap_fd, size, flags, &dmabuf_fd);
		clock_gettime(CLOCK_MONOTONIC, &ts_end);
		if (ret) {
			alloc_s[i] = 0;
			mmap_s[i] = 0;
			memset_s[i] = 0;
			iommu_map_s[i] = 0;
			iommu_unmap_s[i] = 0;
			free_s[i] = 0;
			integrity_ok = false;
			continue;
		}
		alloc_s[i] = time_diff_us(&ts_start, &ts_end);

		/* GROUP B: CPU Access */
		clock_gettime(CLOCK_MONOTONIC, &ts_start);
		p = mmap(NULL, size, PROT_READ | PROT_WRITE,
			 MAP_SHARED, dmabuf_fd, 0);
		clock_gettime(CLOCK_MONOTONIC, &ts_end);
		mmap_s[i] = (p != MAP_FAILED) ?
			    time_diff_us(&ts_start, &ts_end) : 0;

		if (p != MAP_FAILED) {
			dmabuf_sync(dmabuf_fd, DMA_BUF_SYNC_START);
			c = (unsigned char *)p;

			/* Zero-check on first rep (before memset) */
			if (i == 0 && integrity_ok) {
				for (j = 0; j < size; j++) {
					if (c[j] != 0) {
						integrity_ok = false;
						break;
					}
				}
			}

			/* Time the memset — memory bandwidth proxy */
			clock_gettime(CLOCK_MONOTONIC, &ts_start);
			memset(p, 0xa5, size);
			clock_gettime(CLOCK_MONOTONIC, &ts_end);
			memset_s[i] = time_diff_us(&ts_start, &ts_end);

			/* Verify 0xa5 on first rep (catches aliasing bugs) */
			if (i == 0 && integrity_ok) {
				for (j = 0; j < size; j++) {
					if (c[j] != 0xa5) {
						integrity_ok = false;
						break;
					}
				}
			}

			dmabuf_sync(dmabuf_fd, DMA_BUF_SYNC_END);
			munmap(p, size);
		} else {
			memset_s[i] = 0;
		}

		/* GROUP C: IOMMU map/unmap via vgem (optional) */
		if (vgem_fd >= 0) {
			clock_gettime(CLOCK_MONOTONIC, &ts_start);
			ret = import_vgem_fd(vgem_fd, dmabuf_fd, &handle);
			clock_gettime(CLOCK_MONOTONIC, &ts_end);
			iommu_map_s[i] = (!ret) ?
					 time_diff_us(&ts_start, &ts_end) : 0;
			if (!ret) {
				clock_gettime(CLOCK_MONOTONIC, &ts_start);
				close_handle(vgem_fd, handle);
				clock_gettime(CLOCK_MONOTONIC, &ts_end);
				iommu_unmap_s[i] =
					time_diff_us(&ts_start, &ts_end);
			} else {
				iommu_unmap_s[i] = 0;
			}
		} else {
			iommu_map_s[i] = 0;
			iommu_unmap_s[i] = 0;
		}

		/* GROUP D: Buffer Release */
		clock_gettime(CLOCK_MONOTONIC, &ts_start);
		close(dmabuf_fd);
		clock_gettime(CLOCK_MONOTONIC, &ts_end);
		free_s[i] = time_diff_us(&ts_start, &ts_end);
	}

	compute_stats(alloc_s, BENCH_REPS, &alloc_st);
	compute_stats(mmap_s, BENCH_REPS, &mmap_st);
	compute_stats(memset_s, BENCH_REPS, &memset_st);
	compute_stats(free_s, BENCH_REPS, &free_st);

	ksft_print_msg("heap=%s size=%s flags=%s reps=%d\n",
		       heap_name, size_label, flag_name, BENCH_REPS);
	ksft_print_msg("  alloc:  avg=%.0fus std=%.0fus min=%ldus max=%ldus\n",
		       alloc_st.avg_us, alloc_st.std_dev_us,
		       alloc_st.min_us, alloc_st.max_us);
	ksft_print_msg("  mmap:   avg=%.0fus std=%.0fus min=%ldus max=%ldus\n",
		       mmap_st.avg_us, mmap_st.std_dev_us,
		       mmap_st.min_us, mmap_st.max_us);
	ksft_print_msg("  memset: avg=%.0fus std=%.0fus min=%ldus max=%ldus\n",
		       memset_st.avg_us, memset_st.std_dev_us,
		       memset_st.min_us, memset_st.max_us);

	if (vgem_fd >= 0) {
		compute_stats(iommu_map_s, BENCH_REPS, &iommu_map_st);
		compute_stats(iommu_unmap_s, BENCH_REPS, &iommu_unmap_st);
		ksft_print_msg("  iommu_map:   avg=%.0fus std=%.0fus min=%ldus max=%ldus\n",
			       iommu_map_st.avg_us, iommu_map_st.std_dev_us,
			       iommu_map_st.min_us, iommu_map_st.max_us);
		ksft_print_msg("  iommu_unmap: avg=%.0fus std=%.0fus min=%ldus max=%ldus\n",
			       iommu_unmap_st.avg_us, iommu_unmap_st.std_dev_us,
			       iommu_unmap_st.min_us, iommu_unmap_st.max_us);
	}

	ksft_print_msg("  free:   avg=%.0fus std=%.0fus min=%ldus max=%ldus\n",
		       free_st.avg_us, free_st.std_dev_us,
		       free_st.min_us, free_st.max_us);

	return integrity_ok;
}

int main(void)
{
	char heap_names[MAX_HEAPS][HEAP_NAME_LEN];
	int n_heaps, n_sizes, n_flags;
	int vgem_fd, heap_fd;
	int h, s, f;
	bool ok;

	ksft_print_header();

	n_heaps = discover_heaps(heap_names, MAX_HEAPS);
	if (!n_heaps) {
		ksft_print_msg("No heaps found in %s\n", DEVPATH);
		return KSFT_SKIP;
	}

	n_sizes = ARRAY_SIZE(bench_sizes);
	n_flags = ARRAY_SIZE(flag_variants);

	vgem_fd = open_vgem();
	if (vgem_fd < 0)
		ksft_print_msg("vgem unavailable; skipping IOMMU tests\n");
	else
		ksft_print_msg("vgem available; IOMMU map/unmap measured\n");

	ksft_set_plan(n_heaps * n_sizes * n_flags);

	for (h = 0; h < n_heaps; h++) {
		ksft_print_msg("Benchmarking heap: %s\n", heap_names[h]);
		ksft_print_msg("===========================================\n");
		heap_fd = dmabuf_heap_open(heap_names[h]);

		for (s = 0; s < n_sizes; s++) {
			for (f = 0; f < n_flags; f++) {
				ok = bench_one(heap_fd,
					       heap_names[h],
					       bench_sizes[s].bytes,
					       bench_sizes[s].label,
					       flag_variants[f].name,
					       flag_variants[f].val,
					       vgem_fd);
				ksft_test_result(ok, "%s %s %s\n",
						 heap_names[h],
						 bench_sizes[s].label,
						 flag_variants[f].name);
			}
		}
		close(heap_fd);
	}

	if (vgem_fd >= 0)
		close(vgem_fd);

	ksft_finished();
}
