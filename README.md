# firmir-compositor1

Compositor example with firmir-wayland.

Requires CMake 3.19 or newer (and a C++17 compiler), pkg-config, plus firmir
either installed or checked out next to this directory. Nothing else: the
keymap (xkbcommon), the input forwarding and the socket handling all live in
`firmir-wayland-compositor`.

```sh
# against an installed firmir (pkg-config firmir-wayland-compositor)
cmake -S . -B build && cmake --build build -j$(nproc)

# against a firmir source tree (no installation needed)
cmake -S . -B build -DFIRMIR_ROOT=/path/to/firmir && cmake --build build -j$(nproc)
```

Install (default prefix is /usr, so this lands in /usr/bin):

```sh
sudo cmake --install build                    # -> /usr/bin/firmir-compositor1
cmake --install build --prefix /opt/firmir    # -> /opt/firmir/bin/firmir-compositor1
DESTDIR=/tmp/stage cmake --install build      # -> /tmp/stage/usr/bin/... (packaging)
```

When building against a firmir source tree only the binary is installed; the
firmir libraries and plugins come from their own build tree.

## Usage

```sh
firmir-compositor1
# WAYLAND_DISPLAY=/run/display/wayland-0
WAYLAND_DISPLAY=/run/display/wayland-0 firmir-weather
# or just (firmir finds /run/display/wayland-* on its own):
firmir-weather
```

The socket lives in `/run/display` (created on demand — a bare KMS console has
no `XDG_RUNTIME_DIR`) as `wayland-0`; if a live compositor already holds that
name, `wayland-1`, `wayland-2`, … is used. The printed line is the exact value
for `WAYLAND_DISPLAY`; a firmir app started without it probes the sockets in
`/run/display` and connects to the first one that accepts. Overrides:

| Variable | Meaning |
| --- | --- |
| `FIRMIR_WAYLAND_SOCKET_DIR` | socket directory (default `/run/display`) |
| `FIRMIR_WAYLAND_SOCKET` | exact socket name, or an absolute path used verbatim |

Client frames are read through a buffer backend plugin of
`firmir-wayland-compositor` (`firmir-wayland-compositor-<name>.so`, `drm_egl`
first, `shm` as the fallback). An installed firmir finds them in
`lib/firmir/plugins/wayland/compositor`; when this binary runs against a firmir
build tree, point it there:

```sh
FIRMIR_WAYLAND_COMPOSITOR_PLUGIN_PATH=/path/to/firmir/build/plugins/wayland/compositor \
    firmir-compositor1
```

Without any plugin the compositor still reads `wl_shm` buffers itself, so this
is a performance/acceleration switch, not a requirement.

The hosted clients get the mouse, the keyboard and the touchscreen through
firmir's `WaylandGuiItem`, which forwards the events through
`firmir-wayland-compositor` to the client of the surface under the pointer.
The window itself is drag-able and its title-bar buttons are clickable with a
finger as well: `TitleBar`/`TitleButton` turn a touch contact into the
equivalent mouse event.

The UI is declared in `compositor1.fml`; firmlc (installed or built by the
firmir tree) compiles it to C++ at build time.

The decorations are sized in millimetres, not pixels: a hosted window's title
bar is 9 mm high (its four buttons are 9 mm squares) and the taskbar is 11 mm
with one 45×9 mm button per hosted window. Both are computed from the density
of the window's own screen — `Window::ppcm()` is pixels per *centimetre*, so
a millimetre costs `ppcm / 10` — with a 96 ppi fallback (3.7795 px/mm) when the
backend does not report the panel size, so the same build is finger-sized on a
tablet panel and keeps the old desktop look on a 96 ppi display. Moving the
window to another screen (`Window::setScreen()`) re-applies them to every
hosted frame and re-configures the clients for the smaller/larger client area.

The panel size reaches the hosted clients too: the compositor's `wl_output`
reports `Screen::physicalSize()` through `wl_output.geometry` (millimetres, not
the pixel mode), so a client that lays out in millimetres is as readable inside
a hosted window as it is standalone. Without it a client falls back to 96 ppi
and draws a dense panel far too small.

Window state requests from the clients are honoured as well: `set_minimized`
(the app's own "_" button) minimizes the hosted window, and a client that asks
for `set_maximized` while fullscreen leaves fullscreen first (the protocol
ignores maximize while fullscreen, so its "[]" button would otherwise do
nothing). A fullscreen window has no title bar, so its taskbar button is the
way back: it leaves fullscreen and activates the window.
