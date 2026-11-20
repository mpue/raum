#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <xkbcommon/xkbcommon.h>

#include "RingMenu.hpp"

struct ImDrawList;
struct ImFont;
struct ImGuiContext;

namespace raum {

class Server;

// Everything raum draws flat on top of the room, with Dear ImGui, in the
// holographic style of Theme.hpp: the HUD strip at the top (rooms, readouts,
// the clock), the labels that name windows and rooms when the camera has
// stepped back, the ring menu (RingMenu), renaming a room, the help card, the
// short notes that confirm what a key did, and the streaks when the camera
// flies.
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
    bool launcherOpen() const { return m_ring.isOpen(); }
    // Start a program (an index into Apps::all()) in a room.
    void launch(int app, int room);

private:
    enum class Mode { None, Rename, Help, Quit };
    void drawTopBar(int w);
    void drawLabels(int w, int h);
    void drawWarp(int w, int h);
    void drawRename(int w, int h);
    void drawHelp(int w, int h);
    void drawQuit(int w, int h);
    void drawToasts(int w, int h);
    void drawEmptyRoom(int w, int h);
    void cardFrame(ImDrawList* dl, float x0, float y0, float x1, float y1, const char* title);
    void readSystem();

    Server& m_server;
    ImGuiContext* m_ctx = nullptr;
    RingMenu::Fonts m_fonts;
    RingMenu m_ring;
    Mode m_mode = Mode::None;
    double m_modeSince = 0.0;
    int m_targetRoom = 0;
    char m_rename[128] = {};
    bool m_focusInput = false;
    bool m_typedEarly = false;
    struct Toast { std::string text; double age = 0.0; };
    std::vector<Toast> m_toasts;
    double m_time = 0.0;
    bool m_hadInput = false;
    // Enter in a card: acted on in the next frame, once ImGui has put the
    // characters typed before it into the field.
    bool m_accept = false;
    // The readouts in the strip: processor and memory, now and then.
    double m_sysAt = -10.0;
    float m_cpu = 0.0f, m_mem = 0.0f;
    unsigned long long m_cpuIdle = 0, m_cpuTotal = 0;
    int m_hoverChip = -2;   // for the hover sound: which control the pointer is on
};

} // namespace raum
