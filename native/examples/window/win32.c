#define UNICODE
#define _UNICODE
#include "window.h"
#include <limits.h>
#include <stdlib.h>
#include <windows.h>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

struct NativeWindow {
  HWND hwnd;
  DWORD owner;
  HBRUSH color;
  wchar_t *text;
  int key, closing, resized;
  LONG released;
};
static const wchar_t className[] = L"KalandraNativeWindow";
static LONG live;
static void dispose(NativeWindow *w) {
  if (w->hwnd)
    DestroyWindow(w->hwnd);
  if (w->color)
    DeleteObject(w->color);
  free(w->text);
  free(w);
  InterlockedDecrement(&live);
}
static LRESULT CALLBACK events(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE)
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
  NativeWindow *w = (NativeWindow *)GetWindowLongPtrW(h, GWLP_USERDATA);
  if (w)
    switch (msg) {
    case WM_KEYDOWN:
      w->key = 1;
      return 0;
    case WM_CLOSE:
      w->closing = 1;
      return 0;
    case WM_SIZE:
      w->resized = 1;
      return 0;
    case WM_APP + 1:
      if (InterlockedCompareExchange(&w->released, 2, 1) == 1)
        dispose(w);
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT client;
      GetClientRect(h, &client);
      FillRect(dc, &client, w->color ? w->color : GetSysColorBrush(COLOR_WINDOW));
      SetTextColor(dc, RGB(255, 255, 255));
      SetBkMode(dc, TRANSPARENT);
      if (w->text)
        DrawTextW(dc, w->text, -1, &client, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_NCDESTROY:
      w->hwnd = NULL;
      SetWindowLongPtrW(h, GWLP_USERDATA, 0);
      break;
    }
  return DefWindowProcW(h, msg, wp, lp);
}
static RECT frame(const struct WindowSize *size) {
  RECT r = {0, 0, size->width, size->height};
  AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
  return r;
}
NativeWindow *windowCreate(const struct WindowSize size[static 1]) {
  NativeWindow *w = calloc(1, sizeof(*w));
  if (!w)
    abort();
  InterlockedIncrement(&live);
  w->owner = GetCurrentThreadId();
  WNDCLASSW cls = {0};
  cls.lpfnWndProc = events;
  cls.hInstance = GetModuleHandleW(NULL);
  cls.hCursor = LoadCursorW(NULL, IDC_ARROW);
  cls.lpszClassName = className;
  if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    return w;
  RECT r = frame(size);
  w->hwnd = CreateWindowExW(0, className, L"", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            r.right - r.left, r.bottom - r.top, NULL, NULL, cls.hInstance, w);
  return w;
}
int windowReady(NativeWindow *w) { return w->hwnd != NULL; }
static wchar_t *wide(const char *text, unsigned long long count) {
  if (count > INT_MAX)
    return NULL;
  int n = count ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, (int)count, NULL, 0) : 0;
  if (!n && count)
    return NULL;
  wchar_t *s = calloc((size_t)n + 1, sizeof(*s));
  if (s && n)
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, (int)count, s, n);
  return s;
}
void windowTitle(NativeWindow *w, const char *text, unsigned long long count) {
  wchar_t *s = wide(text, count);
  if (s) {
    SetWindowTextW(w->hwnd, s);
    free(s);
  }
}
void windowText(NativeWindow *w, const char *text, unsigned long long count) {
  wchar_t *s = wide(text, count);
  if (s) {
    free(w->text);
    w->text = s;
    InvalidateRect(w->hwnd, NULL, TRUE);
  }
}
void windowColor(NativeWindow *w, unsigned rgb) {
  HBRUSH color = CreateSolidBrush(RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255));
  if (color) {
    if (w->color)
      DeleteObject(w->color);
    w->color = color;
    InvalidateRect(w->hwnd, NULL, TRUE);
  }
}
void windowShow(NativeWindow *w) {
  ShowWindow(w->hwnd, SW_SHOWNORMAL);
  UpdateWindow(w->hwnd);
}
int windowNextEvent(NativeWindow *w) {
  MSG m;
  if (!w->closing && !w->key && !w->resized)
    MsgWaitForMultipleObjectsEx(0, NULL, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
  if (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
  if (w->closing)
    return 2;
  if (w->key) {
    w->key = 0;
    return 1;
  }
  if (w->resized) {
    w->resized = 0;
    return 3;
  }
  return 0;
}
void windowRelease(NativeWindow *w) {
  if (!w->hwnd || w->owner == GetCurrentThreadId())
    dispose(w);
  else {
    HWND hwnd = w->hwnd;
    InterlockedExchange(&w->released, 1);
    SendNotifyMessageW(hwnd, WM_APP + 1, 0, 0);
  }
}
int windowLive(void) { return (int)InterlockedCompareExchange(&live, 0, 0); }
void windowResize(NativeWindow *w, const struct WindowSize size[static 1]) {
  RECT r = frame(size);
  SetWindowPos(w->hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
}
int windowWidth(NativeWindow *w) {
  RECT r = {0};
  GetClientRect(w->hwnd, &r);
  return r.right - r.left;
}
void windowPostKey(NativeWindow *w) { PostMessageW(w->hwnd, WM_KEYDOWN, 'K', 0); }
