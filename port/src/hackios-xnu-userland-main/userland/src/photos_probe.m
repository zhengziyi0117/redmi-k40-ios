/* Exercise the real PhotoKit library; no replacement Photos UI. */
#import <Foundation/Foundation.h>
#import <Photos/Photos.h>
#include <unistd.h>
int main(int argc, char **argv)
{
    @autoreleasepool {
        sleep(50);
        PHFetchResult *before = [PHAsset fetchAssetsWithOptions:nil];
        NSLog(@"photos_probe: authorization=%ld assets-before=%lu", (long)[PHPhotoLibrary authorizationStatus], (unsigned long)before.count);
        if (before.count == 0 && argc == 2) {
            NSError *error = nil;
            NSURL *source = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]];
            BOOL ok = [[PHPhotoLibrary sharedPhotoLibrary] performChangesAndWait:^{
                [PHAssetChangeRequest creationRequestForAssetFromImageAtFileURL:source];
            } error:&error];
            NSLog(@"photos_probe: import test image: %d error=%@", ok, error);
        }
        NSLog(@"photos_probe: assets-after=%lu", (unsigned long)[PHAsset fetchAssetsWithOptions:nil].count);
    }
    return 0;
}
