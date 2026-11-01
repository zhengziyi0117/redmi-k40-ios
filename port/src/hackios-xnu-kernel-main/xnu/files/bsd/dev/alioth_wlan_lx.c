/* Alioth adaptation: Apollo PCIe/MHI implementation, APSL 2.0.
 * GPIOs, PHY sequence, six supply resources/voltages, MSI and SID
 * checked against this K40 stock-device-tree.dtb. Opt-in aliothwifi=N. */
/*
 * The kernel's side of the Wi-Fi driver's object (hakios.lx.h): what Linux's ath11k, built on
 * FreeBSD's LinuxKPI and wifi/lx by tools/mk-ath11k.sh and linked into the kernel (xnu 0128),
 * asks of it -- memory, time, threads, one lock to sleep under, the kernel's options -- and what it
 * asks of the radio, which alioth_wlan_wifi.c knows: its files, its view of memory, its boot.
 *
 * From HackiOS's bsd/dev/hakios.lx.c (github.com/oosexgang/hackios-xnu-kernel) as the Xiaomi Mi 10
 * (umi) port has it (github.com/JopangJovan/hackios-xnu-kernel-umi; APSL 2.0). The radio's
 * interrupts are not used (as on the Mi 10: wifi_irq=0, the rings are polled), so the vectors are
 * never attached; the MSI address and data are still given, the firmware wants them.
 *
 * A kernel built without the object has the weak lx_start() at the end of this file, which says so.
 */
#if defined(PLATFORM_ALIOTH)
#include <sys/cdefs.h>
extern int alioth_wlan_log(const char *, ...) __printflike(1, 2);
#define printf alioth_wlan_log
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/random.h>
#include <sys/systm.h>
#include <sys/redwood_lx.h>
#include <sys/redwood_net.h>
#include <sys/redwood_say.h>

extern kern_return_t thread_terminate(thread_t thread);
extern void thread_set_thread_name(thread_t thread, const char *name);
extern void read_random(void *buffer, u_int numBytes);
/* alioth_wlan_wifi.c */
extern unsigned alioth_wlan_wifi_option(const char *name);
extern const uint8_t *alioth_wlan_wifi_file(const char *name, unsigned int *size);
extern void *alioth_wlan_wifi_radio_memory(size_t size, unsigned long long *address);
extern void *alioth_wlan_wifi_firmware_memory(unsigned type, size_t size, unsigned long long *address);
extern void *alioth_wlan_wifi_registers(unsigned long long physical, size_t size);
extern bool alioth_wlan_wifi_msi(unsigned long long *address, unsigned *data);
extern int alioth_wlan_wifi_radio_boot(void);
extern void alioth_wlan_wifi_radio_reset(void);
extern bool alioth_wlan_wifi_radio_address(uint8_t *address);
extern bool alioth_wlan_wifi_network(uint8_t *name, unsigned *name_length, uint8_t *passphrase, unsigned *passphrase_length);
void redwood_lx_net_early(void);
int redwood_lx_start(void);
void redwood_lx_stop(void);
int redwood_lx_prepare(void);

static LCK_GRP_DECLARE(lx_lock_group, "sweet-lx");
static LCK_MTX_DECLARE(lx_lock, &lx_lock_group);

/* ---- memory ------------------------------------------------------------------------------------------ */

void *
lx_os_alloc(unsigned long size)
{
	return kalloc_data(size, Z_WAITOK | Z_ZERO);
}

void
lx_os_free(void *memory, unsigned long size)
{
	kfree_data(memory, size);
}

void *
lx_os_radio_memory(unsigned long size, unsigned long long *address)
{
	return alioth_wlan_wifi_radio_memory(size, address);
}

/* (the Mi 10 layer's own fixed-address memory is asked for by type, below: nothing asks for this) */
void *
lx_os_radio_memory_at(unsigned long size, unsigned long long address)
{
	(void)size;
	(void)address;
	return NULL;
}

int
lx_os_radio_firmware_memory(unsigned type, unsigned long size, void **memory, unsigned long long *address)
{
	*memory = alioth_wlan_wifi_firmware_memory(type, size, address);
	return *memory != NULL ? 0 : ENOMEM;
}

void *
lx_os_map_registers(unsigned long long physical, unsigned long size)
{
	return alioth_wlan_wifi_registers(physical, size);
}

/* ---- saying things ----------------------------------------------------------------------------------- */

void
lx_os_print(const char *text)
{
	alioth_wlan_wifi_say("%s", text);
}

void
lx_os_tell(const char *text)
{
	alioth_wlan_wifi_tell("%s", text);
}

void
lx_os_panic(const char *text)
{
	panic("%s", text);
}

/* ---- time, threads, sleeping --------------------------------------------------------------------------- */

unsigned long long
lx_os_nanoseconds(void)
{
	uint64_t nanoseconds;

	absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
	return nanoseconds;
}

void *
lx_os_thread_self(void)
{
	return current_thread();
}

struct lx_thread {
	void (*function)(void *);
	void *argument;
};

static void
lx_thread_runs(void *parameter, __unused wait_result_t result)
{
	struct lx_thread start = *(struct lx_thread *)parameter;

	kfree_type(struct lx_thread, parameter);
	start.function(start.argument);
	thread_terminate(current_thread());
}

int
lx_os_thread(const char *name, void (*function)(void *), void *argument)
{
	struct lx_thread *start = kalloc_type(struct lx_thread, Z_WAITOK | Z_ZERO);
	thread_t thread;

	if (start == NULL) {
		return ENOMEM;
	}
	start->function = function;
	start->argument = argument;
	if (kernel_thread_start(lx_thread_runs, start, &thread) != KERN_SUCCESS) {
		kfree_type(struct lx_thread, start);
		return EAGAIN;
	}
	thread_set_thread_name(thread, name);
	thread_deallocate(thread);
	return 0;
}

void
lx_os_lock(void)
{
	lck_mtx_lock(&lx_lock);
}

void
lx_os_unlock(void)
{
	lck_mtx_unlock(&lx_lock);
}

int
lx_os_sleep(const void *address, unsigned long long nanoseconds)
{
	uint64_t deadline;

	if (nanoseconds == 0) {
		return lck_mtx_sleep(&lx_lock, LCK_SLEEP_DEFAULT, (event_t)(uintptr_t)address, THREAD_UNINT) == THREAD_TIMED_OUT;
	}
	nanoseconds_to_absolutetime(nanoseconds, &deadline);
	deadline += mach_absolute_time();
	return lck_mtx_sleep_deadline(&lx_lock, LCK_SLEEP_DEFAULT, (event_t)(uintptr_t)address, THREAD_UNINT, deadline) == THREAD_TIMED_OUT;
}

void
lx_os_wakeup(const void *address)
{
	thread_wakeup((event_t)(uintptr_t)address);
}

/* ---- the rest of the machine ------------------------------------------------------------------------- */

void
lx_os_random(void *buffer, unsigned long size)
{
	read_random(buffer, (u_int)size);
}

unsigned
lx_os_option(const char *name)
{
	return alioth_wlan_wifi_option(name);
}

/* ath11k asks by its Linux firmware paths ("ath11k/QCA6390/hw2.0/board-2.bin"): the last part names the file. */
int
lx_os_firmware(const char *name, const void **data, unsigned long *size)
{
	static const char *const files[][2] = {
		{ "amss.bin", "amss" }, { "m3.bin", "m3" }, { "board.bin", "board" }, { "regdb.bin", "regdb" },
	};
	const char *last = strrchr(name, '/');
	unsigned int length = 0;

	last = last == NULL ? name : last + 1;
	for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		if (strcmp(last, files[i][0]) == 0) {
			const uint8_t *file = alioth_wlan_wifi_file(files[i][1], &length);
			if (file == NULL) {
				break;
			}
			*data = file;
			*size = length;
			alioth_wlan_wifi_say("lx: the driver's %s: /sweet-wifi \"%s\", %u bytes\n", name, files[i][1], length);
			return 0;
		}
	}
	alioth_wlan_wifi_say("lx: the driver asks for %s: not there\n", name);
	return ENOENT;
}

int
lx_os_rproc_boot(void)
{
	return alioth_wlan_wifi_radio_boot();
}

void
lx_os_rproc_shutdown(void)
{
	alioth_wlan_wifi_radio_reset();
}

int
lx_os_radio_address(unsigned char *address)
{
	return alioth_wlan_wifi_radio_address(address);
}

int
lx_os_network(unsigned char *name, unsigned *name_length, unsigned char *passphrase, unsigned *passphrase_length)
{
	return alioth_wlan_wifi_network(name, name_length, passphrase, passphrase_length);
}

/* ---- frames: the radio as the link behind en0 (hakios.net.h) ------------------------------------------ */

/*
 * A frame from the network stack comes on whatever thread sends it, and the driver's side keeps
 * a record of every thread that enters it: so frames are put in a queue here, and one thread of
 * ours takes them to the driver. A queue that is full drops what comes (TCP sends it again).
 */
#define LX_QUEUE        256

struct lx_frame {
	uint16_t length;
	uint8_t bytes[REDWOOD_NET_FRAME_MAX];
};

static bool lx_link_attached;
static LCK_MTX_DECLARE(lx_queue_lock, &lx_lock_group);
static struct lx_frame *lx_queue;
static uint32_t lx_queue_in, lx_queue_out;      /* frames put in and taken out, counted from the start */
static uint64_t lx_queue_dropped;

static void __attribute__((noreturn))
lx_queue_runs(__unused void *parameter, __unused wait_result_t result)
{
	for (;;) {
		struct lx_frame *frame;

		lck_mtx_lock(&lx_queue_lock);
		while (lx_queue_in == lx_queue_out) {
			lck_mtx_sleep(&lx_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&lx_queue_in, THREAD_UNINT);
		}
		frame = &lx_queue[lx_queue_out % LX_QUEUE];     /* (stays ours until lx_queue_out moves) */
		lck_mtx_unlock(&lx_queue_lock);
		lx_transmit(frame->bytes, frame->length);
		lck_mtx_lock(&lx_queue_lock);
		lx_queue_out++;
		lck_mtx_unlock(&lx_queue_lock);
	}
}

static bool
lx_link_transmit(mbuf_t packet, size_t length)
{
	struct lx_frame *frame;

	lck_mtx_lock(&lx_queue_lock);
	if (length > REDWOOD_NET_FRAME_MAX || lx_queue_in - lx_queue_out == LX_QUEUE) {
		lx_queue_dropped++;
		lck_mtx_unlock(&lx_queue_lock);
		return false;
	}
	frame = &lx_queue[lx_queue_in % LX_QUEUE];
	if (mbuf_copydata(packet, 0, length, frame->bytes) != 0) {
		lck_mtx_unlock(&lx_queue_lock);
		return false;
	}
	frame->length = (uint16_t)length;
	lx_queue_in++;
	lck_mtx_unlock(&lx_queue_lock);
	thread_wakeup((event_t)&lx_queue_in);
	return true;
}

static struct redwood_net_link lx_link = {
	.name = "the phone's radio (ath11k)",
	.wireless = true,
	.transmit = lx_link_transmit,
};

void
lx_os_net_attach(const unsigned char *address)
{
	thread_t thread;

	if (lx_link_attached) {
		return;
	}
	lx_queue = kalloc_data(LX_QUEUE * sizeof(*lx_queue), Z_WAITOK | Z_ZERO);
	if (lx_queue == NULL || kernel_thread_start(lx_queue_runs, NULL, &thread) != KERN_SUCCESS) {
		alioth_wlan_wifi_tell("lx: no memory or no thread for the frames to send: the radio's frames go nowhere\n");
		return;
	}
	thread_set_thread_name(thread, "wifi-send");
	thread_deallocate(thread);
	memcpy(lx_link.address, address, sizeof(lx_link.address));
	lx_link_attached = redwood_net_attach(&lx_link);
	alioth_wlan_wifi_tell("lx: %s\n", lx_link_attached ? "the radio is the link behind en0" : "EN0 HAS A LINK ALREADY: the radio's "
	    "frames go nowhere");
}

/*
 * For a session that stays up, the interface is made when the kernel starts, before the radio is up:
 * iOS's Wi-Fi side looks for it and for its registry entry as soon as it runs, once, and a radio
 * that turns up seconds later is one it has already failed to open.
 */
void
redwood_lx_net_early(void)
{
	uint8_t address[6];

	if (alioth_wlan_wifi_radio_address(address)) {
		lx_os_net_attach(address);
	}
}

void
lx_os_net_link(int up)
{
	if (lx_link_attached) {
		redwood_net_link_state(up != 0);
	}
}

void
lx_os_net_input(const unsigned char *frame, unsigned long length)
{
	if (lx_link_attached && length <= REDWOOD_NET_FRAME_MAX) {
		redwood_net_input(frame, length);
	}
}

/* ---- the radio's interrupts: not taken (the bus layer polls; wifi_irq is 0) ------------------------------- */

int
lx_os_irq_msi(unsigned long long *address, unsigned *data)
{
	return alioth_wlan_wifi_msi(address, data) ? 1 : 0;
}

int
lx_os_irq_attach(unsigned vector, void (*function)(void *), void *argument)
{
	(void)vector;
	(void)function;
	(void)argument;
	return ENXIO;
}

void
lx_os_irq_enable(unsigned vector)
{
	(void)vector;
}

void
lx_os_irq_disable(unsigned vector)
{
	(void)vector;
}

void
lx_os_irq_detach_all(void)
{
}

unsigned long long
lx_os_irq_count(unsigned vector)
{
	(void)vector;
	return 0;
}

/* ---- the station (hakios.sta.h) ------------------------------------------------------------------------- */

void
lx_os_station(const struct redwood_sta_ops *ops)
{
	redwood_sta_attach(ops);
}

void
lx_os_station_lost(int error)
{
	redwood_sta_lost(error);
}

/* ---- the driver ---------------------------------------------------------------------------------------- */

/* The object has its own; this is the one of a kernel built without it. */
__attribute__((weak)) int
lx_start(void)
{
	alioth_wlan_wifi_tell("lx: THE DRIVER IS NOT BUILT IN: its object was not there when this kernel was built "
	    "(tools/mk-ath11k.sh in CI)\n");
	return ENXIO;
}

__attribute__((weak)) void
lx_stop(void)
{
}

__attribute__((weak)) int
lx_prepare(void)
{
	return 0;
}

__attribute__((weak)) int
lx_transmit(__unused const unsigned char *frame, __unused unsigned long length)
{
	return ENXIO;
}

int
redwood_lx_start(void)
{
	return lx_start();
}

void
redwood_lx_stop(void)
{
	lx_stop();
}

int
redwood_lx_prepare(void)
{
	return lx_prepare();
}
#endif /* PLATFORM_ALIOTH */
