#!/usr/bin/env python3
"""Compile and exercise the actual loader region-discovery code with mocked UEFI."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
loader = (root / 'port/src/hackios-xnu-kernel-main/xnuloader/loader.c').read_text()
function = loader[loader.index('static void\nblock_add_alioth_data'):loader.index('static void\nblock_start', loader.index('static void\nblock_add_alioth_data'))]
prefix = r'''
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <stdarg.h>
#include <zlib.h>
#define BLOCK_MAX_PARTS 160
#define EFI_ERROR(s) ((s) != 0)
typedef struct { int read_only; unsigned media_id; } media_t;
typedef struct efi_block_io {
    media_t *media;
    int (*read_blocks)(struct efi_block_io *, unsigned, uint64_t, unsigned, void *);
} efi_block_io;
struct loader_block_part { char name[36]; uint32_t block_size; uint64_t blocks, start_lba; uint16_t unit; uint8_t lun, flags; uint32_t reserved; };
static struct { unsigned parts; struct loader_block_part part[BLOCK_MAX_PARTS]; } block_shared;
static uint64_t block_offsets[BLOCK_MAX_PARTS];
static efi_block_io *block_bios[BLOCK_MAX_PARTS];
static int block_write_mode, block_write_part;
static uint64_t block_write_first;
static struct { uint64_t fb_base; } platform, *PLAT = &platform;
static uint8_t filesystem[4096], descriptor[4096];
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v,p,4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v,p,8); return v; }
static uint32_t ufs_crc32(const void *p, unsigned n) { return crc32(0,p,n); }
static int str_eq(const char *a,const char *b) { return strcmp(a,b)==0; }
static void print(const char *format,...) { (void)format; }
static int read_blocks(efi_block_io *b,unsigned id,uint64_t lba,unsigned n,void *dst) {
    (void)b; (void)id; assert(n==4096); assert(lba==0 || lba==2048);
    memcpy(dst,lba==0 ? filesystem : descriptor,n); return 0;
}
static media_t media;
static efi_block_io bio = { &media, read_blocks };
static void reset(void) {
    memset(&block_shared,0,sizeof(block_shared)); memset(block_offsets,0,sizeof(block_offsets));
    block_shared.parts=1; strcpy(block_shared.part[0].name,"cust");
    block_shared.part[0].block_size=4096; block_shared.part[0].blocks=262144;
    block_shared.part[0].start_lba=253952; block_bios[0]=&bio;
    block_write_mode=3; block_write_part=-1; block_write_first=42;
    platform.fb_base=0x9c000000; media.read_only=0;
}
'''
suffix = r'''
int main(int argc,char **argv) {
    assert(argc==3); FILE *f=fopen(argv[1],"rb"); assert(f); assert(fread(filesystem,1,4096,f)==4096); fclose(f);
    f=fopen(argv[2],"rb"); assert(f); assert(fread(descriptor,1,4096,f)==4096); fclose(f);
    reset(); block_add_alioth_data(8);
    assert(block_shared.parts==3 && block_write_part==2 && block_write_first==0);
    assert(block_offsets[1]==4096 && block_offsets[2]==131072);
    assert(block_shared.part[1].blocks==126976 && block_shared.part[2].blocks==131072);
    assert(block_shared.part[2].start_lba==253952+131072);
    assert((block_offsets[1]+block_shared.part[1].blocks)==block_offsets[2]);
    assert((block_offsets[2]+block_shared.part[2].blocks)==262144);
    for (unsigned test=0;test<8;test++) {
        reset(); uint8_t saved_fs[4096], saved_header[4096];
        memcpy(saved_fs,filesystem,4096); memcpy(saved_header,descriptor,4096);
        if(test==0) block_write_mode=2;
        if(test==1) platform.fb_base=0;
        if(test==2) media.read_only=1;
        if(test==3) block_shared.part[0].blocks--;
        if(test==4) memset(filesystem+1028,255,4);
        if(test==5) descriptor[56]^=1;
        if(test==6) filesystem[1360]=1;
        if(test==7) block_shared.parts=BLOCK_MAX_PARTS-8;
        unsigned before=block_shared.parts;
        block_add_alioth_data(8);
        assert(block_shared.parts==before && block_write_part==-1);
        memcpy(filesystem,saved_fs,4096); memcpy(descriptor,saved_header,4096);
    }
    puts("Actual loader guard: valid region accepted; 8 unsafe/mismatched cases rejected.");
}
'''
with tempfile.TemporaryDirectory() as temp:
    p = Path(temp)
    (p / 'check.c').write_text(prefix + function + suffix)
    with (root / 'runtime/app-test/cust-app-data.img').open('rb') as f:
        (p / 'ext4').write_bytes(f.read(4096))
        f.seek(8 << 20)
        (p / 'header').write_bytes(f.read(4096))
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(p / 'check.c'), '-lz', '-o', str(p / 'check')], check=True)
    subprocess.run([str(p / 'check'), str(p / 'ext4'), str(p / 'header')], check=True)
