# A native AppKit window

Build on an Apple Silicon Mac with Xcode or its command-line tools. From this directory, run:

```sh
mkdir -p build/HelloWindow.app/Contents/MacOS
kalandra --project kalandra.toml -o build/HelloWindow.app/Contents/MacOS/HelloWindow
cp Info.plist build/HelloWindow.app/Contents/
codesign --force --sign - build/HelloWindow.app
open build/HelloWindow.app
```

The executable opens a resizable window and displays “Hello, world!”. Click the close button or press Escape to quit.

The manifest names AppKit as a system dependency. The compiler locates the active macOS SDK, reads the public declarations used by the app, and links the framework. In Kalandra, qualify a method with its declaring class or protocol, join selector components with underscores, and pass the receiver first. For example, `NSWindow_makeKeyAndOrderFront(window, 0)` sends `makeKeyAndOrderFront:` to the window. An `unsafe` call places responsibility for the receiver, object lifetimes and UI thread on the caller.
