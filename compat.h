/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Kernel-version shims.  Every LINUX_VERSION_CODE guard lives here so
 * version churn stays out of the filesystem code.
 */
#ifndef AUFSNG_COMPAT_H
#define AUFSNG_COMPAT_H

#include <linux/version.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(7, 1, 0)
#error "aufs-ng supports kernel 7.1 and later"
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 3, 0)
#define AUFSNG_CREATE_EXCL
#else
#define AUFSNG_CREATE_EXCL , bool excl
#endif

#endif /* AUFSNG_COMPAT_H */
