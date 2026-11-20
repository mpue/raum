#include "HoloIcons.hpp"

#include <imgui_internal.h>   // ImTextCharFromUtf8

#include <cmath>

namespace raum {

namespace {

constexpr float kPi = 3.14159265f;

ImVec2 P(ImVec2 c, float r, float x, float y) { return ImVec2(c.x + x * r, c.y + y * r); }

void line(ImDrawList* dl, ImVec2 c, float r, float x0, float y0, float x1, float y1, ImU32 col, float t) {
    dl->AddLine(P(c, r, x0, y0), P(c, r, x1, y1), col, t);
}

void arc(ImDrawList* dl, ImVec2 c, float rad, float a0, float a1, ImU32 col, float t) {
    dl->PathArcTo(c, rad, a0, a1, 24);
    dl->PathStroke(col, ImDrawFlags_None, t);
}

} // namespace

void hexagon(ImDrawList* dl, ImVec2 c, float r, ImU32 col, float t, bool filled) {
    ImVec2 pts[6];
    for (int i = 0; i < 6; ++i) {
        const float a = kPi / 3.0f * i - kPi / 2.0f;
        pts[i] = ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r);
    }
    if (filled) dl->AddConvexPolyFilled(pts, 6, col);
    else dl->AddPolyline(pts, 6, col, ImDrawFlags_Closed, t);
}

void holoFrame(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 lineCol, ImU32 bracket, float len, float open) {
    dl->AddRect(a, b, lineCol, 0.0f, ImDrawFlags_None, 1.0f);
    const float o = (1.0f - open) * 18.0f + 3.0f;   // brackets sit a little outside
    const ImVec2 A(a.x - o, a.y - o), B(b.x + o, b.y + o);
    const float t = 2.0f;
    dl->AddLine(A, ImVec2(A.x + len, A.y), bracket, t);
    dl->AddLine(A, ImVec2(A.x, A.y + len), bracket, t);
    dl->AddLine(ImVec2(B.x, A.y), ImVec2(B.x - len, A.y), bracket, t);
    dl->AddLine(ImVec2(B.x, A.y), ImVec2(B.x, A.y + len), bracket, t);
    dl->AddLine(ImVec2(A.x, B.y), ImVec2(A.x + len, B.y), bracket, t);
    dl->AddLine(ImVec2(A.x, B.y), ImVec2(A.x, B.y - len), bracket, t);
    dl->AddLine(B, ImVec2(B.x - len, B.y), bracket, t);
    dl->AddLine(B, ImVec2(B.x, B.y - len), bracket, t);
}

float spacedWidth(ImFont* font, float size, const char* text, float spacing) {
    float w = 0;
    int n = 0;
    for (const char* p = text; *p;) {
        unsigned int cp = 0;
        const int len = ImTextCharFromUtf8(&cp, p, nullptr);
        if (len <= 0) break;
        w += font->CalcTextSizeA(size, 1e9f, 0.0f, p, p + len).x;
        p += len;
        ++n;
    }
    return w + spacing * (n > 0 ? n - 1 : 0);
}

void spacedText(ImDrawList* dl, ImFont* font, float size, ImVec2 at, ImU32 col, const char* text,
                float spacing) {
    float x = at.x;
    for (const char* p = text; *p;) {
        unsigned int cp = 0;
        const int len = ImTextCharFromUtf8(&cp, p, nullptr);
        if (len <= 0) break;
        dl->AddText(font, size, ImVec2(x, at.y), col, p, p + len);
        x += font->CalcTextSizeA(size, 1e9f, 0.0f, p, p + len).x + spacing;
        p += len;
    }
}

void drawGlyph(ImDrawList* dl, Glyph g, ImVec2 c, float r, ImU32 col, float t) {
    switch (g) {
        case Glyph::All:
            for (int y = -1; y <= 1; ++y)
                for (int x = -1; x <= 1; ++x) {
                    const ImVec2 p = P(c, r, x * 0.62f, y * 0.62f);
                    dl->AddRect(ImVec2(p.x - r * 0.2f, p.y - r * 0.2f), ImVec2(p.x + r * 0.2f, p.y + r * 0.2f),
                                col, 0.0f, ImDrawFlags_None, t);
                }
            break;
        case Glyph::Recent:
            dl->AddCircle(c, r * 0.85f, col, 32, t);
            line(dl, c, r, 0, 0, 0, -0.55f, col, t);
            line(dl, c, r, 0, 0, 0.4f, 0.22f, col, t);
            break;
        case Glyph::Office:
            dl->AddRect(P(c, r, -0.6f, -0.85f), P(c, r, 0.6f, 0.85f), col, 0.0f, ImDrawFlags_None, t);
            for (int i = 0; i < 4; ++i) line(dl, c, r, -0.35f, -0.45f + i * 0.3f, i == 3 ? 0.1f : 0.35f, -0.45f + i * 0.3f, col, t);
            break;
        case Glyph::Internet:
            dl->AddCircle(c, r * 0.85f, col, 32, t);
            dl->AddEllipse(c, ImVec2(r * 0.38f, r * 0.85f), col, 0.0f, 32, t);
            line(dl, c, r, -0.85f, 0, 0.85f, 0, col, t);
            line(dl, c, r, -0.72f, -0.42f, 0.72f, -0.42f, col, t);
            line(dl, c, r, -0.72f, 0.42f, 0.72f, 0.42f, col, t);
            break;
        case Glyph::Graphics:
            dl->AddRect(P(c, r, -0.85f, -0.7f), P(c, r, 0.85f, 0.7f), col, 0.0f, ImDrawFlags_None, t);
            dl->PathLineTo(P(c, r, -0.85f, 0.6f));
            dl->PathLineTo(P(c, r, -0.25f, -0.05f));
            dl->PathLineTo(P(c, r, 0.15f, 0.35f));
            dl->PathLineTo(P(c, r, 0.45f, 0.1f));
            dl->PathLineTo(P(c, r, 0.85f, 0.5f));
            dl->PathStroke(col, ImDrawFlags_None, t);
            dl->AddCircle(P(c, r, 0.42f, -0.32f), r * 0.14f, col, 12, t);
            break;
        case Glyph::Media:
            dl->AddCircle(c, r * 0.85f, col, 32, t);
            dl->AddTriangle(P(c, r, -0.25f, -0.42f), P(c, r, -0.25f, 0.42f), P(c, r, 0.45f, 0.0f), col, t);
            break;
        case Glyph::Development:
            dl->PathLineTo(P(c, r, -0.35f, -0.6f));
            dl->PathLineTo(P(c, r, -0.85f, 0.0f));
            dl->PathLineTo(P(c, r, -0.35f, 0.6f));
            dl->PathStroke(col, ImDrawFlags_None, t);
            dl->PathLineTo(P(c, r, 0.35f, -0.6f));
            dl->PathLineTo(P(c, r, 0.85f, 0.0f));
            dl->PathLineTo(P(c, r, 0.35f, 0.6f));
            dl->PathStroke(col, ImDrawFlags_None, t);
            line(dl, c, r, 0.15f, -0.75f, -0.15f, 0.75f, col, t);
            break;
        case Glyph::Games:
            dl->AddRect(P(c, r, -0.9f, -0.45f), P(c, r, 0.9f, 0.5f), col, r * 0.35f, ImDrawFlags_None, t);
            line(dl, c, r, -0.62f, 0.02f, -0.22f, 0.02f, col, t);
            line(dl, c, r, -0.42f, -0.18f, -0.42f, 0.22f, col, t);
            dl->AddCircle(P(c, r, 0.35f, -0.05f), r * 0.09f, col, 8, t);
            dl->AddCircle(P(c, r, 0.58f, 0.15f), r * 0.09f, col, 8, t);
            break;
        case Glyph::System: {
            dl->AddCircle(c, r * 0.32f, col, 16, t);
            for (int i = 0; i < 8; ++i) {
                const float a = kPi / 4.0f * i;
                line(dl, c, r, std::cos(a) * 0.55f, std::sin(a) * 0.55f, std::cos(a) * 0.88f,
                     std::sin(a) * 0.88f, col, t * 1.4f);
            }
            dl->AddCircle(c, r * 0.6f, col, 24, t);
            break;
        }
        case Glyph::Accessories:
            dl->AddCircle(P(c, r, 0.45f, -0.45f), r * 0.32f, col, 16, t);
            line(dl, c, r, 0.22f, -0.22f, -0.75f, 0.75f, col, t * 1.6f);
            break;
        case Glyph::NewRoom:
            hexagon(dl, c, r * 0.9f, col, t);
            line(dl, c, r, -0.4f, 0, 0.4f, 0, col, t);
            line(dl, c, r, 0, -0.4f, 0, 0.4f, col, t);
            break;
        case Glyph::Screenshot:
            for (int sx = -1; sx <= 1; sx += 2)
                for (int sy = -1; sy <= 1; sy += 2) {
                    line(dl, c, r, sx * 0.85f, sy * 0.85f, sx * 0.45f, sy * 0.85f, col, t);
                    line(dl, c, r, sx * 0.85f, sy * 0.85f, sx * 0.85f, sy * 0.45f, col, t);
                }
            dl->AddCircle(c, r * 0.3f, col, 16, t);
            break;
        case Glyph::Help:
            arc(dl, P(c, r, 0, -0.3f), r * 0.38f, kPi, kPi * 2.25f, col, t);
            line(dl, c, r, 0.27f, -0.03f, 0.0f, 0.18f, col, t);
            line(dl, c, r, 0.0f, 0.18f, 0.0f, 0.38f, col, t);
            dl->AddCircleFilled(P(c, r, 0, 0.7f), t * 1.1f, col);
            break;
        case Glyph::SoundOn:
        case Glyph::SoundOff:
            dl->PathLineTo(P(c, r, -0.8f, -0.25f));
            dl->PathLineTo(P(c, r, -0.45f, -0.25f));
            dl->PathLineTo(P(c, r, 0.0f, -0.65f));
            dl->PathLineTo(P(c, r, 0.0f, 0.65f));
            dl->PathLineTo(P(c, r, -0.45f, 0.25f));
            dl->PathLineTo(P(c, r, -0.8f, 0.25f));
            dl->PathStroke(col, ImDrawFlags_Closed, t);
            if (g == Glyph::SoundOn) {
                arc(dl, P(c, r, 0.05f, 0), r * 0.35f, -0.9f, 0.9f, col, t);
                arc(dl, P(c, r, 0.05f, 0), r * 0.7f, -0.9f, 0.9f, col, t);
            } else {
                line(dl, c, r, 0.3f, -0.3f, 0.85f, 0.3f, col, t);
                line(dl, c, r, 0.3f, 0.3f, 0.85f, -0.3f, col, t);
            }
            break;
        case Glyph::Power:
            arc(dl, c, r * 0.75f, -kPi * 0.5f + 0.6f, kPi * 1.5f - 0.6f, col, t);
            line(dl, c, r, 0, -0.9f, 0, -0.15f, col, t);
            break;
        case Glyph::Search:
            dl->AddCircle(P(c, r, -0.15f, -0.15f), r * 0.55f, col, 20, t);
            line(dl, c, r, 0.25f, 0.25f, 0.8f, 0.8f, col, t * 1.4f);
            break;
        case Glyph::Logo:
            hexagon(dl, c, r, col, t);
            hexagon(dl, c, r * 0.58f, col, t);
            line(dl, c, r, 0, -1.0f, 0, -0.58f, col, t);
            line(dl, c, r, 0, 1.0f, 0, 0.58f, col, t);
            break;
    }
}

} // namespace raum
