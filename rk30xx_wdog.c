/*-
 * Copyright (c) 2013 Ganbold Tsagaankhuu <ganbold@gmail.com>
 * Copyright (c) 2013 Oleksandr Tymoshenko <gonzo@freebsd.org>
 * All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/watchdog.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>

#include <dev/fdt/fdt_common.h>
#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <machine/bus.h>
#include <machine/cpufunc.h>
#include <machine/machdep.h>
#include <machine/fdt.h>

#include <arm/rockchip/rk30xx_wdog.h>

#define	RK30_WDT_READ(_sc, _r)		bus_read_4((_sc)->res, (_r))
#define	RK30_WDT_WRITE(_sc, _r, _v)	bus_write_4((_sc)->res, (_r), (_v))

#define	WDOG_CTRL		0x00
#define	WDOG_CTRL_EN		(1 << 0)
#define	WDOG_CTRL_RSP_MODE	(1 << 1)
#define	WDOG_CTRL_RST_PULSE	(4 << 2)
#define	WDOG_TORR		0x04
#define	WDOG_TORR_INTVL_SHIFT	0
#define	WDOG_CCVR		0x08
#define	WDOG_CRR		0x0c
#define	WDOG_STAT		0x10
#define	WDOG_EOI		0x14

struct rk30_wd_interval {
	uint64_t	milliseconds;
	unsigned int	value;
};

struct rk30_wd_interval wd_intervals[] = {
	{     2730,	 0 },
	{     5460,	 1 },
	{    10920,	 2 },
	{    21840,	 3 },
	{    43680,	 4 },
	{    87360,	 5 },
	{   174720,	 6 },
	{   349440,	 7 },
	{   698880,	 8 },
	{  1397760,	 9 },
	{  2795520,	10 },
	{  5591040,	11 },
	{ 11182080,	12 },
	{ 22364160,	13 },
	{ 44728320,	14 },
	{ 89456640,	15 },
	{ 0,		 0 } /* sentinel */
};

static struct rk30_wd_softc *rk30_wd_sc = NULL;

struct rk30_wd_softc {
	device_t		dev;
	struct resource *	res;
	struct mtx		mtx;
};

static void rk30_wd_watchdog_fn(void *private, u_int cmd, int *error);

static int
rk30_wd_probe(device_t dev)
{

	if (ofw_bus_is_compatible(dev, "rockchip,rk30xx-wdt")) {
		device_set_desc(dev, "Rockchip RK30XX Watchdog");
		return (BUS_PROBE_DEFAULT);
	}

	return (ENXIO);
}

static int
rk30_wd_attach(device_t dev)
{
	struct rk30_wd_softc *sc;
	int rid;

	if (rk30_wd_sc != NULL)
		return (ENXIO);

	sc = device_get_softc(dev);
	sc->dev = dev;

	rid = 0;
	sc->res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
	if (sc->res == NULL) {
		device_printf(dev, "could not allocate memory resource\n");
		return (ENXIO);
	}

	rk30_wd_sc = sc;
	mtx_init(&sc->mtx, "RK30XX Watchdog", "rk30_wd", MTX_DEF);
	EVENTHANDLER_REGISTER(watchdog_list, rk30_wd_watchdog_fn, sc, 0);

	return (0);
}

static void
rk30_wd_watchdog_fn(void *private, u_int cmd, int *error)
{
	struct rk30_wd_softc *sc;
	uint64_t ms;
	int i;

	sc = private;
	mtx_lock(&sc->mtx);

	cmd &= WD_INTERVAL;

	if (cmd > 0) {
		ms = ((uint64_t)1 << (cmd & WD_INTERVAL)) / 1000000;
		i = 0;
		while (wd_intervals[i].milliseconds && 
		    (ms > wd_intervals[i].milliseconds))
			i++;
		if (wd_intervals[i].milliseconds) {
			RK30_WDT_WRITE(sc, WDOG_TORR, 
			    wd_intervals[i].value << WDOG_TORR_INTVL_SHIFT);
			RK30_WDT_WRITE(sc, WDOG_CTRL, 
			    WDOG_CTRL_EN | WDOG_CTRL_RSP_MODE | 
			    WDOG_CTRL_RST_PULSE);
			RK30_WDT_WRITE(sc, WDOG_CRR, 0x76);
			*error = 0;
		}
		else {
			/*
			 * Can't arm
			 * disable watchdog as watchdog(9) requires
			 */
			device_printf(sc->dev,
			    "Can't arm, timeout is more than 16 sec\n");
			mtx_unlock(&sc->mtx);
			RK30_WDT_WRITE(sc, WDOG_CTRL, 0xa);
			return;
		}
	}
	else
		RK30_WDT_WRITE(sc, WDOG_CTRL, 0xa);

	mtx_unlock(&sc->mtx);
}

void
rk30_wd_watchdog_reset()
{

	if (rk30_wd_sc == NULL) {
		printf("Reset: watchdog device has not been initialized\n");
		return;
	}

	RK30_WDT_WRITE(rk30_wd_sc, WDOG_TORR, 
	    wd_intervals[0].value << WDOG_TORR_INTVL_SHIFT);
	RK30_WDT_WRITE(rk30_wd_sc, WDOG_CTRL, 
	    WDOG_CTRL_EN | WDOG_CTRL_RSP_MODE | WDOG_CTRL_RST_PULSE);

	for (;;)
		;

}

static device_method_t rk30_wd_methods[] = {
	DEVMETHOD(device_probe, rk30_wd_probe),
	DEVMETHOD(device_attach, rk30_wd_attach),

	DEVMETHOD_END
};

static driver_t rk30_wd_driver = {
	"rk30_wd",
	rk30_wd_methods,
	sizeof(struct rk30_wd_softc),
};
static devclass_t rk30_wd_devclass;

DRIVER_MODULE(rk30_wd, simplebus, rk30_wd_driver, rk30_wd_devclass, 0, 0);
