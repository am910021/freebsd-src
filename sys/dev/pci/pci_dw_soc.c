/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/linker_set.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/systm.h>

#include <machine/bus.h>

#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/ofw/ofw_pci.h>
#include <dev/ofw/ofwpci.h>
#include <dev/pci/pci_dw.h>
#include <dev/pci/pci_dw_soc.h>

SET_DECLARE(pci_dw_soc_set, const struct pci_dw_soc_ops);

void
pci_dw_soc_init(struct pci_dw_softc *sc)
{
	const struct pci_dw_soc_ops **ops;

	SET_FOREACH(ops, pci_dw_soc_set) {
		if (*ops == NULL || (*ops)->match == NULL)
			continue;
		if ((*ops)->match(sc)) {
			sc->soc_ops = *ops;
			return;
		}
	}
}

bool
pci_dw_soc_active(struct pci_dw_softc *sc)
{

	return (sc->soc_ops != NULL);
}

bool
pci_dw_soc_allow_dbi_ro(struct pci_dw_softc *sc)
{

	if (sc->soc_ops != NULL && sc->soc_ops->allow_dbi_ro != NULL)
		return (sc->soc_ops->allow_dbi_ro(sc));
	return (true);
}

bool
pci_dw_soc_allow_iatu(struct pci_dw_softc *sc)
{

	if (sc->soc_ops != NULL && sc->soc_ops->allow_iatu != NULL)
		return (sc->soc_ops->allow_iatu(sc));
	return (true);
}

bool
pci_dw_soc_mem_range_prefetch(struct pci_dw_softc *sc,
    const struct ofw_pci_range *range)
{

	if (sc->soc_ops != NULL && sc->soc_ops->mem_range_prefetch != NULL)
		return (sc->soc_ops->mem_range_prefetch(sc, range));
	return (false);
}

bool
pci_dw_soc_suppress_root_config(struct pci_dw_softc *sc)
{

	if (sc->soc_ops != NULL &&
	    sc->soc_ops->suppress_root_config != NULL)
		return (sc->soc_ops->suppress_root_config(sc));
	return (false);
}

bool
pci_dw_soc_force_unroll(struct pci_dw_softc *sc, int atu_rv)
{

	if (sc->soc_ops != NULL && sc->soc_ops->force_unroll != NULL)
		return (sc->soc_ops->force_unroll(sc, atu_rv));
	return (false);
}

bool
pci_dw_soc_force_atu_regions(struct pci_dw_softc *sc, bool unroll_mode,
    int atu_rid)
{

	if (sc->soc_ops != NULL && sc->soc_ops->force_atu_regions != NULL)
		return (sc->soc_ops->force_atu_regions(sc, unroll_mode,
		    atu_rid));
	return (false);
}

bool
pci_dw_soc_prepare_dbi_protect(struct pci_dw_softc *sc, bool protect)
{

	if (sc->soc_ops != NULL && sc->soc_ops->prepare_dbi_protect != NULL)
		return (sc->soc_ops->prepare_dbi_protect(sc, protect));
	return (true);
}

bool
pci_dw_soc_prepare_iatu_unroll(struct pci_dw_softc *sc)
{

	if (sc->soc_ops != NULL && sc->soc_ops->prepare_iatu_unroll != NULL)
		return (sc->soc_ops->prepare_iatu_unroll(sc));
	return (true);
}

void
pci_dw_soc_root_config_done(struct pci_dw_softc *sc)
{

	if (sc->soc_ops != NULL && sc->soc_ops->root_config_done != NULL)
		sc->soc_ops->root_config_done(sc);
}

void
pci_dw_soc_fini(struct pci_dw_softc *sc)
{

	if (sc->soc_ops != NULL && sc->soc_ops->fini != NULL)
		sc->soc_ops->fini(sc);
	sc->soc_ops = NULL;
}
