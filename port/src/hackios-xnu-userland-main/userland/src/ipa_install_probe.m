/* Download a pinned small native IPA and exercise the official installer helper. */
#import <Foundation/Foundation.h>
#import <CommonCrypto/CommonDigest.h>
#include <unistd.h>
#include <sys/wait.h>
extern char **environ;
int main(void)
{
    @autoreleasepool {
        sleep(80);
        NSString *url = @"https://github.com/pa-jesusf/Ibili/releases/download/release/Ibili-unsigned_010.ipa";
        NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:url]];
        request.timeoutInterval = 90;
        NSError *error = nil;
        NSURLResponse *response = nil;
        NSData *data = [NSURLConnection sendSynchronousRequest:request returningResponse:&response error:&error];
        if (error || data.length != 5106770) {
            NSLog(@"ipa_install_probe: download failed length=%lu error=%@", (unsigned long)data.length, error);
            return 1;
        }
        unsigned char digest[CC_SHA256_DIGEST_LENGTH];
        CC_SHA256(data.bytes, (CC_LONG)data.length, digest);
        NSMutableString *hex = [NSMutableString string];
        for (int i=0;i<CC_SHA256_DIGEST_LENGTH;i++) [hex appendFormat:@"%02x",digest[i]];
        if (![hex isEqualToString:@"7ed63860e55cff9f4fb1c4e8d78399cfef5ca44857493cfce719320085a134e3"]) {
            NSLog(@"ipa_install_probe: checksum mismatch; nothing executed"); return 1;
        }
        NSString *path = @"/var/tmp/k40-ibili-test.ipa";
        if (![data writeToFile:path options:NSDataWritingAtomic error:&error]) {
            NSLog(@"ipa_install_probe: write failed %@",error); return 1;
        }
        NSLog(@"ipa_install_probe: pinned IPA verified; invoking native helper");
        char *args[] = {"/iosapps/Applications/TrollStore.app/trollstorehelper", "install", (char *)path.fileSystemRepresentation, NULL};
        pid_t pid=fork();
        if(pid==0) { execve(args[0],args,environ); _exit(127); }
        int status=0;
        if(pid<0 || waitpid(pid,&status,0)!=pid) return 1;
        NSLog(@"ipa_install_probe: helper wait status=%d",status);
        [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
        return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    }
}
