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
#include "Stage.hpp"
#include "SurfaceTextures.hpp"
#include "Theme.hpp"
#include "View.hpp"

namespace raum {

namespace rhi = fitzel::rhi;

namespace {

// --- Shaders ---------------------------------------------------------------------

// The sky: deep space. A gradient by view direction, two layers of stars
// that twinkle while the hall is awake, a faint nebula in the room's colour,
// and the horizon drawn as a line of light.
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
uniform float uTime;
uniform vec3 uZenith;
uniform vec3 uHorizon;
uniform vec3 uHolo;

// Integer hashing: a sine hash falls apart on GPUs whose sin() is coarse for
// big arguments (the Raphael iGPU drew the stars as blocks and lines).
uint pcg(uint v) {
    uint s = v * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}
float hash(vec2 p) {
    ivec2 i = ivec2(floor(p)) + ivec2(65536);
    return float(pcg(uint(i.x) * 1597334677u ^ pcg(uint(i.y)))) / 4294967295.0;
}
float hash3(vec3 p) { p = fract(p * 0.3183099 + 0.1); p *= 17.0; return fract(p.x * p.y * p.z * (p.x + p.y + p.z)); }
float noise(vec3 x) {
    vec3 i = floor(x); vec3 f = fract(x); f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash3(i), hash3(i + vec3(1, 0, 0)), f.x),
                   mix(hash3(i + vec3(0, 1, 0)), hash3(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(hash3(i + vec3(0, 0, 1)), hash3(i + vec3(1, 0, 1)), f.x),
                   mix(hash3(i + vec3(0, 1, 1)), hash3(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}
float fbm(vec3 p) { float a = 0.5, s = 0.0; for (int i = 0; i < 5; ++i) { s += a * noise(p); p *= 2.03; a *= 0.5; } return s; }

// Stars on a grid over azimuth and elevation: at most one per cell.
vec3 stars(vec3 dir, float density, float keep, float size) {
    vec2 uv = vec2(atan(dir.z, dir.x) / 6.2831853, asin(clamp(dir.y, -1.0, 1.0)) / 3.1415927) * density;
    // Derivatives before any branch: inside one that neighbouring pixels do
    // not all take they are undefined (AMD drew the cell edges as lines).
    float px = max(length(fwidth(uv)), 1e-4);
    vec2 cell = floor(uv);
    float h = hash(cell);
    if (h > keep) return vec3(0.0);
    vec2 pos = cell + 0.15 + 0.7 * vec2(hash(cell + 3.7), hash(cell + 9.1));
    float d = length(uv - pos) / px;
    float b = exp(-d * d / (size * size));
    float tw = 0.75 + 0.25 * sin(uTime * (0.8 + 2.0 * h) + h * 60.0);
    vec3 col = mix(vec3(0.70, 0.86, 1.0), vec3(1.0, 0.86, 0.72), step(0.8, hash(cell + 1.3)));
    return col * b * tw * (0.35 + 0.65 * (1.0 - h / keep));
}

void main() {
    vec4 p = uInvViewProj * vec4(vNdc, 1.0, 1.0);
    vec3 dir = normalize(p.xyz / p.w - uEye);
    float up = dir.y;
    vec3 c = mix(uHorizon, uZenith, smoothstep(-0.02, 0.45, up));
    // The nebula: slow, faint, coloured by the room.
    float n = fbm(dir * 2.4 + vec3(0.0, 0.0, uTime * 0.004));
    float neb = smoothstep(0.48, 0.85, n) * smoothstep(-0.05, 0.35, up) * (1.0 - smoothstep(0.5, 0.95, up));
    c += mix(uHolo * 0.6, uAccent, 0.55) * neb * 0.07;
    // Stars: many faint, a few bright; none below the horizon.
    float above = smoothstep(0.0, 0.06, up);
    c += stars(dir, 420.0, 0.20, 0.9) * 0.55 * above;
    c += stars(dir, 140.0, 0.12, 1.4) * 0.9 * above;
    // The horizon: a line of light and a haze above it.
    c += uHolo * (exp(-abs(up) / 0.0025) * 0.22 + exp(-max(up, 0.0) / 0.05) * 0.035);
    c += uAccent * exp(-max(up, 0.0) / 0.12) * 0.02;
    float v = dot(vNdc * vec2(0.75, 0.9), vNdc * vec2(0.75, 0.9));
    c *= 1.0 - 0.32 * v;
    frag = vec4(c, 1.0);
})";

// The floor: polished black glass under a hexagon grid of light that fades
// with distance and brightens round the room in view. Rings of light run out
// over it when something happens (uWave), the room's time axis lies down its
// aisle, and the windows throw soft shadows.
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
uniform float uTime;
uniform vec4 uWave[3];       // origin x, z, age in seconds, strength
uniform vec3 uHolo;
uniform vec3 uHorizon;

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

// Distance to the nearest hexagon edge, in cell units (0 on an edge).
float hexEdge(vec2 p) {
    const vec2 s = vec2(1.0, 1.7320508);
    vec4 hc = floor(vec4(p, p - vec2(0.5, 1.0)) / s.xyxy) + 0.5;
    vec4 h = vec4(p - hc.xy * s, p - (hc.zw + 0.5) * s);
    vec2 g = dot(h.xy, h.xy) < dot(h.zw, h.zw) ? h.xy : h.zw;
    vec2 a = abs(g);
    return 0.5 - max(dot(a, s * 0.5), a.x);
}

float hexLine(vec2 p, float width) {
    float e = hexEdge(p);
    float aa = length(fwidth(p)) * 0.9;
    return 1.0 - smoothstep(width, width + aa, e);
}

void main() {
    vec2 p = vWorld.xz;
    float dist = length(vWorld - uEye);
    vec3 c = vec3(0.006, 0.013, 0.022);

    // The grid: small hexagons, and a coarser set over them.
    float fade = exp(-dist / 7000.0);
    float fine = hexLine(p / 150.0, 0.02) * 0.32;
    float coarse = hexLine(p / 1050.0, 0.006) * 0.55;
    float grid = max(fine, coarse);
    // Brighter round the room in view.
    float rx = uRoom[uCurrentRoom].x;
    float near = exp(-length(vec2(p.x - rx, p.y + 1200.0)) / 4200.0);
    c += uHolo * grid * fade * (0.10 + 0.22 * near);

    // Rings of light running out over the floor.
    for (int i = 0; i < 3; ++i) {
        float age = uWave[i].z;
        if (age < 0.0 || age > 2.6) continue;
        float r = age * 2400.0;
        float d = length(p - uWave[i].xy);
        float band = exp(-pow((d - r) / (60.0 + age * 90.0), 2.0)) * exp(-age * 1.4) * uWave[i].w;
        c += uHolo * band * (0.05 + grid * 1.4);
    }

    // Rooms: a faint pool of each room's colour, and its time axis down the aisle.
    for (int i = 0; i < uRoomCount; ++i) {
        float x = uRoom[i].x;
        vec3 acc = uRoom[i].yzw;
        vec2 d = abs(p - vec2(x, -2400.0)) - vec2(uRoomPitch * 0.40, 3200.0);
        float rug = 1.0 - smoothstep(0.0, 400.0, length(max(d, 0.0)) + min(max(d.x, d.y), 0.0));
        c += acc * rug * (i == uCurrentRoom ? 0.020 : 0.010);
        float axis = exp(-pow((p.x - x) / 5.0, 2.0)) + 0.35 * exp(-pow((p.x - x) / 40.0, 2.0));
        float along = smoothstep(400.0, 0.0, p.y) * exp(min(p.y, 0.0) / 5000.0);
        along *= (i == uCurrentRoom) ? 1.0 : 0.0;
        // Ticks across the axis every 600 units, like a ruler, and a pulse
        // running along it into the past.
        float tick = gridLine(p.y, 600.0, 2.5) * exp(-pow((p.x - x) / 70.0, 2.0));
        float pulse = exp(-pow(fract(-p.y / 3000.0 - uTime * 0.25) - 0.5, 2.0) / 0.002);
        c += mix(uHolo, acc, 0.5) * (axis * (0.5 + 0.6 * pulse) + tick * 0.4) * along * min(uAisle, 1.0);
    }

    // Shadows: each window's bottom edge, softer the higher it floats.
    float lit = 1.0;
    for (int i = 0; i < uShadowCount; ++i) {
        float h = uShadowB[i].x;
        float d = segDist(p, uShadowA[i].xy, uShadowA[i].zw);
        float blur = 30.0 + h * 0.45;
        float s = exp(-(d * d) / (blur * blur)) * uShadowB[i].y;
        lit *= 1.0 - s * 0.8;
    }
    c *= lit;

    // Into the distance the floor gives way to the sky behind it: no edge.
    float far = smoothstep(2500.0, 40000.0, dist);
    c = mix(c, uHorizon, far * 0.6);
    frag = vec4(c, uAlpha * (1.0 - far));
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
uniform float uHolo;     // 0: the window as it is; 1: a hologram (in the background)
uniform float uReveal;   // the materialize, 0 -> 1 (1: done)
uniform float uGhost;    // folding away, 0 -> 1 (0: not)
uniform float uTime;
uniform vec2 uSizePx;    // the canvas in pixels
uniform vec3 uHoloColor;
void main() {
    vec4 c = texture(uTex, vUV);   // premultiplied
    float a = uAlpha;
    if (uHolo > 0.0) {
        // A hologram: light in one colour, scan lines, a glowing rim.
        float l = dot(c.rgb, vec3(0.299, 0.587, 0.114));
        vec3 holo = uHoloColor * (l * 1.25 + 0.04) * c.a;
        c.rgb = mix(c.rgb, holo, 0.6 * uHolo);
        float scan = 0.80 + 0.20 * sin(gl_FragCoord.y * 1.5707963 + uTime * 3.0);
        c.rgb *= mix(1.0, scan, uHolo);
        vec2 e = min(vUV, 1.0 - vUV) * uSizePx;
        float rim = exp(-min(e.x, e.y) / 5.0);
        c.rgb += uHoloColor * rim * 0.45 * uHolo * c.a;
        c.rgb *= 1.0 - uDim * 0.55;
        c *= 1.0 - 0.06 * uHolo * step(0.985, fract(uTime * 0.37 + vUV.y * 0.3));
    } else {
        // Into the background without the hologram: towards the hall's colour.
        float l = dot(c.rgb, vec3(0.299, 0.587, 0.114));
        vec3 grey = mix(c.rgb, vec3(l) * 0.9, uDim * 0.6);
        c.rgb = mix(grey, uFog * c.a, uDim * 0.75);
    }
    if (uReveal < 1.0) {
        // Materialize: a scan line runs down and leaves the window behind it;
        // ahead of it only a faint grid.
        float front = uReveal * 1.12 - 0.06;
        float d = vUV.y - front;
        if (d > 0.0) {
            vec2 g = abs(fract(vUV * uSizePx / 26.0) - 0.5);
            float grid = step(0.47, max(g.x, g.y)) * smoothstep(0.4, 0.0, d) * (1.0 - uReveal);
            c = vec4(uHoloColor * 0.6, 0.6) * grid * 0.45;
        }
        float band = exp(-pow(d / 0.010, 2.0));
        c.rgb += uHoloColor * band * 1.3;
        c.a = max(c.a, band * 0.85);
    }
    if (uGhost > 0.0) {
        // Folding away: the picture turns to light and goes out.
        c.rgb = mix(c.rgb, uHoloColor * max(c.a, 0.4) * 1.8, smoothstep(0.0, 0.45, uGhost));
        c.a = max(c.a, 0.4 * smoothstep(0.0, 0.3, uGhost));
        a *= 1.0 - smoothstep(0.75, 1.0, uGhost);
    }
    if (uEdgeAA != 0) {
        // Half a pixel of coverage at the edges of a quad that is not on the
        // pixel grid; one that is keeps its outermost row whole.
        vec2 px = min(vUV, 1.0 - vUV) / max(fwidth(vUV), vec2(1e-6));
        a *= clamp(min(px.x, px.y) + 0.5, 0.0, 1.0);
    }
    if (uReflection != 0) a *= 0.22 * exp(-(uFloorY - vWorld.y) / 260.0);
    frag = c * a;
})";

// A window's frame: a hairline round the window geometry, corner brackets
// that come in from outside and lock on when the window gets the focus, and
// a soft glow. Drawn on a quad bigger than the window by uMargin each side.
const char* kGlowFrag = R"(#version 330 core
in vec2 vUV;
in vec3 vWorld;
out vec4 frag;
uniform vec2 uSize;      // the quad in pixels (window plus margin on each side)
uniform float uMargin;
uniform vec3 uColor;
uniform float uStrength; // the outline and glow
uniform float uBrackets; // 0: none
uniform float uLock;     // 0 -> 1: the brackets coming in
uniform float uTime;

float segDist(vec2 p, vec2 a, vec2 b) {
    vec2 ab = b - a;
    float t = clamp(dot(p - a, ab) / dot(ab, ab), 0.0, 1.0);
    return length(p - (a + ab * t));
}

void main() {
    vec2 p = vUV * uSize;
    vec2 half_ = uSize * 0.5 - vec2(uMargin);   // the window's half size
    vec2 q = abs(p - uSize * 0.5);              // folded into one corner
    vec2 o = q - half_;
    float out_ = length(max(o, 0.0)) + min(max(o.x, o.y), 0.0);   // > 0 outside the window
    float px = max(fwidth(p.x), 1e-4);
    float a = 0.0;
    // Hairline just outside the window, and a soft glow.
    if (out_ > -0.5) {
        a += (1.0 - smoothstep(0.6 * px, 1.6 * px, abs(out_ - 1.0))) * 0.55 * uStrength;
        a += exp(-max(out_, 0.0) / 16.0) * 0.18 * uStrength;
    }
    if (uBrackets > 0.0) {
        // Brackets: an L at each corner, flying in from far out.
        float off = mix(46.0, 7.0, uLock);
        vec2 c = half_ + vec2(off);
        float len = 34.0;
        float d = min(segDist(q, c, c - vec2(len, 0.0)), segDist(q, c, c - vec2(0.0, len)));
        float th = 1.6 * px;
        float b = 1.0 - smoothstep(th, th + 1.2 * px, d);
        b += exp(-d / 6.0) * 0.35;
        float flash = 1.0 + 1.5 * exp(-uLock * 5.0) * step(uLock, 0.999);
        a += b * uBrackets * flash;
        // A small tick in the middle of each side.
        float mid = min(segDist(q, vec2(0.0, c.y), vec2(10.0, c.y)), segDist(q, vec2(c.x, 0.0), vec2(c.x, 10.0)));
        a += (1.0 - smoothstep(th, th + 1.2 * px, mid)) * uBrackets * 0.6;
    }
    a = clamp(a, 0.0, 1.0);
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

// A stage's picture (Stage.hpp): the whole screen, drawn from the camera the
// room is drawn from this frame, premultiplied, nothing where its world is not.
const char* kStageVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
out vec2 vUV;
void main() {
    // The client's first row is the top of the screen; GL's is the bottom.
    vUV = vec2(aPos.x, 1.0 - aPos.y);
    gl_Position = vec4(aPos.xy * 2.0 - 1.0, 0.0, 1.0);
})";
const char* kStageFrag = R"(#version 330 core
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

// Seconds for the shaders' motion, kept small so a float keeps its precision.
float shaderTime(double now) { return static_cast<float>(std::fmod(now, 3600.0)); }

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
    m_stageShader = fitzel::Shader::fromSource(kStageVert, kStageFrag);
    std::vector<fitzel::Vertex> v(4);
    v[0].position = {0, 0, 0}; v[1].position = {1, 0, 0};
    v[2].position = {1, 1, 0}; v[3].position = {0, 1, 0};
    for (auto& x : v) { x.normal = {0, 0, 1}; x.uv = glm::vec2(x.position); }
    m_quad = fitzel::Mesh::create(v, {0, 1, 2, 0, 2, 3});
}

DeskRenderer::~DeskRenderer() {
    for (auto& [img, tex] : m_cursorTextures)
        if (tex.valid()) m_server.gpu->device().destroy(tex);
}

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
    m_skyShader.setFloat("uTime", shaderTime(desk.now()));
    m_skyShader.setVec3("uZenith", theme::kSpaceZenith);
    m_skyShader.setVec3("uHorizon", theme::kSpaceHorizon);
    m_skyShader.setVec3("uHolo", theme::kHolo);
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
    m_floorShader.setFloat("uAlpha", 0.86f);
    m_floorShader.setFloat("uTime", shaderTime(desk.now()));
    m_floorShader.setVec3("uHolo", theme::kHolo);
    m_floorShader.setVec3("uHorizon", theme::kSpaceHorizon);
    for (int i = 0; i < 3; ++i) {
        const Desk::Wave& w = desk.waves[static_cast<std::size_t>(i)];
        m_floorShader.setVec4("uWave[" + std::to_string(i) + "]",
                              glm::vec4(w.origin.x, w.origin.y, static_cast<float>(desk.now() - w.start), w.strength));
    }
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

void DeskRenderer::drawFrame(const Pose& pose, const wlr_box& g, const glm::vec3& color, float strength,
                             float brackets, float lock) {
    // Round the window geometry (not its shadows), with room for the brackets.
    const Desk& desk = *m_server.desk;
    const float margin = 64.0f;
    const glm::vec3 R(std::cos(pose.yaw), 0.0f, -std::sin(pose.yaw));
    const glm::vec3 U(0.0f, 1.0f, 0.0f);
    const float s = pose.scale;
    const float hw = g.width * 0.5f + margin, hh = g.height * 0.5f + margin;
    // Just behind the window so the window covers its own middle.
    const glm::vec3 n(std::sin(pose.yaw), 0.0f, std::cos(pose.yaw));
    const glm::vec3 c = pose.center - n * 0.6f;
    m_glowShader.bind();
    m_glowShader.setMat4("uViewProj", desk.projMatrix() * desk.viewMatrix());
    m_glowShader.setVec3("uCorner[0]", c - R * (hw * s) + U * (hh * s));
    m_glowShader.setVec3("uCorner[1]", c + R * (hw * s) + U * (hh * s));
    m_glowShader.setVec3("uCorner[2]", c + R * (hw * s) - U * (hh * s));
    m_glowShader.setVec3("uCorner[3]", c - R * (hw * s) - U * (hh * s));
    m_glowShader.setVec2("uSize", glm::vec2(hw * 2.0f, hh * 2.0f));
    m_glowShader.setFloat("uMargin", margin);
    m_glowShader.setVec3("uColor", color);
    m_glowShader.setFloat("uStrength", strength);
    m_glowShader.setFloat("uBrackets", brackets);
    m_glowShader.setFloat("uLock", lock);
    m_glowShader.setFloat("uTime", shaderTime(desk.now()));
    m_server.gpu->device().setRenderState(blendPremultiplied(true, false));
    m_quad.draw();
}

void DeskRenderer::drawWindows(bool reflection) {
    const Desk& desk = *m_server.desk;
    rhi::Device& dev = m_server.gpu->device();
    const glm::mat4 vp = desk.projMatrix() * desk.viewMatrix();
    View* focus = m_server.focused();
    View* focusRoot = focus;
    while (focusRoot && focusRoot->parentView()) focusRoot = focusRoot->parentView();
    const float floorY = desk.floorY();
    const double now = desk.now();
    for (View* v : desk.drawList()) {
        if (!v->canvas.valid()) continue;
        if (v == desk.dragging) continue;
        Pose pose = v->pose;
        glm::vec3 c[4];
        desk.canvasQuad(v, pose, c);
        View* root = v;
        while (View* p = root->parentView()) root = p;
        const bool front = desk.level == Level::Work && root->slot >= 0 && root->room == desk.current;
        const float reveal = static_cast<float>(std::clamp((now - v->mappedAt) / 0.55, 0.0, 1.0));
        if (reflection) {
            for (glm::vec3& w : c) w.y = 2.0f * floorY - w.y;
        } else if (!v->parentView()) {
            // The frame: brackets on the window with the focus (they lock on
            // when it gets it), a hairline round the others.
            const glm::vec3 acc = desk.rooms[static_cast<std::size_t>(v->room)].accent;
            const glm::vec3 col = glm::mix(theme::kHolo, acc, 0.35f);
            const bool hover = v == desk.hovered && !front;
            const bool focused = v == focusRoot;
            const float lock = theme::easeBack(static_cast<float>((now - v->focusedAt) / 0.45));
            float strength = front ? (focused ? 0.35f : 0.18f) : (hover ? 0.8f : 0.3f);
            float brackets = focused ? (front ? 0.55f : 0.9f) : (hover ? 0.7f : 0.0f);
            // Fullscreen is a window that is the screen: no frame round it.
            if (root->fullscreen && desk.level == Level::Work) strength = brackets = 0.0f;
            const float a = pose.alpha * std::min(1.0f, reveal * 1.6f);
            if (strength > 0.0f || brackets > 0.0f)
                drawFrame(pose, v->geometry(), col, strength * a, brackets * a, focused ? lock : 1.0f);
        }
        m_windowShader.bind();
        m_windowShader.setMat4("uViewProj", vp);
        for (int i = 0; i < 4; ++i)
            m_windowShader.setVec3("uCorner[" + std::to_string(i) + "]", c[i]);
        float dim = pose.dim;
        if (v == desk.hovered && !front) dim *= 0.45f;
        m_windowShader.setFloat("uDim", dim);
        m_windowShader.setFloat("uAlpha", pose.alpha * std::min(1.0f, v->appear));
        m_windowShader.setVec3("uFog", glm::vec3(0.02f, 0.05f, 0.07f));
        m_windowShader.setInt("uReflection", reflection ? 1 : 0);
        m_windowShader.setFloat("uFloorY", floorY);
        // In the background a window is a hologram; in front it is itself.
        const float holo = front ? 0.0f : std::clamp(pose.dim * 2.4f, 0.0f, 1.0f) * (v == desk.hovered ? 0.35f : 1.0f);
        m_windowShader.setFloat("uHolo", holo);
        m_windowShader.setFloat("uReveal", reveal);
        m_windowShader.setFloat("uGhost", 0.0f);
        m_windowShader.setFloat("uTime", shaderTime(now));
        m_windowShader.setVec2("uSizePx", glm::vec2(v->canvas.width, v->canvas.height));
        m_windowShader.setVec3("uHoloColor", theme::kHolo);
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

void DeskRenderer::drawGhosts() {
    // Windows that went: their last picture folds to a line of light and out.
    Desk& desk = *m_server.desk;
    rhi::Device& dev = m_server.gpu->device();
    const glm::mat4 vp = desk.projMatrix() * desk.viewMatrix();
    const double now = desk.now();
    constexpr double kLife = 0.45;
    for (const Desk::Ghost& gh : desk.ghosts) {
        const float t = static_cast<float>(std::clamp((now - gh.start) / kLife, 0.0, 1.0));
        // First flat to a line, then the line in to a point.
        const float sy = 1.0f - 0.985f * theme::easeInOut(t / 0.6f);
        const float sx = 1.0f - theme::easeInOut((t - 0.55f) / 0.45f);
        const wlr_box g = gh.geometry;
        const wlr_box b = gh.canvas.box;
        const double cx = g.x + g.width * 0.5, cy = g.y + g.height * 0.5;
        const Pose& p = gh.pose;
        const glm::vec3 R(std::cos(p.yaw), 0.0f, -std::sin(p.yaw));
        const glm::vec3 U(0.0f, 1.0f, 0.0f);
        auto corner = [&](double lx, double ly) {
            return p.center + R * static_cast<float>((lx - cx) * p.scale * sx) -
                   U * static_cast<float>((ly - cy) * p.scale * sy);
        };
        m_windowShader.bind();
        m_windowShader.setMat4("uViewProj", vp);
        m_windowShader.setVec3("uCorner[0]", corner(b.x, b.y));
        m_windowShader.setVec3("uCorner[1]", corner(b.x + b.width, b.y));
        m_windowShader.setVec3("uCorner[2]", corner(b.x + b.width, b.y + b.height));
        m_windowShader.setVec3("uCorner[3]", corner(b.x, b.y + b.height));
        m_windowShader.setFloat("uDim", 0.0f);
        m_windowShader.setFloat("uAlpha", p.alpha);
        m_windowShader.setInt("uReflection", 0);
        m_windowShader.setFloat("uHolo", 0.0f);
        m_windowShader.setFloat("uReveal", 1.0f);
        m_windowShader.setFloat("uGhost", std::max(t, 0.001f));
        m_windowShader.setFloat("uTime", shaderTime(now));
        m_windowShader.setVec2("uSizePx", glm::vec2(gh.canvas.width, gh.canvas.height));
        m_windowShader.setVec3("uHoloColor", theme::kHolo);
        m_windowShader.setInt("uEdgeAA", 1);
        m_windowShader.setInt("uTex", 0);
        dev.bindTexture(0, gh.canvas.color);
        dev.setRenderState(blendPremultiplied(true, false));
        m_quad.draw();
    }
    // The ones that are done go, with their pictures.
    for (auto it = desk.ghosts.begin(); it != desk.ghosts.end();) {
        if (now - it->start >= kLife) {
            if (it->canvas.fbo.valid()) dev.destroy(it->canvas.fbo);
            if (it->canvas.color.valid()) dev.destroy(it->canvas.color);
            it = desk.ghosts.erase(it);
        } else {
            ++it;
        }
    }
}

void DeskRenderer::drawStage() {
    const Desk& desk = *m_server.desk;
    Stage* stage = desk.diorama();
    // Only a picture drawn from our camera belongs in the room; until the first
    // one has come, the room shows without it.
    if (!stage || stage->shownSerial == 0 || !stage->shownKnown) return;
    SurfaceTextures& st = *m_server.surfaces;
    const SurfaceTextures::Sampled sm = st.sampled(stage->surface());
    if (!sm.texture.valid()) return;
    static const bool debug = std::getenv("RAUM_DEBUG_STAGE") != nullptr;
    if (debug)
        std::fprintf(stderr, "[stage] picture %u %dx%d opaque %d swap %d eye %.0f,%.0f,%.0f\n",
                     stage->shownSerial, sm.width, sm.height, sm.opaque, sm.swapRB, stage->shownCam.eye.x,
                     stage->shownCam.eye.y, stage->shownCam.eye.z);
    st.waitReady(stage->surface());
    rhi::Device& dev = m_server.gpu->device();
    dev.setRenderState(blendPremultiplied(false, false));
    m_stageShader.bind();
    m_stageShader.setInt("uTex", 0);
    m_stageShader.setInt("uSwapRB", sm.swapRB ? 1 : 0);
    m_stageShader.setInt("uOpaque", sm.opaque ? 1 : 0);
    dev.bindTexture(0, sm.texture);
    m_quad.draw();
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
    dev.setClearColor(glm::vec4(theme::kSpaceZenith, 1.0f));
    dev.setClearDepth(1.0f);
    dev.clear(rhi::ClearColor | rhi::ClearDepth);
    drawSky();
    drawWindows(true);
    drawFloor(false);
    drawStage();
    drawWindows(false);
    drawGhosts();
    dev.setRenderState(rhi::RenderState{});
}

void DeskRenderer::drawCursor(double x, double y) {
    if (!m_scene || !m_server.softwareCursor()) return;
    const Server::CursorImage& ci = m_server.cursorImage;
    rhi::Device& dev = m_server.gpu->device();
    rhi::TextureHandle tex;
    int w = 0, h = 0, hx = 0, hy = 0;
    bool swap = false, opaque = false;
    if (ci.surface) {
        // The client's own cursor surface.
        SurfaceTextures& st = *m_server.surfaces;
        const SurfaceTextures::Sampled sm = st.sampled(ci.surface);
        if (!sm.texture.valid()) return;
        st.waitReady(ci.surface);
        tex = sm.texture;
        w = ci.surface->current.width;
        h = ci.surface->current.height;
        hx = ci.hotX;
        hy = ci.hotY;
        swap = sm.swapRB;
        opaque = sm.opaque;
    } else if (!ci.name.empty()) {
        wlr_xcursor* xc = wlr_xcursor_manager_get_xcursor(m_server.xcursor, ci.name.c_str(), 1.0f);
        if (!xc) xc = wlr_xcursor_manager_get_xcursor(m_server.xcursor, "default", 1.0f);
        if (!xc || xc->image_count == 0) return;
        // An animated one (the busy cursor) runs on the clock.
        unsigned frame = 0;
        if (xc->image_count > 1 && xc->total_delay > 0) {
            std::uint32_t t = static_cast<std::uint32_t>(m_server.desk->now() * 1000.0) % xc->total_delay;
            while (frame + 1 < xc->image_count && t >= xc->images[frame]->delay) t -= xc->images[frame++]->delay;
        }
        wlr_xcursor_image* img = xc->images[frame];
        auto it = m_cursorTextures.find(img);
        if (it == m_cursorTextures.end()) {
            rhi::TextureDesc d;
            d.width = static_cast<int>(img->width);
            d.height = static_cast<int>(img->height);
            d.mipLevels = 1;
            d.usage = rhi::UsageSampled;
            // ARGB8888, premultiplied: B, G, R, A in memory.
            it = m_cursorTextures.emplace(img, dev.createTexture(d, img->buffer, rhi::Format::RGBA8)).first;
        }
        tex = it->second;
        w = static_cast<int>(img->width);
        h = static_cast<int>(img->height);
        hx = static_cast<int>(img->hotspot_x);
        hy = static_cast<int>(img->hotspot_y);
        swap = true;
    } else {
        return;   // hidden by the client
    }
    m_scene->bind();
    dev.setRenderState(blendPremultiplied(false, false));
    m_surfaceShader.bind();
    m_surfaceShader.setVec2("uCanvas", glm::vec2(m_w, m_h));
    // The scene is the right way up for OpenGL (y up); the texture's first row
    // is the top of the image.
    const float x0 = static_cast<float>(std::floor(x)) - hx;
    const float yTop = static_cast<float>(std::floor(y)) - hy;
    m_surfaceShader.setVec4("uRect", glm::vec4(x0, m_h - yTop - h, w, h));
    m_surfaceShader.setVec4("uSrc", glm::vec4(0.0f, 1.0f, 1.0f, 0.0f));
    m_surfaceShader.setInt("uSwapRB", swap ? 1 : 0);
    m_surfaceShader.setInt("uOpaque", opaque ? 1 : 0);
    m_surfaceShader.setInt("uTex", 0);
    dev.bindTexture(0, tex);
    m_quad.draw();
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
