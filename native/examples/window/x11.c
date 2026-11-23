#include "window.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct NativeWindow {
  Display *display;
  Window id;
  GC pen;
  XFontSet font;
  Atom protocols, close;
  int width, height, textLength;
  char *text;
};
static atomic_int live;
static pthread_once_t initialized = PTHREAD_ONCE_INIT;
static int threadSafe;
static void initialize(void) { threadSafe = XInitThreads(); }

NativeWindow *windowCreate(const struct WindowSize size[static 1]) {
  pthread_once(&initialized, initialize);
  NativeWindow *w = calloc(1, sizeof(*w));
  if (!w)
    abort();
  atomic_fetch_add(&live, 1);
  if (!threadSafe || size->width < 1 || size->height < 1 || !(w->display = XOpenDisplay(NULL)))
    return w;
  Display *d = w->display;
  int screen = DefaultScreen(d);
  w->id = XCreateSimpleWindow(d, RootWindow(d, screen), 0, 0, size->width, size->height, 0, 0,
                              BlackPixel(d, screen));
  w->width = size->width;
  w->height = size->height;
  w->pen = XCreateGC(d, w->id, 0, NULL);
  XSetForeground(d, w->pen, WhitePixel(d, screen));
  char **missing, *replacement;
  int count;
  w->font = XCreateFontSet(d, "fixed", &missing, &count, &replacement);
  if (missing)
    XFreeStringList(missing);
  w->protocols = XInternAtom(d, "WM_PROTOCOLS", False);
  w->close = XInternAtom(d, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(d, w->id, &w->close, 1);
  XSelectInput(d, w->id, ExposureMask | KeyPressMask | StructureNotifyMask);
  return w;
}
int windowReady(NativeWindow *w) { return w->id && w->font && w->pen; }
void windowTitle(NativeWindow *w, const char *text, unsigned long long count) {
  if (!windowReady(w) || count > INT_MAX)
    return;
  XChangeProperty(w->display, w->id, XInternAtom(w->display, "_NET_WM_NAME", False),
                  XInternAtom(w->display, "UTF8_STRING", False), 8, PropModeReplace,
                  (const unsigned char *)text, (int)count);
}
void windowText(NativeWindow *w, const char *text, unsigned long long count) {
  if (!windowReady(w) || count > INT_MAX)
    return;
  char *copy = malloc(count + 1);
  if (!copy)
    return;
  memcpy(copy, text, count);
  copy[count] = 0;
  free(w->text);
  w->text = copy;
  w->textLength = (int)count;
  XClearArea(w->display, w->id, 0, 0, 0, 0, True);
}
void windowColor(NativeWindow *w, unsigned rgb) {
  if (!windowReady(w))
    return;
  XColor c = {.red = ((rgb >> 16) & 255) * 257,
              .green = ((rgb >> 8) & 255) * 257,
              .blue = (rgb & 255) * 257};
  if (XAllocColor(w->display, DefaultColormap(w->display, DefaultScreen(w->display)), &c)) {
    XSetWindowBackground(w->display, w->id, c.pixel);
    XClearArea(w->display, w->id, 0, 0, 0, 0, True);
  }
}
void windowShow(NativeWindow *w) {
  if (!windowReady(w))
    return;
  XMapWindow(w->display, w->id);
  XFlush(w->display);
}
int windowNextEvent(NativeWindow *w) {
  if (!windowReady(w))
    return 2;
  Display *d = w->display;
  if (!XPending(d)) {
    struct pollfd p = {ConnectionNumber(d), POLLIN, 0};
    if (poll(&p, 1, 50) <= 0 || !XPending(d))
      return 0;
  }
  XEvent e;
  XNextEvent(d, &e);
  if (e.xany.window != w->id)
    return 0;
  switch (e.type) {
  case KeyPress:
    return 1;
  case ClientMessage:
    return e.xclient.message_type == w->protocols && e.xclient.format == 32 &&
                   (Atom)e.xclient.data.l[0] == w->close
               ? 2
               : 0;
  case ConfigureNotify:
    w->width = e.xconfigure.width;
    w->height = e.xconfigure.height;
    XClearArea(d, w->id, 0, 0, 0, 0, True);
    return 3;
  case Expose:
    if (w->text && !e.xexpose.count) {
      int width = Xutf8TextEscapement(w->font, w->text, w->textLength);
      Xutf8DrawString(d, w->id, w->font, w->pen, (w->width - width) / 2, w->height / 2, w->text,
                      w->textLength);
    }
  }
  return 0;
}
void windowRelease(NativeWindow *w) {
  if (w->display) {
    if (w->pen)
      XFreeGC(w->display, w->pen);
    if (w->font)
      XFreeFontSet(w->display, w->font);
    if (w->id)
      XDestroyWindow(w->display, w->id);
    XCloseDisplay(w->display);
  }
  free(w->text);
  free(w);
  atomic_fetch_sub(&live, 1);
}
int windowLive(void) { return atomic_load(&live); }
void windowResize(NativeWindow *w, const struct WindowSize size[static 1]) {
  if (!windowReady(w))
    return;
  if (size->width > 0 && size->height > 0)
    XResizeWindow(w->display, w->id, size->width, size->height);
  XFlush(w->display);
}
int windowWidth(NativeWindow *w) { return w->width; }
void windowPostKey(NativeWindow *w) {
  if (!windowReady(w))
    return;
  XEvent e = {0};
  e.xkey.type = KeyPress;
  e.xkey.display = w->display;
  e.xkey.window = w->id;
  e.xkey.root = DefaultRootWindow(w->display);
  e.xkey.same_screen = True;
  e.xkey.keycode = XKeysymToKeycode(w->display, XK_K);
  XSendEvent(w->display, w->id, False, KeyPressMask, &e);
  XFlush(w->display);
}
