#include "Hud.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <filesystem>

#include <glm/glm.hpp>
#include <imgui.h>

#include <fitzel/rhi/Device.hpp>

#include "Apps.hpp"
#include "Desk.hpp"
#include "Gpu.hpp"
#include "Server.hpp"
#include "State.hpp"
#include "View.hpp"

namespace raum {

namespace {

ImU32 rgba(const glm::vec3& c, float a) {
    return IM_COL32(static_cast<int>(c.r * 255), static_cast<int>(c.g * 255),
                    static_cast<int>(c.b * 255), static_cast<int>(a * 255));
}

ImVec4 vec4(const glm::vec3& c, float a) { return ImVec4(c.r, c.g, c.b, a); }

const char* firstFont(std::initializer_list<const char*> paths) {
    std::error_code ec;
    for (const char* p : paths)
        if (std::filesystem::exists(p, ec)) return p;
    return nullptr;
}

std::string ago(double seconds) {
    if (seconds < 50) return "gerade eben";
    const int min = static_cast<int>(seconds / 60.0 + 0.5);
    if (min < 60) return "vor " + std::to_string(std::max(1, min)) + " min";
    const int h = static_cast<int>(seconds / 3600.0 + 0.5);
    if (h < 24) return "vor " + std::to_string(h) + (h == 1 ? " Stunde" : " Stunden");
    const int d = static_cast<int>(seconds / 86400.0 + 0.5);
    return "vor " + std::to_string(d) + (d == 1 ? " Tag" : " Tagen");
}

// Shorten to fit `width` pixels in the current font, with an ellipsis.
std::string fit(const std::string& s, float width) {
    if (ImGui::CalcTextSize(s.c_str()).x <= width) return s;
    std::string t = s;
    while (!t.empty() && ImGui::CalcTextSize((t + "…").c_str()).x > width) {
        t.pop_back();
        while (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0x80) t.pop_back();
    }
    return t + "…";
}

ImGuiKey imguiKey(xkb_keysym_t sym) {
    switch (sym) {
        case XKB_KEY_Return: case XKB_KEY_KP_Enter: return ImGuiKey_Enter;
        case XKB_KEY_Escape: return ImGuiKey_Escape;
        case XKB_KEY_BackSpace: return ImGuiKey_Backspace;
        case XKB_KEY_Delete: return ImGuiKey_Delete;
        case XKB_KEY_Left: return ImGuiKey_LeftArrow;
        case XKB_KEY_Right: return ImGuiKey_RightArrow;
        case XKB_KEY_Up: return ImGuiKey_UpArrow;
        case XKB_KEY_Down: return ImGuiKey_DownArrow;
        case XKB_KEY_Home: return ImGuiKey_Home;
        case XKB_KEY_End: return ImGuiKey_End;
        case XKB_KEY_Tab: return ImGuiKey_Tab;
        case XKB_KEY_Page_Up: return ImGuiKey_PageUp;
        case XKB_KEY_Page_Down: return ImGuiKey_PageDown;
        default: break;
    }
    if (sym >= XKB_KEY_a && sym <= XKB_KEY_z) return static_cast<ImGuiKey>(ImGuiKey_A + (sym - XKB_KEY_a));
    if (sym >= XKB_KEY_A && sym <= XKB_KEY_Z) return static_cast<ImGuiKey>(ImGuiKey_A + (sym - XKB_KEY_A));
    return ImGuiKey_None;
}

const char* kWeekdays[] = {"So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"};
const char* kMonths[] = {"Jan.", "Feb.", "März", "Apr.", "Mai", "Juni",
                         "Juli", "Aug.", "Sep.", "Okt.", "Nov.", "Dez."};

const glm::vec3 kText(0.88f, 0.90f, 0.93f);
const glm::vec3 kDimText(0.55f, 0.59f, 0.65f);
const glm::vec3 kPanel(0.075f, 0.085f, 0.105f);

} // namespace

Hud::Hud(Server& server) : m_server(server) {
    IMGUI_CHECKVERSION();
    m_ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Every queued key and character in the next frame, not one per frame: a
    // name typed quickly and confirmed with Enter has to be in the field by
    // the time Enter is acted on.
    io.ConfigInputTrickleEventQueue = false;
    io.BackendPlatformName = "raum";
    const char* regular = firstFont({"/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                                     "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"});
    const char* bold = firstFont({"/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
                                  "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"});
    // Noto has no arrows; DejaVu fills in what a font is missing.
    const char* fallback = firstFont({"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"});
    const char* fallbackBold = firstFont({"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"});
    auto withFallback = [&](const char* main, const char* extra) -> ImFont* {
        if (!main) return nullptr;
        ImFont* f = io.Fonts->AddFontFromFileTTF(main, 16.0f);
        if (f && extra) {
            ImFontConfig cfg;
            cfg.MergeMode = true;
            io.Fonts->AddFontFromFileTTF(extra, 16.0f, &cfg);
        }
        return f;
    };
    m_font = withFallback(regular, fallback);
    if (!m_font) m_font = io.Fonts->AddFontDefault();
    m_bold = withFallback(bold, fallbackBold);
    if (!m_bold) m_bold = m_font;

    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 14.0f;
    st.FrameRounding = 8.0f;
    st.PopupRounding = 10.0f;
    st.WindowBorderSize = 0.0f;
    st.FrameBorderSize = 0.0f;
    st.WindowPadding = ImVec2(18, 16);
    st.FramePadding = ImVec2(12, 8);
    st.ItemSpacing = ImVec2(10, 8);
    st.Colors[ImGuiCol_WindowBg] = vec4(kPanel, 0.94f);
    st.Colors[ImGuiCol_Text] = vec4(kText, 1.0f);
    st.Colors[ImGuiCol_TextDisabled] = vec4(kDimText, 1.0f);
    st.Colors[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, 0.06f);
    st.Colors[ImGuiCol_FrameBgHovered] = ImVec4(1, 1, 1, 0.09f);
    st.Colors[ImGuiCol_Button] = ImVec4(1, 1, 1, 0.0f);
    st.Colors[ImGuiCol_ButtonHovered] = ImVec4(1, 1, 1, 0.08f);
    st.Colors[ImGuiCol_ButtonActive] = ImVec4(1, 1, 1, 0.14f);
    st.Colors[ImGuiCol_Header] = ImVec4(1, 1, 1, 0.08f);
    st.Colors[ImGuiCol_HeaderHovered] = ImVec4(1, 1, 1, 0.10f);
    st.Colors[ImGuiCol_HeaderActive] = ImVec4(1, 1, 1, 0.14f);

    m_server.gpu->device().imguiInit(nullptr);
}

Hud::~Hud() {
    if (m_ctx) {
        ImGui::SetCurrentContext(m_ctx);
        m_server.gpu->makeCurrent();
        m_server.gpu->device().imguiShutdown();
        ImGui::DestroyContext(m_ctx);
    }
}

bool Hud::animating() const {
    return !m_toasts.empty() || m_mode == Mode::Launcher || m_mode == Mode::Rename;
}

// --- Input -------------------------------------------------------------------------

void Hud::pointerMotion(double x, double y) {
    ImGui::SetCurrentContext(m_ctx);
    ImGui::GetIO().AddMousePosEvent(static_cast<float>(x), static_cast<float>(y));
}

bool Hud::wantsPointer() const {
    if (m_mode != Mode::None) return true;
    ImGui::SetCurrentContext(m_ctx);
    return ImGui::GetIO().WantCaptureMouse;
}

bool Hud::wantsKeyboard() const { return m_mode != Mode::None; }

bool Hud::pointerButton(std::uint32_t button, bool pressed) {
    if (!wantsPointer() && pressed) return false;
    ImGui::SetCurrentContext(m_ctx);
    int b = -1;
    if (button == 0x110) b = 0;        // BTN_LEFT
    else if (button == 0x111) b = 1;   // BTN_RIGHT
    else if (button == 0x112) b = 2;   // BTN_MIDDLE
    if (b >= 0) ImGui::GetIO().AddMouseButtonEvent(b, pressed);
    m_hadInput = true;
    m_server.scheduleFrame();
    return true;
}

bool Hud::pointerAxis(double dx, double dy) {
    if (!wantsPointer()) return false;
    ImGui::SetCurrentContext(m_ctx);
    ImGui::GetIO().AddMouseWheelEvent(static_cast<float>(-dx / 15.0), static_cast<float>(-dy / 15.0));
    m_server.scheduleFrame();
    return true;
}

bool Hud::key(xkb_keysym_t sym, bool pressed, std::uint32_t mods, const char* utf8) {
    static const bool debug = std::getenv("RAUM_DEBUG_HUD") != nullptr;
    if (debug)
        std::fprintf(stderr, "[hud] key 0x%x %s mods 0x%x utf8 '%s' mode %d focusPending %d\n", sym,
                     pressed ? "down" : "up", mods, utf8 ? utf8 : "", static_cast<int>(m_mode), m_focusInput);
    if (!wantsKeyboard()) return false;
    ImGui::SetCurrentContext(m_ctx);
    ImGuiIO& io = ImGui::GetIO();
    m_server.scheduleFrame();
    if (pressed) {
        if (sym == XKB_KEY_Escape) {
            close();
            return true;
        }
        if (m_mode == Mode::Launcher) {
            const int n = static_cast<int>(m_matches.size());
            if (sym == XKB_KEY_Down) { m_selected = std::min(m_selected + 1, std::max(0, n - 1)); return true; }
            if (sym == XKB_KEY_Up) { m_selected = std::max(m_selected - 1, 0); return true; }
            if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
                m_accept = true;
                return true;
            }
        } else if (m_mode == Mode::Rename) {
            if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
                m_accept = true;
                return true;
            }
        } else if (m_mode == Mode::Quit) {
            if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter || sym == XKB_KEY_e || sym == XKB_KEY_E)
                m_server.quit();
            return true;
        } else if (m_mode == Mode::Help) {
            close();
            return true;
        }
    }
    // Typed before the card's first frame gave its field the focus: straight
    // into the field (replacing what is there, as the select-all would).
    if (m_focusInput && (m_mode == Mode::Launcher || m_mode == Mode::Rename)) {
        char* buf = m_mode == Mode::Launcher ? m_query : m_rename;
        const std::size_t cap = m_mode == Mode::Launcher ? sizeof(m_query) : sizeof(m_rename);
        if (pressed && !m_typedEarly) {
            buf[0] = 0;
            m_typedEarly = true;
        }
        if (pressed && sym == XKB_KEY_BackSpace) {
            std::size_t n = std::strlen(buf);
            while (n > 0 && (static_cast<unsigned char>(buf[n - 1]) & 0xC0) == 0x80) --n;
            if (n > 0) buf[n - 1] = 0;
        } else if (pressed && utf8 && static_cast<unsigned char>(utf8[0]) >= 0x20 && utf8[0] != 0x7f &&
                   !(mods & (WLR_MODIFIER_CTRL | WLR_MODIFIER_LOGO))) {
            std::strncat(buf, utf8, cap - std::strlen(buf) - 1);
        }
        return true;
    }
    io.AddKeyEvent(ImGuiMod_Ctrl, (mods & WLR_MODIFIER_CTRL) != 0);
    io.AddKeyEvent(ImGuiMod_Shift, (mods & WLR_MODIFIER_SHIFT) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, (mods & WLR_MODIFIER_ALT) != 0);
    const ImGuiKey k = imguiKey(sym);
    if (k != ImGuiKey_None) io.AddKeyEvent(k, pressed);
    if (pressed && utf8 && utf8[0] && static_cast<unsigned char>(utf8[0]) >= 0x20 &&
        utf8[0] != 0x7f && !(mods & (WLR_MODIFIER_CTRL | WLR_MODIFIER_LOGO)))
        io.AddInputCharactersUTF8(utf8);
    return true;
}

// --- Cards --------------------------------------------------------------------------

void Hud::toast(const std::string& text) {
    if (!m_toasts.empty() && m_toasts.back().text == text && m_toasts.back().age < 1.0) {
        m_toasts.back().age = 0.0;
    } else {
        m_toasts.push_back({text, 0.0});
        if (m_toasts.size() > 3) m_toasts.erase(m_toasts.begin());
    }
    m_server.scheduleFrame();
}

void Hud::openLauncher(int room) {
    m_mode = Mode::Launcher;
    m_targetRoom = room < 0 ? m_server.desk->current : room;
    m_query[0] = 0;
    m_selected = 0;
    m_focusInput = true;
    m_typedEarly = false;
    m_server.refreshPointerFocus();
    m_server.scheduleFrame();
}

void Hud::openRename(int room) {
    m_mode = Mode::Rename;
    clearToasts();   // the card says which room it is
    m_targetRoom = room;
    std::snprintf(m_rename, sizeof(m_rename), "%s",
                  m_server.desk->rooms[static_cast<std::size_t>(room)].name.c_str());
    m_focusInput = true;
    m_typedEarly = false;
    m_server.scheduleFrame();
}

void Hud::toggleHelp() {
    m_mode = m_mode == Mode::Help ? Mode::None : Mode::Help;
    m_server.scheduleFrame();
}

void Hud::askQuit() {
    m_mode = Mode::Quit;
    m_server.scheduleFrame();
}

void Hud::close() {
    m_mode = Mode::None;
    m_accept = false;
    m_server.scheduleFrame();
    m_server.refreshPointerFocus();
}

void Hud::launch(int index) {
    Apps& apps = *m_server.apps;
    if (index < 0 || index >= static_cast<int>(apps.all().size())) return;
    const App& app = apps.all()[static_cast<std::size_t>(index)];
    Desk& desk = *m_server.desk;
    if (m_targetRoom != desk.current) desk.goToRoom(m_targetRoom);
    desk.setLevel(Level::Work);
    auto& recent = desk.rooms[static_cast<std::size_t>(m_targetRoom)].recentApps;
    recent.erase(std::remove(recent.begin(), recent.end(), app.id), recent.end());
    recent.insert(recent.begin(), app.id);
    if (recent.size() > 8) recent.resize(8);
    m_server.state->save();
    m_server.spawn(apps.command(app));
    toast(app.name + " startet …");
    close();
}

// --- Drawing ------------------------------------------------------------------------

void Hud::drawTopBar(int w) {
    Desk& desk = *m_server.desk;
    if (desk.topBar() == 0) return;
    const float h = static_cast<float>(desk.topBar());
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(w), h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.035f, 0.04f, 0.055f, 0.82f));
    ImGui::Begin("##top", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const glm::vec3 acc = desk.rooms[static_cast<std::size_t>(desk.current)].accent;

    // The name, small and quiet; a dot in the room's colour for the playful part.
    ImGui::SetCursorPos(ImVec2(14, (h - 16) * 0.5f));
    dl->AddCircleFilled(ImVec2(ImGui::GetCursorScreenPos().x + 5, h * 0.5f), 5.0f, rgba(acc, 1.0f));
    ImGui::SetCursorPosX(30);
    ImGui::PushFont(m_bold, 15.0f);
    ImGui::TextUnformatted("raum");
    ImGui::PopFont();

    // The rooms, as chips.
    ImGui::SameLine(0, 22);
    for (int i = 0; i < static_cast<int>(desk.rooms.size()); ++i) {
        const Room& r = desk.rooms[static_cast<std::size_t>(i)];
        const bool cur = i == desk.current;
        const int count = desk.windowCount(i);
        char label[192];
        std::snprintf(label, sizeof(label), "%d  %s%s%s##room%d", i + 1, r.name.c_str(),
                      count ? "  · " : "", count ? std::to_string(count).c_str() : "", i);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(11, 4));
        ImGui::PushStyleColor(ImGuiCol_Button, cur ? vec4(r.accent, 0.28f) : ImVec4(1, 1, 1, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, vec4(r.accent, cur ? 0.36f : 0.16f));
        ImGui::PushStyleColor(ImGuiCol_Text, cur ? vec4(kText, 1.0f) : vec4(kDimText, 1.0f));
        ImGui::SetCursorPosY((h - ImGui::GetFrameHeight()) * 0.5f);
        if (ImGui::Button(label)) {
            if (cur && desk.level == Level::Work) desk.setLevel(Level::Room);
            else {
                desk.goToRoom(i);
                desk.setLevel(Level::Work);
            }
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) openRename(i);
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
        ImGui::SameLine(0, 4);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(9, 4));
    ImGui::SetCursorPosY((h - ImGui::GetFrameHeight()) * 0.5f);
    if (ImGui::Button("+##newroom")) {
        const int r = desk.newRoom();
        desk.goToRoom(r);
        openRename(r);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Neuer Raum (Super+N)");
    ImGui::PopStyleVar();

    // Right: the starter, the level, the clock.
    const std::time_t t = std::time(nullptr);
    std::tm lt{};
    localtime_r(&t, &lt);
    char clock[64];
    std::snprintf(clock, sizeof(clock), "%s %d. %s   %02d:%02d", kWeekdays[lt.tm_wday], lt.tm_mday,
                  kMonths[lt.tm_mon], lt.tm_hour, lt.tm_min);
    ImGui::PushFont(m_bold, 15.0f);
    const float cw = ImGui::CalcTextSize(clock).x;
    ImGui::SetCursorPos(ImVec2(w - cw - 18.0f, (h - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::TextUnformatted(clock);
    ImGui::PopFont();

    const char* levels[] = {"Arbeit", "Raum", "Haus"};
    const char* lv = levels[static_cast<int>(desk.level)];
    char hint[128];
    std::snprintf(hint, sizeof(hint), "%s   ·   Super+Leertaste starten   ·   Super+H Hilfe", lv);
    const float hw = ImGui::CalcTextSize(hint).x;
    ImGui::SetCursorPos(ImVec2(w - cw - hw - 54.0f, (h - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::TextDisabled("%s", hint);
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void Hud::drawLabels(int w, int h) {
    Desk& desk = *m_server.desk;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const double wall = desk.wallNow();
    const glm::vec3 eye = desk.cameraPos();

    if (desk.level == Level::Room) {
        // Each window: its title under it, and when it was last used.
        for (View* v : desk.drawList()) {
            if (v->parentView() || v == desk.dragging) continue;
            const wlr_box g = v->geometry();
            const Pose& p = v->pose;
            const glm::vec3 bottom = p.center - glm::vec3(0, g.height * 0.5f * p.scale + 18.0f, 0);
            const glm::vec3 s = desk.toScreen(bottom);
            if (s.z <= 0 || s.x < -200 || s.x > w + 200 || s.y < 0 || s.y > h + 40) continue;
            glm::vec3 c[4];
            desk.canvasQuad(v, p, c);
            const float pw = glm::length(glm::vec2(desk.toScreen(c[1]) - desk.toScreen(c[0])));
            const float size = std::clamp(pw / 26.0f, 12.0f, 19.0f);
            // Hidden behind a nearer window: no label floating over it.
            const Desk::Hit hit = desk.pick(s.x, s.y - 6.0);
            if (hit.view && hit.view != v &&
                glm::length(hit.view->pose.center - eye) < glm::length(p.center - eye))
                continue;
            ImGui::PushFont(m_bold, size);
            const std::string title = fit(v->title().empty() ? v->appId() : v->title(),
                                          std::max(90.0f, pw * 0.95f));
            const ImVec2 ts = ImGui::CalcTextSize(title.c_str());
            const float a = std::min(1.0f, p.alpha) * (1.0f - p.dim * 0.6f);
            dl->AddText(ImGui::GetFont(), size, ImVec2(s.x - ts.x * 0.5f, s.y), rgba(kText, a),
                        title.c_str());
            ImGui::PopFont();
            const std::string when = v->slot >= 0 ? std::string("jetzt") : ago(wall - v->lastFocusWall);
            ImGui::PushFont(m_font, size * 0.85f);
            const ImVec2 ws = ImGui::CalcTextSize(when.c_str());
            const glm::vec3 acc = desk.rooms[static_cast<std::size_t>(v->room)].accent;
            dl->AddText(ImGui::GetFont(), size * 0.85f, ImVec2(s.x - ws.x * 0.5f, s.y + ts.y + 1),
                        rgba(v->slot >= 0 ? acc : kDimText, a), when.c_str());
            ImGui::PopFont();
        }
    } else if (desk.hovered && (desk.hovered->slot < 0 || desk.level == Level::House)) {
        // Otherwise only the window under the pointer says what it is.
        View* v = desk.hovered;
        const wlr_box g = v->geometry();
        const glm::vec3 top = v->pose.center + glm::vec3(0, g.height * 0.5f * v->pose.scale + 10.0f, 0);
        const glm::vec3 s = desk.toScreen(top);
        if (s.z > 0) {
            ImGui::PushFont(m_bold, 15.0f);
            const std::string title = fit(v->title(), 500.0f) + "   ·   " + ago(wall - v->lastFocusWall);
            const ImVec2 ts = ImGui::CalcTextSize(title.c_str());
            const ImVec2 at(std::clamp(s.x - ts.x * 0.5f, 8.0f, w - ts.x - 8.0f),
                            std::max(s.y - ts.y - 8.0f, static_cast<float>(desk.topBar()) + 8.0f));
            dl->AddRectFilled(ImVec2(at.x - 10, at.y - 5), ImVec2(at.x + ts.x + 10, at.y + ts.y + 5),
                              IM_COL32(12, 14, 19, 220), 8.0f);
            dl->AddText(at, rgba(kText, 1.0f), title.c_str());
            ImGui::PopFont();
        }
    }

    if (desk.level == Level::House) {
        // The rooms' names over them, the current one marked.
        for (int i = 0; i < static_cast<int>(desk.rooms.size()); ++i) {
            const Room& r = desk.rooms[static_cast<std::size_t>(i)];
            const glm::vec3 s = desk.toScreen(glm::vec3(desk.roomX(i), desk.floorY(), 260.0f));
            if (s.z <= 0) continue;
            const float size = std::clamp(90000.0f / s.z, 18.0f, 38.0f);
            ImGui::PushFont(m_bold, size);
            const ImVec2 ts = ImGui::CalcTextSize(r.name.c_str());
            dl->AddText(ImGui::GetFont(), size, ImVec2(s.x - ts.x * 0.5f, s.y), rgba(kText, 0.95f),
                        r.name.c_str());
            ImGui::PopFont();
            const int n = desk.windowCount(i);
            const std::string sub = n == 0 ? std::string("leer") :
                                    std::to_string(n) + " Fenster";
            ImGui::PushFont(m_font, size * 0.5f);
            const ImVec2 ss = ImGui::CalcTextSize(sub.c_str());
            dl->AddText(ImGui::GetFont(), size * 0.5f, ImVec2(s.x - ss.x * 0.5f, s.y + ts.y + 2),
                        rgba(kDimText, 0.9f), sub.c_str());
            ImGui::PopFont();
            const float bar = ts.x * (i == desk.current ? 1.0f : 0.35f);
            dl->AddRectFilled(ImVec2(s.x - bar * 0.5f, s.y - 10), ImVec2(s.x + bar * 0.5f, s.y - 6),
                              rgba(r.accent, i == desk.current ? 1.0f : 0.6f), 2.0f);
        }
    }

    if (desk.dragging) {
        // The window being carried to another room, under the pointer.
        View* v = desk.dragging;
        if (v->canvas.valid()) {
            const float tw = 260.0f;
            const float th = tw * v->canvas.height / std::max(1, v->canvas.width);
            const ImVec2 at(static_cast<float>(desk.dragPointer.x) - tw * 0.5f,
                            static_cast<float>(desk.dragPointer.y) - th * 0.5f);
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            fg->AddRectFilled(ImVec2(at.x - 4, at.y - 4), ImVec2(at.x + tw + 4, at.y + th + 4),
                              rgba(desk.rooms[static_cast<std::size_t>(v->room)].accent, 0.6f), 8.0f);
            fg->AddImage(ImTextureRef(static_cast<ImTextureID>(
                             m_server.gpu->device().imguiTextureId(v->canvas.color))),
                         at, ImVec2(at.x + tw, at.y + th));
            const int over = desk.pickRoom(desk.dragPointer.x, desk.dragPointer.y);
            if (over >= 0) {
                const std::string t = "→ " + desk.rooms[static_cast<std::size_t>(over)].name;
                fg->AddText(m_bold, 17.0f, ImVec2(at.x, at.y + th + 8), rgba(kText, 1.0f), t.c_str());
            }
        }
    }
}

void Hud::drawLauncher(int w, int h) {
    Apps& apps = *m_server.apps;
    Desk& desk = *m_server.desk;
    const float pw = std::min(680.0f, w - 40.0f), ph = std::min(560.0f, h - 120.0f);
    ImGui::SetNextWindowPos(ImVec2((w - pw) * 0.5f, std::max(80.0f, h * 0.16f)));
    ImGui::SetNextWindowSize(ImVec2(pw, ph));
    ImGui::Begin("##launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                            ImGuiWindowFlags_NoSavedSettings);
    const Room& room = desk.rooms[static_cast<std::size_t>(m_targetRoom)];
    ImGui::PushFont(m_bold, 14.0f);
    ImGui::TextColored(vec4(room.accent, 1.0f), "STARTEN IN  %s", room.name.c_str());
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::PushFont(m_font, 22.0f);
    ImGui::SetNextItemWidth(-1);
    if (m_focusInput) {
        ImGui::SetKeyboardFocusHere();
        m_focusInput = false;
    }
    if (ImGui::InputTextWithHint("##q", "Programm suchen …", m_query, sizeof(m_query)))
        m_selected = 0;
    ImGui::PopFont();
    ImGui::Spacing();

    // Matches; with nothing typed, what was started in this room comes first.
    m_matches = apps.search(m_query);
    if (m_query[0] == 0) {
        std::vector<int> front;
        for (const std::string& id : room.recentApps)
            for (int i : m_matches)
                if (apps.all()[static_cast<std::size_t>(i)].id == id) front.push_back(i);
        for (int i : m_matches)
            if (std::find(front.begin(), front.end(), i) == front.end()) front.push_back(i);
        m_matches = front;
    }
    m_selected = std::clamp(m_selected, 0, std::max(0, static_cast<int>(m_matches.size()) - 1));
    ImGui::BeginChild("##list", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    const float rowH = 48.0f;
    for (int k = 0; k < static_cast<int>(m_matches.size()) && k < 200; ++k) {
        App& app = const_cast<App&>(apps.all()[static_cast<std::size_t>(m_matches[static_cast<std::size_t>(k)])]);
        ImGui::PushID(k);
        const bool sel = k == m_selected;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##row", sel, ImGuiSelectableFlags_None, ImVec2(0, rowH))) {
            ImGui::PopID();
            launch(m_matches[static_cast<std::size_t>(k)]);
            break;
        }
        if (sel && ImGui::GetScrollMaxY() > 0) {
            const float y = ImGui::GetCursorPosY();
            if (y - rowH < ImGui::GetScrollY()) ImGui::SetScrollY(y - rowH);
            if (y > ImGui::GetScrollY() + ImGui::GetWindowHeight()) ImGui::SetScrollY(y - ImGui::GetWindowHeight());
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (sel) dl->AddRectFilled(at, ImVec2(at.x + 4, at.y + rowH), rgba(room.accent, 1.0f), 2.0f);
        const ImVec2 icon(at.x + 14, at.y + 6);
        if (fitzel::Texture* t = apps.icon(app)) {
            dl->AddImage(ImTextureRef(static_cast<ImTextureID>(
                             m_server.gpu->device().imguiTextureId(t->id()))),
                         icon, ImVec2(icon.x + 36, icon.y + 36));
        } else {
            dl->AddRectFilled(icon, ImVec2(icon.x + 36, icon.y + 36), rgba(room.accent, 0.35f), 9.0f);
            const char letter[2] = {app.name.empty() ? '?' : app.name[0], 0};
            dl->AddText(m_bold, 20.0f, ImVec2(icon.x + 11, icon.y + 6), rgba(kText, 1.0f), letter);
        }
        dl->AddText(m_bold, 17.0f, ImVec2(at.x + 64, at.y + 5), rgba(kText, 1.0f), app.name.c_str());
        const std::string sub = app.generic.empty() ? app.id : app.generic.substr(1);
        dl->AddText(m_font, 14.0f, ImVec2(at.x + 64, at.y + 26), rgba(kDimText, 1.0f), sub.c_str());
        ImGui::PopID();
    }
    if (m_matches.empty()) ImGui::TextDisabled("Nichts gefunden.");
    ImGui::EndChild();
    ImGui::End();
    if (m_accept) {
        m_accept = false;
        if (!m_matches.empty())
            launch(m_matches[static_cast<std::size_t>(
                std::clamp(m_selected, 0, static_cast<int>(m_matches.size()) - 1))]);
    }
}

void Hud::drawRename(int w, int h) {
    const float pw = 420.0f;
    ImGui::SetNextWindowPos(ImVec2((w - pw) * 0.5f, h * 0.3f));
    ImGui::SetNextWindowSize(ImVec2(pw, 0));
    ImGui::Begin("##rename", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_AlwaysAutoResize);
    const Room& r = m_server.desk->rooms[static_cast<std::size_t>(m_targetRoom)];
    ImGui::PushFont(m_bold, 14.0f);
    ImGui::TextColored(vec4(r.accent, 1.0f), "RAUM BENENNEN");
    ImGui::PopFont();
    ImGui::PushFont(m_font, 22.0f);
    ImGui::SetNextItemWidth(pw - 36.0f);
    if (m_focusInput) {
        ImGui::SetKeyboardFocusHere();
        m_focusInput = false;
    }
    ImGui::InputText("##name", m_rename, sizeof(m_rename), ImGuiInputTextFlags_AutoSelectAll);
    ImGui::PopFont();
    ImGui::TextDisabled("Enter übernimmt, Esc bricht ab");
    ImGui::End();
    if (m_accept) {
        m_accept = false;
        m_server.desk->renameRoom(m_targetRoom, m_rename);
        close();
    }
}

void Hud::drawHelp(int w, int h) {
    const float pw = std::min(760.0f, w - 40.0f);
    ImGui::SetNextWindowPos(ImVec2((w - pw) * 0.5f, std::max(60.0f, h * 0.1f)));
    ImGui::SetNextWindowSize(ImVec2(pw, 0));
    ImGui::Begin("##help", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushFont(m_bold, 22.0f);
    ImGui::TextUnformatted("raum");
    ImGui::PopFont();
    ImGui::TextDisabled("Vorn wird gearbeitet. Dahinter wartet, was du zuletzt benutzt hast,\n"
                        "und zwar umso weiter hinten, je länger es her ist. Jedes Projekt bekommt seinen Raum.");
    ImGui::Spacing();
    const char* rows[][2] = {
        {"Super + Leertaste", "Programm starten (in diesem Raum)"},
        {"Super + Enter", "Terminal"},
        {"Super + Tab  /  Super + Umschalt + Tab", "das Fenster dahinter nach vorn / das älteste"},
        {"Klick auf ein Fenster dahinter", "holt es nach vorn"},
        {"Super (kurz)  /  Super + ↑ ↓  /  Super + Mausrad", "zurücklehnen: Arbeit → Raum → Haus"},
        {"Super + ← →  /  Super + 1 … 9", "Raum wechseln"},
        {"Super + Umschalt + ← →  /  1 … 9", "Fenster in einen anderen Raum mitnehmen"},
        {"Super + N  /  Super + R", "neuer Raum  /  Raum umbenennen"},
        {"Super + P", "zwei / drei / ein Fenster vorn nebeneinander"},
        {"Super + F  /  Super + Umschalt + F", "Fenster maximieren / Vollbild"},
        {"Super + Ziehen", "Fenster verschieben (rechte Taste: Größe)"},
        {"Haus-Ansicht: Fenster ziehen", "auf einen anderen Raum legen"},
        {"Super + Q", "Fenster schließen"},
        {"Super + S", "Bildschirmfoto nach ~/Bilder"},
        {"Super + F11", "raum im Vollbild / im Fenster (verschachtelt)"},
        {"Super + Umschalt + E", "raum beenden"},
    };
    if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_SizingStretchProp)) {
        for (const auto& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushFont(m_bold, 15.0f);
            ImGui::TextColored(vec4(m_server.desk->rooms[static_cast<std::size_t>(m_server.desk->current)].accent, 1.0f),
                               "%s", r[0]);
            ImGui::PopFont();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r[1]);
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Eine beliebige Taste schließt diese Karte.");
    ImGui::End();
}

void Hud::drawQuit(int w, int h) {
    const float pw = 420.0f;
    ImGui::SetNextWindowPos(ImVec2((w - pw) * 0.5f, h * 0.32f));
    ImGui::SetNextWindowSize(ImVec2(pw, 0));
    ImGui::Begin("##quit", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushFont(m_bold, 20.0f);
    ImGui::TextUnformatted("raum beenden?");
    ImGui::PopFont();
    ImGui::TextDisabled("Alle Programme darin werden geschlossen.");
    ImGui::Spacing();
    if (ImGui::Button("Beenden (Enter)")) m_server.quit();
    ImGui::SameLine();
    if (ImGui::Button("Bleiben (Esc)")) close();
    ImGui::End();
}

void Hud::drawEmptyRoom(int w, int h) {
    // An empty room greets with what used to be in it: one click sets it up again.
    Desk& desk = *m_server.desk;
    if (desk.level != Level::Work || desk.windowCount(desk.current) > 0) return;
    Apps& apps = *m_server.apps;
    const Room& room = desk.rooms[static_cast<std::size_t>(desk.current)];
    std::vector<App*> recent;
    for (const std::string& id : room.recentApps)
        if (App* a = apps.find(id)) recent.push_back(a);
    const float pw = 560.0f;
    ImGui::SetNextWindowPos(ImVec2((w - pw) * 0.5f, h * 0.30f));
    ImGui::SetNextWindowSize(ImVec2(pw, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, vec4(kPanel, 0.0f));
    ImGui::Begin("##empty", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                         ImGuiWindowFlags_AlwaysAutoResize |
                                         ImGuiWindowFlags_NoBringToFrontOnFocus);
    auto centered = [&](ImFont* f, float size, const glm::vec3& col, float a, const char* text) {
        ImGui::PushFont(f, size);
        const float tw = ImGui::CalcTextSize(text).x;
        ImGui::SetCursorPosX((pw - tw) * 0.5f);
        ImGui::TextColored(vec4(col, a), "%s", text);
        ImGui::PopFont();
    };
    centered(m_bold, 34.0f, kText, 0.92f, room.name.c_str());
    centered(m_font, 16.0f, kDimText, 1.0f,
             recent.empty() ? "Ein leerer Raum. Super+Leertaste holt ein Programm herein."
                            : "Hier war zuletzt:");
    if (!recent.empty()) {
        ImGui::Spacing();
        const int n = std::min<int>(6, static_cast<int>(recent.size()));
        const float tile = 84.0f, gap = 10.0f;
        ImGui::SetCursorPosX((pw - (n * tile + (n - 1) * gap)) * 0.5f);
        for (int i = 0; i < n; ++i) {
            App* a = recent[static_cast<std::size_t>(i)];
            ImGui::PushID(i);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##app", ImVec2(tile, tile + 22.0f))) {
                m_server.spawn(apps.command(*a));
                toast(a->name + " startet …");
            }
            const bool hov = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(at, ImVec2(at.x + tile, at.y + tile + 22.0f),
                              hov ? rgba(room.accent, 0.22f) : IM_COL32(255, 255, 255, 10), 14.0f);
            const ImVec2 ic(at.x + (tile - 44.0f) * 0.5f, at.y + 12.0f);
            if (fitzel::Texture* t = apps.icon(*a))
                dl->AddImage(ImTextureRef(static_cast<ImTextureID>(
                                 m_server.gpu->device().imguiTextureId(t->id()))),
                             ic, ImVec2(ic.x + 44, ic.y + 44));
            else
                dl->AddRectFilled(ic, ImVec2(ic.x + 44, ic.y + 44), rgba(room.accent, 0.4f), 10.0f);
            ImGui::PushFont(m_font, 13.0f);
            const std::string name = fit(a->name, tile - 8.0f);
            const float nw = ImGui::CalcTextSize(name.c_str()).x;
            dl->AddText(ImVec2(at.x + (tile - nw) * 0.5f, at.y + 64.0f), rgba(kText, 0.9f), name.c_str());
            ImGui::PopFont();
            ImGui::PopID();
            if (i + 1 < n) ImGui::SameLine(0, gap);
        }
        ImGui::Spacing();
        centered(m_font, 14.0f, kDimText, 0.8f, "Ein Klick startet es wieder hier.  Super+Leertaste: etwas anderes.");
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

void Hud::drawToasts(int w, int h) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    float y = h - 70.0f;
    for (auto it = m_toasts.rbegin(); it != m_toasts.rend(); ++it) {
        const float a = static_cast<float>(std::clamp(2.2 - it->age, 0.0, 0.3) / 0.3) *
                        static_cast<float>(std::min(1.0, it->age / 0.12));
        ImGui::PushFont(m_bold, 17.0f);
        const ImVec2 ts = ImGui::CalcTextSize(it->text.c_str());
        const ImVec2 at((w - ts.x) * 0.5f, y);
        dl->AddRectFilled(ImVec2(at.x - 18, at.y - 9), ImVec2(at.x + ts.x + 18, at.y + ts.y + 9),
                          IM_COL32(14, 16, 22, static_cast<int>(225 * a)), 20.0f);
        const glm::vec3 acc = m_server.desk->rooms[static_cast<std::size_t>(m_server.desk->current)].accent;
        dl->AddCircleFilled(ImVec2(at.x - 6, at.y + ts.y * 0.5f), 3.5f, rgba(acc, a));
        dl->AddText(ImVec2(at.x + 4, at.y), rgba(kText, a), it->text.c_str());
        ImGui::PopFont();
        y -= ts.y + 26.0f;
    }
}

void Hud::render(int width, int height, double dt) {
    ImGui::SetCurrentContext(m_ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    io.DeltaTime = static_cast<float>(std::clamp(dt, 1e-4, 0.25));
    m_time += dt;
    for (Toast& t : m_toasts) t.age += dt;
    m_toasts.erase(std::remove_if(m_toasts.begin(), m_toasts.end(),
                                  [](const Toast& t) { return t.age > 2.2; }),
                   m_toasts.end());

    m_server.gpu->device().imguiNewFrame();
    ImGui::NewFrame();
    drawLabels(width, height);
    if (m_mode == Mode::None) drawEmptyRoom(width, height);
    drawTopBar(width);
    switch (m_mode) {
        case Mode::Launcher: drawLauncher(width, height); break;
        case Mode::Rename: drawRename(width, height); break;
        case Mode::Help: drawHelp(width, height); break;
        case Mode::Quit: drawQuit(width, height); break;
        case Mode::None: break;
    }
    drawToasts(width, height);
    ImGui::Render();
    m_server.gpu->device().imguiRender(ImGui::GetDrawData());
}

} // namespace raum
