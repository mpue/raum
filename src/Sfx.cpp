#include "Sfx.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

#include <fitzel/audio/Audio.hpp>

namespace raum {

namespace fs = std::filesystem;

namespace {

constexpr int kRate = 48000;
constexpr float kTau = 6.28318530718f;
// Bump when a sound changes: the cache is rebuilt in a directory of its own.
constexpr const char* kCacheVersion = "sfx-5";

const char* kNames[Sfx::Count] = {"tick", "hover", "menu-open", "menu-close", "launch",
                                  "materialize", "dematerialize", "focus", "whoosh-left",
                                  "whoosh-right", "zoom-out", "zoom-in", "notify", "error",
                                  "shutter"};

double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// --- A little synthesiser ---------------------------------------------------------

struct Buffer {
    std::vector<float> l, r;
    explicit Buffer(float seconds)
        : l(static_cast<std::size_t>(seconds * kRate), 0.0f), r(l.size(), 0.0f) {}
    std::size_t size() const { return l.size(); }
    float time(std::size_t i) const { return static_cast<float>(i) / kRate; }
    void add(std::size_t i, float v, float pan = 0.0f) {   // pan -1 .. 1, constant power
        const float a = (std::clamp(pan, -1.0f, 1.0f) + 1.0f) * 0.25f * 3.14159265f;
        l[i] += v * std::cos(a);
        r[i] += v * std::sin(a);
    }
};

struct Noise {
    std::uint32_t s = 0x9E3779B9u;
    float next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return static_cast<float>(s) / 4294967295.0f * 2.0f - 1.0f;
    }
};

// To a peak level, with a few milliseconds of fade at the end so nothing clicks.
void finish(Buffer& b, float peak) {
    float m = 1e-6f;
    for (std::size_t i = 0; i < b.size(); ++i) m = std::max({m, std::abs(b.l[i]), std::abs(b.r[i])});
    const float g = peak / m;
    const std::size_t fade = std::min<std::size_t>(b.size(), kRate / 200);
    for (std::size_t i = 0; i < b.size(); ++i) {
        float f = g;
        if (i + fade >= b.size()) f *= static_cast<float>(b.size() - i) / fade;
        b.l[i] *= f;
        b.r[i] *= f;
    }
}

// A click: a struck resonance -- a sine at `freq` that rings out in `decay`
// seconds -- with a hint of noise at the very start for the contact. Short,
// dry, like a detent or a small switch; nothing glides, nothing swooshes.
void click(Buffer& b, float start, float freq, float decay, float level, float pan = 0.0f,
           float contact = 0.25f) {
    Noise n;
    n.s ^= static_cast<std::uint32_t>(freq * 977.0f + start * 100003.0f);
    const std::size_t i0 = static_cast<std::size_t>(start * kRate);
    const std::size_t len = static_cast<std::size_t>((decay * 7.0f + 0.002f) * kRate);
    float lp = 0.0f;
    for (std::size_t k = 0; k < len && i0 + k < b.size(); ++k) {
        const float t = static_cast<float>(k) / kRate;
        lp += 0.35f * (n.next() - lp);
        const float v = std::sin(kTau * freq * t) * std::exp(-t / decay) +
                        contact * lp * std::exp(-t / 0.0007f);
        b.add(i0 + k, v * level, pan);
    }
}

// The whole sound through one soft low-pass: crisp, but no sharp edge.
void soften(Buffer& b, float cutoff) {
    const float k = 1.0f - std::exp(-kTau * cutoff / kRate);
    float l = 0.0f, r = 0.0f;
    for (std::size_t i = 0; i < b.size(); ++i) {
        l += k * (b.l[i] - l);
        r += k * (b.r[i] - r);
        b.l[i] = l;
        b.r[i] = r;
    }
}

// Peak levels are low on purpose: these sit under whatever else is playing.
Buffer make(Sfx::Id id) {
    switch (id) {
        case Sfx::Tick: {            // one detent of the dial
            Buffer b(0.04f);
            click(b, 0.0f, 2400.0f, 0.0018f, 1.0f);
            soften(b, 7000.0f);
            finish(b, 0.05f);
            return b;
        }
        case Sfx::Hover: {           // a hair of a click
            Buffer b(0.03f);
            click(b, 0.0f, 3200.0f, 0.0010f, 1.0f, 0.0f, 0.1f);
            soften(b, 7000.0f);
            finish(b, 0.018f);
            return b;
        }
        case Sfx::MenuOpen: {        // a latch: two clicks, the second higher
            Buffer b(0.09f);
            click(b, 0.000f, 1500.0f, 0.0025f, 1.0f, -0.1f);
            click(b, 0.035f, 2300.0f, 0.0020f, 0.8f, 0.1f);
            soften(b, 6500.0f);
            finish(b, 0.06f);
            return b;
        }
        case Sfx::MenuClose: {       // the latch let go: two clicks, falling
            Buffer b(0.08f);
            click(b, 0.000f, 2100.0f, 0.0018f, 0.8f, 0.1f);
            click(b, 0.028f, 1400.0f, 0.0022f, 0.9f, -0.1f);
            soften(b, 6000.0f);
            finish(b, 0.045f);
            return b;
        }
        case Sfx::Launch: {          // a switch thrown: click, and its echo
            Buffer b(0.12f);
            click(b, 0.000f, 1800.0f, 0.0030f, 1.0f, 0.0f, 0.35f);
            click(b, 0.045f, 2700.0f, 0.0018f, 0.45f, 0.0f);
            click(b, 0.075f, 2700.0f, 0.0018f, 0.2f, 0.0f);
            soften(b, 6500.0f);
            finish(b, 0.06f);
            return b;
        }
        case Sfx::Materialize: {     // a relay pulling in: three quick clicks
            Buffer b(0.12f);
            click(b, 0.000f, 1900.0f, 0.0015f, 0.5f);
            click(b, 0.030f, 2100.0f, 0.0015f, 0.7f);
            click(b, 0.052f, 2300.0f, 0.0020f, 1.0f);
            soften(b, 6500.0f);
            finish(b, 0.045f);
            return b;
        }
        case Sfx::Dematerialize: {   // the relay dropping out: two, falling
            Buffer b(0.08f);
            click(b, 0.000f, 1900.0f, 0.0020f, 1.0f);
            click(b, 0.032f, 1300.0f, 0.0018f, 0.6f);
            soften(b, 6000.0f);
            finish(b, 0.04f);
            return b;
        }
        case Sfx::Focus: {
            Buffer b(0.03f);
            click(b, 0.0f, 2800.0f, 0.0012f, 1.0f, 0.0f, 0.15f);
            soften(b, 7000.0f);
            finish(b, 0.025f);
            return b;
        }
        case Sfx::WhooshLeft:
        case Sfx::WhooshRight: {     // a carriage sliding over: clicks walking sideways
            Buffer b(0.11f);
            const float s = id == Sfx::WhooshRight ? 1.0f : -1.0f;
            click(b, 0.000f, 1700.0f, 0.0016f, 0.7f, -0.6f * s);
            click(b, 0.030f, 1700.0f, 0.0016f, 0.8f, 0.0f);
            click(b, 0.060f, 1700.0f, 0.0018f, 0.9f, 0.6f * s);
            soften(b, 6000.0f);
            finish(b, 0.045f);
            return b;
        }
        case Sfx::ZoomOut:
        case Sfx::ZoomIn: {          // a lens ring: a lower or a higher pair
            Buffer b(0.07f);
            const bool out = id == Sfx::ZoomOut;
            click(b, 0.000f, out ? 1500.0f : 1100.0f, 0.0022f, 0.8f);
            click(b, 0.032f, out ? 1100.0f : 1500.0f, 0.0022f, 0.9f);
            soften(b, 5500.0f);
            finish(b, 0.045f);
            return b;
        }
        case Sfx::Notify: {
            Buffer b(0.07f);
            click(b, 0.000f, 2600.0f, 0.0015f, 0.8f);
            click(b, 0.040f, 2600.0f, 0.0015f, 0.6f);
            soften(b, 7000.0f);
            finish(b, 0.035f);
            return b;
        }
        case Sfx::Error: {           // a dull thock, twice
            Buffer b(0.12f);
            click(b, 0.000f, 420.0f, 0.0060f, 1.0f, 0.0f, 0.15f);
            click(b, 0.065f, 380.0f, 0.0060f, 0.9f, 0.0f, 0.15f);
            soften(b, 2500.0f);
            finish(b, 0.06f);
            return b;
        }
        case Sfx::Shutter: {         // a small shutter: click-clack
            Buffer b(0.08f);
            click(b, 0.000f, 2200.0f, 0.0012f, 1.0f, 0.0f, 0.5f);
            click(b, 0.045f, 1600.0f, 0.0016f, 0.8f, 0.0f, 0.5f);
            soften(b, 6500.0f);
            finish(b, 0.05f);
            return b;
        }
        case Sfx::Count: break;
    }
    return Buffer(0.01f);
}

bool writeWav(const std::string& path, const Buffer& b) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    const std::uint32_t frames = static_cast<std::uint32_t>(b.size());
    const std::uint32_t dataBytes = frames * 4;
    auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4); u32(36 + dataBytes); out.write("WAVE", 4);
    out.write("fmt ", 4); u32(16); u16(1); u16(2); u32(kRate); u32(kRate * 4); u16(4); u16(16);
    out.write("data", 4); u32(dataBytes);
    for (std::uint32_t i = 0; i < frames; ++i) {
        auto s16 = [](float v) { return static_cast<std::int16_t>(std::clamp(v, -1.0f, 1.0f) * 32767.0f); };
        const std::int16_t l = s16(b.l[i]), r = s16(b.r[i]);
        out.write(reinterpret_cast<const char*>(&l), 2);
        out.write(reinterpret_cast<const char*>(&r), 2);
    }
    return static_cast<bool>(out);
}

std::string cacheDir() {
    std::string base;
    if (const char* c = std::getenv("XDG_CACHE_HOME"); c && *c) base = c;
    else if (const char* h = std::getenv("HOME")) base = std::string(h) + "/.cache";
    else base = "/tmp";
    return base + "/raum/" + kCacheVersion;
}

} // namespace

Sfx::Sfx(bool allowed, bool muted) : m_muted(muted) {
    const std::string dir = cacheDir();
    if (!generate(dir)) return;
    if (!allowed || std::getenv("RAUM_MUTE")) return;
    m_audio = std::make_unique<fitzel::Audio>(2);
    if (!m_audio->ok()) {
        m_audio.reset();
        return;
    }
    for (int i = 0; i < Count; ++i) {
        auto s = std::make_unique<fitzel::Sound>(
            fitzel::Sound::fromFile(*m_audio, dir + "/" + kNames[i] + ".wav"));
        if (s->isValid()) m_sounds[static_cast<std::size_t>(i)] = std::move(s);
    }
}

Sfx::~Sfx() {
    for (auto& s : m_sounds) s.reset();
    m_audio.reset();
}

bool Sfx::generate(const std::string& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    for (int i = 0; i < Count; ++i) {
        const std::string path = dir + "/" + kNames[i] + ".wav";
        if (fs::exists(path, ec)) continue;
        if (!writeWav(path, make(static_cast<Id>(i)))) {
            std::fprintf(stderr, "[raum] cannot write %s\n", path.c_str());
            return false;
        }
    }
    return true;
}

double Sfx::sinceLast() const { return nowSeconds() - m_last; }

void Sfx::play(Id id, float volume) {
    m_last = nowSeconds();
    if (m_muted || !m_audio) return;
    if (auto& s = m_sounds[static_cast<std::size_t>(id)]) {
        s->setVolume(volume);
        s->play();
    }
}

} // namespace raum
