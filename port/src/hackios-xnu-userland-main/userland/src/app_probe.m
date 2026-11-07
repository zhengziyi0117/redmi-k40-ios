/* Device-local diagnostic: use the same application launch service as iOS. */
#import <Foundation/Foundation.h>
#include <unistd.h>
#include <stdlib.h>
#include <dlfcn.h>
@interface LSApplicationWorkspace : NSObject
+ (instancetype)defaultWorkspace;
- (BOOL)openApplicationWithBundleID:(NSString *)identifier;
@end
@interface FBSSystemService : NSObject
+ (instancetype)sharedService;
- (void)openApplication:(NSString *)identifier options:(NSDictionary *)options withResult:(void (^)(NSError *))result;
@end
@interface LSApplicationProxy : NSObject
+ (instancetype)applicationProxyForIdentifier:(NSString *)identifier;
@end

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc != 3) return 2;
        sleep((unsigned)atoi(argv[1]));
        NSDictionary *space = [[NSFileManager defaultManager] attributesOfFileSystemForPath:@"/var" error:nil];
        NSLog(@"app_probe: data volume bytes=%@ free=%@",space[NSFileSystemSize],space[NSFileSystemFreeSize]);
        NSString *identifier = [NSString stringWithUTF8String:argv[2]];
        NSLog(@"app_probe: proxy=%@",[LSApplicationProxy applicationProxyForIdentifier:identifier]);
        id proxy=[LSApplicationProxy applicationProxyForIdentifier:identifier];
        for(NSString *key in @[@"isInstalled",@"isPlaceholder",@"isRestricted",@"applicationType",@"appState",@"isLaunchProhibited"])
            if([proxy respondsToSelector:NSSelectorFromString(key)]) NSLog(@"app_probe: %@=%@",key,[proxy valueForKey:key]);
        BOOL ok = [[LSApplicationWorkspace defaultWorkspace] openApplicationWithBundleID:identifier];
        NSLog(@"app_probe: launch %@: %@", identifier, ok ? @"accepted" : @"refused");
        if(!ok) {
            dlopen("/System/Library/PrivateFrameworks/FrontBoardServices.framework/FrontBoardServices",RTLD_NOW);
            __block BOOL done=NO;
            if(NSClassFromString(@"FBSSystemService")) {
                [[NSClassFromString(@"FBSSystemService") sharedService] openApplication:identifier options:@{} withResult:^(NSError *error) {
                    NSLog(@"app_probe: FrontBoard launch result=%@",error); done=YES;
                }];
                NSDate *deadline=[NSDate dateWithTimeIntervalSinceNow:10];
                while(!done && deadline.timeIntervalSinceNow>0)
                    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
            }
        }
        return ok ? 0 : 1;
    }
}
