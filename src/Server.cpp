#include "Server.hpp"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <sys/wait.h>
#include <unistd.h>

extern "C" {
#include <wlr/backend/multi.h>
}

#include <fitzel/rhi/Device.hpp>

#include "Apps.hpp"
#include "Desk.hpp"
#include "DeskRenderer.hpp"
#include "Gpu.hpp"
#include "HostShortcuts.hpp"
#include "HostWindow.hpp"
#include "Hud.hpp"
#include "Output.hpp"
#include "Sfx.hpp"
#include "Stage.hpp"
#include "State.hpp"
#include "SurfaceTextures.hpp"
#include "View.hpp"

namespace raum {

namespace {

void findNested(wlr_backend* b, void* data) {
    if (wlr_backend_is_wl(b) || wlr_backend_is_x11(b)) *static_cast<bool*>(data) = true;
}

void findWl(wlr_backend* b, void* data) {
    if (wlr_backend_is_wl(b)) *static_cast<wlr_backend**>(data) = b;
}

// The device node of the first GPU from `vendor` ("0x10de" NVIDIA): its
// /dev/dri/cardN (`render` false) or /dev/dri/renderDN (`render` true).
std::string gpuNode(const char* vendor, bool render) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<std::string> found;
    for (const auto& e : fs::directory_iterator("/sys/class/drm", ec)) {
        const std::string n = e.path().filename().string();
        const bool isCard = n.rfind("card", 0) == 0 && n.find('-') == std::string::npos;
        const bool isRender = n.rfind("renderD", 0) == 0;
        if (render ? !isRender : !isCard) continue;
        std::ifstream v(e.path() / "device" / "vendor");
        std::string id;
        v >> id;
        if (id == vendor) found.push_back("/dev/dri/" + n);
    }
    std::sort(found.begin(), found.end());
    return found.empty() ? std::string() : found.front();
}

// On a machine with an NVIDIA card raum uses that one and only that one: the
// screen, raum's drawing and (through the dmabuf feedback) the programs'. With
// a second GPU in the box wlroots would otherwise take whichever the kernel
// numbered first -- this machine's iGPU on some boots -- and drive the NVIDIA
// screen across both, with no hardware cursor. Nested, the host desktop
// decides; RAUM_GPU=any leaves the choice to wlroots.
void preferNvidia() {
    const char* any = std::getenv("RAUM_GPU");
    if (any && std::string(any) == "any") return;
    if (std::getenv("WAYLAND_DISPLAY") || std::getenv("DISPLAY")) return;
    const char* backends = std::getenv("WLR_BACKENDS");
    const bool headless = backends && std::string(backends) == "headless";
    if (headless) {
        if (std::getenv("WLR_RENDER_DRM_DEVICE")) return;
        const std::string node = gpuNode("0x10de", true);
        if (!node.empty()) setenv("WLR_RENDER_DRM_DEVICE", node.c_str(), 1);
        return;
    }
    if (std::getenv("WLR_DRM_DEVICES")) return;
    const std::string card = gpuNode("0x10de", false);
    if (card.empty()) return;
    setenv("WLR_DRM_DEVICES", card.c_str(), 1);
    setenv("GBM_BACKEND", "nvidia-drm", 0);
    setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", 0);
    std::fprintf(stderr, "[raum] NVIDIA only: %s\n", card.c_str());
}

void findHeadless(wlr_backend* b, void* data) {
    if (wlr_backend_is_headless(b)) *static_cast<wlr_backend**>(data) = b;
}

// The keyboard layout: XKB_DEFAULT_* if the session sets them, else the
// system's /etc/default/keyboard (what the console and GDM use).
xkb_rule_names keymapNames(std::string store[4]) {
    xkb_rule_names names{};
    const char* layout = std::getenv("XKB_DEFAULT_LAYOUT");
    if (layout && *layout) {
        names.layout = layout;
        names.variant = std::getenv("XKB_DEFAULT_VARIANT");
        names.options = std::getenv("XKB_DEFAULT_OPTIONS");
        names.model = std::getenv("XKB_DEFAULT_MODEL");
        return names;
    }
    std::ifstream in("/etc/default/keyboard");
    std::string line;
    auto value = [](const std::string& l) {
        std::string v = l.substr(l.find('=') + 1);
        v.erase(std::remove(v.begin(), v.end(), '"'), v.end());
        return v;
    };
    while (std::getline(in, line)) {
        if (line.rfind("XKBLAYOUT=", 0) == 0) store[0] = value(line);
        else if (line.rfind("XKBVARIANT=", 0) == 0) store[1] = value(line);
        else if (line.rfind("XKBOPTIONS=", 0) == 0) store[2] = value(line);
        else if (line.rfind("XKBMODEL=", 0) == 0) store[3] = value(line);
    }
    if (!store[0].empty()) names.layout = store[0].c_str();
    if (!store[1].empty()) names.variant = store[1].c_str();
    if (!store[2].empty()) names.options = store[2].c_str();
    if (!store[3].empty()) names.model = store[3].c_str();
    return names;
}

} // namespace

// --- Keyboards ---------------------------------------------------------------------

Keyboard::Keyboard(Server& server, wlr_keyboard* keyboard) : kb(keyboard), m_server(server) {
    std::string store[4];
    const xkb_rule_names names = keymapNames(store);
    xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    xkb_keymap* keymap = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap) keymap = xkb_keymap_new_from_names(ctx, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    wlr_keyboard_set_keymap(kb, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(ctx);
    wlr_keyboard_set_repeat_info(kb, 30, 400);
    m_key.connect(&kb->events.key, this, &Keyboard::onKey);
    m_modifiers.connect(&kb->events.modifiers, this, &Keyboard::onModifiers);
    m_destroy.connect(&kb->base.events.destroy, this, &Keyboard::onDestroy);
    wlr_seat_set_keyboard(server.seat, kb);
}

void Keyboard::onKey(void* data) { m_server.keyboardKey(*this, *static_cast<wlr_keyboard_key_event*>(data)); }
void Keyboard::onModifiers(void*) { m_server.keyboardModifiers(*this); }

void Keyboard::onDestroy(void*) {
    auto& ks = m_server.keyboards;
    ks.erase(std::remove_if(ks.begin(), ks.end(),
                            [this](const std::unique_ptr<Keyboard>& k) { return k.get() == this; }),
             ks.end());
}

// --- The server ----------------------------------------------------------------------

Server::Server(const Options& opts) : options(opts) {
    m_windowedW = opts.width;
    m_windowedH = opts.height;
    const char* debug = std::getenv("RAUM_DEBUG");
    wlr_log_init(debug && *debug ? WLR_DEBUG : WLR_ERROR, nullptr);
    // raum draws through its own OpenGL context on the GLES renderer's EGL display.
    setenv("WLR_RENDERER", "gles2", 1);
    preferNvidia();

    display = wl_display_create();
    loop = wl_display_get_event_loop(display);
    // Nested on Wayland raum makes its window itself (HostWindow), so wlroots
    // is told to make none. Unset again right after: a nested compositor
    // started inside raum should make its own as usual.
    const bool setOutputs = std::getenv("WAYLAND_DISPLAY") && !std::getenv("WLR_WL_OUTPUTS");
    if (setOutputs) setenv("WLR_WL_OUTPUTS", "0", 1);
    backend = wlr_backend_autocreate(loop, &session);
    if (setOutputs) unsetenv("WLR_WL_OUTPUTS");
    if (!backend) {
        std::fprintf(stderr, "[raum] no wlroots backend\n");
        return;
    }
    if (wlr_backend_is_multi(backend)) wlr_multi_for_each_backend(backend, findNested, &nested);
    renderer = wlr_renderer_autocreate(backend);
    if (!renderer) {
        std::fprintf(stderr, "[raum] no wlroots renderer\n");
        return;
    }
    wlr_renderer_init_wl_display(renderer, display);   // wl_shm and linux-dmabuf
    allocator = wlr_allocator_autocreate(backend, renderer);

    gpu = std::make_unique<Gpu>(renderer);
    if (!gpu->ok()) return;
    // Explicit sync for clients that render on the GPU (NVIDIA's driver has
    // no implicit sync): raum waits for their fences itself (SurfaceTextures).
    if (renderer->features.timeline) {
        const int fd = wlr_renderer_get_drm_fd(renderer);
        if (fd >= 0 && wlr_linux_drm_syncobj_manager_v1_create(display, 1, fd))
            std::fprintf(stderr, "[raum] explicit sync on\n");
    }

    // No renderer for the compositor: raum makes its textures from the buffers
    // itself (SurfaceTextures), in its own context.
    compositor = wlr_compositor_create(display, 6, nullptr);
    wlr_subcompositor_create(display);
    stages = std::make_unique<Stages>(*this);
    wlr_data_device_manager_create(display);
    wlr_primary_selection_v1_device_manager_create(display);
    wlr_data_control_manager_v1_create(display);
    wlr_viewporter_create(display);
    wlr_single_pixel_buffer_manager_v1_create(display);
    wlr_fractional_scale_manager_v1_create(display, 1);
    wlr_screencopy_manager_v1_create(display);
    wlr_export_dmabuf_manager_v1_create(display);
    wlr_presentation_create(display, backend, 2);
    outputLayout = wlr_output_layout_create(display);
    wlr_xdg_output_manager_v1_create(display, outputLayout);
    xdgShell = wlr_xdg_shell_create(display, 6);
    m_activation = wlr_xdg_activation_v1_create(display);
    m_cursorShape = wlr_cursor_shape_manager_v1_create(display, 1);

    cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(cursor, outputLayout);
    xcursor = wlr_xcursor_manager_create(nullptr, 24);
    seat = wlr_seat_create(display, "seat0");

    surfaces = std::make_unique<SurfaceTextures>(*gpu);
    desk = std::make_unique<Desk>(*this);
    state = std::make_unique<State>(*this);
    state->load();
    {
        // Sounds go out through the machine's speakers -- never from a headless
        // run (tests), which nobody is listening to.
        wlr_backend* hl = nullptr;
        if (wlr_backend_is_multi(backend)) wlr_multi_for_each_backend(backend, findHeadless, &hl);
        else if (wlr_backend_is_headless(backend)) hl = backend;
        sfx = std::make_unique<Sfx>(hl == nullptr, !state->soundOn);
    }
    apps = std::make_unique<Apps>();
    deskRenderer = std::make_unique<DeskRenderer>(*this);
    hud = std::make_unique<Hud>(*this);

    m_newOutput.connect(&backend->events.new_output, this, &Server::onNewOutput);
    m_newInput.connect(&backend->events.new_input, this, &Server::onNewInput);
    m_newSurface.connect(&compositor->events.new_surface, this, &Server::onNewSurface);
    m_newToplevel.connect(&xdgShell->events.new_toplevel, this, &Server::onNewToplevel);
    m_newPopup.connect(&xdgShell->events.new_popup, this, &Server::onNewPopup);
    m_cursorMotion.connect(&cursor->events.motion, this, &Server::onCursorMotion);
    m_cursorMotionAbs.connect(&cursor->events.motion_absolute, this, &Server::onCursorMotionAbsolute);
    m_cursorButton.connect(&cursor->events.button, this, &Server::onCursorButton);
    m_cursorAxis.connect(&cursor->events.axis, this, &Server::onCursorAxis);
    m_cursorFrame.connect(&cursor->events.frame, this, &Server::onCursorFrame);
    m_requestSetCursor.connect(&seat->events.request_set_cursor, this, &Server::onRequestSetCursor);
    m_requestSetSelection.connect(&seat->events.request_set_selection, this, &Server::onRequestSetSelection);
    m_requestSetPrimarySelection.connect(&seat->events.request_set_primary_selection, this,
                                         &Server::onRequestSetPrimarySelection);
    m_cursorShapeRequest.connect(&m_cursorShape->events.request_set_shape, this, &Server::onCursorShape);
    m_activate.connect(&m_activation->events.request_activate, this, &Server::onActivate);
}

Server::~Server() {
    if (m_busPid > 0) kill(m_busPid, SIGTERM);
    if (!display) return;
    wl_display_destroy_clients(display);
    // wlroots asserts that nobody still listens to what it destroys.
    for (Listener<Server>* l : {&m_newOutput, &m_newInput, &m_newSurface, &m_newToplevel, &m_newPopup,
                                &m_cursorMotion, &m_cursorMotionAbs, &m_cursorButton, &m_cursorAxis,
                                &m_cursorFrame, &m_requestSetCursor, &m_requestSetSelection,
                                &m_requestSetPrimarySelection, &m_cursorShapeRequest, &m_activate,
                                &m_newXwaylandSurface, &m_xwaylandReady, &m_cursorSurfaceDestroy})
        l->disconnect();
    if (xwayland) wlr_xwayland_destroy(xwayland);
    stages.reset();   // its clients are gone: only the global is left
    views.clear();
    unmanaged.clear();
    m_popups.clear();
    if (gpu && gpu->ok()) gpu->makeCurrent();
    hud.reset();
    deskRenderer.reset();
    hostShortcuts.reset();
    hostWindow.reset();   // its proxies before the connection they live on
    if (backend) wlr_backend_destroy(backend);   // outputs and their buffers go here
    outputs.clear();
    keyboards.clear();
    if (xcursor) wlr_xcursor_manager_destroy(xcursor);
    if (cursor) wlr_cursor_destroy(cursor);
    if (allocator) wlr_allocator_destroy(allocator);
    if (renderer) wlr_renderer_destroy(renderer);
    surfaces.reset();
    gpu.reset();
    wl_display_destroy(display);
}

bool Server::start() {
    if (!gpu || !gpu->ok()) return false;
    const char* sock = wl_display_add_socket_auto(display);
    if (!sock) {
        std::fprintf(stderr, "[raum] no Wayland socket\n");
        return false;
    }
    socketName = sock;
    if (!wlr_backend_start(backend)) {
        std::fprintf(stderr, "[raum] the backend did not start\n");
        return false;
    }
    // Nested on Wayland: raum's own window (fullscreen when asked), or the one
    // wlroots makes when the host will not have it.
    wlr_backend* wl = nullptr;
    if (wlr_backend_is_multi(backend)) wlr_multi_for_each_backend(backend, findWl, &wl);
    else if (wlr_backend_is_wl(backend)) wl = backend;
    if (wl && outputs.empty()) {
        hostWindow = std::make_unique<HostWindow>(
            wl, options.fullscreen, [this](int w, int h) { hostResized(w, h); }, [this] { quit(); });
        if (hostWindow->initialWidth() > 0 && hostWindow->initialHeight() > 0) {
            options.width = hostWindow->initialWidth();
            options.height = hostWindow->initialHeight();
        }
        m_creatingHostOutput = true;
        hostWindow->createOutput(wl);
        m_creatingHostOutput = false;
        if (!hostWindow->output()) {
            hostWindow.reset();
            wlr_wl_output_create(wl);
        }
    }
    // Headless (WLR_BACKENDS=headless, for tests): one virtual screen.
    wlr_backend* headless = nullptr;
    if (wlr_backend_is_multi(backend)) wlr_multi_for_each_backend(backend, findHeadless, &headless);
    else if (wlr_backend_is_headless(backend)) headless = backend;
    if (headless && outputs.empty())
        wlr_headless_add_output(headless, static_cast<unsigned>(options.width),
                                static_cast<unsigned>(options.height));
    if (options.xwayland) {
        xwayland = wlr_xwayland_create(display, compositor, true);
        if (xwayland) {
            m_newXwaylandSurface.connect(&xwayland->events.new_surface, this, &Server::onNewXwaylandSurface);
            m_xwaylandReady.connect(&xwayland->events.ready, this, &Server::onXwaylandReady);
        }
    }
    setupEnvironment();
    // Inside another desktop (nested, or headless for a test) clients get a bus
    // of their own; in a session of its own raum IS the desktop.
    if (!session && options.privateBus) startPrivateBus();
    if (session) {
        // A session of its own: the user's services (portals, keyring, systemd
        // units) have to learn which display is theirs now.
        spawn("dbus-update-activation-environment --systemd WAYLAND_DISPLAY DISPLAY "
              "XDG_CURRENT_DESKTOP XDG_SESSION_TYPE XDG_SESSION_DESKTOP 2>/dev/null");
    }
    m_sigchld = wl_event_loop_add_signal(loop, SIGCHLD, &Server::onSigchld, this);
    m_timer = wl_event_loop_add_timer(loop, &Server::onTimer, this);
    wl_event_source_timer_update(m_timer, 1000);
    if (!options.script.empty()) loadScript(options.script);
    std::fprintf(stderr, "[raum] WAYLAND_DISPLAY=%s%s%s\n", socketName.c_str(),
                 xwayland ? "  DISPLAY=" : "", xwayland ? xwayland->display_name : "");
    runAutostart();
    return true;
}

void Server::run() { wl_display_run(display); }
void Server::quit() {
    // A screenshot still waiting for its frame is taken first.
    if (!pendingScreenshot.empty() || !pendingOutputShot.empty()) {
        quitAfterFrame = true;
        scheduleFrame();
        return;
    }
    wl_display_terminate(display);
}

double Server::now() const {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + ts.tv_nsec * 1e-9;
}

Output* Server::mainOutput() const { return outputs.empty() ? nullptr : outputs.front().get(); }

void Server::scheduleFrame() {
    if (Output* o = mainOutput()) o->scheduleFrame();
}

void Server::setupEnvironment() {
    setenv("WAYLAND_DISPLAY", socketName.c_str(), 1);
    if (xwayland) setenv("DISPLAY", xwayland->display_name, 1);
    else unsetenv("DISPLAY");
    setenv("XDG_SESSION_TYPE", "wayland", 1);
    // raum first, then GNOME: Chromium and Electron choose where they keep
    // their secrets by this list, and for a desktop they do not know they keep
    // them nowhere -- Claude Desktop, Chrome, VS Code could not read their own
    // logins (the keyring is GNOME's, and it runs in raum's session too). The
    // portals look for raum-portals.conf first and then take GNOME's.
    setenv("XDG_CURRENT_DESKTOP", "raum:GNOME", 1);
    setenv("XDG_SESSION_DESKTOP", "raum", 1);
    setenv("GDK_BACKEND", "wayland,x11", 1);
    setenv("QT_QPA_PLATFORM", "wayland;xcb", 1);
    setenv("MOZ_ENABLE_WAYLAND", "1", 1);
    setenv("ELECTRON_OZONE_PLATFORM_HINT", "auto", 1);
    setenv("_JAVA_AWT_WM_NONREPARENTING", "1", 1);
    unsetenv("WAYLAND_SOCKET");
}

void Server::startPrivateBus() {
    // Nested, the host desktop's session bus would catch every program that
    // keeps a single instance (most GNOME apps): starting one here would only
    // open a window over there. A bus of raum's own keeps them here. The
    // portals stay with the host, so GTK is told not to wait for them.
    FILE* p = popen("dbus-daemon --session --fork --print-address=1 --print-pid=1 2>/dev/null", "r");
    if (!p) return;
    char addr[512] = {}, pid[32] = {};
    if (fgets(addr, sizeof(addr), p) && fgets(pid, sizeof(pid), p)) {
        addr[std::strcspn(addr, "\n")] = 0;
        setenv("DBUS_SESSION_BUS_ADDRESS", addr, 1);
        m_busPid = static_cast<pid_t>(std::atoi(pid));
        setenv("GDK_DEBUG", "no-portals", 1);
        setenv("ADW_DISABLE_PORTAL", "1", 1);
        setenv("GTK_USE_PORTAL", "0", 1);
        std::fprintf(stderr, "[raum] clients get their own D-Bus (%s)\n", addr);
    }
    pclose(p);
}

void Server::runAutostart() {
    for (const std::string& c : options.autostart) spawn(c);
}

void Server::spawn(const std::string& command) {
    const pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        setsid();
        sigset_t set;
        sigemptyset(&set);
        sigprocmask(SIG_SETMASK, &set, nullptr);
        signal(SIGCHLD, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        execl("/bin/sh", "/bin/sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    std::fprintf(stderr, "[raum] started: %s\n", command.c_str());
}

int Server::onSigchld(int, void*) {
    while (waitpid(-1, nullptr, WNOHANG) > 0) {}
    return 0;
}

int Server::onTimer(void* data) {
    auto* s = static_cast<Server*>(data);
    // The clock in the top bar changes once a minute.
    // The clock in the strip shows the seconds: one frame a second, idle or not.
    const std::time_t t = std::time(nullptr);
    if (t != s->m_lastMinute) {
        s->m_lastMinute = t;
        s->scheduleFrame();
    }
    wl_event_source_timer_update(s->m_timer, 1000);
    return 0;
}

void Server::toggleSound() {
    const bool on = sfx->muted();
    sfx->setMuted(!on);
    state->soundOn = on;
    state->save();
    if (on) sfx->play(Sfx::Notify);
    hud->toast(on ? "Ton an" : "Ton aus");
}

void Server::toggleFullscreen() {
    if (!hostWindow) {
        hud->toast(nested ? "Dieses Fenster hat wlroots gemacht: kein Vollbild"
                          : "raum hat den Bildschirm schon ganz");
        return;
    }
    hostWindow->setFullscreen(!hostWindow->fullscreen());
}

void Server::hostResized(int width, int height) {
    // 0 x 0: the host leaves the size to raum (out of fullscreen again).
    if (width <= 0 || height <= 0) {
        width = m_windowedW;
        height = m_windowedH;
    }
    Output* o = mainOutput();
    if (!o) return;
    wlr_output_state st;
    wlr_output_state_init(&st);
    wlr_output_state_set_custom_mode(&st, width, height, 0);
    wlr_output_commit_state(o->wlr, &st);
    wlr_output_state_finish(&st);
    scheduleFrame();
}

void Server::screenshotTaken() {
    if (m_scriptAwaitsShot && m_scriptTimer) {
        m_scriptAwaitsShot = false;
        wl_event_source_timer_update(m_scriptTimer, 1);
    }
}

void Server::takeScreenshot(const std::string& path) {
    pendingScreenshot = path;
    scheduleFrame();
}

// --- New things -------------------------------------------------------------------------

void Server::onNewOutput(void* data) {
    auto* o = static_cast<wlr_output*>(data);
    if (!wlr_output_init_render(o, allocator, renderer)) return;
    wlr_output_state st;
    wlr_output_state_init(&st);
    if (!outputs.empty()) {
        // One screen in this first version: the others stay dark (README).
        wlr_output_state_set_enabled(&st, false);
        wlr_output_commit_state(o, &st);
        wlr_output_state_finish(&st);
        return;
    }
    wlr_output_state_set_enabled(&st, true);
    if (wlr_output_is_headless(o)) {
        wlr_output_state_set_custom_mode(&st, options.width, options.height, 0);
    } else if (wlr_output_is_wl(o)) {
        wlr_output_state_set_custom_mode(&st, options.width, options.height, 0);
        // A window wlroots made itself gets its name here; raum's own has it.
        if (!m_creatingHostOutput) {
            wlr_wl_output_set_title(o, "raum");
            wlr_wl_output_set_app_id(o, "raum");
        }
    } else if (wlr_output_mode* mode = wlr_output_preferred_mode(o)) {
        wlr_output_state_set_mode(&st, mode);
    }
    wlr_output_commit_state(o, &st);
    wlr_output_state_finish(&st);
    outputs.push_back(std::make_unique<Output>(*this, o));
    wlr_output_layout_add_auto(outputLayout, o);
    wlr_output_create_global(o, display);
    wlr_cursor_warp(cursor, nullptr, o->width * 0.5, o->height * 0.5);
    resetCursorImage();
    if (nested && !std::getenv("RAUM_NO_INHIBIT"))
        hostShortcuts = std::make_unique<HostShortcuts>(backend, o);
    std::fprintf(stderr, "[raum] output %s %dx%d\n", o->name, o->width, o->height);
    scheduleFrame();
}

void Server::onNewInput(void* data) {
    auto* dev = static_cast<wlr_input_device*>(data);
    switch (dev->type) {
        case WLR_INPUT_DEVICE_KEYBOARD:
            keyboards.push_back(std::make_unique<Keyboard>(*this, wlr_keyboard_from_input_device(dev)));
            break;
        case WLR_INPUT_DEVICE_POINTER:
            wlr_cursor_attach_input_device(cursor, dev);
            break;
        default:
            break;
    }
    std::uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!keyboards.empty()) caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    wlr_seat_set_capabilities(seat, caps);
}

void Server::onNewSurface(void* data) { surfaces->track(static_cast<wlr_surface*>(data)); }

void Server::onNewToplevel(void* data) {
    addView(std::make_unique<XdgView>(*this, static_cast<wlr_xdg_toplevel*>(data)));
}

void Server::onNewPopup(void* data) {
    if (std::getenv("RAUM_DEBUG_CANVAS")) std::fprintf(stderr, "[popup] new\n");
    m_popups.push_back(std::make_unique<XdgPopup>(*this, static_cast<wlr_xdg_popup*>(data)));
}

void Server::popupCreated(XdgPopup*) {}

void Server::popupDestroyed(XdgPopup* popup) {
    m_popups.erase(std::remove_if(m_popups.begin(), m_popups.end(),
                                  [popup](const std::unique_ptr<XdgPopup>& p) { return p.get() == popup; }),
                   m_popups.end());
    scheduleFrame();
}

void Server::onNewXwaylandSurface(void* data) {
    auto* xs = static_cast<wlr_xwayland_surface*>(data);
    if (xs->override_redirect) unmanaged.push_back(std::make_unique<XwaylandUnmanaged>(*this, xs));
    else addView(std::make_unique<XwaylandView>(*this, xs));
}

void Server::onXwaylandReady(void*) {
    wlr_xwayland_set_seat(xwayland, seat);
    if (wlr_xcursor* xc = wlr_xcursor_manager_get_xcursor(xcursor, "default", 1)) {
        wlr_xcursor_image* img = xc->images[0];
        wlr_xwayland_set_cursor(xwayland, img->buffer, img->width * 4, img->width, img->height,
                                img->hotspot_x, img->hotspot_y);
    }
}

void Server::unmanagedDestroyed(XwaylandUnmanaged* u) {
    unmanaged.erase(std::remove_if(unmanaged.begin(), unmanaged.end(),
                                   [u](const std::unique_ptr<XwaylandUnmanaged>& p) { return p.get() == u; }),
                    unmanaged.end());
    scheduleFrame();
}

// --- Windows ----------------------------------------------------------------------------

void Server::addView(std::unique_ptr<View> view) { views.push_back(std::move(view)); }

void Server::destroyView(View* view) {
    if (m_focused == view) m_focused = nullptr;
    if (m_pointerView == view) m_pointerView = nullptr;
    if (m_grabView == view) {
        m_grabView = nullptr;
        m_grab = Grab::None;
    }
    gpu->makeCurrent();
    views.erase(std::remove_if(views.begin(), views.end(),
                               [view](const std::unique_ptr<View>& v) { return v.get() == view; }),
                views.end());
    scheduleFrame();
}

View* Server::viewOfSurface(wlr_surface* surface) const {
    if (!surface) return nullptr;
    for (const auto& v : views)
        if (v->surface() == surface) return v.get();
    return nullptr;
}

void Server::viewMapped(View* view) {
    sfx->play(Sfx::Materialize, view->parentView() ? 0.6f : 1.0f);
    desk->viewMapped(view);
    focus(view);
    scheduleFrame();
}

void Server::viewUnmapped(View* view) {
    sfx->play(Sfx::Dematerialize, view->parentView() ? 0.6f : 1.0f);
    gpu->makeCurrent();
    desk->addGhost(view);   // its last picture folds away where it stood
    desk->viewUnmapped(view);
    if (m_pointerView == view) m_pointerView = nullptr;
    if (m_grabView == view) {
        m_grabView = nullptr;
        m_grab = Grab::None;
    }
    if (m_focused == view) {
        m_focused = nullptr;
        View* next = view->parentView();
        if (!next || !next->mapped()) next = desk->mostRecent(desk->current, view);
        focus(next);
    }
    scheduleFrame();
}

void Server::viewTitleChanged(View*) { scheduleFrame(); }

void Server::focus(View* view) {
    if (view && !view->mapped()) return;
    if (view == m_focused) {
        if (view) desk->focused(view);
        return;
    }
    if (m_focused) m_focused->setActivated(false);
    m_focused = view;
    if (!view) {
        wlr_seat_keyboard_notify_clear_focus(seat);
        scheduleFrame();
        return;
    }
    view->setActivated(true);
    if (wlr_keyboard* kb = wlr_seat_get_keyboard(seat))
        wlr_seat_keyboard_notify_enter(seat, view->surface(), kb->keycodes, kb->num_keycodes, &kb->modifiers);
    else
        wlr_seat_keyboard_notify_enter(seat, view->surface(), nullptr, 0, nullptr);
    desk->focused(view);
    scheduleFrame();
}

void Server::beginMove(View* view) {
    if (m_buttonsDown == 0 || desk->level != Level::Work) return;
    m_grab = Grab::Move;
    m_grabView = view;
    m_grabStart = glm::dvec2(cursor->x, cursor->y);
    m_grabOffsetStart = view->userOffset;
    wlr_seat_pointer_notify_clear_focus(seat);
}

void Server::beginResize(View* view, std::uint32_t edges) {
    if (m_buttonsDown == 0 || desk->level != Level::Work) return;
    m_grab = Grab::Resize;
    m_grabView = view;
    m_grabEdges = edges;
    m_grabStart = glm::dvec2(cursor->x, cursor->y);
    m_grabOffsetStart = view->userOffset;
    const wlr_box g = view->geometry();
    m_grabW = g.width;
    m_grabH = g.height;
    wlr_seat_pointer_notify_clear_focus(seat);
}

// --- Seat requests -------------------------------------------------------------------------

void Server::onRequestSetCursor(void* data) {
    auto* e = static_cast<wlr_seat_pointer_request_set_cursor_event*>(data);
    if (e->seat_client != seat->pointer_state.focused_client) return;
    if (m_grab == Grab::Move || m_grab == Grab::Resize || hud->wantsPointer()) return;
    wlr_cursor_set_surface(cursor, e->surface, e->hotspot_x, e->hotspot_y);
    setCursorImage({}, e->surface, e->hotspot_x, e->hotspot_y);
    m_clientCursor = true;
}

void Server::onCursorShape(void* data) {
    auto* e = static_cast<wlr_cursor_shape_manager_v1_request_set_shape_event*>(data);
    if (e->seat_client != seat->pointer_state.focused_client) return;
    if (m_grab == Grab::Move || m_grab == Grab::Resize || hud->wantsPointer()) return;
    wlr_cursor_set_xcursor(cursor, xcursor, wlr_cursor_shape_v1_name(e->shape));
    setCursorImage(wlr_cursor_shape_v1_name(e->shape), nullptr, 0, 0);
    m_clientCursor = true;
}

void Server::onRequestSetSelection(void* data) {
    auto* e = static_cast<wlr_seat_request_set_selection_event*>(data);
    wlr_seat_set_selection(seat, e->source, e->serial);
}

void Server::onRequestSetPrimarySelection(void* data) {
    auto* e = static_cast<wlr_seat_request_set_primary_selection_event*>(data);
    wlr_seat_set_primary_selection(seat, e->source, e->serial);
}

void Server::onActivate(void* data) {
    auto* e = static_cast<wlr_xdg_activation_v1_request_activate_event*>(data);
    if (View* v = viewOfSurface(e->surface)) {
        if (desk->level != Level::Work) desk->setLevel(Level::Work);
        focus(v);
    }
}

void Server::resetCursorImage() {
    wlr_cursor_set_xcursor(cursor, xcursor, "default");
    setCursorImage("default", nullptr, 0, 0);
    m_clientCursor = false;
}

void Server::setCursorImage(const std::string& name, wlr_surface* surface, int hotX, int hotY) {
    m_cursorSurfaceDestroy.disconnect();
    cursorImage = CursorImage{name, surface, hotX, hotY};
    if (surface) m_cursorSurfaceDestroy.connect(&surface->events.destroy, this, &Server::onCursorSurfaceDestroy);
    scheduleFrame();
}

void Server::onCursorSurfaceDestroy(void*) {
    m_cursorSurfaceDestroy.disconnect();
    cursorImage.surface = nullptr;   // a client's cursor that went: none shown
}

bool Server::softwareCursor() const {
    // A session of its own draws the cursor itself when the screen has no
    // cursor plane for it (wlroots would leave that to the renderer, which in
    // raum is not wlroots'). Nested, the host shows it; headless, nobody looks
    // -- except a test that asks (RAUM_SOFTWARE_CURSOR=1).
    static const bool forced = std::getenv("RAUM_SOFTWARE_CURSOR") != nullptr;
    if (forced) return true;
    if (!session) return false;
    const Output* o = mainOutput();
    return o && o->wlr->hardware_cursor == nullptr;
}

// --- Scripts (tests, demos) -------------------------------------------------------------------

void Server::loadScript(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "[raum] no script %s\n", path.c_str());
        return;
    }
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#') continue;
        m_script.push_back(line);
    }
    // Its keyboard exists from the start, so clients have bound it before the
    // script types anything.
    scriptKeyboard();
    m_scriptTimer = wl_event_loop_add_timer(loop, &Server::onScriptTimer, this);
    wl_event_source_timer_update(m_scriptTimer, 1500);   // let the output come up first
}

int Server::onScriptTimer(void* data) {
    auto* s = static_cast<Server*>(data);
    while (s->m_scriptPos < s->m_script.size()) {
        std::istringstream in(s->m_script[s->m_scriptPos++]);
        std::string cmd;
        in >> cmd;
        std::string rest;
        std::getline(in, rest);
        if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
        std::fprintf(stderr, "[raum] script: %s %s\n", cmd.c_str(), rest.c_str());
        if (cmd == "wait") {
            wl_event_source_timer_update(s->m_scriptTimer,
                                         std::max(1, static_cast<int>(std::atof(rest.c_str()) * 1000)));
            return 0;
        }
        if (cmd == "shot" || cmd == "outshot") {
            // Go on once the frame with it is drawn (screenshotTaken).
            s->m_scriptAwaitsShot = true;
            if (cmd == "shot") s->takeScreenshot(rest);
            else s->pendingOutputShot = rest;
            s->scheduleFrame();
            return 0;
        }
        s->scriptCommand(cmd, rest);
    }
    return 0;
}

} // namespace raum
