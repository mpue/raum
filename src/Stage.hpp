#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

#include "wlr.hpp"

struct wl_global;
struct wl_resource;

namespace raum {

class Server;
class View;

// A camera in raum's world: where the eye is, where it looks, its up, and the
// vertical field of view in degrees.
struct StageCamera {
    glm::vec3 eye{0.0f};
    glm::vec3 front{0.0f, 0.0f, -1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    float     fov = 32.0f;
};

// Where a stage's world lies in its room: the middle of the table's top and
// its width, in raum's units.
struct StageTable {
    glm::vec3 centre{0.0f};
    float     width = 1.0f;
};

// A client that draws its world into one of raum's rooms
// (protocols/raum-stage-v1.xml). While its room is at work and in front, the
// stage is an ordinary window; leant back, the window goes and the client
// draws from raum's camera, onto the table, and DeskRenderer sets that picture
// into the room.
class Stage {
public:
    Stage(Server& server, wl_resource* resource, wlr_surface* surface);
    ~Stage();
    Stage(const Stage&)            = delete;
    Stage& operator=(const Stage&) = delete;

    wlr_surface* surface() const { return m_surface; }
    View* view() const;

    // Ask the client to draw from `cam` (a diorama) or, with `own`, from its
    // own camera. Sends only what changed since the last call.
    void ask(bool own, const StageCamera& cam, const StageTable& table);

    // The picture the surface holds now: drawn from raum's camera (`shownSerial`
    // != 0) or from the client's own. `shownCam` and `shownTable` are what it
    // was drawn with -- the room around it is drawn from the same camera.
    std::uint32_t shownSerial = 0;
    StageCamera   shownCam;
    StageTable    shownTable;
    bool          shownKnown = false;   // a frame request came with the buffer
    // Where the client's own eye is, in raum's world: the fly-in goes there.
    StageCamera   ownCam;
    bool          ownKnown = false;

    // --- From the protocol ----------------------------------------------------------
    void frameRequested(std::uint32_t serial, const float* cams, std::size_t n);
    void resourceGone();

private:
    void onCommit(void*);
    void onSurfaceDestroy(void*);

    Server&      m_server;
    wl_resource* m_resource;
    wlr_surface* m_surface;
    // The frame request waits for its commit.
    bool          m_pending = false;
    std::uint32_t m_pendingSerial = 0;
    StageCamera   m_pendingDrawn, m_pendingOwn;
    // What raum asked for, by serial: the last few, so a picture's camera and
    // table are known when it arrives.
    struct Sent { std::uint32_t serial; StageCamera cam; StageTable table; };
    std::vector<Sent> m_sent;
    std::uint32_t m_serial = 0;
    bool          m_askedOwn = true;
    StageCamera   m_askedCam;
    StageTable    m_askedTable;
    bool          m_askedAny = false;
    Listener<Stage> m_commit, m_surfaceDestroy;
};

// The protocol's global, and every stage there is.
class Stages {
public:
    explicit Stages(Server& server);
    ~Stages();

    Stage* ofView(const View* view) const;
    Stage* ofSurface(const wlr_surface* surface) const;

    // --- From the protocol ----------------------------------------------------------
    void create(wl_resource* resource, wlr_surface* surface);
    void destroyed(Stage* stage);

private:
    Server& m_server;
    wl_global* m_global = nullptr;
    std::vector<std::unique_ptr<Stage>> m_stages;
};

} // namespace raum
