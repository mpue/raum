// raum -- a 3D desktop. A Wayland compositor on wlroots whose windows are
// drawn into a room by the fitzel engine.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "Server.hpp"

namespace {

void usage() {
    std::printf(
        "raum %s -- ein 3D-Desktop (Wayland-Compositor auf wlroots, gezeichnet von fitzel)\n\n"
        "  raum [Optionen]\n\n"
        "  -s, --start BEFEHL   nach dem Start ausführen (mehrfach möglich)\n"
        "  -f, --fullscreen     verschachtelt: im Vollbild starten (Super+F11 schaltet um)\n"
        "  --size BxH           Fenstergröße, wenn raum verschachtelt läuft (1920x1080)\n"
        "  --script DATEI       zeitgesteuerte Aktionen ausführen (Tests, Vorführungen)\n"
        "  --no-xwayland        keine X11-Programme\n"
        "  --shared-bus         verschachtelt den D-Bus des Host-Desktops mitbenutzen\n"
        "  -h, --help           diese Hilfe\n\n"
        "Unter GNOME gestartet läuft raum in einem Fenster; von GDM als Sitzung\n"
        "gestartet übernimmt es den Bildschirm. Super+H zeigt die Tasten.\n",
        RAUM_VERSION);
}

} // namespace

int main(int argc, char** argv) {
    raum::Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "raum: %s braucht einen Wert\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (a == "-s" || a == "--start") {
            opts.autostart.push_back(next());
        } else if (a == "--size") {
            const std::string v = next();
            if (std::sscanf(v.c_str(), "%dx%d", &opts.width, &opts.height) != 2) {
                std::fprintf(stderr, "raum: --size BxH, etwa 1920x1080\n");
                return 2;
            }
        } else if (a == "-f" || a == "--fullscreen") {
            opts.fullscreen = true;
        } else if (a == "--script") {
            opts.script = next();
        } else if (a == "--no-xwayland") {
            opts.xwayland = false;
        } else if (a == "--shared-bus") {
            opts.privateBus = false;
        } else {
            std::fprintf(stderr, "raum: unbekannte Option %s (--help)\n", a.c_str());
            return 2;
        }
    }
    raum::Server server(opts);
    if (!server.start()) return 1;
    server.run();
    return 0;
}
