/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * PASID (Processor Address Space ID) is a PCIe concept for tagging
 * address spaces in DMA requests. When system-wide PASID allocation
 * is required by the underlying iommu driver (e.g. Intel VT-d), this
 * provides an interface for userspace to request ioasid alloc/free
 * for its assigned devices.
 *
 * Copyright (C) 2021 Intel Corporation.  All rights reserved.
 *     Author: Liu Yi L <yi.l.liu@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef _IOASID_H
#define _IOASID_H

#include <linux/types.h>
#include <linux/ioctl.h>
#include <linux/ioasid.h>

#define IOASID_API_VERSION	0


/* Kernel & User level defines for IOASID IOCTLs. */

#define IOASID_TYPE	('i')
#define IOASID_BASE	100

/* -------- IOCTLs for IOASID file descriptor (/dev/ioasid) -------- */

/**
 * IOASID_GET_API_VERSION - _IO(IOASID_TYPE, IOASID_BASE + 0)
 *
 * Report the version of the IOASID API.  This allows us to bump the entire
 * API version should we later need to add or change features in incompatible
 * ways.
 * Return: IOASID_API_VERSION
 * Availability: Always
 */
#define IOASID_GET_API_VERSION		_IO(IOASID_TYPE, IOASID_BASE + 0)

/**
 * IOASID_GET_INFO - _IOR(IOASID_TYPE, IOASID_BASE + 1, struct ioasid_info)
 *
 * Retrieve information about the IOASID object. Fills in provided
 * struct ioasid_info. Caller sets argsz.
 *
 * @argsz:	 user filled size of this data.
 * @flags:	 currently reserved for future extension. must set to 0.
 * @ioasid_bits: maximum supported PASID bits, 0 represents no PASID
 *		 support.

 * Availability: Always
 */
struct ioasid_info {
	__u32	argsz;
	__u32	flags;
	__u32	ioasid_bits;
};
#define IOASID_GET_INFO _IO(IOASID_TYPE, IOASID_BASE + 1)

/**
 * IOASID_REQUEST_ALLOC - _IOWR(IOASID_TYPE, IOASID_BASE + 2,
 *					struct ioasid_request)
 *
 * Alloc a PASID within @range. @range is [min, max], which means both
 * @min and @max are inclusive.
 * User space should provide min, max no more than the ioasid bits reports
 * in ioasid_info via IOASID_GET_INFO.
 *
 * @argsz: user filled size of this data.
 * @flags: currently reserved for future extension. must set to 0.
 * @range: allocated ioasid is expected in the range.
 *
 * returns: allocated ID on success, -errno on failure
 */
struct ioasid_alloc_request {
	__u32	argsz;
	__u32	flags;
	struct {
		__u32	min;
		__u32	max;
	} range;
};
#define IOASID_REQUEST_ALLOC	_IO(IOASID_TYPE, IOASID_BASE + 2)

/**
 * IOASID_REQUEST_FREE - _IOWR(IOASID_TYPE, IOASID_BASE + 3, int)
 *
 * Free a PASID.
 *
 * returns: 0 on success, -errno on failure
 */
#define IOASID_REQUEST_FREE	_IO(IOASID_TYPE, IOASID_BASE + 3)

#endif /* _IOASID_H */
