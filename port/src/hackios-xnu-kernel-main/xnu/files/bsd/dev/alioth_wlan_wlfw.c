/* Alioth adaptation: Apollo PCIe/MHI implementation, APSL 2.0.
 * GPIOs, PHY sequence, six supply resources/voltages, MSI and SID
 * checked against this K40 stock-device-tree.dtb. Opt-in aliothwifi=N. */
/*
 * Wi-Fi of apollo, phase 2: the QCA6390 firmware's WLFW service (QMI service 0x45, instance 1),
 * spoken to the way a driver does before it touches the radio. Steps 8-10 of aliothwifi=N
 * (alioth_wlan_wifi.c), on the QRTR transport alioth_wlan_wifi.c installs once the firmware runs:
 *
 *   8  indications wanted; host capabilities; the memory the firmware asks for (its "DDR" and
 *      calibration segments, at stock CNSS's fixed IOVAs); "memory ready"; what the chip, the
 *      board and the firmware are (logged)
 *   9  the regulatory database (if given), the board's data, the M3 image; "init done"
 *  10  cold-boot calibration: "calibration done" or "ready"
 *
 * A stand-in for ath11k's own qmi.c, to learn whether this firmware takes these messages, the
 * phone's board file and M3, before the driver itself is ported. From the Xiaomi Mi 10 (umi) port
 * of github.com/JopangJovan/hackios-xnu-kernel-umi, hakios.wlfw.c redwood_wlfw_query() (APSL 2.0);
 * its identity checks (firmware version, M3 size, board id of the Mi 10) are only logged here.
 *
 * The message structures and tables are ath11k's (qmi.h, qmi.c of Linux 7.0.14, BSD-3-Clause-Clear),
 * made into alioth_wlan_wlfw_ath11k.h when the kernel is built (tools/mk-wlfw-header.py,
 * .github/workflows/xnu.yml); they are not kept in the repository.
 */
#if defined(PLATFORM_ALIOTH)
#include <sys/cdefs.h>
extern int alioth_wlan_log(const char *, ...) __printflike(1, 2);
#define printf alioth_wlan_log
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <sys/redwood_qmi.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include "alioth_wlan_wlfw_ath11k.h"
#pragma clang diagnostic pop

int alioth_wlan_wlfw_run(unsigned steps);
extern const uint8_t *alioth_wlan_wifi_file(const char *name, unsigned int *size);
extern void *alioth_wlan_wifi_memory_at(size_t size, uint64_t iova);
extern void alioth_wlan_wifi_clean(const void *memory, size_t size);

#define MODE_COLD_BOOT          7               /* core.h: ATH11K_FIRMWARE_MODE_COLD_BOOT */
#define QCA_DDR_IOVA            0xa1000000ull   /* stock CNSS: the firmware's main region */
#define QCA_CAL_IOVA            0xa1600000ull   /* ... its cold-boot calibration region */
#define QCA_M3_IOVA             0xa1880000ull   /* ... M3 after the calibration area */
#define QCA_WAKE_MSI            813u            /* stock CNSS: WAKE after the three MHI vectors */

static struct qmi_handle handle;
static struct {
	volatile bool server, gone, mem_request, mem_ready, init_done, ready, cal_done;
	uint64_t began;
	uint32_t mem_seg_count;
	struct {
		uint32_t size, type;
		uint64_t address;
	} mem_seg[ATH11K_QMI_WLANFW_MAX_NUM_MEM_SEG_V01];
} wlfw;

static uint64_t
now_ms(void)
{
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_MSEC;
}

static int
wlfw_new_server(struct qmi_handle *qmi, struct qmi_service *service)
{
	int error = redwood_qmi_connect(qmi->sock, service->node, service->port);
	printf("ALIOTH WIFI: WLFW at node %u port %#x (service %#x version %u instance %u)%s\n", service->node, service->port,
	    service->service, service->version, service->instance, error ? ": CANNOT BE CONNECTED TO" : "");
	wlfw.server = error == 0;
	return error;
}

static void
wlfw_del_server(struct qmi_handle *qmi, struct qmi_service *service)
{
	(void)qmi;
	printf("ALIOTH WIFI: WLFW AT NODE %u PORT %#x HAS GONE\n", service->node, service->port);
	wlfw.gone = true;
}

static void
wlfw_said(volatile bool *flag, const char *what)
{
	*flag = true;
	printf("ALIOTH WIFI: +%llu ms: THE FIRMWARE SAYS \"%s\"\n", now_ms() - wlfw.began, what);
}

static void
wlfw_mem_request(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, struct qmi_txn *txn, const void *decoded)
{
	(void)qmi;
	(void)sq;
	(void)txn;
	const struct qmi_wlanfw_request_mem_ind_msg_v01 *request = decoded;
	wlfw.mem_seg_count = request->mem_seg_len;
	if (wlfw.mem_seg_count > ATH11K_QMI_WLANFW_MAX_NUM_MEM_SEG_V01) {
		wlfw.mem_seg_count = ATH11K_QMI_WLANFW_MAX_NUM_MEM_SEG_V01;
	}
	for (uint32_t i = 0; i < wlfw.mem_seg_count; i++) {
		wlfw.mem_seg[i].size = request->mem_seg[i].size;
		wlfw.mem_seg[i].type = request->mem_seg[i].type;
		printf("ALIOTH WIFI:   memory segment %u: type %u, %#x bytes, %u configurations\n", i, request->mem_seg[i].type,
		    request->mem_seg[i].size, request->mem_seg[i].mem_cfg_len);
	}
	wlfw_said(&wlfw.mem_request, "give me memory");
}

static void
wlfw_mem_ready(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, struct qmi_txn *txn, const void *decoded)
{
	(void)qmi;
	(void)sq;
	(void)txn;
	(void)decoded;
	wlfw_said(&wlfw.mem_ready, "memory ready");
}

static void
wlfw_ready(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, struct qmi_txn *txn, const void *decoded)
{
	(void)qmi;
	(void)sq;
	(void)txn;
	(void)decoded;
	wlfw_said(&wlfw.ready, "firmware ready");
}

static void
wlfw_cal_done(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, struct qmi_txn *txn, const void *decoded)
{
	(void)qmi;
	(void)sq;
	(void)txn;
	(void)decoded;
	wlfw_said(&wlfw.cal_done, "calibration done");
}

static void
wlfw_init_done(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, struct qmi_txn *txn, const void *decoded)
{
	(void)qmi;
	(void)sq;
	(void)txn;
	(void)decoded;
	wlfw_said(&wlfw.init_done, "init done");
}

static const struct qmi_ops wlfw_ops = { .new_server = wlfw_new_server, .del_server = wlfw_del_server };
static const struct qmi_msg_handler wlfw_handlers[] = {
	{ QMI_INDICATION, QMI_WLANFW_REQUEST_MEM_IND_V01, qmi_wlanfw_request_mem_ind_msg_v01_ei,
	  sizeof(struct qmi_wlanfw_request_mem_ind_msg_v01), wlfw_mem_request },
	{ QMI_INDICATION, QMI_WLFW_FW_MEM_READY_IND_V01, qmi_wlanfw_mem_ready_ind_msg_v01_ei,
	  sizeof(struct qmi_wlanfw_fw_mem_ready_ind_msg_v01), wlfw_mem_ready },
	{ QMI_INDICATION, QMI_WLFW_FW_READY_IND_V01, qmi_wlanfw_fw_ready_ind_msg_v01_ei,
	  sizeof(struct qmi_wlanfw_fw_ready_ind_msg_v01), wlfw_ready },
	{ QMI_INDICATION, QMI_WLFW_COLD_BOOT_CAL_DONE_IND_V01, qmi_wlanfw_cold_boot_cal_done_ind_msg_v01_ei,
	  sizeof(struct qmi_wlanfw_fw_cold_cal_done_ind_msg_v01), wlfw_cal_done },
	{ QMI_INDICATION, QMI_WLFW_FW_INIT_DONE_IND_V01, qmi_wlfw_fw_init_done_ind_msg_v01_ei,
	  sizeof(struct qmi_wlfw_fw_init_done_ind_msg_v01), wlfw_init_done },
	{ 0, 0, NULL, 0, NULL },
};

/* One request and its reply; true when the firmware says "success". A refused request is dumped. */
static bool
ask(const char *what, int message, size_t largest, const struct qmi_elem_info *request_ei, const void *request,
    const struct qmi_elem_info *reply_ei, void *reply, bool quiet)
{
	const struct qmi_response_type_v01 *result = reply;
	static uint8_t bytes[QMI_HEADER_SIZE + 6400];
	struct qmi_txn txn;
	uint64_t began = now_ms();
	int ret = qmi_txn_init(&handle, &txn, reply_ei, reply);
	if (ret >= 0) {
		ret = qmi_send_request(&handle, NULL, &txn, message, largest, request_ei, request);
		if (ret < 0) {
			qmi_txn_cancel(&txn);
		} else {
			ret = redwood_qmi_txn_wait_ms(&txn, ATH11K_QMI_WLANFW_TIMEOUT_MS);
		}
	}
	if (ret >= 0 && result->result == QMI_RESULT_SUCCESS_V01) {
		if (!quiet) {
			printf("ALIOTH WIFI: %s (message %#x): success, %llu ms\n", what, message, now_ms() - began);
		}
		return true;
	}
	if (ret < 0) {
		printf("ALIOTH WIFI: %s (message %#x): NO ANSWER THAT CAN BE READ: %d, %llu ms\n", what, message, ret,
		    now_ms() - began);
	} else {
		printf("ALIOTH WIFI: %s (message %#x): THE FIRMWARE REFUSES: result %u error %u, %llu ms\n", what, message,
		    result->result, result->error, now_ms() - began);
	}
	ret = redwood_qmi_encode(QMI_REQUEST, (uint16_t)message, 0, request_ei, request, bytes, sizeof(bytes));
	for (int at = 0; at < ret && at < 96; at += 24) {
		char text[3 * 24 + 1] = "";
		for (int i = 0; i < 24 && at + i < ret; i++) {
			snprintf(text + 3 * i, 4, " %02x", bytes[at + i]);
		}
		printf("ALIOTH WIFI:   the request, %d bytes, from %d:%s\n", ret, at, text);
	}
	return false;
}

/* A file to the firmware in pieces, as ath11k_qmi_load_file_target_mem() sends one. */
static bool
send_file(const char *what, const char *name, uint8_t type, uint32_t board_id)
{
	static struct qmi_wlanfw_bdf_download_req_msg_v01 request;
	struct qmi_wlanfw_bdf_download_resp_msg_v01 reply;
	unsigned int size = 0;
	const uint8_t *data = alioth_wlan_wifi_file(name, &size);
	uint32_t left, pieces = 0;
	uint64_t began = now_ms();
	if (data == NULL || size == 0 || size > 256u * 1024u) {
		printf("ALIOTH WIFI: %s: no /sweet-wifi \"%s\" in the ADT (or %u bytes: too big)\n", what, name, size);
		return false;
	}
	printf("ALIOTH WIFI: %s: %u bytes, beginning %02x %02x %02x %02x, as type %u for board id %#x\n", what, size, data[0],
	    data[1], data[2], data[3], type, board_id);
	bzero(&request, sizeof(request));
	for (left = size; left > 0; pieces++) {
		request.valid = 1;
		request.file_id_valid = 1;
		request.file_id = board_id;
		request.total_size_valid = 1;
		request.total_size = left;
		request.seg_id_valid = 1;
		request.data_valid = 1;
		request.bdf_type = type;
		request.bdf_type_valid = 1;
		request.end_valid = 1;
		request.end = left <= QMI_WLANFW_MAX_DATA_SIZE_V01;
		request.data_len = request.end ? left : QMI_WLANFW_MAX_DATA_SIZE_V01;
		memcpy(request.data, data, request.data_len);
		bzero(&reply, sizeof(reply));
		if (!ask(what, QMI_WLANFW_BDF_DOWNLOAD_REQ_V01, QMI_WLANFW_BDF_DOWNLOAD_REQ_MSG_V01_MAX_LEN,
		    qmi_wlanfw_bdf_download_req_msg_v01_ei, &request, qmi_wlanfw_bdf_download_resp_msg_v01_ei, &reply, true)) {
			printf("ALIOTH WIFI: %s: REFUSED AT PIECE %u (%u bytes, %u still to go)\n", what, pieces, request.data_len,
			    left);
			return false;
		}
		left -= request.data_len;
		data += request.data_len;
		request.seg_id++;
	}
	printf("ALIOTH WIFI: %s: ACCEPTED, %u pieces in %llu ms\n", what, pieces, now_ms() - began);
	return true;
}

/* Poll the router until `flag` (or the service went away) or `ms` have passed. */
static bool
wait_for(volatile bool *flag, uint32_t ms)
{
	for (uint32_t i = 0; i < ms && !*flag && !wlfw.gone; i++) {
		int error = redwood_qmi_poll(&handle, 1);
		if (error) {
			printf("ALIOTH WIFI: QRTR poll failed while waiting: %d\n", error);
			return false;
		}
	}
	return *flag;
}

/* The two segments this chip asks for, at stock CNSS's addresses, in ascending order. */
static bool
respond_memory(void)
{
	static struct qmi_wlanfw_respond_mem_req_msg_v01 request;
	struct qmi_wlanfw_respond_mem_resp_msg_v01 reply;
	bool have_ddr = false, have_cal = false;
	bzero(&request, sizeof(request));
	bzero(&reply, sizeof(reply));
	for (uint32_t i = 0; i < wlfw.mem_seg_count; i++) {
		if (wlfw.mem_seg[i].size == 0) {
			printf("ALIOTH WIFI: memory segment %u of 0 bytes\n", i);
			return false;
		}
		if (wlfw.mem_seg[i].type == QMI_WLANFW_MEM_TYPE_DDR_V01 && !have_ddr &&
		    wlfw.mem_seg[i].size <= QCA_CAL_IOVA - QCA_DDR_IOVA) {
			have_ddr = true;
			wlfw.mem_seg[i].address = QCA_DDR_IOVA;
		} else if (wlfw.mem_seg[i].type == QMI_WLANFW_MEM_CAL_V01 && !have_cal &&
		    wlfw.mem_seg[i].size <= QCA_M3_IOVA - QCA_CAL_IOVA) {
			have_cal = true;
			wlfw.mem_seg[i].address = QCA_CAL_IOVA;
		} else {
			printf("ALIOTH WIFI: memory segment %u (type %u, %#x bytes) is not one stock CNSS places\n", i,
			    wlfw.mem_seg[i].type, wlfw.mem_seg[i].size);
			return false;
		}
	}
	for (unsigned pass = 0; pass < 2; pass++) {             /* DDR first: the IOVAs are handed out upwards */
		for (uint32_t i = 0; i < wlfw.mem_seg_count; i++) {
			if ((wlfw.mem_seg[i].type == QMI_WLANFW_MEM_TYPE_DDR_V01) != (pass == 0)) {
				continue;
			}
			if (alioth_wlan_wifi_memory_at(wlfw.mem_seg[i].size, wlfw.mem_seg[i].address) == NULL) {
				return false;
			}
			printf("ALIOTH WIFI:   memory segment %u: %#x bytes at IOVA %#llx\n", i, wlfw.mem_seg[i].size,
			    wlfw.mem_seg[i].address);
		}
	}
	request.mem_seg_len = wlfw.mem_seg_count;
	for (uint32_t i = 0; i < wlfw.mem_seg_count; i++) {
		request.mem_seg[i].addr = wlfw.mem_seg[i].address;
		request.mem_seg[i].size = wlfw.mem_seg[i].size;
		request.mem_seg[i].type = wlfw.mem_seg[i].type;
	}
	return ask("the memory asked for", QMI_WLANFW_RESPOND_MEM_REQ_V01, QMI_WLANFW_RESPOND_MEM_REQ_MSG_V01_MAX_LEN,
	    qmi_wlanfw_respond_mem_req_msg_v01_ei, &request, qmi_wlanfw_respond_mem_resp_msg_v01_ei, &reply, false);
}

static bool
calibrate(void)
{
	struct qmi_wlanfw_wlan_mode_req_msg_v01 mode;
	struct qmi_wlanfw_wlan_mode_resp_msg_v01 mode_reply;
	bzero(&mode, sizeof(mode));
	bzero(&mode_reply, sizeof(mode_reply));
	mode.mode = MODE_COLD_BOOT;
	mode.hw_debug_valid = 1;
	if (!ask("the mode: cold-boot calibration", QMI_WLANFW_WLAN_MODE_REQ_V01, QMI_WLANFW_WLAN_MODE_REQ_MSG_V01_MAX_LEN,
	    qmi_wlanfw_wlan_mode_req_msg_v01_ei, &mode, qmi_wlanfw_wlan_mode_resp_msg_v01_ei, &mode_reply, false)) {
		return false;
	}
	for (unsigned ms = 0; ms < 60000 && !wlfw.cal_done && !wlfw.ready && !wlfw.gone; ms += 10) {
		if (redwood_qmi_poll(&handle, 10)) {
			return false;
		}
	}
	if (!wlfw.cal_done && !wlfw.ready) {
		printf("ALIOTH WIFI: step 10: neither \"calibration done\" nor \"ready\" within 60 s\n");
		return false;
	}
	(void)wait_for(&wlfw.ready, 2000);
	printf("ALIOTH WIFI: step 10: CALIBRATION %s, \"ready\" %s\n", wlfw.cal_done ? "DONE" : "not said",
	    wlfw.ready ? "SAID" : "not said");
	return true;
}

/* Steps 8..`steps`: 0, or an errno. */
int
alioth_wlan_wlfw_run(unsigned steps)
{
	static struct qmi_wlanfw_ind_register_req_msg_v01 indications;
	static struct qmi_wlanfw_ind_register_resp_msg_v01 indications_reply;
	static struct qmi_wlanfw_host_cap_req_msg_v01 host;
	static struct qmi_wlanfw_host_cap_resp_msg_v01 host_reply;
	static struct qmi_wlanfw_cap_req_msg_v01 what;
	static struct qmi_wlanfw_cap_resp_msg_v01 target;
	int error = EIO;
	bzero(&wlfw, sizeof(wlfw));
	wlfw.began = now_ms();
	printf("ALIOTH WIFI: step 8: WLFW (%s's tables): service %#x version %u instance %u\n", WLFW_CHIP,
	    ATH11K_QMI_WLFW_SERVICE_ID_V01, ATH11K_QMI_WLFW_SERVICE_VERS_V01, WLFW_SERVICE_INSTANCE);
	int ret = qmi_handle_init(&handle, ATH11K_QMI_RESP_LEN_MAX, &wlfw_ops, wlfw_handlers);
	if (ret < 0) {
		printf("ALIOTH WIFI: step 8: no QMI handle on the router: %d\n", ret);
		return ENXIO;
	}
	ret = qmi_add_lookup(&handle, ATH11K_QMI_WLFW_SERVICE_ID_V01, ATH11K_QMI_WLFW_SERVICE_VERS_V01, WLFW_SERVICE_INSTANCE);
	if (ret < 0 || !wait_for(&wlfw.server, 3000)) {
		printf("ALIOTH WIFI: step 8: THE WLFW SERVICE IS NOT THERE (lookup %d)\n", ret);
		goto out;
	}

	bzero(&indications, sizeof(indications));
	bzero(&indications_reply, sizeof(indications_reply));
	indications.client_id_valid = 1;
	indications.client_id = QMI_WLANFW_CLIENT_ID;
	indications.request_mem_enable_valid = 1;
	indications.request_mem_enable = 1;
	indications.fw_mem_ready_enable_valid = 1;
	indications.fw_mem_ready_enable = 1;
	indications.fw_ready_enable_valid = 1;
	indications.fw_ready_enable = 1;
	indications.cal_done_enable_valid = 1;
	indications.cal_done_enable = 1;
	indications.fw_init_done_enable_valid = 1;
	indications.fw_init_done_enable = 1;
	if (!ask("the indications we want", QMI_WLANFW_IND_REGISTER_REQ_V01, QMI_WLANFW_IND_REGISTER_REQ_MSG_V01_MAX_LEN,
	    qmi_wlanfw_ind_register_req_msg_v01_ei, &indications, qmi_wlanfw_ind_register_resp_msg_v01_ei, &indications_reply,
	    false)) {
		goto out;
	}
	if (indications_reply.fw_status_valid) {
		printf("ALIOTH WIFI:   the firmware's status word: %#llx\n", (unsigned long long)indications_reply.fw_status);
	}

	bzero(&host, sizeof(host));
	bzero(&host_reply, sizeof(host_reply));
	host.num_clients_valid = 1;
	host.num_clients = 1;
	host.wake_msi_valid = 1;
	host.wake_msi = QCA_WAKE_MSI;
	host.bdf_support_valid = 1;
	host.bdf_support = 1;
	host.m3_support_valid = 1;
	host.m3_support = 1;
	host.m3_cache_support_valid = 1;
	host.m3_cache_support = 1;
	host.cal_done_valid = 1;
	host.cal_done = 0;                      /* freshly allocated calibration memory */
	if (!ask("what the host can do", QMI_WLANFW_HOST_CAP_REQ_V01, QMI_WLANFW_HOST_CAP_REQ_MSG_V01_MAX_LEN,
	    qmi_wlanfw_host_cap_req_msg_v01_ei, &host, qmi_wlanfw_host_cap_resp_msg_v01_ei, &host_reply, false)) {
		goto out;
	}
	for (uint32_t i = 0; i < 5000 && !wlfw.mem_request && !wlfw.mem_ready && !wlfw.gone; i++) {
		if (redwood_qmi_poll(&handle, 1)) {
			goto out;
		}
	}
	if (wlfw.mem_request && !respond_memory()) {
		goto out;
	}
	if (!wlfw.mem_ready && !wait_for(&wlfw.mem_ready, 5000)) {
		printf("ALIOTH WIFI: step 8: NO \"MEMORY READY\" (asked for memory: %s)\n", wlfw.mem_request ? "yes" : "no");
		goto out;
	}

	bzero(&what, sizeof(what));
	bzero(&target, sizeof(target));
	if (!ask("what the chip and the firmware are", QMI_WLANFW_CAP_REQ_V01, QMI_WLANFW_CAP_REQ_MSG_V01_MAX_LEN,
	    qmi_wlanfw_cap_req_msg_v01_ei, &what, qmi_wlanfw_cap_resp_msg_v01_ei, &target, false)) {
		goto out;
	}
	uint32_t board_id = target.board_info_valid ? target.board_info.board_id : 0xff;
	printf("ALIOTH WIFI: step 8: chip %#x family %#x (%s), board id %#x (%s), soc %#x, firmware %#x built \"%s\", build id "
	    "\"%s\"\n", target.chip_info.chip_id, target.chip_info.chip_family, target.chip_info_valid ? "given" : "not given",
	    board_id, target.board_info_valid ? "given" : "not given: 0xff, as ath11k", target.soc_info.soc_id,
	    target.fw_version_info.fw_version, target.fw_version_info.fw_build_timestamp, target.fw_build_id);
	printf("ALIOTH WIFI: step 8: THE WLAN FIRMWARE TALKS (Mi 10's: family 0xb, board 0xff, firmware 0x1014046e)\n");
	if (steps < 9) {
		error = 0;
		goto out;
	}

	(void)send_file("step 9: the regulatory database", "regdb", ATH11K_QMI_BDF_TYPE_REGDB, board_id);
	if (!send_file("step 9: the board's data", "board", ATH11K_QMI_BDF_TYPE_ELF, board_id)) {
		goto out;
	}
	unsigned int m3_size = 0;
	const uint8_t *m3_file = alioth_wlan_wifi_file("m3", &m3_size);
	if (m3_file == NULL || m3_size == 0) {
		printf("ALIOTH WIFI: step 9: no /sweet-wifi \"m3\" in the ADT\n");
		goto out;
	}
	void *m3_memory = alioth_wlan_wifi_memory_at(m3_size, QCA_M3_IOVA);
	if (m3_memory == NULL) {
		goto out;
	}
	memcpy(m3_memory, m3_file, m3_size);
	alioth_wlan_wifi_clean(m3_memory, m3_size);
	struct qmi_wlanfw_m3_info_req_msg_v01 m3;
	struct qmi_wlanfw_m3_info_resp_msg_v01 m3_reply;
	bzero(&m3, sizeof(m3));
	bzero(&m3_reply, sizeof(m3_reply));
	m3.addr = QCA_M3_IOVA;
	m3.size = m3_size;
	printf("ALIOTH WIFI: step 9: M3, %u bytes (the Mi 10's: 0x411bc), at IOVA %#llx\n", m3_size, QCA_M3_IOVA);
	if (!ask("step 9: M3", QMI_WLANFW_M3_INFO_REQ_V01, QMI_WLANFW_M3_INFO_REQ_MSG_V01_MAX_MSG_LEN,
	    qmi_wlanfw_m3_info_req_msg_v01_ei, &m3, qmi_wlanfw_m3_info_resp_msg_v01_ei, &m3_reply, false)) {
		goto out;
	}
	if (!wait_for(&wlfw.init_done, 10000)) {
		printf("ALIOTH WIFI: step 9: NO \"INIT DONE\" within 10 s of the board's data and M3\n");
		goto out;
	}
	printf("ALIOTH WIFI: step 9: THE WLAN FIRMWARE HAS ITS BOARD DATA AND M3: INIT DONE\n");
	if (steps >= 10 && !calibrate()) {
		goto out;
	}
	error = 0;
out:
	printf("ALIOTH WIFI: WLFW ends after %llu ms: memory asked %s, memory ready %s, init done %s, calibration done %s, "
	    "ready %s\n", now_ms() - wlfw.began, wlfw.mem_request ? "yes" : "no", wlfw.mem_ready ? "yes" : "no",
	    wlfw.init_done ? "yes" : "no", wlfw.cal_done ? "yes" : "no", wlfw.ready ? "yes" : "no");
	qmi_handle_release(&handle);
	return error;
}
#endif /* PLATFORM_ALIOTH */
