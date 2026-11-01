/* Alioth adaptation: Apollo PCIe/MHI implementation, APSL 2.0.
 * GPIOs, PHY sequence, six supply resources/voltages, MSI and SID
 * checked against this K40 stock-device-tree.dtb. Opt-in aliothwifi=N. */
/* PCIe0 endpoint enumeration (alioth_wlan_pcie.c, from the Mi 10 port's umi_pcie.h).
 * The platform owns mappings and RPMh votes. */
#ifndef _ALIOTH_WLAN_PCIE_H_
#define _ALIOTH_WLAN_PCIE_H_
#include <stdbool.h>
#include <stdint.h>

struct alioth_wlan_pcie_io {
	void *context;
	uint32_t (*read32)(void *, uint32_t physical_address);
	void (*write32)(void *, uint32_t physical_address, uint32_t value);
	int (*power_on)(void *); /* Board-specific rail votes; no endpoint DMA. */
	void (*delay_us)(void *, unsigned);
	void (*report)(void *, const char *, uint32_t);
};
struct alioth_wlan_pcie {
	const struct alioth_wlan_pcie_io *io;
	bool failed, enumerated, dma_enabled;
	uint32_t msi_cap;
};
/* Explicit diagnostic only, on a zero-initialized host. Bus mastering stays
 * disabled. No SMMU, MSI or MHI context is installed by this component.
 */
int alioth_wlan_pcie_enumerate(struct alioth_wlan_pcie *, const struct alioth_wlan_pcie_io *);
/* Requires an owned, verified SMMU context for stream 0x1c01. No caller
 * may enable bus mastering before that protection is installed. */
int alioth_wlan_pcie_enable_dma(struct alioth_wlan_pcie *);
int alioth_wlan_pcie_prepare_radio(struct alioth_wlan_pcie *);
int alioth_wlan_pcie_setup_mhi_msi(struct alioth_wlan_pcie *, uint64_t address, uint32_t data);
/* Hold endpoint in reset before returning to loader. Rail/clock votes remain
 * until hardware reset; shared supplies are never disabled here. */
void alioth_wlan_pcie_quiesce(struct alioth_wlan_pcie *);
#endif
