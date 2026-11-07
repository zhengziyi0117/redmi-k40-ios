/* Native CoreText glyph coverage and an actual H.264 decode, independent of Bilibili. */
#import <Foundation/Foundation.h>
#import <CoreText/CoreText.h>
#import <AVFoundation/AVFoundation.h>
#import <VideoToolbox/VideoToolbox.h>
#include <unistd.h>
int main(void)
{
    @autoreleasepool {
        sleep(65);
        CTFontRef font=CTFontCreateWithName(CFSTR("PingFangSC-Regular"),18,NULL);
        UniChar text[]={0x54d4,0x54e9,0x4f60,0x597d}; CGGlyph glyphs[4]={0};
        BOOL covered=CTFontGetGlyphsForCharacters(font,text,glyphs,4);
        CFStringRef name=CTFontCopyPostScriptName(font);
        NSLog(@"native_render_probe: Chinese font=%@ coverage=%d glyphs=%u,%u,%u,%u",name,covered,glyphs[0],glyphs[1],glyphs[2],glyphs[3]);
        CFRelease(name); CFRelease(font);
        CTFontRef system=CTFontCreateUIFontForLanguage(kCTFontUIFontSystem,18,NULL);
        CTFontRef fallback=CTFontCreateForString(system,CFSTR("哔哩你好"),CFRangeMake(0,4));
        name=CTFontCopyPostScriptName(fallback);
        NSLog(@"native_render_probe: Chinese fallback=%@",name);
        CFRelease(name); CFRelease(fallback); CFRelease(system);
        AVURLAsset *asset=[AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:@"/redwood/share/test-h264.mp4"] options:nil];
        NSError *error=nil;
        AVAssetReader *reader=[[AVAssetReader alloc] initWithAsset:asset error:&error];
        NSArray *tracks=[asset tracksWithMediaType:AVMediaTypeVideo];
        if(!reader || !tracks.count) { NSLog(@"native_render_probe: no reader/track error=%@",error); return 1; }
        AVAssetReaderTrackOutput *output=[AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:tracks.firstObject
            outputSettings:@{(id)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)}];
        output.alwaysCopiesSampleData=NO;
        [reader addOutput:output];
        BOOL started=[reader startReading]; unsigned frames=0;
        while(started && frames<24) {
            CMSampleBufferRef sample=[output copyNextSampleBuffer];
            if(!sample) break;
            if(CMSampleBufferGetImageBuffer(sample)) frames++;
            else { CFRelease(sample); break; }
            CFRelease(sample);
        }
        NSLog(@"native_render_probe: H264 start=%d decoded-frames=%u status=%ld error=%@",started,frames,(long)reader.status,reader.error);
        return frames ? 0 : 1;
    }
}
