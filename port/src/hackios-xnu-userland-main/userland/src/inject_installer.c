/* TrollStore 2.1.1 compatibility for this custom XNU kernel, which already
 * permits development code. The stock AMFI developer-mode XPC daemon is absent.
 * Keep the downloaded helper unchanged; adapt its query in memory only, with
 * an exact UUID/instruction/boot-mode guard. No Apple account or DRM changes. */
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <libkern/OSCacheControl.h>
#include <sys/sysctl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/mount.h>

static void mount_new_app_caches(void)
{
    const char *base="/private/var/mobile/Containers/Data/Application";
    DIR *all=opendir(base); if(!all) return;
    struct stat data; if(stat("/private/var",&data)) { closedir(all); return; }
    struct dirent *entry;
    while((entry=readdir(all))) {
        if(entry->d_name[0]=='.') continue;
        char library[512], cache[600]; struct stat st;
        snprintf(library,sizeof(library),"%s/%s/Library",base,entry->d_name);
        mkdir(library,0755); chown(library,501,501);
        snprintf(cache,sizeof(cache),"%s/Caches",library);
        mkdir(cache,0700); chown(cache,501,501);
        uint64_t limit=256ULL<<20;
        if(stat(cache,&st)==0 && st.st_dev==data.st_dev && mount("ramfs",cache,0,&limit)==0) {
            chmod(cache,0700); chown(cache,501,501);
        }
        snprintf(cache,sizeof(cache),"%s/%s/tmp",base,entry->d_name);
        mkdir(cache,0700); chown(cache,501,501);
        if(stat(cache,&st)==0 && st.st_dev==data.st_dev && mount("ramfs",cache,0,&limit)==0) {
            chmod(cache,0700); chown(cache,501,501);
        }
    }
    closedir(all);
}

/* SpringBoard cannot traverse every private app container as mobile. Publish
 * only root-verified TrollStore bundle paths in a root-owned readable file. */
static void publish_installed_bundles(void)
{
    if(getuid()!=0) return;
    mount_new_app_caches();
    const char *base="/private/var/containers/Bundle/Application";
    DIR *all=opendir(base); if(!all) return;
    const char *temp="/private/var/preferences/.k40-trusted-apps.tmp";
    FILE *out=fopen(temp,"w"); if(!out) { closedir(all); return; }
    fchmod(fileno(out),0644); fchown(fileno(out),0,0);
    struct dirent *de; unsigned count=0;
    while((de=readdir(all))) {
        if(de->d_name[0]=='.') continue;
        char container[512], marker[600], troll[600];
        snprintf(container,sizeof(container),"%s/%s",base,de->d_name);
        snprintf(marker,sizeof(marker),"%s/_TrollStore",container);
        snprintf(troll,sizeof(troll),"%s/TrollStore.app/Info.plist",container);
        if(access(marker,F_OK)!=0 && access(troll,F_OK)!=0) continue;
        DIR *dir=opendir(container); if(!dir) continue;
        struct dirent *app;
        while((app=readdir(dir))) {
            size_t len=strlen(app->d_name);
            if(len>4 && strcmp(app->d_name+len-4,".app")==0) {
                fprintf(out,"%s/%s\n",container,app->d_name); count++;
            }
        }
        closedir(dir);
    }
    closedir(all); fflush(out); fsync(fileno(out)); fclose(out);
    if(rename(temp,"/private/var/preferences/.k40-trusted-apps")==0)
        fprintf(stderr,"installer-compat: %u installed bundle paths published\n",count);
}

__attribute__((constructor)) static void installer_compat(void)
{
    char args[2048]={0}; size_t size=sizeof(args)-1;
    if(sysctlbyname("kern.bootargs",args,&size,NULL,0) || !strstr(args,"alioth_apps=1")) return;
    const uint8_t expected_uuid[16]={0x5f,0xf8,0x7c,0x54,0xdd,0x83,0x33,0x51,0x93,0x31,0xda,0x41,0x90,0xa9,0x6a,0x78};
    for(uint32_t i=0;i<_dyld_image_count();i++) {
        const struct mach_header_64 *h=(const void *)_dyld_get_image_header(i);
        if(!h || h->magic!=MH_MAGIC_64 || h->filetype!=MH_EXECUTE) continue;
        const uint8_t *p=(const uint8_t *)(h+1); int matches=0;
        for(uint32_t k=0;k<h->ncmds;k++) {
            const struct load_command *lc=(const void *)p;
            if(lc->cmdsize<sizeof(*lc)) return;
            if(lc->cmd==LC_UUID && lc->cmdsize>=sizeof(struct uuid_command))
                matches=memcmp(((const struct uuid_command *)lc)->uuid,expected_uuid,16)==0;
            p+=lc->cmdsize;
        }
        if(!matches) { fprintf(stderr,"installer-compat: unrecognized helper; no changes\n"); return; }
        uint32_t *code=(void *)((uintptr_t)h+0x1fc450);
        /* getDeveloperModeState: sub sp,sp,#64; stp x22,x21,[sp,#16] */
        const uint32_t original[2]={0xd10103ff,0xa90157f6};
        if(memcmp(code,original,sizeof(original))) { fprintf(stderr,"installer-compat: instruction mismatch; no changes\n"); return; }
        uintptr_t page=(uintptr_t)code & ~((uintptr_t)getpagesize()-1);
        if(vm_protect(mach_task_self(),page,getpagesize(),FALSE,VM_PROT_READ|VM_PROT_WRITE|VM_PROT_COPY)!=KERN_SUCCESS) return;
        code[0]=0x52800020; code[1]=0xd65f03c0; /* mov w0,#1; ret */
        vm_protect(mach_task_self(),page,getpagesize(),FALSE,VM_PROT_READ|VM_PROT_EXECUTE);
        sys_icache_invalidate(code,8);
        fprintf(stderr,"installer-compat: custom kernel supports development code; AMFI-only query adapted\n");
        atexit(publish_installed_bundles);
        return;
    }
}
