// SPDX-License-Identifier: GPL-2.0-only
/*
 * IOMMU DMA stack profiling — debugfs interface
 *
 * Exposes per-function latency statistics (count, total, min, max, histogram)
 * for the full DMA/IOMMU path via:
 *
 *   /sys/kernel/debug/iommu/profiling/
 *     enable   — write "1"/"0" to enable/disable; read current state
 *     stats    — read to get stats snapshot; CLEARS stats on read
 *     reset    — write "1" to clear stats without reading
 *
 * Designed for live Ethernet profiling:
 *   echo 1 > /sys/kernel/debug/iommu/profiling/enable
 *   iperf3 -c ... -P 8 -t 30 &
 *   # Sample every second while traffic runs:
 *   while sleep 1; do cat /sys/kernel/debug/iommu/profiling/stats; done
 *   echo 0 > /sys/kernel/debug/iommu/profiling/enable
 *
 * The clear-on-read semantics give per-interval snapshots at whatever
 * sampling rate the user chooses, without needing to stop the workload.
 *
 * Overhead when disabled: one branch prediction miss per instrumented call
 * (~1 ns). Overhead when enabled: ~10 ns per call (two ktime_get() calls).
 */

#include <linux/debugfs.h>
#include <linux/iommu.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/math64.h>

/* Histogram bucket boundaries in µs (lower bound of each bucket) */
static const u32 hist_us_lower[16] = {
	0, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1000, 2000, 4000, 8000, 16000
};

static void print_func_stats(struct seq_file *m, const char *name,
			      const struct iommu_func_stats *s)
{
	u64 avg_ns = 0, avg_us, min_us, max_us;
	u32 avg_rem, min_rem, max_rem;
	int i;

	if (!s->count) {
		seq_printf(m, "  %-32s  count=0\n", name);
		return;
	}

	avg_ns = div_u64(s->total_ns, s->count);
	avg_us = div_u64_rem(avg_ns, 1000, &avg_rem);
	min_us = div_u64_rem(s->min_ns, 1000, &min_rem);
	max_us = div_u64_rem(s->max_ns, 1000, &max_rem);

	seq_printf(m,
		"  %-32s  count=%-8llu  avg=%llu.%03u µs  "
		"min=%llu.%03u µs  max=%llu.%03u µs\n",
		name, s->count,
		avg_us, avg_rem,
		min_us, min_rem,
		max_us, max_rem);

	/* Print histogram — only non-zero buckets */
	for (i = 0; i < 16; i++) {
		if (!s->hist[i])
			continue;
		if (i < 15)
			seq_printf(m, "    [%5u–%5u µs]: %llu\n",
				   hist_us_lower[i], hist_us_lower[i + 1] - 1,
				   s->hist[i]);
		else
			seq_printf(m, "    [≥%5u µs    ]: %llu\n",
				   hist_us_lower[i], s->hist[i]);
	}
}

static int iommu_prof_stats_show(struct seq_file *m, void *v)
{
	struct iommu_prof_stats snap;
	unsigned long flags;

	/* Snapshot + clear atomically under spinlock */
	spin_lock_irqsave(&iommu_prof_lock, flags);
	snap = iommu_prof_stats;
	memset(&iommu_prof_stats, 0, sizeof(iommu_prof_stats));
	spin_unlock_irqrestore(&iommu_prof_lock, flags);

	seq_printf(m, "IOMMU profiling snapshot (cleared on read)\n");
	seq_printf(m, "enabled=%d\n\n", iommu_prof_enabled);

	seq_printf(m, "--- MAP path ---\n");
	print_func_stats(m, "dma_map_phys (top)",    &snap.dma_map);
	print_func_stats(m, "__iommu_dma_map (inner)", &snap.__dma_map);
	print_func_stats(m, "alloc_iova",             &snap.alloc_iova);
	print_func_stats(m, "iommu_map",              &snap.iommu_map);
	print_func_stats(m, "iommu_sync_map",         &snap.iommu_sync_map);
	print_func_stats(m, "alloc_pgt",              &snap.alloc_pgt);
	print_func_stats(m, "install_table",          &snap.install_table);

	seq_printf(m, "\n--- UNMAP path ---\n");
	print_func_stats(m, "dma_unmap_phys (top)",    &snap.dma_unmap);
	print_func_stats(m, "__iommu_dma_unmap (inner)", &snap.__dma_unmap);
	print_func_stats(m, "iommu_unmap_fast",        &snap.iommu_unmap);
	print_func_stats(m, "iommu_iotlb_sync (TLB)",  &snap.tlb_sync);
	print_func_stats(m, "free_iova",               &snap.free_iova);
	print_func_stats(m, "iova_to_phys",            &snap.iova_to_phys);

	return 0;
}

static int iommu_prof_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, iommu_prof_stats_show, NULL);
}

static const struct file_operations iommu_prof_stats_fops = {
	.open    = iommu_prof_stats_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* enable file: read/write iommu_prof_enabled */
static ssize_t iommu_prof_enable_read(struct file *file, char __user *buf,
				      size_t count, loff_t *ppos)
{
	char tmp[4];
	int len = snprintf(tmp, sizeof(tmp), "%d\n", iommu_prof_enabled ? 1 : 0);

	return simple_read_from_buffer(buf, count, ppos, tmp, len);
}

static ssize_t iommu_prof_enable_write(struct file *file,
				       const char __user *buf,
				       size_t count, loff_t *ppos)
{
	char tmp[4];
	bool val;

	if (count >= sizeof(tmp))
		return -EINVAL;
	if (copy_from_user(tmp, buf, count))
		return -EFAULT;
	tmp[count] = '\0';

	if (kstrtobool(tmp, &val))
		return -EINVAL;

	iommu_prof_enabled = val;
	return count;
}

static const struct file_operations iommu_prof_enable_fops = {
	.read  = iommu_prof_enable_read,
	.write = iommu_prof_enable_write,
	.llseek = default_llseek,
};

/* reset file: write "1" to clear stats without reading */
static ssize_t iommu_prof_reset_write(struct file *file,
				      const char __user *buf,
				      size_t count, loff_t *ppos)
{
	unsigned long flags;

	spin_lock_irqsave(&iommu_prof_lock, flags);
	memset(&iommu_prof_stats, 0, sizeof(iommu_prof_stats));
	spin_unlock_irqrestore(&iommu_prof_lock, flags);

	return count;
}

static const struct file_operations iommu_prof_reset_fops = {
	.write  = iommu_prof_reset_write,
	.llseek = default_llseek,
};

static int __init iommu_profiling_init(void)
{
	struct dentry *dir;

	if (!iommu_debugfs_dir)
		return 0;

	dir = debugfs_create_dir("profiling", iommu_debugfs_dir);
	if (IS_ERR(dir))
		return PTR_ERR(dir);

	debugfs_create_file("enable",       0644, dir, NULL, &iommu_prof_enable_fops);
	debugfs_create_file("stats",        0444, dir, NULL, &iommu_prof_stats_fops);
	debugfs_create_file("reset",        0200, dir, NULL, &iommu_prof_reset_fops);

	pr_info("iommu: profiling debugfs at /sys/kernel/debug/iommu/profiling/\n");
	return 0;
}
late_initcall(iommu_profiling_init);
