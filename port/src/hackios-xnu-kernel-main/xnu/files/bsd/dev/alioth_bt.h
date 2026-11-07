/*
 * Bluetooth H4 framing and the QCA6390's firmware records, for alioth_bt.c (xnu 0136).
 *
 * From the Xiaomi Mi 10 port of github.com/JopangJovan/hackios-xnu-kernel-umi,
 * devices/xiaomi-mi-10/overlays/xnu/files/bsd/sys/hakios.bluetooth.h (MIT). Between whole H4
 * packets the chip also sends single bytes of its in-band sleep protocol (IBS): 0xfd "wake",
 * 0xfe "sleep", 0xfc "awake, acknowledged".
 */
#ifndef ALIOTH_BT_H
#define ALIOTH_BT_H
#include <stdint.h>
#include <stddef.h>

#define ALIOTH_BT_H4_MAX 4096u

struct alioth_bt_h4 {
	uint8_t bytes[ALIOTH_BT_H4_MAX];
	uint32_t used, wanted, header;
	int complete;
};

/*
 * One more byte of an H4 stream: 0 not yet a packet, 1 a packet is complete (bytes[0..used)), 2 an
 * IBS byte, -1 a byte that begins no packet type, -2 a packet longer than ALIOTH_BT_H4_MAX. After
 * -1 or -2 the stream is out of step and the caller starts over.
 */
static inline int
alioth_bt_h4_push(struct alioth_bt_h4 *p, uint8_t byte)
{
	if (p->complete) {
		p->used = p->wanted = p->header = 0;
		p->complete = 0;
	}
	if (p->used == 0) {
		if (byte >= 0xfc && byte <= 0xfe) {
			return 2;
		}
		switch (byte) {
		case 1:                 /* command: type, opcode (2), length (1) */
		case 3:                 /* SCO data: type, handle (2), length (1) */
			p->header = 4;
			break;
		case 2:                 /* ACL data: type, handle (2), length (2) */
		case 5:                 /* ISO data: type, handle (2), length (2, 14 bits) */
			p->header = 5;
			break;
		case 4:                 /* event: type, code, length (1) */
			p->header = 3;
			break;
		default:
			return -1;
		}
		p->wanted = p->header;
	}
	if (p->used >= ALIOTH_BT_H4_MAX) {
		return -2;
	}
	p->bytes[p->used++] = byte;
	if (p->used == p->header) {
		uint32_t length;

		switch (p->bytes[0]) {
		case 1:
		case 3:
			length = p->bytes[3];
			break;
		case 4:
			length = p->bytes[2];
			break;
		default:
			length = p->bytes[3] | (uint32_t)p->bytes[4] << 8;
			if (p->bytes[0] == 5) {
				length &= 0x3fff;
			}
			break;
		}
		if (length > ALIOTH_BT_H4_MAX - p->header) {
			p->used = p->wanted = p->header = 0;
			return -2;
		}
		p->wanted = p->header + length;
	}
	if (p->used == p->wanted) {
		p->complete = 1;
		return 1;
	}
	return 0;
}

/* The status of an HCI Command Complete event for `opcode`, or -1 if `e` is not one. */
static inline int
alioth_bt_cc_status(const uint8_t *e, size_t n, uint16_t opcode)
{
	if (n < 7 || e[0] != 4 || e[1] != 0x0e || (size_t)e[2] + 3 != n ||
	    e[4] != (uint8_t)opcode || e[5] != (uint8_t)(opcode >> 8)) {
		return -1;
	}
	return e[6];
}

/* Whether `e` acknowledges a firmware record segment (0xfc00, EDL patch request 0x1e). */
static inline int
alioth_bt_qca_tlv_ack(const uint8_t *e, size_t n)
{
	/* the QCA6390 ("Hastings") answers with Command Complete, its sub-opcode included */
	return (n == 8 && alioth_bt_cc_status(e, n, 0xfc00) == 0 && e[7] == 0x1e) ||
	       (n == 6 && e[0] == 4 && e[1] == 0xff && e[2] == 3 && e[3] == 0 && e[4] == 4 && e[5] == 0);
}

/*
 * The UART speeds this knows (xnu 0145): the QCA set-baudrate/NVM index (Linux's enum qca_baudrate)
 * and how SE6 makes it -- its root clock from GPLL0's even output (300 MHz) x m / n, an M/N/D
 * counter, as Linux's gcc-kona QUP table, and the GENI divider: root = 16 x baud x div.
 */
struct alioth_bt_speed {
	uint32_t baud;
	uint8_t qca;
	uint16_t m, n;
	uint8_t div;
};

static const struct alioth_bt_speed alioth_bt_speeds[] = {
	{ 115200, 0, 384, 15625, 4 },   /* 7.3728 MHz, the speed the chip's ROM starts at */
	{ 230400, 5, 384, 15625, 2 },
	{ 460800, 7, 384, 15625, 1 },
	{ 921600, 10, 768, 15625, 1 },  /* 14.7456 MHz */
	{ 1000000, 11, 4, 75, 1 },      /* 16 MHz */
	{ 2000000, 13, 8, 75, 1 },      /* 32 MHz */
	{ 3000000, 14, 4, 25, 1 },      /* 48 MHz */
	{ 3200000, 17, 64, 375, 1 },    /* 51.2 MHz, Linux's operating speed for the QCA6390 */
};

/* The entry for `baud`, or NULL. */
static inline const struct alioth_bt_speed *
alioth_bt_speed_for(uint32_t baud)
{
	for (size_t i = 0; i < sizeof(alioth_bt_speeds) / sizeof(alioth_bt_speeds[0]); i++) {
		if (alioth_bt_speeds[i].baud == baud) {
			return &alioth_bt_speeds[i];
		}
	}
	return NULL;
}

/*
 * The NVM file (TLV type 2) made to put the UART at the speed with QCA index `qca` once the
 * firmware runs: tag 17 (the HCI transport) has the baud rate in its byte 1 on the QCA6390 (byte 2
 * on the older ROME), as Linux's qca_tlv_check_data sets it. Every record is checked before
 * anything is changed. 0, or -1 if the file is not what it should be.
 */
static inline int
alioth_bt_qca6390_nvm_prepare(uint8_t *data, size_t n, uint8_t qca)
{
	size_t hci = 0;

	if (n < 4 || data[0] != 2 ||
	    ((uint32_t)data[1] | (uint32_t)data[2] << 8 | (uint32_t)data[3] << 16) != n - 4) {
		return -1;
	}
	for (size_t off = 4; off < n;) {
		unsigned id, len;

		if (n - off < 12) {
			return -1;
		}
		id = data[off] | (unsigned)data[off + 1] << 8;
		len = data[off + 2] | (unsigned)data[off + 3] << 8;
		if (len > n - off - 12) {
			return -1;
		}
		if (id == 17) {
			if (len < 3 || hci) {
				return -1;
			}
			hci = off + 12;
		}
		off += 12 + len;
	}
	if (!hci) {
		return -1;
	}
	data[hci + 1] = qca;
	return 0;
}
/*
 * Apple's bluetoothd was written for Broadcom-descended chips and sends their vendor commands (OGF 0x3f, opcodes
 * 0xfcxx: sleep mode, BD_ADDR, verbose config, LE features...) while it starts the controller. The QCA6390 does not
 * know them, or takes them as its own: Qualcomm's vendor commands share the range. bluetoothd also treats the
 * optional Read/Write Default Erroneous Data Reporting (0x0c5a/0x0c5b), which the QCA6390 lacks, as mandatory. Those
 * are answered here on the chip's behalf instead of being sent, as the Xiaomi 15 port answers them
 * (github.com/superboyyy/hakios-arm64e-dada, kernel patch, hakios_dada_wifi.c bt_answer_vendor; APSL 2.0). The Mi 10
 * port skipped the same init step inside bluetoothd instead (inject_bluetooth.c).
 *
 * LE Read Periodic Advertiser List Size (0x204a) is answered here too. It is one of Bluetooth 5's extended
 * advertising commands, and once a controller has had one of those since reset the specification makes it refuse
 * the legacy advertising and scanning commands with Command Disallowed (Core 5.x Vol 4 Part E 3.1.1). bluetoothd asks
 * for it during init (step 0x10b590, OI_HCICMD_ReadPeriodicAdvListSize) but then advertises and scans the legacy way
 * (enableExtendedScanning=0): the QCA6390 refused 0x2007 and LE Set Scan Parameters 0x200b with 0x0c, and bluetoothd
 * restarted ("Bluetooth error - restarting", reason 712; apollo boot 75). The Mi 10 port replaced the same step.
 *
 * If the H4 command cmd[0..n) is one to answer here (a vendor command, 0x0c5a, 0x0c5b, 0x204a, or one of
 * extra[0..nextra)), its Command Complete goes into ev (ALIOTH_BT_LOCAL_EVENT_MAX bytes) and its length is returned;
 * 0 sends it on:
 *   vendor: status 0, the first parameter byte echoed (often a sub-opcode) and 8 zero bytes -- bluetoothd's vendor
 *     command code asserts on an answer with nothing after the status (dada port: aci_vsc.c:135)
 *   0x0c5a: status 0 and the setting, 0 (off)
 *   0x204a: status 0 and the list size, 0 (bluetoothd does not use periodic advertising here)
 *   others: status 0 alone
 */
#define ALIOTH_BT_LOCAL_EVENT_MAX 16u

static inline unsigned
alioth_bt_local_answer(const uint8_t *cmd, size_t n, const uint16_t *extra, unsigned nextra, uint8_t *ev)
{
	uint16_t op;
	int vendor, hit;
	unsigned len;

	if (n < 4 || cmd[0] != 1 || (size_t)cmd[3] + 4 != n) {
		return 0;
	}
	op = (uint16_t)(cmd[1] | cmd[2] << 8);
	vendor = (op & 0xfc00) == 0xfc00;
	hit = vendor || op == 0x0c5a || op == 0x0c5b || op == 0x204a;
	for (unsigned i = 0; !hit && i < nextra; i++) {
		hit = extra[i] != 0 && extra[i] == op;
	}
	if (!hit) {
		return 0;
	}
	for (unsigned i = 0; i < ALIOTH_BT_LOCAL_EVENT_MAX; i++) {
		ev[i] = 0;
	}
	ev[0] = 4;                      /* an event */
	ev[1] = 0x0e;                   /* Command Complete */
	ev[3] = 1;                      /* the host may send one more command */
	ev[4] = cmd[1];
	ev[5] = cmd[2];
	if (vendor) {
		ev[7] = n > 4 ? cmd[4] : 0;
		len = 7 + 1 + 8;
	} else if (op == 0x0c5a || op == 0x204a) {
		len = 8;
	} else {
		len = 7;
	}
	ev[2] = (uint8_t)(len - 3);
	return len;
}

/*
 * The chip's answer to Read Local Supported Commands (0x1002: Command Complete, status 0, 64 octets) given octet 18
 * bits 2 and 3, Read/Write Default Erroneous Data Reporting, which are answered here: bluetoothd will not start
 * without them in the mask (dada port, "status 628"). 1 if e[0..n) was that answer.
 */
static inline int
alioth_bt_fix_supported_commands(uint8_t *e, size_t n)
{
	if (n != 7 + 64 || alioth_bt_cc_status(e, n, 0x1002) != 0) {
		return 0;
	}
	e[7 + 18] |= 0x0c;
	return 1;
}

/* "0x0c5b,0x2011": up to max hex opcodes (0x optional) into out[]; how many. Stops at anything else. */
static inline unsigned
alioth_bt_parse_opcodes(const char *s, uint16_t *out, unsigned max)
{
	unsigned count = 0;

	while (*s != 0 && count < max) {
		uint32_t v = 0;
		int digits = 0;

		if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
			s += 2;
		}
		for (;; s++, digits++) {
			int d = (*s >= '0' && *s <= '9') ? *s - '0' : (*s >= 'a' && *s <= 'f') ? *s - 'a' + 10 :
			    (*s >= 'A' && *s <= 'F') ? *s - 'A' + 10 : -1;

			if (d < 0 || digits > 4) {
				break;
			}
			v = v << 4 | (uint32_t)d;
		}
		if (!digits || digits > 4) {
			break;
		}
		out[count++] = (uint16_t)v;
		if (*s != ',') {
			break;
		}
		s++;
	}
	return count;
}
#endif /* ALIOTH_BT_H */
