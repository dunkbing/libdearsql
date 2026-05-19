// One-shot helper to install Oracle Instant Client locally. Not registered
// as a gtest; invoked manually:
//   ./build/dearsql_oracle_install
// Prints progress, returns 0 on success.

#include "dearsql/oracle_installer.hpp"
#include <iostream>

int main() {
    using namespace dearsql::oracle;
    if (isInstalled()) {
        std::cout << "Already installed at: " << installDir() << "\n";
        return 0;
    }
    std::cout << "Installing from " << downloadUrl() << "\n";
    auto [ok, err] = install([](const Progress& p) {
        if (p.phase == "downloading" && p.bytesTotal > 0) {
            const double pct = 100.0 * static_cast<double>(p.bytesDownloaded) /
                               static_cast<double>(p.bytesTotal);
            std::cout << "\r  download " << static_cast<int>(pct) << "% (" << p.bytesDownloaded
                      << "/" << p.bytesTotal << ")    " << std::flush;
        } else {
            std::cout << "\n  " << p.phase << " " << p.message;
        }
    });
    std::cout << "\n";
    if (!ok) {
        std::cerr << "install failed: " << err << "\n";
        return 1;
    }
    std::cout << "installed at: " << installDir() << "\n";
    return 0;
}
