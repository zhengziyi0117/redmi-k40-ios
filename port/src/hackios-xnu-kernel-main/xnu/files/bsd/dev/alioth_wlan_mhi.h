/* Alioth adaptation: Apollo PCIe/MHI implementation, APSL 2.0.
 * GPIOs, PHY sequence, six supply resources/voltages, MSI and SID
 * checked against this K40 stock-device-tree.dtb. Opt-in aliothwifi=N. */
/* QCA6390 MHI boot transport (alioth_wlan_mhi.c, from the Mi 10 port's umi_mhi.h).
 * The PCIe owner supplies MMIO and coherent DMA.
 * The caller must serialize calls and keep the endpoint powered throughout each call.
 */
#ifndef _ALIOTH_WLAN_MHI_H_
#define _ALIOTH_WLAN_MHI_H_
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct alioth_wlan_mhi_dma {
	void *bytes;
	uint64_t address;                 /* SMMU IOVA, never a CPU physical address */
	size_t size;
};

struct alioth_wlan_mhi_io {
	void *context;
	uint32_t bar_size;
	uint32_t (*read32)(void *, uint32_t);
	void (*write32)(void *, uint32_t, uint32_t);
	/* Page-aligned coherent allocations, owned until endpoint shutdown/reset.
	 * Failure returns an errno; partial allocations remain owned by the platform.
	 */
	int (*allocate)(void *, size_t, struct alioth_wlan_mhi_dma *);
	void (*sync_for_device)(void *, const struct alioth_wlan_mhi_dma *);
	void (*delay_us)(void *, unsigned);
	void (*report)(void *, const char *, uint32_t);
	void (*qrtr_packet)(void *, const void *, size_t);
};

struct alioth_wlan_mhi {
	const struct alioth_wlan_mhi_io *io;
	uint32_t bhi, bhie, channel_db, event_db, sequence;
	uint64_t event_consumer[3], event_wp[3], command_wp, ipcr_rx_wp, ipcr_tx_wp;
	unsigned ipcr_tx_slot;
	uint16_t ipcr_wait_transaction;
	bool poisoned, sbl_sent, contexts_ready, amss_sent, ipcr_started;
	bool ipcr_tx_pending, ipcr_tx_done, ipcr_waiting_response, ipcr_response_done;
	/* Retain these even on timeout: the endpoint may still be using them. */
	struct alioth_wlan_mhi_dma sbl, image, vectors;
	struct alioth_wlan_mhi_dma channels, events, commands, command_ring, event_ring[3];
	struct alioth_wlan_mhi_dma ipcr_tx_ring, ipcr_tx_buffers, ipcr_rx_ring, ipcr_rx_buffers;
};

/* A zero-initialized host, an enumerated endpoint, and a mapped BAR are required.
 * Probe reads registers only. All functions return zero or a positive errno.
 */
int alioth_wlan_mhi_probe(struct alioth_wlan_mhi *, const struct alioth_wlan_mhi_io *);
int alioth_wlan_mhi_execution_environment(const struct alioth_wlan_mhi *, uint32_t *);
/* BHI sends the first 512 KiB while in PBL. Before BHIE, the PCIe owner must
 * service READY, install MHI contexts/event rings and request M0.
 * BHIE sends the complete firmware image, including the first segment.
 */
int alioth_wlan_mhi_download_sbl(struct alioth_wlan_mhi *, const void *, size_t);
/* Install the three event rings used by UMI's stock MHI configuration and
 * enter M0. Call only once, after SBL has put MHI into READY.
 */
int alioth_wlan_mhi_prepare_ready(struct alioth_wlan_mhi *);
int alioth_wlan_mhi_download_amss(struct alioth_wlan_mhi *, const void *, size_t);
int alioth_wlan_mhi_wait_mission(struct alioth_wlan_mhi *);
/* Start the stock UMI QRTR/IPCR channels (20/21) and post bounded receive
 * buffers. Polling is for bring-up only; production needs MSI event handling.
 */
int alioth_wlan_mhi_start_ipcr(struct alioth_wlan_mhi *);
int alioth_wlan_mhi_poll_ipcr(struct alioth_wlan_mhi *, unsigned timeout_ms, unsigned *received);
int alioth_wlan_mhi_send_qrtr_hello(struct alioth_wlan_mhi *);
int alioth_wlan_mhi_send_qrtr_packet(struct alioth_wlan_mhi *, const void *, size_t,
	unsigned timeout_ms, uint16_t expected_transaction);

#endif
