from "appkit" import CGRect, NSObject_alloc, NSObject_new
from "appkit" import NSObject_release, NSAutoreleasePool_drain, NSThread_isMainThread
from "appkit" import NSApplication_sharedApplication, NSApplication_setActivationPolicy, NSApplication_finishLaunching
from "appkit" import NSApplication_activateIgnoringOtherApps, NSWindow_initWithContentRect_styleMask_backing_defer, NSWindow_setReleasedWhenClosed
from "appkit" import NSWindow_setTitle, NSTextField_labelWithString, NSFont_systemFontOfSize
from "appkit" import NSControl_setFont, NSControl_setAlignment, NSView_setFrame
from "appkit" import NSView_setAutoresizingMask, NSWindow_contentView, NSView_addSubview
from "appkit" import NSWindow_center, NSWindow_makeKeyAndOrderFront, NSWindow_isVisible
from "appkit" import NSDate_dateWithTimeIntervalSinceNow, NSApplication_nextEventMatchingMask_untilDate_inMode_dequeue, NSApplication_sendEvent
from "appkit" import NSApplication_updateWindows, NSEvent_type, NSEvent_charactersIgnoringModifiers
from "appkit" import NSString_isEqualToString, NSWindow_close, objc_getClass
from "appkit" import NSString_stringWithUTF8String
from "std/process" import ExitStatus

fn nsString(text: String) -> u64:
  return unsafe NSString_stringWithUTF8String(unsafe objc_getClass("NSString"), text)

fn openWindow(title: String, message: String) -> u64:
  frame = CGRect {x: 0.0, y: 0.0, width: 540.0, height: 300.0}
  # Titled, closable, miniaturizable and resizable; buffered backing.
  window = unsafe NSWindow_initWithContentRect_styleMask_backing_defer(unsafe NSObject_alloc(unsafe objc_getClass("NSWindow")), frame, 15, 2, false)
  unsafe NSWindow_setReleasedWhenClosed(window, false)
  unsafe NSWindow_setTitle(window, nsString(title))

  label = unsafe NSTextField_labelWithString(unsafe objc_getClass("NSTextField"), nsString(message))
  unsafe NSControl_setFont(label, unsafe NSFont_systemFontOfSize(unsafe objc_getClass("NSFont"), 32.0))
  unsafe NSControl_setAlignment(label, 1)
  unsafe NSView_setFrame(label, CGRect {x: 0.0, y: 128.0, width: 540.0, height: 44.0})
  # Flexible width and equal flexible top/bottom margins keep the label centered.
  unsafe NSView_setAutoresizingMask(label, 42)
  unsafe NSView_addSubview(unsafe NSWindow_contentView(window), label)
  unsafe NSWindow_center(window)
  unsafe NSWindow_makeKeyAndOrderFront(window, 0)
  return window

fn handleEvents(app: u64, window: u64, poolClass: u64) -> unit:
  mode = nsString("kCFRunLoopDefaultMode")
  escape = nsString("\u{1b}")
  dateClass = unsafe objc_getClass("NSDate")
  while unsafe NSWindow_isVisible(window):
    pool = unsafe NSObject_new(poolClass)
    until = unsafe NSDate_dateWithTimeIntervalSinceNow(dateClass, 0.25)
    event = unsafe NSApplication_nextEventMatchingMask_untilDate_inMode_dequeue(app, 18446744073709551615, until, mode, true)
    if event != 0:
      if unsafe NSEvent_type(event) == 10 and unsafe NSString_isEqualToString(unsafe NSEvent_charactersIgnoringModifiers(event), escape):
        unsafe NSWindow_close(window)
      else:
        unsafe NSApplication_sendEvent(app, event)
    unsafe NSApplication_updateWindows(app)
    unsafe NSAutoreleasePool_drain(pool)
  return ()

fn main() -> ExitStatus:
  if (unsafe NSThread_isMainThread(unsafe objc_getClass("NSThread"))) == false:
    return ExitStatus {code: 1}
  poolClass = unsafe objc_getClass("NSAutoreleasePool")
  pool = unsafe NSObject_new(poolClass)
  app = unsafe NSApplication_sharedApplication(unsafe objc_getClass("NSApplication"))
  unsafe NSApplication_setActivationPolicy(app, 0)
  unsafe NSApplication_finishLaunching(app)
  window = openWindow("Kalandra", "Hello, world!")
  unsafe NSApplication_activateIgnoringOtherApps(app, true)
  handleEvents(app, window, poolClass)
  unsafe NSObject_release(window)
  unsafe NSAutoreleasePool_drain(pool)
  return ExitStatus {code: 0}
