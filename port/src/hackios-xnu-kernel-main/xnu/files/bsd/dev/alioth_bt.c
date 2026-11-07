/*
 * Bluetooth of apollo (xnu 0136): the Bluetooth side of the QCA6390 -- the Wi-Fi's chip -- on the
 * UART of QUP serial engine 6. Nothing runs unless boot-arg aliothbt=N asks for steps 1..N; they
 * run in a thread of their own, aliothbtdelay seconds (default 0) after it is started (from
 * sweet_ufs_init(), early in bsd_init: the firmware is in the ADT, nothing waits for the root).
 *
 * Ported from the Xiaomi Mi 10 (umi: the same SM8250 and QCA6390) of
 * github.com/JopangJovan/hackios-xnu-kernel-umi, devices/xiaomi-mi-10 (umi_bluetooth.c,
 * hakios.bluetooth.h, hakios.bluetooth.cpp and umi_rpmh.c; MIT), on which iOS's own bluetoothd
 * found devices and paired through it (its commit b7122b2). Apollo's live DT wires the chip the
 * same way: BT_EN GPIO 21 (qca,bt-reset-gpio), SW_CTRL GPIO 124 (qca,bt-sw-ctrl-gpio),
 * qupv3_se6_4uart at 0x998000 on GPIO 16-19 (CTS, RTS, TX, RX), and the rails of
 * sweet_rpmh_bt_power(). The firmware is the phone's own, from its bluetooth partition
 * (tools/pull-bt-fw.sh), in the ADT: /sweet-bt "patch" (htbtfw20.tlv) and "nvm" (htnv20.bin),
 * mkadt.py --bt-fw research/apollo-bt.
 *
 *   1  read   SE6's GCC clocks and the pins
 *   2  write  SE6's clocks voted for a moment: the UART's GENI registers; put back
 *   3  write  ... and the UART looped back on itself inside the SoC (nothing reaches the chip)
 *   4  write  rails, BT_EN, the UART at 115200 baud (H4): HCI reset and the version from the
 *             chip's ROM; then BT_EN low again and the UART put back
 *   5  write  ... the UART moved to aliothbtbaud (below), the firmware (patch, then NVM), reset, the
 *             version it runs, the address
 *             (/chosen mac-address-bluetooth0), and what the radio hears: 3 s of LE scan and
 *             4 s of classic inquiry; then BT_EN low again
 *   6  write  as 5 without the scans, and the radio stays up for iOS: an H4 endpoint (the kernel
 *             control "com.alioth.bluetooth", for bluetoothd) and the registry entry "bluetooth"
 *             that bluetoothd looks for (iokit/Kernel/arm/AliothBluetooth.cpp)
 *
 * The chip's ROM talks at 115200 baud. From step 5 on (xnu 0145) the UART is moved to boot-arg
 * aliothbtbaud (default 921600; 115200 keeps it there, as umi does) before the firmware goes in, as
 * Linux's hci_qca does, and the NVM is made to say the same speed: 115200 carries ~11 KB/s, too
 * little for A2DP (AAC at 256 kbit/s stuttered, apollo boot 77). Should the new speed not work the
 * chip is started over at 115200. 921600 is the default because 92 bytes a millisecond still fit
 * the 128-byte receive FIFO between the worker's 1 ms polls, whether or not the chip heeds RFR.
 * No interrupt is taken: the FIFOs are polled. Differences from umi: SW_CTRL is an input that is
 * read, as Android's btpower.c has it (umi drives it high; aliothbtswctrl=1 does as umi does); the firmware
 * comes from the ADT, not from the kernel's own data; the ADT's address is given to the chip
 * (0xfc14, as Linux's qca_set_bdaddr), else it keeps the NVM's; a thread, not bsd_init itself.
 */
#if defined(PLATFORM_ALIOTH)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
#include <sys/termios.h>
#include <sys/ttycom.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <libkern/libkern.h>
#include "alioth_bt.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void thread_set_thread_name(thread_t thread, const char *name);
extern kern_return_t thread_terminate(thread_t);
extern int alioth_wlan_rpmh_bt_power(void);
extern int AliothBluetoothPublish(const uint8_t address[6], const uint8_t commands[64]);  /* AliothBluetooth.cpp */
extern void AliothBluetoothSetReady(bool ready);
void alioth_bt_init(void);
extern void IOSleep(unsigned int milliseconds);
extern void redwood_tl(const char *, ...) __printflike(1, 2);
#define printf redwood_tl
static volatile bool bt_stopping, bt_boot_alive, bt_worker_alive;
static vm_offset_t bt_saved_tlmm, bt_saved_uart;
static bool bt_powered;

#define GCC_PA          0x00100000u
#define TLMM_NORTH_PA   0x0f900000u     /* GPIO 16-21 and 124 are on the north tile */
#define SE6_PA          0x00998000u

/* GCC: the QUP wrapper 0 votes (bits 6 and 7: its AHB clocks, 16: SE6), SE6's branches and root */
#define GCC_QUP_VOTE    0x52008u
#define GCC_QUP_M_AHB   0x17004u
#define GCC_QUP_S_AHB   0x17008u        /* bit 1: hardware clock gating */
#define GCC_SE6_CBCR    0x1772cu
#define GCC_SE6_CMD     0x17730u
#define GCC_SE6_CFG     0x17734u
#define GCC_SE6_M       0x17738u
#define GCC_SE6_N       0x1773cu
#define GCC_SE6_D       0x17740u
#define GCC_QUP_BITS    ((1u << 6) | (1u << 7) | (1u << 16))
#define CBCR_OFF        0x80000000u

#define PIN_BT_EN       21u
#define PIN_SW_CTRL     124u
#define TLMM_CFG(p)     ((p) * 0x1000u)
#define TLMM_IO(p)      ((p) * 0x1000u + 4u)

/* GENI serial engine registers (the UART's) */
#define GENI_STATUS     0x40u           /* bits 0 and 12: the main and secondary sequencers are busy */
#define GENI_TX_LEN     0x270u
#define GENI_M_CMD      0x600u
#define GENI_M_CANCEL   0x604u
#define GENI_M_IRQ      0x610u
#define GENI_M_IRQ_CLR  0x618u
#define GENI_S_CMD      0x630u
#define GENI_S_CANCEL   0x634u
#define GENI_S_IRQ      0x640u
#define GENI_S_IRQ_CLR  0x648u
#define GENI_TX_FIFO    0x700u
#define GENI_RX_FIFO    0x780u
#define GENI_TX_STATUS  0x800u
#define GENI_RX_STATUS  0x804u
#define GENI_TX_DEPTH   0xe24u
#define GENI_RX_DEPTH   0xe28u

#define BT_EVENT_MAX    258u
#define BT_RX_RAW_SIZE  16384u          /* a power of two */
#define BT_TX_QUEUE     65536u          /* a power of two */
#define BT_NVM_MOST     16384u
#define BT_PATCH_MOST   (512u * 1024u)

static uint32_t bt_steps, bt_delay, bt_swctrl_drive;
static uint32_t bt_baud = 921600;
static const struct alioth_bt_speed *bt_want;                    /* aliothbtbaud */
static const struct alioth_bt_speed *bt_speed = &alioth_bt_speeds[0];     /* what the UART runs at */
static uint32_t bt_uart_div = 4;        /* GENI's clock divider for bt_speed */

/* the UART's registers that steps 3-6 set, put back as they were found afterwards */
static const uint32_t bt_uart_regs[] = {
	0x48, 0x4c, 0x22c, 0x258, 0x25c, 0x260, 0x264, 0x268, 0x26c, 0x270, 0x280, 0x284, 0x288, 0x28c,
	0x294, 0x2a4, 0x2a8, 0x80c, 0x810, 0x814
};
#define BT_UART_REGS    (sizeof(bt_uart_regs) / sizeof(bt_uart_regs[0]))

static uint32_t
bt_read(vm_offset_t base, uint32_t offset)
{
	uint32_t value = *(volatile uint32_t *)(base + offset);

	__asm__ volatile ("dmb oshld" ::: "memory");
	return value;
}

static void
bt_write(vm_offset_t base, uint32_t offset, uint32_t value)
{
	__asm__ volatile ("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + offset) = value;
	__asm__ volatile ("dsb osh" ::: "memory");
}

static uint16_t
bt_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t
bt_le32(const uint8_t *p)
{
	return bt_le16(p) | (uint32_t)bt_le16(p + 2) << 16;
}

static const uint8_t *
bt_file(const char *name, unsigned int *size)
{
    extern const uint8_t *redwood_image_file(const char *, const char *, uint64_t *, int *);
    uint64_t bytes = 0;
    const char *file = strcmp(name, "patch") == 0 ? "htbtfw20.tlv" : "htnv20.bin";
    const uint8_t *data = redwood_image_file("fw-bt", file, &bytes, NULL);
    if (bytes > 0xffffffffu) return NULL;
    *size = (unsigned int)bytes;
    return data;
}

/*
 * The UART set up as umi's (and Android's) driver has it: 8 bits, no parity, 1 stop bit, words of bytes;
 * the clock divided by bt_uart_div.
 */
static void
bt_uart_setup(vm_offset_t uart)
{
	bt_write(uart, 0x48, (bt_uart_div << 4) | 1);
	bt_write(uart, 0x4c, (bt_uart_div << 4) | 1);
	bt_write(uart, 0x258, 0);
	bt_write(uart, 0x22c, 0);
	bt_write(uart, 0x25c, 2);
	bt_write(uart, 0x280, 0);
	bt_write(uart, 0x260, 0xf);
	bt_write(uart, 0x264, 0);
	bt_write(uart, 0x284, 0x4380e);
	bt_write(uart, 0x288, 0xc3e0e);
	bt_write(uart, 0x268, 8);
	bt_write(uart, 0x28c, 8);
	bt_write(uart, 0x26c, 0);
	bt_write(uart, 0x294, 160);
	bt_write(uart, 0x2a4, 0);
	bt_write(uart, 0x2a8, 0);
	bt_write(uart, 0x80c, 1);
	bt_write(uart, 0x810, 0);
	bt_write(uart, 0x814, 6);
	bt_write(uart, GENI_M_IRQ_CLR, 0xffffffff);
	bt_write(uart, GENI_S_IRQ_CLR, 0xffffffff);
}

/* Both sequencers stopped (cancelled, then aborted) and the receive FIFO emptied. */
static int
bt_uart_quiesce(vm_offset_t uart)
{
	unsigned depth, words;

	for (unsigned attempt = 0; attempt < 2; attempt++) {
		uint32_t active = bt_read(uart, GENI_STATUS);

		if (!(active & 0x1001)) {
			break;
		}
		if (active & 1) {
			bt_write(uart, GENI_M_CANCEL, attempt ? 2 : 4);
		}
		if (active & 0x1000) {
			bt_write(uart, GENI_S_CANCEL, attempt ? 2 : 4);
		}
		for (unsigned t = 0; t < 10000 && (bt_read(uart, GENI_STATUS) & 0x1001); t++) {
			delay(1);
		}
	}
	if (bt_read(uart, GENI_STATUS) & 0x1001) {
		return ETIMEDOUT;
	}
	depth = (bt_read(uart, GENI_RX_DEPTH) >> 16) & 0x3f;
	words = bt_read(uart, GENI_RX_STATUS) & 0x1ffffff;
	if (!depth || words > depth) {
		return EIO;
	}
	while (words--) {
		(void)bt_read(uart, GENI_RX_FIFO);
	}
	bt_write(uart, GENI_M_IRQ_CLR, 0xffffffff);
	bt_write(uart, GENI_S_IRQ_CLR, 0xffffffff);
	return 0;
}

/* Step 3: eight bytes through the UART's internal loopback, nothing on the pins. */
static void
bt_loopback(vm_offset_t uart)
{
	static const uint8_t pattern[] = { 0x01, 0x03, 0x0c, 0x00, 0x55, 0xaa, 0x7e, 0xff };
	uint32_t saved[BT_UART_REGS];
	uint8_t received[sizeof(pattern)];
	unsigned count = 0, elapsed;

	if ((bt_read(uart, GENI_STATUS) & 0x1001) || bt_read(uart, GENI_TX_STATUS) || bt_read(uart, GENI_RX_STATUS)) {
		printf("ALIOTH BT: step 3: loopback skipped: the UART is busy\n");
		return;
	}
	for (unsigned i = 0; i < BT_UART_REGS; i++) {
		saved[i] = bt_read(uart, bt_uart_regs[i]);
	}
	bt_uart_setup(uart);
	bt_write(uart, 0x22c, 1);       /* loopback */
	bt_write(uart, GENI_S_CMD, (1u << 27) | 0x80);
	bt_write(uart, GENI_TX_LEN, sizeof(pattern));
	bt_write(uart, GENI_M_CMD, 1u << 27);
	for (unsigned i = 0; i < sizeof(pattern); i++) {
		bt_write(uart, GENI_TX_FIFO, pattern[i]);
	}
	for (elapsed = 0; elapsed < 20000 && count < sizeof(pattern); elapsed++) {
		uint32_t status = bt_read(uart, GENI_RX_STATUS), words = status & 0x1ffffff;

		while (words--) {
			unsigned bytes = 4;
			uint32_t value;

			if (!words && (status & (1u << 31))) {
				bytes = (status >> 28) & 7;
			}
			value = bt_read(uart, GENI_RX_FIFO);
			for (unsigned b = 0; b < bytes; b++) {
				if (count < sizeof(received)) {
					received[count++] = (uint8_t)(value >> (8 * b));
				}
			}
		}
		delay(1);
	}
	printf("ALIOTH BT: step 3: loopback %s: %u of %u bytes in %u us\n",
	    count == sizeof(pattern) && !memcmp(pattern, received, sizeof(pattern)) ? "WORKS" : "FAILED",
	    count, (unsigned)sizeof(pattern), elapsed);
	(void)bt_uart_quiesce(uart);
	for (unsigned i = 0; i < BT_UART_REGS; i++) {
		bt_write(uart, bt_uart_regs[i], saved[i]);
	}
	bt_write(uart, GENI_M_IRQ_CLR, 0xffffffff);
	bt_write(uart, GENI_S_IRQ_CLR, 0xffffffff);
}

/* ---- H4 over the UART ----------------------------------------------------------------------------- */

static struct alioth_bt_h4 bt_rx_packet;
static uint8_t bt_rx_raw[BT_RX_RAW_SIZE];
static unsigned bt_rx_head, bt_rx_tail;
static bool bt_ibs_ack_pending, bt_chip_awake;
static unsigned bt_ibs_wakes, bt_ibs_sleeps;
static uint8_t bt_nvm_work[BT_NVM_MOST];
static uint8_t bt_address[6];           /* display order */
static uint8_t bt_commands[64];         /* Read Local Supported Commands */

/* What the receive FIFO holds, into bt_rx_raw. */
static int
bt_rx_capture(vm_offset_t uart)
{
	uint32_t irq = bt_read(uart, GENI_S_IRQ);

	if (irq) {
		bt_write(uart, GENI_S_IRQ_CLR, irq);
	}
	for (unsigned budget = 0; budget < 64; budget++) {
		uint32_t status = bt_read(uart, GENI_RX_STATUS), words = status & 0x1ffffff, word;
		unsigned last, bytes;

		if (!words) {
			return 0;
		}
		if (words > ((bt_read(uart, GENI_RX_DEPTH) >> 16) & 0x3f)) {
			return EIO;
		}
		last = (status >> 28) & 7;
		bytes = words == 1 && (status >> 31) && last ? last : 4;
		if (bytes > 4) {
			return EIO;
		}
		if (BT_RX_RAW_SIZE - (bt_rx_head - bt_rx_tail) < bytes) {
			return ENOBUFS;
		}
		word = bt_read(uart, GENI_RX_FIFO);
		for (unsigned b = 0; b < bytes; b++) {
			bt_rx_raw[bt_rx_head++ & (BT_RX_RAW_SIZE - 1)] = (uint8_t)(word >> (8 * b));
		}
	}
	return 0;
}

/* The next whole packet from the chip (only events if `events_only`): 1, 0 if none yet, or -errno. */
static int
bt_rx_frame(vm_offset_t uart, uint8_t *frame, unsigned capacity, unsigned *length, bool events_only)
{
	int error = bt_rx_capture(uart);

	if (error) {
		return -error;
	}
	while (bt_rx_head != bt_rx_tail) {
		uint8_t byte = bt_rx_raw[bt_rx_tail++ & (BT_RX_RAW_SIZE - 1)];
		int result = alioth_bt_h4_push(&bt_rx_packet, byte);

		if (result < 0) {
			printf("ALIOTH BT: H4 out of step (%d) at byte %#x\n", result, byte);
			memset(&bt_rx_packet, 0, sizeof(bt_rx_packet));
			return -EILSEQ;
		}
		if (result == 2) {
			if (byte == 0xfd) {
				bt_ibs_ack_pending = true;
				bt_ibs_wakes++;
			} else if (byte == 0xfe) {
				bt_ibs_sleeps++;
			} else {
				bt_chip_awake = true;
			}
		}
		if (result != 1 || (events_only && bt_rx_packet.bytes[0] != 4)) {
			continue;
		}
		if (bt_rx_packet.used > capacity) {
			return -EMSGSIZE;
		}
		*length = bt_rx_packet.used;
		memcpy(frame, bt_rx_packet.bytes, *length);
		return 1;
	}
	return 0;
}

static int
bt_uart_tx(vm_offset_t uart, const uint8_t *packet, unsigned length)
{
	unsigned sent = 0, depth = (bt_read(uart, GENI_TX_DEPTH) >> 16) & 0x3f;

	if (!depth || depth > 64 || !length || length > ALIOTH_BT_H4_MAX) {
		return EINVAL;
	}
	bt_write(uart, GENI_M_IRQ_CLR, 0xffffffff);
	bt_write(uart, GENI_TX_LEN, length);
	bt_write(uart, GENI_M_CMD, 1u << 27);
	/* ~87 us a byte at 115200 baud, the slowest: twice that, and 10 ms */
	for (unsigned elapsed = 0; elapsed < length * 180 + 10000; elapsed++) {
		int rx_error = bt_rx_capture(uart);
		unsigned used;
		uint32_t irq;

		if (rx_error) {
			return rx_error;
		}
		used = bt_read(uart, GENI_TX_STATUS) & 0x0fffffff;
		if (used > depth) {
			return EIO;
		}
		for (unsigned room = depth - used; room && sent < length; room--) {
			bt_write(uart, GENI_TX_FIFO, packet[sent++]);
		}
		irq = bt_read(uart, GENI_M_IRQ);
		if (sent == length && (irq & 1)) {
			bt_write(uart, GENI_M_IRQ_CLR, irq);
			return 0;
		}
		delay(1);
	}
	printf("ALIOTH BT: sending timed out: %u of %u bytes, irq %#x, fifo %#x\n", sent, length,
	    bt_read(uart, GENI_M_IRQ), bt_read(uart, GENI_TX_STATUS));
	bt_write(uart, GENI_M_CANCEL, 4);
	return ETIMEDOUT;
}

/* The chip woke up and said so (IBS 0xfd): it is acknowledged (0xfc). */
static int
bt_service_ibs(vm_offset_t uart)
{
	const uint8_t ack = 0xfc;
	int error;

	if (!bt_ibs_ack_pending) {
		return 0;
	}
	error = bt_uart_tx(uart, &ack, 1);
	if (!error) {
		bt_ibs_ack_pending = false;
	}
	return error;
}

/* An HCI command; with `wait`, its Command Complete or Command Status (1 s at most). 0, or an errno. */
static int
bt_hci_command(vm_offset_t uart, uint16_t opcode, const uint8_t *params, unsigned param_length, bool wait,
    uint8_t event[BT_EVENT_MAX], unsigned *event_length)
{
	uint8_t packet[259];
	int error;

	if (param_length > 255) {
		return EINVAL;
	}
	packet[0] = 1;
	packet[1] = (uint8_t)opcode;
	packet[2] = (uint8_t)(opcode >> 8);
	packet[3] = (uint8_t)param_length;
	if (param_length) {
		memcpy(packet + 4, params, param_length);
	}
	*event_length = 0;
	error = bt_uart_tx(uart, packet, param_length + 4);
	if (error || !wait) {
		return error;
	}
	for (unsigned elapsed = 0; elapsed < 100000; elapsed++) {
		int result = bt_rx_frame(uart, event, BT_EVENT_MAX, event_length, true);

		if (result < 0) {
			return -result;
		}
		error = bt_service_ibs(uart);
		if (error) {
			return error;
		}
		if (result == 1) {
			int status = alioth_bt_cc_status(event, *event_length, opcode);

			if (status >= 0) {
				if (status) {
					printf("ALIOTH BT: HCI %#x: status %#x\n", opcode, status);
				}
				return status ? EIO : 0;
			}
			if (opcode == 0xfc00 && alioth_bt_qca_tlv_ack(event, *event_length)) {
				return 0;
			}
			/* the address (Linux's qca_set_bdaddr waits for a vendor event) */
			if (opcode == 0xfc14 && *event_length >= 3 && event[1] == 0xff) {
				return 0;
			}
			/* inquiry and other slow commands are acknowledged with Command Status */
			if (*event_length == 7 && event[1] == 0x0f && event[5] == (uint8_t)opcode &&
			    event[6] == (uint8_t)(opcode >> 8)) {
				return event[3] ? EIO : 0;
			}
			printf("ALIOTH BT: event %#x while waiting for %#x\n", event[1], opcode);
		}
		*event_length = 0;
		delay(10);
	}
	printf("ALIOTH BT: HCI %#x timed out (rx %#x, irq %#x)\n", opcode, bt_read(uart, GENI_RX_STATUS),
	    bt_read(uart, GENI_S_IRQ));
	return ETIMEDOUT;
}

/* A firmware file (TLV: patch or NVM) in segments of 243 bytes (EDL patch request 0x1e). */
static int
bt_qca_download(vm_offset_t uart, const uint8_t *tlv, size_t size, bool skip_events)
{
	size_t offset = 0;
	unsigned segment = 0;

	if (!tlv || size < 4 || size > BT_PATCH_MOST ||
	    (tlv[1] | (uint32_t)tlv[2] << 8 | (uint32_t)tlv[3] << 16) != size - 4) {
		return EINVAL;
	}
	while (offset < size) {
        if (__atomic_load_n(&bt_stopping, __ATOMIC_ACQUIRE)) return ECANCELED;
		unsigned part = size - offset > 243 ? 243 : (unsigned)(size - offset), length = 0;
		uint8_t params[245], event[BT_EVENT_MAX];
		bool ack;
		int error;

		params[0] = 0x1e;
		params[1] = (uint8_t)part;
		memcpy(params + 2, tlv + offset, part);
		offset += part;
		segment++;
		ack = !skip_events || offset == size;
		error = bt_hci_command(uart, 0xfc00, params, part + 2, ack, event, &length);
		if (!error && ack && !alioth_bt_qca_tlv_ack(event, length)) {
			error = EIO;
		}
		if (error) {
			printf("ALIOTH BT: firmware record type %u: segment %u of %u refused, errno %d\n", tlv[0], segment,
			    (unsigned)((size + 242) / 243), error);
			return error;
		}
	}
	printf("ALIOTH BT: firmware record type %u taken: %u bytes, %u segments\n", tlv[0], (unsigned)size, segment);
	return 0;
}

/* The ADT's address for Bluetooth (mkadt.py: the Wi-Fi's address + 1), if it has one. */
static bool
bt_adt_address(uint8_t address[6])
{
	DTEntry node;
	void const *prop = NULL;
	unsigned int size = 0;

	if (SecureDTLookupEntry(NULL, "/chosen", &node) != kSuccess ||
	    SecureDTGetProperty(node, "mac-address-bluetooth0", &prop, &size) != kSuccess || size != 6) {
		return false;
	}
	memcpy(address, prop, 6);
	return (address[0] & 1) == 0 && (address[0] | address[1] | address[2] | address[3] | address[4] | address[5]);
}

/* Steps 5 and 6: the firmware into the chip, then what it now is: version, commands, address. */
static int
bt_qca_firmware(vm_offset_t uart)
{
	uint8_t event[BT_EVENT_MAX], wanted[6] = { 0 };
	unsigned length = 0, patch_size = 0, nvm_size = 0, mode, nonzero = 0;
	bool have_wanted;
	const uint8_t version_request = 0x19;
	const uint8_t logging[] = { 0x14, 0 };
	const uint8_t *patch = bt_file("patch", &patch_size), *nvm = bt_file("nvm", &nvm_size);
	uint32_t product, soc;
	uint16_t rom;
	int error;

	error = bt_hci_command(uart, 0xfc00, &version_request, 1, true, event, &length);
	if (error) {
		return error;
	}
	if (length != 21 || event[7] != 0x19) {
		printf("ALIOTH BT: the chip's version answer is %u bytes, not 21\n", length);
		return ENODEV;
	}
	product = bt_le32(event + 9);
	rom = bt_le16(event + 15);
	soc = bt_le32(event + 17);
	printf("ALIOTH BT: QCA product %#x, ROM %#x, SoC %#x, patch %#x\n", product, rom, soc, bt_le16(event + 13));
	if (product != 0x10 || rom != 0x200 || soc != 0x400a0200) {
		printf("ALIOTH BT: not the QCA6390 (product 0x10, ROM 0x200, SoC 0x400a0200) the firmware is for\n");
		return ENODEV;
	}
	if (patch == NULL || nvm == NULL || patch_size < 28 || nvm_size < 4) {
		printf("ALIOTH BT: no firmware in the ADT (/sweet-bt \"patch\" and \"nvm\": mkadt.py --bt-fw research/apollo-bt)\n");
		return ENOENT;
	}
	if (bt_le16(patch + 16) != product || bt_le16(patch + 18) != rom) {
		printf("ALIOTH BT: the patch is for product %#x ROM %#x\n", bt_le16(patch + 16), bt_le16(patch + 18));
		return EINVAL;
	}
	mode = patch[14];               /* 3: the chip answers only the last segment */
	if (mode != 0 && mode != 3) {
		return EINVAL;
	}
	if (nvm_size > sizeof(bt_nvm_work)) {
		return E2BIG;
	}
	memcpy(bt_nvm_work, nvm, nvm_size);
	if (alioth_bt_qca6390_nvm_prepare(bt_nvm_work, nvm_size, bt_speed->qca)) {
		printf("ALIOTH BT: the NVM file is not one this knows (no HCI transport record)\n");
		return EINVAL;
	}
	printf("ALIOTH BT: firmware: patch %u bytes, NVM %u bytes, at %u baud (about %u s)\n", patch_size, nvm_size,
	    bt_speed->baud, (patch_size + nvm_size) * 10 / bt_speed->baud + 1);
	error = bt_qca_download(uart, patch, patch_size, mode == 3);
	if (error) {
		return error;
	}
	delay(10000);
	error = bt_qca_download(uart, bt_nvm_work, nvm_size, false);
	if (error) {
		return error;
	}
	/* as Linux's QCA6390 setup: the chip's own diagnostic log off, then a reset */
	error = bt_hci_command(uart, 0xfc17, logging, sizeof(logging), true, event, &length);
	if (!error) {
		error = bt_hci_command(uart, 0x0c03, NULL, 0, true, event, &length);
	}
	if (!error) {
		error = bt_hci_command(uart, 0x1001, NULL, 0, true, event, &length);
	}
	if (error) {
		return error;
	}
	if (length != 15 || bt_le16(event + 11) != 29 || bt_le16(event + 13) == 0x0d2b) {
		printf("ALIOTH BT: after the firmware the chip does not say it runs it (manufacturer %u, subversion %#x)\n",
		    length == 15 ? bt_le16(event + 11) : 0, length == 15 ? bt_le16(event + 13) : 0);
		return EIO;
	}
	printf("ALIOTH BT: THE FIRMWARE RUNS: HCI %#x, LMP %#x, subversion %#x\n", event[7], event[10], bt_le16(event + 13));
	error = bt_hci_command(uart, 0x1002, NULL, 0, true, event, &length);
	if (error) {
		return error;
	}
	if (length != 71) {
		return EILSEQ;
	}
	memcpy(bt_commands, event + 7, sizeof(bt_commands));
	have_wanted = bt_adt_address(wanted);
	if (have_wanted) {
		/*
		 * (xnu 0137) In display order, the first byte first: QCA's chips after ROME take 0xfc14's
		 * address big-endian, unlike every other HCI address (Linux: "Bluetooth: qca: fix
		 * device-address endianness", 2024). Boot 61 sent it the HCI way, last byte first, and the
		 * address the chip then said it had was one no device can have (reversed, the ADT's last
		 * byte comes first: if odd, a group address; presumably that, not checked).
		 */
		error = bt_hci_command(uart, 0xfc14, wanted, sizeof(wanted), true, event, &length);
		printf("ALIOTH BT: the ADT's address given to the chip: errno %d%s\n", error,
		    error ? " (it keeps the NVM's)" : "");
	}
	error = bt_hci_command(uart, 0x1009, NULL, 0, true, event, &length);
	if (error) {
		return error;
	}
	if (length != 13) {
		return EILSEQ;
	}
	for (unsigned i = 0; i < 6; i++) {
		bt_address[i] = event[12 - i];
		nonzero |= bt_address[i];
	}
	if (!nonzero || (bt_address[0] & 1)) {
		printf("ALIOTH BT: the chip's address is not one a device can have (%s, first byte %#x)\n",
		    nonzero ? "a group address" : "zero", bt_address[0]);
		return EINVAL;
	}
	printf("ALIOTH BT: the chip's address: %s\n", !have_wanted ? "its own (the ADT has none for Bluetooth)" :
	    memcmp(bt_address, wanted, 6) ? "its own, NOT the ADT's" : "the ADT's");
	return 0;
}

/* Step 5: 3 s of LE passive scan and 4 s of classic inquiry; how many answers. */
static int
bt_radio_scan(vm_offset_t uart)
{
	static const uint8_t mask[] = { 0xff, 0xff, 0xfb, 0xff, 0x07, 0xf8, 0xbf, 0x3d };
	static const uint8_t le_mask[] = { 2, 0, 0, 0, 0, 0, 0, 0 };
	static const uint8_t params[] = { 0, 0x60, 0, 0x30, 0, 0, 0 };     /* passive: 30 ms of every 60 */
	static const uint8_t enable[] = { 1, 1 }, disable[] = { 0, 0 };
	static const uint8_t inquiry[] = { 0x33, 0x8b, 0x9e, 2, 0 };       /* GIAC, 2 x 1.28 s */
	uint8_t event[BT_EVENT_MAX];
	unsigned length = 0, reports = 0, classic = 0;
	uint64_t deadline;
	bool complete = false;
	int error, stop;

	error = bt_hci_command(uart, 0x0c01, mask, sizeof(mask), true, event, &length);
	if (!error) {
		error = bt_hci_command(uart, 0x2001, le_mask, sizeof(le_mask), true, event, &length);
	}
	if (!error) {
		error = bt_hci_command(uart, 0x200b, params, sizeof(params), true, event, &length);
	}
	if (!error) {
		error = bt_hci_command(uart, 0x200c, enable, sizeof(enable), true, event, &length);
	}
	if (error) {
		return error;
	}
	clock_interval_to_deadline(3000, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		int result = bt_rx_frame(uart, event, BT_EVENT_MAX, &length, true);

		if (result < 0) {
			error = -result;
			break;
		}
		error = bt_service_ibs(uart);
		if (error) {
			break;
		}
		if (result == 1 && event[1] == 0x3e && length >= 5 && event[3] == 2) {
			reports += event[4];
		}
		delay(100);
	}
	stop = bt_hci_command(uart, 0x200c, disable, sizeof(disable), true, event, &length);
	printf("ALIOTH BT: LE scan: %u advertisements heard (stop: %d)\n", reports, stop);
	if (error || stop) {
		return error ? error : stop;
	}
	error = bt_hci_command(uart, 0x0401, inquiry, sizeof(inquiry), true, event, &length);
	if (error) {
		return error;
	}
	clock_interval_to_deadline(4000, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		int result = bt_rx_frame(uart, event, BT_EVENT_MAX, &length, true);

		if (result < 0) {
			return -result;
		}
		error = bt_service_ibs(uart);
		if (error) {
			return error;
		}
		if (result == 1) {
			/* inquiry results: plain, with RSSI, extended */
			if ((event[1] == 2 || event[1] == 0x22 || event[1] == 0x2f) && length >= 4) {
				classic += event[3];
			}
			if (event[1] == 1) {
				complete = true;
				break;
			}
		}
		delay(100);
	}
	printf("ALIOTH BT: classic inquiry: %u answers%s; in-band sleep: %u wakes, %u sleeps\n", classic,
	    complete ? "" : " (not over in 4 s)", bt_ibs_wakes, bt_ibs_sleeps);
	if (complete) {
		printf("ALIOTH BT: THE RADIO HEARS: %u LE advertisements, %u classic devices\n", reports, classic);
	}
	return complete ? 0 : ETIMEDOUT;
}

/* ---- step 6: the H4 endpoint for bluetoothd ---------------------------------------------------------- */

/*
 * bluetoothd's H4:POSIX transport is a stream kernel control socket (its job's environment
 * BT_HCI_TRANSPORT=H4:POSIX:com.alioth.bluetooth,115200 names it). Only the worker thread touches
 * the UART; the socket's calls only queue the host's bytes.
 */
static LCK_GRP_DECLARE(bt_control_group, "sweet-bt");
static LCK_MTX_DECLARE(bt_control_lock, &bt_control_group);
static kern_ctl_ref bt_control;
static vm_offset_t bt_control_uart;
static uint32_t bt_control_unit, bt_control_generation;
static uint8_t bt_tx_queue[BT_TX_QUEUE];
static unsigned bt_tx_head, bt_tx_tail;
static bool bt_control_fault;
static struct termios bt_control_termios;

/*
 * bluetoothd's commands answered here on the chip's behalf and never sent (alioth_bt_local_answer): Broadcom vendor
 * commands, Read/Write Default Erroneous Data Reporting, LE Read Periodic Advertiser List Size (0x204a), and those
 * boot-arg aliothbtfake=0xOPCODE[,...] names (up to 8). Boot-arg aliothbtvsc=0 sends everything to the chip, as before.
 */
static bool bt_answer_here;
static uint16_t bt_answer_extra[8];
static unsigned bt_answer_extras;

static errno_t
bt_control_connect(__unused kern_ctl_ref ref, struct sockaddr_ctl *address, void **info)
{
	uid_t uid = kauth_cred_getuid(kauth_cred_get());
	int error;

	if (uid != 0 && uid != 501) {
		return EPERM;
	}
	lck_mtx_lock(&bt_control_lock);
	error = bt_control_fault ? EIO : bt_control_unit ? EBUSY : 0;
	if (!error) {
		bt_control_unit = address->sc_unit;
		bt_control_generation++;
		bt_tx_head = bt_tx_tail = 0;
		*info = NULL;
		printf("ALIOTH BT: H4: connected (uid %u, unit %u)\n", uid, bt_control_unit);
	}
	lck_mtx_unlock(&bt_control_lock);
	return error;
}

static errno_t
bt_control_disconnect(__unused kern_ctl_ref ref, uint32_t unit, __unused void *info)
{
	lck_mtx_lock(&bt_control_lock);
	if (bt_control_unit == unit) {
		bt_control_unit = 0;
		bt_control_generation++;
		bt_tx_head = bt_tx_tail = 0;
		printf("ALIOTH BT: H4: disconnected (unit %u)\n", unit);
	}
	lck_mtx_unlock(&bt_control_lock);
	return 0;
}

static errno_t
bt_control_send(__unused kern_ctl_ref ref, uint32_t unit, __unused void *info, mbuf_t message, __unused int flags)
{
	size_t length = mbuf_pkthdr_len(message);
	int error = 0;

	lck_mtx_lock(&bt_control_lock);
	if (bt_control_fault) {
		error = EIO;
	} else if (unit != bt_control_unit) {
		error = ENOTCONN;
	} else if (length > BT_TX_QUEUE - (bt_tx_head - bt_tx_tail)) {
		error = ENOBUFS;
	}
	for (size_t offset = 0; !error && offset < length;) {
		uint8_t bytes[256];
		size_t count = length - offset > sizeof(bytes) ? sizeof(bytes) : length - offset;

		error = mbuf_copydata(message, offset, count, bytes);
		if (error) {
			break;
		}
		for (size_t i = 0; i < count; i++) {
			bt_tx_queue[bt_tx_head++ & (BT_TX_QUEUE - 1)] = bytes[i];
		}
		offset += count;
	}
	lck_mtx_unlock(&bt_control_lock);
	mbuf_freem(message);
	return error;
}

/* bluetoothd asks the serial port's settings and lines as socket options (the tty ioctl numbers). */
static errno_t
bt_control_getopt(__unused kern_ctl_ref ref, __unused uint32_t unit, __unused void *info, int option, void *data,
    size_t *length)
{
	int lines = TIOCM_CTS | TIOCM_DSR | TIOCM_CAR;
	uint32_t opt = (uint32_t)option;
	size_t wanted;

	if (opt == (uint32_t)TIOCGETA) {
		wanted = sizeof(bt_control_termios);
	} else if (opt == (uint32_t)TIOCMGET) {
		wanted = sizeof(lines);
	} else {
		printf("ALIOTH BT: H4: unknown option %#x asked\n", opt);
		return ENOPROTOOPT;
	}
	if (!data) {
		*length = wanted;
		return 0;
	}
	if (*length < wanted) {
		return EMSGSIZE;
	}
	lck_mtx_lock(&bt_control_lock);
	if (opt == (uint32_t)TIOCGETA) {
		memcpy(data, &bt_control_termios, wanted);
	} else {
		memcpy(data, &lines, wanted);
	}
	lck_mtx_unlock(&bt_control_lock);
	*length = wanted;
	return 0;
}

static errno_t
bt_control_setopt(__unused kern_ctl_ref ref, __unused uint32_t unit, __unused void *info, int option, void *data,
    size_t length)
{
	uint32_t opt = (uint32_t)option;

	if (opt == (uint32_t)TIOCSETA || opt == (uint32_t)TIOCSETAW || opt == (uint32_t)TIOCSETAF) {
		const struct termios *settings = data;

		if (length != sizeof(struct termios) || !data) {
			return EINVAL;
		}
		if ((settings->c_ispeed && settings->c_ispeed != 115200) ||
		    (settings->c_ospeed && settings->c_ospeed != 115200)) {
			return EINVAL;  /* bluetoothd's own idea of the speed (BT_HCI_TRANSPORT); the kernel picks the UART's */
		}
		lck_mtx_lock(&bt_control_lock);
		bt_control_termios = *settings;
		lck_mtx_unlock(&bt_control_lock);
		return 0;
	}
	if (opt == (uint32_t)TIOCSDTR || opt == (uint32_t)TIOCCDTR || opt == (uint32_t)TIOCMSET ||
	    opt == (uint32_t)TIOCMBIS || opt == (uint32_t)TIOCMBIC || opt == (uint32_t)TIOCFLUSH) {
		return 0;
	}
	printf("ALIOTH BT: H4: unknown option %#x set (%u bytes)\n", opt, (unsigned)length);
	return ENOPROTOOPT;
}

static void bt_control_worker(void *unused, wait_result_t result) __attribute__((noreturn));

static void
bt_control_worker(__unused void *unused, __unused wait_result_t result)
{
	static struct alioth_bt_h4 tx;
	static uint8_t receive[ALIOTH_BT_H4_MAX];
	unsigned receive_length = 0, commands = 0, answered = 0;
	uint32_t generation = 0;
	bool waking = false;
	uint64_t wake_deadline = 0;

    __atomic_store_n(&bt_worker_alive, true, __ATOMIC_RELEASE);
	for (;;) {
        if (__atomic_load_n(&bt_stopping, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&bt_worker_alive, false, __ATOMIC_RELEASE);
            (void)thread_terminate(current_thread());
            __builtin_unreachable();
        }
		uint32_t unit;
		bool fault;
		int error = 0;

		lck_mtx_lock(&bt_control_lock);
		unit = bt_control_unit;
		fault = bt_control_fault;
		if (generation != bt_control_generation) {
			generation = bt_control_generation;
			memset(&tx, 0, sizeof(tx));
			receive_length = 0;
			commands = answered = 0;
		}
		while (unit && !fault && !tx.complete && bt_tx_head != bt_tx_tail) {
			uint8_t byte = bt_tx_queue[bt_tx_tail++ & (BT_TX_QUEUE - 1)];
			int state = alioth_bt_h4_push(&tx, byte);

			/* the host sends commands and data, never events or IBS bytes */
			if (state < 0 || state == 2 || (tx.used && tx.bytes[0] == 4)) {
				fault = bt_control_fault = true;
				printf("ALIOTH BT: H4: what the host sent is not H4; the endpoint stops\n");
			}
		}
		lck_mtx_unlock(&bt_control_lock);
		if (unit && !fault && tx.complete) {
			uint8_t local[ALIOTH_BT_LOCAL_EVENT_MAX];
			unsigned answer = bt_answer_here ?
			    alioth_bt_local_answer(tx.bytes, tx.used, bt_answer_extra, bt_answer_extras, local) : 0;

			if (answer) {
				/* answered on the chip's behalf, once any packet of the chip's waiting to go up has gone */
				if (!receive_length) {
					if (answered++ < 128) {
						printf("ALIOTH BT: iOS: HCI %#x (%u bytes) answered here, not sent to the chip\n",
						    bt_le16(tx.bytes + 1), tx.used);
					}
					memcpy(receive, local, answer);
					receive_length = answer;
					memset(&tx, 0, sizeof(tx));
				}
			} else if (!bt_chip_awake) {
				/* the chip may be asleep (IBS): woken first, and the packet goes once it says it is awake */
				if (!waking) {
					const uint8_t wake = 0xfd;

					error = bt_uart_tx(bt_control_uart, &wake, 1);
					clock_interval_to_deadline(1000, NSEC_PER_MSEC, &wake_deadline);
					waking = true;
				} else if (mach_absolute_time() >= wake_deadline) {
					error = ETIMEDOUT;
				}
			} else {
				waking = false;
				if (tx.bytes[0] == 1 && commands++ < 128) {
					printf("ALIOTH BT: iOS: HCI %#x (%u bytes)\n", bt_le16(tx.bytes + 1), tx.used);
				}
				error = bt_uart_tx(bt_control_uart, tx.bytes, tx.used);
				memset(&tx, 0, sizeof(tx));
			}
		}
		if (!error && !receive_length) {
			int state = bt_rx_frame(bt_control_uart, receive, sizeof(receive), &receive_length, false);

			if (state < 0) {
				error = -state;
			}
			if (state > 0 && receive_length >= 7 && receive[0] == 4 && receive[1] == 0x0e && receive[6]) {
				printf("ALIOTH BT: iOS: HCI %#x refused by the chip: status %#x\n", bt_le16(receive + 4), receive[6]);
			}
			if (state > 0 && receive_length >= 7 && receive[0] == 4 && receive[1] == 0x0f && receive[3]) {
				printf("ALIOTH BT: iOS: HCI %#x refused by the chip (Command Status): %#x\n", bt_le16(receive + 5),
				    receive[3]);
			}
			if (state > 0 && bt_answer_here && alioth_bt_fix_supported_commands(receive, receive_length)) {
				printf("ALIOTH BT: iOS: the chip's Supported Commands given octet 18 = %#x (Read/Write Default "
				    "Erroneous Data Reporting, answered here)\n", receive[7 + 18]);
			}
		}
		if (!error) {
			error = bt_service_ibs(bt_control_uart);
		}
		if (error && !fault) {
			lck_mtx_lock(&bt_control_lock);
			bt_control_fault = true;
			lck_mtx_unlock(&bt_control_lock);
			AliothBluetoothSetReady(false);
			printf("ALIOTH BT: H4: the UART failed (errno %d); the endpoint stops\n", error);
		}
		/* (never queued under bt_control_lock: the socket's calls take it) */
		if (receive_length) {
			int queued = unit ? ctl_enqueuedata(bt_control, unit, receive, receive_length, 0) : ENOTCONN;

			if (queued != ENOBUFS) {
				receive_length = 0;
			}
		}
		IOSleep(1);
	}
}

static int
bt_control_start(vm_offset_t uart)
{
	struct kern_ctl_reg registration;
	thread_t thread;
	int error;

	bzero(&registration, sizeof(registration));
	strlcpy(registration.ctl_name, "com.alioth.bluetooth", sizeof(registration.ctl_name));
	registration.ctl_flags = CTL_FLAG_REG_SOCK_STREAM;
	registration.ctl_sendsize = BT_TX_QUEUE;
	registration.ctl_recvsize = BT_TX_QUEUE;
	registration.ctl_connect = bt_control_connect;
	registration.ctl_disconnect = bt_control_disconnect;
	registration.ctl_send = bt_control_send;
	registration.ctl_getopt = bt_control_getopt;
	registration.ctl_setopt = bt_control_setopt;
	{
		uint32_t answer = 1;
		char list[96];

		(void)PE_parse_boot_argn("aliothbtvsc", &answer, sizeof(answer));
		bt_answer_here = answer != 0;
		bzero(list, sizeof(list));
		if (PE_parse_boot_argn("aliothbtfake", list, sizeof(list))) {
			bt_answer_extras = alioth_bt_parse_opcodes(list, bt_answer_extra,
			    sizeof(bt_answer_extra) / sizeof(bt_answer_extra[0]));
		}
		printf("ALIOTH BT: bluetoothd's vendor, erroneous-data-reporting and 0x204a commands %s, %u more opcode(s)\n",
		    bt_answer_here ? "answered here" : "sent to the chip (aliothbtvsc=0)", bt_answer_extras);
	}
	bt_control_termios.c_cflag = CS8 | CREAD | CLOCAL;
	bt_control_termios.c_ispeed = bt_control_termios.c_ospeed = 115200;
	error = ctl_register(&registration, &bt_control);
	if (error) {
		return error;
	}
	error = AliothBluetoothPublish(bt_address, bt_commands);
	if (error) {
		(void)ctl_deregister(bt_control);
		return error;
	}
	bt_control_uart = uart;
	bt_chip_awake = false;
	AliothBluetoothSetReady(true);
	if (kernel_thread_start(bt_control_worker, NULL, &thread) != KERN_SUCCESS) {
		AliothBluetoothSetReady(false);
		(void)ctl_deregister(bt_control);
		return EIO;
	}
	thread_set_thread_name(thread, "sweet-bt-h4");
	thread_deallocate(thread);
	printf("ALIOTH BT: THE RADIO IS UP for iOS: H4 at com.alioth.bluetooth, registry entry \"bluetooth\"\n");
	return 0;
}

/* ---- the steps ------------------------------------------------------------------------------------- */

/* SE6's root for `speed`: GPLL0's even output (parent 6) x m/n, dual-edge M/N (115200: 7.3728 MHz, 384/15625). */
static bool
bt_uart_clock(vm_offset_t gcc, const struct alioth_bt_speed *speed)
{
	bt_write(gcc, GCC_SE6_M, speed->m);
	bt_write(gcc, GCC_SE6_N, ~((uint32_t)speed->n - speed->m) & 0xffff);
	bt_write(gcc, GCC_SE6_D, ~(uint32_t)speed->n & 0xffff);
	bt_write(gcc, GCC_SE6_CFG, 0x2601);
	bt_write(gcc, GCC_SE6_CMD, 1);
	for (unsigned i = 0; i < 10000; i++) {
		if (!(bt_read(gcc, GCC_SE6_CMD) & 1)) {
			return true;
		}
		delay(1);
	}
	return false;
}

/*
 * The chip and the UART moved to `speed`, as Linux's qca_set_speed does for the QCA6390: the set-baudrate
 * vendor command (0xfc48, the speed's QCA index) at the old speed and 300 ms for the chip to take it, then
 * SE6's clock and divider (bt_speed); whatever came in meanwhile is dropped, and Read Local Version must
 * then work at the new speed. 0, or an errno (the chip's speed is then not known).
 */
static int
bt_set_speed(vm_offset_t gcc, vm_offset_t uart, const struct alioth_bt_speed *speed)
{
	uint8_t event[BT_EVENT_MAX];
	unsigned length = 0;
	int error;

	error = bt_hci_command(uart, 0xfc48, &speed->qca, 1, false, event, &length);
	if (error) {
		return error;
	}
	delay(300000);
	error = bt_uart_quiesce(uart);
	if (error) {
		return error;
	}
	if (!bt_uart_clock(gcc, speed)) {
		return ETIMEDOUT;
	}
	bt_speed = speed;
	bt_uart_div = speed->div;
	bt_write(uart, 0x48, (bt_uart_div << 4) | 1);
	bt_write(uart, 0x4c, (bt_uart_div << 4) | 1);
	memset(&bt_rx_packet, 0, sizeof(bt_rx_packet));
	bt_rx_head = bt_rx_tail = 0;
	bt_write(uart, GENI_S_CMD, (1u << 27) | 0x80);      /* reading again; RFR open */
	error = bt_hci_command(uart, 0x1001, NULL, 0, true, event, &length);
	if (!error && length != 15) {
		error = EILSEQ;
	}
	return error;
}

/* A pin as a GPIO: its config (function 0, drive, pull, output enable) and, for an output, its level. */
static void
bt_gpio(vm_offset_t tlmm, unsigned pin, uint32_t cfg, bool high)
{
	bt_write(tlmm, TLMM_IO(pin), high ? 2 : 0);
	bt_write(tlmm, TLMM_CFG(pin), cfg);
}

static bool
bt_clocks_running(vm_offset_t gcc)
{
	return !((bt_read(gcc, GCC_QUP_M_AHB) | bt_read(gcc, GCC_QUP_S_AHB) | bt_read(gcc, GCC_SE6_CBCR)) & CBCR_OFF);
}

/*
 * Steps 4-6: the chip powered and its ROM (4) or its firmware (5, 6) talked to. With 6 and success
 * it stays so (true); otherwise BT_EN goes low again and the UART, its pins and clock are put back.
 */
static bool
bt_chip(vm_offset_t gcc, vm_offset_t tlmm, vm_offset_t uart, unsigned steps)
{
	uint32_t root[5] = { bt_read(gcc, GCC_SE6_CMD), bt_read(gcc, GCC_SE6_CFG), bt_read(gcc, GCC_SE6_M),
		             bt_read(gcc, GCC_SE6_N), bt_read(gcc, GCC_SE6_D) };
	uint32_t en_cfg = bt_read(tlmm, TLMM_CFG(PIN_BT_EN)), en_io = bt_read(tlmm, TLMM_IO(PIN_BT_EN));
	uint32_t sw_cfg = bt_read(tlmm, TLMM_CFG(PIN_SW_CTRL)), sw_io = bt_read(tlmm, TLMM_IO(PIN_SW_CTRL));
	uint32_t pins[4], uart_saved[BT_UART_REGS];
	uint32_t vote = bt_read(gcc, GCC_QUP_VOTE), added = GCC_QUP_BITS & ~vote;
	uint32_t hwcg = bt_read(gcc, GCC_QUP_S_AHB) & 2u;
	uint8_t event[BT_EVENT_MAX];
	unsigned length = 0, i;
	int error, stopped;

	for (unsigned p = 0; p < 4; p++) {
		pins[p] = bt_read(tlmm, TLMM_CFG(16 + p));
	}
	for (unsigned r = 0; r < BT_UART_REGS; r++) {
		uart_saved[r] = bt_read(uart, bt_uart_regs[r]);
	}
	bt_write(gcc, GCC_QUP_S_AHB, bt_read(gcc, GCC_QUP_S_AHB) & ~2u);
	bt_write(gcc, GCC_QUP_VOTE, vote | GCC_QUP_BITS);
	for (i = 0; i < 10000 && !bt_clocks_running(gcc); i++) {
		delay(1);
	}
	error = i == 10000 ? ETIMEDOUT : bt_uart_quiesce(uart);
	bt_speed = &alioth_bt_speeds[0];
	bt_uart_div = bt_speed->div;
	if (!error && !bt_uart_clock(gcc, bt_speed)) {
		error = ETIMEDOUT;
	}
	if (!error) {
		/* (the rails are shared with the Wi-Fi, whose thread may be voting just now) */
		for (unsigned tries = 0; tries < 50; tries++) {
			error = alioth_wlan_rpmh_bt_power();
			if (error != EBUSY) {
				break;
			}
			delay(100000);
		}
	}
	if (error) {
		printf("ALIOTH BT: step 4: clocks or rails failed: errno %d\n", error);
		goto put_back;
	}
	/*
	 * BT_EN low, then high, as Android's btpower.c: 50 ms and more each (umi's 56). The ROM answers at
	 * 115200 baud; from step 5 on the UART then goes to aliothbtbaud, and if that, or the firmware at that
	 * speed, does not work, the chip is started over and everything stays at 115200.
	 */
again:
	for (unsigned attempt = 0;; attempt++) {
		if (bt_speed != &alioth_bt_speeds[0]) {
			bt_speed = &alioth_bt_speeds[0];
			bt_uart_div = bt_speed->div;
			(void)bt_uart_quiesce(uart);
			if (!bt_uart_clock(gcc, bt_speed)) {
				error = ETIMEDOUT;
				break;
			}
		}
		bt_gpio(tlmm, PIN_BT_EN, 0x241, false);             /* GPIO, 4 mA, pull-down, output */
		if (bt_swctrl_drive) {
			bt_gpio(tlmm, PIN_SW_CTRL, 0x240, true);    /* as umi: output, high */
		} else {
			bt_gpio(tlmm, PIN_SW_CTRL, 0x040, false);   /* an input, no pull, as Android */
		}
		delay(56000);
		bt_gpio(tlmm, PIN_BT_EN, 0x241, true);
		delay(56000);
		printf("ALIOTH BT: step 4: BT_EN high; SW_CTRL reads %u\n", bt_read(tlmm, TLMM_IO(PIN_SW_CTRL)) & 1);
		/* the UART's pins: QUP function 1; RTS (17) pulled down, TX (18) pulled up, as the DT's active state */
		for (unsigned pin = 16; pin <= 19; pin++) {
			bt_write(tlmm, TLMM_CFG(pin), pin == 17 ? 5u : pin == 18 ? 7u : 4u);
		}
		bt_uart_setup(uart);
		bt_write(uart, GENI_S_CMD, (1u << 27) | 0x80);      /* start reading; RFR open */
		memset(&bt_rx_packet, 0, sizeof(bt_rx_packet));
		bt_rx_head = bt_rx_tail = 0;
		bt_chip_awake = bt_ibs_ack_pending = false;
		bt_ibs_wakes = bt_ibs_sleeps = 0;
		error = bt_hci_command(uart, 0x0c03, NULL, 0, true, event, &length);
		if (!error) {
			error = bt_hci_command(uart, 0x1001, NULL, 0, true, event, &length);
		}
		if (error || length != 15) {
			break;
		}
		printf("ALIOTH BT: step 4: THE CHIP ANSWERS from its ROM: HCI %#x, manufacturer %u, subversion %#x\n",
		    event[7], bt_le16(event + 11), bt_le16(event + 13));
		if (steps < 5 || attempt || bt_want == NULL || bt_want == bt_speed) {
			break;
		}
		error = bt_set_speed(gcc, uart, bt_want);
		if (!error) {
			printf("ALIOTH BT: the UART now runs at %u baud (aliothbtbaud)\n", bt_speed->baud);
			break;
		}
		printf("ALIOTH BT: %u baud did not work (errno %d): the chip is started over at 115200\n",
		    bt_want->baud, error);
	}
	if (!error && length == 15) {
		if (steps >= 5) {
			error = bt_qca_firmware(uart);
			if (error && bt_speed != &alioth_bt_speeds[0]) {
				printf("ALIOTH BT: the firmware did not go in at %u baud (errno %d): all again at 115200\n",
				    bt_speed->baud, error);
				bt_want = &alioth_bt_speeds[0];
				goto again;
			}
			if (!error && steps == 5) {
				error = bt_radio_scan(uart);
			}
			if (!error && steps == 6) {
				error = bt_control_start(uart);
				if (!error) {
					return true;
				}
			}
		}
	} else if (!error) {
		error = EILSEQ;
	}
	printf("ALIOTH BT: step %u: errno %d; BT_EN low again\n", steps, error);
	bt_gpio(tlmm, PIN_BT_EN, 0x241, false);
	stopped = bt_uart_quiesce(uart);
	if (stopped) {
		printf("ALIOTH BT: the UART did not stop: errno %d\n", stopped);
	}
put_back:
	for (unsigned r = 0; r < BT_UART_REGS; r++) {
		bt_write(uart, bt_uart_regs[r], uart_saved[r]);
	}
	bt_write(uart, GENI_M_IRQ_CLR, 0xffffffff);
	bt_write(uart, GENI_S_IRQ_CLR, 0xffffffff);
	bt_write(tlmm, TLMM_IO(PIN_BT_EN), en_io & 2);
	bt_write(tlmm, TLMM_CFG(PIN_BT_EN), en_cfg);
	bt_write(tlmm, TLMM_IO(PIN_SW_CTRL), sw_io & 2);
	bt_write(tlmm, TLMM_CFG(PIN_SW_CTRL), sw_cfg);
	for (unsigned p = 0; p < 4; p++) {
		bt_write(tlmm, TLMM_CFG(16 + p), pins[p]);
	}
	bt_write(gcc, GCC_SE6_M, root[2]);
	bt_write(gcc, GCC_SE6_N, root[3]);
	bt_write(gcc, GCC_SE6_D, root[4]);
	bt_write(gcc, GCC_SE6_CFG, root[1]);
	bt_write(gcc, GCC_SE6_CMD, root[0] | 1);
	for (i = 0; i < 10000 && (bt_read(gcc, GCC_SE6_CMD) & 1); i++) {
		delay(1);
	}
	if (added) {
		bt_write(gcc, GCC_QUP_VOTE, bt_read(gcc, GCC_QUP_VOTE) & ~(added & (1u << 16)));
	}
	bt_write(gcc, GCC_QUP_S_AHB, (bt_read(gcc, GCC_QUP_S_AHB) & ~2u) | hwcg);
	return false;
}

static void
bt_run(unsigned steps)
{
	static const uint32_t clocks[] = {
		GCC_QUP_VOTE, GCC_QUP_M_AHB, GCC_QUP_S_AHB, GCC_SE6_CBCR, GCC_SE6_CMD, GCC_SE6_CFG, GCC_SE6_M,
		GCC_SE6_N, GCC_SE6_D
	};
	static const unsigned pin_list[] = { 16, 17, 18, 19, PIN_BT_EN, PIN_SW_CTRL };
	static const uint32_t geni[] = {
		GENI_STATUS, 0x48, 0x4c, 0x64, 0x68, 0x6c, 0x7c, 0x258, GENI_M_IRQ, GENI_S_IRQ, GENI_TX_STATUS,
		GENI_RX_STATUS, GENI_TX_DEPTH, GENI_RX_DEPTH
	};
	vm_offset_t gcc = ml_io_map(GCC_PA, 0x80000), tlmm = ml_io_map(TLMM_NORTH_PA, 0x80000), uart;
	uint32_t added = 0, hwcg = 0;

	bt_saved_tlmm = tlmm;
	if (!gcc || !tlmm) {
		printf("ALIOTH BT: GCC or TLMM not mapped\n");
		return;
	}
	for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
		printf("ALIOTH BT: step 1: GCC %#x = %#x\n", clocks[i], bt_read(gcc, clocks[i]));
	}
	for (unsigned i = 0; i < sizeof(pin_list) / sizeof(pin_list[0]); i++) {
		printf("ALIOTH BT: step 1: gpio%u config %#x io %#x\n", pin_list[i], bt_read(tlmm, TLMM_CFG(pin_list[i])),
		    bt_read(tlmm, TLMM_IO(pin_list[i])));
	}
	if (steps < 2) {
		return;
	}
	/* step 2 on: the UART's clocks voted for as long as it is looked at */
	{
		uint32_t vote = bt_read(gcc, GCC_QUP_VOTE);
		unsigned waited;

		added = GCC_QUP_BITS & ~vote;
		hwcg = bt_read(gcc, GCC_QUP_S_AHB) & 2u;
		bt_write(gcc, GCC_QUP_S_AHB, bt_read(gcc, GCC_QUP_S_AHB) & ~2u);
		bt_write(gcc, GCC_QUP_VOTE, vote | GCC_QUP_BITS);
		for (waited = 0; waited < 10000 && !bt_clocks_running(gcc); waited++) {
			delay(1);
		}
		printf("ALIOTH BT: step 2: the UART's clocks after %u us: %#x %#x %#x\n", waited,
		    bt_read(gcc, GCC_QUP_M_AHB), bt_read(gcc, GCC_QUP_S_AHB), bt_read(gcc, GCC_SE6_CBCR));
	}
	/* (a GENI register read without its clocks can stall the interconnect) */
	if (!bt_clocks_running(gcc)) {
		printf("ALIOTH BT: step 2: the UART's clocks do not run: its registers are not read\n");
	} else if ((uart = ml_io_map(SE6_PA, 0x4000)) != 0) {
		for (unsigned i = 0; i < sizeof(geni) / sizeof(geni[0]); i++) {
			printf("ALIOTH BT: step 2: GENI %#x = %#x\n", geni[i], bt_read(uart, geni[i]));
		}
		if (steps == 3) {
			bt_loopback(uart);
		}
        if (steps >= 4) {
            bt_saved_uart = uart;
            bt_powered = bt_chip(gcc, tlmm, uart, steps);
        }
		if (steps >= 4 && bt_powered) {
			return;                 /* step 6: the radio stays up, with its clocks */
		}
	}
	bt_write(gcc, GCC_QUP_S_AHB, (bt_read(gcc, GCC_QUP_S_AHB) & ~2u) | hwcg);
	if (added) {
		bt_write(gcc, GCC_QUP_VOTE, bt_read(gcc, GCC_QUP_VOTE) & ~(added & (1u << 16)));
	}
}

static void
bt_thread(__unused void *unused, __unused wait_result_t result)
{
    __atomic_store_n(&bt_boot_alive, true, __ATOMIC_RELEASE);
    for (unsigned ms = 0; ms < bt_delay * 1000 && !bt_stopping; ms += 100) IOSleep(100);
    if (bt_stopping) {
        __atomic_store_n(&bt_boot_alive, false, __ATOMIC_RELEASE);
        (void)thread_terminate(current_thread());
        return;
    }
	printf("ALIOTH BT: steps 1..%u\n", bt_steps);
	bt_run(bt_steps);
	printf("ALIOTH BT: done: steps 1..%u\n", bt_steps);
    __atomic_store_n(&bt_boot_alive, false, __ATOMIC_RELEASE);
	(void)thread_terminate(current_thread());
}

void
alioth_bt_init(void)
{
	thread_t thread;

	(void)PE_parse_boot_argn("aliothbt", &bt_steps, sizeof(bt_steps));
	(void)PE_parse_boot_argn("aliothbtdelay", &bt_delay, sizeof(bt_delay));
	(void)PE_parse_boot_argn("aliothbtswctrl", &bt_swctrl_drive, sizeof(bt_swctrl_drive));
	(void)PE_parse_boot_argn("aliothbtbaud", &bt_baud, sizeof(bt_baud));
	bt_want = alioth_bt_speed_for(bt_baud);
	if (bt_want == NULL) {
		printf("ALIOTH BT: aliothbtbaud=%u is not a speed this knows; 115200\n", bt_baud);
		bt_want = &alioth_bt_speeds[0];
	}
	if (!bt_steps) {
		return;
	}
	if (bt_steps > 6) {
		bt_steps = 6;
	}
	if (bt_delay > 600) {
		bt_delay = 600;
	}
	if (kernel_thread_start(bt_thread, NULL, &thread) == KERN_SUCCESS) {
		printf("ALIOTH BT: queued steps 1..%u after %u seconds\n", bt_steps, bt_delay);
		thread_set_thread_name(thread, "alioth-bt");
		thread_deallocate(thread);
	} else {
		printf("ALIOTH BT: kernel_thread_start failed\n");
	}
}
void alioth_bt_quiesce(void)
{
    __atomic_store_n(&bt_stopping, true, __ATOMIC_RELEASE);
    if (bt_powered && bt_saved_tlmm) bt_gpio(bt_saved_tlmm, PIN_BT_EN, 1u << 9, false);
}
void alioth_bt_stop(void)
{
    __atomic_store_n(&bt_stopping, true, __ATOMIC_RELEASE);
    for (unsigned ms = 0; ms < 5000 && (bt_boot_alive || bt_worker_alive); ms++) IOSleep(1);
    if (bt_powered) {
        AliothBluetoothSetReady(false);
        (void)bt_uart_quiesce(bt_saved_uart);
        alioth_bt_quiesce();
        bt_powered = false;
        printf("ALIOTH BT: worker stopped, UART quiesced, BT_EN low\n");
    }
}
#endif /* PLATFORM_ALIOTH */
