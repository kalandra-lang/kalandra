// C17 adapter contract. Each implementation owns its platform's native objects.
#pragma once

typedef struct NativeWindow NativeWindow;
struct WindowSize { int width, height; };
#define UI __attribute__((swift_attr("@MainActor")))
#define WINDOW_BORROW __attribute__((use_handle("NativeWindow"), noescape))
#define TEXT_EXTENT(p,n) __attribute__((diagnose_if((n) > __builtin_object_size((p), 0), "text exceeds storage", "error")))

NativeWindow * _Nonnull windowCreate(const struct WindowSize size[static 1] __attribute__((noescape)))
    __attribute__((acquire_handle("NativeWindow"))) UI;
int windowReady(NativeWindow * _Nonnull window WINDOW_BORROW) UI;
void windowTitle(NativeWindow * _Nonnull window WINDOW_BORROW,
                 const char * _Nonnull text __attribute__((noescape)), unsigned long long count)
    UI TEXT_EXTENT(text,count);
void windowText(NativeWindow * _Nonnull window WINDOW_BORROW,
                const char * _Nonnull text __attribute__((noescape)), unsigned long long count)
    UI TEXT_EXTENT(text,count);
void windowColor(NativeWindow * _Nonnull window WINDOW_BORROW, unsigned rgb) UI;
void windowShow(NativeWindow * _Nonnull window WINDOW_BORROW) UI;
// 0 idle, 1 key down, 2 close request, 3 resize.
int windowNextEvent(NativeWindow * _Nonnull window WINDOW_BORROW) UI;
void windowRelease(NativeWindow * _Nonnull window __attribute__((release_handle("NativeWindow"))))
    __attribute__((swift_attr("nonisolated")));

// Used by the integration test to inject native events through the platform queue.
void windowResize(NativeWindow * _Nonnull window WINDOW_BORROW,
                  const struct WindowSize size[static 1] __attribute__((noescape))) UI;
void windowPostKey(NativeWindow * _Nonnull window WINDOW_BORROW) UI;
int windowWidth(NativeWindow * _Nonnull window WINDOW_BORROW) UI;
int windowLive(void) UI;
