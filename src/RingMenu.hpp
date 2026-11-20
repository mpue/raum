#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <xkbcommon/xkbcommon.h>

struct ImDrawList;
struct ImFont;

namespace raum {

class Server;
struct App;

// The menu: a ring of light round the middle of the screen.
//
//   core      the selected program, large, and what has been typed
//   inner     the categories, one segment each
//   outer     the programs of the category -- a dial: the one at the top is
//             the selected one, the arrows and the wheel turn it
//   below     raum's own actions (a new room, a screenshot, sound, help, quit)
//
// Typing searches every program at once. It draws itself on the HUD's
// foreground layer and takes keys and the pointer while it is open.
class RingMenu {
public:
    struct Fonts {
        ImFont* text = nullptr;
        ImFont* bold = nullptr;
        ImFont* display = nullptr;   // headings in spaced capitals
        ImFont* mono = nullptr;
    };

    explicit RingMenu(Server& server);

    void open(int room);
    void close();
    bool isOpen() const { return m_state != State::Closed; }
    bool animating() const { return isOpen(); }

    void draw(ImDrawList* dl, const Fonts& f, int width, int height, double now);

    bool key(xkb_keysym_t sym, bool pressed, std::uint32_t mods, const char* utf8);
    void pointerMotion(double x, double y);
    bool pointerButton(std::uint32_t button, bool pressed);
    bool pointerAxis(double dy);

private:
    enum class State { Closed, Opening, Open, Launching, Closing };
    struct Category { const char* name; int glyph; std::vector<int> apps; };
    enum Action { NewRoom, Screenshot, Sound, Help, Quit, ActionCount };

    void rebuild();                 // categories and the list for the query
    const std::vector<int>& list() const;
    void select(int index, bool sound);
    void turn(int steps);
    void launchSelected();
    void launch(int app);
    void runAction(int a);
    int  slots() const;

    Server& m_server;
    State  m_state = State::Closed;
    double m_t0 = 0.0;              // when the state began
    double m_now = 0.0;
    int    m_room = 0;
    std::string m_query;
    std::vector<Category> m_cats;
    std::vector<int> m_found;       // the search's results, while typing
    bool   m_recentIsDefault = false; // "Zuletzt" is a suggestion: nothing started yet
    int    m_cat = 0;
    int    m_sel = 0;
    float  m_rot = 0.0f;            // the dial as drawn, in slots
    int    m_hoverApp = 0, m_hoverCat = -1, m_hoverAction = -1;
    bool   m_onApp = false;           // the pointer is on a program (m_hoverApp: its place)
    bool   m_hoverCore = false;
    int    m_launchApp = -1;
    double m_mx = -1, m_my = -1;
    float  m_cx = 0, m_cy = 0, m_u = 1;   // centre and scale of the last draw
};

} // namespace raum
