#include "Apps.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <unistd.h>

namespace raum {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    // The German capitals that matter in program names.
    const std::pair<const char*, const char*> umlauts[] = {
        {"\xC3\x84", "\xC3\xA4"}, {"\xC3\x96", "\xC3\xB6"}, {"\xC3\x9C", "\xC3\xBC"}};
    for (const auto& [up, lo] : umlauts)
        for (std::size_t p = 0; (p = s.find(up, p)) != std::string::npos; p += 2) s.replace(p, 2, lo);
    return s;
}

std::vector<std::string> dataDirs() {
    std::vector<std::string> out;
    const char* home = std::getenv("HOME");
    const char* dh = std::getenv("XDG_DATA_HOME");
    if (dh && *dh) out.push_back(dh);
    else if (home) out.push_back(std::string(home) + "/.local/share");
    const char* dd = std::getenv("XDG_DATA_DIRS");
    std::string dirs = (dd && *dd) ? dd : "/usr/local/share:/usr/share";
    std::stringstream ss(dirs);
    for (std::string d; std::getline(ss, d, ':');)
        if (!d.empty()) out.push_back(d);
    // Snaps and Flatpaks put their entries here; not every session lists them.
    out.push_back("/var/lib/snapd/desktop");
    out.push_back("/var/lib/flatpak/exports/share");
    if (home) out.push_back(std::string(home) + "/.local/share/flatpak/exports/share");
    return out;
}

std::string stripFieldCodes(const std::string& exec) {
    std::string out;
    for (std::size_t i = 0; i < exec.size(); ++i) {
        if (exec[i] == '%' && i + 1 < exec.size()) {
            if (exec[i + 1] == '%') out += '%';
            ++i;
            continue;
        }
        out += exec[i];
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

bool executable(const std::string& name) {
    if (name.find('/') != std::string::npos) return access(name.c_str(), X_OK) == 0;
    const char* path = std::getenv("PATH");
    std::stringstream ss(path ? path : "/usr/bin:/bin");
    for (std::string d; std::getline(ss, d, ':');)
        if (access((d + "/" + name).c_str(), X_OK) == 0) return true;
    return false;
}

} // namespace

Apps::Apps() {
    for (const std::string& d : dataDirs()) scan(d + "/applications");
    std::sort(m_apps.begin(), m_apps.end(),
              [](const App& a, const App& b) { return lower(a.name) < lower(b.name); });
    indexIcons();
}

void Apps::scan(const std::string& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().extension() != ".desktop") continue;
        const std::string id = e.path().stem().string();
        if (std::any_of(m_apps.begin(), m_apps.end(), [&](const App& a) { return a.id == id; }))
            continue;
        std::ifstream in(e.path());
        std::string line;
        bool inEntry = false, hidden = false, isApp = false, onlyShowIn = false;
        App app;
        app.id = id;
        std::string nameDe, nameDeDe, tryExec;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            if (line[0] == '[') {
                inEntry = line == "[Desktop Entry]";
                continue;
            }
            if (!inEntry) continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k == "Type") isApp = v == "Application";
            else if (k == "Name") app.name = v;
            else if (k == "Name[de]") nameDe = v;
            else if (k == "Name[de_DE]") nameDeDe = v;
            else if (k == "GenericName" || k == "GenericName[de]") app.generic += " " + v;
            else if (k == "Keywords" || k == "Keywords[de]") app.keywords += " " + v;
            else if (k == "Exec") app.exec = stripFieldCodes(v);
            else if (k == "TryExec") tryExec = v;
            else if (k == "Icon") app.icon = v;
            else if (k == "Terminal") app.terminal = v == "true";
            else if ((k == "NoDisplay" || k == "Hidden") && v == "true") hidden = true;
            else if (k == "OnlyShowIn") onlyShowIn = true;
        }
        if (!isApp || hidden || onlyShowIn || app.exec.empty() || app.name.empty()) continue;
        if (!tryExec.empty() && !executable(tryExec)) continue;
        if (!nameDeDe.empty()) app.name = nameDeDe;
        else if (!nameDe.empty()) app.name = nameDe;
        m_apps.push_back(std::move(app));
    }
}

void Apps::indexIcons() {
    // name -> (score, path): bigger is better, a themed icon before a pixmap.
    std::unordered_map<std::string, std::pair<int, std::string>> best;
    auto consider = [&](const fs::path& p, int score) {
        if (p.extension() != ".png") return;
        auto& slot = best[p.stem().string()];
        if (score > slot.first) slot = {score, p.string()};
    };
    std::vector<std::string> roots;
    for (const std::string& d : dataDirs()) roots.push_back(d + "/icons");
    if (const char* home = std::getenv("HOME")) roots.push_back(std::string(home) + "/.icons");
    const char* themes[] = {"hicolor", "Adwaita", "Yaru"};   // later wins on a tie
    std::error_code ec;
    for (int t = 0; t < 3; ++t)
        for (const std::string& root : roots) {
            const fs::path base = fs::path(root) / themes[t];
            if (!fs::is_directory(base, ec)) continue;
            for (const auto& size : fs::directory_iterator(base, ec)) {
                if (!size.is_directory()) continue;
                const std::string sz = size.path().filename().string();
                int px = std::atoi(sz.c_str());
                if (px <= 0) continue;
                if (sz.find("@2x") != std::string::npos) px *= 2;
                // 128 is what the starter shows; bigger is fine, smaller less so.
                const int score = (px >= 96 ? 1000 - std::abs(px - 128) : px * 4) * 10 + t;
                for (const auto& cat : fs::directory_iterator(size.path(), ec)) {
                    if (!cat.is_directory()) continue;
                    for (const auto& f : fs::directory_iterator(cat.path(), ec)) consider(f.path(), score);
                }
            }
        }
    for (const auto& f : fs::directory_iterator("/usr/share/pixmaps", ec)) consider(f.path(), 1);
    for (auto& [name, v] : best) m_icons[name] = v.second;
}

std::string Apps::iconPath(const std::string& name) const {
    if (name.empty()) return {};
    if (name[0] == '/') {
        std::error_code ec;
        if (fs::path(name).extension() == ".png" && fs::exists(name, ec)) return name;
        const auto it = m_icons.find(fs::path(name).stem().string());
        return it == m_icons.end() ? std::string() : it->second;
    }
    const auto it = m_icons.find(name);
    return it == m_icons.end() ? std::string() : it->second;
}

App* Apps::find(const std::string& id) {
    for (App& a : m_apps)
        if (a.id == id) return &a;
    return nullptr;
}

std::vector<int> Apps::search(const std::string& query) const {
    // German words for what most installs only name in English.
    static const std::pair<const char*, const char*> kAliases[] = {
        {"rechner", "calculator"}, {"taschenrechner", "calculator"}, {"uhr", "clock"},
        {"wecker", "clock"}, {"dateien", "files"}, {"dateimanager", "files"},
        {"einstellungen", "settings"}, {"textedit", "text editor"}, {"editor", "editor"},
        {"browser", "web"}, {"bilder", "image"}, {"musik", "music"}, {"kalender", "calendar"},
        {"karten", "maps"}, {"wetter", "weather"}, {"systemmonitor", "system monitor"},
        {"datentr", "disks"}, {"schrift", "font"}, {"zeichen", "characters"},
    };
    std::string q = lower(query);
    if (q.size() >= 3)
        for (const auto& [de, en] : kAliases)
            if (std::string(de).rfind(q, 0) == 0) {
                q = en;
                break;
            }
    std::vector<std::pair<int, int>> scored;
    for (int i = 0; i < static_cast<int>(m_apps.size()); ++i) {
        const App& a = m_apps[static_cast<std::size_t>(i)];
        if (q.empty()) {
            scored.push_back({0, i});
            continue;
        }
        const std::string name = lower(a.name);
        int score = -1;
        if (name.rfind(q, 0) == 0) score = 300;
        else if (name.find(" " + q) != std::string::npos) score = 250;
        else if (name.find(q) != std::string::npos) score = 200;
        else if (lower(a.id).find(q) != std::string::npos) score = 150;
        else if (lower(a.generic).find(q) != std::string::npos) score = 120;
        else if (lower(a.keywords).find(q) != std::string::npos) score = 100;
        else if (lower(a.exec).find(q) != std::string::npos) score = 60;
        if (score >= 0) scored.push_back({score, i});
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<int> out;
    for (const auto& s : scored) out.push_back(s.second);
    return out;
}

fitzel::Texture* Apps::icon(App& app) {
    if (!app.textureTried) {
        app.textureTried = true;
        const std::string path = iconPath(app.icon);
        if (!path.empty()) {
            auto t = std::make_shared<fitzel::Texture>(fitzel::Texture::fromFile(path, false));
            if (t->isValid()) app.texture = t;
        }
    }
    return app.texture.get();
}

std::string Apps::terminal() const {
    for (const char* t : {"foot", "ptyxis", "kgx", "gnome-terminal", "alacritty", "kitty", "xterm"})
        if (executable(t)) return t;
    return "xterm";
}

std::string Apps::command(const App& app) const {
    if (!app.terminal) return app.exec;
    const std::string t = terminal();
    if (t == "foot" || t == "alacritty" || t == "kitty") return t + " " + app.exec;
    if (t == "ptyxis" || t == "kgx" || t == "gnome-terminal") return t + " -- " + app.exec;
    return t + " -e " + app.exec;
}

} // namespace raum
