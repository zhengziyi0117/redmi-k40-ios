/* Real filesystem checks: sparse growth must obey the payload quota, while
 * 1000 one-byte files must not consume 1000 full RAM pages of that quota. */
#include <sys/mount.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
static uint64_t free_bytes(void) { struct statfs s; return statfs("/private/var",&s) ? 0 : (uint64_t)s.f_bavail*s.f_bsize; }
int main(void)
{
    sleep(45);
    const char *dir="/var/mobile/Media/.k40-quota-check";
    mkdir(dir,0700);
    char path[256];snprintf(path,sizeof(path),"%s/sparse",dir);
    int fd=open(path,O_CREAT|O_RDWR|O_TRUNC,0600);
    if(fd<0) return 1;
    errno=0; int large=ftruncate(fd,(240LL<<20)+1), reason=errno;
    struct stat st;fstat(fd,&st);
    int sparse=ftruncate(fd,1<<20);
    printf("storage_quota_probe: oversized sparse result=%d errno=%d size-after-reject=%lld; small sparse result=%d\n",large,reason,(long long)st.st_size,sparse);
    close(fd);unlink(path);
    uint64_t before=free_bytes(); unsigned made=0;
    for(unsigned i=0;i<1000;i++) {
        snprintf(path,sizeof(path),"%s/%u",dir,i);
        fd=open(path,O_CREAT|O_WRONLY|O_TRUNC,0600);
        if(fd<0) break;
        ssize_t n=write(fd,"x",1);close(fd);
        if(n!=1) break;
        made++;
    }
    uint64_t after=free_bytes();
    for(unsigned i=0;i<1000;i++) { snprintf(path,sizeof(path),"%s/%u",dir,i);unlink(path); }
    rmdir(dir);
    printf("storage_quota_probe: %u tiny files; quota delta=%lld bytes; final-free=%llu\n",made,(long long)(before-after),(unsigned long long)free_bytes());
    return large==-1 && reason==ENOSPC && st.st_size==0 && sparse==0 && made==1000 ? 0 : 1;
}
