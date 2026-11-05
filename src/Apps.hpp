#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <fitzel/graphics/Texture.hpp>

namespace raum {

// A program the starter can start, read from its .desktop file.
struct App {
    std::string id;          // the desktop file's name, without .desktop
    std::string name;        // Name[de] where there is one
    std::string generic;     // GenericName, for searching
    std::string keywords;    // Keywords, for searching
    std::string exec;        // the command line, field codes removed
    std::string icon;        // Icon=
    bool terminal = false;
    std::shared_ptr<fitzel::Texture> texture;   // loaded on first use
    bool textureTried = false;
};

// The installed programs: every .desktop file under the XDG data directories
// that is meant to be shown, with icons looked up in the icon themes.
class Apps {
public:
    Apps();

    const std::vector<App>& all() const { return m_apps; }
    App* find(const std::string& id);
    // Indices into all(), best first: names that start with the query before
    // names that contain it before the rest of the fields.
    std::vector<int> search(const std::string& query) const;
    // The icon (a texture on the current device), or null when there is none.
    fitzel::Texture* icon(App& app);
    // The command to start it (in a terminal when it wants one).
    std::string command(const App& app) const;
    std::string terminal() const;

private:
    void scan(const std::string& dir);
    void indexIcons();
    std::string iconPath(const std::string& name) const;

    std::vector<App> m_apps;
    std::unordered_map<std::string, std::string> m_icons;   // name -> best PNG
};

} // namespace raum
