/* Restore app registrations; bootstrap TrollStore only when its bundle is absent. */
#import <Foundation/Foundation.h>
#include <unistd.h>
#include <sys/wait.h>
#include <dlfcn.h>
#include <sys/mount.h>
#include <sys/stat.h>
#import <CoreFoundation/CoreFoundation.h>
extern char **environ;
/* Bilibili downloads executable/resource modules into modmgr rather than
 * Library/Caches. Keep this rebuildable directory in RAM, preserving its
 * current contents during migration. Documents and preferences are untouched. */
static void moveModuleCache(void)
{
    NSFileManager *fm=[NSFileManager defaultManager];
    NSString *base=@"/private/var/mobile/Containers/Data/Application";
    for(NSString *uuid in [fm contentsOfDirectoryAtPath:base error:nil]) {
        NSUUID *identifier=[[NSUUID alloc] initWithUUIDString:uuid];
        if(!identifier) continue;[identifier release];
        NSString *container=[base stringByAppendingPathComponent:uuid];
        NSDictionary *metadata=[NSDictionary dictionaryWithContentsOfFile:[container stringByAppendingPathComponent:@".com.apple.mobile_container_manager.metadata.plist"]];
        if(![metadata[@"MCMMetadataIdentifier"] isEqualToString:@"tv.danmaku.bilianime"]) continue;
        NSString *cache=[container stringByAppendingPathComponent:@"Library/Application Support/modmgr"];
        NSString *scratch=[@"/private/var/tmp" stringByAppendingPathComponent:[@"k40-modmgr-" stringByAppendingString:uuid]];
        NSError *error=nil;
        struct stat st;
        for(NSString *part in @[@"Library",@"Library/Application Support"]) {
            NSString *parent=[container stringByAppendingPathComponent:part];
            if(!lstat(parent.fileSystemRepresentation,&st) && !S_ISDIR(st.st_mode)) return;
        }
        if(!lstat(cache.fileSystemRepresentation,&st) && !S_ISDIR(st.st_mode)) return;
        BOOL existed=[fm fileExistsAtPath:cache];
        if(existed && ![fm copyItemAtPath:cache toPath:scratch error:&error]) {
            NSLog(@"installer_setup: module cache migration copy failed code=%ld",(long)error.code);return;
        }
        if(existed && ![fm removeItemAtPath:cache error:&error]) return;
        [fm createDirectoryAtPath:cache withIntermediateDirectories:YES attributes:nil error:nil];
        uint64_t limit=256ULL<<20;
        BOOL mounted=mount("ramfs",cache.fileSystemRepresentation,0,&limit)==0;
        chown(cache.fileSystemRepresentation,501,501);chmod(cache.fileSystemRepresentation,0755);
        BOOL copied=YES;
        if(existed) for(NSString *name in [fm contentsOfDirectoryAtPath:scratch error:nil]) {
            if(![fm copyItemAtPath:[scratch stringByAppendingPathComponent:name] toPath:[cache stringByAppendingPathComponent:name] error:&error]) {copied=NO;break;}
        }
        if(!copied) { NSLog(@"installer_setup: module cache copy-back incomplete; temporary copy retained code=%ld",(long)error.code);return; }
        if(existed) [fm removeItemAtPath:scratch error:nil];
        if(!mounted) {NSLog(@"installer_setup: module cache mount unavailable; contents restored");return;}
        NSString *relative=[cache substringFromIndex:[@"/private/var/" length]];
        NSString *marker=@"/private/var/preferences/.k40-bili-volatile-path";
        if([relative writeToFile:marker atomically:YES encoding:NSUTF8StringEncoding error:nil]) {
            chown(marker.fileSystemRepresentation,0,0);chmod(marker.fileSystemRepresentation,0644);
        }
        NSLog(@"installer_setup: Bilibili downloadable module cache on separate RAM mount");
        return;
    }
}
static int run(NSString *command, NSString *arg1, NSString *arg2)
{
    const char *helper="/iosapps/Applications/TrollStore.app/trollstorehelper";
    char *args[]={(char *)helper,(char *)command.UTF8String,(char *)arg1.UTF8String,(char *)arg2.UTF8String,NULL};
    pid_t pid=fork(); if(pid==0) { execve(helper,args,environ); _exit(127); }
    int status=0; if(pid<0 || waitpid(pid,&status,0)!=pid || !WIFEXITED(status)) return 1;
    return WEXITSTATUS(status);
}
static NSString *findInstaller(void)
{
    NSFileManager *fm=[NSFileManager defaultManager];
    NSString *base=@"/var/containers/Bundle/Application";
    for(NSString *dir in [fm contentsOfDirectoryAtPath:base error:nil]) {
        NSString *app=[[base stringByAppendingPathComponent:dir] stringByAppendingPathComponent:@"TrollStore.app"];
        NSDictionary *info=[NSDictionary dictionaryWithContentsOfFile:[app stringByAppendingPathComponent:@"Info.plist"]];
        if([info[@"CFBundleIdentifier"] isEqualToString:@"com.opa334.TrollStore"]) return app;
    }
    return nil;
}
int main(void)
{
    @autoreleasepool {
        sleep(35);
        moveModuleCache();
        /* System-class apps outside /Applications are hidden unless this
         * preference is present. Legacy settings migration can replace it. */
        typedef void (*set_pref_fn)(CFStringRef,CFPropertyListRef,CFStringRef,CFStringRef,CFStringRef,CFStringRef);
        typedef Boolean (*sync_pref_fn)(CFStringRef,CFStringRef,CFStringRef,CFStringRef);
        set_pref_fn set_pref=(set_pref_fn)dlsym(RTLD_DEFAULT,"_CFPreferencesSetValueWithContainer");
        sync_pref_fn sync_pref=(sync_pref_fn)dlsym(RTLD_DEFAULT,"_CFPreferencesSynchronizeWithContainer");
        if(set_pref && sync_pref) {
            set_pref(CFSTR("SBShowNonDefaultSystemApps"),kCFBooleanTrue,CFSTR("com.apple.springboard"),CFSTR("mobile"),kCFPreferencesAnyHost,CFSTR("kCFPreferencesNoContainer"));
            sync_pref(CFSTR("com.apple.springboard"),CFSTR("mobile"),kCFPreferencesAnyHost,CFSTR("kCFPreferencesNoContainer"));
        }
        NSString *app=findInstaller();
        if(!app) {
            int code=run(@"install-trollstore",@"/redwood/share/TrollStore.tar",nil);
            NSLog(@"installer_setup: initial install result=%d",code);
            app=findInstaller();
            if(code || !app) return 1;
        }
        if(![[NSFileManager defaultManager] fileExistsAtPath:[app stringByAppendingPathComponent:@"ldid"]])
            if(run(@"install-ldid",@"/redwood/share/ldid-arm64",@"v2.1.5-procursus7-new")) return 1;
        int code=run(@"refresh",nil,nil);
        NSLog(@"installer_setup: registrations restored result=%d",code);
        return code;
    }
}
