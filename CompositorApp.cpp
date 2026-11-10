#include "CompositorApp.hpp"

#include <firmir/gui/animation.hpp>
#include <firmir/gui/item.hpp>
#include <firmir/gui/listmodel.hpp>
#include <firmir/gui/painter.hpp>
#include <firmir/gui/rectangle.hpp>
#include <firmir/gui/screen.hpp>
#include <firmir/gui/text.hpp>
#include <firmir/gui/window.hpp>
#include <firmir/log.hpp>
#include <firmir/wayland/compositor/compositor.hpp>

#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>

using namespace Firmir;
using namespace Firmir::Wayland::Compositor;

namespace {

/// Title bar height in millimetres: a finger-sized target on a touch panel,
/// close to the old fixed 28 px on a desktop display.
constexpr double kTitleBarMillimetres = 9.0;
/// Pixels per millimetre of a 96 ppi screen, the fallback when the backend
/// does not report the panel size.
constexpr double kFallbackPixelsPerMillimetre = 3.7795;

/// Starts a 4-property geometry animation on @p item; self-destructs when
/// finished.
void animateGeometry(Gui::Item* item, const Gui::RectF& target, int ms = 180) {
    auto* group = new Gui::ParallelAnimation(item);
    const struct {
        const char* property;
        float value;
    } entries[] = {
        {"x", target.x}, {"y", target.y}, {"width", target.width}, {"height", target.height},
    };
    for (const auto& entry : entries) {
        auto* animation = new Gui::NumberAnimation(group);
        animation->setTargetItem(item);
        animation->setTargetProperty(entry.property);
        animation->setTo(entry.value);
        animation->setDuration(std::chrono::milliseconds(ms));
        animation->setEasing(Gui::Easing::InOutQuad);
    }
    group->connect(group, Gui::Animation::signalFinished, [group] { group->deleteLater(); });
    group->start();
}

/// Starts a fade/scale animation on @p item; self-destructs when finished.
void animatePop(Gui::Item* item, float fromOpacity, float toOpacity, float fromScale,
                float toScale, int ms = 170) {
    auto* group = new Gui::ParallelAnimation(item);
    auto* opacity = new Gui::NumberAnimation(group);
    opacity->setTargetItem(item);
    opacity->setTargetProperty("opacity");
    opacity->setFrom(fromOpacity);
    opacity->setTo(toOpacity);
    opacity->setDuration(std::chrono::milliseconds(ms));
    opacity->setEasing(Gui::Easing::OutCubic);
    auto* scale = new Gui::NumberAnimation(group);
    scale->setTargetItem(item);
    scale->setTargetProperty("scale");
    scale->setFrom(fromScale);
    scale->setTo(toScale);
    scale->setDuration(std::chrono::milliseconds(ms));
    scale->setEasing(Gui::Easing::OutCubic);
    group->connect(group, Gui::Animation::signalFinished, [group] { group->deleteLater(); });
    group->start();
}

/// Handle of the headless GUI backend (the FAKE_* hooks reach the test input
/// API through it); nullptr when that backend is not in use.
void* headlessPlugin() {
    const char* pluginPath = std::getenv("FIRMIR_GUI_PLUGIN_PATH");
    const std::string so = (pluginPath ? std::string(pluginPath) : std::string()) +
                           "/firmir-gui-headless.so";
    void* handle = dlopen(so.c_str(), RTLD_NOW | RTLD_NOLOAD);
    if (!handle) handle = dlopen(so.c_str(), RTLD_NOW);
    return handle;
}

/// Debug hooks (FIRMIR_COMPOSITOR1_FAKE_INPUT): forwards a pointer event to
/// the window through the headless backend — the same entry point a real
/// input plugin uses. @p local is item-local and is mapped to the scene, so
/// the item gets the event with its own coordinates back.
void injectMouse(Gui::Item* item, Gui::MouseEvent::Action action, Gui::Point local,
                 int button = 0) {
    using InjectMouse = void (*)(int, float, float, int);
    void* handle = headlessPlugin();
    auto inject = handle ? reinterpret_cast<InjectMouse>(
        dlsym(handle, "firmir_gui_headless_inject_mouse")) : nullptr;
    if (!inject) {
        std::fprintf(stderr, "compositor1: no headless mouse injection\n");
        return;
    }
    const Gui::Point scene = item ? item->mapToScene(local) : local;
    inject(static_cast<int>(action), scene.x, scene.y, button);
}

/// Same for a touch contact: @p action is a Gui::TouchEvent::Action value.
void injectTouch(Gui::Item* item, Gui::TouchEvent::Action action, int id,
                 Gui::Point local) {
    using InjectTouch = void (*)(int, int, float, float);
    void* handle = headlessPlugin();
    auto inject = handle ? reinterpret_cast<InjectTouch>(
        dlsym(handle, "firmir_gui_headless_inject_touch")) : nullptr;
    if (!inject) {
        std::fprintf(stderr, "compositor1: no headless touch injection\n");
        return;
    }
    const Gui::Point scene = item ? item->mapToScene(local) : local;
    inject(static_cast<int>(action), id, scene.x, scene.y);
}

} // namespace
namespace {

/// Converts a touch contact into the equivalent mouse event (the button and
/// drag handlers below are written for the mouse; a finger press reports
/// button 1, like MouseArea/AbstractButton do).
Gui::MouseEvent toMouseEvent(const Gui::TouchEvent& event) {
    Gui::MouseEvent mouse;
    mouse.action = event.action == Gui::TouchEvent::Action::Begin
                       ? Gui::MouseEvent::Action::Press
                   : event.action == Gui::TouchEvent::Action::End
                       ? Gui::MouseEvent::Action::Release
                       : Gui::MouseEvent::Action::Move;
    mouse.button = 1;
    mouse.pos = event.points.empty() ? Gui::Point{} : event.points.front().pos;
    return mouse;
}

/// Window title bar: a Rectangle that drags its parent frame.
class TitleBar : public Gui::Rectangle {
public:
    explicit TitleBar(Object* parent = nullptr) : Gui::Rectangle(parent) {}

    std::function<void(float dx, float dy)> onDrag;
    std::function<void()> onPressed;

protected:
    void mouseEvent(Gui::MouseEvent& event) override {
        if (event.action == Gui::MouseEvent::Action::Press && event.button == 1) {
            m_dragging = true;
            m_last = mapToScene(event.pos);
            event.accepted = true;
            if (onPressed) onPressed();
        } else if (event.action == Gui::MouseEvent::Action::Move && m_dragging) {
            const Gui::Point scene = mapToScene(event.pos);
            if (onDrag) onDrag(scene.x - m_last.x, scene.y - m_last.y);
            m_last = scene;
            event.accepted = true;
        } else if (event.action == Gui::MouseEvent::Action::Release && m_dragging) {
            m_dragging = false;
            event.accepted = true;
        }
    }

    void touchEvent(Gui::TouchEvent& event) override {
        Gui::MouseEvent mouse = toMouseEvent(event);
        mouseEvent(mouse);
        event.accepted = mouse.accepted;
    }

private:
    bool m_dragging = false;
    Gui::Point m_last;
};

/// Clickable title-bar button (a Text glyph with its own hit area).
class TitleButton : public Gui::Text {
public:
    explicit TitleButton(Object* parent = nullptr) : Gui::Text(parent) {}

    std::function<void()> onClick;

protected:
    void mouseEvent(Gui::MouseEvent& event) override {
        if (event.action == Gui::MouseEvent::Action::Press) {
            event.accepted = true;
        } else if (event.action == Gui::MouseEvent::Action::Release && event.button == 1) {
            event.accepted = true;
            if (event.pos.x >= 0 && event.pos.y >= 0 && event.pos.x < width() &&
                event.pos.y < height() && onClick)
                onClick();
        }
    }

    void touchEvent(Gui::TouchEvent& event) override {
        Gui::MouseEvent mouse = toMouseEvent(event);
        mouseEvent(mouse);
        event.accepted = mouse.accepted;
    }
};

} // namespace

// ---------------------------------------------------------------------------

enum class WindowState { Normal, Maximized, Fullscreen, Minimized };

struct CompositorApp::Hosted {
    WaylandShellSurface* shellSurface = nullptr; // not owned (dies with client)
    WaylandXdgToplevel* toplevel = nullptr;      // same object for xdg clients
    WaylandSurface* surface = nullptr;           // not owned
    WaylandView* view = nullptr;                 // child of CompositorApp
    Gui::Item* frame = nullptr;                  // child of the desktop item
    TitleBar* titleBar = nullptr;                // child of frame
    Gui::Text* titleText = nullptr;              // child of titleBar
    std::vector<TitleButton*> buttons;           // children of titleBar
    WaylandGuiItem* surfaceItem = nullptr;       // child of frame
    WindowState state = WindowState::Normal;
    WindowState stateBeforeMinimize = WindowState::Normal;
    Gui::RectF normalGeometry;                   // frame geometry when Normal
    Size bufferSize;                             // last seen buffer size
    std::string title;
    // Running minimize fade (child of frame); tracked so restore() and
    // sweepDead() can stop it — its completion handler hides the frame,
    // which must not fire after a restore or after Hosted is freed.
    Gui::ParallelAnimation* minimizeAnim = nullptr;
    bool dead = false;
};

CompositorApp::CompositorApp(Object* parent) : Object(parent) {
    // The FML bindings read these at creation time, before attach() runs.
    setProperty("socketName", String(""));
    setProperty("windowCount", 0.0);
}
CompositorApp::~CompositorApp() = default;

bool CompositorApp::attach(Gui::Window* root, Gui::Item* desktop,
                           Gui::ListModel* taskbarModel) {
    m_root = root;
    m_desktop = desktop;
    m_model = taskbarModel;
    if (!m_root || !m_desktop || !m_model) return false;

    // The decorations are sized from the density of the window's own screen
    // (9 mm title bars); a screen change re-applies them to every hosted window.
    m_barHeight = titleBarHeight();
    m_root->connect<Gui::Screen*>(this, Gui::Window::signalScreenChanged,
                                  [this](Gui::Screen*) { updateMetrics(); });

    m_compositor = std::make_unique<WaylandCompositor>();
    if (!m_compositor->create()) {
        logError() << "compositor1: WaylandCompositor::create failed";
        return false;
    }
    WaylandOutput* output = m_compositor->addOutput(
        {m_root->geometry().width, m_root->geometry().height});
    output->setManufacturer("Firmir");
    output->setModel("compositor1");
    // The panel size reaches hosted clients through wl_output.geometry; without
    // it they fall back to 96 ppi and lay out a dense panel far too small.
    // Read once at startup: a client gets the geometry when it binds the
    // output, so a later screen change only reaches clients started afterwards.
    if (Gui::Screen* screen = m_root->screen())
        output->setPhysicalSize(screen->physicalSize());

    m_xdgShell = new WaylandXdgShell(m_compositor.get());
    m_xdgShell->initialize(m_compositor.get());
    m_wlShell = new WaylandWlShell(m_compositor.get());
    m_wlShell->initialize(m_compositor.get());
    m_seat = new WaylandSeat(m_compositor.get());
    m_seat->setCapabilities(WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD |
                            WL_SEAT_CAPABILITY_TOUCH);
    m_seat->initialize(m_compositor.get());
    auto* viewporter = new WaylandViewporter(m_compositor.get());
    viewporter->initialize(m_compositor.get());
    auto* decoration = new WaylandXdgDecorationManagerV1(m_compositor.get());
    decoration->setDefaultMode(WaylandXdgDecorationManagerV1::Mode::ClientSide);
    decoration->initialize(m_compositor.get());
    auto* outputs = new WaylandXdgOutputManagerV1(m_compositor.get());
    outputs->initialize(m_compositor.get());
    // The library compiles the default xkb keymap (XKB_DEFAULT_* from the
    // environment) and enables key repeat: compositors need no xkbcommon.
    if (!m_seat->keyboard()->setDefaultKeymap())
        logWarning() << "compositor1: no xkb keymap (xkb-data installed?)";

    const std::string socket = m_compositor->addSocketAuto();
    if (socket.empty()) {
        logError() << "compositor1: cannot create the Wayland socket "
                      "(set FIRMIR_WAYLAND_SOCKET or FIRMIR_WAYLAND_SOCKET_DIR)";
        return false;
    }
    m_socketDisplay = "WAYLAND_DISPLAY=" + socket;
    setProperty("socketName", String(m_socketDisplay.c_str()));
    logInfo() << "compositor1: listening on" << socket;

    m_tickTimer.setInterval(std::chrono::milliseconds(2));
    m_tickTimer.connect(this, "timeout", [this] { tick(); });
    m_tickTimer.start();
    return true;
}

float CompositorApp::pixelsPerMillimetre() const {
    // Window::ppcm() is pixels per centimetre, so a tenth of it is what one
    // millimetre of layout costs.
    const double ppcm = m_root ? m_root->ppcm() : 0.0;
    return static_cast<float>(ppcm > 0.0 ? ppcm / 10.0
                                         : kFallbackPixelsPerMillimetre);
}

float CompositorApp::titleBarHeight() const {
    return static_cast<float>(kTitleBarMillimetres * pixelsPerMillimetre());
}

CompositorApp::TaskbarTarget CompositorApp::taskbarButtonCentre() const {
    // compositor1.fml: an 11 mm taskbar, right-anchored cells of 45x9 mm with
    // 1.5 mm margins and spacing; with one hosted window the single cell is
    // both the first and the last one.
    const float pxPerMm = pixelsPerMillimetre();
    const float width = m_root ? static_cast<float>(m_root->geometry().width) : 0.0f;
    const float height = m_root ? static_cast<float>(m_root->geometry().height) : 0.0f;
    return {width - 1.5f * pxPerMm - 22.5f * pxPerMm,
            height - 5.5f * pxPerMm};
}

void CompositorApp::updateMetrics() {
    // No screen (unplugged): the decorations are meaningless and the window is
    // hidden anyway; keep the last metrics for when it comes back.
    if (!m_root || !m_root->screen())
        return;
    const float bar = titleBarHeight();
    if (std::abs(bar - m_barHeight) < 0.5f)
        return;
    const float delta = bar - m_barHeight;
    const int fontSize = static_cast<int>(std::lround(bar * 0.46));
    const float textMargin = bar * 0.3f;
    m_barHeight = bar;

    for (const auto& entry : m_hosted) {
        Hosted* hosted = entry.get();
        if (hosted->dead || !hosted->frame)
            continue;
        if (hosted->titleBar)
            hosted->titleBar->setHeight(bar);
        if (hosted->titleText) {
            hosted->titleText->setFontSize(fontSize);
            hosted->titleText->setLeftMargin(textMargin);
        }
        for (TitleButton* button : hosted->buttons) {
            button->setWidth(bar);
            button->setHeight(bar);
            button->setFontSize(fontSize);
        }
        if (hosted->state == WindowState::Fullscreen) {
            // No title bar: the client keeps the whole frame.
            if (hosted->surfaceItem)
                hosted->surfaceItem->setTopMargin(0.0f);
            continue;
        }
        if (hosted->surfaceItem)
            hosted->surfaceItem->setTopMargin(bar);
        if (hosted->state == WindowState::Normal) {
            // Keep the client area as it is and grow/shrink the frame by the
            // change of the title bar.
            hosted->normalGeometry.height += delta;
            hosted->frame->setGeometry(hosted->normalGeometry.x,
                                       hosted->normalGeometry.y,
                                       hosted->normalGeometry.width,
                                       hosted->normalGeometry.height);
            if (hosted->toplevel)
                configureHosted(
                    hosted, static_cast<int>(hosted->normalGeometry.width),
                    static_cast<int>(hosted->normalGeometry.height - bar));
        } else if (hosted->state == WindowState::Maximized) {
            hosted->frame->setGeometry(0, 0, m_desktop->width(),
                                       m_desktop->height());
            if (hosted->toplevel)
                configureHosted(hosted, static_cast<int>(m_desktop->width()),
                                static_cast<int>(m_desktop->height() - bar));
        }
    }
}

void CompositorApp::tick() {
    m_compositor->dispatch(0);
    m_compositor->display()->flushClients();
    discoverSurfaces();
    syncHosted();
    sweepDead();
}

void CompositorApp::discoverSurfaces() {
    for (WaylandShellSurface* candidate : m_xdgShell->shellSurfaces()) {
        // Only toplevel windows are hosted; skip xdg_surface wrappers and
        // popups (also registered in shellSurfaces()).
        auto* toplevel = dynamic_cast<WaylandXdgToplevel*>(candidate);
        auto* legacy = dynamic_cast<WaylandWlShellSurface*>(candidate);
        if (!toplevel && !legacy) continue;
        const bool known = std::any_of(m_hosted.begin(), m_hosted.end(),
            [candidate](const std::unique_ptr<Hosted>& h) {
                return h->shellSurface == candidate;
            });
        if (known) continue;

        auto hosted = std::make_unique<Hosted>();
        hosted->shellSurface = candidate;
        hosted->toplevel = toplevel;
        hosted->surface = candidate->surface();
        hosted->title = candidate->title();
        Hosted* raw = hosted.get();
        m_hosted.push_back(std::move(hosted));

        if (!raw->surface) {
            raw->dead = true;
            continue;
        }
        // The library deletes these objects when the client destroys the
        // resources (or disconnects); sweepDead() removes the frame.
        candidate->connect(this, Object::signalDestroyed, [raw] { raw->dead = true; });
        raw->surface->connect(this, WaylandSurface::signalAboutToBeDestroyed, [raw] {
            raw->dead = true;
            if (raw->surfaceItem) raw->surfaceItem->detachSurface();
            raw->surface = nullptr;
        });
        addHosted(raw);
    }

    // Debug hook: FIRMIR_COMPOSITOR1_FAKE_INPUT=1 synthesizes a click at the
    // center of the first hosted window (enter+press, release 500ms later),
    // exercising the full WaylandGuiItem -> seat -> client path without a
    // real mouse.
    static const char* const fakeInputEnv = std::getenv("FIRMIR_COMPOSITOR1_FAKE_INPUT");
    static const int fakeInput = fakeInputEnv ? std::atoi(fakeInputEnv) : 0;
    if (fakeInput && !m_hosted.empty()) {
        Hosted* hosted = m_hosted.front().get();
        if (hosted->surfaceItem && !hosted->dead &&
            !hosted->surfaceItem->property("fakeInputDone").has_value()) {
            hosted->surfaceItem->setProperty("fakeInputDone", true);
            WaylandGuiItem* item = hosted->surfaceItem;
            if (fakeInput == 7) {
                // Click the first task row's checkbox (hosted firmir-tasks):
                // press+release at (29,64) in surface coordinates.
                Timer::singleShot(std::chrono::milliseconds(3500), this, [this, item] {
                    const Gui::Point point = {29.0f, 108.0f};
                    injectMouse(item, Gui::MouseEvent::Action::Move, point);
                    injectMouse(item, Gui::MouseEvent::Action::Press, point, 1);
                    Timer::singleShot(std::chrono::milliseconds(150), this, [item, point] {
                        injectMouse(item, Gui::MouseEvent::Action::Release, point, 1);
                        std::fprintf(stderr, "compositor1: FAKE7 checkbox clicked\n");
                    });
                });
                return;
            }
            if (fakeInput == 6) {
                // Implicit grab: press inside, drag far outside, release
                // there — the release must still reach the client.
                Timer::singleShot(std::chrono::milliseconds(3000), this, [this, item] {
                    const Gui::Point in{item->width() / 2, item->height() / 2};
                    injectMouse(item, Gui::MouseEvent::Action::Move, in);
                    injectMouse(item, Gui::MouseEvent::Action::Press, in, 1);
                    std::fprintf(stderr, "compositor1: FAKE6 press sent\n");
                    Timer::singleShot(std::chrono::milliseconds(400), this, [item] {
                        const Gui::Point out{-50.0f, -50.0f};
                        injectMouse(item, Gui::MouseEvent::Action::Move, out);
                        injectMouse(item, Gui::MouseEvent::Action::Release, out, 1);
                        std::fprintf(stderr, "compositor1: FAKE6 outside release sent\n");
                    });
                });
                return;
            }
            if (fakeInput == 8) {
                // Rapid minimize/restore cycling, faster than the minimize
                // animation (160ms) — regression hook for the crash on
                // repeated minimize/restore.
                Timer::singleShot(std::chrono::milliseconds(3000), this, [this, hosted] {
                    auto* step = new Timer(this);
                    step->setInterval(std::chrono::milliseconds(120));
                    auto count = std::make_shared<int>(0);
                    step->connect(step, "timeout", [this, hosted, count, step] {
                        if (++*count > 40 || hosted->dead) {
                            step->stop();
                            step->deleteLater();
                            std::fprintf(stderr, "compositor1: FAKE8 done\n");
                            return;
                        }
                        if (hosted->state == WindowState::Minimized) {
                            restore(hosted);
                            activate(hosted);
                        } else {
                            minimize(hosted);
                        }
                    });
                    step->start();
                    std::fprintf(stderr, "compositor1: FAKE8 cycling started\n");
                });
                return;
            }
            if (fakeInput == 9) {
                // Minimize/restore through the taskbar button (the FML
                // delegate onClicked path): click the first taskbar cell
                // every 300ms.
                Timer::singleShot(std::chrono::milliseconds(3000), this, [this] {
                    const char* pluginPath = std::getenv("FIRMIR_GUI_PLUGIN_PATH");
                    std::string so = (pluginPath ? std::string(pluginPath) : std::string()) +
                                     "/firmir-gui-headless.so";
                    void* handle = dlopen(so.c_str(), RTLD_NOW | RTLD_NOLOAD);
                    if (!handle) handle = dlopen(so.c_str(), RTLD_NOW);
                    using InjectMouse = void (*)(int, float, float, int);
                    auto inject = handle ? reinterpret_cast<InjectMouse>(
                        dlsym(handle, "firmir_gui_headless_inject_mouse")) : nullptr;
                    if (!inject) return;
                    auto* step = new Timer(this);
                    step->setInterval(std::chrono::milliseconds(300));
                    auto count = std::make_shared<int>(0);
                    step->connect(step, "timeout", [this, inject, count, step] {
                        if (++*count > 30) {
                            step->stop();
                            step->deleteLater();
                            std::fprintf(stderr, "compositor1: FAKE9 done\n");
                            return;
                        }
                        const TaskbarTarget target = taskbarButtonCentre();
                        const float x = target.x;
                        const float y = target.y;
                        inject(0 /*Move*/, x, y, 0);
                        inject(1 /*Press*/, x, y, 1);
                        inject(2 /*Release*/, x, y, 1);
                    });
                    step->start();
                    std::fprintf(stderr, "compositor1: FAKE9 taskbar cycling started\n");
                });
                return;
            }
            if (fakeInput == 14) {
                // Fullscreen escape hatch: go fullscreen, then click the
                // window's taskbar button — it must come back to a normal
                // window (a fullscreen window has no title bar to click).
                Timer::singleShot(std::chrono::milliseconds(3000), this, [this, hosted] {
                    toggleFullscreen(hosted);
                    std::fprintf(stderr, "compositor1: FAKE14 fullscreen=%d\n",
                                 hosted->state == WindowState::Fullscreen);
                    Timer::singleShot(std::chrono::milliseconds(1500), this, [this, hosted] {
                        const char* pluginPath = std::getenv("FIRMIR_GUI_PLUGIN_PATH");
                        std::string so = (pluginPath ? std::string(pluginPath) : std::string()) +
                                         "/firmir-gui-headless.so";
                        void* handle = dlopen(so.c_str(), RTLD_NOW | RTLD_NOLOAD);
                        if (!handle) handle = dlopen(so.c_str(), RTLD_NOW);
                        using InjectMouse = void (*)(int, float, float, int);
                        auto inject = handle ? reinterpret_cast<InjectMouse>(
                            dlsym(handle, "firmir_gui_headless_inject_mouse")) : nullptr;
                        if (!inject) return;
                        const TaskbarTarget target = taskbarButtonCentre();
                        inject(0 /*Move*/, target.x, target.y, 0);
                        inject(1 /*Press*/, target.x, target.y, 1);
                        inject(2 /*Release*/, target.x, target.y, 1);
                        std::fprintf(stderr, "compositor1: FAKE14 taskbar clicked\n");
                        Timer::singleShot(std::chrono::milliseconds(700), this, [hosted] {
                            std::fprintf(stderr,
                                         "compositor1: FAKE14 fullscreen_now=%d normal=%d\n",
                                         hosted->state == WindowState::Fullscreen,
                                         hosted->state == WindowState::Normal);
                        });
                    });
                });
                return;
            }
            if (fakeInput == 15) {
                // Client-requested minimize (the app's own "_" button sends
                // xdg_toplevel.set_minimized), then restore from the taskbar.
                Timer::singleShot(std::chrono::milliseconds(3000), this, [this, hosted] {
                    if (hosted->toplevel)
                        hosted->toplevel->setMinimized();
                    std::fprintf(stderr, "compositor1: FAKE15 minimize requested\n");
                    Timer::singleShot(std::chrono::milliseconds(1500), this, [this, hosted] {
                        std::fprintf(stderr, "compositor1: FAKE15 minimized=%d\n",
                                     hosted->state == WindowState::Minimized);
                        const char* pluginPath = std::getenv("FIRMIR_GUI_PLUGIN_PATH");
                        std::string so = (pluginPath ? std::string(pluginPath) : std::string()) +
                                         "/firmir-gui-headless.so";
                        void* handle = dlopen(so.c_str(), RTLD_NOW | RTLD_NOLOAD);
                        if (!handle) handle = dlopen(so.c_str(), RTLD_NOW);
                        using InjectMouse = void (*)(int, float, float, int);
                        auto inject = handle ? reinterpret_cast<InjectMouse>(
                            dlsym(handle, "firmir_gui_headless_inject_mouse")) : nullptr;
                        if (!inject) return;
                        const TaskbarTarget target = taskbarButtonCentre();
                        inject(0 /*Move*/, target.x, target.y, 0);
                        inject(1 /*Press*/, target.x, target.y, 1);
                        inject(2 /*Release*/, target.x, target.y, 1);
                        std::fprintf(stderr, "compositor1: FAKE15 taskbar clicked\n");
                        Timer::singleShot(std::chrono::milliseconds(700), this, [hosted] {
                            std::fprintf(stderr,
                                         "compositor1: FAKE15 minimized_now=%d normal=%d\n",
                                         hosted->state == WindowState::Minimized,
                                         hosted->state == WindowState::Normal);
                        });
                    });
                });
                return;
            }
            if (fakeInput == 10) {
                // EditableText selection e2e: click the tasks input, type
                // "hello", Shift+Left x3, type "p", Enter -> store gains
                // "hep" (the selected "llo" is replaced).
                Timer::singleShot(std::chrono::milliseconds(3000), this, [this, item] {
                    const char* pluginPath = std::getenv("FIRMIR_GUI_PLUGIN_PATH");
                    std::string so = (pluginPath ? std::string(pluginPath) : std::string()) +
                                     "/firmir-gui-headless.so";
                    void* handle = dlopen(so.c_str(), RTLD_NOW | RTLD_NOLOAD);
                    if (!handle) handle = dlopen(so.c_str(), RTLD_NOW);
                    using InjectKey = void (*)(bool, int, const char*);
                    auto injectKey = handle ? reinterpret_cast<InjectKey>(
                        dlsym(handle, "firmir_gui_headless_inject_key")) : nullptr;
                    if (!injectKey) return;
                    // Click the input field (surface-local coords; see fake7).
                    const Gui::Point field = {200.0f, 69.0f};
                    injectMouse(item, Gui::MouseEvent::Action::Move, field);
                    injectMouse(item, Gui::MouseEvent::Action::Press, field, 1);
                    injectMouse(item, Gui::MouseEvent::Action::Release, field, 1);
                    const struct { int key; const char* text; } keys[] = {
                        {35, "h"}, {18, "e"}, {38, "l"}, {38, "l"}, {24, "o"},
                    };
                    for (const auto& k : keys) {
                        injectKey(true, k.key, k.text);
                        injectKey(false, k.key, "");
                    }
                    injectKey(true, 42, "");   // Shift down
                    for (int i = 0; i < 3; ++i) {
                        injectKey(true, 105, "");  // Left
                        injectKey(false, 105, "");
                    }
                    injectKey(false, 42, "");  // Shift up
                    injectKey(true, 25, "p");  // replaces the "llo" selection
                    injectKey(false, 25, "");
                    injectKey(true, 28, "");   // Enter -> accepted
                    injectKey(false, 28, "");
                    std::fprintf(stderr, "compositor1: FAKE10 selection typed\n");
                });
                return;
            }
            if (fakeInput == 11) {
                // Full-path pointer routing: click the hosted tasks input
                // through the compositor's own input dispatch (not the
                // helper above), then type "hi" + Enter.
                Timer::singleShot(std::chrono::milliseconds(3000), this, [this] {
                    const char* pluginPath = std::getenv("FIRMIR_GUI_PLUGIN_PATH");
                    std::string so = (pluginPath ? std::string(pluginPath) : std::string()) +
                                     "/firmir-gui-headless.so";
                    void* handle = dlopen(so.c_str(), RTLD_NOW | RTLD_NOLOAD);
                    if (!handle) handle = dlopen(so.c_str(), RTLD_NOW);
                    using InjectMouse = void (*)(int, float, float, int);
                    using InjectKey = void (*)(bool, int, const char*);
                    auto inject = handle ? reinterpret_cast<InjectMouse>(
                        dlsym(handle, "firmir_gui_headless_inject_mouse")) : nullptr;
                    auto injectKey = handle ? reinterpret_cast<InjectKey>(
                        dlsym(handle, "firmir_gui_headless_inject_key")) : nullptr;
                    if (!inject || !injectKey) return;
                    // Frame at cascade (70,70) + 28px title bar; the tasks
                    // input sits at surface-local (200,69).
                    const float x = 70.0f + 200.0f;
                    const float y = 70.0f + 28.0f + 69.0f;
                    inject(0 /*Move*/, x, y, 0);
                    inject(1 /*Press*/, x, y, 1);
                    inject(2 /*Release*/, x, y, 1);
                    std::fprintf(stderr, "compositor1: FAKE11 input clicked\n");
                    injectKey(true, 35, "h");
                    injectKey(false, 35, "");
                    injectKey(true, 23, "i");
                    injectKey(false, 23, "");
                    injectKey(true, 28, "");
                    injectKey(false, 28, "");
                    std::fprintf(stderr, "compositor1: FAKE11 typed\n");
                });
                return;
            }
            if (fakeInput == 4) {
                // Maximize/restore: the client must receive xdg configures
                // (toplevel + xdg_surface) and resize its content.
                Timer::singleShot(std::chrono::milliseconds(3000), this, [this, hosted] {
                    toggleMaximized(hosted);
                    std::fprintf(stderr, "compositor1: FAKE4 maximized\n");
                    Timer::singleShot(std::chrono::milliseconds(2500), this, [this, hosted] {
                        toggleMaximized(hosted);
                        std::fprintf(stderr, "compositor1: FAKE4 restored\n");
                    });
                });
                return;
            }
            if (fakeInput == 5) {
                // Minimize, then click the taskbar button to restore (the
                // FML MouseArea clicked path that used to crash).
                Timer::singleShot(std::chrono::milliseconds(3000), this, [this, hosted] {
                    minimize(hosted);
                    std::fprintf(stderr, "compositor1: FAKE5 minimized\n");
                    Timer::singleShot(std::chrono::milliseconds(2000), this, [this] {
                        const char* pluginPath = std::getenv("FIRMIR_GUI_PLUGIN_PATH");
                        std::string so = (pluginPath ? std::string(pluginPath) : std::string()) +
                                         "/firmir-gui-headless.so";
                        void* handle = dlopen(so.c_str(), RTLD_NOW | RTLD_NOLOAD);
                        if (!handle) handle = dlopen(so.c_str(), RTLD_NOW);
                        using InjectMouse = void (*)(int, float, float, int);
                        auto inject = handle ? reinterpret_cast<InjectMouse>(
                            dlsym(handle, "firmir_gui_headless_inject_mouse")) : nullptr;
                        if (!inject) return;
                        // First taskbar button (the Row is right-anchored;
                        // see taskbarButtonCentre()).
                        const TaskbarTarget target = taskbarButtonCentre();
                        const float x = target.x;
                        const float y = target.y;
                        inject(0 /*Move*/, x, y, 0);
                        inject(1 /*Press*/, x, y, 1);
                        inject(2 /*Release*/, x, y, 1);
                        std::fprintf(stderr, "compositor1: FAKE5 taskbar clicked\n");
                    });
                });
                return;
            }
            if (fakeInput == 3) {
                // Key repeat check: hold Space for 2.2s, then release.
                Timer::singleShot(std::chrono::milliseconds(3000), this, [this, item] {
                    const char* pluginPath = std::getenv("FIRMIR_GUI_PLUGIN_PATH");
                    std::string so = (pluginPath ? std::string(pluginPath) : std::string()) +
                                     "/firmir-gui-headless.so";
                    void* handle = dlopen(so.c_str(), RTLD_NOW | RTLD_NOLOAD);
                    if (!handle) handle = dlopen(so.c_str(), RTLD_NOW);
                    using InjectKey = void (*)(bool, int, const char*);
                    auto injectKey = handle ? reinterpret_cast<InjectKey>(
                        dlsym(handle, "firmir_gui_headless_inject_key")) : nullptr;
                    if (!injectKey) return;
                    item->setFocus(true);
                    injectKey(true, 57 /*KEY_SPACE*/, " ");
                    std::fprintf(stderr, "compositor1: FAKE3 space pressed\n");
                    Timer::singleShot(std::chrono::milliseconds(2200), this, [injectKey] {
                        injectKey(false, 57, "");
                        std::fprintf(stderr, "compositor1: FAKE3 space released\n");
                    });
                });
                return;
            }
            if (fakeInput == 2) {
                // Full-path variant: inject through the headless backend's
                // test API (same entry as a real input plugin).
                Timer::singleShot(std::chrono::milliseconds(3000), this, [this, item] {
                    const char* pluginPath = std::getenv("FIRMIR_GUI_PLUGIN_PATH");
                    std::string so = (pluginPath ? std::string(pluginPath) : std::string()) +
                                     "/firmir-gui-headless.so";
                    void* handle = dlopen(so.c_str(), RTLD_NOW | RTLD_NOLOAD);
                    if (!handle) handle = dlopen(so.c_str(), RTLD_NOW);
                    using InjectMouse = void (*)(int, float, float, int);
                    auto inject = handle ? reinterpret_cast<InjectMouse>(
                        dlsym(handle, "firmir_gui_headless_inject_mouse")) : nullptr;
                    if (!inject) {
                        std::fprintf(stderr, "compositor1: FAKE2 no headless inject (%s)\n",
                                     dlerror());
                        return;
                    }
                    const Gui::Point scene = item->mapToScene(
                        {item->width() / 2, item->height() / 2});
                    inject(0 /*Move*/, scene.x, scene.y, 0);
                    inject(1 /*Press*/, scene.x, scene.y, 1);
                    std::fprintf(stderr, "compositor1: FAKE2 press injected at %.0f,%.0f\n",
                                 scene.x, scene.y);
                    Timer::singleShot(std::chrono::milliseconds(500), this, [inject, scene] {
                        inject(2 /*Release*/, scene.x, scene.y, 1);
                        std::fprintf(stderr, "compositor1: FAKE2 release injected\n");
                    });
                });
                return;
            }
            if (fakeInput == 12) {
                // Touch path: tap the centre of the hosted surface (Begin ->
                // End), exercising WaylandGuiItem::touchEvent -> wl_touch of
                // the client (e.g. a calculator button under the finger).
                Timer::singleShot(std::chrono::milliseconds(3000), this, [this, item] {
                    const Gui::Point center{item->width() / 2, item->height() / 2};
                    injectTouch(item, Gui::TouchEvent::Action::Begin, 0, center);
                    Timer::singleShot(std::chrono::milliseconds(80), this,
                                      [item, center] {
                        injectTouch(item, Gui::TouchEvent::Action::End, 0, center);
                        std::fprintf(stderr, "compositor1: FAKE12 touch tap sent\n");
                    });
                });
                return;
            }
            if (fakeInput == 13) {
                // Touch drag of the title bar: the frame must follow the
                // finger (TitleBar::touchEvent -> the mouse drag handler).
                Timer::singleShot(std::chrono::milliseconds(3000), this,
                                  [this, hosted] {
                    Gui::Item* bar = hosted->titleBar;
                    if (!bar) return;
                    const Gui::Point start{bar->width() - 200.0f, bar->height() / 2};
                    const Gui::Point end{start.x + 120.0f, start.y + 80.0f};
                    std::fprintf(stderr, "compositor1: FAKE13 drag from %.0f,%.0f\n",
                                 hosted->frame->x(), hosted->frame->y());
                    injectTouch(bar, Gui::TouchEvent::Action::Begin, 0, start);
                    Timer::singleShot(std::chrono::milliseconds(60), this,
                                      [this, bar, hosted, end] {
                        injectTouch(bar, Gui::TouchEvent::Action::Update, 0, end);
                        Timer::singleShot(std::chrono::milliseconds(60), this,
                                          [bar, hosted, end] {
                            injectTouch(bar, Gui::TouchEvent::Action::End, 0, end);
                            std::fprintf(stderr,
                                         "compositor1: FAKE13 title drag -> %.0f,%.0f\n",
                                         hosted->frame ? hosted->frame->x() : -1.0f,
                                         hosted->frame ? hosted->frame->y() : -1.0f);
                        });
                    });
                });
                return;
            }
            Timer::singleShot(std::chrono::milliseconds(3000), this, [this, item] {
                const Gui::Point center{item->width() / 2, item->height() / 2};
                injectMouse(item, Gui::MouseEvent::Action::Move, center);
                injectMouse(item, Gui::MouseEvent::Action::Press, center, 1);
                std::fprintf(stderr, "compositor1: FAKE press sent\n");
                Timer::singleShot(std::chrono::milliseconds(500), this, [item, center] {
                    injectMouse(item, Gui::MouseEvent::Action::Release, center, 1);
                    std::fprintf(stderr, "compositor1: FAKE release sent\n");
                });
            });
        }
    }
}

void CompositorApp::addHosted(Hosted* hosted) {
    // Frame: plain item under the desktop, decorated with a title bar.
    const Size bufferSize = hosted->surface->buffer().size();
    const float w = bufferSize.width > 0 ? bufferSize.width : 480;
    const float h = bufferSize.height > 0 ? bufferSize.height : 360;
    const float cascade = 40.0f + 30.0f * static_cast<float>(m_hosted.size() % 6);
    hosted->normalGeometry = {cascade, cascade, w, h + m_barHeight};

    Gui::Item* frame = m_root->createItem<Gui::Item>(m_desktop);
    hosted->frame = frame;
    // Parented to the frame so the view dies together with the item that
    // paints it (frame destruction holds the scene mutex).
    hosted->view = new WaylandView(frame);
    hosted->view->setSurface(hosted->surface);
    if (!m_compositor->outputs().empty())
        hosted->view->setOutput(m_compositor->outputs().front());
    frame->setClip(true);
    frame->setGeometry(hosted->normalGeometry.x, hosted->normalGeometry.y,
                       hosted->normalGeometry.width, hosted->normalGeometry.height);
    frame->setZ(m_zCounter++);
    frame->setTransformOrigin(Gui::TransformOrigin::Center);

    auto* background = m_root->createItem<Gui::Rectangle>(frame);
    background->setColor(Gui::Color::fromString("#000000"));
    background->fill(frame);

    auto* titleBar = m_root->createItem<TitleBar>(frame);
    hosted->titleBar = titleBar;
    titleBar->setHeight(m_barHeight);
    titleBar->setAnchorLeft({frame, Gui::AnchorLine::Left});
    titleBar->setAnchorRight({frame, Gui::AnchorLine::Right});
    titleBar->setAnchorTop({frame, Gui::AnchorLine::Top});
    titleBar->setColor(Gui::Color::fromString("#3A4A66"));
    titleBar->onPressed = [this, hosted] { activate(hosted); };
    titleBar->onDrag = [this, hosted](float dx, float dy) {
        if (hosted->state != WindowState::Normal || !hosted->frame) return;
        hosted->frame->setX(hosted->frame->x() + dx);
        hosted->frame->setY(hosted->frame->y() + dy);
    };

    auto* title = m_root->createItem<Gui::Text>(titleBar);
    hosted->titleText = title;
    title->setText(String(hosted->title.empty() ? "window" : hosted->title.c_str()));
    title->setColor(Gui::Color::fromString("#E6E8EE"));
    title->setFontSize(static_cast<int>(std::lround(m_barHeight * 0.46)));
    title->setAnchorLeft({titleBar, Gui::AnchorLine::Left});
    title->setLeftMargin(m_barHeight * 0.3f);
    title->setAnchorVerticalCenter({titleBar, Gui::AnchorLine::VerticalCenter});

    // Title-bar buttons: minimize / maximize / fullscreen / close.
    TitleButton* previous = nullptr;
    const struct {
        const char* glyph;
        std::function<void(CompositorApp*, Hosted*)> action;
    } buttons[] = {
        {"X", [](CompositorApp* self, Hosted* h) { self->close(h); }},
        {"F", [](CompositorApp* self, Hosted* h) { self->toggleFullscreen(h); }},
        {"M", [](CompositorApp* self, Hosted* h) { self->toggleMaximized(h); }},
        {"_", [](CompositorApp* self, Hosted* h) { self->minimize(h); }},
    };
    for (const auto& spec : buttons) {
        auto* button = m_root->createItem<TitleButton>(titleBar);
        button->setText(String(spec.glyph));
        button->setColor(Gui::Color::fromString("#C9CEDB"));
        button->setFontSize(static_cast<int>(std::lround(m_barHeight * 0.46)));
        button->setWidth(m_barHeight);
        button->setHeight(m_barHeight);
        button->setAnchorVerticalCenter({titleBar, Gui::AnchorLine::VerticalCenter});
        if (previous)
            button->setAnchorRight({previous, Gui::AnchorLine::Left});
        else
            button->setAnchorRight({titleBar, Gui::AnchorLine::Right});
        button->onClick = [this, hosted, action = spec.action] { action(this, hosted); };
        hosted->buttons.push_back(button);
        previous = button;
    }

    auto* surfaceItem = m_root->createItem<WaylandGuiItem>(frame);
    hosted->surfaceItem = surfaceItem;
    // The item forwards pointer/keyboard input to the compositor seat; the
    // click/focus notification raises the frame.
    surfaceItem->setSeat(m_seat);
    surfaceItem->setView(hosted->view);
    surfaceItem->setAnchorLeft({frame, Gui::AnchorLine::Left});
    surfaceItem->setAnchorRight({frame, Gui::AnchorLine::Right});
    surfaceItem->setAnchorTop({frame, Gui::AnchorLine::Top});
    surfaceItem->setAnchorBottom({frame, Gui::AnchorLine::Bottom});
    surfaceItem->setTopMargin(m_barHeight);
    surfaceItem->connect(surfaceItem, WaylandGuiItem::signalActivated,
                         [this, hosted] { activate(hosted); });

    hosted->surface->connect(hosted->surfaceItem, WaylandSurface::signalCommitted,
                             [item = hosted->surfaceItem] { item->update(); });

    animatePop(frame, 0.0f, 1.0f, 0.85f, 1.0f);
    activate(hosted);
    rebuildTaskbar();
}

void CompositorApp::syncHosted() {
    bool taskbarDirty = false;
    for (const auto& entry : m_hosted) {
        Hosted* hosted = entry.get();
        if (hosted->dead) continue;

        // Title changes carry no signal; poll them.
        if (hosted->shellSurface->title() != hosted->title) {
            hosted->title = hosted->shellSurface->title();
            hosted->titleText->setText(
                String(hosted->title.empty() ? "window" : hosted->title.c_str()));
            taskbarDirty = true;
        }

        // Client-side state requests (xdg set_maximized/set_fullscreen/
        // set_minimized set the flags inside the library); converge the WM
        // state to them.
        if (hosted->toplevel) {
            // A client minimize (its own "_" button) reaches the WM only as
            // this one-shot request.
            if (hosted->toplevel->takeMinimizeRequest())
                minimize(hosted);
            const bool wantFullscreen = hosted->toplevel->fullscreen();
            const bool wantMaximized = hosted->toplevel->maximized();
            if (wantFullscreen && hosted->state != WindowState::Fullscreen)
                toggleFullscreen(hosted);
            else if (!wantFullscreen && hosted->state == WindowState::Fullscreen)
                toggleFullscreen(hosted);
            else if (wantMaximized && hosted->state == WindowState::Fullscreen) {
                // The client cannot leave fullscreen by itself (xdg ignores
                // set_maximized while fullscreen), so its maximize button
                // takes it back to a normal maximized window first.
                toggleFullscreen(hosted);
                toggleMaximized(hosted);
            } else if (wantMaximized && hosted->state == WindowState::Normal)
                toggleMaximized(hosted);
            else if (!wantMaximized && hosted->state == WindowState::Maximized)
                toggleMaximized(hosted);
        }

        // In Normal state the frame follows the client's buffer size.
        if (hosted->surface && hosted->state == WindowState::Normal) {
            const Size size = hosted->surface->buffer().size();
            if (size.width > 0 && size.height > 0 &&
                (size.width != hosted->bufferSize.width ||
                 size.height != hosted->bufferSize.height)) {
                const bool first = hosted->bufferSize.width <= 0;
                hosted->bufferSize = size;
                hosted->frame->setWidth(static_cast<float>(size.width));
                hosted->frame->setHeight(static_cast<float>(size.height) + m_barHeight);
                if (first)
                    hosted->normalGeometry = hosted->frame->geometry();
            }
        }
    }
    if (taskbarDirty) rebuildTaskbar();
}

void CompositorApp::sweepDead() {
    bool changed = false;
    for (auto it = m_hosted.begin(); it != m_hosted.end();) {
        Hosted* hosted = it->get();
        if (!hosted->dead) {
            ++it;
            continue;
        }
        // Stop the minimize fade before Hosted is freed: its completion
        // handler dereferences hosted (minimizeAnim = nullptr).
        if (hosted->minimizeAnim) {
            hosted->minimizeAnim->stop();
            hosted->minimizeAnim = nullptr; // stays parented to the frame
        }
        if (m_active == hosted) m_active = nullptr;
        if (hosted->frame) {
            // Fade the frame out, then destroy it.
            Gui::Item* frame = hosted->frame;
            hosted->frame = nullptr;
            frame->setEnabled(false);
            auto* group = new Gui::ParallelAnimation(frame);
            auto* opacity = new Gui::NumberAnimation(group);
            opacity->setTargetItem(frame);
            opacity->setTargetProperty("opacity");
            opacity->setTo(0.0);
            opacity->setDuration(std::chrono::milliseconds(130));
            auto* scale = new Gui::NumberAnimation(group);
            scale->setTargetItem(frame);
            scale->setTargetProperty("scale");
            scale->setTo(0.7);
            scale->setDuration(std::chrono::milliseconds(130));
            group->connect(group, Gui::Animation::signalFinished, [frame, group] {
                group->deleteLater();
                frame->deleteLater();
            });
            group->start();
        }
        logInfo() << "compositor1: window closed:" << hosted->title;
        it = m_hosted.erase(it);
        changed = true;
    }
    if (changed) {
        rebuildTaskbar();
        // Focus the topmost remaining window.
        if (!m_active && !m_hosted.empty())
            activate(m_hosted.back().get());
    }
}

void CompositorApp::rebuildTaskbar() {
    if (!m_model) return;
    m_model->clear();
    for (const auto& entry : m_hosted) {
        const Hosted* hosted = entry.get();
        m_model->append({
            {"title", String(hosted->title.empty() ? "window" : hosted->title.c_str())},
            {"active", hosted == m_active},
            {"minimized", hosted->state == WindowState::Minimized},
        });
    }
    setProperty("windowCount", static_cast<double>(m_hosted.size()));
}

void CompositorApp::activate(Hosted* hosted) {
    if (!hosted || hosted->dead) return;
    if (hosted->state == WindowState::Minimized)
        restore(hosted);
    if (m_active == hosted) return;
    if (m_active && m_active->surface && m_seat)
        m_seat->keyboard()->leave(m_active->surface);
    if (m_active && m_active->titleBar)
        m_active->titleBar->setColor(Gui::Color::fromString("#262A33"));
    m_active = hosted;
    if (m_active->titleBar)
        m_active->titleBar->setColor(Gui::Color::fromString("#3A4A66"));
    if (hosted->state != WindowState::Fullscreen && hosted->frame)
        hosted->frame->setZ(m_zCounter++);
    if (hosted->surface && m_seat)
        m_seat->keyboard()->enter(hosted->surface);
    if (hosted->surfaceItem)
        hosted->surfaceItem->setFocus(true);
    rebuildTaskbar();
}

void CompositorApp::minimize(Hosted* hosted) {
    if (!hosted || hosted->dead || hosted->state == WindowState::Minimized) return;
    hosted->stateBeforeMinimize = hosted->state;
    hosted->state = WindowState::Minimized;
    Gui::Item* frame = hosted->frame;
    auto* group = new Gui::ParallelAnimation(frame);
    hosted->minimizeAnim = group;
    auto* opacity = new Gui::NumberAnimation(group);
    opacity->setTargetItem(frame);
    opacity->setTargetProperty("opacity");
    opacity->setTo(0.0);
    opacity->setDuration(std::chrono::milliseconds(160));
    opacity->setEasing(Gui::Easing::InQuad);
    auto* scale = new Gui::NumberAnimation(group);
    scale->setTargetItem(frame);
    scale->setTargetProperty("scale");
    scale->setTo(0.4);
    scale->setDuration(std::chrono::milliseconds(160));
    scale->setEasing(Gui::Easing::InQuad);
    // The group only completes when no restore() stopped it meanwhile, so
    // hiding the frame here always matches the (still minimized) state.
    // hosted is safe to touch: sweepDead() stops the animation before
    // freeing Hosted, so this handler cannot run after that.
    group->connect(group, Gui::Animation::signalFinished, [frame, group, hosted] {
        group->deleteLater();
        hosted->minimizeAnim = nullptr;
        frame->setVisible(false);
    });
    group->start();
    if (m_active == hosted) {
        m_active = nullptr;
        for (auto it = m_hosted.rbegin(); it != m_hosted.rend(); ++it)
            if (!(*it)->dead && (*it)->state != WindowState::Minimized) {
                activate(it->get());
                break;
            }
    }
    rebuildTaskbar();
}

void CompositorApp::restore(Hosted* hosted) {
    if (!hosted || hosted->dead || hosted->state != WindowState::Minimized) return;
    hosted->state = hosted->stateBeforeMinimize;
    // Cancel a still-running minimize fade: its completion handler hides the
    // frame, which must not happen once the window is restored.
    if (hosted->minimizeAnim) {
        hosted->minimizeAnim->stop(); // stop() does not emit finished
        hosted->minimizeAnim->deleteLater();
        hosted->minimizeAnim = nullptr;
    }
    hosted->frame->setVisible(true);
    animatePop(hosted->frame, 0.0f, 1.0f, 0.4f, 1.0f, 160);
    rebuildTaskbar();
}

void CompositorApp::configureHosted(Hosted* hosted, int width, int height) {
    // xdg requires xdg_surface.configure (ackable) in addition to the
    // role-specific configure; go through the xdg surface when available.
    // The back-pointer is resolved lazily: it is nulled when either object
    // dies, so no dangling state is kept here.
    if (auto* xdg = hosted->toplevel ? hosted->toplevel->xdgSurface() : nullptr)
        xdg->sendConfigure(width, height);
    else if (hosted->toplevel)
        hosted->toplevel->sendConfigure(width, height);
}

void CompositorApp::toggleMaximized(Hosted* hosted) {
    if (!hosted || hosted->dead || hosted->state == WindowState::Minimized ||
        hosted->state == WindowState::Fullscreen)
        return;
    activate(hosted);
    if (hosted->state == WindowState::Normal) {
        hosted->normalGeometry = hosted->frame->geometry();
        hosted->state = WindowState::Maximized;
        const Gui::RectF target{0, 0, m_desktop->width(), m_desktop->height()};
        animateGeometry(hosted->frame, target);
        if (hosted->toplevel) {
            hosted->toplevel->setMaximized(true);
            configureHosted(hosted, static_cast<int>(target.width),
                            static_cast<int>(target.height - m_barHeight));
        }
    } else {
        hosted->state = WindowState::Normal;
        animateGeometry(hosted->frame, hosted->normalGeometry);
        if (hosted->toplevel) {
            hosted->toplevel->setMaximized(false);
            configureHosted(hosted, static_cast<int>(hosted->normalGeometry.width),
                            static_cast<int>(hosted->normalGeometry.height - m_barHeight));
        }
    }
}

void CompositorApp::toggleFullscreen(Hosted* hosted) {
    if (!hosted || hosted->dead || hosted->state == WindowState::Minimized) return;
    activate(hosted);
    if (hosted->state != WindowState::Fullscreen) {
        if (hosted->state == WindowState::Normal)
            hosted->normalGeometry = hosted->frame->geometry();
        hosted->stateBeforeMinimize = hosted->state; // reuse: where to return
        hosted->state = WindowState::Fullscreen;
        m_desktop->setZ(100.0f);
        hosted->frame->setZ(10000.0f);
        hosted->titleBar->setVisible(false);
        hosted->surfaceItem->setTopMargin(0);
        // The desktop area, not the whole window: the taskbar stays visible and
        // clickable, which is how a fullscreen window is left again (it has no
        // title bar). The desktop item is raised above the taskbar and clipped
        // to this area, so the frame never covers the bar.
        const Gui::RectF target{0, 0, m_desktop->width(), m_desktop->height()};
        animateGeometry(hosted->frame, target);
        if (hosted->toplevel) {
            hosted->toplevel->setFullscreen(true);
            configureHosted(hosted, static_cast<int>(target.width),
                            static_cast<int>(target.height));
        }
    } else {
        hosted->state = hosted->stateBeforeMinimize == WindowState::Fullscreen
                            ? WindowState::Normal
                            : hosted->stateBeforeMinimize;
        m_desktop->setZ(0.0f);
        hosted->frame->setZ(m_zCounter++);
        hosted->titleBar->setVisible(true);
        hosted->surfaceItem->setTopMargin(m_barHeight);
        const Gui::RectF target =
            hosted->state == WindowState::Maximized
                ? Gui::RectF{0, 0, m_desktop->width(), m_desktop->height()}
                : hosted->normalGeometry;
        animateGeometry(hosted->frame, target);
        if (hosted->toplevel) {
            hosted->toplevel->setFullscreen(false);
            configureHosted(hosted, static_cast<int>(target.width),
                            static_cast<int>(target.height -
                                             (hosted->state == WindowState::Normal
                                                  ? m_barHeight : 0)));
        }
    }
}

void CompositorApp::close(Hosted* hosted) {
    if (!hosted || hosted->dead) return;
    if (hosted->toplevel)
        hosted->toplevel->sendClose();
    // wl_shell has no close request; the client stays alive until it exits.
}

void CompositorApp::activateTask(double index) {
    const int i = static_cast<int>(index);
    if (std::getenv("FIRMIR_COMPOSITOR1_FAKE_INPUT"))
        std::fprintf(stderr, "compositor1: activateTask(%d) state reached\n", i);
    if (i < 0 || i >= static_cast<int>(m_hosted.size())) return;
    Hosted* hosted = m_hosted[static_cast<std::size_t>(i)].get();
    if (hosted->dead) return;
    // A fullscreen window has no title bar, so its taskbar button is the only
    // way out of fullscreen short of killing the client.
    if (hosted->state == WindowState::Fullscreen) {
        toggleFullscreen(hosted);
        activate(hosted);
        return;
    }
    if (hosted->state == WindowState::Minimized) {
        // Restoring always activates: the user asked for this window.
        restore(hosted);
        activate(hosted);
    } else if (hosted == m_active) {
        minimize(hosted); // clicking the active task toggles it away
    } else {
        activate(hosted);
    }
}
