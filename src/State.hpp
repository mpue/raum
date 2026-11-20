#pragma once

#include <string>

namespace raum {

class Server;

// What survives a restart: the rooms (names, colours, how many windows stand
// side by side, what was started in each) and which one was open. Windows
// themselves do not survive -- their programs end with the session -- but a
// room remembers what it held, and the starter offers that first.
// ~/.config/raum/state.json.
class State {
public:
    explicit State(Server& server);
    void load();
    void save();
    const std::string& path() const { return m_path; }
    bool soundOn = true;   // raum's own sounds (Super+M)

private:
    Server& m_server;
    std::string m_path;
    bool m_loading = false;
};

} // namespace raum
