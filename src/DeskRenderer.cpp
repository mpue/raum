#include "DeskRenderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <fitzel/rhi/Device.hpp>

#include "Desk.hpp"
#include "Gpu.hpp"
#include "Server.hpp"
#include "SurfaceTextures.hpp"
#include "View.hpp"

namespace raum {

namespace rhi = fitzel::rhi;

namespace {

// --- Shaders ---------------------------------------------------------------------

// The sky: a gradient by view direction, lighter towards the horizon, with the
// current room's colour as a faint glow behind it.
const char* kSkyVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
out vec2 vNdc;
void main() {
    vNdc = aPos.xy * 2.0 - 1.0;
    gl_Position = vec4(vNdc, 0.0, 1.0);
})";
const char* kSkyFrag = R"(#version 330 core
in vec2 vNdc;
out vec4 frag;
uniform mat4 uInvViewProj;
uniform vec3 uAccent;
uniform vec3 uEye;
void main() {
    vec4 p = uInvViewProj * vec4(vNdc, 1.0, 1.0);
    vec3 dir = normalize(p.xyz / p.w - uEye);
    float up = dir.y;
    vec3 zenith  = vec3(0.020, 0.026, 0.038);
    vec3 horizon = vec3(0.075, 0.088, 0.112);
    vec3 c = mix(horizon, zenith, smoothstep(-0.05, 0.55, up));
    // A soft band of the room's colour just above the horizon.
    c += uAccent * 0.045 * exp(-pow((up - 0.04) / 0.10, 2.0));
    // Vignette, gently.
    float v = dot(vNdc * vec2(0.75, 0.9), vNdc * vec2(0.75, 0.9));
    c *= 1.0 - 0.28 * v;
    frag = vec4(c, 1.0);
})";

// The floor: dark stone with a grid that fades with distance, a lit line down
// each room's aisle (the time axis), soft contact shadows under the windows.
const char* kFloorVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uViewProj;
uniform vec4 uExtent;   // x0, z0, x1, z1
uniform float uFloorY;
out vec3 vWorld;
void main() {
    vec3 w = vec3(mix(uExtent.x, uExtent.z, aPos.x), uFloorY, mix(uExtent.y, uExtent.w, aPos.y));
    vWorld = w;
    gl_Position = uViewProj * vec4(w, 1.0);
})";
const char* kFloorFrag = R"(#version 330 core
in vec3 vWorld;
out vec4 frag;
uniform vec3 uEye;
uniform int uShadowCount;
uniform vec4 uShadowA[48];   // x0, z0, x1, z1 of the bottom edge
uniform vec4 uShadowB[48];   // height above the floor, strength
uniform int uRoomCount;
uniform vec4 uRoom[16];      // centre x, accent rgb
uniform float uRoomPitch;
uniform float uAisle;        // how much of the time axis shows, 0..1
uniform int uCurrentRoom;
uniform float uAlpha;

float segDist(vec2 p, vec2 a, vec2 b) {
    vec2 ab = b - a;
    float t = clamp(dot(p - a, ab) / max(dot(ab, ab), 1e-3), 0.0, 1.0);
    return length(p - (a + ab * t));
}

float gridLine(float coord, float spacing, float width) {
    float f = abs(fract(coord / spacing + 0.5) - 0.5) * spacing;
    float aa = fwidth(coord) * 1.2;
    return 1.0 - smoothstep(width, width + aa, f);
}

void main() {
    vec2 p = vWorld.xz;
    float dist = length(vWorld - uEye);
    vec3 base = vec3(0.060, 0.066, 0.080);
    // Grid: fine lines every 160 units, stronger every 800, gone with distance.
    float fade = exp(-dist / 9000.0);
    float g = max(gridLine(p.x, 160.0, 0.8) * 0.35, gridLine(p.y, 160.0, 0.8) * 0.35);
    g = max(g, max(gridLine(p.x, 800.0, 1.4), gridLine(p.y, 800.0, 1.4)) * 0.7);
    vec3 c = base + vec3(0.050, 0.058, 0.070) * g * fade;

    // Rooms: a faint rug of each room's colour, and its time axis down the aisle.
    for (int i = 0; i < uRoomCount; ++i) {
        float rx = uRoom[i].x;
        vec3 acc = uRoom[i].yzw;
        vec2 d = abs(p - vec2(rx, -2400.0)) - vec2(uRoomPitch * 0.40, 3200.0);
        float rug = 1.0 - smoothstep(0.0, 260.0, length(max(d, 0.0)) + min(max(d.x, d.y), 0.0));
        c += acc * rug * (i == uCurrentRoom ? 0.035 : 0.018);
        float axis = exp(-pow((p.x - rx) / 6.0, 2.0));
        float along = smoothstep(400.0, 0.0, p.y) * exp(min(p.y, 0.0) / (uAisle > 1.5 ? 1500.0 : 5000.0));
        // The time axis of the room in view (all of them in the house view).
        along *= (i == uCurrentRoom || uAisle > 1.5) ? 1.0 : 0.0;
        // Ticks across the axis every 600 units, like a ruler.
        float tick = gridLine(p.y, 600.0, 2.0) * exp(-pow((p.x - rx) / 60.0, 2.0));
        c += acc * (axis * 0.45 + tick * 0.30) * along * min(uAisle, 1.0);
    }

    // Shadows: each window's bottom edge, softer the higher it floats.
    float lit = 1.0;
    for (int i = 0; i < uShadowCount; ++i) {
        float h = uShadowB[i].x;
        float d = segDist(p, uShadowA[i].xy, uShadowA[i].zw);
        float blur = 30.0 + h * 0.45;
        float s = exp(-(d * d) / (blur * blur)) * uShadowB[i].y;
        lit *= 1.0 - s * 0.75;
    }
    c *= lit;

    // Into the distance the floor takes the sky's horizon colour: no hard edge.
    float far = smoothstep(3000.0, 45000.0, dist);
    c = mix(c, vec3(0.075, 0.088, 0.112), far);
    frag = vec4(c, mix(uAlpha, 1.0, far));
})";

// A window: its canvas on a quad given by four corners in the world.
const char* kWindowVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uViewProj;
uniform vec3 uCorner[4];   // TL, TR, BR, BL
out vec2 vUV;
out vec3 vWorld;
void main() {
    vec3 top = mix(uCorner[0], uCorner[1], aPos.x);
    vec3 bot = mix(uCorner[3], uCorner[2], aPos.x);
    vec3 w = mix(top, bot, aPos.y);
    vUV = aPos.xy;
    vWorld = w;
    gl_Position = uViewProj * vec4(w, 1.0);
})";
const char* kWindowFrag = R"(#version 330 core
in vec2 vUV;
in vec3 vWorld;
out vec4 frag;
uniform sampler2D uTex;
uniform float uDim;
uniform float uAlpha;
uniform vec3 uFog;
uniform int uReflection;
uniform float uFloorY;
uniform int uEdgeAA;
void main() {
    vec4 c = texture(uTex, vUV);   // premultiplied
    // Into the background: towards the hall's colour, less saturated.
    float l = dot(c.rgb, vec3(0.299, 0.587, 0.114));
    vec3 grey = mix(c.rgb, vec3(l) * 0.9, uDim * 0.6);
    c.rgb = mix(grey, uFog * c.a, uDim * 0.75);
    float a = uAlpha;
    if (uEdgeAA != 0) {
        // Half a pixel of coverage at the edges of a quad that is not on the
        // pixel grid; one that is keeps its outermost row whole.
        vec2 px = min(vUV, 1.0 - vUV) / max(fwidth(vUV), vec2(1e-6));
        a *= clamp(min(px.x, px.y) + 0.5, 0.0, 1.0);
    }
    if (uReflection != 0) a *= 0.22 * exp(-(uFloorY - vWorld.y) / 260.0);
    frag = c * a;
})";

// A focus glow: a soft rim of the room's colour around a window.
const char* kGlowFrag = R"(#version 330 core
in vec2 vUV;
in vec3 vWorld;
out vec4 frag;
uniform vec2 uSize;     // the quad in pixels (window plus margin on each side)
uniform float uMargin;
uniform vec3 uColor;
uniform float uStrength;
void main() {
    vec2 p = vUV * uSize;
    vec2 halfInner = uSize * 0.5 - vec2(uMargin);
    vec2 q = abs(p - uSize * 0.5) - halfInner + vec2(8.0);
    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - 8.0;   // rounded, radius 8
    if (d < 0.0) discard;
    float a = exp(-d * d / (2.0 * 9.0 * 9.0)) * uStrength;
    frag = vec4(uColor * a, a);
})";

// Composing a canvas: one client surface into the window's image.
const char* kSurfaceVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
uniform vec4 uRect;      // x, y, w, h in canvas pixels
uniform vec2 uCanvas;    // canvas size
uniform vec4 uSrc;       // u0, v0, u1, v1
out vec2 vUV;
void main() {
    vec2 px = uRect.xy + aPos.xy * uRect.zw;
    vUV = mix(uSrc.xy, uSrc.zw, aPos.xy);
    gl_Position = vec4(px / uCanvas * 2.0 - 1.0, 0.0, 1.0);
})";
const char* kSurfaceFrag = R"(#version 330 core
in vec2 vUV;
out vec4 frag;
uniform sampler2D uTex;
uniform int uSwapRB;
uniform int uOpaque;
void main() {
    vec4 c = texture(uTex, vUV);
    if (uSwapRB != 0) c = c.bgra;
    if (uOpaque != 0) c.a = 1.0;
    frag = c;
})";

rhi::RenderState blendPremultiplied(bool depthTest, bool depthWrite) {
    rhi::RenderState s;
    s.blend.enabled = true;
    s.blend.srcColor = rhi::BlendFactor::One;
    s.blend.dstColor = rhi::BlendFactor::OneMinusSrcAlpha;
    s.blend.srcAlpha = rhi::BlendFactor::One;
    s.blend.dstAlpha = rhi::BlendFactor::OneMinusSrcAlpha;
    s.depth.test = depthTest;
    s.depth.write = depthWrite;
    s.depth.func = rhi::CompareOp::LessEqual;
    return s;
}

std::uint64_t mix64(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

} // namespace

DeskRenderer::DeskRenderer(Server& server) : m_server(server) {
    m_skyShader = fitzel::Shader::fromSource(kSkyVert, kSkyFrag);
    m_floorShader = fitzel::Shader::fromSource(kFloorVert, kFloorFrag);
    m_windowShader = fitzel::Shader::fromSource(kWindowVert, kWindowFrag);
    m_glowShader = fitzel::Shader::fromSource(kWindowVert, kGlowFrag);
    m_surfaceShader = fitzel::Shader::fromSource(kSurfaceVert, kSurfaceFrag);
    std::vector<fitzel::Vertex> v(4);
    v[0].position = {0, 0, 0}; v[1].position = {1, 0, 0};
    v[2].position = {1, 1, 0}; v[3].position = {0, 1, 0};
    for (auto& x : v) { x.normal = {0, 0, 1}; x.uv = glm::vec2(x.position); }
    m_quad = fitzel::Mesh::create(v, {0, 1, 2, 0, 2, 3});
}

DeskRenderer::~DeskRenderer() = default;

// --- Canvases ---------------------------------------------------------------------

void DeskRenderer::updateCanvas(View* view) {
    struct Piece { wlr_surface* s; int x, y, w, h; };
    std::vector<Piece> pieces;
    const wlr_box g = view->geometry();
    int x0 = g.x, y0 = g.y, x1 = g.x + std::max(1, g.width), y1 = g.y + std::max(1, g.height);
    std::uint64_t key = 0;
    SurfaceTextures& st = *m_server.surfaces;
    view->forEachSurface([&](wlr_surface* s, int sx, int sy) {
        const int w = s->current.width, h = s->current.height;
        if (w <= 0 || h <= 0) return;
        pieces.push_back({s, sx, sy, w, h});
        x0 = std::min(x0, sx); y0 = std::min(y0, sy);
        x1 = std::max(x1, sx + w); y1 = std::max(y1, sy + h);
        key = mix64(key, reinterpret_cast<std::uintptr_t>(s));
        key = mix64(key, st.sampled(s).version);
        key = mix64(key, (static_cast<std::uint64_t>(static_cast<std::uint32_t>(sx)) << 32) |
                             static_cast<std::uint32_t>(sy));
        key = mix64(key, (static_cast<std::uint64_t>(w) << 32) | static_cast<std::uint32_t>(h));
    });
    // Popups can reach far outside a window; a canvas is never made bigger
    // than a few screens, whatever a client asks for.
    const int cw = std::min(x1 - x0, 8192), ch = std::min(y1 - y0, 8192);
    Canvas& c = view->canvas;
    rhi::Device& dev = m_server.gpu->device();
    if (c.valid() && c.contentKey == key && c.width == cw && c.height == ch &&
        c.box.x == x0 && c.box.y == y0)
        return;
    if (!c.valid() || c.width != cw || c.height != ch) {
        if (c.fbo.valid()) dev.destroy(c.fbo);
        if (c.color.valid()) dev.destroy(c.color);
        rhi::TextureDesc d;
        d.width = cw;
        d.height = ch;
        d.mipLevels = 0;   // the full chain: far windows are read from small mips
        d.usage = rhi::UsageSampled | rhi::UsageRenderTarget;
        d.sampler.minFilter = rhi::Filter::Linear;
        d.sampler.magFilter = rhi::Filter::Linear;
        d.sampler.mipFilter = rhi::MipFilter::Linear;
        d.sampler.anisotropy = 4.0f;
        c.color = dev.createTexture(d, nullptr, rhi::Format::RGBA8);
        rhi::FramebufferDesc fd;
        fd.color[0].texture = c.color;
        c.fbo = dev.createFramebuffer(fd);
        c.width = cw;
        c.height = ch;
    }
    c.box = wlr_box{x0, y0, cw, ch};
    c.contentKey = key;
    static const bool debug = std::getenv("RAUM_DEBUG_CANVAS") != nullptr;
    if (debug) {
        std::fprintf(stderr, "[canvas] %s box %d,%d %dx%d geo %d,%d %dx%d pieces %zu\n",
                     view->appId().c_str(), x0, y0, cw, ch, g.x, g.y, g.width, g.height, pieces.size());
        for (const Piece& p : pieces) {
            const SurfaceTextures::Sampled sm = st.sampled(p.s);
            std::fprintf(stderr, "    surface %p at %d,%d %dx%d tex %d buf %dx%d swap %d opaque %d v%u\n",
                         static_cast<void*>(p.s), p.x, p.y, p.w, p.h, sm.texture.valid() ? 1 : 0,
                         sm.width, sm.height, sm.swapRB, sm.opaque, sm.version);
        }
    }

    dev.bindFramebuffer(c.fbo);
    dev.setViewport(0, 0, cw, ch);
    dev.setRenderState(rhi::RenderState{});
    dev.setClearColor(glm::vec4(0.0f));
    dev.clear(rhi::ClearColor);
    dev.setRenderState(blendPremultiplied(false, false));
    m_surfaceShader.bind();
    m_surfaceShader.setVec2("uCanvas", glm::vec2(cw, ch));
    m_surfaceShader.setInt("uTex", 0);
    for (const Piece& p : pieces) {
        const SurfaceTextures::Sampled sm = st.sampled(p.s);
        if (!sm.texture.valid()) continue;
        st.waitReady(p.s);
        glm::vec4 src(0, 0, 1, 1);
        const auto& vp = p.s->current.viewport;
        if (vp.has_src && sm.width > 0 && sm.height > 0) {
            // In buffer pixels after scale; the buffer scale is 1 on raum's outputs.
            const float sc = static_cast<float>(std::max(1, p.s->current.scale));
            src = glm::vec4(vp.src.x * sc / sm.width, vp.src.y * sc / sm.height,
                            (vp.src.x + vp.src.width) * sc / sm.width,
                            (vp.src.y + vp.src.height) * sc / sm.height);
        }
        m_surfaceShader.setVec4("uRect", glm::vec4(p.x - x0, p.y - y0, p.w, p.h));
        m_surfaceShader.setVec4("uSrc", src);
        m_surfaceShader.setInt("uSwapRB", sm.swapRB ? 1 : 0);
        m_surfaceShader.setInt("uOpaque", sm.opaque ? 1 : 0);
        dev.bindTexture(0, sm.texture);
        m_quad.draw();
    }
    dev.generateMipmaps(c.color);
}

// --- The scene ----------------------------------------------------------------------

void DeskRenderer::drawSky() {
    const Desk& desk = *m_server.desk;
    rhi::Device& dev = m_server.gpu->device();
    dev.setRenderState(rhi::RenderState{});
    m_skyShader.bind();
    m_skyShader.setMat4("uInvViewProj", glm::inverse(desk.projMatrix() * desk.viewMatrix()));
    m_skyShader.setVec3("uEye", desk.cameraPos());
    m_skyShader.setVec3("uAccent", desk.rooms[static_cast<std::size_t>(desk.current)].accent);
    m_quad.draw();
}

void DeskRenderer::drawFloor(bool) {
    const Desk& desk = *m_server.desk;
    rhi::Device& dev = m_server.gpu->device();
    rhi::RenderState s;
    s.blend.enabled = true;
    s.blend.srcColor = rhi::BlendFactor::SrcAlpha;
    s.blend.dstColor = rhi::BlendFactor::OneMinusSrcAlpha;
    s.depth.test = true;
    s.depth.write = true;
    s.depth.func = rhi::CompareOp::LessEqual;
    dev.setRenderState(s);
    m_floorShader.bind();
    m_floorShader.setMat4("uViewProj", desk.projMatrix() * desk.viewMatrix());
    m_floorShader.setVec3("uEye", desk.cameraPos());
    const float x0 = desk.roomX(0) - desk.width() * 6.0f;
    const float x1 = desk.roomX(static_cast<int>(desk.rooms.size()) - 1) + desk.width() * 6.0f;
    m_floorShader.setVec4("uExtent", glm::vec4(x0, 9000.0f, x1, -90000.0f));
    m_floorShader.setFloat("uFloorY", desk.floorY());
    m_floorShader.setFloat("uAlpha", 0.88f);
    const int roomCount = std::min<int>(16, static_cast<int>(desk.rooms.size()));
    m_floorShader.setInt("uRoomCount", roomCount);
    for (int i = 0; i < roomCount; ++i) {
        const glm::vec3 a = desk.rooms[static_cast<std::size_t>(i)].accent;
        m_floorShader.setVec4("uRoom[" + std::to_string(i) + "]",
                              glm::vec4(desk.roomX(i), a.r, a.g, a.b));
    }
    m_floorShader.setFloat("uRoomPitch", desk.roomPitch());
    m_floorShader.setInt("uCurrentRoom", desk.current);
    // The time axis is for when the history is laid out; at work it stays dark.
    static float aisle = 0.0f;
    aisle += ((desk.level == Level::Work ? 0.0f : 1.0f) - aisle) * 0.2f;
    m_floorShader.setFloat("uAisle", desk.level == Level::House ? 0.0f : aisle);

    int n = 0;
    for (View* v : desk.drawList()) {
        if (n >= 48) break;
        glm::vec3 c[4];
        desk.canvasQuad(v, v->pose, c);
        // The geometry's bottom edge, straight down onto the floor.
        const wlr_box g = v->geometry();
        const glm::vec3 R(std::cos(v->pose.yaw), 0.0f, -std::sin(v->pose.yaw));
        const float hw = g.width * 0.5f * v->pose.scale, hh = g.height * 0.5f * v->pose.scale;
        const glm::vec3 a = v->pose.center - R * hw, b = v->pose.center + R * hw;
        const float bottom = v->pose.center.y - hh - desk.floorY();
        const std::string i = "[" + std::to_string(n) + "]";
        m_floorShader.setVec4("uShadowA" + i, glm::vec4(a.x, a.z, b.x, b.z));
        m_floorShader.setVec4("uShadowB" + i,
                              glm::vec4(std::max(0.0f, bottom), 0.55f * v->pose.alpha, 0, 0));
        ++n;
    }
    m_floorShader.setInt("uShadowCount", n);
    m_quad.draw();
}

void DeskRenderer::drawFrame(View* v, float alpha) {
    // The focus glow, around the window geometry (not its shadows).
    const Desk& desk = *m_server.desk;
    const wlr_box g = v->geometry();
    const float margin = 28.0f;
    const glm::vec3 R(std::cos(v->pose.yaw), 0.0f, -std::sin(v->pose.yaw));
    const glm::vec3 U(0.0f, 1.0f, 0.0f);
    const float s = v->pose.scale;
    const float hw = g.width * 0.5f + margin, hh = g.height * 0.5f + margin;
    // Just behind the window so the window covers its own middle.
    const glm::vec3 n(std::sin(v->pose.yaw), 0.0f, std::cos(v->pose.yaw));
    const glm::vec3 c = v->pose.center - n * 0.6f;
    m_glowShader.bind();
    m_glowShader.setMat4("uViewProj", desk.projMatrix() * desk.viewMatrix());
    m_glowShader.setVec3("uCorner[0]", c - R * (hw * s) + U * (hh * s));
    m_glowShader.setVec3("uCorner[1]", c + R * (hw * s) + U * (hh * s));
    m_glowShader.setVec3("uCorner[2]", c + R * (hw * s) - U * (hh * s));
    m_glowShader.setVec3("uCorner[3]", c - R * (hw * s) - U * (hh * s));
    m_glowShader.setVec2("uSize", glm::vec2(hw * 2.0f, hh * 2.0f));
    m_glowShader.setFloat("uMargin", margin);
    m_glowShader.setVec3("uColor", desk.rooms[static_cast<std::size_t>(v->room)].accent);
    m_glowShader.setFloat("uStrength", alpha);
    m_server.gpu->device().setRenderState(blendPremultiplied(true, false));
    m_quad.draw();
}

void DeskRenderer::drawWindows(bool reflection) {
    const Desk& desk = *m_server.desk;
    rhi::Device& dev = m_server.gpu->device();
    const glm::mat4 vp = desk.projMatrix() * desk.viewMatrix();
    View* focus = m_server.focused();
    const float floorY = desk.floorY();
    for (View* v : desk.drawList()) {
        if (!v->canvas.valid()) continue;
        if (v == desk.dragging) continue;
        Pose pose = v->pose;
        glm::vec3 c[4];
        desk.canvasQuad(v, pose, c);
        if (reflection) {
            for (glm::vec3& w : c) w.y = 2.0f * floorY - w.y;
        } else {
            const bool hover = v == desk.hovered && v->slot < 0;
            View* root = focus;
            while (root && root->parentView()) root = root->parentView();
            const bool showGlow = (v == root && desk.level != Level::Work) ||
                                  (v == root && desk.rooms[static_cast<std::size_t>(v->room)].split > 1) ||
                                  hover;
            if (showGlow) drawFrame(v, (hover ? 0.55f : 0.45f) * pose.alpha);
        }
        m_windowShader.bind();
        m_windowShader.setMat4("uViewProj", vp);
        for (int i = 0; i < 4; ++i)
            m_windowShader.setVec3("uCorner[" + std::to_string(i) + "]", c[i]);
        float dim = pose.dim;
        if (v == desk.hovered && v->slot < 0) dim *= 0.45f;
        m_windowShader.setFloat("uDim", dim);
        m_windowShader.setFloat("uAlpha", pose.alpha * std::min(1.0f, v->appear));
        m_windowShader.setVec3("uFog", glm::vec3(0.07f, 0.08f, 0.10f));
        m_windowShader.setInt("uReflection", reflection ? 1 : 0);
        m_windowShader.setFloat("uFloorY", floorY);
        // On the pixel grid exactly (a settled front window): no edge blending.
        const bool exact = desk.level == Level::Work && v->slot >= 0 &&
                           std::abs(pose.scale - v->target.scale) < 1e-6f &&
                           glm::length(pose.center - v->target.center) < 1e-3f && pose.yaw == 0.0f;
        m_windowShader.setInt("uEdgeAA", exact ? 0 : 1);
        m_windowShader.setInt("uTex", 0);
        dev.bindTexture(0, v->canvas.color);
        dev.setRenderState(blendPremultiplied(!reflection, !reflection));
        m_quad.draw();
    }
}

void DeskRenderer::render(int width, int height) {
    rhi::Device& dev = m_server.gpu->device();
    if (!m_scene || m_w != width || m_h != height) {
        m_scene = std::make_unique<fitzel::RenderTarget>(width, height);
        m_w = width;
        m_h = height;
    }
    // Canvases first: each is a pass of its own.
    for (View* v : m_server.desk->drawList()) {
        v->appear = std::min(1.0f, v->appear + 0.12f);
        updateCanvas(v);
    }
    m_scene->bind();
    dev.setRenderState(rhi::RenderState{});
    dev.setClearColor(glm::vec4(0.02f, 0.025f, 0.035f, 1.0f));
    dev.setClearDepth(1.0f);
    dev.clear(rhi::ClearColor | rhi::ClearDepth);
    drawSky();
    drawWindows(true);
    drawFloor(false);
    drawWindows(false);
    dev.setRenderState(rhi::RenderState{});
}

void DeskRenderer::bindScene() {
    if (m_scene) m_scene->bind();
}

void DeskRenderer::present() {
    if (!m_scene) return;
    rhi::Device& dev = m_server.gpu->device();
    dev.setRenderState(rhi::RenderState{});
    // The output's buffer has its first row at the top, OpenGL's framebuffers at
    // the bottom: the blit turns the frame over.
    dev.blit(m_scene->framebuffer(), rhi::Rect{0, 0, m_w, m_h}, rhi::FramebufferHandle{},
             rhi::Rect{0, m_h, m_w, -m_h}, rhi::BlitColor, rhi::Filter::Nearest, 0);
    dev.bindFramebuffer(rhi::FramebufferHandle{});
}

bool DeskRenderer::saveOutputPng(const std::string& path) {
    rhi::Device& dev = m_server.gpu->device();
    std::vector<unsigned char> px(static_cast<std::size_t>(m_w) * m_h * 4);
    // The output's buffer is in display order already: first row at the top.
    dev.readPixels(rhi::FramebufferHandle{}, 0, rhi::Rect{0, 0, m_w, m_h}, rhi::Format::RGBA8, px.data());
    for (std::size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    const bool ok = stbi_write_png(path.c_str(), m_w, m_h, 4, px.data(), m_w * 4) != 0;
    std::fprintf(stderr, "[raum] %s %s (output buffer)\n", ok ? "screenshot" : "could not write", path.c_str());
    return ok;
}

bool DeskRenderer::savePng(const std::string& path) {
    if (!m_scene) return false;
    rhi::Device& dev = m_server.gpu->device();
    std::vector<unsigned char> px(static_cast<std::size_t>(m_w) * m_h * 4);
    dev.readPixels(m_scene->framebuffer(), 0, rhi::Rect{0, 0, m_w, m_h}, rhi::Format::RGBA8,
                   px.data());
    std::vector<unsigned char> flipped(px.size());
    const std::size_t row = static_cast<std::size_t>(m_w) * 4;
    for (int y = 0; y < m_h; ++y)
        std::copy_n(px.data() + static_cast<std::size_t>(m_h - 1 - y) * row, row,
                    flipped.data() + static_cast<std::size_t>(y) * row);
    for (std::size_t i = 3; i < flipped.size(); i += 4) flipped[i] = 255;
    const bool ok = stbi_write_png(path.c_str(), m_w, m_h, 4, flipped.data(),
                                   static_cast<int>(row)) != 0;
    std::fprintf(stderr, "[raum] %s %s\n", ok ? "screenshot" : "could not write", path.c_str());
    return ok;
}

} // namespace raum
