#include "State.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "Desk.hpp"
#include "Server.hpp"

namespace raum {

namespace fs = std::filesystem;
using nlohmann::json;

State::State(Server& server) : m_server(server) {
    std::string dir;
    if (const char* x = std::getenv("RAUM_CONFIG_DIR"); x && *x) dir = x;
    else if (const char* c = std::getenv("XDG_CONFIG_HOME"); c && *c) dir = std::string(c) + "/raum";
    else if (const char* h = std::getenv("HOME")) dir = std::string(h) + "/.config/raum";
    else dir = "/tmp/raum";
    m_path = dir + "/state.json";
}

void State::load() {
    std::ifstream in(m_path);
    if (!in) return;
    json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[raum] %s unreadable (%s), starting fresh\n", m_path.c_str(), e.what());
        return;
    }
    m_loading = true;
    Desk& desk = *m_server.desk;
    if (j.contains("rooms") && j["rooms"].is_array() && !j["rooms"].empty()) {
        desk.rooms.clear();
        for (const json& r : j["rooms"]) {
            Room room;
            room.name = r.value("name", std::string("Raum"));
            if (r.contains("accent") && r["accent"].is_array() && r["accent"].size() == 3)
                room.accent = glm::vec3(r["accent"][0].get<float>(), r["accent"][1].get<float>(),
                                        r["accent"][2].get<float>());
            room.split = std::clamp(r.value("split", 1), 1, 3);
            if (r.contains("recent") && r["recent"].is_array())
                for (const json& a : r["recent"])
                    if (a.is_string()) room.recentApps.push_back(a.get<std::string>());
            desk.rooms.push_back(room);
        }
    }
    desk.current = std::clamp(j.value("current", 0), 0, static_cast<int>(desk.rooms.size()) - 1);
    soundOn = j.value("sound", true);
    m_loading = false;
}

void State::save() {
    if (m_loading) return;
    const Desk& desk = *m_server.desk;
    json j;
    j["current"] = desk.current;
    j["sound"] = soundOn;
    j["rooms"] = json::array();
    for (const Room& r : desk.rooms)
        j["rooms"].push_back({{"name", r.name},
                              {"accent", {r.accent.r, r.accent.g, r.accent.b}},
                              {"split", r.split},
                              {"recent", r.recentApps}});
    std::error_code ec;
    fs::create_directories(fs::path(m_path).parent_path(), ec);
    const std::string tmp = m_path + ".tmp";
    {
        std::ofstream out(tmp);
        if (!out) return;
        out << j.dump(2) << "\n";
    }
    fs::rename(tmp, m_path, ec);
}

} // namespace raum
