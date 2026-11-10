// firmir-compositor1: a Wayland compositor embedded into a firmir-gui
// window. It connects to an outer compositor (e.g. WSLg) as a regular
// client, and hosts other Wayland clients (any firmir app) inside its own
// UI, with window decorations, a taskbar and open/minimize/maximize
// animations.
//
// Usage:
//   firmir-compositor1            # prints WAYLAND_DISPLAY=<socket>
//   WAYLAND_DISPLAY=<socket> firmir-weather    # in another terminal
//
// Backend selection for the compositor's own window works like in every
// firmir-gui app (FIRMIR_GUI_DISPLAY / FIRMIR_GUI_PLUGIN_PATH).

#include "compositor1.hpp"

#include "CompositorApp.hpp"

#include <firmir/gui/platform.hpp>
#include <firmir/gui/screen.hpp>
#include <firmir/gui/window.hpp>
#include <firmir/log.hpp>
#include <firmir/thread.hpp>

#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <string>

namespace {
// The headless backend ships no default screen; add one when it is in use
// (test/CI runs). FIRMIR_COMPOSITOR1_SCREEN=1200x2000 and
// FIRMIR_COMPOSITOR1_SCREEN_MM=100x167 give it the size and the panel size of
// the target device, so the density path (hosted clients receive both through
// wl_output) can be exercised without hardware.
void ensureHeadlessScreen() {
    const char* display = std::getenv("FIRMIR_GUI_DISPLAY");
    if (!display || std::string(display) != "headless")
        return;
    const char* pluginPath = std::getenv("FIRMIR_GUI_PLUGIN_PATH");
    std::string so = (pluginPath ? std::string(pluginPath) : std::string()) +
                     "/firmir-gui-headless.so";
    void* handle = dlopen(so.c_str(), RTLD_NOW | RTLD_NOLOAD);
    if (!handle)
        handle = dlopen(so.c_str(), RTLD_NOW);
    if (!handle)
        return;
    int width = 1600;
    int height = 900;
    if (const char* size = std::getenv("FIRMIR_COMPOSITOR1_SCREEN")) {
        int w = 0;
        int h = 0;
        if (std::sscanf(size, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            width = w;
            height = h;
        }
    }
    using AddScreen = void (*)(const char*, int, int);
    if (auto add = reinterpret_cast<AddScreen>(
            dlsym(handle, "firmir_gui_headless_add_screen")))
        add("headless", width, height);
    if (const char* mm = std::getenv("FIRMIR_COMPOSITOR1_SCREEN_MM")) {
        int widthMm = 0;
        int heightMm = 0;
        if (std::sscanf(mm, "%dx%d", &widthMm, &heightMm) == 2 && widthMm > 0 &&
            heightMm > 0) {
            using SetPhysicalSize = void (*)(const char*, int, int);
            if (auto set = reinterpret_cast<SetPhysicalSize>(dlsym(
                    handle, "firmir_gui_headless_set_screen_physical_size")))
                set("headless", widthMm, heightMm);
        }
    }
}
} // namespace

int main() {
    auto* platform = Firmir::Gui::Platform::instance();
    ensureHeadlessScreen();
    if (platform->screens().empty()) {
        std::fprintf(stderr, "firmir-compositor1: no screens — check "
                             "FIRMIR_GUI_DISPLAY / FIRMIR_GUI_PLUGIN_PATH\n");
        return 1;
    }

    // Creates the window, the UI tree and the CompositorApp logic object,
    // then shows the window (see compositor1.fml).
    Compositor1Objects objects = createCompositor1(platform->screens().front());
    if (!objects.wm->attach(objects.root, objects.desktop, objects.taskbarModel)) {
        // No compositor socket: take the window down the way a normal exit
        // does instead of leaving the render thread running.
        objects.root->close();
        delete objects.root;
        return 1;
    }

    // The socket name is shown in the taskbar; also print it for scripts:
    //   WAYLAND_DISPLAY=/run/display/wayland-0 firmir-weather
    std::printf("%s\n", objects.wm->socketDisplay().c_str());
    std::fflush(stdout);

    objects.root->connect<bool>(
        objects.root, Firmir::Gui::Window::signalVisibilityChanged,
        [](bool visible) {
            if (!visible)
                Firmir::Thread::mainThread()->quit();
        });

    const int rc = Firmir::Thread::mainThread()->exec();
    Firmir::logInfo() << "firmir-compositor1: bye";
    delete objects.root; // owns the whole tree (including the compositor)
    return rc;
}
