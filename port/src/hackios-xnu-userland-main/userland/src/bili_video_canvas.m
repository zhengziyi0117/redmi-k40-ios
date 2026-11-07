/* CPU frame presentation for the pinned Bilibili client. This uses IJK's
 * public overlay prefix ABI (bilibili/ijkplayer 30eb9441), without importing
 * its rendering implementation. Standard YUV420 frames become CGImages on a
 * normal sublayer; the app's Apple-specific EAGL drawable is not required.
 * Called only after the client UUID and opt-in boot argument were checked. */
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
struct frame_prefix { int width,height; uint32_t format; int planes; uint16_t *stride; uint8_t **data; };
static IMP original_display;
static void (*original_enqueue)(id,SEL,CMSampleBufferRef);
static BOOL (*original_ready)(id,SEL);
static NSInteger (*original_status)(id,SEL);
static atomic_int pending;
static unsigned frames;
static char canvas_key;
static BOOL vtb_layout_valid;
static BOOL present_pixel_buffer(id view,CVPixelBufferRef buffer);
static unsigned char clip(int x) { return x<0 ? 0 : x>255 ? 255 : (unsigned char)x; }
static void release_pixels(void *info,const void *data,size_t size) { (void)info; (void)size; free((void *)data); }
static BOOL present(id view,struct frame_prefix *f)
{
    if(!f) return YES; /* layout invalidation, not a video frame */
    if(f->format==0x4254565f && vtb_layout_valid) {
        /* This pinned SDK stores its CVPixelBufferRef directly at +48.
         * Its own convertOverlyToImage: implementation verifies this layout;
         * the open-source IJK opaque layout is different and is not assumed. */
        CVPixelBufferRef buffer=NULL;memcpy(&buffer,(const uint8_t *)f+48,sizeof(buffer));
        return present_pixel_buffer(view,buffer);
    }
    BOOL planar=f->format==0x30323449 || f->format==0x32315659;
    BOOL semiplanar=f->format==0x3231564e || f->format==0x3132564e;
    if(f->width<2 || f->height<2 || f->width>4096 || f->height>2304 || !f->stride || !f->data ||
       (!planar && !semiplanar) || f->planes<(planar?3:2) || !f->data[0] || !f->data[1] ||
       f->stride[0]<f->width || f->stride[1]<(planar?(f->width+1)/2:f->width) ||
       (planar && (!f->data[2] || f->stride[2]<(f->width+1)/2))) {
        static unsigned rejected;
        if(rejected++<4) NSLog(@"bili-canvas: unsupported overlay %dx%d format=%08x planes=%d",f->width,f->height,f->format,f->planes);
        return NO;
    }
    if(![view isKindOfClass:[CALayer class]] && ![view isKindOfClass:[UIView class]]) {
        SEL getter=sel_registerName("getRendererView");
        if(![view respondsToSelector:getter]) return NO;
        view=[view performSelector:getter];
        if(![view isKindOfClass:[UIView class]]) return NO;
    }
    int expected=0;
    if(!atomic_compare_exchange_strong(&pending,&expected,1)) return YES;
    size_t stride=(size_t)f->width*4, bytes=stride*f->height;
    uint8_t *rgb=malloc(bytes);
    if(!rgb) { atomic_store(&pending,0); return YES; }
    for(int y=0;y<f->height;y++) {
        const uint8_t *luma=f->data[0]+(size_t)y*f->stride[0];
        const uint8_t *a=f->data[1]+(size_t)(y/2)*f->stride[1];
        const uint8_t *b=planar ? f->data[2]+(size_t)(y/2)*f->stride[2] : NULL;
        uint8_t *out=rgb+(size_t)y*stride;
        for(int x=0;x<f->width;x++) {
            int u,v;
            if(planar) { u=a[x/2];v=b[x/2]; if(f->format==0x32315659) { int swap=u;u=v;v=swap; } }
            else { u=a[(x/2)*2];v=a[(x/2)*2+1]; if(f->format==0x3132564e) { int swap=u;u=v;v=swap; } }
            int yy=((int)luma[x]-16)*298; u-=128;v-=128;
            out[x*4]=clip((yy+409*v+128)>>8);
            out[x*4+1]=clip((yy-100*u-208*v+128)>>8);
            out[x*4+2]=clip((yy+516*u+128)>>8);out[x*4+3]=255;
        }
    }
    CGDataProviderRef provider=CGDataProviderCreateWithData(NULL,rgb,bytes,release_pixels);
    CGColorSpaceRef color=CGColorSpaceCreateDeviceRGB();
    CGImageRef image=CGImageCreate(f->width,f->height,8,32,stride,color,
        kCGBitmapByteOrder32Big|kCGImageAlphaPremultipliedLast,provider,NULL,true,kCGRenderingIntentDefault);
    CGColorSpaceRelease(color);CGDataProviderRelease(provider);
    if(!image) { atomic_store(&pending,0); return YES; }
    unsigned number=++frames;
    if(number<=3 || number%120==0) NSLog(@"bili-canvas: frame=%u size=%dx%d format=%08x",number,f->width,f->height,f->format);
    [view retain];
    dispatch_async(dispatch_get_main_queue(),^{ @autoreleasepool {
        CALayer *parent=[view isKindOfClass:[CALayer class]] ? (CALayer *)view : [(UIView *)view layer];
        CALayer *layer=objc_getAssociatedObject(view,&canvas_key);
        if(!layer) { layer=[CALayer layer]; layer.contentsGravity=kCAGravityResizeAspect;
            [parent addSublayer:layer];objc_setAssociatedObject(view,&canvas_key,layer,OBJC_ASSOCIATION_RETAIN_NONATOMIC); }
        [CATransaction begin];[CATransaction setDisableActions:YES];
        layer.frame=parent.bounds;layer.contents=(id)image;[CATransaction commit];
        CGImageRelease(image);[view release];atomic_store(&pending,0);
    }});
    return YES;
}
static BOOL present_pixel_buffer(id view,CVPixelBufferRef buffer)
{
    if(!buffer) return NO;
    CVPixelBufferRetain(buffer);
    static unsigned seen;
    if(seen++<3) NSLog(@"bili-canvas: sample pixel-format=%08x planes=%lu",buffer ? CVPixelBufferGetPixelFormatType(buffer) : 0,buffer ? (unsigned long)CVPixelBufferGetPlaneCount(buffer) : 0);
    if(CVPixelBufferGetPixelFormatType(buffer)!=kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
       CVPixelBufferGetPlaneCount(buffer)!=2 || CVPixelBufferLockBaseAddress(buffer,kCVPixelBufferLock_ReadOnly)) {
        CVPixelBufferRelease(buffer);
        return NO;
    }
    size_t w=CVPixelBufferGetWidth(buffer),h=CVPixelBufferGetHeight(buffer);
    size_t ystride=CVPixelBufferGetBytesPerRowOfPlane(buffer,0),uvstride=CVPixelBufferGetBytesPerRowOfPlane(buffer,1);
    uint16_t strides[2]={(uint16_t)ystride,(uint16_t)uvstride};
    uint8_t *planes[2]={CVPixelBufferGetBaseAddressOfPlane(buffer,0),CVPixelBufferGetBaseAddressOfPlane(buffer,1)};
    static unsigned checked;
    if(planes[0] && planes[1] && w && h && ystride>=w && uvstride>=w && checked++<3) {
        unsigned lo=255,hi=0,ulo=255,uhi=0;
        for(size_t y=0;y<h;y+=16) for(size_t x=0;x<w;x+=16) { unsigned v=planes[0][y*ystride+x];if(v<lo)lo=v;if(v>hi)hi=v; }
        for(size_t y=0;y<(h+1)/2;y+=16) for(size_t x=0;x<w;x+=16) { unsigned v=planes[1][y*uvstride+x];if(v<ulo)ulo=v;if(v>uhi)uhi=v; }
        NSLog(@"bili-canvas: decoded plane sample ranges Y=%u..%u UV=%u..%u rows=%lu,%lu",lo,hi,ulo,uhi,(unsigned long)ystride,(unsigned long)uvstride);
    }
    struct frame_prefix f={(int)w,(int)h,0x3231564e,2,strides,planes};
    BOOL accepted=ystride<=UINT16_MAX && uvstride<=UINT16_MAX && present(view,&f);
    CVPixelBufferUnlockBaseAddress(buffer,kCVPixelBufferLock_ReadOnly);
    CVPixelBufferRelease(buffer);
    return accepted;
}
static void enqueue(id self,SEL cmd,CMSampleBufferRef sample)
{
    CVImageBufferRef buffer=sample ? CMSampleBufferGetImageBuffer(sample) : NULL;
    if(!present_pixel_buffer(self,buffer)) original_enqueue(self,cmd,sample);
}
static BOOL ready(id self,SEL cmd)
{
    return objc_getAssociatedObject(self,&canvas_key) ? YES : original_ready(self,cmd);
}
static NSInteger status(id self,SEL cmd)
{
    return objc_getAssociatedObject(self,&canvas_key) ? AVQueuedSampleBufferRenderingStatusRendering : original_status(self,cmd);
}
void alioth_bili_sample_canvas_init(void)
{
    Class c=objc_getClass("AVSampleBufferDisplayLayer");
    Method m=c ? class_getInstanceMethod(c,sel_registerName("enqueueSampleBuffer:")) : NULL;
    if(!m || method_getNumberOfArguments(m)!=3) return;
    original_enqueue=(void *)method_setImplementation(m,(IMP)enqueue);
    m=class_getInstanceMethod(c,sel_registerName("isReadyForMoreMediaData"));
    if(m) original_ready=(void *)method_setImplementation(m,(IMP)ready);
    m=class_getInstanceMethod(c,sel_registerName("status"));
    if(m) original_status=(void *)method_setImplementation(m,(IMP)status);
    NSLog(@"bili-canvas: public sample-buffer CPU presenter installed");
}
static void display_void(id self,SEL cmd,struct frame_prefix *f)
{
    if(!present(self,f)) ((void (*)(id,SEL,void *))original_display)(self,cmd,f);
}
static BOOL display_bool(id self,SEL cmd,struct frame_prefix *f)
{
    return present(self,f) || ((BOOL (*)(id,SEL,void *))original_display)(self,cmd,f);
}
void alioth_bili_canvas_init(void)
{
    for(uint32_t i=0;i<_dyld_image_count();i++) {
        const struct mach_header_64 *header=(const void *)_dyld_get_image_header(i);
        if(header && header->magic==MH_MAGIC_64 && header->filetype==MH_EXECUTE) {
            /* ldr x2,[x2,#0x30] in this exact UUID's converter, immediately
             * before its imageWithCVPixelBuffer: call. */
            vtb_layout_valid=*(const uint32_t *)((const uint8_t *)header+0x14b3ebbc)==0xf9401842;
            break;
        }
    }
    NSLog(@"bili-canvas: pinned VideoToolbox pixel-buffer layout=%d",vtb_layout_valid);
    Class c=objc_getClass("IJKVideoRenderer");
    Method m=c ? class_getInstanceMethod(c,sel_registerName("display_pixels:")) : NULL;
    if(!m) { c=objc_getClass("IJKSDLGLView");m=c ? class_getInstanceMethod(c,sel_registerName("display:")) : NULL; }
    if(!m || method_getNumberOfArguments(m)!=3) { NSLog(@"bili-canvas: no compatible IJK view");return; }
    const char *type=method_getTypeEncoding(m);char *arg=method_copyArgumentType(m,2);
    BOOL pointer=arg && arg[0]=='^';free(arg);
    if(!pointer || !type || (type[0]!='v' && type[0]!='B' && type[0]!='c')) return;
    original_display=method_setImplementation(m,type[0]=='v'?(IMP)display_void:(IMP)display_bool);
    NSLog(@"bili-canvas: native IJK frame presenter installed on %s encoding=%s",class_getName(c),type);
}
