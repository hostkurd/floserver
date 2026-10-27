// floserverd: the background service in the user's session (D2). It will own
// runtimes, processes, web server config, certificates and idle timers. This
// first version only answers --version so the build and packaging can start.

#include "floserver/core/build_info.h"

#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--version") {
        std::cout << "floserverd " << floserver::core::kVersion << "\n";
        return 0;
    }
    std::cerr << "floserverd " << floserver::core::kVersion << ": the service isn't built yet.\n";
    return 1;
}
