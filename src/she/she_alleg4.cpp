// SHE library
// Copyright (C) 2012-2013  David Capello
//
// This source file is distributed under MIT license,
// please read LICENSE.txt for more information.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "she.h"

#include "base/compiler_specific.h"
#include "base/string.h"

#include <allegro.h>
#include <allegro/internal/aintern.h>

#ifdef WIN32
  #include <winalleg.h>

  #if defined STRICT || defined __GNUC__
    typedef WNDPROC wndproc_t;
  #else
    typedef FARPROC wndproc_t;
  #endif
#endif

#include "loadpng.h"

#include <cassert>
#include <vector>

#define DISPLAY_FLAG_FULL_REFRESH     1
#define DISPLAY_FLAG_WINDOW_RESIZE    2

static volatile int display_flags = 0;
static volatile int original_width = 0;
static volatile int original_height = 0;

// Used by set_display_switch_callback(SWITCH_IN, ...).
static void display_switch_in_callback()
{
  display_flags |= DISPLAY_FLAG_FULL_REFRESH;
}

END_OF_STATIC_FUNCTION(display_switch_in_callback);

#ifdef ALLEGRO4_WITH_RESIZE_PATCH
// Called when the window is resized
static void resize_callback(RESIZE_DISPLAY_EVENT* ev)
{
  if (ev->is_maximized) {
    original_width = ev->old_w;
    original_height = ev->old_h;
  }
  display_flags |= DISPLAY_FLAG_WINDOW_RESIZE;
}
#endif // ALLEGRO4_WITH_RESIZE_PATCH

namespace she {

class Alleg4Surface : public Surface
                    , public LockedSurface {
public:
  enum DestroyFlag { NoDestroy, AutoDestroy };

  Alleg4Surface(BITMAP* bmp, DestroyFlag destroy)
    : m_bmp(bmp)
    , m_destroy(destroy)
  {
  }

  Alleg4Surface(int width, int height)
    : m_bmp(create_bitmap(width, height))
    , m_destroy(AutoDestroy)
  {
  }

  ~Alleg4Surface() {
    if (m_destroy == AutoDestroy)
      destroy_bitmap(m_bmp);
  }

  // Surface implementation

  void dispose() {
    delete this;
  }

  int width() const {
    return m_bmp->w;
  }

  int height() const {
    return m_bmp->h;
  }

  LockedSurface* lock() {
    acquire_bitmap(m_bmp);
    return this;
  }

  void* nativeHandle() {
    return reinterpret_cast<void*>(m_bmp);
  }

  // LockedSurface implementation

  void unlock() {
    release_bitmap(m_bmp);
  }

  void clear() {
    clear_to_color(m_bmp, 0);
  }

  void blitTo(LockedSurface* dest, int srcx, int srcy, int dstx, int dsty, int width, int height) const {
    ASSERT(m_bmp);
    ASSERT(dest);
    ASSERT(static_cast<Alleg4Surface*>(dest)->m_bmp);

    blit(m_bmp,
         static_cast<Alleg4Surface*>(dest)->m_bmp,
         srcx, srcy,
         dstx, dsty,
         width, height);
  }

  void drawAlphaSurface(const LockedSurface* src, int dstx, int dsty) {
    set_alpha_blender();
    draw_trans_sprite(m_bmp, static_cast<const Alleg4Surface*>(src)->m_bmp, dstx, dsty);
  }

private:
  BITMAP* m_bmp;
  DestroyFlag m_destroy;
};

class Alleg4EventQueue : public EventQueue {
public:
  Alleg4EventQueue() {
  }

  void dispose() {
    delete this;
  }

  void getEvent(Event& event) {
    if (m_events.size() > 0) {
      event = m_events[0];
      m_events.erase(m_events.begin());
    }
    else
      event.setType(Event::None);
  }

  void queueEvent(const Event& event) {
    m_events.push_back(event);
  }

private:
  std::vector<Event> m_events;
};

#if WIN32
namespace {

Display* unique_display = NULL;
wndproc_t base_wndproc = NULL;

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
  switch (msg) {

    case WM_DROPFILES:
      {
        HDROP hdrop = (HDROP)(wparam);
        Event::Files files;

        int count = DragQueryFile(hdrop, 0xFFFFFFFF, NULL, 0);
        for (int index=0; index<count; ++index) {
          int length = DragQueryFile(hdrop, index, NULL, 0);
          if (length > 0) {
            std::vector<TCHAR> str(length+1);
            DragQueryFile(hdrop, index, &str[0], str.size());
            files.push_back(base::to_utf8(&str[0]));
          }
        }

        DragFinish(hdrop);

        Event ev;
        ev.setType(Event::DropFiles);
        ev.setFiles(files);
        static_cast<Alleg4EventQueue*>(unique_display->getEventQueue())
          ->queueEvent(ev);
      }
      break;

  }
  return ::CallWindowProc(base_wndproc, hwnd, msg, wparam, lparam);
}

void subclass_hwnd(HWND hwnd)
{
  // Add the WS_EX_ACCEPTFILES
  SetWindowLong(hwnd, GWL_EXSTYLE,
    GetWindowLong(hwnd, GWL_EXSTYLE) | WS_EX_ACCEPTFILES);

  base_wndproc = (wndproc_t)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)wndproc);
}

void unsubclass_hwnd(HWND hwnd)
{
  SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)base_wndproc);
  base_wndproc = NULL;
}
  
}
#endif

class Alleg4Display : public Display {
public:
  Alleg4Display(int width, int height, int scale)
    : m_surface(NULL)
    , m_scale(0) {
    unique_display = this;

    if (install_mouse() < 0) throw DisplayCreationException(allegro_error);
    if (install_keyboard() < 0) throw DisplayCreationException(allegro_error);

#ifdef FULLSCREEN_PLATFORM
    set_color_depth(16);        // TODO Try all color depths for fullscreen platforms
#else
    set_color_depth(desktop_color_depth());
#endif

    if (set_gfx_mode(
#ifdef FULLSCREEN_PLATFORM
                     GFX_AUTODETECT_FULLSCREEN,
#else
                     GFX_AUTODETECT_WINDOWED,
#endif
                     width, height, 0, 0) < 0)
      throw DisplayCreationException(allegro_error);

    setScale(scale);

    m_queue = new Alleg4EventQueue();

    // Add a hook to display-switch so when the user returns to the
    // screen it's completelly refreshed/redrawn.
    LOCK_VARIABLE(display_flags);
    LOCK_FUNCTION(display_switch_in_callback);
    set_display_switch_callback(SWITCH_IN, display_switch_in_callback);

#ifdef ALLEGRO4_WITH_RESIZE_PATCH
    // Setup the handler for window-resize events
    set_resize_callback(resize_callback);
#endif

#if WIN32
    subclass_hwnd((HWND)nativeHandle());
#endif WIN32
  }

  ~Alleg4Display() {
    delete m_queue;

#if WIN32
    unsubclass_hwnd((HWND)nativeHandle());
#endif WIN32

    m_surface->dispose();
    set_gfx_mode(GFX_TEXT, 0, 0, 0, 0);

    unique_display = NULL;
  }

  void dispose() OVERRIDE {
    delete this;
  }

  int width() const OVERRIDE {
    return SCREEN_W;
  }

  int height() const OVERRIDE {
    return SCREEN_H;
  }

  int originalWidth() const OVERRIDE {
    return original_width > 0 ? original_width: width();
  }

  int originalHeight() const OVERRIDE {
    return original_height > 0 ? original_height: height();
  }

  void setScale(int scale) OVERRIDE {
    ASSERT(scale >= 1);

    if (m_scale == scale)
      return;

    m_scale = scale;
    Surface* newSurface = new Alleg4Surface(SCREEN_W/m_scale,
                                            SCREEN_H/m_scale);
    if (m_surface)
      m_surface->dispose();
    m_surface = newSurface;
  }

  NotDisposableSurface* getSurface() OVERRIDE {
    return static_cast<NotDisposableSurface*>(m_surface);
  }

  bool flip() OVERRIDE {
#ifdef ALLEGRO4_WITH_RESIZE_PATCH
    if (display_flags & DISPLAY_FLAG_WINDOW_RESIZE) {
      display_flags ^= DISPLAY_FLAG_WINDOW_RESIZE;

      acknowledge_resize();

      int scale = m_scale;
      m_scale = 0;
      setScale(scale);
      return false;
    }
#endif

    BITMAP* bmp = reinterpret_cast<BITMAP*>(m_surface->nativeHandle());
    if (m_scale == 1) {
      blit(bmp, screen, 0, 0, 0, 0, SCREEN_W, SCREEN_H);
    }
    else {
      stretch_blit(bmp, screen,
                   0, 0, bmp->w, bmp->h,
                   0, 0, SCREEN_W, SCREEN_H);
    }

    return true;
  }

  void maximize() OVERRIDE {
#ifdef WIN32
    ::ShowWindow(win_get_window(), SW_MAXIMIZE);
#endif
  }

  bool isMaximized() const OVERRIDE {
#ifdef WIN32
    return (::GetWindowLong(win_get_window(), GWL_STYLE) & WS_MAXIMIZE ? true: false);
#else
    return false;
#endif
  }

  EventQueue* getEventQueue() OVERRIDE {
    return m_queue;
  }

  void* nativeHandle() OVERRIDE {
#ifdef WIN32
    return reinterpret_cast<void*>(win_get_window());
#else
    return NULL;
#endif
  }

private:
  Surface* m_surface;
  int m_scale;
  Alleg4EventQueue* m_queue;
};

class Alleg4System : public System {
public:
  Alleg4System() {
    allegro_init();
    set_uformat(U_UTF8);
    _al_detect_filename_encoding();
    install_timer();

    // Register PNG as a supported bitmap type
    register_bitmap_file_type("png", load_png, save_png);
  }

  ~Alleg4System() {
    remove_timer();
    allegro_exit();
  }

  void dispose() {
    delete this;
  }

  Capabilities capabilities() const {
    return kCanResizeDisplayCapability;
  }

  Display* createDisplay(int width, int height, int scale) {
    return new Alleg4Display(width, height, scale);
  }

  Surface* createSurface(int width, int height) {
    return new Alleg4Surface(width, height);
  }

  Surface* createSurfaceFromNativeHandle(void* nativeHandle) {
    return new Alleg4Surface(reinterpret_cast<BITMAP*>(nativeHandle),
                             Alleg4Surface::AutoDestroy);
  }

};

static System* g_instance;

System* CreateSystem() {
  return g_instance = new Alleg4System();
}

System* Instance()
{
  return g_instance;
}

}

// It must be defined by the user program code.
extern int app_main(int argc, char* argv[]);

int main(int argc, char* argv[]) {
  return app_main(argc, argv);
}

END_OF_MAIN();
