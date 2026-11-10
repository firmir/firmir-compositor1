#pragma once

// firmir-compositor1 window manager logic, exposed to the Firml UI
// (compositor1.fml).
//
// The app is a Wayland compositor running as a client of an outer
// compositor: it embeds other Wayland clients (e.g. firmir apps launched
// with WAYLAND_DISPLAY=<socketName>) into its own window. Every client
// toplevel gets a decorated frame (title bar with minimize/maximize/
// fullscreen/close); the taskbar (a ListModel + Repeater in FML) lists all
// windows and restores minimized ones on click.

#include <firmir/object.hpp>
#include <firmir/timer.hpp>

#include <memory>
#include <string>
#include <vector>

namespace Firmir {
namespace Gui {
class Item;
class ListModel;
class Window;
} // namespace Gui
namespace Wayland {
namespace Compositor {
class WaylandCompositor;
class WaylandSeat;
class WaylandXdgShell;
class WaylandWlShell;
} // namespace Compositor
} // namespace Wayland
} // namespace Firmir

class CompositorApp : public Firmir::Object {
public:
    explicit CompositorApp(Object* parent = nullptr);
    ~CompositorApp() override;

    /// Creates the Wayland server and wires the FML-created views.
    /// Must be called once, on the GUI thread, after the window is shown.
    bool attach(Firmir::Gui::Window* root, Firmir::Gui::Item* desktop,
                Firmir::Gui::ListModel* taskbarModel);

    // ---- called from the Firml handlers ----
    /// Taskbar click: restores a minimized window, minimizes the active one,
    /// raises/focuses any other.
    void activateTask(double index);

    /// "WAYLAND_DISPLAY=<socket path>" of the hosted compositor (what the
    /// clients have to export); empty before attach().
    std::string socketDisplay() const { return m_socketDisplay; }

private:
    struct Hosted; // one embedded client window

    /// Height of a hosted window's title bar in pixels, from the density of the
    /// window's own screen: 9 mm, i.e. a finger-sized target on a touch panel
    /// and the old 28 px on a 96 ppi display.
    float titleBarHeight() const;
    /// Pixels per millimetre on the window's screen (96 ppi when the backend
    /// does not report the panel size); every decoration size is in mm.
    float pixelsPerMillimetre() const;
    /// Centre of the taskbar's first cell (the fake-input hooks click it);
    /// mirrors the metrics in compositor1.fml.
    struct TaskbarTarget {
        float x = 0.0f;
        float y = 0.0f;
    };
    TaskbarTarget taskbarButtonCentre() const;
    /// Re-applies titleBarHeight() to the decorations and the hosted clients
    /// (called when the window moves to a screen with a different density).
    void updateMetrics();

    void tick();              // server dispatch + state sync (timer)
    void discoverSurfaces();  // adopts newly created toplevels
    void syncHosted();        // title/state/size polling per hosted window
    void sweepDead();         // destroys frames of dead clients
    void rebuildTaskbar();

    void addHosted(Hosted* hosted); // takes ownership; builds the frame
    void activate(Hosted* hosted);
    void minimize(Hosted* hosted);
    void restore(Hosted* hosted);
    void configureHosted(Hosted* hosted, int width, int height);
    void toggleMaximized(Hosted* hosted);
    void toggleFullscreen(Hosted* hosted);
    void close(Hosted* hosted);

    Firmir::Gui::Window* m_root = nullptr;       // not owned
    Firmir::Gui::Item* m_desktop = nullptr;      // not owned
    Firmir::Gui::ListModel* m_model = nullptr;   // not owned
    std::string m_socketDisplay;                 // WAYLAND_DISPLAY=<socket>

    std::unique_ptr<Firmir::Wayland::Compositor::WaylandCompositor> m_compositor;
    Firmir::Wayland::Compositor::WaylandXdgShell* m_xdgShell = nullptr;  // child
    Firmir::Wayland::Compositor::WaylandWlShell* m_wlShell = nullptr;    // child
    Firmir::Wayland::Compositor::WaylandSeat* m_seat = nullptr;          // child

    std::vector<std::unique_ptr<Hosted>> m_hosted;
    Hosted* m_active = nullptr;
    float m_zCounter = 1.0f;
    /// Current title bar height (see titleBarHeight()); updated by
    /// updateMetrics() when the window's screen density changes.
    float m_barHeight = 28.0f;
    Firmir::Timer m_tickTimer{this};
};
