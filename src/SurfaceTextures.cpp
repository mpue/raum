#include "SurfaceTextures.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <fitzel/rhi/Device.hpp>

#include "Gpu.hpp"

namespace raum {

namespace rhi = fitzel::rhi;

namespace {

// --- A dmabuf, imported: lives as long as the buffer -------------------------------

struct BufferImport {
    wlr_addon   addon{};
    Gpu*        gpu = nullptr;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    rhi::TextureHandle texture;
    int width = 0, height = 0;
    bool opaque = false;
};

void destroyBufferImport(wlr_addon* addon) {
    BufferImport* b = reinterpret_cast<BufferImport*>(addon);
    b->gpu->makeCurrent();
    if (b->texture.valid()) b->gpu->device().destroy(b->texture);
    b->gpu->destroyImage(b->image);
    wlr_addon_finish(&b->addon);
    delete b;
}

const wlr_addon_interface kBufferImportImpl = {"raum-buffer-import", destroyBufferImport};

bool opaqueFormat(std::uint32_t f) {
    switch (f) {
        case DRM_FORMAT_XRGB8888: case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_RGBX8888: case DRM_FORMAT_BGRX8888:
        case DRM_FORMAT_XRGB2101010: case DRM_FORMAT_XBGR2101010:
        case DRM_FORMAT_RGB565: case DRM_FORMAT_BGR888: case DRM_FORMAT_RGB888:
            return true;
        default:
            return false;
    }
}

BufferImport* importOf(Gpu& gpu, wlr_buffer* buffer) {
    if (wlr_addon* found = wlr_addon_find(&buffer->addons, &gpu, &kBufferImportImpl))
        return reinterpret_cast<BufferImport*>(found);
    wlr_dmabuf_attributes attribs{};
    if (!wlr_buffer_get_dmabuf(buffer, &attribs)) return nullptr;
    gpu.makeCurrent();
    BufferImport* b = new BufferImport;
    b->gpu = &gpu;
    b->width = attribs.width;
    b->height = attribs.height;
    b->opaque = opaqueFormat(attribs.format);
    b->image = gpu.importDmabuf(attribs);
    if (b->image != EGL_NO_IMAGE_KHR) {
        if (const unsigned name = gpu.textureFromImage(b->image)) {
            rhi::TextureDesc d;
            d.width = attribs.width;
            d.height = attribs.height;
            d.mipLevels = 1;
            d.usage = rhi::UsageSampled;
            b->texture = gpu.device().adoptTexture(name, d);
        }
    }
    // Kept even when the import failed, so a buffer the driver refuses is not
    // retried on every commit.
    wlr_addon_init(&b->addon, &buffer->addons, &gpu, &kBufferImportImpl);
    return b;
}

// --- A surface: follows its commits --------------------------------------------------

// What a dmabuf's producer is still writing, as a sync_file: the implicit
// fence, for clients that do not use explicit sync. -1 when there is none or
// the kernel cannot say.
int implicitFence(int dmabufFd) {
    dma_buf_export_sync_file req{};
    req.flags = DMA_BUF_SYNC_READ;
    req.fd = -1;
    if (ioctl(dmabufFd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &req) != 0) return -1;
    return req.fd;
}

struct SurfaceRecord {
    wlr_addon addon{};
    int fence = -1;   // the current buffer is ready once this signals
    // The buffer on show. Without a renderer wlroots lets go of a surface's
    // buffer as soon as the commit has been handled, so raum holds its own
    // lock until the next one arrives -- which is also what tells the client
    // it may draw into it again.
    wlr_buffer* held = nullptr;
    void hold(wlr_buffer* b) {
        if (b) wlr_buffer_lock(b);
        if (held) wlr_buffer_unlock(held);
        held = b;
    }
    Gpu* gpu = nullptr;
    wlr_surface* surface = nullptr;
    Listener<SurfaceRecord> commit;
    // The shared-memory copy (one per surface, rewritten in place).
    rhi::TextureHandle shm;
    int shmW = 0, shmH = 0;
    SurfaceTextures::Sampled now;

    void onCommit(void*);
    void uploadShm(wlr_buffer* buffer);
};

void destroySurfaceRecord(wlr_addon* addon) {
    SurfaceRecord* r = reinterpret_cast<SurfaceRecord*>(addon);
    r->commit.disconnect();
    if (r->fence >= 0) close(r->fence);
    r->hold(nullptr);
    if (r->shm.valid()) {
        r->gpu->makeCurrent();
        r->gpu->device().destroy(r->shm);
    }
    wlr_addon_finish(&r->addon);
    delete r;
}

const wlr_addon_interface kSurfaceRecordImpl = {"raum-surface", destroySurfaceRecord};

void SurfaceRecord::uploadShm(wlr_buffer* buffer) {
    void* data = nullptr;
    std::uint32_t format = 0;
    std::size_t stride = 0;
    if (!wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format,
                                          &stride)) {
        std::fprintf(stderr, "[raum] a buffer that is neither dmabuf nor readable (%dx%d)\n",
                     buffer->width, buffer->height);
        now.texture = {};
        return;
    }
    bool swap = false;
    switch (format) {
        case DRM_FORMAT_ARGB8888: case DRM_FORMAT_XRGB8888: swap = true; break;
        case DRM_FORMAT_ABGR8888: case DRM_FORMAT_XBGR8888: swap = false; break;
        default:
            wlr_buffer_end_data_ptr_access(buffer);
            std::fprintf(stderr, "[raum] shm format %.4s not supported\n",
                         reinterpret_cast<const char*>(&format));
            now.texture = {};
            return;
    }
    gpu->makeCurrent();
    rhi::Device& dev = gpu->device();
    const int w = buffer->width, h = buffer->height;
    bool full = false;
    if (!shm.valid() || shmW != w || shmH != h) {
        if (shm.valid()) dev.destroy(shm);
        rhi::TextureDesc d;
        d.width = w;
        d.height = h;
        d.mipLevels = 1;
        d.usage = rhi::UsageSampled;
        shm = dev.createTexture(d, nullptr, rhi::Format::RGBA8);
        shmW = w;
        shmH = h;
        full = true;
    }
    const auto* bytes = static_cast<const unsigned char*>(data);
    const int rowLength = static_cast<int>(stride / 4);
    auto upload = [&](int x, int y, int rw, int rh) {
        if (rw <= 0 || rh <= 0) return;
        rhi::TextureRegion r;
        r.x = x; r.y = y; r.w = rw; r.h = rh;
        r.rowLength = rowLength;
        dev.updateTexture(shm, r, rhi::Format::RGBA8,
                          bytes + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4);
    };
    if (full) {
        upload(0, 0, w, h);
    } else {
        int n = 0;
        const pixman_box32_t* rects =
            pixman_region32_rectangles(&surface->current.buffer_damage, &n);
        for (int i = 0; i < n; ++i) {
            const int x0 = std::max(0, rects[i].x1), y0 = std::max(0, rects[i].y1);
            const int x1 = std::min(w, rects[i].x2), y1 = std::min(h, rects[i].y2);
            upload(x0, y0, x1 - x0, y1 - y0);
        }
    }
    wlr_buffer_end_data_ptr_access(buffer);
    now.texture = shm;
    now.width = w;
    now.height = h;
    now.swapRB = swap;
    now.opaque = format == DRM_FORMAT_XRGB8888 || format == DRM_FORMAT_XBGR8888;
}

void SurfaceRecord::onCommit(void*) {
    wlr_buffer* buffer = surface->current.buffer;
    if (!buffer) {
        // A commit without a buffer keeps the content -- unless it took the
        // buffer away (a null attach: the surface shows nothing now).
        if (surface->current.committed & WLR_SURFACE_STATE_BUFFER) {
            hold(nullptr);
            now.texture = {};
            ++now.version;
        }
        return;
    }
    ++now.version;
    if (fence >= 0) {
        close(fence);
        fence = -1;
    }
    // Explicit sync: the client said which point its rendering signals, and is
    // told when raum is done with the buffer. Without it, the dmabuf's own fence.
    wlr_dmabuf_attributes attribs{};
    const bool dmabuf = wlr_buffer_get_dmabuf(buffer, &attribs);
    if (auto* st = wlr_linux_drm_syncobj_v1_get_surface_state(surface)) {
        if (st->acquire_timeline)
            fence = wlr_drm_syncobj_timeline_export_sync_file(st->acquire_timeline, st->acquire_point);
        wlr_linux_drm_syncobj_v1_state_signal_release_with_buffer(st, buffer);
    } else if (dmabuf && attribs.n_planes > 0) {
        fence = implicitFence(attribs.fd[0]);
    }
    if (BufferImport* b = importOf(*gpu, buffer)) {
        now.texture = b->texture;
        now.width = b->width;
        now.height = b->height;
        now.swapRB = false;
        now.opaque = b->opaque;
        hold(buffer);   // sampled where it is, every frame
        return;
    }
    uploadShm(buffer);
    hold(nullptr);      // copied: the client may have it back
}

} // namespace

void SurfaceTextures::track(wlr_surface* surface) {
    if (wlr_addon_find(&surface->addons, &m_gpu, &kSurfaceRecordImpl)) return;
    SurfaceRecord* r = new SurfaceRecord;
    r->gpu = &m_gpu;
    r->surface = surface;
    r->commit.connect(&surface->events.commit, r, &SurfaceRecord::onCommit);
    wlr_addon_init(&r->addon, &surface->addons, &m_gpu, &kSurfaceRecordImpl);
}

void SurfaceTextures::waitReady(wlr_surface* surface) {
    if (wlr_addon* a = wlr_addon_find(&surface->addons, &m_gpu, &kSurfaceRecordImpl)) {
        auto* r = reinterpret_cast<SurfaceRecord*>(a);
        if (r->fence >= 0) {
            m_gpu.waitFence(r->fence);
            r->fence = -1;
        }
    }
}

SurfaceTextures::Sampled SurfaceTextures::sampled(wlr_surface* surface) const {
    if (wlr_addon* a = wlr_addon_find(&surface->addons, &m_gpu, &kSurfaceRecordImpl))
        return reinterpret_cast<SurfaceRecord*>(a)->now;
    return {};
}

} // namespace raum
