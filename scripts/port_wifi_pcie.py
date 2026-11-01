#!/usr/bin/env python3
"""Stage the opt-in Alioth PCIe/MHI experiment without replacing the working payload."""
from pathlib import Path
import argparse
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--full', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
reference = root / ('build/wifi-reference-full/bsd/dev' if args.full else 'build/wifi-reference-0126/bsd/dev')
project = root / 'build/k40'
kernel = project / 'xnu-src/xnu-8796.121.2'
files = [f for f in reference.glob('sweet_*') if f.name != 'sweet_net.c'
         and (args.full or f.name not in {'sweet_sta.c', 'sweet_apple80211.c'})]
source_root = root / 'upstream/ios-alioth/src/hackios-xnu-kernel-main'
def destinations(path):
    return [project / 'xnu/files' / path, kernel / path, source_root / 'xnu/files' / path]

if args.full:
    for name in ['qmi', 'qrtr', 'lx', 'say']:
        contents = (reference / f'hakios.{name}.h').read_text()
        contents = contents.replace('hakios_wifi_', 'alioth_wlan_wifi_')
        contents = contents.replace('hakios.', 'redwood_').replace('hakios_', 'redwood_').replace('HAKIOS_', 'REDWOOD_')
        for dest in destinations(f'bsd/sys/redwood_{name}.h'):
            dest.write_text(contents)
    for name in ['qmi', 'lx', 'sta', 'apple80211']:
        for dest in destinations(f'bsd/dev/redwood_{name}.c'):
            contents = dest.read_text()
            if not contents.startswith('#if !defined(PLATFORM_ALIOTH)'):
                dest.write_text('#if !defined(PLATFORM_ALIOTH)\n' + contents + '\n#endif /* original AHB implementation */\n')

for src in files:
    name = src.name.replace('sweet_', 'alioth_wlan_')
    contents = src.read_text().replace('ARM64_BOARD_CONFIG_SWEET', 'PLATFORM_ALIOTH')
    contents = contents.replace('sweet_', 'alioth_wlan_').replace('SWEET_', 'ALIOTH_WLAN_')
    contents = contents.replace('SWEET WIFI', 'ALIOTH WIFI').replace('SWEET RPMH', 'ALIOTH RPMH')
    contents = contents.replace('sweetwifi', 'aliothwifi')
    if args.full:
        contents = contents.replace('hakios_wifi_', 'alioth_wlan_wifi_')
        contents = contents.replace('hakios_', 'redwood_').replace('HAKIOS_', 'REDWOOD_')
        contents = re.sub(r'#include "hakios\.(\w+)\.h"', r'#include <sys/redwood_\1.h>', contents)
        # GenericARMPE already starts MiniIONetworkStack; keep its existing ownership.
        contents = contents.replace('\t\tSweetNetworkStackStart();', '\t\t/* MiniIONetworkStack is already started by GenericARMPE. */')
        contents = contents.replace('\t\tSweetIOKitDaemonStandIn();', '\t\t/* Keep the existing GenericARMPE registry lifecycle. */')
    if src.suffix == '.c':
        mark = '#if defined(PLATFORM_ALIOTH)\n'
        contents = contents.replace(mark, mark + '#include <sys/cdefs.h>\n'
            'extern int alioth_wlan_log(const char *, ...) __printflike(1, 2);\n'
            '#define printf alioth_wlan_log\n', 1)
    if src.name == 'sweet_wifi.c':
        old = '''\tDTEntry node;
\tvoid const *prop = NULL;
\tif (SecureDTLookupEntry(NULL, "/sweet-wifi", &node) != kSuccess ||
\t    SecureDTGetProperty(node, name, &prop, size) != kSuccess) {
\t\treturn NULL;
\t}
\treturn prop;'''
        new = '''\textern const uint8_t *redwood_image_file(const char *, const char *, uint64_t *, int *);
\tconst char *file = NULL;
\tif (!strcmp(name, "amss")) file = "amss20.bin";
\telse if (!strcmp(name, "m3")) file = "m3.bin";
\telse if (!strcmp(name, "board")) file = "board.bin";
\telse if (!strcmp(name, "regdb")) file = "regdb.bin";
\telse if (!strcmp(name, "mac")) file = "wifi-mac";
\telse if (!strcmp(name, "network")) file = "network";
\tuint64_t length = 0;
\tconst uint8_t *bytes = file ? redwood_image_file("fw", file, &length, NULL) : NULL;
\tif (!bytes || length > UINT32_MAX) { *size = 0; return NULL; }
\t*size = (unsigned int)length;
\treturn bytes;'''
        assert old in contents
        contents = contents.replace(old, new, 1)
        contents = contents.replace('static uint32_t wifi_steps', '''extern void loader_log_line(const char *);
extern void loader_log_flush(void);
int alioth_wlan_log(const char *format, ...)
{
    char buffer[256];
    va_list args;
    va_start(args, format);
    int count = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    loader_log_line(buffer);
    loader_log_flush();
    return count;
}

static uint32_t wifi_steps''')
        if not args.full:
            contents = contents.replace('vm_offset_t va = wifi_reg(pa);\n\treturn va ?',
            'static uint32_t last = 0;\n\tif (last != pa) { printf("ALIOTH WIFI: read %#x\\n", pa); last = pa; }\n'
            '\tvm_offset_t va = wifi_reg(pa);\n\treturn va ?', 1)
            contents = contents.replace('vm_offset_t va = wifi_reg(pa);\n\tif (va) {',
            'printf("ALIOTH WIFI: write %#x <- %#x\\n", pa, value);\n'
            '\tvm_offset_t va = wifi_reg(pa);\n\tif (va) {', 1)
    contents = ('/* Alioth adaptation: Apollo PCIe/MHI implementation, APSL 2.0.\n'
        ' * GPIOs, PHY sequence, six supply resources/voltages, MSI and SID\n'
        ' * checked against this K40 stock-device-tree.dtb. Opt-in aliothwifi=N. */\n' + contents)
    for dest in [project / 'xnu/files/bsd/dev' / name, kernel / 'bsd/dev' / name,
                 root / 'upstream/ios-alioth/src/hackios-xnu-kernel-main/xnu/files/bsd/dev' / name]:
        dest.write_text(contents)

conf = kernel / 'bsd/conf/files'
text = re.sub(r'^bsd/dev/(?:alioth_pcie|alioth_wlan_\w+)\.c[^\n]*\n', '', conf.read_text(), flags=re.M)
insert = 'bsd/dev/alioth_pcie.c\t\tstandard\n' + ''.join(
    f'bsd/dev/{s.name.replace("sweet_", "alioth_wlan_")}\t\tstandard\n'
    for s in files if s.suffix == '.c')
needle = 'bsd/dev/redwood_input.c\t\tstandard\n'
assert needle in text
conf.write_text(text.replace(needle, needle + insert, 1))

for dest in [project / 'xnu/files/bsd/dev/redwood_input.c', kernel / 'bsd/dev/redwood_input.c',
             root / 'upstream/ios-alioth/src/hackios-xnu-kernel-main/xnu/files/bsd/dev/redwood_input.c']:
    text = dest.read_text()
    if 'alioth_wlan_wifi_init' not in text:
        text = text.replace('extern void alioth_pcie_init(void);',
            'extern void alioth_wlan_wifi_init(void);\nextern void alioth_pcie_init(void);')
        text = text.replace('\talioth_pcie_init();', '\talioth_pcie_init();\n\talioth_wlan_wifi_init();')
        dest.write_text(text)

# A small standalone patch makes fresh xnu/apply.sh builds include these objects.
base = kernel / 'bsd/conf/files.before-wlan'
base.write_text(conf.read_text().replace(insert, '', 1))
diff = subprocess.run(['diff', '-u', '--label', 'a/bsd/conf/files', str(base),
                       '--label', 'b/bsd/conf/files', str(conf)], capture_output=True, text=True)
base.unlink()
assert diff.returncode == 1
for dest in [project / 'xnu/patches/alioth-wlan-objects.patch',
             root / 'upstream/ios-alioth/src/hackios-xnu-kernel-main/xnu/patches/alioth-wlan-objects.patch']:
    dest.write_text(diff.stdout)
print('Staged Alioth Wi-Fi PCIe/MHI diagnostic; current runtime/boot was not changed.')
