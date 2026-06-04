// SPDX-License-Identifier: GPL-2.0-only
/*
 * zram_compr_test.c - Test module for zram_get_compr_size_for_swp_entry()
 *
 * Creates /proc/zram_compr_test.  Reading it walks the calling process's
 * swap PTEs and sums the compressed bytes each page occupies in ZRAM.
 *
 * Output format (one line):
 *   pid=<PID> swap_pages=<N> zram_compr_bytes=<B> zram_compr_kb=<KB>
 *   ptes=<N> present=<N> none=<N> swap=<N>
 *
 * Uses walk_page_range() directly (built-in, no export needed).
 */

#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/pagewalk.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/leafops.h>
#include <linux/pgtable.h>
#include <linux/sched/mm.h>
#include <linux/zram_compr.h>

#define PROC_NAME "zram_compr_test"

struct zram_walk_data {
	unsigned long swap_pages;	/* PTEs backed by ZRAM (sz > 0) */
	unsigned long zram_compr_bytes;
	unsigned long ptes_checked;
	unsigned long ptes_present;
	unsigned long ptes_none;
	unsigned long ptes_swap;	/* all non-present, non-none PTEs */
};

static int zram_pte_entry(pte_t *pte, unsigned long addr, unsigned long next,
			  struct mm_walk *walk)
{
	struct zram_walk_data *data = walk->private;
	softleaf_t leaf;
	pte_t pteval = ptep_get(pte);

	data->ptes_checked++;

	if (pte_present(pteval)) {
		data->ptes_present++;
		return 0;
	}
	if (pte_none(pteval)) {
		data->ptes_none++;
		return 0;
	}

	leaf = softleaf_from_pte(pteval);
	if (softleaf_is_swap(leaf)) {
		swp_entry_t entry = leaf;
		size_t sz;

		data->ptes_swap++;
		sz = zram_get_compr_size_for_swp_entry(entry);
		if (sz) {
			data->swap_pages++;
			data->zram_compr_bytes += sz;
		}
	}
	return 0;
}

static const struct mm_walk_ops zram_walk_ops = {
	.pte_entry = zram_pte_entry,
	.walk_lock = PGWALK_RDLOCK,
};

static int zram_compr_test_show(struct seq_file *m, void *v)
{
	struct task_struct *task = current;
	struct mm_struct *mm;
	struct zram_walk_data data = {};

	mm = get_task_mm(task);
	if (!mm) {
		seq_printf(m, "pid=%d error=no_mm\n", task_pid_nr(task));
		return 0;
	}

	mmap_read_lock(mm);
	walk_page_range(mm, 0, TASK_SIZE, &zram_walk_ops, &data);
	mmap_read_unlock(mm);
	mmput(mm);

	seq_printf(m, "pid=%d swap_pages=%lu zram_compr_bytes=%lu zram_compr_kb=%lu ptes=%lu present=%lu none=%lu swap=%lu\n",
		   task_pid_nr(task),
		   data.swap_pages,
		   data.zram_compr_bytes,
		   data.zram_compr_bytes / 1024,
		   data.ptes_checked,
		   data.ptes_present,
		   data.ptes_none,
		   data.ptes_swap);

	return 0;
}

static int zram_compr_test_open(struct inode *inode, struct file *file)
{
	return single_open(file, zram_compr_test_show, NULL);
}

static const struct proc_ops zram_compr_test_fops = {
	.proc_open    = zram_compr_test_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

static struct proc_dir_entry *proc_entry;

static int __init zram_compr_test_init(void)
{
	proc_entry = proc_create(PROC_NAME, 0444, NULL, &zram_compr_test_fops);
	if (!proc_entry) {
		pr_err("zram_compr_test: failed to create /proc/%s\n", PROC_NAME);
		return -ENOMEM;
	}
	pr_info("zram_compr_test: /proc/%s created\n", PROC_NAME);
	return 0;
}

static void __exit zram_compr_test_exit(void)
{
	proc_remove(proc_entry);
}

module_init(zram_compr_test_init);
module_exit(zram_compr_test_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Prakash Gupta <prakash.gupta@oss.qualcomm.com>");
MODULE_DESCRIPTION("Test module for zram_get_compr_size_for_swp_entry()");
