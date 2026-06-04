/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * zram_compr.h - per-process ZRAM compressed-size accounting
 *
 * Declares zram_get_compr_size_for_swp_entry(), the building block for
 * walking a process's swap PTEs and summing the true compressed footprint
 * each page occupies inside ZRAM.
 */

#ifndef _LINUX_ZRAM_COMPR_H
#define _LINUX_ZRAM_COMPR_H

#include <linux/swap.h>

#ifdef CONFIG_ZRAM
size_t zram_get_compr_size_for_swp_entry(swp_entry_t entry);
#else
static inline size_t zram_get_compr_size_for_swp_entry(swp_entry_t entry)
{
	return 0;
}
#endif /* CONFIG_ZRAM */

#endif /* _LINUX_ZRAM_COMPR_H */
