/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Kernel-version compatibility shims for aufs-ng.
 *
 * aufs-ng builds as an external module against multiple kernel
 * versions; every API difference it has to absorb is centralized here
 * as a LINUX_VERSION_CODE guard, so that version churn never spreads
 * into the filesystem code itself.  Guards are only added when a real
 * kernel release changes an interface aufs-ng uses - this file is
 * expected to start out nearly empty.
 */
#ifndef AUFSNG_COMPAT_H
#define AUFSNG_COMPAT_H

#include <linux/version.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(7, 1, 0)
#error "aufs-ng supports kernel 7.1 and later"
#endif

#endif /* AUFSNG_COMPAT_H */
