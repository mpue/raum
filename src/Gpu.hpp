#pragma once

#include <memory>

#include "wlr.hpp"

namespace fitzel::rhi { class Device; }

namespace raum {

// The OpenGL context raum draws with, and what it takes to draw with images
// other processes made.
//
// The context is desktop OpenGL (4.3 core, 3.3 at worst) -- fitzel's -- on the
// EGL display wlroots' renderer opened for the GPU, so both speak to the same
// device and every buffer one can see the other can import. It has no surface:
// what it draws into is a buffer of the output's swapchain, imported as an
// image (outputFramebuffer), and what it samples are the clients' buffers,
// imported the same way (dmabufs) or uploaded (shared memory, SurfaceTextures).
//
// wlroots' own renderer is a GLES context on the same thread and makes itself
// current when it draws (the cursor plane, for one). Everything here that calls
// GL starts with makeCurrent().
class Gpu {
public:
    explicit Gpu(wlr_renderer* renderer);
    ~Gpu();
    Gpu(const Gpu&)            = delete;
    Gpu& operator=(const Gpu&) = delete;

    bool ok() const { return m_device != nullptr; }
    void makeCurrent();
    fitzel::rhi::Device& device() { return *m_device; }

    // An image over a dmabuf; EGL_NO_IMAGE_KHR when the driver will not take it.
    EGLImageKHR importDmabuf(const wlr_dmabuf_attributes& a);
    void destroyImage(EGLImageKHR image);
    // A GL texture whose storage is `image` (0 on failure).
    unsigned textureFromImage(EGLImageKHR image);
    // A framebuffer object drawing into `image` through a renderbuffer; both
    // names come back, the framebuffer 0 on failure.
    unsigned framebufferFromImage(EGLImageKHR image, unsigned& renderbuffer);
    void deleteFramebuffer(unsigned fbo, unsigned renderbuffer);

    // Wait for the GPU: everything drawn so far is in memory. The buffers are
    // handed to another process (the display, the host compositor) and nothing
    // here shares a fence with it, so a frame ends with this.
    void finish();
    // Make the GPU wait for a sync_file (a client's rendering) before what is
    // drawn next; takes the fd. Falls back to waiting on the CPU.
    void waitFence(int syncFileFd);

private:
    EGLDisplay m_display = EGL_NO_DISPLAY;
    EGLContext m_context = EGL_NO_CONTEXT;
    std::unique_ptr<fitzel::rhi::Device> m_device;
    bool m_modifiers = false;   // EGL_EXT_image_dma_buf_import_modifiers

    PFNEGLCREATEIMAGEKHRPROC  m_createImage  = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC m_destroyImage = nullptr;
    using ImageTargetFn = void (*)(unsigned target, void* image);
    ImageTargetFn m_imageTargetTexture      = nullptr;   // glEGLImageTargetTexture2DOES
    ImageTargetFn m_imageTargetRenderbuffer = nullptr;   // glEGLImageTargetRenderbufferStorageOES
    PFNEGLCREATESYNCKHRPROC  m_createSync  = nullptr;
    PFNEGLDESTROYSYNCKHRPROC m_destroySync = nullptr;
    PFNEGLWAITSYNCKHRPROC    m_waitSync    = nullptr;
};

} // namespace raum
