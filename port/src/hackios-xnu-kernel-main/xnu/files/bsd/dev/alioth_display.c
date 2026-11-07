/*
 * Alioth adaptation, 2026-11-06: same SM8250 DPU/registers, clocks and reserved region
 * checked against diagnostics/k40-stock.dts. Based on pinned ios-apollo patch 0156.
 * apollo (xnu 0156): page flips of the panel's scanout, so that SweetFramebuffer.cpp draws each frame where the panel is
 * not reading and switches the panel to it between two of its refreshes: no tearing (boot 87: "the screen is tearing").
 * Taken from the Mi 10T port github.com/enesuzun2002/hakios-apollo (devices/mi-10t/xnu/files/bsd/dev/
 * hakios.apollo_display.c, APSL 2.0: find_ctl(), dpu_setup(), smmu_look(), apollo_display_flip(),
 * apollo_display_flip_latched(), flip_reserve()), whose apollo_flip=1 flips this phone's display the same way; only that
 * part, without their timing changes, dumps, test and triple-buffer thread (the framebuffer's present thread does it here).
 *
 * The UEFI left the display controller (the SM8250's DPU) running: a source pipe fetches the boot framebuffer
 * (0x9c000000, 1080x2400, stride 4320; boot 48's probe: VIG0) through a layer mixer and INTF_1 to the DSC video-mode panel
 * (144 Hz). A flip writes the pipe's SRC0_ADDR (MDP 0x0ae00000 + the pipe's offset + 0x14) and then the pipe's bit in the
 * CTL_FLUSH of the CTL that drives INTF_1; the hardware takes the address at the next frame's start and clears the bit
 * (alioth_display_flip_latched()). The frames are in the display's own reservation (the device tree's cont_splash_region
 * 0x9c000000, 0x2300000 bytes; with mkadt.py --highmem the kernel's memory starts at 4 GB and never has it): the boot
 * framebuffer and two more behind it, write-combined. Flips are refused when the display's SMMU stream would translate the
 * address (the stream's match register says translate with the context's MMU on, or fault): then nothing changes.
 * Boot-arg aliothflip=0: no flips (the copy into the scanned buffer, as before).
 */
#if defined(PLATFORM_ALIOTH)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <machine/machine_routines.h>

int alioth_display_log(const char *, ...) __printflike(1, 2);
extern void loader_log_line(const char *);
extern void loader_log_flush(void);
int
alioth_display_log(const char *format, ...)
{
    char buffer[512];
    va_list args;
    va_start(args, format);
    int count = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    loader_log_line(buffer);
    loader_log_flush();
    return count;
}
#define printf alioth_display_log

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, uint32_t);
/* osfmk/arm/memory_types.h, not visible to BSD code (as in sweet_gpu.c) */
#ifndef VM_WIMG_IO
#define VM_WIMG_IO              0x7u
#endif
#ifndef VM_WIMG_WCOMB
#define VM_WIMG_WCOMB           0x6u
#endif

int alioth_display_flip_init(uint64_t fb_phys, uint32_t stride, uint32_t height, uint32_t *frame_stride, uint64_t pa[3], vm_offset_t va[3]);
bool alioth_display_flip(uint64_t phys, uint32_t stride);
bool alioth_display_flip_latched(void);
void alioth_display_quiesce(void);

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

#define INTF_PHYS               0x0ae6b800UL
#define INTF_SIZE               0x2c0UL
#define INTF_FRAME_COUNT        0x0ac

#define CTL_PHYS                0x0ae02000UL
#define CTL_STRIDE              0x200UL
#define CTL_SIZE                0x1e0UL
#define CTL_COUNT               6
#define CTL_TOP                 0x014
#define CTL_FLUSH               0x018
#define CTL_INTF_ACTIVE         0x0f4
#define CTL_INTF_ACTIVE_INTF1   (1u << 1)

#define MDP_PHYS                0x0ae00000UL
#define MDP_SIZE                0x84208UL
#define SSPP_COUNT              8
static const struct {
	const char *name;
	uint32_t offset;
	uint32_t flush_bit;
} sspp_table[SSPP_COUNT] = {
	{ "VIG0", 0x05000, 0 }, { "VIG1", 0x07000, 1 }, { "VIG2", 0x09000, 2 }, { "VIG3", 0x0b000, 18 },
	{ "DMA0", 0x25000, 11 }, { "DMA1", 0x27000, 12 }, { "DMA2", 0x29000, 24 }, { "DMA3", 0x2b000, 25 },
};
#define SSPP_SRC0_ADDR          0x14
#define SSPP_SRC_YSTRIDE0       0x24

#define SMMU_PHYS               0x15000000UL
#define SMMU_SIZE               0x100000UL
#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_SMR(n)             (0x800 + 4 * (n))
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))
#define SMMU_CB_SCTLR           0x00
#define MDSS_STREAM             0x820   /* the device tree's smmu_sde_unsec_cb iommus <&apps_smmu 0x820 0x402> */
#define MDSS_STREAM_MASK        0x402

#define DISPLAY_RESERVED_PHYS   0x9c000000ull
#define DISPLAY_RESERVED_SIZE   0x2300000ull    /* cont_splash_region (hakios: the UEFI's 0x2400000) */

static vm_offset_t mdp, ctl_base[CTL_COUNT];
static int ctl_driving = -1, sspp_found = -1;
static uint32_t flip_bit;
static bool ready;

static void
probe_step(const char *what)
{
    uint32_t probe = 0;
    (void)PE_parse_boot_argn("aliothflip_probe", &probe, sizeof(probe));
    printf("ALIOTH DISPLAY: %s\n", what);
    if (probe) delay(2000000);
}

static void
find_ctl(void)
{
	int first_top = -1, active = -1;

	for (int n = 0; n < CTL_COUNT; n++) {
		ctl_base[n] = ml_io_map(CTL_PHYS + CTL_STRIDE * (uint32_t)n, CTL_SIZE);
		if (ctl_base[n] == 0) continue;
		uint32_t top = rd(ctl_base[n] + CTL_TOP), intf_active = rd(ctl_base[n] + CTL_INTF_ACTIVE);
		if (active < 0 && (intf_active & CTL_INTF_ACTIVE_INTF1)) {
			active = n;
		}
		if (first_top < 0 && top != 0) {
			first_top = n;
		}
	}
	ctl_driving = active >= 0 ? active : first_top;
}

/* hakios's smmu_look(): true if an address of ours would be translated (or fault) for the display's stream */
static bool
smmu_blocks(void)
{
	vm_offset_t base = ml_io_map_unmappable(SMMU_PHYS, SMMU_SIZE, VM_WIMG_IO);
	uint32_t scr0, matches, contexts, page, pages, found = 0;
	bool blocks = false;

	if (base == 0) {
		return true;
	}
	scr0 = rd(base + SMMU_SCR0);
	matches = rd(base + SMMU_IDR0) & 0xff;
	contexts = rd(base + SMMU_IDR1) & 0xff;
	page = (rd(base + SMMU_IDR1) >> 31) ? 0x10000 : 0x1000;
	pages = 2u << (rd(base + SMMU_IDR1) >> 28 & 7);
	if (scr0 & 1u << 3) {
		printf("ALIOTH DISPLAY: the SMMU has 16-bit stream ids: not looked at, no flips\n");
		return true;
	}
	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(base + SMMU_SMR(n)), s2cr = rd(base + SMMU_S2CR(n));
		uint32_t stream = smr & 0x7fff, mask = smr >> 16 & 0x7fff, type = s2cr >> 16 & 3;

		if (!(smr >> 31) || ((stream ^ MDSS_STREAM) & ~mask & ~MDSS_STREAM_MASK) != 0) {
			continue;
		}
		found++;
		if (type == 0 && (s2cr & 0xff) < contexts) {
			uint32_t sctlr = rd(base + (pages + (s2cr & 0xff)) * page + SMMU_CB_SCTLR);
			printf("ALIOTH DISPLAY: the display's stream: match %u, translate in context %u (MMU %s)\n", n, s2cr & 0xff,
			    (sctlr & 1) ? "on" : "off");
			blocks = blocks || (sctlr & 1);
		} else {
			printf("ALIOTH DISPLAY: the display's stream: match %u, %s\n", n, type == 1 ? "bypass" : "fault");
			blocks = blocks || type == 2;
		}
	}
	if (found == 0 && !(scr0 & 1) && (scr0 >> 10 & 1)) {
		blocks = true;          /* unmatched streams fault */
	}
	return blocks;
}

/*
 * Ready the flips: the pipe that scans `fb_phys`, the CTL that drives INTF_1, the SMMU; the boot framebuffer (frame 0) and
 * two frames behind it in the display's reservation, mapped write-combined into va[1], va[2] (va[0] is the caller's).
 * The number of frames (3), or 0: no flips (nothing was written to the display).
 */
int
alioth_display_flip_init(uint64_t fb_phys, uint32_t stride, uint32_t height, uint32_t *frame_stride, uint64_t pa[3], vm_offset_t va[3])
{
	uint32_t enabled = 0, scan = 0;
	uint64_t bytes;
    PE_parse_boot_argn("aliothscan", &scan, sizeof(scan));
    uint32_t gpu_stride = scan ? (stride + 63u) & ~63u : stride;

	(void)PE_parse_boot_argn("aliothflip", &enabled, sizeof(enabled));
	if (!enabled) {
		printf("ALIOTH DISPLAY: aliothflip=0: frames are copied into the scanned buffer\n");
		return 0;
	}
	if (fb_phys != DISPLAY_RESERVED_PHYS || stride == 0 || stride > 0xffff || height == 0) {
		printf("ALIOTH DISPLAY: the framebuffer is at %#llx, not at the display's reservation: no flips\n", fb_phys);
		return 0;
	}
	bytes = ((uint64_t)gpu_stride * height + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
	if (3 * bytes > DISPLAY_RESERVED_SIZE) {
		printf("ALIOTH DISPLAY: three frames of %llu bytes do not fit the display's reservation: no flips\n", bytes);
		return 0;
	}
    /* The firmware gates register clocks after leaving the panel running. */
    probe_step("enable GCC display AHB clock");
    /* Linux drivers/clk/qcom/gcc-sm8250.c: GCC_DISP_AHB_CLK at GCC + 0xb00c. */
    vm_offset_t gcc = ml_io_map(0x0010b000, 0x1000);
    if (gcc == 0) return 0;
    wr(gcc + 0xc, rd(gcc + 0xc) | 1u);
    delay(1000);
    probe_step("read display power and enable register clock");
    /* SM8250 DISPCC, not the older 0x1004/0x1050 layout in redwood_adreno's display_probe.
     * Linux drivers/clk/qcom/dispcc-sm8250.c: MDSS GDSC 0x3000, AHB 0x2080, non-GDSC AHB 0x4004.
     * Keep firmware power/timing as-is; request the register clocks only when MDSS is already on.
     */
    vm_offset_t cc = ml_io_map(0x0af00000, 0x5000);
    if (cc == 0 || !(rd(cc + 0x3000) & 0x80000000u)) return 0;
    wr(cc + 0x4004, rd(cc + 0x4004) | 1u);
    wr(cc + 0x2080, rd(cc + 0x2080) | 1u);
    delay(1000);
    if (rd(cc + 0x2080) & 0x80000000u) return 0;
    probe_step("find the display control block");
    find_ctl();
    probe_step("find the pipe scanning the boot framebuffer");
	mdp = ml_io_map_unmappable(MDP_PHYS, MDP_SIZE, VM_WIMG_IO);
	for (int i = 0; mdp != 0 && i < SSPP_COUNT; i++) {
		if (rd(mdp + sspp_table[i].offset + SSPP_SRC0_ADDR) == (uint32_t)fb_phys) {
			sspp_found = i;
			break;
		}
	}
	if (ctl_driving < 0 || sspp_found < 0) {
		printf("ALIOTH DISPLAY: no %s: no flips\n", ctl_driving < 0 ? "CTL driving INTF_1" : "source pipe on the framebuffer");
		return 0;
	}
	if ((rd(mdp + sspp_table[sspp_found].offset + SSPP_SRC_YSTRIDE0) & 0xffff) != stride) {
		printf("ALIOTH DISPLAY: %s's stride is %u, not %u: no flips\n", sspp_table[sspp_found].name,
		    rd(mdp + sspp_table[sspp_found].offset + SSPP_SRC_YSTRIDE0) & 0xffff, stride);
		return 0;
	}
    probe_step("check the display SMMU stream");
	if (smmu_blocks()) {
		printf("ALIOTH DISPLAY: the display's SMMU stream translates: no flips\n");
		return 0;
	}
    probe_step("map the reserved back buffers");
	flip_bit = 1u << sspp_table[sspp_found].flush_bit;
	*frame_stride = gpu_stride;
    for (int i = 0; i < 3; i++) {
		pa[i] = fb_phys + (uint64_t)i * bytes;
		va[i] = ml_io_map_unmappable((vm_offset_t)pa[i], (vm_size_t)bytes, VM_WIMG_WCOMB);
		if (va[i] == 0) {
			printf("ALIOTH DISPLAY: frame %d at %#llx cannot be mapped: no flips\n", i, pa[i]);
			return 0;
		}
		/* (not filled: the framebuffer flips to a frame only after a whole picture was drawn into it) */
	}
    vm_offset_t intf = ml_io_map(INTF_PHYS, INTF_SIZE);
    if (intf != 0) {
        uint32_t frames = rd(intf + INTF_FRAME_COUNT);
        delay(100000);
        printf("ALIOTH DISPLAY: panel refreshed %u times in 100 ms (hardware counter)\n",
            rd(intf + INTF_FRAME_COUNT) - frames);
    }
	ready = true;
	printf("ALIOTH DISPLAY: %s on %#llx flipped by ctl_%d (flush bit %u); frames at %#llx, %#llx, %#llx (%llu bytes each)\n",
	    sspp_table[sspp_found].name, fb_phys, ctl_driving, sspp_table[sspp_found].flush_bit, pa[0], pa[1], pa[2], bytes);
	return 3;
}

/* the pipe to `phys` at the next frame start (hakios's apollo_display_flip, stride unchanged); false: nothing written */
bool
alioth_display_flip(uint64_t phys, uint32_t stride)
{
	if (!ready || phys >= (1ull << 32) || (phys & 0xff) != 0 || stride == 0 || stride > 0xffff) {
		return false;
	}
    vm_offset_t pipe = mdp + sspp_table[sspp_found].offset;
    wr(pipe + SSPP_SRC_YSTRIDE0, (rd(pipe + SSPP_SRC_YSTRIDE0) & 0xffff0000u) | stride);
	wr(pipe + SSPP_SRC0_ADDR, (uint32_t)phys);
	__asm__ volatile ("dsb sy" : : : "memory");
	wr(ctl_base[ctl_driving] + CTL_FLUSH, flip_bit);        /* last */
	return true;
}

/* the hardware has cleared the pipe's flush bit: the last flip is what the panel scans now */
bool
alioth_display_flip_latched(void)
{
	return ready && (rd(ctl_base[ctl_driving] + CTL_FLUSH) & flip_bit) == 0;
}

/* Return the pipe to the boot buffer before UEFI reuses its framebuffer. Other CPUs are stopped. */
void
alioth_display_quiesce(void)
{
    if (ready) {
        (void)alioth_display_flip(DISPLAY_RESERVED_PHYS, 1080 * 4);
        for (unsigned n = 0; n < 500 && !alioth_display_flip_latched(); n++) {
            delay(100);
        }
        ready = false;
    }
}
#endif /* PLATFORM_ALIOTH */
