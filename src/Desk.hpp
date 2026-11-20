#pragma once

#include <array>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "Stage.hpp"
#include "View.hpp"

namespace raum {

class Server;

// A room: one project, task or context. Its windows live in it; the rooms stand
// side by side along the hall.
struct Room {
    std::string name;
    glm::vec3   accent{0.25f, 0.72f, 0.69f};
    int         split = 1;                 // windows side by side at the front
    std::vector<std::string> recentApps;   // desktop ids started here, newest first
};

// How far the camera has stepped back.
//   Work:  the front windows at 1:1, pixel for pixel; the rest of the room
//          waits behind them in depth, most recent nearest.
//   Room:  the camera leans back and the room's history fans out along the
//          floor, ordered by when each window was last used.
//   House: every room at once, side by side; windows move between rooms here.
enum class Level { Work = 0, Room = 1, House = 2 };

// The 3D desktop as a model: rooms, which window is where and why, the camera,
// and the arithmetic that maps a screen pixel to a window pixel and back.
//
// World units are the front plane's pixels: at Level::Work the camera sits at
// distance D on +Z looking down -Z, and D is chosen so that one unit at z = 0
// is exactly one screen pixel. A window placed there with scale 1 and its
// corners on whole pixels is sampled texel for texel -- text stays as sharp as
// on any flat desktop. Everything else (depth, the floor, the other rooms) is
// arranged around that one plane.
class Desk {
public:
    explicit Desk(Server& server);

    // --- Rooms --------------------------------------------------------------------
    std::vector<Room> rooms;
    int current = 0;
    Level level = Level::Work;

    void goToRoom(int index);
    void stepRoom(int dir);
    int  newRoom(const std::string& name = {});
    void renameRoom(int index, const std::string& name);
    void deleteRoomIfEmpty(int index);
    void moveViewToRoom(View* view, int room, bool follow);
    void setLevel(Level l);
    void zoom(int dir);           // +1 out (Work -> Room -> House), -1 in
    void dolly(float steps);      // walk along the room's history (Level::Room)
    void cycle(int dir);          // the next/previous window of the room's history
    void setSplit(int n);
    int  windowCount(int room) const;
    // The room's windows, most recently used first (top-level ones only).
    std::vector<View*> history(int room) const;

    // --- Told by the server ---------------------------------------------------------
    void viewMapped(View* view);
    void viewUnmapped(View* view);
    void focused(View* view);
    View* mostRecent(int room, const View* except = nullptr) const;

    // --- Per frame -----------------------------------------------------------------
    void update(double now, double dt, int width, int height);
    bool animating() const { return m_animating; }
    // Back to front, everything worth drawing this frame.
    const std::vector<View*>& drawList() const { return m_drawList; }

    // --- Geometry -------------------------------------------------------------------
    int   width() const { return m_w; }
    int   height() const { return m_h; }
    int   topBar() const;                    // the HUD's strip at the top, pixels
    float frontDistance() const { return m_D; }
    float fovY() const { return m_fovY; }
    float floorY() const;
    float roomX(int index) const;
    float roomPitch() const;
    const glm::mat4& viewMatrix() const { return m_view; }
    const glm::mat4& projMatrix() const { return m_proj; }
    glm::vec3 cameraPos() const { return m_drawEye; }
    // A world point on the screen (pixels, y down); z < 0 behind the camera.
    glm::vec3 toScreen(const glm::vec3& world) const;
    // The canvas quad's corners (TL, TR, BR, BL) for a pose.
    void canvasQuad(const View* view, const Pose& pose, glm::vec3 out[4]) const;
    // The point of `view`'s plane under screen pixel (x, y), in main-surface
    // coordinates; false when the ray misses the plane.
    bool pointOnView(const View* view, double x, double y, double& mx, double& my) const;
    // The window pixel rect of `view` on screen at Work level (front only).
    bool frontRect(const View* view, wlr_box& out) const;

    struct Hit {
        View* view = nullptr;
        wlr_surface* surface = nullptr;   // null: on the window but no surface takes input there
        double sx = 0, sy = 0;            // in `surface`
        double mx = 0, my = 0;            // in the main surface
        bool front = false;               // a front window at Level::Work: input goes to it
    };
    Hit pick(double x, double y) const;
    int pickRoom(double x, double y) const;   // House level: the room whose floor is there

    View* hovered = nullptr;
    // Being dragged to another room at House level (drawn under the pointer).
    View* dragging = nullptr;
    glm::dvec2 dragPointer{0.0};

    double now() const { return m_now; }
    double wallNow() const;

    // --- Effects -----------------------------------------------------------------------
    // A window that went: its last picture, folding away (DeskRenderer draws and
    // frees it).
    struct Ghost {
        Canvas canvas;
        Pose pose;
        wlr_box geometry{};
        double start = 0.0;
    };
    std::vector<Ghost> ghosts;
    void addGhost(View* view);
    // A ring of light running out over the floor from a point.
    struct Wave {
        glm::vec2 origin{0.0f};   // x, z on the floor
        double start = -100.0;
        float strength = 1.0f;
    };
    std::array<Wave, 3> waves{};
    void wave(const glm::vec3& at, float strength = 1.0f);
    glm::vec3 cameraVelocity() const { return m_camVel; }
    // Something happened: the slow motion of the hall (stars, the HUD's dials)
    // runs for a while after, then the picture stands still and costs nothing.
    void poke() { m_lastActivity = m_now; }
    bool ambient() const { return m_now - m_lastActivity < 6.0; }

    // --- Stages (Stage.hpp) ---------------------------------------------------------
    // The stage drawn as its room's diorama this frame -- its picture, set into
    // the room from the camera it was drawn with -- or null.
    Stage* diorama() const { return m_diorama; }

private:
    void layout();
    void layoutRoom(int room);
    void placeChildren();
    Pose exactPose(double x0, double y0, int w, int h, float z) const;
    void cameraTarget(glm::vec3& pos, glm::vec3& look) const;
    void assignSlots(int room, const std::vector<View*>& order);

    // A stage's way between its window and its room's diorama.
    //   Window:  an ordinary window, drawn from the game's own camera.
    //   Leaving: the camera is about to lean back. The level waits, and the
    //            window stays, until the first picture from raum's camera --
    //            the game's own eye, without its HUD -- has arrived; then the
    //            camera starts out from exactly there, and the change is not
    //            seen.
    //   Diorama: its picture in the room, drawn from raum's camera.
    //   Landing: back to work: the camera flies into the game's own eye, and
    //            there the window takes over again.
    enum class StageMode { Window, Leaving, Diorama, Landing };
    Stage* stageOf(int room) const;          // the room's stage, its window mapped
    bool stageInFront() const;               // the current room's, in front at work
    StageTable tableTarget(int room) const;
    void updateStage();
    void askStage();
    StageMode  m_stageMode = StageMode::Window;
    Level      m_leaveTo = Level::Room;
    double     m_leavingSince = 0.0;
    double     m_landingSince = 0.0;
    Stage*     m_stage = nullptr;            // the stage this frame looks after
    int        m_stageRoom = -1;
    Stage*     m_diorama = nullptr;
    StageTable m_table;
    bool       m_tableValid = false;
    float      m_fov = 32.0f;                // as drawn; the game's own while flying to or from it
    glm::vec3  m_drawEye{0.0f};              // the eye of the camera drawn with

    Server& m_server;
    int   m_w = 1920, m_h = 1080;
    float m_fovY = 32.0f;            // degrees
    float m_D = 1884.0f;
    float m_dolly = 0.0f, m_dollyTarget = 0.0f;
    glm::vec3 m_camPos{0.0f}, m_camLook{0.0f, 0.0f, -1.0f};
    glm::vec3 m_camPosTarget{0.0f}, m_camLookTarget{0.0f, 0.0f, -1.0f};
    bool  m_camValid = false;
    glm::mat4 m_view{1.0f}, m_proj{1.0f};
    double m_now = 0.0;
    bool  m_animating = true;
    glm::vec3 m_camVel{0.0f}, m_camPrev{0.0f};
    double m_lastActivity = 0.0;
    View* m_lockRoot = nullptr;   // whose brackets locked on last
    std::vector<View*> m_drawList;
};

} // namespace raum
