/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#ifndef _PCI_DW_SOC_H_
#define	_PCI_DW_SOC_H_

#include <sys/linker_set.h>

struct ofw_pci_range;
struct pci_dw_softc;

struct pci_dw_soc_ops {
	bool	(*match)(struct pci_dw_softc *);
	bool	(*allow_dbi_ro)(struct pci_dw_softc *);
	bool	(*allow_iatu)(struct pci_dw_softc *);
	bool	(*mem_range_prefetch)(struct pci_dw_softc *,
			    const struct ofw_pci_range *);
	bool	(*suppress_root_config)(struct pci_dw_softc *);
	bool	(*force_unroll)(struct pci_dw_softc *, int);
	bool	(*force_atu_regions)(struct pci_dw_softc *, bool, int);
	bool	(*prepare_dbi_protect)(struct pci_dw_softc *, bool);
	bool	(*prepare_iatu_unroll)(struct pci_dw_softc *);
	void	(*root_config_done)(struct pci_dw_softc *);
	void	(*fini)(struct pci_dw_softc *);
};

#define	PCI_DW_SOC_REGISTER(name, ops)					\
	DATA_SET(pci_dw_soc_set, ops)

void	pci_dw_soc_init(struct pci_dw_softc *);
bool	pci_dw_soc_active(struct pci_dw_softc *);
bool	pci_dw_soc_allow_dbi_ro(struct pci_dw_softc *);
bool	pci_dw_soc_allow_iatu(struct pci_dw_softc *);
bool	pci_dw_soc_mem_range_prefetch(struct pci_dw_softc *,
		    const struct ofw_pci_range *);
bool	pci_dw_soc_suppress_root_config(struct pci_dw_softc *);
bool	pci_dw_soc_force_unroll(struct pci_dw_softc *, int);
bool	pci_dw_soc_force_atu_regions(struct pci_dw_softc *, bool, int);
bool	pci_dw_soc_prepare_dbi_protect(struct pci_dw_softc *, bool);
bool	pci_dw_soc_prepare_iatu_unroll(struct pci_dw_softc *);
void	pci_dw_soc_root_config_done(struct pci_dw_softc *);
void	pci_dw_soc_fini(struct pci_dw_softc *);

#endif
