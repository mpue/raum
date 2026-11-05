#include "Gpu.hpp"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include <poll.h>
#include <unistd.h>

#include <glad/gl.h>

#include <fitzel/rhi/Device.hpp>

namespace raum {

namespace {

void* loadProc(const char* name) { return reinterpret_cast<void*>(eglGetProcAddress(name)); }

bool hasExtension(EGLDisplay d, const char* ext) {
    const char* all = eglQueryString(d, EGL_EXTENSIONS);
    if (!all) return false;
    const std::size_t n = std::strlen(ext);
    for (const char* p = all; (p = std::strstr(p, ext)); p += n)
        if ((p == all || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0')) return true;
    return false;
}

} // namespace

Gpu::Gpu(wlr_renderer* renderer) {
    if (!wlr_renderer_is_gles2(renderer)) {
        std::fprintf(stderr, "[raum] wlroots picked a renderer that is not GLES2 -- "
                             "start with WLR_RENDERER=gles2\n");
        return;
    }
    m_display = wlr_egl_get_display(wlr_gles2_renderer_get_egl(renderer));
    if (!hasExtension(m_display, "EGL_KHR_no_config_context") &&
        !hasExtension(m_display, "EGL_MESA_configless_context"))
        std::fprintf(stderr, "[raum] EGL has no config-less contexts; trying anyway\n");
    m_modifiers = hasExtension(m_display, "EGL_EXT_image_dma_buf_import_modifiers");

    if (!eglBindAPI(EGL_OPENGL_API)) {
        std::fprintf(stderr, "[raum] EGL cannot do desktop OpenGL on this display\n");
        return;
    }
    // 4.3 first, 3.3 if the driver will not give it -- what fitzel's own window asks.
    static const EGLint versions[][2] = {{4, 3}, {3, 3}};
    for (const auto& v : versions) {
        const EGLint attribs[] = {
            EGL_CONTEXT_MAJOR_VERSION, v[0],
            EGL_CONTEXT_MINOR_VERSION, v[1],
            EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
            EGL_NONE};
        m_context = eglCreateContext(m_display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attribs);
        if (m_context != EGL_NO_CONTEXT) break;
    }
    if (m_context == EGL_NO_CONTEXT) {
        std::fprintf(stderr, "[raum] no desktop OpenGL context (EGL error 0x%x)\n", eglGetError());
        return;
    }
    if (!eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_context)) {
        std::fprintf(stderr, "[raum] cannot make the context current (0x%x)\n", eglGetError());
        return;
    }

    m_createImage  = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(loadProc("eglCreateImageKHR"));
    m_destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(loadProc("eglDestroyImageKHR"));
    m_imageTargetTexture =
        reinterpret_cast<ImageTargetFn>(loadProc("glEGLImageTargetTexture2DOES"));
    m_imageTargetRenderbuffer =
        reinterpret_cast<ImageTargetFn>(loadProc("glEGLImageTargetRenderbufferStorageOES"));
    if (!m_createImage || !m_destroyImage || !m_imageTargetTexture || !m_imageTargetRenderbuffer) {
        std::fprintf(stderr, "[raum] the driver cannot import EGL images into OpenGL\n");
        return;
    }

    if (hasExtension(m_display, "EGL_ANDROID_native_fence_sync") &&
        hasExtension(m_display, "EGL_KHR_wait_sync")) {
        m_createSync = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(loadProc("eglCreateSyncKHR"));
        m_destroySync = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(loadProc("eglDestroySyncKHR"));
        m_waitSync = reinterpret_cast<PFNEGLWAITSYNCKHRPROC>(loadProc("eglWaitSyncKHR"));
    }

    try {
        m_device = fitzel::rhi::createExternalDevice(fitzel::rhi::Backend::OpenGL, &loadProc, 1, 1);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[raum] fitzel: %s\n", e.what());
        return;
    }
    std::fprintf(stderr, "[raum] drawing with %s on %s\n", m_device->caps().apiName,
                 m_device->caps().renderer);
}

Gpu::~Gpu() {
    if (m_context != EGL_NO_CONTEXT) {
        makeCurrent();
        fitzel::rhi::makeCurrent(nullptr);
        m_device.reset();
        eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(m_display, m_context);
    }
}

void Gpu::makeCurrent() {
    if (eglGetCurrentContext() == m_context) return;
    eglBindAPI(EGL_OPENGL_API);
    eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_context);
}

EGLImageKHR Gpu::importDmabuf(const wlr_dmabuf_attributes& a) {
    if (!m_createImage || a.n_planes < 1 || a.n_planes > 4) return EGL_NO_IMAGE_KHR;
    const bool modifier = a.modifier != DRM_FORMAT_MOD_INVALID;
    if (modifier && a.modifier != DRM_FORMAT_MOD_LINEAR && !m_modifiers) return EGL_NO_IMAGE_KHR;

    static const EGLint planeAttribs[4][5] = {
        {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE0_PITCH_EXT,
         EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT},
        {EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
         EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT},
        {EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT,
         EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT},
        {EGL_DMA_BUF_PLANE3_FD_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT,
         EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT},
    };
    EGLint attribs[64];
    int n = 0;
    attribs[n++] = EGL_WIDTH;                     attribs[n++] = a.width;
    attribs[n++] = EGL_HEIGHT;                    attribs[n++] = a.height;
    attribs[n++] = EGL_LINUX_DRM_FOURCC_EXT;      attribs[n++] = static_cast<EGLint>(a.format);
    for (int i = 0; i < a.n_planes; ++i) {
        attribs[n++] = planeAttribs[i][0]; attribs[n++] = a.fd[i];
        attribs[n++] = planeAttribs[i][1]; attribs[n++] = static_cast<EGLint>(a.offset[i]);
        attribs[n++] = planeAttribs[i][2]; attribs[n++] = static_cast<EGLint>(a.stride[i]);
        if (modifier && m_modifiers) {
            attribs[n++] = planeAttribs[i][3];
            attribs[n++] = static_cast<EGLint>(a.modifier & 0xFFFFFFFFu);
            attribs[n++] = planeAttribs[i][4];
            attribs[n++] = static_cast<EGLint>(a.modifier >> 32);
        }
    }
    attribs[n++] = EGL_IMAGE_PRESERVED_KHR;       attribs[n++] = EGL_TRUE;
    attribs[n++] = EGL_NONE;
    EGLImageKHR image = m_createImage(m_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr,
                                      attribs);
    if (image == EGL_NO_IMAGE_KHR)
        std::fprintf(stderr, "[raum] dmabuf import failed (format %.4s, modifier 0x%llx, 0x%x)\n",
                     reinterpret_cast<const char*>(&a.format),
                     static_cast<unsigned long long>(a.modifier), eglGetError());
    return image;
}

void Gpu::destroyImage(EGLImageKHR image) {
    if (image != EGL_NO_IMAGE_KHR && m_destroyImage) m_destroyImage(m_display, image);
}

unsigned Gpu::textureFromImage(EGLImageKHR image) {
    if (image == EGL_NO_IMAGE_KHR) return 0;
    makeCurrent();
    GLint prev = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    while (glGetError() != GL_NO_ERROR) {}
    m_imageTargetTexture(GL_TEXTURE_2D, image);
    const GLenum err = glGetError();
    const bool failed = err != GL_NO_ERROR;
    if (failed) std::fprintf(stderr, "[raum] EGL image as texture failed (GL 0x%x)\n", err);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev));
    if (failed) {
        glDeleteTextures(1, &tex);
        return 0;
    }
    return tex;
}

unsigned Gpu::framebufferFromImage(EGLImageKHR image, unsigned& renderbuffer) {
    renderbuffer = 0;
    if (image == EGL_NO_IMAGE_KHR) return 0;
    makeCurrent();
    GLint prevFb = 0, prevRb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFb);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &prevRb);
    GLuint rb = 0, fb = 0;
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    m_imageTargetRenderbuffer(GL_RENDERBUFFER, image);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFb));
    glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(prevRb));
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "[raum] output framebuffer incomplete (0x%x)\n", status);
        glDeleteFramebuffers(1, &fb);
        glDeleteRenderbuffers(1, &rb);
        return 0;
    }
    renderbuffer = rb;
    return fb;
}

void Gpu::deleteFramebuffer(unsigned fbo, unsigned renderbuffer) {
    makeCurrent();
    GLuint fb = fbo, rb = renderbuffer;
    if (fb) glDeleteFramebuffers(1, &fb);
    if (rb) glDeleteRenderbuffers(1, &rb);
}

void Gpu::finish() { glFinish(); }

void Gpu::waitFence(int fd) {
    if (fd < 0) return;
    if (m_createSync && m_waitSync) {
        makeCurrent();
        const EGLint attribs[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fd, EGL_NONE};
        EGLSyncKHR sync = m_createSync(m_display, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
        if (sync != EGL_NO_SYNC_KHR) {
            // EGL owns the fd now.
            m_waitSync(m_display, sync, 0);
            m_destroySync(m_display, sync);
            return;
        }
    }
    // A sync_file is readable once it has signalled. A client that never
    // finishes does not get to stall the desktop for more than a moment.
    pollfd p{fd, POLLIN, 0};
    poll(&p, 1, 100);
    close(fd);
}

} // namespace raum
