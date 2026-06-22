// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/iommu.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/sizes.h>
#include <linux/io-pgtable.h>
#include <linux/list.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include "arm/arm-smmu-v3/arm-smmu-v3.h"

#define DRV_NAME "smmutest"
#define TEST_PTE_CONT           BIT_ULL(52)
#define TEST_PTE_TYPE_MASK      0x3ULL
#define TEST_PTE_TYPE_BLOCK     1ULL

static char *bdf = "0000:00:01.0";
module_param(bdf, charp, 0644);
MODULE_PARM_DESC(bdf, "PCI BDF to test, format dddd:bb:dd.f (default 0000:00:01.0)");

static bool dump_pgtbl = true;
module_param(dump_pgtbl, bool, 0644);
MODULE_PARM_DESC(dump_pgtbl, "Dump IOVA page translations for mapped ranges");

static bool run_contig_hint_test_on_init;
module_param(run_contig_hint_test_on_init, bool, 0644);
MODULE_PARM_DESC(run_contig_hint_test_on_init,
		 "Run auto iommu_map/iommu_unmap contig-hint-size tests at insmod");

static struct pci_dev *test_pdev;
static struct kobject *smmu_kobj;
static DEFINE_MUTEX(test_lock);

struct smmutest_map_entry {
	struct list_head node;
	dma_addr_t iova;
	phys_addr_t pa;
	size_t size;
};

static LIST_HEAD(map_list);
static char last_contig_test[96] = "not-run";

/* atos: last IOVA queried and its translated PA */
static dma_addr_t atos_last_iova;
static phys_addr_t atos_last_pa;

/* profiling: per-size map/unmap latency results */
#define PROF_SIZES 3
static const size_t prof_sizes[PROF_SIZES] = { SZ_4K, SZ_64K, SZ_2M };
static const char * const prof_size_names[PROF_SIZES] = { "4K", "64K", "2M" };

struct smmutest_prof_result {
	u64 map_avg_ns[PROF_SIZES];
	u64 unmap_avg_ns[PROF_SIZES];
	u32 nr_iters;
	bool valid;
};

static struct smmutest_prof_result last_prof;

/* dma_profiling: DMA API map/unmap latency (uses flush queue when DMA_FQ) */
static struct smmutest_prof_result last_dma_prof;

static bool get_leaf_pte(struct iommu_domain *domain, dma_addr_t iova,
			 u64 *leaf_pte, int *leaf_level);

static int parse_bdf(const char *s, u16 *domain, u8 *bus, u8 *dev, u8 *fn)
{
	unsigned int d, b, dv, f;
	int n;

	n = sscanf(s, "%x:%x:%x.%x", &d, &b, &dv, &f);
	if (n != 4)
		return -EINVAL;
	if (d > 0xffff || b > 0xff || dv > 0x1f || f > 0x7)
		return -EINVAL;

	*domain = (u16)d;
	*bus = (u8)b;
	*dev = (u8)dv;
	*fn = (u8)f;
	return 0;
}

static void report_ats(struct pci_dev *pdev)
{
	int cap;
	u16 ctrl = 0, sts = 0;

	cap = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_ATS);
	if (!cap) {
		pr_info(DRV_NAME ": ATS capability not present on %s\n", pci_name(pdev));
		return;
	}

	pci_read_config_word(pdev, cap + PCI_ATS_CTRL, &ctrl);
	pci_read_config_word(pdev, cap + PCI_ATS_CAP, &sts);

	pr_info(DRV_NAME ": ATS cap present on %s (cap=0x%x)\n", pci_name(pdev), cap);
	pr_info(DRV_NAME ": ATS ctrl=0x%04x (Enable bit=%u), cap=0x%04x\n",
		ctrl, !!(ctrl & PCI_ATS_CTRL_ENABLE), sts);
}

static void dump_iova_pages(struct iommu_domain *domain, dma_addr_t iova_start,
			    size_t len, const char *tag, phys_addr_t exp_pa_base,
			    bool has_expected)
{
	dma_addr_t iova;
	size_t pg;
	size_t pages = DIV_ROUND_UP(len, PAGE_SIZE);
	int prev_state = -2;
	size_t range_start_pg = 0;
	size_t cont_set_pages = 0, cont_not_set_pages = 0, no_pte_pages = 0;
	size_t match_pages = 0, mismatch_pages = 0;

	if (!domain || !dump_pgtbl)
		return;

	pr_info(DRV_NAME ": %s contig-range dump iova=0x%llx len=0x%zx pages=%zu\n",
		tag, (unsigned long long)iova_start, len, pages);

	for (pg = 0, iova = iova_start; pg < pages; pg++, iova += PAGE_SIZE) {
		u64 leaf_pte = 0;
		int leaf_level = -1;
		int state;
		phys_addr_t pa = iommu_iova_to_phys(domain, iova);
		phys_addr_t exp = 0;

		if (has_expected) {
			exp = exp_pa_base + (iova - iova_start);
			if (pa == exp)
				match_pages++;
			else
				mismatch_pages++;
		}

		if (!get_leaf_pte(domain, iova, &leaf_pte, &leaf_level))
			state = -1; /* no leaf pte */
		else
			state = !!(leaf_pte & TEST_PTE_CONT);

		if (state != prev_state) {
			if (prev_state != -2) {
				dma_addr_t start_iova = iova_start + range_start_pg * PAGE_SIZE;
				dma_addr_t end_iova = iova_start + pg * PAGE_SIZE - PAGE_SIZE;
				size_t range_pages = pg - range_start_pg;
				const char *prev_name = (prev_state == 1) ? "SET" :
							(prev_state == 0) ? "NOT_SET" : "NO_PTE";

				pr_info(DRV_NAME ": %s contig=%s iova=0x%llx..0x%llx pages=%zu\n",
					tag, prev_name,
					(unsigned long long)start_iova,
					(unsigned long long)end_iova,
					range_pages);
			}
			range_start_pg = pg;
			prev_state = state;
		}

		if (state == 1)
			cont_set_pages++;
		else if (state == 0)
			cont_not_set_pages++;
		else
			no_pte_pages++;
	}

	if (prev_state != -2) {
		dma_addr_t start_iova = iova_start + range_start_pg * PAGE_SIZE;
		dma_addr_t end_iova = iova_start + pages * PAGE_SIZE - PAGE_SIZE;
		size_t range_pages = pages - range_start_pg;
		const char *state_name;

		state_name = (prev_state == 1) ? "SET" :
			     (prev_state == 0) ? "NOT_SET" : "NO_PTE";
		pr_info(DRV_NAME ": %s contig=%s iova=0x%llx..0x%llx pages=%zu\n",
			tag, state_name,
			(unsigned long long)start_iova,
			(unsigned long long)end_iova,
			range_pages);
	}

	pr_info(DRV_NAME ": %s summary: contig_set_pages=%zu contig_not_set_pages=%zu no_pte_pages=%zu\n",
		tag, cont_set_pages, cont_not_set_pages, no_pte_pages);
	if (has_expected)
		pr_info(DRV_NAME ": %s map-check: match=%zu mismatch=%zu\n",
			tag, match_pages, mismatch_pages);
}

struct contig_unmap_step {
	u64 off;
	u64 size;
};

struct contig_case {
	const char *name;
	u64 iova;
	u64 pa;
	u64 size;
	u32 nr_unmaps;
	struct contig_unmap_step unmaps[3];
};

static bool get_leaf_pte(struct iommu_domain *domain, dma_addr_t iova,
			 u64 *leaf_pte, int *leaf_level)
{
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);
	struct io_pgtable_ops *ops = smmu_domain->pgtbl_ops;
	struct arm_lpae_io_pgtable_walk_data wd = {};
	int lvl, ret;

	if (!ops || !ops->pgtable_walk)
		return false;

	ret = ops->pgtable_walk(ops, (unsigned long)iova, &wd);
	if (ret)
		return false;

	for (lvl = 3; lvl >= 0; lvl--) {
		u64 pte = wd.ptes[lvl];

		if (!pte)
			continue;
		if (lvl == 3 || ((pte & TEST_PTE_TYPE_MASK) == TEST_PTE_TYPE_BLOCK)) {
			*leaf_pte = pte;
			*leaf_level = lvl;
			return true;
		}
	}

	return false;
}

static void report_contig_hint_status(struct iommu_domain *domain,
				      dma_addr_t iova, size_t size,
				      const char *name)
{
	size_t pages = DIV_ROUND_UP(size, PAGE_SIZE);
	size_t pg, cont_pages = 0, leaf_pages = 0;
	size_t l2_block_pages = 0, l3_page_entries = 0;

	for (pg = 0; pg < pages; pg++) {
		u64 leaf;
		int level = -1;

		if (!get_leaf_pte(domain, iova + pg * PAGE_SIZE, &leaf, &level))
			continue;
		leaf_pages++;
		if (level == 2)
			l2_block_pages++;
		else if (level == 3)
			l3_page_entries++;
		if (leaf & TEST_PTE_CONT)
			cont_pages++;
	}

	pr_info(DRV_NAME ": %s entry-type summary: leaf_pages=%zu l2_block(2MB)=%zu l3_page(4KB)=%zu\n",
		name, leaf_pages, l2_block_pages, l3_page_entries);
	pr_info(DRV_NAME ": %s contig-hint validation: cont_pages=%zu status=%s\n",
		name, cont_pages, cont_pages ? "SET" : "NOT_SET");
}

static int run_one_contig_case(struct iommu_domain *domain, const struct contig_case *tc)
{
	phys_addr_t got;
	u32 i;
	int ret;
	size_t cleanup = 0;
	size_t total_unmapped = 0;
	bool mapped = false;

	if (!IS_ALIGNED(tc->iova, PAGE_SIZE) ||
	    !IS_ALIGNED(tc->pa, PAGE_SIZE) ||
	    !IS_ALIGNED(tc->size, PAGE_SIZE) || !tc->size) {
		pr_err(DRV_NAME ": %s invalid map alignment/size iova=0x%llx pa=0x%llx size=0x%llx\n",
		       tc->name, tc->iova, tc->pa, tc->size);
		return -EINVAL;
	}

	for (i = 0; i < tc->nr_unmaps; i++) {
		u64 off = tc->unmaps[i].off;
		u64 usz = tc->unmaps[i].size;
		if (!IS_ALIGNED(off, PAGE_SIZE) || !IS_ALIGNED(usz, PAGE_SIZE) || !usz ||
		    off + usz > tc->size) {
			pr_err(DRV_NAME ": %s invalid unmap[%u] off=0x%llx size=0x%llx map_size=0x%llx\n",
			       tc->name, i, off, usz, tc->size);
			return -EINVAL;
		}
	}

	ret = iommu_map(domain, (dma_addr_t)tc->iova, (phys_addr_t)tc->pa,
			(size_t)tc->size, IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
	if (ret) {
		pr_err(DRV_NAME ": %s iommu_map failed ret=%d iova=0x%llx pa=0x%llx size=0x%llx\n",
		       tc->name, ret, tc->iova, tc->pa, tc->size);
		return ret;
	}
	mapped = true;

	got = iommu_iova_to_phys(domain, (dma_addr_t)tc->iova);
	if (got != (phys_addr_t)tc->pa) {
		pr_err(DRV_NAME ": %s iova_to_phys mismatch got=0x%llx expected=0x%llx\n",
		       tc->name, (unsigned long long)got, tc->pa);
		ret = -EFAULT;
		goto out_cleanup;
	}

	report_contig_hint_status(domain, (dma_addr_t)tc->iova, (size_t)tc->size, tc->name);
	dump_iova_pages(domain, (dma_addr_t)tc->iova, (size_t)tc->size,
			tc->name, (phys_addr_t)tc->pa, true);

		for (i = 0; i < tc->nr_unmaps; i++) {
			dma_addr_t unmap_iova = (dma_addr_t)(tc->iova + tc->unmaps[i].off);
			size_t unmap_size = (size_t)tc->unmaps[i].size;
			size_t unmapped = iommu_unmap(domain, unmap_iova, unmap_size);
			phys_addr_t pa_after = iommu_iova_to_phys(domain, unmap_iova);

			if (unmapped != unmap_size) {
				pr_err(DRV_NAME ": %s unmap[%u] short unmapped=0x%zx expected=0x%zx\n",
				       tc->name, i, unmapped, unmap_size);
				ret = -EFAULT;
				goto out_cleanup;
			}
			if (pa_after) {
				pr_err(DRV_NAME ": %s unmap[%u] iova still mapped pa=0x%llx\n",
				       tc->name, i, (unsigned long long)pa_after);
				ret = -EFAULT;
				goto out_cleanup;
			}
			total_unmapped += unmapped;
		}

	ret = 0;
out_cleanup:
	if (mapped) {
		/*
		 * Cleanup only any remaining mapped bytes. Re-unmapping a fully
		 * unmapped range may trigger io-pgtable warnings for contig cases.
		 */
		if (total_unmapped < tc->size) {
			size_t remain = (size_t)(tc->size - total_unmapped);
			cleanup = iommu_unmap(domain, (dma_addr_t)tc->iova, remain);
			if (cleanup)
				pr_info(DRV_NAME ": %s cleanup unmapped remaining=0x%zx\n",
					tc->name, cleanup);
		}
	}

	if (iommu_iova_to_phys(domain, (dma_addr_t)tc->iova)) {
		pr_err(DRV_NAME ": %s base iova still mapped after cleanup\n", tc->name);
		ret = -EFAULT;
	}

	if (!ret)
		pr_info(DRV_NAME ": %s PASS size=0x%llx\n", tc->name, tc->size);

	return ret;
}

static int run_contig_hint_tests_locked(void)
{
	struct iommu_domain *domain;
	static const struct contig_case tests[] = {
		{ "4K", 0x1000ULL, 0xc0000000ULL, 0x1000ULL, 1, { { 0x0ULL, 0x1000ULL } } },
		{ "64K", 0x10000ULL, 0xc0000000ULL, 0x10000ULL, 1, { { 0x0ULL, 0x10000ULL } } },
		{ "64K+4K", 0x10000ULL, 0xc0000000ULL, 0x11000ULL, 1, { { 0x0ULL, 0x11000ULL } } },
		{ "128K", 0x40000ULL, 0xc0000000ULL, 0x20000ULL, 1, { { 0x0ULL, 0x20000ULL } } },
		{ "128K+4K", 0x40000ULL, 0xc0000000ULL, 0x21000ULL, 1, { { 0x0ULL, 0x21000ULL } } },
		{ "2M", 0x200000ULL, 0xc0000000ULL, 0x200000ULL, 1, { { 0x0ULL, 0x200000ULL } } },
		{ "2M+64K", 0x200000ULL, 0xc0000000ULL, 0x210000ULL, 1, { { 0x0ULL, 0x210000ULL } } },
		{ "32M_unaligned", 0x10000ULL, 0xc0000000ULL, 0x2000000ULL, 1, { { 0x0ULL, 0x2000000ULL } } },
		{ "32M", 0x2000000ULL, 0xc0000000ULL, 0x2000000ULL, 1, { { 0x0ULL, 0x2000000ULL } } },
		{ "32M_alt_iova", 0x4000000ULL, 0xc0000000ULL, 0x2000000ULL, 1, { { 0x0ULL, 0x2000000ULL } } },
		{ "32M+4K", 0x2000000ULL, 0xc0000000ULL, 0x2001000ULL, 1, { { 0x0ULL, 0x2001000ULL } } },
		{ "32M+64K_iova32M", 0x2000000ULL, 0xc0000000ULL, 0x2010000ULL, 1, { { 0x0ULL, 0x2010000ULL } } },
		{ "32M+64K_iova64K", 0x2010000ULL, 0xc0000000ULL, 0x2010000ULL, 1, { { 0x0ULL, 0x2010000ULL } } },
		{ "128M", 0x8000000ULL, 0xc0000000ULL, 0x8000000ULL, 1, { { 0x0ULL, 0x8000000ULL } } },
		{ "128M+4K", 0x8000000ULL, 0xc0000000ULL, 0x8001000ULL, 1, { { 0x0ULL, 0x8001000ULL } } },
		{ "128M+256K_pa_off", 0x8040000ULL, 0xc0040000ULL, 0x8040000ULL, 1, { { 0x0ULL, 0x8040000ULL } } },
		{ "128M+256K", 0x8040000ULL, 0xc0000000ULL, 0x8040000ULL, 1, { { 0x0ULL, 0x8040000ULL } } },
		{ "4M+256K_case", 0x800000ULL, 0xc0000000ULL, 0x440000ULL, 1, { { 0x0ULL, 0x440000ULL } } },
	};
	int pass = 0, total = 0, ret;
	size_t i;
	const u64 case_stride = 0x10000000ULL; /* 256MB window per case */

	domain = iommu_get_domain_for_dev(&test_pdev->dev);
	if (!domain)
		return -ENODEV;

	pr_info(DRV_NAME ": contig-hint auto tests pgsize_bitmap=0x%lx (from contig_hint_tests.txt)\n",
		domain->pgsize_bitmap);

	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		struct contig_case tc = tests[i];

		/*
		 * Isolate each case into its own IOVA/PA window to guarantee no
		 * cross-case overlap even if a previous case had partial cleanup.
		 */
		tc.iova += i * case_stride;
		tc.pa += i * case_stride;

		total++;
		ret = run_one_contig_case(domain, &tc);
		if (!ret)
			pass++;
	}

	snprintf(last_contig_test, sizeof(last_contig_test),
		 "%s %d/%d", (pass == total) ? "PASS" : "FAIL", pass, total);
	pr_info(DRV_NAME ": contig-hint auto test result: %s\n", last_contig_test);

	return (pass == total) ? 0 : -EFAULT;
}

static ssize_t dump_store(struct kobject *kobj, struct kobj_attribute *attr,
			  const char *buf, size_t count)
{
	struct iommu_domain *domain;
	unsigned long long iova, size;
	struct smmutest_map_entry *e;
	int ret;

	mutex_lock(&test_lock);
	domain = iommu_get_domain_for_dev(&test_pdev->dev);
	if (!domain) {
		mutex_unlock(&test_lock);
		return -ENODEV;
	}

	ret = sscanf(buf, "%llx %llx", &iova, &size);
	if (ret == 2) {
		if (!size || !IS_ALIGNED(iova, PAGE_SIZE) || !IS_ALIGNED(size, PAGE_SIZE)) {
			mutex_unlock(&test_lock);
			return -EINVAL;
		}
		dump_iova_pages(domain, (dma_addr_t)iova, (size_t)size, "manual-dump",
				0, false);
	} else {
		if (list_empty(&map_list))
			pr_info(DRV_NAME ": no active iommu_map; provide 'iova size' to dump\n");
		list_for_each_entry(e, &map_list, node)
			dump_iova_pages(domain, e->iova, e->size, "iommu-map",
					e->pa, true);
	}
	mutex_unlock(&test_lock);

	return count;
}

static ssize_t state_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct smmutest_map_entry *e;
	ssize_t n;
	size_t used = 0;
	int idx = 0;
	int map_count = 0;

	mutex_lock(&test_lock);
	list_for_each_entry(e, &map_list, node)
		map_count++;

	used += scnprintf(buf + used, PAGE_SIZE - used,
			  "bdf=%s mappings=%d contig_test=%s\n",
			  bdf, map_count, last_contig_test);
	list_for_each_entry(e, &map_list, node) {
		used += scnprintf(buf + used, PAGE_SIZE - used,
				  "map[%d]: iova=0x%llx pa=0x%llx size=0x%zx\n",
				  idx++,
				  (unsigned long long)e->iova,
				  (unsigned long long)e->pa,
				  e->size);
		if (used >= PAGE_SIZE - 64)
			break;
	}
	n = used;
	mutex_unlock(&test_lock);

	return n;
}

static ssize_t iommu_map_store(struct kobject *kobj, struct kobj_attribute *attr,
			       const char *buf, size_t count)
{
	struct iommu_domain *domain;
	struct smmutest_map_entry *e;
	unsigned long long iova, pa, size;
	int ret;
	phys_addr_t pa_from_domain;

	ret = sscanf(buf, "%llx %llx %llx", &iova, &pa, &size);
	if (ret != 3)
		return -EINVAL;
	if (!size)
		return -EINVAL;
	if (!IS_ALIGNED(iova, PAGE_SIZE) ||
	    !IS_ALIGNED(pa, PAGE_SIZE) ||
	    !IS_ALIGNED(size, PAGE_SIZE))
		return -EINVAL;

	mutex_lock(&test_lock);
	domain = iommu_get_domain_for_dev(&test_pdev->dev);
	if (!domain) {
		mutex_unlock(&test_lock);
		return -ENODEV;
	}

	ret = iommu_map(domain, (dma_addr_t)iova, (phys_addr_t)pa, (size_t)size,
			IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
	if (ret) {
		mutex_unlock(&test_lock);
		return ret;
	}

	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e) {
		iommu_unmap(domain, (dma_addr_t)iova, (size_t)size);
		mutex_unlock(&test_lock);
		return -ENOMEM;
	}
	e->iova = (dma_addr_t)iova;
	e->pa = (phys_addr_t)pa;
	e->size = (size_t)size;
	list_add_tail(&e->node, &map_list);

	pa_from_domain = iommu_iova_to_phys(domain, e->iova);
	pr_info(DRV_NAME ": iommu_map iova=0x%llx pa=0x%llx size=0x%zx iova_to_phys=0x%llx\n",
		(unsigned long long)e->iova,
		(unsigned long long)e->pa,
		e->size,
		(unsigned long long)pa_from_domain);

	mutex_unlock(&test_lock);
	return count;
}

static ssize_t iommu_unmap_store(struct kobject *kobj, struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	struct iommu_domain *domain;
	struct smmutest_map_entry *e, *tmp;
	unsigned long long iova, size;
	size_t unmapped;
	phys_addr_t pa_after;
	int ret;

	ret = sscanf(buf, "%llx %llx", &iova, &size);
	if (ret != 2)
		return -EINVAL;
	if (!size || !IS_ALIGNED(iova, PAGE_SIZE) || !IS_ALIGNED(size, PAGE_SIZE))
		return -EINVAL;

	mutex_lock(&test_lock);
	domain = iommu_get_domain_for_dev(&test_pdev->dev);
	if (!domain) {
		mutex_unlock(&test_lock);
		return -ENODEV;
	}

	unmapped = iommu_unmap(domain, (dma_addr_t)iova, (size_t)size);
	pa_after = iommu_iova_to_phys(domain, (dma_addr_t)iova);
	pr_info(DRV_NAME ": iommu_unmap iova=0x%llx size=0x%llx unmapped=0x%zx post_unmap_pa=0x%llx\n",
		iova, size, unmapped, (unsigned long long)pa_after);

	list_for_each_entry_safe(e, tmp, &map_list, node) {
		if (e->iova == (dma_addr_t)iova && e->size == (size_t)size) {
			list_del(&e->node);
			kfree(e);
			break;
		}
	}

	mutex_unlock(&test_lock);
	return count;
}

static ssize_t run_contig_hint_test_store(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  const char *buf, size_t count)
{
	unsigned int val;
	int ret;

	if (kstrtouint(buf, 0, &val) || val != 1)
		return -EINVAL;

	mutex_lock(&test_lock);
	ret = run_contig_hint_tests_locked();
	mutex_unlock(&test_lock);

	return ret ? ret : count;
}

/*
 * atos — Address Translation Operations (software page table walk)
 *
 * Write an IOVA (hex) to perform iommu_iova_to_phys() and store the result.
 * Read to retrieve the last queried IOVA and its physical address.
 *
 * Usage:
 *   echo "0x1000" > /sys/kernel/smmutest/atos
 *   cat /sys/kernel/smmutest/atos
 *   # → iova=0x1000 pa=0xc0000000
 *
 * Note: requires an active iommu_map for the IOVA to return a non-zero PA.
 */
static ssize_t atos_store(struct kobject *kobj, struct kobj_attribute *attr,
			  const char *buf, size_t count)
{
	struct iommu_domain *domain;
	unsigned long long iova;
	phys_addr_t pa;

	if (sscanf(buf, "%llx", &iova) != 1)
		return -EINVAL;

	mutex_lock(&test_lock);
	domain = iommu_get_domain_for_dev(&test_pdev->dev);
	if (!domain) {
		mutex_unlock(&test_lock);
		return -ENODEV;
	}

	pa = iommu_iova_to_phys(domain, (dma_addr_t)iova);
	atos_last_iova = (dma_addr_t)iova;
	atos_last_pa = pa;

	pr_info(DRV_NAME ": atos iova=0x%llx -> pa=0x%llx%s\n",
		(unsigned long long)iova,
		(unsigned long long)pa,
		pa ? "" : " (not mapped)");

	mutex_unlock(&test_lock);
	return count;
}

static ssize_t atos_show(struct kobject *kobj, struct kobj_attribute *attr,
			 char *buf)
{
	ssize_t n;

	mutex_lock(&test_lock);
	n = scnprintf(buf, PAGE_SIZE, "iova=0x%llx pa=0x%llx\n",
		      (unsigned long long)atos_last_iova,
		      (unsigned long long)atos_last_pa);
	mutex_unlock(&test_lock);
	return n;
}

/*
 * profiling — iommu_map/iommu_unmap per-call latency measurement
 *
 * Write the number of iterations (decimal) to run the profiling loop.
 * Read to retrieve the last profiling results as a latency table.
 *
 * Usage:
 *   echo "1000" > /sys/kernel/smmutest/profiling
 *   cat /sys/kernel/smmutest/profiling
 *
 * The test uses fixed IOVAs (non-overlapping, page-aligned):
 *   4K  → IOVA 0x01000000, PA 0xc0000000
 *   64K → IOVA 0x02000000, PA 0xc1000000
 *   2M  → IOVA 0x04000000, PA 0xc2000000
 */
static ssize_t profiling_store(struct kobject *kobj, struct kobj_attribute *attr,
			       const char *buf, size_t count)
{
	static const dma_addr_t prof_iovas[PROF_SIZES] = {
		0x01000000ULL, 0x02000000ULL, 0x04000000ULL
	};
	static const phys_addr_t prof_pas[PROF_SIZES] = {
		0xc0000000ULL, 0xc1000000ULL, 0xc2000000ULL
	};
	struct iommu_domain *domain;
	unsigned int nr_iters;
	int s, i, ret;

	if (kstrtouint(buf, 0, &nr_iters) || !nr_iters || nr_iters > 100000)
		return -EINVAL;

	mutex_lock(&test_lock);
	domain = iommu_get_domain_for_dev(&test_pdev->dev);
	if (!domain) {
		mutex_unlock(&test_lock);
		return -ENODEV;
	}

	memset(&last_prof, 0, sizeof(last_prof));
	last_prof.nr_iters = nr_iters;

	for (s = 0; s < PROF_SIZES; s++) {
		u64 map_total_ns = 0, unmap_total_ns = 0;
		dma_addr_t iova = prof_iovas[s];
		phys_addr_t pa = prof_pas[s];
		size_t size = prof_sizes[s];
		int completed = 0;

		for (i = 0; i < nr_iters; i++) {
			ktime_t t;
			size_t unmapped;

			t = ktime_get();
			ret = iommu_map(domain, iova, pa, size,
					IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
			map_total_ns += ktime_to_ns(ktime_sub(ktime_get(), t));

			if (ret) {
				pr_err(DRV_NAME ": profiling[%s] iommu_map failed at iter %d: %d\n",
				       prof_size_names[s], i, ret);
				break;
			}

			t = ktime_get();
			unmapped = iommu_unmap(domain, iova, size);
			unmap_total_ns += ktime_to_ns(ktime_sub(ktime_get(), t));

			if (unmapped != size) {
				pr_err(DRV_NAME ": profiling[%s] iommu_unmap short at iter %d: %zu/%zu\n",
				       prof_size_names[s], i, unmapped, size);
				break;
			}
			completed++;
		}

		if (completed > 0) {
			last_prof.map_avg_ns[s] = div_u64(map_total_ns, completed);
			last_prof.unmap_avg_ns[s] = div_u64(unmap_total_ns, completed);
		}

		pr_info(DRV_NAME ": profiling[%s] %d iters: map_avg=%llu ns, unmap_avg=%llu ns\n",
			prof_size_names[s], completed,
			last_prof.map_avg_ns[s], last_prof.unmap_avg_ns[s]);
	}

	last_prof.valid = true;
	mutex_unlock(&test_lock);
	return count;
}

static ssize_t profiling_show(struct kobject *kobj, struct kobj_attribute *attr,
			      char *buf)
{
	ssize_t used = 0;
	int s;

	mutex_lock(&test_lock);

	if (!last_prof.valid) {
		used = scnprintf(buf, PAGE_SIZE,
				 "no profiling data — write N to run N iterations\n");
		goto out;
	}

	used += scnprintf(buf + used, PAGE_SIZE - used,
			  "(average over %u iterations)\n", last_prof.nr_iters);
	used += scnprintf(buf + used, PAGE_SIZE - used,
			  "    %4s   %12s   %12s\n",
			  "size", "iommu_map", "iommu_unmap");

	for (s = 0; s < PROF_SIZES; s++) {
		u32 map_us_rem, unmap_us_rem;
		u64 map_us = div_u64_rem(last_prof.map_avg_ns[s], 1000, &map_us_rem);
		u64 unmap_us = div_u64_rem(last_prof.unmap_avg_ns[s], 1000, &unmap_us_rem);

		used += scnprintf(buf + used, PAGE_SIZE - used,
				  "    %4s   %8llu.%03u us   %8llu.%03u us\n",
				  prof_size_names[s],
				  map_us, map_us_rem,
				  unmap_us, unmap_us_rem);
	}
out:
	mutex_unlock(&test_lock);
	return used;
}

static struct kobj_attribute dump_attr = __ATTR_WO(dump);
static struct kobj_attribute state_attr = __ATTR_RO(state);
static struct kobj_attribute iommu_map_attr = __ATTR_WO(iommu_map);
static struct kobj_attribute iommu_unmap_attr = __ATTR_WO(iommu_unmap);
static struct kobj_attribute run_contig_hint_test_attr = __ATTR_WO(run_contig_hint_test);
/*
 * dma_profiling — DMA API map/unmap latency measurement
 *
 * Unlike the 'profiling' entry (which uses iommu_map/iommu_unmap directly),
 * this entry uses dma_map_single/dma_unmap_single — the full DMA API path.
 *
 * Key difference: dma_unmap_single() goes through iommu_dma_unmap_page()
 * which uses the flush queue (IOMMU_DOMAIN_DMA_FQ) to defer TLB sync.
 * With DMA_FQ enabled in arm_smmu_def_domain_type(), the unmap latency
 * should be significantly lower than with iommu_unmap() which always syncs.
 *
 * Usage:
 *   echo "1000" > /sys/kernel/smmutest/dma_profiling
 *   cat /sys/kernel/smmutest/dma_profiling
 */
static ssize_t dma_profiling_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	unsigned int nr_iters;
	int s, i;

	if (kstrtouint(buf, 0, &nr_iters) || !nr_iters || nr_iters > 100000)
		return -EINVAL;

	mutex_lock(&test_lock);

	memset(&last_dma_prof, 0, sizeof(last_dma_prof));
	last_dma_prof.nr_iters = nr_iters;

	for (s = 0; s < PROF_SIZES; s++) {
		u64 map_total_ns = 0, unmap_total_ns = 0;
		size_t size = prof_sizes[s];
		int completed = 0;
		void *buf_virt;

		buf_virt = kmalloc(size, GFP_KERNEL);
		if (!buf_virt) {
			pr_err(DRV_NAME ": dma_profiling[%s] kmalloc failed\n",
			       prof_size_names[s]);
			break;
		}

		for (i = 0; i < nr_iters; i++) {
			ktime_t t;
			dma_addr_t dma_addr;

			t = ktime_get();
			dma_addr = dma_map_single(&test_pdev->dev, buf_virt,
						  size, DMA_TO_DEVICE);
			map_total_ns += ktime_to_ns(ktime_sub(ktime_get(), t));

			if (dma_mapping_error(&test_pdev->dev, dma_addr)) {
				pr_err(DRV_NAME ": dma_profiling[%s] dma_map_single failed at iter %d\n",
				       prof_size_names[s], i);
				break;
			}

			t = ktime_get();
			dma_unmap_single(&test_pdev->dev, dma_addr, size,
					 DMA_TO_DEVICE);
			unmap_total_ns += ktime_to_ns(ktime_sub(ktime_get(), t));

			completed++;
		}

		kfree(buf_virt);

		if (completed > 0) {
			last_dma_prof.map_avg_ns[s] = div_u64(map_total_ns, completed);
			last_dma_prof.unmap_avg_ns[s] = div_u64(unmap_total_ns, completed);
		}

		pr_info(DRV_NAME ": dma_profiling[%s] %d iters: map_avg=%llu ns, unmap_avg=%llu ns\n",
			prof_size_names[s], completed,
			last_dma_prof.map_avg_ns[s], last_dma_prof.unmap_avg_ns[s]);
	}

	last_dma_prof.valid = true;
	mutex_unlock(&test_lock);
	return count;
}

static ssize_t dma_profiling_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	ssize_t used = 0;
	int s;

	mutex_lock(&test_lock);

	if (!last_dma_prof.valid) {
		used = scnprintf(buf, PAGE_SIZE,
				 "no dma_profiling data — write N to run N iterations\n");
		goto out;
	}

	used += scnprintf(buf + used, PAGE_SIZE - used,
			  "(DMA API, average over %u iterations)\n",
			  last_dma_prof.nr_iters);
	used += scnprintf(buf + used, PAGE_SIZE - used,
			  "    %4s   %12s   %12s\n",
			  "size", "dma_map", "dma_unmap");

	for (s = 0; s < PROF_SIZES; s++) {
		u32 map_us_rem, unmap_us_rem;
		u64 map_us = div_u64_rem(last_dma_prof.map_avg_ns[s], 1000, &map_us_rem);
		u64 unmap_us = div_u64_rem(last_dma_prof.unmap_avg_ns[s], 1000, &unmap_us_rem);

		used += scnprintf(buf + used, PAGE_SIZE - used,
				  "    %4s   %8llu.%03u us   %8llu.%03u us\n",
				  prof_size_names[s],
				  map_us, map_us_rem,
				  unmap_us, unmap_us_rem);
	}
out:
	mutex_unlock(&test_lock);
	return used;
}

/*
 * profiling_compare — comprehensive per-function DMA latency breakdown
 *
 * Ports Dusa's ktime instrumentation approach (from fastmap-kpi-regression)
 * to the upstream kernel. Measures per-function latency across the full
 * DMA API stack: dma_map, IOVA alloc, iommu_map, alloc_pgt, install_table,
 * iommu_unmap, tlb_sync, free_iova.
 *
 * Usage:
 *   echo "1000" > /sys/kernel/smmutest/profiling_compare
 *   cat /sys/kernel/smmutest/profiling_compare
 *
 * Output shows per-function avg latency for 4K DMA map/unmap, enabling
 * side-by-side comparison of strict vs DMA_FQ vs fastmap modes.
 */
static struct iommu_prof_stats last_compare_stats;
static u32 last_compare_iters;

static ssize_t profiling_compare_store(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	unsigned int nr_iters;
	int i;
	void *buf_virt;

	if (kstrtouint(buf, 0, &nr_iters) || !nr_iters || nr_iters > 100000)
		return -EINVAL;

	buf_virt = kmalloc(SZ_4K, GFP_KERNEL);
	if (!buf_virt)
		return -ENOMEM;

	mutex_lock(&test_lock);

	/* Reset and enable profiling */
	memset(&iommu_prof_stats, 0, sizeof(iommu_prof_stats));
	iommu_prof_enabled = true;

	for (i = 0; i < nr_iters; i++) {
		dma_addr_t dma_addr;

		dma_addr = dma_map_single(&test_pdev->dev, buf_virt,
					  SZ_4K, DMA_TO_DEVICE);
		if (dma_mapping_error(&test_pdev->dev, dma_addr)) {
			pr_err(DRV_NAME ": profiling_compare dma_map_single failed at iter %d\n", i);
			break;
		}
		dma_unmap_single(&test_pdev->dev, dma_addr, SZ_4K, DMA_TO_DEVICE);
	}

	iommu_prof_enabled = false;
	last_compare_stats = iommu_prof_stats;
	last_compare_iters = i;

	mutex_unlock(&test_lock);
	kfree(buf_virt);
	return count;
}

static ssize_t profiling_compare_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	ssize_t used = 0;
	u32 n;

	mutex_lock(&test_lock);
	n = last_compare_iters;

	if (!n) {
		used = scnprintf(buf, PAGE_SIZE,
				 "no data — write N to run N iterations\n");
		goto out;
	}

#define PSTAT(label, field) do {					\
	const struct iommu_func_stats *_s = &last_compare_stats.field;	\
	u32 _avg_rem;							\
	u64 _avg_ns = _s->count ? div_u64(_s->total_ns, _s->count) : 0;\
	u64 _avg_us = div_u64_rem(_avg_ns, 1000, &_avg_rem);		\
	used += scnprintf(buf + used, PAGE_SIZE - used,			\
		"  %-32s Total: %llu ns  Count: %llu  Avg: %llu.%03u us\n",\
		label, _s->total_ns, _s->count, _avg_us, _avg_rem);	\
} while (0)

	used += scnprintf(buf + used, PAGE_SIZE - used,
			  "profiling_compare: 4K DMA map/unmap, %u iterations\n"
			  "  (iommu_prof_enabled instrumentation)\n"
			  "  (for full histogram: cat /sys/kernel/debug/iommu/profiling/stats)\n\n",
			  n);

	used += scnprintf(buf + used, PAGE_SIZE - used,
			  "--- MAP path ---\n");
	PSTAT("dma_map_phys (top):",      dma_map);
	PSTAT("__iommu_dma_map (inner):", __dma_map);
	PSTAT("alloc_iova:",              alloc_iova);
	PSTAT("iommu_map:",               iommu_map);
	PSTAT("iommu_sync_map:",          iommu_sync_map);
	PSTAT("alloc_pgt:",               alloc_pgt);
	PSTAT("install_table:",           install_table);

	used += scnprintf(buf + used, PAGE_SIZE - used,
			  "\n--- UNMAP path ---\n");
	PSTAT("dma_unmap_phys (top):",      dma_unmap);
	PSTAT("__iommu_dma_unmap (inner):", __dma_unmap);
	PSTAT("iommu_unmap_fast:",          iommu_unmap);
	PSTAT("iommu_iotlb_sync (TLB):",    tlb_sync);
	PSTAT("free_iova:",                 free_iova);
	PSTAT("iova_to_phys:",              iova_to_phys);
#undef PSTAT

out:
	mutex_unlock(&test_lock);
	return used;
}

static struct kobj_attribute atos_attr = __ATTR_RW(atos);
static struct kobj_attribute profiling_attr = __ATTR_RW(profiling);
static struct kobj_attribute dma_profiling_attr = __ATTR_RW(dma_profiling);
static struct kobj_attribute profiling_compare_attr = __ATTR_RW(profiling_compare);

static struct attribute *smmu_attrs[] = {
	&dump_attr.attr,
	&iommu_map_attr.attr,
	&iommu_unmap_attr.attr,
	&run_contig_hint_test_attr.attr,
	&state_attr.attr,
	&atos_attr.attr,
	&profiling_attr.attr,
	&dma_profiling_attr.attr,
	&profiling_compare_attr.attr,
	NULL,
};

static const struct attribute_group smmu_attr_group = {
	.attrs = smmu_attrs,
};

static int __init smmutest_init(void)
{
	u16 domain;
	u8 bus, dev, fn;
	int ret;

	ret = parse_bdf(bdf, &domain, &bus, &dev, &fn);
	if (ret) {
		pr_err(DRV_NAME ": invalid bdf '%s'\n", bdf);
		return ret;
	}

	test_pdev = pci_get_domain_bus_and_slot(domain, bus, PCI_DEVFN(dev, fn));
	if (!test_pdev) {
		pr_err(DRV_NAME ": PCI device %s not found\n", bdf);
		return -ENODEV;
	}

	ret = pci_enable_device(test_pdev);
	if (ret) {
		pr_err(DRV_NAME ": pci_enable_device failed: %d\n", ret);
		goto err_put;
	}

	pci_set_master(test_pdev);

	ret = dma_set_mask_and_coherent(&test_pdev->dev, DMA_BIT_MASK(64));
	if (ret) {
		ret = dma_set_mask_and_coherent(&test_pdev->dev, DMA_BIT_MASK(32));
		if (ret) {
			pr_err(DRV_NAME ": failed to set DMA mask\n");
			goto err_disable;
		}
	}

	pr_info(DRV_NAME ": probing %s vendor=0x%04x device=0x%04x\n",
		pci_name(test_pdev), test_pdev->vendor, test_pdev->device);
	report_ats(test_pdev);

	smmu_kobj = kobject_create_and_add(DRV_NAME, kernel_kobj);
	if (!smmu_kobj) {
		ret = -ENOMEM;
		goto err_disable;
	}

	ret = sysfs_create_group(smmu_kobj, &smmu_attr_group);
	if (ret)
		goto err_kobj;

	pr_info(DRV_NAME ": sysfs at /sys/kernel/%s/ (iommu_map, iommu_unmap, dump, run_contig_hint_test, state)\n",
		DRV_NAME);

	if (run_contig_hint_test_on_init) {
		mutex_lock(&test_lock);
		run_contig_hint_tests_locked();
		mutex_unlock(&test_lock);
	}

	return 0;

err_kobj:
	kobject_put(smmu_kobj);
	smmu_kobj = NULL;
err_disable:
	pci_disable_device(test_pdev);
err_put:
	pci_dev_put(test_pdev);
	test_pdev = NULL;
	return ret;
}

static void __exit smmutest_exit(void)
{
	struct iommu_domain *domain;
	struct smmutest_map_entry *e, *tmp;

	if (smmu_kobj) {
		sysfs_remove_group(smmu_kobj, &smmu_attr_group);
		kobject_put(smmu_kobj);
		smmu_kobj = NULL;
	}

	mutex_lock(&test_lock);
	domain = test_pdev ? iommu_get_domain_for_dev(&test_pdev->dev) : NULL;
	list_for_each_entry_safe(e, tmp, &map_list, node) {
		if (domain) {
			size_t unmapped = iommu_unmap(domain, e->iova, e->size);
			pr_info(DRV_NAME ": iommu unmap(cleanup) iova=0x%llx size=0x%zx unmapped=0x%zx\n",
				(unsigned long long)e->iova, e->size, unmapped);
		}
		list_del(&e->node);
		kfree(e);
	}
	mutex_unlock(&test_lock);

	if (test_pdev) {
		pci_disable_device(test_pdev);
		pci_dev_put(test_pdev);
		test_pdev = NULL;
	}
	pr_info(DRV_NAME ": unloaded\n");
}

module_init(smmutest_init);
module_exit(smmutest_exit);

MODULE_DESCRIPTION("SMMUv3 iommu_map/iommu_unmap test module with dump + contig-hint test");
MODULE_LICENSE("GPL");
