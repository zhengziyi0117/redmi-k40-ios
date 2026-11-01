/* Alioth adaptation: Apollo PCIe/MHI implementation, APSL 2.0.
 * GPIOs, PHY sequence, six supply resources/voltages, MSI and SID
 * checked against this K40 stock-device-tree.dtb. Opt-in aliothwifi=N. */
/*
 * Wi-Fi of apollo (QCA6390 on PCIe0), phase 1: power, link, firmware boot.
 * Nothing runs unless boot-arg aliothwifi=N asks for steps 1..N. The steps
 * run in a thread of their own, aliothwifidelay seconds (default 5) after the
 * root is mounted, and leave the radio held in reset afterwards.
 *
 * Ported from the Xiaomi Mi 10 (umi: the same SM8250 and QCA6390) bring-up
 * of github.com/JopangJovan/hackios-xnu-kernel-umi, devices/xiaomi-mi-10
 * (umi_wifi.c, umi_pcie.c, umi_mhi.c, umi_rpmh.c), with the apps SMMU
 * context of HackiOS (github.com/oosexgang/hackios-xnu-kernel,
 * bsd/dev/hakios.wifi.c); APSL 2.0. Apollo's kona DT has the same PCIe0
 * (identical PHY sequence; PERST GPIO 79, CLKREQ 80, WAKE 81), wlan-en
 * GPIO 20, MSI 0x17a10040 = SPI 768.. and SMMU stream 0x1c01. Its one
 * difference is the radio's digital rail: PM8150 S6, not PM8009 S2
 * (apollo-sm8250.dtsi &wlan), see alioth_wlan_rpmh_wifi_power().
 *
 *   1  read   GCC PCIe0 clocks/resets and the radio's pins (TLMM north)
 *   2  write  rails (RPMh), wlan-en, PCIe0 clocks, PHY and link; reads the
 *             QCA6390's ID (17cb:1101), BAR and MHI registers and which
 *             apps SMMU match registers exist; bus mastering stays off
 *   3  write  a free apps SMMU context for stream 0x1c01 with empty tables,
 *             read back (the hypervisor decides), put back as found
 *   4  write  that context with our tables, WLAON power control, MSI, bus
 *             mastering; BHI download of the firmware's first 512 KiB
 *             (/sweet-wifi "amss" in the ADT: mkadt.py --wifi-fw amss.bin)
 *   5  write  MHI contexts and event rings, M0
 *   6  write  BHIe download of the whole firmware; mission mode
 *   7  talk   QRTR on MHI channels 20/21: hello, then aliothwifipoll ms
 *             (default 5000) of what the firmware announces (the WLAN
 *             firmware service is 0x45)
 *   8  talk   QMI with that service (alioth_wlan_wlfw.c, xnu 0127): indications,
 *             host capabilities, the memory the firmware asks for (given at
 *             stock CNSS's fixed IOVAs), what the chip and firmware are
 *   9  talk   the regulatory database, the board's data and M3 (/sweet-wifi
 *             "regdb", "board", "m3": mkadt.py --wifi-fw DIR); "init done"
 *  10  talk   cold-boot calibration: "calibration done" / "ready"
 *  11  driver Linux's ath11k (xnu 0128: the object tools/mk-ath11k.sh builds, on
 *             alioth_wlan_lx.c) instead of 8-10: its own handshake, copy engines,
 *             WMI, then a scan of the networks around; then taken down
 *  12  driver as 11, but the radio stays up as en0 for iOS: wifid scans and
 *             joins through it (alioth_wlan_apple80211.c); en0 is made at once, the
 *             radio comes up behind it
 * Afterwards PERST and wlan-en go low and the SMMU is put back as found
 * (not with 12 once the driver is up).
 * The rails stay voted (shared with Bluetooth and the PCIe/UFS PHYs).
 * No interrupt is taken: MHI's rings are polled.
 */
#if defined(PLATFORM_ALIOTH)
#include <sys/cdefs.h>
extern int alioth_wlan_log(const char *, ...) __printflike(1, 2);
#define printf alioth_wlan_log
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <mach/vm_param.h>
#include <vm/vm_kern.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <libkern/libkern.h>
#include "alioth_wlan_pcie.h"
#include "alioth_wlan_mhi.h"
#include <sys/redwood_qrtr.h>
#include <sys/redwood_say.h>

extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, uint32_t);
extern uint64_t kvtophys(vm_offset_t);
extern kern_return_t thread_terminate(thread_t);
extern int hz;
extern int alioth_wlan_rpmh_wifi_power(void);
extern int alioth_wlan_wlfw_run(unsigned steps);
extern void read_random(void *buffer, u_int numBytes);
extern void redwood_lx_net_early(void);
extern int redwood_lx_start(void);
extern void redwood_lx_stop(void);
extern int redwood_lx_prepare(void);
extern void redwood_net_init(void);
extern void SweetNetworkStackStart(void);       /* iokit/Kernel/arm/SweetNetworkStack.cpp (0129) */
extern void AliothWLANRegistryReady(void);      /* ... (0131) */
void alioth_wlan_wifi_init(void);
const uint8_t *alioth_wlan_wifi_file(const char *name, unsigned int *size);
void *alioth_wlan_wifi_memory_at(size_t size, uint64_t iova);
void alioth_wlan_wifi_clean(const void *memory, size_t size);
/* for alioth_wlan_lx.c (the driver's object) */
unsigned alioth_wlan_wifi_option(const char *name);
void *alioth_wlan_wifi_radio_memory(size_t size, unsigned long long *address);
void *alioth_wlan_wifi_firmware_memory(unsigned type, size_t size, unsigned long long *address);
void *alioth_wlan_wifi_registers(unsigned long long physical, size_t size);
bool alioth_wlan_wifi_msi(unsigned long long *address, unsigned *data);
int alioth_wlan_wifi_radio_boot(void);
void alioth_wlan_wifi_radio_reset(void);
bool alioth_wlan_wifi_radio_address(uint8_t *address);
bool alioth_wlan_wifi_network(uint8_t *name, unsigned *name_length, uint8_t *passphrase, unsigned *passphrase_length);

/* osfmk/arm/memory_types.h, not visible to BSD code (as in alioth_wlan_gpu.c) */
#ifndef VM_WIMG_IO
#define VM_WIMG_IO              0x7u    /* coherent | guarded | not cacheable: device */
#endif
#ifndef VM_WIMG_WCOMB
#define VM_WIMG_WCOMB           0x6u    /* not cacheable | coherent: normal non-cacheable */
#endif

extern void loader_log_line(const char *);
extern void loader_log_flush(void);
int alioth_wlan_log(const char *format, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, format);
    int count = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    loader_log_line(buffer);
    loader_log_flush();
    return count;
}

static uint32_t wifi_steps, wifi_delay = 5, wifi_poll_ms = 5000;

/*
 * With the driver (steps 11 and 12) several threads reach the radio: the driver's own, and one
 * that polls MHI's rings every millisecond (the QMI replies and indications come in there). MHI
 * and QRTR are one at a time under wifi_mhi_lock, as umi_wifi.c has it; the SMMU's tables and
 * the radio's addresses under wifi_map_lock.
 */
static LCK_GRP_DECLARE(wifi_lck_grp, "sweet-wifi");
static LCK_MTX_DECLARE(wifi_mhi_lock, &wifi_lck_grp);
static LCK_MTX_DECLARE(wifi_map_lock, &wifi_lck_grp);

int
alioth_wlan_wifi_say(const char *format, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, format);
	int n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	printf("ALIOTH WIFI: %s", line);
	return n;
}

int
alioth_wlan_wifi_tell(const char *format, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, format);
	int n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	printf("ALIOTH WIFI: %s", line);
	return n;
}

static uint32_t
wifi_rd(vm_offset_t va)
{
	uint32_t v = *(volatile uint32_t *)va;
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
wifi_wr(vm_offset_t va, uint32_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)va = value;
	__asm__ volatile("dsb osh" ::: "memory");
}

/* ---- registers ----------------------------------------------------------------------- */

/* Mapped once in kernel_map (not the 8 MiB io_submap) and never unmapped. */
static struct {
	uint32_t pa, size;
	vm_offset_t va;
} wifi_regions[] = {
	{ 0x00100000u, 0xa0000u, 0 },   /* GCC, including the PCIe TBU clock */
	{ 0x0f900000u, 0x80000u, 0 },   /* TLMM north tile: GPIO 20, 79-81, 124 */
	{ 0x01c00000u, 0x3000u, 0 },    /* PCIe0 PARF */
	{ 0x01c06000u, 0x1000u, 0 },    /* PCIe0 QMP PHY */
	{ 0x60000000u, 0x1000u, 0 },    /* DBI + ELBI */
	{ 0x60001000u, 0x1000u, 0 },    /* unrolled outbound iATU */
	{ 0x60100000u, 0x1000u, 0 },    /* endpoint configuration (CFG0 window) */
	{ 0x60300000u, 0x100000u, 0 },  /* QCA6390 BAR */
};
#define WIFI_BAR        0x60300000u

static bool
wifi_map_regions(void)
{
	for (unsigned i = 0; i < sizeof(wifi_regions) / sizeof(wifi_regions[0]); i++) {
		if (wifi_regions[i].va) {
			continue;
		}
		wifi_regions[i].va = ml_io_map_unmappable(wifi_regions[i].pa, wifi_regions[i].size, VM_WIMG_IO);
		if (!wifi_regions[i].va) {
			printf("ALIOTH WIFI: cannot map %#x (+%#x)\n", wifi_regions[i].pa, wifi_regions[i].size);
			return false;
		}
	}
	return true;
}

static vm_offset_t
wifi_reg(uint32_t pa)
{
	if (pa & 3) {
		return 0;
	}
	for (unsigned i = 0; i < sizeof(wifi_regions) / sizeof(wifi_regions[0]); i++) {
		uint32_t offset = pa - wifi_regions[i].pa;
		if (wifi_regions[i].va && offset <= wifi_regions[i].size - 4) {
			return wifi_regions[i].va + offset;
		}
	}
	printf("ALIOTH WIFI: unmapped register %#x\n", pa);
	return 0;
}

static uint32_t
pcie_read(void *context, uint32_t pa)
{
	(void)context;
	vm_offset_t va = wifi_reg(pa);
	return va ? wifi_rd(va) : UINT32_MAX;
}

static void
pcie_write(void *context, uint32_t pa, uint32_t value)
{
	(void)context;
	vm_offset_t va = wifi_reg(pa);
	if (va) {
		wifi_wr(va, value);
	}
}

static int
pcie_power(void *context)
{
	(void)context;
	return alioth_wlan_rpmh_wifi_power();
}

static void
pcie_delay(void *context, unsigned us)
{
	(void)context;
	delay((int)us);
}

static void
pcie_report(void *context, const char *stage, uint32_t value)
{
	(void)context;
	printf("ALIOTH WIFI: %s = %#x\n", stage, value);
}

/* Step 1: always-readable GCC and TLMM only; nothing behind PCIe0. */
static void
wifi_survey(void)
{
	static const struct { const char *name; uint32_t offset; } clocks[] = {
		{ "pcie0_bcr", 0x6b000 }, { "pcie0_gdsc", 0x6b004 },
		{ "pcie0_clock_votes", 0x52008 }, { "pcie0_slv_q2a", 0x6b010 },
		{ "pcie0_slv_axi", 0x6b014 }, { "pcie0_mstr_axi", 0x6b01c },
		{ "pcie0_cfg_ahb", 0x6b024 }, { "pcie0_aux", 0x6b028 },
		{ "pcie0_pipe", 0x6b02c }, { "pcie0_aux_cmd", 0x6b038 },
		{ "pcie0_aux_cfg", 0x6b03c }, { "pcie0_phy_bcr", 0x6c01c },
		{ "pcie0_phy_refgen", 0x6f02c }, { "pcie0_clkref", 0x8c004 },
	};
	static const struct { const char *name; uint32_t pin; } pins[] = {
		{ "wlan_en", 20 }, { "bt_en", 21 }, { "pcie0_perst", 79 },
		{ "pcie0_clkreq", 80 }, { "pcie0_wake", 81 }, { "wlan_sw_ctrl", 124 },
	};
	printf("ALIOTH WIFI: step 1: QCA6390 on PCIe0; read-only GCC/TLMM survey\n");
	for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
		printf("ALIOTH WIFI: GCC %s (%#x) = %#x\n", clocks[i].name, 0x00100000u + clocks[i].offset,
		    pcie_read(NULL, 0x00100000u + clocks[i].offset));
	}
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		printf("ALIOTH WIFI: GPIO%u %s cfg=%#x io=%#x\n", pins[i].pin, pins[i].name,
		    pcie_read(NULL, 0x0f900000u + pins[i].pin * 0x1000u),
		    pcie_read(NULL, 0x0f900000u + pins[i].pin * 0x1000u + 4));
	}
}

/* ---- the radio's view of memory: a context of the apps SMMU ------------------------ */

/*
 * The boot chain sets nothing up for stream 0x1c01 (PCIe0 base 0x1c00 +
 * BDF-to-SID offset 1, kona DT iommu-map), and a stream nobody matches
 * faults. Before the radio may master the bus it gets the first free stream
 * match register and the lowest context no match register uses, with our
 * page tables: IOVA 0xa0000000..0xb0000000 (the MHI aperture), 4 KiB pages,
 * non-cacheable. Every register written is put back when the steps end.
 */
#define SMMU_PA                 0x15000000u
#define SMMU_SIZE               0x100000u
#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_SGFSR              0x048
#define SMMU_SGFSYNR0           0x050
#define SMMU_SGFSYNR1           0x054
#define SMMU_SMR(n)             (0x800u + 4u * (n))
#define SMMU_S2CR(n)            (0xc00u + 4u * (n))
#define SMMU_CBAR(n)            (4u * (n))              /* second page */
#define SMMU_CBA2R(n)           (0x800u + 4u * (n))     /* second page */
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CB_SCTLR           0x00
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_TCR             0x30
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FSR             0x58
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0          0x68
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)       /* 36-bit PA, 16-bit ASID */
#define SMMU_MAIR0              0xf404ff44u     /* 0 non-cacheable, 1 cached, 2 device */
#define SMMU_FSR_ALL            0xc00001feu
#define RADIO_STREAM            0x1c01u
#define RADIO_IOVA              0xa0000000ull
#define RADIO_IOVA_SIZE         0x10000000ull
#define RADIO_TABLES            130u            /* root, one level 2, 128 level 3: the whole 256 MiB */
#define RADIO_PTE               (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)
#define RADIO_PTE_DEVICE        (RADIO_PTE | 2ull << 2 | 1ull << 53 | 1ull << 54)
#define PTE_ADDRESS             0x0000fffffffff000ull
/* PCIe0's MSI controller (kona-pcie.dtsi pcie0_msi): the GIC-600's message
 * SPI register; writing N raises INTID N. Vectors 0.. = SPI 768.. = INTID 800.. */
#define RADIO_MSI_REGISTER      0x17a10040ull
#define RADIO_MSI_DATA          (768u + 32u)

static const uint32_t smmu_saved_offsets[] = {
	SMMU_CB_SCTLR, SMMU_CB_TCR2, SMMU_CB_TTBR0, SMMU_CB_TTBR0 + 4, SMMU_CB_TTBR1,
	SMMU_CB_TTBR1 + 4, SMMU_CB_TCR, SMMU_CB_MAIR0, SMMU_CB_MAIR1,
};
#define SMMU_SAVED      (sizeof(smmu_saved_offsets) / sizeof(smmu_saved_offsets[0]))

static struct {
	vm_offset_t base;
	bool found, installed;
	uint32_t page, pages, match, context;
	uint32_t old_smr, old_s2cr, cbar, cba2r, saved[SMMU_SAVED];
	vm_offset_t tables;
	uint32_t table_count;
	uint64_t next;                  /* first IOVA offset not handed out */
} rs;

static vm_offset_t
smmu_second(uint32_t offset)
{
	return rs.base + rs.page + offset;
}

static vm_offset_t
smmu_ctx(uint32_t offset)
{
	return rs.base + (rs.pages + rs.context) * rs.page + offset;
}

static uint64_t *
smmu_table(uint32_t n)
{
	return (uint64_t *)(rs.tables + 4096u * n);
}

static vm_offset_t
smmu_table_va(uint32_t n)
{
	return rs.tables + 4096u * n;
}

static uint64_t *
smmu_next_table(uint64_t *entry)
{
	if (*entry & 1) {
		for (uint32_t i = 0; i < rs.table_count; i++) {
			if (kvtophys(smmu_table_va(i)) == (*entry & PTE_ADDRESS)) {
				return smmu_table(i);
			}
		}
		return NULL;
	}
	if (rs.table_count == RADIO_TABLES) {
		return NULL;
	}
	uint64_t *table = smmu_table(rs.table_count++);
	*entry = kvtophys(smmu_table_va(rs.table_count - 1)) | 3;
	return table;
}

/* One page: `pa` behind IOVA `at`. The tables are pushed to memory by smmu_flush(). */
static bool
smmu_set(uint64_t at, uint64_t pa, uint64_t attributes)
{
	if (rs.tables == 0) {
		if (kmem_alloc(kernel_map, &rs.tables, round_page(4096u * RADIO_TABLES), KMA_KOBJECT | KMA_ZERO | KMA_DATA,
		    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			printf("ALIOTH WIFI: no memory for the SMMU tables\n");
			return false;
		}
		rs.table_count = 1;
	}
	if (attributes == 0) {
		return true;            /* the tables only */
	}
	uint64_t *level2 = smmu_next_table(&smmu_table(0)[at >> 30 & 0x1ff]);
	uint64_t *level3 = level2 == NULL ? NULL : smmu_next_table(&level2[at >> 21 & 0x1ff]);
	if (level3 == NULL) {
		printf("ALIOTH WIFI: no page table left for IOVA %#llx\n", at);
		return false;
	}
	level3[at >> 12 & 0x1ff] = pa | attributes;
	return true;
}

/* The SMMU walks the tables in memory, not in this core's cache. */
static void
smmu_flush(void)
{
	for (vm_offset_t line = rs.tables; line < rs.tables + 4096u * RADIO_TABLES; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

static bool
smmu_tlb_sync(void)
{
	if (!rs.installed) {
		return true;
	}
	wifi_wr(smmu_ctx(SMMU_CB_TLBIASID), rs.context);
	wifi_wr(smmu_ctx(SMMU_CB_TLBSYNC), 1);
	for (unsigned us = 0; us < 100000; us++) {
		uint32_t status = wifi_rd(smmu_ctx(SMMU_CB_TLBSTATUS));
		if (status == UINT32_MAX) {
			return false;
		}
		if (!(status & 1)) {
			return true;
		}
		delay(1);
	}
	printf("ALIOTH WIFI: SMMU TLB sync timeout\n");
	return false;
}

/* Step 2: read-only. Which match registers exist and whether any already covers the radio. */
static void
smmu_survey(void)
{
	if (!rs.base) {
		rs.base = ml_io_map_unmappable(SMMU_PA, SMMU_SIZE, VM_WIMG_IO);
	}
	if (!rs.base) {
		printf("ALIOTH WIFI: cannot map the apps SMMU\n");
		return;
	}
	uint32_t scr0 = wifi_rd(rs.base + SMMU_SCR0), idr0 = wifi_rd(rs.base + SMMU_IDR0), idr1 = wifi_rd(rs.base + SMMU_IDR1);
	unsigned matches = idr0 & 0xff, valid = 0, covering = 0;
	for (unsigned n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = wifi_rd(rs.base + SMMU_SMR(n)), s2cr = wifi_rd(rs.base + SMMU_S2CR(n));
		if (!(smr >> 31)) {
			continue;
		}
		valid++;
		if ((((smr & 0x7fff) ^ RADIO_STREAM) & ~((smr >> 16) & 0x7fff)) == 0) {
			covering++;
			printf("ALIOTH WIFI: SMMU SMR%u=%#x S2CR=%#x already covers stream %#x\n", n, smr, s2cr, RADIO_STREAM);
		}
	}
	printf("ALIOTH WIFI: apps SMMU scr0=%#x idr0=%#x idr1=%#x: %u of %u match registers valid, %u cover %#x, %u contexts\n",
	    scr0, idr0, idr1, valid, matches, covering, RADIO_STREAM, idr1 & 0xff);
}

/* A free match register and the lowest unused, switched-off context. Reads only. */
static bool
smmu_find(void)
{
	bool used[256] = { false };
	uint32_t scr0 = wifi_rd(rs.base + SMMU_SCR0), idr0 = wifi_rd(rs.base + SMMU_IDR0), idr1 = wifi_rd(rs.base + SMMU_IDR1);
	uint32_t matches = idr0 & 0xff, contexts = idr1 & 0xff;
	if (scr0 == UINT32_MAX || idr0 == UINT32_MAX || idr1 == UINT32_MAX) {
		return false;
	}
	if (scr0 & 1u << 3) {
		printf("ALIOTH WIFI: the SMMU numbers streams with 16 bits; not handled\n");
		return false;
	}
	rs.page = idr1 >> 31 ? 0x10000u : 0x1000u;
	rs.pages = 2u << (idr1 >> 28 & 7);
	if (2u * rs.pages * rs.page != SMMU_SIZE) {
		printf("ALIOTH WIFI: SMMU says 2 x %u pages of %#x bytes, not 1 MiB; contexts not used\n", rs.pages, rs.page);
		return false;
	}
	rs.match = ~0u;
	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = wifi_rd(rs.base + SMMU_SMR(n)), s2cr = wifi_rd(rs.base + SMMU_S2CR(n));
		if (smr >> 31) {
			if ((((smr & 0x7fff) ^ RADIO_STREAM) & ~((smr >> 16) & 0x7fff)) == 0) {
				printf("ALIOTH WIFI: stream %#x already belongs to SMR%u; left alone\n", RADIO_STREAM, n);
				return false;
			}
			used[s2cr & 0xff] = true;
		} else if (rs.match == ~0u) {
			rs.match = n;
			rs.old_smr = smr;
			rs.old_s2cr = s2cr;
		}
	}
	for (rs.context = 0; rs.context < contexts && used[rs.context]; rs.context++) {
	}
	if (rs.match == ~0u || rs.context >= contexts) {
		printf("ALIOTH WIFI: the SMMU has no free match register or context\n");
		return false;
	}
	for (unsigned i = 0; i < SMMU_SAVED; i++) {
		rs.saved[i] = wifi_rd(smmu_ctx(smmu_saved_offsets[i]));
	}
	rs.cbar = wifi_rd(smmu_second(SMMU_CBAR(rs.context)));
	rs.cba2r = wifi_rd(smmu_second(SMMU_CBA2R(rs.context)));
	printf("ALIOTH WIFI: free: SMR%u (%#x, s2cr %#x) and context %u at %#x: sctlr %#x cbar %#x fsr %#x\n", rs.match,
	    rs.old_smr, rs.old_s2cr, rs.context, SMMU_PA + (rs.pages + rs.context) * rs.page, rs.saved[0], rs.cbar,
	    wifi_rd(smmu_ctx(SMMU_CB_FSR)));
	if (rs.saved[0] & 1) {
		printf("ALIOTH WIFI: context %u is switched on although no stream uses it: somebody's; left alone\n", rs.context);
		return false;
	}
	if (wifi_rd(smmu_ctx(SMMU_CB_FSR)) & SMMU_FSR_ALL) {
		printf("ALIOTH WIFI: context %u has pending faults; left alone\n", rs.context);
		return false;
	}
	rs.found = true;
	return true;
}

static void
smmu_faults(const char *when)
{
	if (!rs.installed) {
		return;
	}
	uint32_t status = wifi_rd(smmu_ctx(SMMU_CB_FSR)), global = wifi_rd(rs.base + SMMU_SGFSR);
	if ((status & SMMU_FSR_ALL) == 0 && global == 0) {
		printf("ALIOTH WIFI: %s: no SMMU faults (context %u fsr %#x)\n", when, rs.context, status);
		return;
	}
	printf("ALIOTH WIFI: %s: SMMU FAULT: context %u fsr %#x far %#llx fsynr0 %#x; global sgfsr %#x sgfsynr %#x/%#x\n", when,
	    rs.context, status, (uint64_t)wifi_rd(smmu_ctx(SMMU_CB_FAR + 4)) << 32 | wifi_rd(smmu_ctx(SMMU_CB_FAR)),
	    wifi_rd(smmu_ctx(SMMU_CB_FSYNR0)), global, wifi_rd(rs.base + SMMU_SGFSYNR0), wifi_rd(rs.base + SMMU_SGFSYNR1));
}

/* Stream 0x1c01 through our context (tables made beforehand); every global write read back. */
static bool
smmu_install(void)
{
	if (!rs.base || !smmu_find() || !smmu_set(RADIO_IOVA, 0, 0)) {
		return false;
	}
	smmu_flush();
	uint64_t root = kvtophys(rs.tables) | (uint64_t)rs.context << 48;
	uint32_t sees = 1u << 31 | RADIO_STREAM;
	printf("ALIOTH WIFI: SMMU: context %u gets our tables (root %#llx), SMR%u sends stream %#x to it\n", rs.context,
	    root & PTE_ADDRESS, rs.match, RADIO_STREAM);
	wifi_wr(smmu_ctx(SMMU_CB_SCTLR), 0);
	wifi_wr(smmu_second(SMMU_CBA2R(rs.context)), 1);         /* 64-bit descriptors */
	wifi_wr(smmu_second(SMMU_CBAR(rs.context)), SMMU_CBAR_STAGE_1);
	wifi_wr(smmu_ctx(SMMU_CB_TCR2), SMMU_TCR2);
	wifi_wr(smmu_ctx(SMMU_CB_TCR), SMMU_TCR_39_BITS);
	wifi_wr(smmu_ctx(SMMU_CB_TTBR0), (uint32_t)root);
	wifi_wr(smmu_ctx(SMMU_CB_TTBR0 + 4), (uint32_t)(root >> 32));
	wifi_wr(smmu_ctx(SMMU_CB_TTBR1), 0);
	wifi_wr(smmu_ctx(SMMU_CB_TTBR1 + 4), 0);
	wifi_wr(smmu_ctx(SMMU_CB_MAIR0), SMMU_MAIR0);
	wifi_wr(smmu_ctx(SMMU_CB_MAIR1), 0);
	wifi_wr(smmu_ctx(SMMU_CB_FSR), SMMU_FSR_ALL);
	rs.installed = true;            /* from here on there is something to put back */
	if (!smmu_tlb_sync()) {
		return false;
	}
	wifi_wr(smmu_ctx(SMMU_CB_SCTLR), SMMU_SCTLR_ON);
	wifi_wr(rs.base + SMMU_S2CR(rs.match), rs.context);
	wifi_wr(rs.base + SMMU_SMR(rs.match), sees);
	/* The global registers are the hypervisor's to grant: see what it made of them. */
	uint32_t smr = wifi_rd(rs.base + SMMU_SMR(rs.match)), s2cr = wifi_rd(rs.base + SMMU_S2CR(rs.match));
	uint32_t cbar = wifi_rd(smmu_second(SMMU_CBAR(rs.context))), sctlr = wifi_rd(smmu_ctx(SMMU_CB_SCTLR));
	uint32_t ttbr_lo = wifi_rd(smmu_ctx(SMMU_CB_TTBR0)), ttbr_hi = wifi_rd(smmu_ctx(SMMU_CB_TTBR0 + 4));
	printf("ALIOTH WIFI: SMMU read back: smr %#x s2cr %#x cbar %#x sctlr %#x ttbr0 %#x:%#x tcr %#x tcr2 %#x\n", smr, s2cr,
	    cbar, sctlr, ttbr_hi, ttbr_lo, wifi_rd(smmu_ctx(SMMU_CB_TCR)), wifi_rd(smmu_ctx(SMMU_CB_TCR2)));
	if (smr != sees || (s2cr & 0x300ff) != rs.context || (cbar & 0x30000) != (SMMU_CBAR_STAGE_1 & 0x30000) ||
	    !(sctlr & 1) || ttbr_lo != (uint32_t)root || ttbr_hi != (uint32_t)(root >> 32)) {
		printf("ALIOTH WIFI: THE SMMU DID NOT TAKE IT: stream %#x is not going through context %u\n", RADIO_STREAM,
		    rs.context);
		return false;
	}
	printf("ALIOTH WIFI: THE RADIO'S STREAM %#x GOES THROUGH SMMU CONTEXT %u WITH OUR TABLES\n", RADIO_STREAM, rs.context);
	smmu_faults("installed");
	return true;
}

/* As found; called with the endpoint in reset (PERST low) or never enabled. */
static void
smmu_restore(void)
{
	if (!rs.found) {
		return;
	}
	if (rs.installed) {
		smmu_faults("before restore");
	}
	wifi_wr(rs.base + SMMU_SMR(rs.match), rs.old_smr);
	wifi_wr(rs.base + SMMU_S2CR(rs.match), rs.old_s2cr);
	wifi_wr(smmu_ctx(SMMU_CB_SCTLR), 0);
	for (unsigned i = 1; i < SMMU_SAVED; i++) {
		wifi_wr(smmu_ctx(smmu_saved_offsets[i]), rs.saved[i]);
	}
	wifi_wr(smmu_second(SMMU_CBAR(rs.context)), rs.cbar);
	wifi_wr(smmu_second(SMMU_CBA2R(rs.context)), rs.cba2r);
	wifi_wr(smmu_ctx(SMMU_CB_SCTLR), rs.saved[0]);
	bool restored = wifi_rd(rs.base + SMMU_SMR(rs.match)) == rs.old_smr &&
	    wifi_rd(rs.base + SMMU_S2CR(rs.match)) == rs.old_s2cr &&
	    wifi_rd(smmu_second(SMMU_CBAR(rs.context))) == rs.cbar &&
	    wifi_rd(smmu_second(SMMU_CBA2R(rs.context))) == rs.cba2r;
	for (unsigned i = 0; i < SMMU_SAVED; i++) {
		uint32_t now = wifi_rd(smmu_ctx(smmu_saved_offsets[i]));
		if (now != rs.saved[i])
			printf("ALIOTH WIFI: restore context offset %#x: wanted %#x, read %#x\n",
			    smmu_saved_offsets[i], rs.saved[i], now);
		restored &= now == rs.saved[i];
	}
	printf("ALIOTH WIFI: SMMU SMR%u and context %u put back as found: %s\n", rs.match, rs.context,
	    restored ? "yes" : "NO (read back differs)");
	rs.installed = false;
	rs.found = false;
}

/*
 * The firmware's own memory has fixed addresses (stock CNSS: 0xa1000000 for its main region,
 * 0xa1600000 for calibration, 0xa1880000 for M3, below 0xa1900000): the addresses handed out one
 * after the other jump over that window, so it can be filled whenever the firmware asks.
 */
#define RADIO_FIXED_START       0x01000000ull           /* offsets from RADIO_IOVA */
#define RADIO_FIXED_END         0x01900000ull

static bool
radio_map_locked(uint64_t pa, vm_offset_t va, uint64_t size, uint64_t attributes, uint64_t *iova, bool fixed)
{
	size = round_page(size);
	if (!fixed) {
		if (rs.next < RADIO_FIXED_END && rs.next + size > RADIO_FIXED_START) {
			rs.next = RADIO_FIXED_END;
		}
		if (rs.next + size > RADIO_IOVA_SIZE) {
			printf("ALIOTH WIFI: the radio's 256 MiB of addresses are used up\n");
			return false;
		}
		*iova = RADIO_IOVA + rs.next;
	} else if (*iova < RADIO_IOVA + RADIO_FIXED_START || *iova + size > RADIO_IOVA + RADIO_FIXED_END || (*iova & 4095)) {
		printf("ALIOTH WIFI: %#llx bytes at IOVA %#llx are not in the firmware's window\n", size, *iova);
		return false;
	}
	for (uint64_t offset = 0; offset < size; offset += 4096) {
		uint64_t page = va ? kvtophys(va + offset) : pa + offset;
		if (!smmu_set(*iova + offset, page, attributes)) {
			return false;
		}
	}
	smmu_flush();
	if (!fixed) {
		rs.next += size;
	}
	return smmu_tlb_sync();
}

/* `size` bytes at the next free IOVA (or, `fixed`, at *iova): from physical `pa` on, or (va != 0) each page of `va`. */
static bool
radio_map(uint64_t pa, vm_offset_t va, uint64_t size, uint64_t attributes, uint64_t *iova)
{
	lck_mtx_lock(&wifi_map_lock);
	bool done = radio_map_locked(pa, va, size, attributes, iova, false);
	lck_mtx_unlock(&wifi_map_lock);
	return done;
}

static bool
radio_map_fixed(uint64_t pa, vm_offset_t va, uint64_t size, uint64_t attributes, uint64_t iova)
{
	lck_mtx_lock(&wifi_map_lock);
	bool done = radio_map_locked(pa, va, size, attributes, &iova, true);
	lck_mtx_unlock(&wifi_map_lock);
	return done;
}

/*
 * For the firmware's own memory (alioth_wlan_wlfw.c): `size` zeroed bytes behind the fixed IOVA stock
 * CNSS gives them, above everything handed out so far. The firmware uses them, the CPU does not
 * read them back: cached pages, pushed out once here (alioth_wlan_wifi_clean after the CPU writes).
 */
void *
alioth_wlan_wifi_memory_at(size_t size, uint64_t iova)
{
	vm_offset_t memory = 0;
	size = round_page(size);
	if (size == 0) {
		return NULL;
	}
	if (kmem_alloc(kernel_map, &memory, size, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("ALIOTH WIFI: no %zu bytes for the firmware\n", size);
		return NULL;
	}
	alioth_wlan_wifi_clean((void *)memory, size);
	if (!radio_map_fixed(0, memory, size, RADIO_PTE, iova)) {
		return NULL;
	}
	return (void *)memory;
}

void
alioth_wlan_wifi_clean(const void *memory, size_t size)
{
	for (vm_offset_t line = (vm_offset_t)memory & ~(vm_offset_t)63; line < (vm_offset_t)memory + size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* ---- DMA memory for MHI --------------------------------------------------------------- */

/*
 * One physical run, mapped a second time non-cacheable (as umi's
 * lx_os_radio_memory); the cached view is cleaned once and not used again.
 * A large block that does not come in one run is taken page by page instead,
 * used through the cached view, and cleaned to memory whenever MHI hands it
 * to the radio (wifi_sync). Only the firmware image (the radio reads it, the
 * CPU never reads what the radio wrote) is that large: rings and receive
 * buffers stay below WIFI_SCATTER_MIN and must come in one run.
 */
#define WIFI_SCATTER_MIN        (1024u * 1024u)
#define WIFI_CACHED_MAX 8
static struct {
	vm_offset_t va;
	size_t size;
} wifi_cached[WIFI_CACHED_MAX];

static int
wifi_allocate(void *context, size_t size, struct alioth_wlan_mhi_dma *dma)
{
	(void)context;
	vm_offset_t cached = 0;
	uint64_t iova = 0;
	if (size == 0 || size > 16u * 1024u * 1024u) {
		return EINVAL;
	}
	size = round_page(size);
	if (kmem_alloc_contig(kernel_map, &cached, size, 0, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) == KERN_SUCCESS) {
		uint64_t pa = kvtophys(cached);
		for (vm_offset_t line = cached; line < cached + size; line += 64) {
			__asm__ volatile ("dc civac, %0" : : "r" (line));
		}
		__asm__ volatile ("dsb sy" ::: "memory");
		vm_offset_t uncached = ml_io_map_unmappable(pa, size, VM_WIMG_WCOMB);
		if (!uncached || !radio_map(pa, 0, size, RADIO_PTE, &iova)) {
			return ENOMEM;
		}
		dma->bytes = (void *)uncached;
	} else {
		unsigned slot;
		for (slot = 0; slot < WIFI_CACHED_MAX && wifi_cached[slot].va; slot++) {
		}
		if (size < WIFI_SCATTER_MIN || slot == WIFI_CACHED_MAX || kmem_alloc(kernel_map, &cached, size,
		    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			printf("ALIOTH WIFI: no %zu bytes of DMA memory\n", size);
			return ENOMEM;
		}
		if (!radio_map(0, cached, size, RADIO_PTE, &iova)) {
			return ENOMEM;
		}
		wifi_cached[slot].va = cached;
		wifi_cached[slot].size = size;
		dma->bytes = (void *)cached;
		printf("ALIOTH WIFI: %zu KiB not in one run: taken page by page (cached, cleaned before use)\n", size >> 10);
	}
	dma->address = iova;
	dma->size = size;
	return 0;
}

static void
wifi_sync(void *context, const struct alioth_wlan_mhi_dma *dma)
{
	(void)context;
	for (unsigned i = 0; i < WIFI_CACHED_MAX; i++) {
		if (wifi_cached[i].va && (vm_offset_t)dma->bytes == wifi_cached[i].va) {
			for (vm_offset_t line = wifi_cached[i].va; line < wifi_cached[i].va + wifi_cached[i].size; line += 64) {
				__asm__ volatile ("dc cvac, %0" : : "r" (line));
			}
		}
	}
	/* Publish writes before a doorbell is rung. */
	__asm__ volatile ("dsb osh" ::: "memory");
}

static uint32_t
mhi_read(void *context, uint32_t offset)
{
	(void)context;
	return offset <= 0xffc && !(offset & 3) ? pcie_read(NULL, WIFI_BAR + offset) : UINT32_MAX;
}

static void
mhi_write(void *context, uint32_t offset, uint32_t value)
{
	(void)context;
	if (offset <= 0xffc && !(offset & 3)) {
		pcie_write(NULL, WIFI_BAR + offset, value);
	}
}

/* ---- QRTR: the IPC router on MHI channels 20/21, for the QMI helpers (hakios.qrtr.h) ---- */

/*
 * From umi_wifi.c of the Mi 10 port: one port of ours, the services the firmware announced, and
 * flow control (a packet that asks for confirmation is answered with RESUME_TX before the next
 * send or poll; never from the receive callback, which runs inside MHI's event-ring consumer).
 * All of it runs on the Wi-Fi thread: MHI is polled, not interrupt-driven.
 */
#define QRTR_SERVICES           16u
#define QRTR_LOOKUPS            4u
#define QRTR_PORT_FIRST         0x400u
#define QRTR_HEADER             32u
#define QRTR_PACKET_MAX         8192u
#define QRTR_NODE_APPS          1u

struct qrtr_service {
	bool valid;
	uint32_t service, version, instance, node, port;
};

struct qrtr_lookup {
	bool valid;
	uint32_t service, version, instance;
};

struct redwood_qrtr_port {
	bool active;
	uint32_t number;
	redwood_qrtr_receive_t receive;
	redwood_qrtr_server_t server;
	void *context;
	struct qrtr_lookup lookups[QRTR_LOOKUPS];
};

static struct {
	struct alioth_wlan_mhi *mhi;
	uint32_t next_port;
	bool resume_pending;
	uint32_t resume_node, resume_port, resume_remote_node, resume_remote_port;
	struct redwood_qrtr_port port;
	struct qrtr_service services[QRTR_SERVICES];
	unsigned announced, wlfw;
} qr;
static uint8_t qr_tx[QRTR_PACKET_MAX];

static uint32_t
le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void
put_le32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}

static bool
qr_matches(const struct qrtr_lookup *lookup, const struct qrtr_service *service)
{
	return lookup->valid && lookup->service == service->service &&
	    (!lookup->version || lookup->version == service->version) &&
	    (!lookup->instance || lookup->instance == service->instance);
}

static void
qr_notify(const struct qrtr_service *service, bool arrived)
{
	struct redwood_qrtr_port *port = &qr.port;
	if (!port->active || port->server == NULL) {
		return;
	}
	for (unsigned i = 0; i < QRTR_LOOKUPS; i++) {
		if (qr_matches(&port->lookups[i], service)) {
			port->server(port->context, arrived, service->service, service->version, service->instance, service->node,
			    service->port);
		}
	}
}

static int
qr_flush_resume(void)
{
	if (!qr.resume_pending) {
		return 0;
	}
	uint8_t packet[52] = { 0 };
	put_le32(packet, 1);
	put_le32(packet + 4, 7);                /* RESUME_TX */
	put_le32(packet + 8, qr.resume_node);
	put_le32(packet + 12, qr.resume_port);
	put_le32(packet + 20, 20);
	put_le32(packet + 24, qr.resume_remote_node);
	put_le32(packet + 28, qr.resume_remote_port);
	put_le32(packet + 32, 7);
	put_le32(packet + 36, qr.resume_node);
	put_le32(packet + 40, qr.resume_port);
	qr.resume_pending = false;
	return alioth_wlan_mhi_send_qrtr_packet(qr.mhi, packet, sizeof(packet), 5000, UINT16_MAX);
}

/* MHI's receive callback: every QRTR packet the firmware sends. */
static void
wifi_qrtr_packet(void *context, const void *data, size_t length)
{
	(void)context;
	const uint8_t *packet = data;
	if (length < QRTR_HEADER || le32(packet) != 1) {
		printf("ALIOTH WIFI: QRTR: %zu bytes, not a version 1 packet\n", length);
		return;
	}
	uint32_t type = le32(packet + 4), src_node = le32(packet + 8), src_port = le32(packet + 12);
	uint32_t size = le32(packet + 20), dst_port = le32(packet + 28);
	const uint8_t *payload = packet + QRTR_HEADER;
	if (size > length - QRTR_HEADER) {
		printf("ALIOTH WIFI: QRTR: type %u says %u bytes, has %zu\n", type, size, length - QRTR_HEADER);
		return;
	}
	if (type == 4 && size >= 20 && le32(payload) == 4) {           /* NEW_SERVER */
		uint32_t instance = le32(payload + 8);
		struct qrtr_service service = {
			.valid = true, .service = le32(payload + 4), .version = instance & 0xff, .instance = instance >> 8,
			.node = le32(payload + 12), .port = le32(payload + 16),
		};
		printf("ALIOTH WIFI: QRTR NEW_SERVER service %#x version %u instance %u at node %u port %#x%s\n", service.service,
		    service.version, service.instance, service.node, service.port,
		    service.service == 0x45 ? " = THE WLAN FIRMWARE SERVICE (WLFW)" : "");
		qr.announced++;
		qr.wlfw += service.service == 0x45;
		unsigned slot = QRTR_SERVICES;
		for (unsigned i = 0; i < QRTR_SERVICES; i++) {
			const struct qrtr_service *known = &qr.services[i];
			if (known->valid && known->service == service.service && known->instance == service.instance &&
			    known->node == service.node && known->port == service.port) {
				slot = i;
				break;
			}
			if (slot == QRTR_SERVICES && !known->valid) {
				slot = i;
			}
		}
		if (slot < QRTR_SERVICES) {
			qr.services[slot] = service;
			qr_notify(&service, true);
		}
		return;
	}
	if (type == 5 && size >= 20 && le32(payload) == 5) {           /* DEL_SERVER */
		uint32_t instance = le32(payload + 8);
		printf("ALIOTH WIFI: QRTR DEL_SERVER service %#x instance %u\n", le32(payload + 4), instance >> 8);
		for (unsigned i = 0; i < QRTR_SERVICES; i++) {
			struct qrtr_service *known = &qr.services[i];
			if (known->valid && known->service == le32(payload + 4) && known->instance == instance >> 8 &&
			    known->node == le32(payload + 12) && known->port == le32(payload + 16)) {
				struct qrtr_service gone = *known;
				known->valid = false;
				qr_notify(&gone, false);
			}
		}
		return;
	}
	if (type == 3) {                                                /* BYE: the node's services are gone */
		printf("ALIOTH WIFI: QRTR BYE from node %u\n", src_node);
		for (unsigned i = 0; i < QRTR_SERVICES; i++) {
			if (qr.services[i].valid && qr.services[i].node == src_node) {
				struct qrtr_service gone = qr.services[i];
				qr.services[i].valid = false;
				qr_notify(&gone, false);
			}
		}
		return;
	}
	if (type == 1 && dst_port == qr.port.number && qr.port.active && qr.port.receive != NULL) {
		if (size >= 7) {
			printf("ALIOTH WIFI: QMI in: type %u message %#x transaction %u, %u bytes\n", payload[0],
			    (unsigned)payload[3] | (unsigned)payload[4] << 8, (unsigned)payload[1] | (unsigned)payload[2] << 8, size);
		}
		qr.port.receive(qr.port.context, src_node, src_port, payload, size);
		if (le32(packet + 16)) {                                /* confirm_rx: answer before the next send */
			qr.resume_node = le32(packet + 24);
			qr.resume_port = dst_port;
			qr.resume_remote_node = src_node;
			qr.resume_remote_port = src_port;
			qr.resume_pending = true;
		}
		return;
	}
	static const char *const names[] = { "?", "DATA", "HELLO", "BYE", "NEW_SERVER", "DEL_SERVER", "DEL_CLIENT",
		                             "RESUME_TX", "EXIT", "PING", "NEW_LOOKUP", "DEL_LOOKUP" };
	printf("ALIOTH WIFI: QRTR %s from node %u port %#x, %u bytes\n", type < 12 ? names[type] : "?", src_node, src_port, size);
}

static struct redwood_qrtr_port *
qr_open_locked(redwood_qrtr_receive_t receive, redwood_qrtr_server_t server, void *context)
{
	struct redwood_qrtr_port *port = &qr.port;
	if (port->active || receive == NULL) {
		return NULL;
	}
	memset(port, 0, sizeof(*port));
	port->active = true;
	port->number = qr.next_port++;
	port->receive = receive;
	port->server = server;
	port->context = context;
	return port;
}

static int
qr_send_locked(struct redwood_qrtr_port *port, uint32_t node, uint32_t to_port, const void *data, size_t length)
{
	if (port != &qr.port || !port->active || qr.mhi == NULL || data == NULL ||
	    length > QRTR_PACKET_MAX - QRTR_HEADER) {
		return EMSGSIZE;
	}
	int error = qr_flush_resume();
	if (error) {
		return error;
	}
	size_t padded = (length + 3u) & ~(size_t)3u;
	memset(qr_tx, 0, QRTR_HEADER + padded);
	put_le32(qr_tx, 1);                     /* QRTR version 1 */
	put_le32(qr_tx + 4, 1);                 /* DATA */
	put_le32(qr_tx + 8, QRTR_NODE_APPS);
	put_le32(qr_tx + 12, port->number);
	put_le32(qr_tx + 20, (uint32_t)length);
	put_le32(qr_tx + 24, node);
	put_le32(qr_tx + 28, to_port);
	memcpy(qr_tx + QRTR_HEADER, data, length);
	const uint8_t *qmi = data;
	uint16_t transaction = UINT16_MAX;      /* a request: wait for the reply with its transaction too */
	if (length >= 7 && qmi[0] == 0) {
		transaction = (uint16_t)(qmi[1] | qmi[2] << 8);
		printf("ALIOTH WIFI: QMI out: message %#x transaction %u, %zu bytes\n", (unsigned)qmi[3] | (unsigned)qmi[4] << 8,
		    transaction, length);
	}
	error = alioth_wlan_mhi_send_qrtr_packet(qr.mhi, qr_tx, QRTR_HEADER + padded, 10000, transaction);
	return error ? error : qr_flush_resume();
}

static int
qr_lookup_locked(struct redwood_qrtr_port *port, uint32_t service, uint32_t version, uint32_t instance)
{
	if (port != &qr.port || !port->active) {
		return EINVAL;
	}
	unsigned slot;
	for (slot = 0; slot < QRTR_LOOKUPS && port->lookups[slot].valid; slot++) {
	}
	if (slot == QRTR_LOOKUPS) {
		return ENOSPC;
	}
	port->lookups[slot] = (struct qrtr_lookup) { .valid = true, .service = service, .version = version, .instance = instance };
	for (unsigned i = 0; i < QRTR_SERVICES; i++) {
		if (port->server != NULL && qr.services[i].valid && qr_matches(&port->lookups[slot], &qr.services[i])) {
			port->server(port->context, true, qr.services[i].service, qr.services[i].version, qr.services[i].instance,
			    qr.services[i].node, qr.services[i].port);
		}
	}
	return 0;
}

static int
qr_poll_locked(struct redwood_qrtr_port *port, uint32_t timeout_ms)
{
	unsigned received = 0;
	if (port != &qr.port || !port->active || qr.mhi == NULL) {
		return EINVAL;
	}
	int error = alioth_wlan_mhi_poll_ipcr(qr.mhi, timeout_ms < 1 ? 1 : timeout_ms > 10000 ? 10000 : timeout_ms, &received);
	return error ? error : qr_flush_resume();
}

static void
qr_close_locked(struct redwood_qrtr_port *port)
{
	if (port == &qr.port) {
		memset(port, 0, sizeof(*port));
	}
}

static struct redwood_qrtr_port *
qr_open(redwood_qrtr_receive_t receive, redwood_qrtr_server_t server, void *context)
{
	lck_mtx_lock(&wifi_mhi_lock);
	struct redwood_qrtr_port *port = qr_open_locked(receive, server, context);
	lck_mtx_unlock(&wifi_mhi_lock);
	return port;
}

static int
qr_send(struct redwood_qrtr_port *port, uint32_t node, uint32_t to_port, const void *data, size_t length)
{
	lck_mtx_lock(&wifi_mhi_lock);
	int error = qr_send_locked(port, node, to_port, data, length);
	lck_mtx_unlock(&wifi_mhi_lock);
	return error;
}

static int
qr_lookup(struct redwood_qrtr_port *port, uint32_t service, uint32_t version, uint32_t instance)
{
	lck_mtx_lock(&wifi_mhi_lock);
	int error = qr_lookup_locked(port, service, version, instance);
	lck_mtx_unlock(&wifi_mhi_lock);
	return error;
}

static int
qr_poll(struct redwood_qrtr_port *port, uint32_t timeout_ms)
{
	lck_mtx_lock(&wifi_mhi_lock);
	int error = qr_poll_locked(port, timeout_ms);
	lck_mtx_unlock(&wifi_mhi_lock);
	return error;
}

static void
qr_close(struct redwood_qrtr_port *port)
{
	lck_mtx_lock(&wifi_mhi_lock);
	qr_close_locked(port);
	lck_mtx_unlock(&wifi_mhi_lock);
}

static const struct redwood_qrtr_ops qr_ops = {
	.open = qr_open, .send = qr_send, .lookup = qr_lookup, .poll = qr_poll, .close = qr_close,
};

/* ---- the steps -------------------------------------------------------------------------- */

const uint8_t *
alioth_wlan_wifi_file(const char *name, unsigned int *size)
{
	extern const uint8_t *redwood_image_file(const char *, const char *, uint64_t *, int *);
	const char *file = NULL;
	if (!strcmp(name, "amss")) file = "amss20.bin";
	else if (!strcmp(name, "m3")) file = "m3.bin";
	else if (!strcmp(name, "board")) file = "board.bin";
	else if (!strcmp(name, "regdb")) file = "regdb.bin";
	else if (!strcmp(name, "mac")) file = "wifi-mac";
	else if (!strcmp(name, "network")) file = "network";
	uint64_t length = 0;
	const uint8_t *bytes = file ? redwood_image_file("fw", file, &length, NULL) : NULL;
	if (!bytes || length > UINT32_MAX) { *size = 0; return NULL; }
	*size = (unsigned int)length;
	return bytes;
}

/* ---- the radio's session: the steps, and what the driver asks for (alioth_wlan_lx.c) ---------------- */

static const struct alioth_wlan_pcie_io wifi_pcie_io = {
	.context = NULL, .read32 = pcie_read, .write32 = pcie_write, .power_on = pcie_power,
	.delay_us = pcie_delay, .report = pcie_report,
};
static const struct alioth_wlan_mhi_io wifi_mhi_io = {
	.context = NULL, .bar_size = 4096, .read32 = mhi_read, .write32 = mhi_write,
	.allocate = wifi_allocate, .sync_for_device = wifi_sync, .delay_us = pcie_delay,
	.report = pcie_report, .qrtr_packet = wifi_qrtr_packet,
};
static struct alioth_wlan_pcie wifi_pcie;
static struct alioth_wlan_mhi wifi_mhi;
static uint64_t wifi_msi_iova;          /* the GIC's MSI register as the radio sees it; 0 until step 4 */
static bool wifi_keep;                  /* step 12 with the driver up: the radio is left running */
static bool wifi_polling, wifi_poll_done = true;

/*
 * Steps 4-7 on the enumerated endpoint, with the SMMU context installed: the firmware's first
 * 512 KiB (BHI), MHI, the whole firmware (BHIe) and mission mode, QRTR's channels and hello.
 * Under wifi_mhi_lock. The driver has it done again when it restarts the radio after calibration.
 */
static int
radio_up_locked(unsigned steps, const uint8_t *amss, unsigned int size)
{
	memset(&wifi_mhi, 0, sizeof(wifi_mhi));
	int error = alioth_wlan_mhi_probe(&wifi_mhi, &wifi_mhi_io);
	if (!error) {
		error = alioth_wlan_pcie_prepare_radio(&wifi_pcie);
	}
	if (!error && wifi_msi_iova == 0) {
		uint64_t msi = 0;
		if (radio_map(RADIO_MSI_REGISTER & ~4095ull, 0, 4096, RADIO_PTE_DEVICE, &msi)) {
			wifi_msi_iova = msi + (RADIO_MSI_REGISTER & 4095);
		} else {
			error = ENOMEM;
		}
	}
	if (!error) {
		error = alioth_wlan_pcie_setup_mhi_msi(&wifi_pcie, wifi_msi_iova, RADIO_MSI_DATA);
	}
	if (!error) {
		error = alioth_wlan_pcie_enable_dma(&wifi_pcie);
	}
	if (!error) {
		error = alioth_wlan_mhi_download_sbl(&wifi_mhi, amss, size);
		printf("ALIOTH WIFI: step 4: BHI download of the first 512 KiB: errno %d\n", error);
	}
	if (!error && steps >= 5) {
		error = alioth_wlan_mhi_prepare_ready(&wifi_mhi);
		printf("ALIOTH WIFI: step 5: MHI READY, contexts, M0: errno %d\n", error);
	}
	if (!error && steps >= 6) {
		error = alioth_wlan_mhi_download_amss(&wifi_mhi, amss, size);
		if (!error) {
			error = alioth_wlan_mhi_wait_mission(&wifi_mhi);
		}
		printf("ALIOTH WIFI: step 6: BHIe download and mission mode: errno %d%s\n", error,
		    error ? "" : ": THE WI-FI FIRMWARE RUNS");
	}
	if (!error && steps >= 7) {
		error = alioth_wlan_mhi_start_ipcr(&wifi_mhi);
		if (!error) {
			error = alioth_wlan_mhi_send_qrtr_hello(&wifi_mhi);
		}
		printf("ALIOTH WIFI: step 7: QRTR channels 20/21 and hello: errno %d\n", error);
	}
	return error;
}

/* With the driver: MHI's rings looked at every millisecond, for the QMI replies and indications it waits for. */
static void
wifi_poll_thread(void *arg, wait_result_t wr)
{
	(void)arg;
	(void)wr;
	while (__atomic_load_n(&wifi_polling, __ATOMIC_ACQUIRE)) {
		int error = qr_poll(&qr.port, 1);
		if (error && error != EINVAL && error != EBUSY) {
			printf("ALIOTH WIFI: looking at MHI's rings failed: errno %d; they are no longer looked at\n", error);
			break;
		}
		struct timespec pause = { .tv_sec = 0, .tv_nsec = 1000000 };
		(void)msleep(&wifi_polling, NULL, PZERO, "aliothwifimhi", &pause);
	}
	__atomic_store_n(&wifi_poll_done, true, __ATOMIC_RELEASE);
	(void)thread_terminate(current_thread());
}

static void
wifi_poll_stop(void)
{
	__atomic_store_n(&wifi_polling, false, __ATOMIC_RELEASE);
	wakeup(&wifi_polling);
	while (!__atomic_load_n(&wifi_poll_done, __ATOMIC_ACQUIRE)) {
		delay(1000);
	}
}

/*
 * Steps 11 and 12: Linux's ath11k (alioth_wlan_lx.c) on the running firmware, through the same QRTR.
 * It does its own handshake -- calibration, then a restart of the radio (alioth_wlan_wifi_radio_reset,
 * alioth_wlan_wifi_radio_boot) -- and then scans and goes down (11), or offers the station to iOS and
 * stays (12).
 */
static int
wifi_driver(unsigned steps)
{
	thread_t thread;
	redwood_qmi_set_transport(&qr_ops);
	__atomic_store_n(&wifi_poll_done, false, __ATOMIC_RELAXED);
	__atomic_store_n(&wifi_polling, true, __ATOMIC_RELEASE);
	if (kernel_thread_start(wifi_poll_thread, NULL, &thread) != KERN_SUCCESS) {
		__atomic_store_n(&wifi_polling, false, __ATOMIC_RELEASE);
		__atomic_store_n(&wifi_poll_done, true, __ATOMIC_RELEASE);
		redwood_qmi_set_transport(NULL);
		printf("ALIOTH WIFI: step %u: no thread to look at MHI's rings\n", steps);
		return EAGAIN;
	}
	thread_set_thread_name(thread, "sweet-wifi-mhi");
	thread_deallocate(thread);
	printf("ALIOTH WIFI: step %u: Linux's ath11k takes the radio: its handshake, then %s\n", steps,
	    steps == 11 ? "a scan, then down again" : "en0 for iOS");
	int error = redwood_lx_start();
	if (!error && steps == 12) {
		wifi_keep = true;
		printf("ALIOTH WIFI: step 12: THE DRIVER IS UP: en0 is the radio's; wifid scans and joins through it\n");
		return 0;
	}
	printf("ALIOTH WIFI: step %u: the driver: errno %d%s\n", steps, error,
	    error ? "" : ": IT CAME UP AND SCANNED (the networks are above)");
	redwood_lx_stop();
	wifi_poll_stop();
	redwood_qmi_set_transport(NULL);
	return error;
}

/* Stop while threads still run; the return path subsequently resets the endpoint. */
void
alioth_wlan_wifi_stop(void)
{
	if (!wifi_keep) return;
	wifi_keep = false;
	redwood_lx_stop();
	wifi_poll_stop();
	redwood_qmi_set_transport(NULL);
}

/* Also safe on a panic after the other CPUs are stopped: no sleeping or locks. */
void
alioth_wlan_wifi_quiesce(void)
{
	if (!wifi_pcie.io) return;
	alioth_wlan_pcie_quiesce(&wifi_pcie);
	if (rs.found) smmu_restore();
}

/* Steps 4 on, with the SMMU context installed and the endpoint enumerated. */
static int
wifi_firmware(unsigned steps)
{
	unsigned int size = 0;
	const uint8_t *amss = alioth_wlan_wifi_file("amss", &size);
	if (!amss || size < 512u * 1024u) {
		printf("ALIOTH WIFI: step 4: no firmware in the ADT (/sweet-wifi \"amss\": mkadt.py --wifi-fw amss.bin)\n");
		return ENOENT;
	}
	uint32_t hash = 2166136261u;
	for (unsigned i = 0; i < size; i++) {
		hash = (hash ^ amss[i]) * 16777619u;
	}
	printf("ALIOTH WIFI: step 4: firmware %u bytes, FNV32 %#x, starts %02x%02x%02x%02x\n", size, hash, amss[0], amss[1],
	    amss[2], amss[3]);
	lck_mtx_lock(&wifi_mhi_lock);
	memset(&qr, 0, sizeof(qr));
	qr.next_port = QRTR_PORT_FIRST;
	qr.mhi = &wifi_mhi;
	int error = radio_up_locked(steps, amss, size);
	if (!error && steps >= 7) {
		unsigned received = 0;
		for (unsigned waited = 0; !error && waited < wifi_poll_ms; waited += 1000) {
			unsigned now = 0;
			error = alioth_wlan_mhi_poll_ipcr(&wifi_mhi, 1000, &now);
			received += now;
		}
		printf("ALIOTH WIFI: step 7: QRTR errno %d, %u packets, %u services announced%s\n", error, received,
		    qr.announced, qr.wlfw ? ": THE WLAN FIRMWARE SERVICE IS UP" : "");
	}
	lck_mtx_unlock(&wifi_mhi_lock);
	if (!error && steps >= 8 && steps <= 10) {
		redwood_qmi_set_transport(&qr_ops);
		error = alioth_wlan_wlfw_run(steps);
		redwood_qmi_set_transport(NULL);
	}
	if (!error && steps >= 11) {
		error = wifi_driver(steps);
	}
	lck_mtx_lock(&wifi_mhi_lock);
	uint32_t ee = UINT32_MAX;
	if (wifi_mhi.io) {
		(void)alioth_wlan_mhi_execution_environment(&wifi_mhi, &ee);
	}
	if (error && wifi_mhi.io) {
		printf("ALIOTH WIFI: BHI error %#x debug %#x/%#x/%#x, MHI status %#x\n", mhi_read(NULL, wifi_mhi.bhi + 0x30),
		    mhi_read(NULL, wifi_mhi.bhi + 0x34), mhi_read(NULL, wifi_mhi.bhi + 0x38), mhi_read(NULL, wifi_mhi.bhi + 0x3c),
		    mhi_read(NULL, 0x48));
	}
	lck_mtx_unlock(&wifi_mhi_lock);
	printf("ALIOTH WIFI: firmware steps up to %u: errno %d, execution environment %#x\n", steps, error, ee);
	return error;
}

static int
wifi_run(unsigned steps)
{
	if (!wifi_map_regions()) {
		return ENOMEM;
	}
	wifi_survey();
	if (steps < 2) {
		return 0;
	}
	printf("ALIOTH WIFI: step 2: rails, wlan-en, PCIe0 link; bus mastering stays off\n");
	memset(&wifi_pcie, 0, sizeof(wifi_pcie));
	int error = alioth_wlan_pcie_enumerate(&wifi_pcie, &wifi_pcie_io);
	if (!error) {
		pcie_report(NULL, "MHICFG", pcie_read(NULL, WIFI_BAR + 0x10));
		pcie_report(NULL, "BHI offset", pcie_read(NULL, WIFI_BAR + 0x28));
		pcie_report(NULL, "BHIE offset", pcie_read(NULL, WIFI_BAR + 0x2c));
		printf("ALIOTH WIFI: step 2: THE QCA6390 ANSWERS ON PCIe0\n");
		smmu_survey();
	}
	if (!error && steps >= 3) {
		error = smmu_install() ? 0 : EIO;
		printf("ALIOTH WIFI: step 3: SMMU context for the radio: %s\n", error ? "refused" : "taken");
	}
	if (!error && steps >= 4) {
		error = wifi_firmware(steps);
	}
	if (wifi_keep) {
		return error;
	}
	/* Reset propagates to the endpoint before its translation goes away. */
	lck_mtx_lock(&wifi_mhi_lock);
	alioth_wlan_pcie_quiesce(&wifi_pcie);
	lck_mtx_unlock(&wifi_mhi_lock);
	smmu_restore();
	return error;
}

/*
 * The driver's restart of the radio (ath11k's power_up after the calibration's power_down: the
 * firmware is booted again from the start, as at steps 2 and 4-7). Its first power_up finds the
 * radio running and leaves it so.
 */
int
alioth_wlan_wifi_radio_boot(void)
{
	unsigned int size = 0;
	const uint8_t *amss = alioth_wlan_wifi_file("amss", &size);
	lck_mtx_lock(&wifi_mhi_lock);
	int error = 0;
	if (wifi_mhi.io && wifi_mhi.ipcr_started && !wifi_mhi.poisoned) {
		goto out;
	}
	if (!rs.installed || !amss || size < 512u * 1024u) {
		error = ENXIO;
		goto out;
	}
	/* (boot 55: once the link did not come back; so: a moment with the radio off first, and
	 * three tries) */
	for (unsigned attempt = 1;; attempt++) {
		delay(100000);
		memset(&wifi_pcie, 0, sizeof(wifi_pcie));
		error = alioth_wlan_pcie_enumerate(&wifi_pcie, &wifi_pcie_io);
		if (!error) {
			error = radio_up_locked(7, amss, size);
		}
		if (!error || attempt == 3) {
			break;
		}
		printf("ALIOTH WIFI: the driver restarts the radio: attempt %u: errno %d; again\n", attempt, error);
		alioth_wlan_pcie_quiesce(&wifi_pcie);
	}
	if (error) {
		alioth_wlan_pcie_quiesce(&wifi_pcie);
	}
	printf("ALIOTH WIFI: the driver restarts the radio: errno %d\n", error);
out:
	lck_mtx_unlock(&wifi_mhi_lock);
	return error;
}

/* The driver's power_down: the endpoint in reset; its memory, the firmware's included, is kept. */
void
alioth_wlan_wifi_radio_reset(void)
{
	lck_mtx_lock(&wifi_mhi_lock);
	alioth_wlan_pcie_quiesce(&wifi_pcie);
	wifi_mhi.ipcr_started = false;
	for (unsigned i = 0; i < QRTR_SERVICES; i++) {
		if (qr.services[i].valid) {
			struct qrtr_service gone = qr.services[i];
			qr.services[i].valid = false;
			qr_notify(&gone, false);
		}
	}
	qr.resume_pending = false;
	printf("ALIOTH WIFI: the driver puts the radio in reset\n");
	lck_mtx_unlock(&wifi_mhi_lock);
}

/* As wifi_allocate's first way: one physical run, used through a second, non-cacheable mapping. */
void *
alioth_wlan_wifi_radio_memory(size_t size, unsigned long long *address)
{
	vm_offset_t cached = 0;
	uint64_t iova = 0;
	size = round_page(size);
	if (size == 0 || kmem_alloc_contig(kernel_map, &cached, size, 0, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("ALIOTH WIFI: no %zu KiB in one piece for the radio\n", size >> 10);
		return NULL;
	}
	uint64_t pa = kvtophys(cached);
	alioth_wlan_wifi_clean((void *)cached, size);
	vm_offset_t uncached = ml_io_map_unmappable(pa, size, VM_WIMG_WCOMB);
	if (!uncached || !radio_map(pa, 0, size, RADIO_PTE, &iova)) {
		return NULL;
	}
	*address = iova;
	return (void *)uncached;
}

/*
 * The memory the firmware asks for (types as in its request: 1 its main region, 4 calibration,
 * 3 M3's image) at stock CNSS's fixed addresses, made once and given again after the restart.
 * M3's is written by the processor (the driver copies m3.bin in): one run, non-cacheable. The
 * other two only the radio uses: cached pages, cleaned once (alioth_wlan_wifi_memory_at).
 */
void *
alioth_wlan_wifi_firmware_memory(unsigned type, size_t size, unsigned long long *address)
{
	static struct {
		unsigned type;
		uint64_t iova;
		size_t largest, size;
		void *memory;
	} regions[] = {
		{ 1, 0xa1000000ull, 0x600000, 0, NULL },
		{ 4, 0xa1600000ull, 0x280000, 0, NULL },
		{ 3, 0xa1880000ull, 0x80000, 0, NULL },
	};
	for (unsigned i = 0; i < sizeof(regions) / sizeof(regions[0]); i++) {
		if (regions[i].type != type) {
			continue;
		}
		if (size == 0 || size > regions[i].largest || (regions[i].memory != NULL && size > regions[i].size)) {
			printf("ALIOTH WIFI: the firmware's memory of type %u: %#zx bytes do not fit at %#llx\n", type, size,
			    regions[i].iova);
			return NULL;
		}
		if (regions[i].memory == NULL) {
			if (type == 3) {
				vm_offset_t cached = 0;
				size_t whole = round_page(size);
				if (kmem_alloc_contig(kernel_map, &cached, whole, 0, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
				    VM_KERN_MEMORY_IOKIT) == KERN_SUCCESS) {
					uint64_t pa = kvtophys(cached);
					alioth_wlan_wifi_clean((void *)cached, whole);
					vm_offset_t uncached = ml_io_map_unmappable(pa, whole, VM_WIMG_WCOMB);
					if (uncached && radio_map_fixed(pa, 0, whole, RADIO_PTE, regions[i].iova)) {
						regions[i].memory = (void *)uncached;
					}
				}
			} else {
				regions[i].memory = alioth_wlan_wifi_memory_at(size, regions[i].iova);
			}
			if (regions[i].memory == NULL) {
				return NULL;
			}
			regions[i].size = round_page(size);
			printf("ALIOTH WIFI: the firmware's memory of type %u: %#zx bytes at %#llx\n", type, size, regions[i].iova);
		}
		*address = regions[i].iova;
		return regions[i].memory;
	}
	printf("ALIOTH WIFI: the firmware asks for memory of type %u: not known\n", type);
	return NULL;
}

void *
alioth_wlan_wifi_registers(unsigned long long physical, size_t size)
{
	unsigned long long page = trunc_page(physical);
	vm_offset_t mapped = ml_io_map_unmappable((vm_offset_t)page, (vm_size_t)(round_page(physical + size) - page), VM_WIMG_IO);
	return mapped == 0 ? NULL : (void *)(mapped + (physical - page));
}

bool
alioth_wlan_wifi_msi(unsigned long long *address, unsigned *data)
{
	if (wifi_msi_iova == 0) {
		return false;
	}
	*address = wifi_msi_iova;
	*data = RADIO_MSI_DATA;
	return true;
}

/*
 * The radio's own address. Its firmware has none: Android hands it the phone's from its storage.
 * Boot-arg aliothwifimac=aa:bb:cc:dd:ee:ff gives it; else the ADT's /chosen "mac-address-wifi0"
 * (mkadt.py --wifi-fw, xnu 0129: the address MobileGestalt reports as the Wi-Fi address, so the
 * two agree); without either a random, locally administered one is made, the same for the whole
 * boot. The log says which way, not the address.
 */
bool
alioth_wlan_wifi_radio_address(uint8_t *address)
{
	static uint8_t made[6];
	static bool have;
	if (!have) {
		char text[32] = "";
		bool given = PE_parse_boot_arg_str("aliothwifimac", text, sizeof(text)) && text[0] != 0, good = given;
		unsigned count = 0, digits = 0, value = 0;
		for (const char *c = text; good; c++) {
			int digit = *c >= '0' && *c <= '9' ? *c - '0' : *c >= 'a' && *c <= 'f' ? *c - 'a' + 10 :
			    *c >= 'A' && *c <= 'F' ? *c - 'A' + 10 : -1;
			if (digit >= 0 && digits < 2) {
				value = value << 4 | (unsigned)digit;
				digits++;
			} else if ((*c == ':' || *c == 0) && digits != 0 && count < 6) {
				made[count++] = (uint8_t)value;
				digits = value = 0;
			} else {
				good = false;
			}
			if (*c == 0) {
				break;
			}
		}
		good = good && count == 6 && !(made[0] & 1) && (made[0] | made[1] | made[2] | made[3] | made[4] | made[5]) != 0;
		bool chosen = false;
		DTEntry node;
		void const *prop = NULL;
		unsigned int size = 0;
		if (!good && SecureDTLookupEntry(NULL, "/chosen", &node) == kSuccess &&
		    SecureDTGetProperty(node, "mac-address-wifi0", &prop, &size) == kSuccess && size == 6) {
			const uint8_t *adt = prop;
			chosen = !(adt[0] & 1) && (adt[0] | adt[1] | adt[2] | adt[3] | adt[4] | adt[5]) != 0;
			if (chosen) {
				memcpy(made, adt, sizeof(made));
			}
		}
		if (!good && !chosen) {
			read_random(made, sizeof(made));
			made[0] = (uint8_t)((made[0] & 0xfe) | 0x02);   /* one station's, locally administered */
		}
		have = true;
		printf("ALIOTH WIFI: the radio's address: %s\n", good ? "aliothwifimac, as given" : chosen ?
		    "the ADT's /chosen mac-address-wifi0" : given ? "SWEETWIFIMAC IS NOT AN ADDRESS: a random one for this boot" :
		    "a random one for this boot (no aliothwifimac, no /chosen mac-address-wifi0)");
	}
	memcpy(address, made, sizeof(made));
	return true;
}

/* A network for the driver to join by itself (HackiOS's wifi_join): none here, iOS joins from Settings. */
bool
alioth_wlan_wifi_network(uint8_t *name, unsigned *name_length, uint8_t *passphrase, unsigned *passphrase_length)
{
	(void)name;
	(void)name_length;
	(void)passphrase;
	(void)passphrase_length;
	return false;
}

/*
 * The kernel's options for the driver (wifi/lx asks by HackiOS's boot-arg names). Fixed here for
 * this radio: the Mi 10 layer (wifi_umi), the whole driver (wifi_lx=2), polled rings (wifi_irq=0),
 * a scan at step 11 or the station for iOS at step 12. Any other is the boot-arg of that name.
 */
unsigned
alioth_wlan_wifi_option(const char *name)
{
	static const struct {
		const char *name;
		unsigned value;
	} fixed[] = {
		{ "wifi_umi", 1 }, { "wifi_lx", 2 }, { "wifi_irq", 0 }, { "wifi_ba", 1 }, { "wifi_wide", 1 },
	};
	unsigned value = 0;
	for (unsigned i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
		if (strcmp(name, fixed[i].name) == 0) {
			return fixed[i].value;
		}
	}
	if (strcmp(name, "wifi_scan") == 0) {
		return wifi_steps == 11;
	}
	if (strcmp(name, "wifi_station") == 0) {
		return wifi_steps == 12;
	}
	if (strcmp(name, "wifi_join") == 0) {
		return 0;
	}
	(void)PE_parse_boot_argn(name, &value, sizeof(value));
	return value;
}

static void
wifi_thread(void *arg, wait_result_t wr)
{
	(void)arg;
	(void)wr;
	int chan = 0;
	if (wifi_steps >= 11) {
		/* 64 MiB in one physical run for the driver's rings and buffers: early, before memory is broken up */
		int error = redwood_lx_prepare();
		printf("ALIOTH WIFI: the driver's memory and clock: errno %d\n", error);
	}
	if (wifi_delay) {
		(void)tsleep(&chan, PZERO, "aliothwifi", (int)wifi_delay * hz);
	}
	printf("ALIOTH WIFI: steps 1..%u\n", wifi_steps);
	int error = wifi_run(wifi_steps);
	printf("ALIOTH WIFI: done: steps 1..%u, errno %d; %s\n", wifi_steps, error,
	    wifi_keep ? "the radio stays up behind en0" : "the radio is held in reset");
	(void)thread_terminate(current_thread());
}

void
alioth_wlan_wifi_init(void)
{
	thread_t thread;
	(void)PE_parse_boot_argn("aliothwifi", &wifi_steps, sizeof(wifi_steps));
	(void)PE_parse_boot_argn("aliothwifidelay", &wifi_delay, sizeof(wifi_delay));
	(void)PE_parse_boot_argn("aliothwifipoll", &wifi_poll_ms, sizeof(wifi_poll_ms));
	/*
	 * Not the radio's, but here because this runs early in bsd_init: the registry is let go quiet
	 * as on iOS (SweetNetworkStack.cpp, xnu 0131; every boot, sweetiokitquiet=0 leaves it).
	 */
	uint32_t quiet = 1;
	(void)PE_parse_boot_argn("sweetiokitquiet", &quiet, sizeof(quiet));
	if (quiet) {
		AliothWLANRegistryReady();
	}
	if (!wifi_steps) {
		return;
	}
	if (wifi_steps > 12) {
		wifi_steps = 12;
	}
	if (wifi_poll_ms < 1000 || wifi_poll_ms > 60000) {
		wifi_poll_ms = 5000;
	}
	if (wifi_steps >= 11) {
		redwood_net_init();
		/* before configd: its interface namer says the registry is quiet only once it has opened this */
		/* MiniIONetworkStack is already started by GenericARMPE. */
	}
	if (wifi_steps == 12) {
		redwood_lx_net_early();          /* en0 and its registry entries before wifid looks for them */
	}
	if (kernel_thread_start(wifi_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("ALIOTH WIFI: kernel_thread_start failed\n");
	}
}
#endif /* PLATFORM_ALIOTH */
