/*
    Headless frontend for 3Beans (not part of upstream): runs the core without a window or
    audio, driven through the debug interface (see src/core/debug.h).

    Usage: 3beans-headless [--dir SETTINGSDIR] [--port N] [--frames N] [--no-limit] [--log N]
                           [--firm FILE] [--sd FILE] [--system-type 0|1|2]
    The settings folder is the same as the GUI's (3beans.ini); options override it.
*/

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../core/core.h"

static volatile sig_atomic_t stopSignal = 0;

int main(int argc, char **argv) {
    std::string dir = ".";
    int port = 4900;
    long maxFrames = -1;
    std::string firm, sd;
    int noLimit = 0, systemType = -1, logLevel = 1;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        bool more = (i + 1 < argc);
        if (a == "--dir" && more) dir = argv[++i];
        else if (a == "--port" && more) port = atoi(argv[++i]);
        else if (a == "--frames" && more) maxFrames = atol(argv[++i]);
        else if (a == "--firm" && more) firm = argv[++i];
        else if (a == "--sd" && more) sd = argv[++i];
        else if (a == "--system-type" && more) systemType = atoi(argv[++i]);
        else if (a == "--log" && more) logLevel = atoi(argv[++i]);
        else if (a == "--no-limit") noLimit = 1;
        else {
            fprintf(stderr, "usage: %s [--dir DIR] [--port N] [--frames N] [--log N] [--no-limit] [--firm F] [--sd F] [--system-type N]\n", argv[0]);
            return 2;
        }
    }

    Settings::load(dir);
    if (!firm.empty()) Settings::firmPath = firm;
    if (!sd.empty()) Settings::sdPath = sd;
    if (systemType >= 0) Settings::systemType = systemType;
    if (noLimit) Settings::fpsLimiter = 0;
    Settings::gpuRenderer = 0; // Software renderer; there is no GL context
    Settings::debugPort = port;
    signal(SIGINT, [](int) { stopSignal = 1; });
    signal(SIGTERM, [](int) { stopSignal = 1; });

    std::string cartPath;
    Core *core;
    try {
        core = new Core(cartPath, nullptr);
    }
    catch (CoreError) {
        fprintf(stderr, "boot ROMs missing; check boot9Path/boot11Path in %s/3beans.ini\n", dir.c_str());
        return 1;
    }

    bool ok;
    core->debug.command("log level " + std::to_string(logLevel), ok);
    for (long frames = 0; !stopSignal && !core->debug.quitRequested && (maxFrames < 0 || frames < maxFrames); frames++) {
        core->runFrame();
        while (uint32_t *frame = core->pdc.getFrame())
            delete[] frame;
    }
    delete core;
    return 0;
}
