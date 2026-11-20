#pragma once

#include <imgui.h>

namespace raum {

// raum's own icons, drawn as light lines rather than loaded: they scale to any
// size, take any colour and can be animated stroke by stroke. Programs keep
// their own icons (Apps); these are for raum's categories and actions.
enum class Glyph {
    All, Recent, Office, Internet, Graphics, Media, Development, Games, System, Accessories,
    NewRoom, Screenshot, Help, SoundOn, SoundOff, Power, Search, Logo,
};

// `c` the centre, `r` half the size, `t` the line width.
void drawGlyph(ImDrawList* dl, Glyph g, ImVec2 c, float r, ImU32 col, float t = 1.6f);

// A hexagon (pointy top), outlined or filled.
void hexagon(ImDrawList* dl, ImVec2 c, float r, ImU32 col, float t, bool filled = false);

// The HUD's frame for a rectangle: a hairline outline and brighter corner
// brackets `len` long; `open` 0..1 lets the brackets come in from outside.
void holoFrame(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 line, ImU32 bracket, float len = 14.0f,
               float open = 1.0f);

// Text with spacing between the letters (headings in capitals).
void spacedText(ImDrawList* dl, ImFont* font, float size, ImVec2 at, ImU32 col, const char* text,
                float spacing);
float spacedWidth(ImFont* font, float size, const char* text, float spacing);

} // namespace raum
