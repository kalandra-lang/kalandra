# Hello window

Build and run from this directory with a Kalandra compiler that supports native imports.

```sh
kalandra --project kalandra.toml -o hello-window
./hello-window
```

On Windows use a Visual Studio developer terminal with Clang and the Windows SDK available. Build with `-o hello-window.exe` and run `.\hello-window.exe`. On macOS install Apple's command-line developer tools. On Linux install the X11 development headers and libraries, then run in an X11 or XWayland session with `DISPLAY` set.

The application opens a resizable blue window with “Hello, world!” centered in it. Press a key in the window or use its close button to exit. Edit the dimensions, title, text, color and event handling in [main.k](main.k).

[window.h](window.h) declares the native contract. The compiler reads the C declarations, checks each borrow and builds the implementation for the selected target. The manifest can also refer to a prebuilt library and its header. An application using an installed Kalandra package can import its public Kalandra API while the package owns these declarations and target choices.

- On macOS, [appkit.m](appkit.m) bridges Objective-C selectors, ARC ownership and the AppKit event queue. The main task creates and operates the window. The window delegate translates resize and close notifications.
- On Windows, [win32.c](win32.c) creates an HWND, translates UTF-8 into UTF-16 for the wide Win32 APIs and translates window messages.
- On Linux, [x11.c](x11.c) opens an Xlib display and subscribes to exposure, configuration, key and window-manager close events.

Releasing the owning value disposes the native object. AppKit and Win32 adapters marshal off-thread teardown to the owning UI thread, which must keep processing events while it owns windows. The application shown here owns its window on the main task. Each call to the event pump waits up to 50 ms for input so control returns to Kalandra between events.
