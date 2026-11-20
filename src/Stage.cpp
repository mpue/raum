#include "Stage.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "raum-stage-v1-protocol.h"

#include "Server.hpp"
#include "View.hpp"

namespace raum {

namespace {

Stage* stageOf(wl_resource* r) { return static_cast<Stage*>(wl_resource_get_user_data(r)); }

void stageDestroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

void stageFrame(wl_client*, wl_resource* r, std::uint32_t serial, wl_array* camera) {
    if (Stage* s = stageOf(r))
        s->frameRequested(serial, static_cast<const float*>(camera->data), camera->size / sizeof(float));
}

const struct raum_stage_v1_interface kStageImpl = {stageDestroy, stageFrame};

void stageResourceDestroyed(wl_resource* r) {
    if (Stage* s = stageOf(r)) s->resourceGone();
}

void managerDestroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

void managerGetStage(wl_client* client, wl_resource* r, std::uint32_t id, wl_resource* surfaceRes) {
    auto* stages = static_cast<Stages*>(wl_resource_get_user_data(r));
    wlr_surface* surface = wlr_surface_from_resource(surfaceRes);
    if (stages->ofSurface(surface)) {
        wl_resource_post_error(r, RAUM_STAGE_MANAGER_V1_ERROR_ALREADY_STAGED,
                               "the surface is a stage already");
        return;
    }
    wl_resource* res = wl_resource_create(client, &raum_stage_v1_interface, wl_resource_get_version(r), id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    stages->create(res, surface);
}

const struct raum_stage_manager_v1_interface kManagerImpl = {managerDestroy, managerGetStage};

void bindManager(wl_client* client, void* data, std::uint32_t version, std::uint32_t id) {
    wl_resource* r = wl_resource_create(client, &raum_stage_manager_v1_interface,
                                        static_cast<int>(std::min<std::uint32_t>(version, 1)), id);
    if (!r) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(r, &kManagerImpl, data, nullptr);
}

StageCamera readCamera(const float* f) {
    StageCamera c;
    c.eye = {f[0], f[1], f[2]};
    c.front = {f[3], f[4], f[5]};
    c.up = {f[6], f[7], f[8]};
    c.fov = f[9];
    return c;
}

bool near(const glm::vec3& a, const glm::vec3& b, float eps) { return glm::length(a - b) <= eps; }

} // namespace

// --- Stage -------------------------------------------------------------------------

Stage::Stage(Server& server, wl_resource* resource, wlr_surface* surface)
    : m_server(server), m_resource(resource), m_surface(surface) {
    wl_resource_set_implementation(resource, &kStageImpl, this, stageResourceDestroyed);
    m_commit.connect(&surface->events.commit, this, &Stage::onCommit);
    m_surfaceDestroy.connect(&surface->events.destroy, this, &Stage::onSurfaceDestroy);
}

Stage::~Stage() {
    if (m_resource) wl_resource_set_user_data(m_resource, nullptr);
}

View* Stage::view() const { return m_server.viewOfSurface(m_surface); }

void Stage::ask(bool own, const StageCamera& cam, const StageTable& table) {
    const bool sameTable = m_askedAny && near(table.centre, m_askedTable.centre, 0.01f) &&
                           std::abs(table.width - m_askedTable.width) < 0.01f;
    const bool sameCam = near(cam.eye, m_askedCam.eye, 0.01f) && near(cam.front, m_askedCam.front, 1e-6f) &&
                         near(cam.up, m_askedCam.up, 1e-6f) && std::abs(cam.fov - m_askedCam.fov) < 1e-4f;
    if (sameTable && own == m_askedOwn && (own || sameCam)) return;
    m_askedAny = true;
    m_askedOwn = own;
    m_askedCam = cam;
    m_askedTable = table;
    std::uint32_t serial = 0;
    if (!own) {
        serial = ++m_serial;
        if (serial == 0) serial = ++m_serial;
        m_sent.push_back({serial, cam, table});
        if (m_sent.size() > 32) m_sent.erase(m_sent.begin());
    }
    const float v[14] = {cam.eye.x,   cam.eye.y,   cam.eye.z,   cam.front.x,     cam.front.y,
                         cam.front.z, cam.up.x,    cam.up.y,    cam.up.z,        cam.fov,
                         table.centre.x, table.centre.y, table.centre.z, table.width};
    wl_array a;
    wl_array_init(&a);
    if (void* p = wl_array_add(&a, sizeof(v))) std::memcpy(p, v, sizeof(v));
    raum_stage_v1_send_camera(m_resource, serial, &a);
    wl_array_release(&a);
}

void Stage::frameRequested(std::uint32_t serial, const float* cams, std::size_t n) {
    if (n < 20) return;
    m_pending = true;
    m_pendingSerial = serial;
    m_pendingDrawn = readCamera(cams);
    m_pendingOwn = readCamera(cams + 10);
}

void Stage::onCommit(void*) {
    // Only a new picture changes what is shown; a commit without a buffer
    // (a title, a configure ack) keeps the camera the picture has.
    if (!(m_surface->current.committed & WLR_SURFACE_STATE_BUFFER)) return;
    if (!m_pending) {
        shownSerial = 0;
        shownKnown = false;
        return;
    }
    m_pending = false;
    shownSerial = m_pendingSerial;
    shownCam = m_pendingDrawn;
    ownCam = m_pendingOwn;
    ownKnown = true;
    shownKnown = shownSerial == 0;
    for (const Sent& s : m_sent)
        if (s.serial == shownSerial) {
            shownTable = s.table;
            shownKnown = true;
        }
    m_server.scheduleFrame();
}

void Stage::onSurfaceDestroy(void*) {
    m_commit.disconnect();
    m_surfaceDestroy.disconnect();
    m_server.stages->destroyed(this);
}

void Stage::resourceGone() {
    m_resource = nullptr;
    m_server.stages->destroyed(this);
}

// --- Stages ------------------------------------------------------------------------

Stages::Stages(Server& server) : m_server(server) {
    m_global = wl_global_create(server.display, &raum_stage_manager_v1_interface, 1, this, bindManager);
}

Stages::~Stages() {
    m_stages.clear();
    if (m_global) wl_global_destroy(m_global);
}

Stage* Stages::ofView(const View* view) const {
    if (!view) return nullptr;
    for (const auto& s : m_stages)
        if (s->surface() == view->surface()) return s.get();
    return nullptr;
}

Stage* Stages::ofSurface(const wlr_surface* surface) const {
    for (const auto& s : m_stages)
        if (s->surface() == surface) return s.get();
    return nullptr;
}

void Stages::create(wl_resource* resource, wlr_surface* surface) {
    m_stages.push_back(std::make_unique<Stage>(m_server, resource, surface));
    std::fprintf(stderr, "[raum] a stage: its world goes into the room when the camera leans back\n");
    m_server.scheduleFrame();
}

void Stages::destroyed(Stage* stage) {
    for (auto it = m_stages.begin(); it != m_stages.end(); ++it)
        if (it->get() == stage) {
            m_stages.erase(it);
            m_server.scheduleFrame();
            return;
        }
}

} // namespace raum
