#import "window.h"
#import <AppKit/AppKit.h>
#include <stdatomic.h>

// AppKit's selectors and event objects stay on the main thread. Kalandra owns
// the window and decides how to lay it out, present it and respond to events.
@interface WindowBinding : NSObject <NSWindowDelegate>
@property(strong) NSWindow *window;
@property(strong) NSTextField *label;
@property BOOL resized;
@property BOOL closing;
@end
@implementation WindowBinding
- (BOOL)windowShouldClose:(NSWindow *)sender {
  self.closing = YES;
  return NO;
}
- (void)windowDidResize:(NSNotification *)notification {
  self.resized = YES;
}
@end

static atomic_int live;
static WindowBinding *binding(NativeWindow *w) { return (__bridge WindowBinding *)w; }
static NSString *string(const char *bytes, unsigned long long count) {
  return [[NSString alloc] initWithBytes:bytes length:count encoding:NSUTF8StringEncoding];
}
NativeWindow *windowCreate(const struct WindowSize size[static 1]) {
  @autoreleasepool {
    NSApplication *app = NSApplication.sharedApplication;
    [app setActivationPolicy:NSApplicationActivationPolicyRegular];
    static dispatch_once_t once;
    dispatch_once(&once, ^{
      [app finishLaunching];
    });
    WindowBinding *b = [WindowBinding new];
    b.window =
        [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, size->width, size->height)
                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                              NSWindowStyleMaskResizable
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    b.window.releasedWhenClosed = NO;
    b.window.delegate = b;
    atomic_fetch_add(&live, 1);
    return (NativeWindow *)CFBridgingRetain(b);
  }
}
int windowReady(NativeWindow *w) { return binding(w).window != nil; }
void windowTitle(NativeWindow *w, const char *bytes, unsigned long long count) {
  @autoreleasepool {
    binding(w).window.title = string(bytes, count);
  }
}
void windowText(NativeWindow *w, const char *bytes, unsigned long long count) {
  @autoreleasepool {
    WindowBinding *b = binding(w);
    if (!b.label) {
      b.label = [NSTextField labelWithString:@""];
      b.label.textColor = NSColor.whiteColor;
      b.label.font = [NSFont systemFontOfSize:32];
      b.label.translatesAutoresizingMaskIntoConstraints = NO;
      NSView *view = b.window.contentView;
      [view addSubview:b.label];
      [NSLayoutConstraint activateConstraints:@[
        [b.label.centerXAnchor constraintEqualToAnchor:view.centerXAnchor],
        [b.label.centerYAnchor constraintEqualToAnchor:view.centerYAnchor]
      ]];
    }
    b.label.stringValue = string(bytes, count);
  }
}
void windowColor(NativeWindow *w, unsigned rgb) {
  @autoreleasepool {
    binding(w).window.backgroundColor = [NSColor colorWithSRGBRed:((rgb >> 16) & 255) / 255.
                                                            green:((rgb >> 8) & 255) / 255.
                                                             blue:(rgb & 255) / 255.
                                                            alpha:1];
  }
}
void windowShow(NativeWindow *w) {
  @autoreleasepool {
    [binding(w).window center];
    [binding(w).window makeKeyAndOrderFront:nil];
  }
}
int windowNextEvent(NativeWindow *w) {
  @autoreleasepool {
    WindowBinding *b = binding(w);
    if (b.closing)
      return 2;
    if (b.resized) {
      b.resized = NO;
      return 3;
    }
    NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                    untilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]
                                       inMode:NSDefaultRunLoopMode
                                      dequeue:YES];
    if (e.type == NSEventTypeKeyDown && e.window == b.window)
      return 1;
    if (e)
      [NSApp sendEvent:e];
    [NSApp updateWindows];
    return 0;
  }
}
void windowRelease(NativeWindow *w) {
  WindowBinding *b = CFBridgingRelease(w);
  void (^finish)(void) = ^{
    @autoreleasepool {
      b.window.delegate = nil;
      [b.window close];
      atomic_fetch_sub(&live, 1);
    }
  };
  if (NSThread.isMainThread)
    finish();
  else
    dispatch_async(dispatch_get_main_queue(), finish);
}
int windowLive(void) { return atomic_load(&live); }
void windowResize(NativeWindow *w, const struct WindowSize size[static 1]) {
  [binding(w).window setContentSize:NSMakeSize(size->width, size->height)];
}
int windowWidth(NativeWindow *w) { return (int)binding(w).window.contentView.bounds.size.width; }
void windowPostKey(NativeWindow *w) {
  @autoreleasepool {
    NSEvent *key = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                    location:NSZeroPoint
                               modifierFlags:0
                                   timestamp:0
                                windowNumber:binding(w).window.windowNumber
                                     context:nil
                                  characters:@"k"
                 charactersIgnoringModifiers:@"k"
                                   isARepeat:NO
                                     keyCode:40];
    [NSApp postEvent:key atStart:YES];
  }
}
