/* Read-only diagnostics: account for real file sizes on /var's own RAM mount.
 * Other mounts (volatile caches, original system) are excluded. No contents
 * or user filenames are logged, just aggregate top-level directory totals. */
#include <sys/stat.h>
#include <sys/mount.h>
#include <fts.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
struct bucket { char path[256]; uint64_t bytes,files; };
int main(void)
{
    sleep(70);
    char *roots[]={"/private/var",NULL};
    FTS *walk=fts_open(roots,FTS_PHYSICAL|FTS_XDEV,NULL);
    static struct bucket buckets[4096]; unsigned used=0; uint64_t total=0;
    if(!walk) return 1;
    FTSENT *e;
    while((e=fts_read(walk))) {
        if(e->fts_info!=FTS_F || e->fts_statp->st_nlink>1) continue;
        total+=e->fts_statp->st_size;
        const char *name=e->fts_path+strlen(roots[0]); if(*name=='/') name++;
        char group[256]; unsigned slash=0,at=0;
        while(name[at] && at<sizeof(group)-1) {
            if(name[at]=='/' && ++slash==8) break;
            group[at]=name[at]; at++;
        }
        group[at]=0;
        unsigned i=0;while(i<used && strcmp(buckets[i].path,group)) i++;
        if(i==used) { if(used==4096) continue;strcpy(buckets[used++].path,group); }
        buckets[i].bytes+=e->fts_statp->st_size;buckets[i].files++;
    }
    fts_close(walk);
    struct statfs fs;statfs(roots[0],&fs);
    printf("storage_usage_probe: payload=%llu available=%llu\n",(unsigned long long)total,(unsigned long long)fs.f_bavail*fs.f_bsize);
    for(unsigned n=0;n<16;n++) {
        unsigned top=0;for(unsigned i=1;i<used;i++) if(buckets[i].bytes>buckets[top].bytes) top=i;
        if(!buckets[top].bytes) break;
        printf("storage_usage_probe: %s bytes=%llu files=%llu\n",buckets[top].path,(unsigned long long)buckets[top].bytes,(unsigned long long)buckets[top].files);
        buckets[top].bytes=0;
    }
    return 0;
}
