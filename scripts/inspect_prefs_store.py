#!/usr/bin/env python3
"""Check a local 32 MiB Alioth settings store without printing any credentials."""
import argparse
from pathlib import Path
import struct
import zlib
import mmap
from collections import Counter

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('archive', type=Path)
p.add_argument('--slot-mib', type=int, choices=(16,256), default=16)
p.add_argument('--offset-mib', type=int, choices=(0,512), default=0)
p.add_argument('--usage-depth',type=int,choices=range(1,10),help='print aggregate directory sizes from the newest validated slot; never print contents')
a = p.parse_args()
source=a.archive.open('rb')
data=mmap.mmap(source.fileno(),0,access=mmap.ACCESS_READ)
slot_bytes = a.slot_mib << 20
offset = a.offset_mib << 20
if len(data) != offset + 2 * slot_bytes:
    raise SystemExit('Expected two archive slots after the specified offset')
valid = 0
newest=None
for slot in range(2):
    start = offset + slot * slot_bytes
    header = bytearray(data[start:start + 56])
    magic, version, number, generation, size, entries, timestamp, crc, head_crc = struct.unpack('<8sIIQQQQII', header)
    if not any(header):
        print(f'slot {slot}: empty')
        continue
    struct.pack_into('<I', header, 52, 0)
    if (magic != b'RDWVAR\0\0' or version != 1 or number != slot or size > slot_bytes - 4096
            or zlib.crc32(header) != head_crc
            or zlib.crc32(data[start + 4096:start + 4096 + size]) != crc):
        print(f'slot {slot}: INVALID')
        continue
    valid += 1
    print(f'slot {slot}: valid generation {generation}, {entries} entries, {size} bytes')
    if newest is None or generation>newest[0]:
        newest=(generation,start+4096,size,entries)
if not valid:
    raise SystemExit('No valid settings archive')
if a.usage_depth:
    _,at,size,count=newest
    end=at+size
    groups=Counter()
    for _ in range(count):
        if at+48>end: raise SystemExit('Truncated entry')
        kind,mode,uid,gid,flags,pathlen,nxattr,nsec,mtime,length=struct.unpack_from('<8IqQ',data,at)
        at+=48
        if pathlen>=1024 or nxattr>4096 or at+pathlen>end: raise SystemExit('Invalid entry')
        path=data[at:at+pathlen].decode(errors='replace');at+=pathlen
        for _ in range(nxattr):
            if at+8>end: raise SystemExit('Truncated attribute')
            namesize,valuesize=struct.unpack_from('<II',data,at)
            at+=8+namesize+valuesize
            if at>end: raise SystemExit('Truncated attribute body')
        if length>end-at: raise SystemExit('Truncated body')
        if kind==2:
            groups['/'.join(path.split('/')[:a.usage_depth])]+=length
        at+=length
        at=(at+7)&~7
    if at!=end: raise SystemExit('Archive size mismatch')
    for path,size in groups.most_common(25):
        print(f'{size:12d}  {path}')
