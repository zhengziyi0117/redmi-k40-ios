/* Experimental software-decoder selection for the pinned native Bilibili test
 * client. IJK's documented player options are used; no account/DRM changes.
 * The original method handles every option except hardware VideoToolbox flags. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <CoreText/CoreText.h>
#import <UIKit/UIKit.h>
#import <objc/message.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <sys/sysctl.h>
#include <fcntl.h>
#include <unistd.h>
extern void alioth_bili_canvas_init(void);
extern void alioth_bili_sample_canvas_init(void);
static void (*old_int)(id,SEL,int64_t,NSString *,int);
static void (*old_value)(id,SEL,NSString *,NSString *,int);
static void (*old_player)(id,SEL,int64_t,NSString *);
static id (*old_font)(id,SEL,NSString *,CGFloat);
static id (*old_descriptor)(id,SEL,NSString *,CGFloat);
static id (*old_descriptor_font)(id,SEL,UIFontDescriptor *,CGFloat);
static void (*old_prepare)(id,SEL);
static BOOL (*old_active)(id,SEL,BOOL,NSError **);
static BOOL (*old_active_options)(id,SEL,BOOL,NSUInteger,NSError **);
static BOOL fonts_enabled;
static CFStringRef ct_readable_font(CFStringRef name)
{
    if(!fonts_enabled || !name) return name;
    if(CFEqual(name,CFSTR("bilibilifont-Regular")) || CFEqual(name,CFSTR("bilibilicharacters-Regular"))) return CFSTR("PingFangSC-Regular");
    if(CFEqual(name,CFSTR("bilibilifont-Medium")) || CFEqual(name,CFSTR("bilibilicharacters-Medium"))) return CFSTR("PingFangSC-Semibold");
    return name;
}
static CTFontRef create_font(CFStringRef name,CGFloat size,const CGAffineTransform *matrix)
{
    return CTFontCreateWithName(ct_readable_font(name),size,matrix);
}
static CTFontRef create_font_options(CFStringRef name,CGFloat size,const CGAffineTransform *matrix,CTFontOptions options)
{
    return CTFontCreateWithNameAndOptions(ct_readable_font(name),size,matrix,options);
}
__attribute__((used,section("__DATA,__interpose"))) static const struct { const void *replacement,*original; } font_interposes[]={
    {create_font,CTFontCreateWithName},{create_font_options,CTFontCreateWithNameAndOptions}
};
static void prepare(id self,SEL cmd) {
    if(old_player) { old_player(self,sel_registerName("setPlayerOptionIntValue:forKey:"),0,@"videotoolbox");NSLog(@"bili-compat: final player videotoolbox=0 before prepare"); }
    NSLog(@"bili-compat: prepareToPlay begin");old_prepare(self,cmd);NSLog(@"bili-compat: prepareToPlay returned");
}
static BOOL active(id self,SEL cmd,BOOL enabled,NSError **error) {
    NSLog(@"bili-compat: audio setActive=%d begin",enabled);
    BOOL result=old_active(self,cmd,enabled,error);
    NSLog(@"bili-compat: audio setActive result=%d",result);return result;
}
static BOOL active_options(id self,SEL cmd,BOOL enabled,NSUInteger options,NSError **error) {
    NSLog(@"bili-compat: audio setActive/options=%d begin",enabled);
    BOOL result=old_active_options(self,cmd,enabled,options,error);
    NSLog(@"bili-compat: audio setActive/options result=%d",result);return result;
}
static NSString *readable_font(NSString *name)
{
    if(!name) return nil;
    if([@[@"bilibilifont-Regular",@"bilibilicharacters-Regular"] containsObject:name]) return @"PingFangSC-Regular";
    if([@[@"bilibilifont-Medium",@"bilibilicharacters-Medium"] containsObject:name]) return @"PingFangSC-Semibold";
    return name;
}
static id font_with_name(id self,SEL cmd,NSString *name,CGFloat size)
{
    return old_font(self,cmd,readable_font(name),size);
}
static id descriptor_with_name(id self,SEL cmd,NSString *name,CGFloat size)
{
    return old_descriptor(self,cmd,readable_font(name),size);
}
static id font_with_descriptor(id self,SEL cmd,UIFontDescriptor *descriptor,CGFloat size)
{
    NSString *name=descriptor.postscriptName, *replacement=readable_font(name);
    if(name && ![name isEqualToString:replacement]) {
        descriptor=[descriptor fontDescriptorByAddingAttributes:@{UIFontDescriptorNameAttribute:replacement}];
    }
    return old_descriptor_font(self,cmd,descriptor,size);
}
static BOOL hardware_option(NSString *key)
{
    return [@[@"videotoolbox",@"videotoolbox-h264",@"videotoolbox-hevc",@"videotoolbox-av1"] containsObject:key];
}
static void set_int(id self,SEL cmd,int64_t value,NSString *key,int category)
{
    if(hardware_option(key)) { value=0; NSLog(@"bili-compat: %@=0 (software decoding, category %d)",key,category); }
    old_int(self,cmd,value,key,category);
}
static void set_value(id self,SEL cmd,NSString *value,NSString *key,int category)
{
    if(hardware_option(key)) { value=@"0"; NSLog(@"bili-compat: %@=0 (software decoding, category %d)",key,category); }
    old_value(self,cmd,value,key,category);
}
static void set_player(id self,SEL cmd,int64_t value,NSString *key)
{
    if(hardware_option(key)) { value=0; NSLog(@"bili-compat: player %@=0 (software decoding)",key); }
    old_player(self,cmd,value,key);
}
__attribute__((constructor)) static void bili_playback_init(void)
{
    char args[2048]={0};size_t size=sizeof(args)-1;
    if(sysctlbyname("kern.bootargs",args,&size,NULL,0) || !strstr(args,"alioth_bili_sw=1")) return;
    const uint8_t uuid[16]={0x5e,0x04,0x6e,0xac,0x18,0xf7,0x31,0xe9,0x8d,0xe4,0x85,0x85,0x8e,0x7d,0x14,0x29};
    BOOL matched=NO;
    for(uint32_t i=0;i<_dyld_image_count();i++) {
        const struct mach_header_64 *h=(const void *)_dyld_get_image_header(i);
        if(!h || h->magic!=MH_MAGIC_64 || h->filetype!=MH_EXECUTE) continue;
        const uint8_t *p=(const uint8_t *)(h+1);
        for(uint32_t k=0;k<h->ncmds;k++) {
            const struct load_command *lc=(const void *)p;
            if(lc->cmdsize<8) return;
            if(lc->cmd==LC_UUID && lc->cmdsize>=24) matched=memcmp(((const struct uuid_command *)lc)->uuid,uuid,16)==0;
            p+=lc->cmdsize;
        }
    }
    if(!matched) return;
    fonts_enabled=YES;
    BOOL diagnostic=strstr(args,"alioth_bili_log=1")!=NULL;
    if(diagnostic) {
        int fd=open("/dev/console",O_WRONLY);
        if(fd>=0) { dup2(fd,2); if(fd!=2) close(fd); }
    }
    /* The system CJK font works, but this app also ships private-use icon
     * fonts. Register the actual bundled files in this process explicitly. */
    unsigned fonts=0;
    for(NSString *extension in @[@"ttf",@"otf",@"ttc"]) {
        for(NSURL *url in [[NSBundle mainBundle] URLsForResourcesWithExtension:extension subdirectory:nil]) {
            CFErrorRef error=NULL;
            if(CTFontManagerRegisterFontsForURL((CFURLRef)url,kCTFontManagerScopeProcess,&error)) fonts++;
            if(error) CFRelease(error);
        }
    }
    NSLog(@"bili-compat: %u bundled fonts explicitly registered",fonts);
    Class fontClass=objc_getClass("UIFont");
    Method fm=fontClass ? class_getClassMethod(fontClass,sel_registerName("fontWithName:size:")) : NULL;
    if(fm && method_getNumberOfArguments(fm)==4) old_font=(void *)method_setImplementation(fm,(IMP)font_with_name);
    fm=fontClass ? class_getClassMethod(fontClass,sel_registerName("fontWithDescriptor:size:")) : NULL;
    if(fm && method_getNumberOfArguments(fm)==4) old_descriptor_font=(void *)method_setImplementation(fm,(IMP)font_with_descriptor);
    Class descriptorClass=objc_getClass("UIFontDescriptor");
    fm=descriptorClass ? class_getClassMethod(descriptorClass,sel_registerName("fontDescriptorWithName:size:")) : NULL;
    if(fm && method_getNumberOfArguments(fm)==4) old_descriptor=(void *)method_setImplementation(fm,(IMP)descriptor_with_name);
    NSLog(@"bili-compat: fallback for four-glyph placeholder text fonts=%d",old_font!=NULL);
    Class options=objc_getClass("IJKFFOptions");
    Method m=options ? class_getInstanceMethod(options,sel_registerName("setOptionIntValue:forKey:ofCategory:")) : NULL;
    if(m && method_getNumberOfArguments(m)==5) old_int=(void *)method_setImplementation(m,(IMP)set_int);
    m=options ? class_getInstanceMethod(options,sel_registerName("setOptionValue:forKey:ofCategory:")) : NULL;
    if(m && method_getNumberOfArguments(m)==5) old_value=(void *)method_setImplementation(m,(IMP)set_value);
    Class player=objc_getClass("IJKFFMoviePlayerControllerFFPlay");
    m=player ? class_getInstanceMethod(player,sel_registerName("setPlayerOptionIntValue:forKey:")) : NULL;
    if(m && method_getNumberOfArguments(m)==4) old_player=(void *)method_setImplementation(m,(IMP)set_player);
    m=player ? class_getInstanceMethod(player,sel_registerName("prepareToPlay")) : NULL;
    if(m && method_getNumberOfArguments(m)==2) old_prepare=(void *)method_setImplementation(m,(IMP)prepare);
    Class audio=objc_getClass("AVAudioSession");
    m=audio ? class_getInstanceMethod(audio,sel_registerName("setActive:error:")) : NULL;
    if(m && method_getNumberOfArguments(m)==4) old_active=(void *)method_setImplementation(m,(IMP)active);
    m=audio ? class_getInstanceMethod(audio,sel_registerName("setActive:withOptions:error:")) : NULL;
    if(m && method_getNumberOfArguments(m)==5) old_active_options=(void *)method_setImplementation(m,(IMP)active_options);
    NSLog(@"bili-compat: IJK software-option hooks int=%d value=%d player=%d",old_int!=NULL,old_value!=NULL,old_player!=NULL);
    /* The current client reports frames through another renderer. Keep the
     * public-IJK CPU presenter experimental until its actual ABI is observed. */
    if(strstr(args,"alioth_bili_canvas=1")) alioth_bili_canvas_init();
    if(strstr(args,"alioth_bili_sample=1")) alioth_bili_sample_canvas_init();
    /* Diagnostic state only. Never log stream URLs, credentials or headers. */
    if(diagnostic) [[NSNotificationCenter defaultCenter] addObserverForName:nil object:nil queue:nil usingBlock:^(NSNotification *note) {
        if(![note.name hasPrefix:@"IJKMP"] && ![note.name hasPrefix:@"IJKFF"]) return;
        static unsigned frame_events;
        if([note.name isEqualToString:@"IJKMPMoviePlayerFrameDecodedNotification"] && ++frame_events>3 && frame_events%120) return;
        NSMutableDictionary *codes=[NSMutableDictionary dictionary];
        for(id key in note.userInfo) {
            id value=note.userInfo[key];
            if([key isKindOfClass:[NSString class]] && [value isKindOfClass:[NSNumber class]]) codes[key]=value;
        }
        NSLog(@"bili-compat: event %@ codes=%@",note.name,codes);
    }];
}
