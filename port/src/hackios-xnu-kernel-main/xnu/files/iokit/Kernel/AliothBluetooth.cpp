/* From the Mi 10 port's devices/xiaomi-mi-10/overlays/xnu/files/iokit/Kernel/hakios.bluetooth.cpp
 * (github.com/JopangJovan/hackios-xnu-kernel-umi; MIT), for apollo's Bluetooth (xnu 0136); as with
 * SweetWLAN.cpp, its warnings are not looked at. */
#pragma clang diagnostic ignored "-Weverything"
/*
 * AliothBluetooth: the registry entry by which iOS finds the Bluetooth controller.
 *
 * bluetoothd looks for a service named "bluetooth" and reads its address from the property
 * local-mac-address; it starts only if that is there and not zero. The HCI traffic itself does
 * not go through IOKit: it goes through the kernel control "com.alioth.bluetooth" of
 * bsd/dev/alioth_bt.c (bluetoothd's job environment names it). So this entry only says who the
 * controller is, as measured once its firmware runs: the address, the commands it supports
 * (AliothBluetoothSupportedCommands, 64 bytes as Read Local Supported Commands has them) and
 * whether the H4 endpoint works (AliothBluetoothReady).
 *
 * Published by alioth_bt.c (aliothbt=6), once.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <sys/errno.h>

extern "C" int AliothBluetoothPublish(const uint8_t address[6], const uint8_t commands[64]);
extern "C" void AliothBluetoothSetReady(bool ready);

static IOService * bluetooth_service;
static const uint8_t transport_speed[4] = { 0x00, 0xc2, 0x01, 0x00 };  /* 115200 */

void
AliothBluetoothSetReady(bool ready)
{
	if (bluetooth_service != NULL) {
		bluetooth_service->setProperty("AliothBluetoothReady", ready);
	}
}

int
AliothBluetoothPublish(const uint8_t address[6], const uint8_t commands[64])
{
	IOService * platform = IOService::getPlatform();
	IOService * service;
	OSData * data;

	if (bluetooth_service != NULL) {
		return EEXIST;
	}
	service = new IOService;
	if (platform == NULL || service == NULL || !service->init()) {
		OSSafeReleaseNULL(service);
		return ENOMEM;
	}
	service->setName("bluetooth");
	if ((data = OSData::withBytes(address, 6)) == NULL) {
		service->release();
		return ENOMEM;
	}
	service->setProperty("local-mac-address", data);
	data->release();
	if ((data = OSData::withBytes(commands, 64)) == NULL) {
		service->release();
		return ENOMEM;
	}
	service->setProperty("AliothBluetoothSupportedCommands", data);
	data->release();
	/*
	 * Bytes, as every property in Apple's device tree is: bluetoothd reads transport-speed with
	 * CFDataGetBytes (4 bytes, little-endian), and a number here made it throw
	 * -[__NSCFNumber getBytes:range:] (apollo boot 74).
	 */
	if ((data = OSData::withBytes(transport_speed, sizeof(transport_speed))) == NULL) {
		service->release();
		return ENOMEM;
	}
	service->setProperty("transport-speed", data);
	data->release();
	service->setProperty("compatible", "qca,qca6390");
	service->setProperty("AliothBluetoothReady", false);
	if (!service->attach(platform)) {
		service->release();
		return EIO;
	}
	if (!service->start(platform)) {
		service->detach(platform);
		service->release();
		return EIO;
	}
	service->registerService();
	bluetooth_service = service;
	service->release();             /* the registry keeps it */
	IOLog("AliothBluetooth: the registry entry \"bluetooth\" is published\n");
	return 0;
}
