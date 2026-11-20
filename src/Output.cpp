#include "Output.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>

#include <fitzel/rhi/Device.hpp>

#include "Desk.hpp"
#include "DeskRenderer.hpp"
#include "Gpu.hpp"
#include "HostShortcuts.hpp"
#include "Hud.hpp"
#include "Server.hpp"
#include "View.hpp"

namespace raum {

namespace {

// A swapchain buffer as a framebuffer of raum's context; lives as long as the
// buffer (the swapchain is rebuilt on a resize, and its old buffers go).
struct OutputTarget {
    wlr_addon addon{};
    Gpu* gpu = nullptr;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    unsigned fbo = 0, rb = 0;
};

void destroyTarget(wlr_addon* addon) {
    auto* t = reinterpret_cast<OutputTarget*>(addon);
    t->gpu->makeCurrent();
    // Never leave the device pointing at a framebuffer that is about to go.
    t->gpu->device().setWindowFramebuffer(0, 1, 1);
    t->gpu->device().bindFramebuffer(fitzel::rhi::FramebufferHandle{});
    t->gpu->deleteFramebuffer(t->fbo, t->rb);
    t->gpu->destroyImage(t->image);
    wlr_addon_finish(&t->addon);
    delete t;
}

const wlr_addon_interface kTargetImpl = {"raum-output-target", destroyTarget};

} // namespace

Output::Output(Server& server, wlr_output* output) : wlr(output), m_server(server) {
    m_frame.connect(&output->events.frame, this, &Output::onFrame);
    m_requestState.connect(&output->events.request_state, this, &Output::onRequestState);
    m_destroy.connect(&output->events.destroy, this, &Output::onDestroy);
    m_lastFrame = server.now();
}

Output::~Output() = default;

void Output::scheduleFrame() { wlr_output_schedule_frame(wlr); }

unsigned Output::framebufferFor(wlr_buffer* buffer) {
    Gpu& gpu = *m_server.gpu;
    if (wlr_addon* a = wlr_addon_find(&buffer->addons, this, &kTargetImpl))
        return reinterpret_cast<OutputTarget*>(a)->fbo;
    wlr_dmabuf_attributes attribs{};
    if (!wlr_buffer_get_dmabuf(buffer, &attribs)) {
        std::fprintf(stderr, "[raum] the output's buffer is not a dmabuf\n");
        return 0;
    }
    auto* t = new OutputTarget;
    t->gpu = &gpu;
    t->image = gpu.importDmabuf(attribs);
    t->fbo = gpu.framebufferFromImage(t->image, t->rb);
    gpu.device().resyncState();
    wlr_addon_init(&t->addon, &buffer->addons, this, &kTargetImpl);
    return t->fbo;
}

void Output::onFrame(void*) {
    Server& s = m_server;
    if (!s.gpu || !s.gpu->ok()) return;
    if (s.mainOutput() != this) return;   // one screen for now (README)
    if (s.hostShortcuts) s.hostShortcuts->dispatch();

    wlr_output_state state;
    wlr_output_state_init(&state);
    if (!wlr_output_configure_primary_swapchain(wlr, &state, &wlr->swapchain)) {
        wlr_output_state_finish(&state);
        return;
    }
    wlr_buffer* buffer = wlr_swapchain_acquire(wlr->swapchain);
    if (!buffer) {
        wlr_output_state_finish(&state);
        return;
    }
    const int w = buffer->width, h = buffer->height;
    const double now = s.now();
    const double dt = std::clamp(now - m_lastFrame, 0.0, 0.25);
    m_lastFrame = now;

    s.gpu->makeCurrent();
    fitzel::rhi::Device& dev = s.gpu->device();
    const unsigned fbo = framebufferFor(buffer);
    dev.setWindowFramebuffer(fbo, w, h);

    s.desk->update(now, dt, w, h);
    s.deskRenderer->render(w, h);
    s.deskRenderer->bindScene();
    s.hud->render(w, h, dt);
    s.deskRenderer->drawCursor(s.cursor->x, s.cursor->y);
    if (!s.pendingScreenshot.empty()) {
        s.deskRenderer->savePng(s.pendingScreenshot);
        s.pendingScreenshot.clear();
        s.screenshotTaken();
    }
    if (fbo) s.deskRenderer->present();
    if (!s.pendingOutputShot.empty() && fbo) {
        s.deskRenderer->saveOutputPng(s.pendingOutputShot);
        s.pendingOutputShot.clear();
        s.screenshotTaken();
    }
    s.gpu->finish();

    wlr_output_state_set_buffer(&state, buffer);
    wlr_buffer_unlock(buffer);
    if (!wlr_output_commit_state(wlr, &state))
        std::fprintf(stderr, "[raum] output commit failed\n");
    wlr_output_state_finish(&state);

    // Every client that showed gets to draw its next frame.
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    for (const auto& v : s.views) {
        if (!v->mapped()) continue;
        v->forEachSurface([&](wlr_surface* surface, int, int) { wlr_surface_send_frame_done(surface, &ts); });
    }
    if (s.quitAfterFrame) {
        s.quitAfterFrame = false;
        s.quit();
        return;
    }
    if (s.desk->animating() || s.hud->animating()) scheduleFrame();
}

void Output::onRequestState(void* data) {
    // Nested: the host resized raum's window.
    auto* e = static_cast<wlr_output_event_request_state*>(data);
    wlr_output_commit_state(wlr, e->state);
    m_server.scheduleFrame();
}

void Output::onDestroy(void*) {
    m_frame.disconnect();
    m_requestState.disconnect();
    m_destroy.disconnect();
    Server& s = m_server;
    auto& outs = s.outputs;
    outs.erase(std::remove_if(outs.begin(), outs.end(),
                              [this](const std::unique_ptr<Output>& o) { return o.get() == this; }),
               outs.end());
    // Nested, the host's window was raum's screen: closing it ends raum.
    if (outs.empty() && s.nested) s.quit();
}

} // namespace raum
