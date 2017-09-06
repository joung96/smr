/* $NetBSD: cgdvar.h,v 1.18 2015/09/06 06:00:59 dholland Exp $ */

/*-
 * Copyright (c) 2002 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Roland C. Dowdeswell.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _DEV_STLVAR_H
#define	_DEV_STLVAR_H

#include <sys/ioccom.h>
#include <dev/dkvar.h>
#include <dev/stl/stl_smr.h>


/* ioctl(2) code: used by CGDIOCSET and CGDIOCCLR */
struct stl_ioctl {
	const char	*si_disk;
	int		 si_flags;
	int		 si_unit;
	size_t		 si_size;
};

#ifdef _KERNEL
struct stl_softc {
	struct dk_softc		 sc_dksc;	/* generic disk interface */
	struct vnode		*sc_tvn;	/* target device's vnode */
	dev_t			 sc_tdev;	/* target device */
	char			*sc_tpath;	/* target device's path */
	void *			 sc_data;	/* emergency buffer */
	int			 sc_data_used;	/* Really lame, we'll change */
	size_t			 sc_tpathlen;	/* length of prior string */
	kmutex_t		 sc_lock;	/* our lock */

	// XXX these should be based on unit?
	unsigned int	 sc_group_bands;
	unsigned int	 sc_map_bands;
	unsigned int	 sc_n_groups;
	double	 	 sc_overprov;

	struct smr sc_smr_dev;
	struct volume *sc_smr_vol;
};
#endif /* _KERNEL */
void stlstrategy(struct buf *bp);
#define STLIOCSET	_IOWR('F', 61, struct stl_ioctl)
#define STLIOCCLR	_IOWR('F', 62, struct stl_ioctl)

#endif /* _DEV_STLVAR_H_ */
