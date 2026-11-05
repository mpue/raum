#pragma once

#include <cstdint>

#include <fitzel/rhi/Types.hpp>

#include "wlr.hpp"

namespace raum {

class Gpu;

// What a client surface shows, as a texture raum can draw with.
//
// Every wl_surface gets a small record of its own the moment it is created
// (track), and that record follows every commit: a dmabuf is imported once per
// buffer and kept with the buffer (a client cycling three buffers costs three
// imports, ever); shared memory is copied into one texture per surface, only
// the rectangles the client said it changed. That copy has to happen AT the
// commit, not when the frame is drawn -- damage is relative to the content
// before it, and two commits between two frames would otherwise lose the first
// one's.
class SurfaceTextures {
public:
    struct Sampled {
        fitzel::rhi::TextureHandle texture;   // invalid: nothing to show
        int  width = 0, height = 0;           // the buffer's pixels
        bool swapRB = false;                  // shm in B,G,R,A byte order
        bool opaque = false;                  // ignore alpha (an X-format buffer)
        std::uint32_t version = 0;            // changes with every commit
    };

    explicit SurfaceTextures(Gpu& gpu) : m_gpu(gpu) {}

    void track(wlr_surface* surface);
    Sampled sampled(wlr_surface* surface) const;
    // Before drawing the surface: have the GPU wait until the client's
    // rendering into its current buffer is done.
    void waitReady(wlr_surface* surface);

    Gpu& gpu() { return m_gpu; }

private:
    Gpu& m_gpu;
};

} // namespace raum
