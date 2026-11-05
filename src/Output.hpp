#pragma once

#include "wlr.hpp"

namespace raum {

class Server;

// A screen. Each frame raum's own GL context draws the room into a buffer of
// the output's swapchain (allocated by wlroots, imported here as a framebuffer)
// and hands it back for display: on a real session that is a KMS plane, nested
// it is a buffer attached to the host compositor's window.
class Output {
public:
    Output(Server& server, wlr_output* output);
    ~Output();
    Output(const Output&)            = delete;
    Output& operator=(const Output&) = delete;

    wlr_output* wlr;
    int width() const { return wlr->width; }
    int height() const { return wlr->height; }
    void scheduleFrame();

private:
    void onFrame(void*);
    void onRequestState(void*);
    void onDestroy(void*);
    unsigned framebufferFor(wlr_buffer* buffer);

    Server& m_server;
    double m_lastFrame = 0.0;
    Listener<Output> m_frame, m_requestState, m_destroy;
};

} // namespace raum
