/* $NetBSD: cgd.c,v 1.106 2015/11/28 21:06:30 mlelstv Exp $ */

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

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: stl.c,v 1.106 2015/11/28 21:06:30 mlelstv Exp $");

#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/errno.h>
#include <sys/buf.h>
#include <sys/bufq.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/pool.h>
#include <sys/ioctl.h>
#include <sys/device.h>
#include <sys/disk.h>
#include <sys/disklabel.h>
#include <sys/fcntl.h>
#include <sys/namei.h> /* for pathbuf */
#include <sys/vnode.h>
#include <sys/conf.h>
#include <sys/syslog.h>

#include <dev/stl/stlvar.h>

#include <dev/dkvar.h>

#include <miscfs/specfs/specdev.h> /* for v_rdev */

#include "ioconf.h"

// Template for disk driver from https://wiki.netbsd.org/users/mlelstv/disk-driver-template/#index4h2

/* Entry Point Functions */

static dev_type_open(stlopen);
static dev_type_close(stlclose);
static dev_type_read(stlread);
static dev_type_write(stlwrite);
static dev_type_ioctl(stlioctl);
//static dev_type_strategy(stlstrategy);
static dev_type_dump(stldump);
static dev_type_size(stlsize);
static dev_type_discard(stldiscard);


const struct bdevsw stl_bdevsw = {
	.d_open = stlopen,
	.d_close = stlclose,
	.d_strategy = stlstrategy,
	.d_ioctl = stlioctl,
	.d_dump = stldump,
	.d_psize = stlsize,
	.d_discard = stldiscard,
	.d_flag = D_DISK
};

const struct cdevsw stl_cdevsw = {
	.d_open = stlopen,
	.d_close = stlclose,
	.d_read = stlread,
	.d_write = stlwrite,
	.d_ioctl = stlioctl,
	.d_stop = nostop,
	.d_tty = notty,
	.d_poll = nopoll,
	.d_mmap = nommap,
	.d_kqfilter = nokqfilter,
	.d_discard = stldiscard,
	.d_flag = D_DISK
};

static void     stlminphys(struct buf *bp);
static int      stl_diskstart(device_t, struct buf *bp);
static void     stl_iosize(device_t, int *);
static int      stl_dumpblocks(device_t, void *, daddr_t, int);
static int      stl_lastclose(device_t);
static int      stl_discard(device_t, off_t, off_t);

static const struct dkdriver stldkdriver = {
    .d_open = stlopen,
    .d_close = stlclose,
    .d_strategy = stlstrategy,
    .d_iosize = stl_iosize,
    .d_minphys  = stlminphys,
    .d_diskstart = stl_diskstart,
    .d_dumpblocks = stl_dumpblocks,
    .d_lastclose = stl_lastclose,
    .d_discard = stl_discard
};

static int      stl_match(device_t, cfdata_t, void *);
static void     stl_attach(device_t, device_t, void *);
static int      stl_detach(device_t, int);

CFATTACH_DECL3_NEW(stl, sizeof(struct stl_softc),
    stl_match, stl_attach, stl_detach, NULL, NULL, NULL, DVF_DETACH_SHUTDOWN);
    extern struct cfdriver stl_cd;
extern struct   cfdriver stl_cd;

void     stlattach(int);
static struct stl_softc *stl_create(int);
static int      stl_destroy(device_t);
static int stl_ioctl_set(struct stl_softc *sc, void *data, struct lwp *l);
static int stl_ioctl_clr(struct stl_softc *sc, struct lwp *l);
static int stlinit(struct stl_softc *sc, const char *cpath, struct vnode *vp, struct lwp *l);

static void      stldone(struct buf *);


static struct stl_softc *
getstl_softc(dev_t dev)
{
        int     unit = DISKUNIT(dev);
        struct stl_softc *sc;

        sc = device_lookup_private(&stl_cd, unit);
        if (sc == NULL)
                sc = stl_create(unit);
        return sc;
}

void
stlattach(int num)
{
    int error;

    error = config_cfattach_attach(stl_cd.cd_name, &stl_ca);
    if (error) {
        aprint_error("%s: unable to register cfattach %d\n",
            stl_cd.cd_name, error);
        return;
    }
}


static struct stl_softc *
stl_create(int unit)
{
    cfdata_t cf;
    device_t dv;

    cf = malloc(sizeof(*cf), M_DEVBUF, M_WAITOK);
    cf->cf_name = stl_cd.cd_name;
    cf->cf_atname = stl_cd.cd_name;
    cf->cf_unit = unit;
    cf->cf_fstate = FSTATE_STAR;

    dv = config_attach_pseudo(cf);
    if (dv == NULL) {
        aprint_error("%s: failed to attach pseudo device\n",
            stl_cd.cd_name);
        free(cf, M_DEVBUF);
        return NULL;
    }

    return device_private(dv);
}

static int
stl_destroy(device_t dv)
{
    int error;
    cfdata_t cf;

    cf = device_cfdata(dv);
    error = config_detach(dv, DETACH_QUIET);
    if (error)
        return error;
    free(cf, M_DEVBUF);
    return 0;
}

static int
stlopen(dev_t dev, int flags, int fmt, struct lwp *l)
{
    struct stl_softc *sc = getstl_softc(dev);
    if (sc == NULL) {
	return ENXIO;
    }

    return dk_open(&sc->sc_dksc, dev, flags, fmt, l);
}

static int
stlclose(dev_t dev, int flags, int fmt, struct lwp *l)
{
    struct stl_softc *sc = getstl_softc(dev);
    int error;
    if (sc == NULL) {
	return ENXIO;
    }

    if ((error = dk_close(&sc->sc_dksc, dev, flags, fmt, l)) != 0) {
	return error;
    }

    if (!DK_ATTACHED(&sc->sc_dksc)) {
	    if ((error = stl_destroy(sc->sc_dksc.sc_dev)) != 0) {
		    aprint_error_dev(sc->sc_dksc.sc_dev,
			"unable to detach instance\n");
		    return error;
	    }
    }
    return 0;
}

static int
stlread(dev_t dev, struct uio *uio, int ioflag)
{
    return physio(stlstrategy, NULL, dev, B_READ, stlminphys, uio);
}

static int
stlwrite(dev_t dev, struct uio *uio, int ioflag)
{
    return physio(stlstrategy, NULL, dev, B_WRITE, stlminphys, uio);
}

static int
stlioctl(dev_t dev, u_long cmd, void *addr, int32_t flag, struct lwp *l)
{
    int error;
    struct stl_softc *sc = getstl_softc(dev);
    if (sc == NULL) {
	return ENXIO;
    }

    const int pmask = 1 << RAW_PART;

    switch (cmd) {
    case STLIOCSET:
	if (DK_ATTACHED(&sc->sc_dksc))
	    return EBUSY;
	return stl_ioctl_set(sc, addr, l);
    case STLIOCCLR:
	if (DK_BUSY(&sc->sc_dksc, pmask)) {
	    return EBUSY;
	}
	return stl_ioctl_clr(sc, l);
    default:
	error = dk_ioctl(&sc->sc_dksc, dev, cmd, addr, flag, l);
	if (error != EPASSTHROUGH)
	    error = 0;
    }

    return error;
}

void
stlstrategy(struct buf *bp)
{
    struct stl_softc *sc = getstl_softc(bp->b_dev);
    if (sc == NULL) {
	return;
    }

    dk_strategy(&sc->sc_dksc, bp);
}

static int
stldiscard(dev_t dev, off_t pos, off_t len)
{
    struct stl_softc *sc = getstl_softc(dev);
    if (sc == NULL) {
	return ENXIO;
    }

    return dk_discard(&sc->sc_dksc, dev, pos, len);
}

static int
stlsize(dev_t dev)
{
    struct stl_softc *sc = getstl_softc(dev);
    if (sc == NULL) {
	return ENXIO;
    }

    return dk_size(&sc->sc_dksc, dev);
}

static int
stldump(dev_t dev, daddr_t blkno, void *va, size_t size)
{
    struct stl_softc *sc = getstl_softc(dev);
    if (sc == NULL) {
	return ENXIO;
    }

    return dk_dump(&sc->sc_dksc, dev, blkno, va, size);
}

static void
stlminphys(struct buf *bp)
{
    struct stl_softc *sc = getstl_softc(bp->b_dev);
    if (sc == NULL) {
	return;
    }

    // XXX ROB
    //stl_iosize(sc->sc_dv, &bp->b_count);
    minphys(bp);
}

static void
stldone(struct buf *nbp)
{
    struct  buf *obp = nbp->b_private;
    struct  stl_softc *cs = getstl_softc(obp->b_dev);
    struct  dk_softc *dksc = &cs->sc_dksc;

    if (nbp->b_error != 0) {
	obp->b_error = nbp->b_error;
    }

    putiobuf(nbp);

    /* Request is complete for whatever reason */
    obp->b_resid = 0;
    if (obp->b_error != 0)
	obp->b_resid = obp->b_bcount;

    dk_done(dksc, obp);
    dk_start(dksc, NULL);
}


static int
stl_match(device_t self, cfdata_t cfdata, void *aux)
{
    return 1;
}

static void
stl_attach(device_t parent, device_t self, void *aux)
{
    struct stl_softc *sc = device_private(self);
    struct dk_softc *dksc = &sc->sc_dksc;

    dk_init(dksc, self, DKTYPE_STL);
    disk_init(&dksc->sc_dkdev, dksc->sc_xname, &stldkdriver);
}

static int
stl_detach(device_t self, int flags)
{
    struct stl_softc *sc = device_private(self);
    struct dk_softc *dksc = &sc->sc_dksc;

    dkwedge_delall(&dksc->sc_dkdev);

    dk_drain(dksc);
    bufq_free(dksc->sc_bufq);

    dk_detach(dksc);
    disk_detach(&dksc->sc_dkdev);
    disk_destroy(&dksc->sc_dkdev);

    return 0;
}

static int
stl_lastclose(device_t dv)
{
    // private cleanup

    return 0;
}

static int
stl_diskstart(device_t dv, struct buf *bp)
{
    // XXX return EAGAIN if controller busy ?
    struct stl_softc *sc = device_private(dv);
    //bp->b_iodone = stldone;


    struct buf *nbp = getiobuf(sc->sc_tvn, false);
    if (nbp == NULL)
    	return EAGAIN;

    // XXX is this whole bp always necessary?
    nbp->b_data = bp->b_data;
    nbp->b_flags = bp->b_flags;
    nbp->b_oflags = bp->b_oflags;
    nbp->b_cflags = bp->b_cflags;
    nbp->b_iodone = stldone;
    nbp->b_proc = bp->b_proc;
    nbp->b_blkno = bp->b_blkno;
    nbp->b_bcount = bp->b_bcount;
    nbp->b_private = bp;

    BIO_COPYPRIO(nbp, bp);

    // XXX is this enough to determine we should go through STL? how to handle initialization...
    if (sc->sc_smr_vol != NULL) {
        if ((nbp->b_flags & B_READ) == 0) {
            struct vnode *vp = nbp->b_vp;
            mutex_enter(vp->v_interlock);
            vp->v_numoutput++;
            mutex_exit(vp->v_interlock);
            return stl_write(sc->sc_smr_vol, nbp);
        }
        else {
            return stl_read(sc->sc_smr_vol, nbp);
        }
    }
    else {
        if ((nbp->b_flags & B_READ) == 0) {
            struct vnode *vp = nbp->b_vp;
            mutex_enter(vp->v_interlock);
            vp->v_numoutput++;
            mutex_exit(vp->v_interlock);
	}
        VOP_STRATEGY(sc->sc_tvn, nbp);
    }

    return 0;
}

static int
stl_dumpblocks(device_t dv, void *va, daddr_t blkno, int nblk)
{
    // issue polling I/O to dump a page
    return 0;
}

static void
stl_iosize(device_t dv, int *countp)
{
    // limit *countp as necessary
}

static int
stl_discard(device_t dv, off_t pos, off_t len)
{
    // issue request to discard bytes
    return 0;
}

static int
stl_ioctl_clr(struct stl_softc *sc, struct lwp *l)
{
        struct dk_softc *dksc = &sc->sc_dksc;

        if (!DK_ATTACHED(dksc))
                return ENXIO;

        stl_close(sc->sc_smr_vol);
        smr_close(&sc->sc_smr_dev);

        // officially uninitialize
        sc->sc_smr_vol = NULL;

        /* Delete all of our wedges. */
        dkwedge_delall(&dksc->sc_dkdev);

        /* Kill off any queued buffers. */
        dk_drain(dksc);
        bufq_free(dksc->sc_bufq);

	// XXX any other cleanup here?
        (void)vn_close(sc->sc_tvn, FREAD|FWRITE, l->l_cred);
        free(sc->sc_tpath, M_DEVBUF);
        dk_detach(dksc);
        disk_detach(&dksc->sc_dkdev);

        return 0;
}

static int
stl_ioctl_set(struct stl_softc *sc, void *data, struct lwp *l)
{
        struct   stl_ioctl *si = data;
        struct   vnode *vp;
        int      ret;

        const char *cp;
        struct pathbuf *pb;
        struct dk_softc *dksc = &sc->sc_dksc;

        cp = si->si_disk;

        ret = pathbuf_copyin(si->si_disk, &pb);
        if (ret != 0) {
                return ret;
        }
        ret = dk_lookup(pb, l, &vp);
        pathbuf_destroy(pb);
        if (ret != 0) {
                return ret;
        }

        if ((ret = stlinit(sc, cp, vp, l)) != 0)
		return ret;


        bufq_alloc(&dksc->sc_bufq, "fcfs", 0);

        /* Attach the disk. */
        dk_attach(dksc);
        disk_attach(&dksc->sc_dkdev);

        disk_set_info(dksc->sc_dev, &dksc->sc_dkdev, NULL);

        /* Discover wedges on this disk. */
        dkwedge_discover(&dksc->sc_dkdev);

        if ((ret = smr_open(sc->sc_tvn, &sc->sc_smr_dev))) {
            return ret;
	}
        sc->sc_smr_vol = stl_open(&sc->sc_smr_dev);

        // XXX need to clean up and stuff
        if (sc->sc_smr_vol == NULL) {
	    smr_close(&sc->sc_smr_dev);
            return EINVAL;
	}

        return 0;
}

static int
stlinit(struct stl_softc *sc, const char *cpath, struct vnode *vp,
    struct lwp *l)
{
    struct  disk_geom *dg;
    int     ret;
    char    *tmppath;
    uint64_t psize;
    unsigned secsize;
    struct dk_softc *dksc = &sc->sc_dksc;

        sc->sc_tvn = vp;

        sc->sc_tpath = NULL;

        tmppath = malloc(MAXPATHLEN, M_TEMP, M_WAITOK);
        ret = copyinstr(cpath, tmppath, MAXPATHLEN, &sc->sc_tpathlen);
        if (ret)
                goto bail;
        sc->sc_tpath = malloc(sc->sc_tpathlen, M_DEVBUF, M_WAITOK);
        memcpy(sc->sc_tpath, tmppath, sc->sc_tpathlen);

        sc->sc_tdev = vp->v_rdev;

        if ((ret = getdisksize(vp, &psize, &secsize)) != 0)
                goto bail;

        if (psize == 0) {
                ret = ENODEV;
                goto bail;
        }


        /*
         * XXX ROB here we should probe the underlying device.  If we
         *     are accessing a partition of type RAW_PART, then
         *     we should populate our initial geometry with the
         *     geometry that we discover from the device.
         */
        dg = &dksc->sc_dkdev.dk_geom;
        memset(dg, 0, sizeof(*dg));
        dg->dg_secperunit = psize;
        dg->dg_secsize = secsize;
        dg->dg_ntracks = 1;
        dg->dg_nsectors = 1024 * 1024 / dg->dg_secsize;
        dg->dg_ncylinders = dg->dg_secperunit / dg->dg_nsectors;

bail:
        free(tmppath, M_TEMP);
        if (ret && sc->sc_tpath)
                free(sc->sc_tpath, M_DEVBUF);
        return ret;
}

/*

Alternative when using separate IO thread

static void
stlstrategy(struct buf *bp)
{
    struct stl_softc *sc = getstl_softc(dev);
    if (sc == NULL) {
	return;
    }

    if (dk_strategy_defer(&sc->sc_dksc, bp))
        return;

    // wake up I/O thread
}

static int
stl_diskstart(device_t dv, struct buf *bp)
{
    // issue I/O for bp
}

static void
stldone(struct stl_softc *sc, struct buf *bp)
{
    struct dk_softc *dksc = &sc->sc_dksc;

    dk_done(dksc, bp);
    // wake up I/O thread
}

static void
stl_IOTHREAD(struct dk_softc *dksc)
{
    while (!shutting_down) {
        if (dk_strategy_pending(dksc))
            dk_start(dksc, NULL);
        // sleep
    }
}
*/

