#pragma once

#include <array>
#include <memory>
#include <string>

namespace fitzel {
class Audio;
class Sound;
} // namespace fitzel

namespace raum {

// raum's sounds. None of them is a file someone recorded: each is synthesised
// here from oscillators, noise and filters the first time raum runs (written as
// WAV into ~/.cache/raum and played through fitzel's audio from there). Short,
// soft and in one family -- the click of a holographic instrument, not a game.
//
// Silent when muted (Super+M, kept in the state), headless (tests must not make
// noise) or with RAUM_MUTE set.
class Sfx {
public:
    enum Id {
        Tick,          // a step: the ring turning, a key in the menu
        Hover,         // the pointer onto something that can be clicked
        MenuOpen,
        MenuClose,
        Launch,
        Materialize,   // a window appears
        Dematerialize, // a window goes
        Focus,
        WhooshLeft,    // to the room on the left
        WhooshRight,
        ZoomOut,       // leaning back
        ZoomIn,
        Notify,
        Error,
        Shutter,
        Count
    };

    // `allowed`: whether there is anybody to hear it at all (not headless).
    Sfx(bool allowed, bool muted);
    ~Sfx();

    void play(Id id, float volume = 1.0f);
    bool muted() const { return m_muted; }
    void setMuted(bool m) { m_muted = m; }
    // Seconds since anything was played: a note does not stack on a sound
    // that already says the same.
    double sinceLast() const;

private:
    bool generate(const std::string& dir);

    std::unique_ptr<fitzel::Audio> m_audio;
    std::array<std::unique_ptr<fitzel::Sound>, Count> m_sounds;
    bool m_muted = false;
    double m_last = -1e9;
};

} // namespace raum
