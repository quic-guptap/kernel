/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * DMABUF Heaps Userspace API
 *
 * Copyright (C) 2011 Google, Inc.
 * Copyright (C) 2019 Linaro Ltd.
 */
#ifndef _UAPI_LINUX_DMABUF_POOL_H
#define _UAPI_LINUX_DMABUF_POOL_H

#include <linux/ioctl.h>
#include <linux/types.h>

/**
 * DOC: DMABUF Heaps Userspace API
 */

/* Valid FD_FLAGS are O_CLOEXEC, O_RDONLY, O_WRONLY, O_RDWR */
#define DMA_HEAP_VALID_FD_FLAGS (O_CLOEXEC | O_ACCMODE)

/**
 * DOC: DMA Heap Allocation Hints
 *
 * The heap_flags field in struct dma_heap_allocation_data carries advisory
 * hints that influence how the heap satisfies the allocation request.
 *
 * DMA_HEAP_ALLOC_HUGEPAGE:
 *   Request that the heap prefer higher-order compound pages for this
 *   allocation. Heaps that support this hint will attempt to use the largest
 *   available page order (e.g. order-8 = 1MB on most systems) before falling
 *   back to smaller orders. This reduces IOMMU TLB pressure for large buffers.
 *
 *   This flag mirrors the effect of madvise(MADV_HUGEPAGE) for DMA memory.
 *   The hint is advisory: the heap may use smaller pages if higher-order
 *   pages are unavailable.
 *
 * DMA_HEAP_ALLOC_NOHUGEPAGE:
 *   Request that the heap use only base-page (order-0) allocations. Heaps
 *   that support this hint will skip high-order allocation attempts, reducing
 *   allocation latency for small or latency-sensitive buffers.
 *
 *   This flag mirrors the effect of madvise(MADV_NOHUGEPAGE) for DMA memory.
 *
 * These flags are mutually exclusive. Passing both returns -EINVAL.
 * Heaps that do not implement hugepage awareness silently ignore these flags.
 */
#define DMA_HEAP_ALLOC_HUGEPAGE		(1ULL << 0)
#define DMA_HEAP_ALLOC_NOHUGEPAGE	(1ULL << 1)

#define DMA_HEAP_VALID_HEAP_FLAGS \
	(DMA_HEAP_ALLOC_HUGEPAGE | DMA_HEAP_ALLOC_NOHUGEPAGE)

/**
 * struct dma_heap_allocation_data - metadata passed from userspace for
 *                                      allocations
 * @len:		size of the allocation
 * @fd:			will be populated with a fd which provides the
 *			handle to the allocated dma-buf
 * @fd_flags:		file descriptor flags used when allocating
 * @heap_flags:		flags passed to heap
 *
 * Provided by userspace as an argument to the ioctl
 */
struct dma_heap_allocation_data {
	__u64 len;
	__u32 fd;
	__u32 fd_flags;
	__u64 heap_flags;
};

#define DMA_HEAP_IOC_MAGIC		'H'

/**
 * DOC: DMA_HEAP_IOCTL_ALLOC - allocate memory from pool
 *
 * Takes a dma_heap_allocation_data struct and returns it with the fd field
 * populated with the dmabuf handle of the allocation.
 */
#define DMA_HEAP_IOCTL_ALLOC	_IOWR(DMA_HEAP_IOC_MAGIC, 0x0,\
				      struct dma_heap_allocation_data)

#endif /* _UAPI_LINUX_DMABUF_POOL_H */
