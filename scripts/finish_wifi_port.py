#!/usr/bin/env python3
"""Add Alioth return-to-loader cleanup after port_wifi_pcie.py --full."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
trees = [root / 'build/k40/xnu/files',
         root / 'build/k40/xnu-src/xnu-8796.121.2',
         root / 'upstream/ios-alioth/src/hackios-xnu-kernel-main/xnu/files']
for tree in trees:
    path = tree / 'bsd/dev/alioth_wlan_wifi.c'
    text = path.read_text().replace('char buffer[256];', 'char buffer[1024];')
    marker = '/* Steps 4 on, with the SMMU context installed and the endpoint enumerated. */'
    if 'alioth_wlan_wifi_stop(void)' not in text:
        text = text.replace(marker, '''/* Stop while threads still run; the return path subsequently resets the endpoint. */
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

''' + marker, 1)
        # This is diagnostic only: identify reserved/read-only bits before changing restoration.
        needle = '\t\trestored &= wifi_rd(smmu_ctx(smmu_saved_offsets[i])) == rs.saved[i];'
        text = text.replace(needle, '''		uint32_t now = wifi_rd(smmu_ctx(smmu_saved_offsets[i]));
		if (now != rs.saved[i])
			printf("ALIOTH WIFI: restore context offset %#x: wanted %#x, read %#x\\n",
			    smmu_saved_offsets[i], rs.saved[i], now);
		restored &= now == rs.saved[i];''', 1)
    path.write_text(text)
    path = tree / 'iokit/Kernel/MiniIONetworkStack.cpp'
    text = path.read_text()
    if 'AliothWLANRegistryReady' not in text:
        text = text.replace('#include "MiniDrivers.h"', '#include "MiniDrivers.h"\n#include <libkern/c++/OSKext.h>')
        text += '''
#if defined(PLATFORM_ALIOTH)
/* This macOS-SDK kernel awaits a kext daemon that the iOS userland does not run.
 * Match the normal daemon check-in once BSD starts, after StartIOKitMatching.
 * Otherwise configd waits 300 seconds for the registry to become quiet.
 */
extern "C" void AliothWLANRegistryReady(void);
void
AliothWLANRegistryReady(void)
{
	static bool done;
	if (done || !OSKext::iokitDaemonAvailable()) return;
	done = true;
	IOService::iokitDaemonLaunched();
}
#endif
'''
    path.write_text(text)
    path = tree / 'bsd/dev/alioth_wlan_wifi.c'
    text = path.read_text().replace('extern void SweetIOKitDaemonStandIn(void);',
                                   'extern void AliothWLANRegistryReady(void);')
    text = text.replace('/* Keep the existing GenericARMPE registry lifecycle. */',
                        'AliothWLANRegistryReady();')
    path.write_text(text)
    path = tree / 'osfmk/arm64/loader_return.c'
    text = path.read_text()
    if 'alioth_wlan_wifi_stop' not in text:
        text = text.replace('extern void redwood_wifi_stop(void);', '''#if defined(PLATFORM_ALIOTH)
extern void alioth_wlan_wifi_stop(void);
extern void alioth_wlan_wifi_quiesce(void);
#endif
extern void redwood_wifi_stop(void);''', 1)
        for operation in ['stop', 'quiesce']:
            needle = f'\tredwood_wifi_{operation}();'
            # Keep this platform's PCIe radio behind the same lifecycle as the AHB implementation.
            lines = text.splitlines(keepends=True)
            result = []
            for line in lines:
                result.append(line)
                if needle.strip() in line:
                    result.extend(['#if defined(PLATFORM_ALIOTH)\n',
                                   f'\talioth_wlan_wifi_{operation}();\n', '#endif\n'])
            text = ''.join(result)
    path.write_text(text)
print('Alioth PCIe radio now stops and resets before returning its RAM to the loader.')
