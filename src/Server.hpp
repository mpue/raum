#pragma once

#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "wlr.hpp"

namespace raum {

class Apps;
class Desk;
class DeskRenderer;
class Gpu;
class HostShortcuts;
class HostWindow;
class Hud;
class Output;
class Sfx;
class State;
class Stages;
class SurfaceTextures;
class View;
class XdgPopup;
class XwaylandUnmanaged;

struct Options {
    int  width = 1920, height = 1080;   // nested window size
    std::vector<std::string> autostart; // commands started once the socket is up
    std::string script;                 // --script: a file of timed actions (tests, demos)
    bool privateBus = true;             // nested: clients get a D-Bus of their own
    bool fullscreen = false;            // nested: the window fills its screen
    bool xwayland = true;
};

class Keyboard {
public:
    Keyboard(class Server& server, wlr_keyboard* kb);
    wlr_keyboard* kb;

private:
    void onKey(void*);
    void onModifiers(void*);
    void onDestroy(void*);
    class Server& m_server;
    Listener<Keyboard> m_key, m_modifiers, m_destroy;
};

// The compositor: wlroots' objects, the clients' windows, input and focus.
// What the windows look like and where they are is the Desk's business; the
// server tells it what happened and asks it where things are.
class Server {
public:
    explicit Server(const Options& options);
    ~Server();

    bool start();
    void run();
    void quit();

    // --- For the parts ------------------------------------------------------------
    wl_display*      display = nullptr;
    wl_event_loop*   loop = nullptr;
    wlr_backend*     backend = nullptr;
    wlr_session*     session = nullptr;
    wlr_renderer*    renderer = nullptr;
    wlr_allocator*   allocator = nullptr;
    wlr_compositor*  compositor = nullptr;
    wlr_xdg_shell*   xdgShell = nullptr;
    wlr_output_layout* outputLayout = nullptr;
    wlr_seat*        seat = nullptr;
    wlr_cursor*      cursor = nullptr;
    wlr_xcursor_manager* xcursor = nullptr;
    wlr_xwayland*    xwayland = nullptr;
    std::string      socketName;
    bool             nested = false;

    std::unique_ptr<Gpu>             gpu;
    std::unique_ptr<SurfaceTextures> surfaces;
    std::unique_ptr<Desk>            desk;
    std::unique_ptr<DeskRenderer>    deskRenderer;
    std::unique_ptr<Hud>             hud;
    std::unique_ptr<Apps>            apps;
    std::unique_ptr<State>           state;
    std::unique_ptr<Sfx>             sfx;
    std::unique_ptr<HostShortcuts>   hostShortcuts;
    std::unique_ptr<HostWindow>      hostWindow;   // nested on Wayland: raum's own window
    std::unique_ptr<Stages>          stages;       // clients drawing into a room (Stage.hpp)
    Options options;

    std::vector<std::unique_ptr<View>> views;      // mapped or not
    std::vector<std::unique_ptr<Output>> outputs;
    std::vector<std::unique_ptr<Keyboard>> keyboards;
    std::vector<std::unique_ptr<XwaylandUnmanaged>> unmanaged;

    Output* mainOutput() const;
    // Draw again soon (something moved, committed or was typed).
    void scheduleFrame();
    double now() const;        // CLOCK_MONOTONIC seconds

    // --- Windows ----------------------------------------------------------------
    void addView(std::unique_ptr<View> view);
    void destroyView(View* view);
    void viewMapped(View* view);
    void viewUnmapped(View* view);
    void viewTitleChanged(View* view);
    View* viewOfSurface(wlr_surface* surface) const;
    View* focused() const { return m_focused; }
    // Keyboard focus to `view` (null: nobody). The desk brings it forward.
    void focus(View* view);
    void beginMove(View* view);
    void beginResize(View* view, std::uint32_t edges);
    void popupCreated(XdgPopup* popup);
    void popupDestroyed(XdgPopup* popup);
    void unmanagedDestroyed(XwaylandUnmanaged* u);

    // --- Programs -----------------------------------------------------------------
    // Start a command line (through /bin/sh) as a client of this display.
    void spawn(const std::string& command);
    void takeScreenshot(const std::string& path);   // after the next frame
    // Nested: raum's window between fullscreen and a window.
    void toggleFullscreen();
    void toggleSound();
    void hostResized(int width, int height);
    void screenshotTaken();
    std::string pendingScreenshot;
    std::string pendingOutputShot;   // script: outshot
    bool quitAfterFrame = false;

    // --- Input (InputHandling.cpp) --------------------------------------------
    void keyboardKey(Keyboard& kb, const wlr_keyboard_key_event& e);
    void keyboardModifiers(Keyboard& kb);
    void pointerMoved(std::uint32_t timeMsec);
    // The host window lost the pointer or the desk moved under a resting one.
    void refreshPointerFocus();
    bool superHeld() const;
    void resetCursorImage();

    // What the pointer looks like: an xcursor by name, or a client's surface
    // with its hotspot (null surface and empty name: hidden). Kept for the
    // cursor raum draws itself (softwareCursor).
    struct CursorImage {
        std::string name = "default";
        wlr_surface* surface = nullptr;
        int hotX = 0, hotY = 0;
    };
    CursorImage cursorImage;
    void setCursorImage(const std::string& name, wlr_surface* surface, int hotX, int hotY);
    bool softwareCursor() const;
    // raum draws on the NVIDIA card (and so do the programs, through the
    // dmabuf feedback).
    bool onNvidia() const;

private:
    void onNewOutput(void*);
    void onNewInput(void*);
    void onNewSurface(void*);
    void onNewToplevel(void*);
    void onNewPopup(void*);
    void onCursorMotion(void*);
    void onCursorMotionAbsolute(void*);
    void onCursorButton(void*);
    void onCursorAxis(void*);
    void onCursorFrame(void*);
    void onRequestSetCursor(void*);
    void onRequestSetSelection(void*);
    void onRequestSetPrimarySelection(void*);
    void onCursorShape(void*);
    void onActivate(void*);
    void onNewXwaylandSurface(void*);
    void onXwaylandReady(void*);
    void onCursorSurfaceDestroy(void*);
    void onCursorSurfaceCommit(void*);
    static int onSigchld(int signal, void* data);
    static int onStopSignal(int signal, void* data);
    static int onTimer(void* data);

    bool handleBinding(xkb_keysym_t sym, std::uint32_t mods);
    void pointerButton(std::uint32_t timeMsec, std::uint32_t button, bool pressed);
    void pointerAxis(const wlr_pointer_axis_event& e);
    void setupEnvironment();
    void startPrivateBus();
    void runAutostart();
    void loadScript(const std::string& path);
    void scriptCommand(const std::string& cmd, const std::string& arg);
    wlr_keyboard* scriptKeyboard();
    void scriptKeys(const std::string& combo);   // "super+shift+Left"
    void scriptType(const std::string& text);    // characters, through the layout
    static int onScriptTimer(void* data);

    wlr_xdg_activation_v1* m_activation = nullptr;
    wlr_cursor_shape_manager_v1* m_cursorShape = nullptr;
    wl_event_source* m_sigchld = nullptr;
    std::vector<wl_event_source*> m_stopSignals;
    wl_event_source* m_timer = nullptr;
    View* m_focused = nullptr;
    pid_t m_busPid = -1;
    long m_lastMinute = -1;
    std::vector<std::unique_ptr<XdgPopup>> m_popups;
    std::vector<std::string> m_script;
    std::size_t m_scriptPos = 0;
    wl_event_source* m_scriptTimer = nullptr;
    bool m_scriptAwaitsShot = false;
    // A keyboard for scripts: keys typed through the same path as real ones.
    wlr_keyboard m_scriptKb{};
    bool m_scriptKbReady = false;

    // Pointer state: who it is over, who holds it (a button is down), the grab.
    enum class Grab { None, Client, Move, Resize, Hud, Desk };
    Grab m_grab = Grab::None;
    View* m_grabView = nullptr;
    std::uint32_t m_grabEdges = 0;
    glm::dvec2 m_grabStart{0.0};
    glm::vec2 m_grabOffsetStart{0.0f};
    int m_grabW = 0, m_grabH = 0;
    int m_buttonsDown = 0;
    View* m_pointerView = nullptr;
    bool m_clientCursor = false;
    bool m_superTap = false;          // Super went down with nothing else
    std::uint32_t m_lastModifiers = 0;
    bool m_creatingHostOutput = false;
    int m_windowedW = 1920, m_windowedH = 1080;   // the window's size out of fullscreen
    std::vector<std::uint32_t> m_swallowed;   // keys whose press raum kept
    double m_axisAccum = 0.0;

    Listener<Server> m_newOutput, m_newInput, m_newSurface, m_newToplevel, m_newPopup;
    Listener<Server> m_cursorMotion, m_cursorMotionAbs, m_cursorButton, m_cursorAxis,
        m_cursorFrame;
    Listener<Server> m_requestSetCursor, m_requestSetSelection, m_requestSetPrimarySelection;
    Listener<Server> m_cursorShapeRequest, m_activate;
    Listener<Server> m_newXwaylandSurface, m_xwaylandReady;
    Listener<Server> m_cursorSurfaceDestroy, m_cursorSurfaceCommit;

    friend class Keyboard;
};

} // namespace raum
