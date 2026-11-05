#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <xkbcommon/xkbcommon.h>

struct ImFont;
struct ImGuiContext;

namespace raum {

class Server;

// Everything raum draws flat on top of the room, with Dear ImGui: the strip at
// the top (rooms, clock), the labels that name windows and rooms when the
// camera has stepped back, the starter, renaming a room, the help card and the
// short notes that confirm what a key did.
//
// ImGui gets no platform backend here -- there is no GLFW window -- so the
// server hands it pointer and keys itself, and only while the HUD wants them.
class Hud {
public:
    explicit Hud(Server& server);
    ~Hud();

    void render(int width, int height, double dt);
    bool animating() const;

    // Input: true when the HUD took it.
    void pointerMotion(double x, double y);
    bool pointerButton(std::uint32_t button, bool pressed);
    bool pointerAxis(double dx, double dy);
    bool key(xkb_keysym_t sym, bool pressed, std::uint32_t mods, const char* utf8);
    bool wantsPointer() const;
    bool wantsKeyboard() const;

    void toast(const std::string& text);
    void clearToasts() { m_toasts.clear(); }
    void openLauncher(int room = -1);
    void openRename(int room);
    void toggleHelp();
    void askQuit();
    void close();   // whatever card is open
    bool launcherOpen() const { return m_mode == Mode::Launcher; }

private:
    enum class Mode { None, Launcher, Rename, Help, Quit };
    void drawTopBar(int w);
    void drawLabels(int w, int h);
    void drawLauncher(int w, int h);
    void drawRename(int w, int h);
    void drawHelp(int w, int h);
    void drawQuit(int w, int h);
    void drawToasts(int w, int h);
    void drawEmptyRoom(int w, int h);
    void launch(int index);

    Server& m_server;
    ImGuiContext* m_ctx = nullptr;
    ImFont* m_font = nullptr;
    ImFont* m_bold = nullptr;
    Mode m_mode = Mode::None;
    int m_targetRoom = 0;
    char m_query[256] = {};
    char m_rename[128] = {};
    int m_selected = 0;
    bool m_focusInput = false;
    bool m_typedEarly = false;
    std::vector<int> m_matches;
    struct Toast { std::string text; double age = 0.0; };
    std::vector<Toast> m_toasts;
    double m_time = 0.0;
    double m_lastClockMinute = -1.0;
    bool m_hadInput = false;
    // Enter in a card: acted on in the next frame, once ImGui has put the
    // characters typed before it into the field.
    bool m_accept = false;
};

} // namespace raum
