/*
 * GPL-3.0-or-later. FrontBoard compatibility based on AppSync Unified's
 * FBSSignatureValidationService hook (Copyright 2014-2024 Karen/あけみ,
 * Linus Yang), commit 235aca6cddfbdc9fa87fcb5b2aec2df37ed6d65a.
 * This adaptation is limited to real bundles marked by TrollStore on this
 * experimental kernel. It does not change Apple ID, FairPlay or app contents.
 * License notice: licenses/AppSync-LICENSE; complete text: licenses/GPL-3.0.txt.
 */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#include <fcntl.h>
#include <unistd.h>
static NSUInteger (*original_trust)(id,SEL,id);
static int hooked;

static NSUInteger local_app_trust(id self,SEL cmd,id app)
{
    NSUInteger original=original_trust(self,cmd,app);
    if(original==8) return original;
    id url=nil;
    if([app respondsToSelector:@selector(bundleURL)]) url=[app valueForKey:@"bundleURL"];
    NSString *path=[url isKindOfClass:[NSURL class]] ? [url path] : nil;
    NSString *resolved=path.stringByResolvingSymlinksInPath;
    if([resolved hasPrefix:@"/var/"]) resolved=[@"/private" stringByAppendingString:resolved];
    NSString *container=resolved.stringByDeletingLastPathComponent;
    BOOL candidate=[resolved hasPrefix:@"/private/var/containers/Bundle/Application/"] &&
        [resolved.pathExtension isEqualToString:@"app"];
    BOOL local=candidate &&
        [resolved.pathExtension isEqualToString:@"app"] &&
        [[NSFileManager defaultManager] fileExistsAtPath:[container stringByAppendingPathComponent:@"_TrollStore"]];
    if(candidate && !local) {
        NSString *list=[NSString stringWithContentsOfFile:@"/private/var/preferences/.k40-trusted-apps"
                                                encoding:NSUTF8StringEncoding error:nil];
        local=[[list componentsSeparatedByString:@"\n"] containsObject:resolved];
    }
    char line[512];
    int length=snprintf(line,sizeof(line),"app-trust: class=%s original=%lu local-TrollStore=%d bundle=%s\n",
                       object_getClassName(app),(unsigned long)original,local,path.UTF8String ?: "(unavailable)");
    int fd=open("/dev/console",O_WRONLY);
    if(fd>=0) { write(fd,line,MIN((size_t)MAX(length,0),sizeof(line)-1)); close(fd); }
    return local ? 8 : original;
}
static void install_hook(const struct mach_header *header,intptr_t slide)
{
    (void)header; (void)slide;
    if(hooked) return;
    Class cls=objc_getClass("FBSSignatureValidationService");
    Method m=cls ? class_getInstanceMethod(cls,sel_registerName("trustStateForApplication:")) : NULL;
    if(!m || method_getNumberOfArguments(m)!=3) return;
    const char *type=method_getTypeEncoding(m);
    if(!type || (type[0]!='Q' && type[0]!='L')) return;
    original_trust=(void *)method_setImplementation(m,(IMP)local_app_trust);
    hooked=original_trust!=NULL;
}
__attribute__((constructor)) static void app_trust_init(void)
{
    char args[2048]={0}; size_t size=sizeof(args)-1;
    if(sysctlbyname("kern.bootargs",args,&size,NULL,0) || !strstr(args,"alioth_apps=1")) return;
    install_hook(NULL,0);
    _dyld_register_func_for_add_image(install_hook);
}
