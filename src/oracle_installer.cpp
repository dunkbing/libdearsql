#include "dearsql/oracle_installer.hpp"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <system_error>

namespace dearsql::oracle {

namespace fs = std::filesystem;

namespace {

constexpr const char* kDownloadHost = "https://download.oracle.com";

#if defined(__linux__) && defined(__x86_64__)
constexpr const char* kDownloadPath = "/otn_software/linux/instantclient/2326100/"
                                      "instantclient-basiclite-linux.x64-23.26.1.0.0.zip";
constexpr const char* kArchiveName = "instantclient-basiclite-linux.x64-23.26.1.0.0.zip";
constexpr const char* kExpectedDirName = "instantclient_23_26";
#elif defined(__linux__) && defined(__aarch64__)
constexpr const char* kDownloadPath = "/otn_software/linux/instantclient/2326100/"
                                      "instantclient-basiclite-linux.arm64-23.26.1.0.0.zip";
constexpr const char* kArchiveName = "instantclient-basiclite-linux.arm64-23.26.1.0.0.zip";
constexpr const char* kExpectedDirName = "instantclient_23_26";
#elif defined(__APPLE__) && defined(__aarch64__)
constexpr const char* kDownloadPath =
    "/otn_software/mac/instantclient/233023/instantclient-basiclite-macos.arm64-23.3.0.23.09-2.dmg";
constexpr const char* kArchiveName = "instantclient-basiclite-macos.arm64-23.3.0.23.09-2.dmg";
constexpr const char* kExpectedDirName = "instantclient_23_3";
#elif defined(__APPLE__) && defined(__x86_64__)
constexpr const char* kDownloadPath =
    "/otn_software/mac/instantclient/1916000/"
    "instantclient-basiclite-macos.x64-19.16.0.0.0dbru.dmg";
constexpr const char* kArchiveName = "instantclient-basiclite-macos.x64-19.16.0.0.0dbru.dmg";
constexpr const char* kExpectedDirName = "instantclient_19_16";
#elif defined(_WIN32)
constexpr const char* kDownloadPath = "/otn_software/nt/instantclient/2326100/"
                                      "instantclient-basiclite-windows.x64-23.26.1.0.0.zip";
constexpr const char* kArchiveName = "instantclient-basiclite-windows.x64-23.26.1.0.0.zip";
constexpr const char* kExpectedDirName = "instantclient_23_26";
#else
constexpr const char* kDownloadPath = "";
constexpr const char* kArchiveName = "";
constexpr const char* kExpectedDirName = "";
#endif

std::mutex& cacheMutex() {
    static std::mutex m;
    return m;
}
std::string& cachedDir() {
    static std::string s;
    return s;
}

fs::path baseDir() {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    if (home && *home)
        return fs::path(home) / ".dearsql" / "oracle-client";
    return fs::path(".") / "oracle-client";
}

#if defined(__linux__)
bool hasCorrectLibaioSoname(const fs::path& file) {
    std::string cmd =
        "objdump -p '" + file.string() + "' 2>/dev/null | grep -q 'SONAME.*libaio\\.so\\.1$'";
    return std::system(cmd.c_str()) == 0;
}

bool downloadLibaio(const fs::path& installDir) {
    fs::path target = installDir / "libaio.so.1";
#if defined(__x86_64__)
    const char* arch = "amd64";
    const char* libPath = "lib/x86_64-linux-gnu";
#elif defined(__aarch64__)
    const char* arch = "arm64";
    const char* libPath = "lib/aarch64-linux-gnu";
#else
    return false;
#endif

    // Use Ubuntu 22.04 (jammy) — has SONAME "libaio.so.1" which Oracle's
    // libclntsh.so requires (24.04+ renamed it to libaio.so.1t64).
    std::string debName = std::format("libaio1_0.3.112-13build1_{}.deb", arch);
    std::string debPath = std::format("/ubuntu/pool/main/liba/libaio/{}", debName);

    httplib::Client cli("https://archive.ubuntu.com");
    cli.set_connection_timeout(10);
    cli.set_read_timeout(30);
    cli.set_follow_location(true);

    fs::path debFile = installDir / debName;
    std::ofstream out(debFile, std::ios::binary);
    if (!out.is_open())
        return false;

    bool writeFailed = false;
    auto res = cli.Get(debPath, [&](const char* data, size_t len) -> bool {
        out.write(data, static_cast<std::streamsize>(len));
        if (!out) {
            writeFailed = true;
            return false;
        }
        return true;
    });
    out.close();

    if (!res || res->status != 200 || writeFailed) {
        std::error_code ec;
        fs::remove(debFile, ec);
        return false;
    }

    std::string destDir = installDir.string();
    std::string extractCmd =
        "cd '" + destDir +
        "' && ("
        "ar p '" + debFile.string() +
        "' data.tar.zst 2>/dev/null | tar --zstd -xf - --wildcards '*/libaio.so*' 2>/dev/null || "
        "ar p '" + debFile.string() +
        "' data.tar.xz 2>/dev/null | tar -xJf - --wildcards '*/libaio.so*' 2>/dev/null || "
        "ar p '" + debFile.string() +
        "' data.tar.gz 2>/dev/null | tar -xzf - --wildcards '*/libaio.so*' 2>/dev/null"
        ")";

    int ret = std::system(extractCmd.c_str());
    std::error_code ec;
    fs::remove(debFile, ec);
    if (ret != 0)
        return false;

    fs::path searchDir = installDir / "usr" / libPath;
    if (fs::exists(searchDir)) {
        for (const auto& entry : fs::directory_iterator(searchDir, ec)) {
            if (entry.path().filename().string().starts_with("libaio.so.1")) {
                fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing, ec);
                break;
            }
        }
    }
    fs::remove_all(installDir / "usr", ec);
    return fs::exists(target);
}

void ensureLibaio(const fs::path& installDir) {
    fs::path target = installDir / "libaio.so.1";
    if (fs::exists(target)) {
        if (hasCorrectLibaioSoname(target))
            return; // already correct
        std::error_code ec;
        fs::remove(target, ec);
    }
    for (const auto* candidate : {
             "/usr/lib/x86_64-linux-gnu/libaio.so.1",
             "/usr/lib/aarch64-linux-gnu/libaio.so.1",
             "/usr/lib64/libaio.so.1",
             "/usr/lib/libaio.so.1",
             "/lib/x86_64-linux-gnu/libaio.so.1",
             "/lib64/libaio.so.1",
         }) {
        if (!fs::exists(candidate))
            continue;
        std::error_code ec;
        auto resolved = fs::canonical(candidate, ec);
        if (ec || !hasCorrectLibaioSoname(resolved))
            continue;
        fs::copy_file(candidate, target, ec);
        if (!ec)
            return;
    }
    downloadLibaio(installDir);
}
#endif // __linux__

} // namespace

std::string downloadUrl() {
    if (std::strlen(kDownloadPath) == 0)
        return {};
    return std::string(kDownloadHost) + kDownloadPath;
}

std::string installDir() {
    std::lock_guard lock(cacheMutex());
    if (!cachedDir().empty())
        return cachedDir();

    fs::path base = baseDir();
    fs::path expected = base / kExpectedDirName;
    if (fs::exists(expected)) {
        cachedDir() = expected.string();
        return cachedDir();
    }
    // fallback: scan for any instantclient_* dir (handles version drift)
    std::error_code ec;
    if (fs::exists(base)) {
        for (const auto& entry : fs::directory_iterator(base, ec)) {
            if (entry.is_directory() &&
                entry.path().filename().string().starts_with("instantclient_")) {
                cachedDir() = entry.path().string();
                return cachedDir();
            }
        }
    }
    return expected.string();
}

bool isInstalled() {
    const auto dir = fs::path(installDir());
    if (!fs::exists(dir))
        return false;
#if defined(__linux__)
    return fs::exists(dir / "libclntsh.so");
#elif defined(__APPLE__)
    return fs::exists(dir / "libclntsh.dylib");
#elif defined(_WIN32)
    return fs::exists(dir / "oci.dll");
#else
    return false;
#endif
}

namespace {

void notify(const ProgressCallback& cb, std::string phase, int64_t done, int64_t total,
            std::string msg = {}) {
    if (!cb)
        return;
    cb({std::move(phase), done, total, std::move(msg)});
}

} // namespace

Status install(const ProgressCallback& onProgress) {
    if (std::strlen(kDownloadPath) == 0) {
        return {false, "Oracle Instant Client auto-install is not supported on this platform"};
    }
    if (isInstalled()) {
        notify(onProgress, "done", 0, 0, "Already installed at " + installDir());
        return {true, ""};
    }

    {
        std::lock_guard lock(cacheMutex());
        cachedDir().clear();
    }

    fs::path parentDir = baseDir();
    fs::path installPath = parentDir / kExpectedDirName;
    std::error_code ec;
    fs::create_directories(parentDir, ec);
    if (ec) {
        std::string err = "Failed to create directory " + parentDir.string() + ": " + ec.message();
        notify(onProgress, "error", 0, 0, err);
        return {false, err};
    }
    fs::path archivePath = parentDir / kArchiveName;

    notify(onProgress, "downloading", 0, 0, std::string(kDownloadHost) + kDownloadPath);

    httplib::Client cli(kDownloadHost);
    cli.set_connection_timeout(15);
    cli.set_read_timeout(300);
    cli.set_follow_location(true);

    std::ofstream outFile(archivePath, std::ios::binary);
    if (!outFile.is_open()) {
        std::string err = "Failed to create download file: " + archivePath.string();
        notify(onProgress, "error", 0, 0, err);
        return {false, err};
    }

    int64_t bytesWritten = 0;
    bool writeFailed = false;
    auto res = cli.Get(
        kDownloadPath,
        [&](const char* data, size_t len) -> bool {
            outFile.write(data, static_cast<std::streamsize>(len));
            if (!outFile) {
                writeFailed = true;
                return false;
            }
            bytesWritten += static_cast<int64_t>(len);
            return true;
        },
        [&](uint64_t cur, uint64_t total) -> bool {
            notify(onProgress, "downloading", static_cast<int64_t>(cur),
                   static_cast<int64_t>(total));
            return true;
        });
    outFile.close();

    if (!res || res->status != 200) {
        std::string err =
            "Download failed: " + (res ? std::to_string(res->status) : std::string("no response"));
        fs::remove(archivePath, ec);
        notify(onProgress, "error", 0, 0, err);
        return {false, err};
    }
    if (writeFailed) {
        std::string err = "Download failed while writing to disk";
        fs::remove(archivePath, ec);
        notify(onProgress, "error", 0, 0, err);
        return {false, err};
    }

    notify(onProgress, "extracting", bytesWritten, bytesWritten);

    int ret = -1;
    std::string archiveStr = archivePath.string();
    std::string destStr = parentDir.string();

#if defined(__APPLE__)
    if (archiveStr.ends_with(".dmg")) {
        std::string mountPoint = destStr + "/oracle_dmg_mount";
        fs::create_directories(mountPoint, ec);
        std::string mountCmd = std::format("hdiutil attach '{}' -mountpoint '{}' -nobrowse -quiet",
                                           archiveStr, mountPoint);
        ret = std::system(mountCmd.c_str());
        if (ret == 0) {
            std::string icDir = installPath.string();
            fs::create_directories(icDir, ec);
            std::string copyCmd = std::format("cp -R '{}/.' '{}'", mountPoint, icDir);
            ret = std::system(copyCmd.c_str());
            std::system(std::format("hdiutil detach '{}' -quiet", mountPoint).c_str());
        }
        fs::remove_all(mountPoint, ec);
    } else {
        ret = std::system(std::format("unzip -o -q '{}' -d '{}'", archiveStr, destStr).c_str());
    }
#elif defined(_WIN32)
    ret = std::system(
        std::format("powershell -Command \"Expand-Archive -Force -Path '{}' -DestinationPath "
                    "'{}'\"",
                    archiveStr, destStr)
            .c_str());
#else
    ret = std::system(std::format("unzip -o -q '{}' -d '{}'", archiveStr, destStr).c_str());
#endif

    fs::remove(archivePath, ec);
    if (ret != 0) {
        std::string err = "Failed to extract Oracle Instant Client archive";
        notify(onProgress, "error", 0, 0, err);
        return {false, err};
    }

    {
        std::lock_guard lock(cacheMutex());
        cachedDir().clear();
    }

    if (!isInstalled()) {
        std::string err =
            "Extraction completed but Oracle Client libraries not found in expected location";
        notify(onProgress, "error", 0, 0, err);
        return {false, err};
    }

#if defined(__linux__)
    notify(onProgress, "installing-libaio", 0, 0);
    ensureLibaio(installPath);
#endif

    notify(onProgress, "done", 0, 0, "Installed to " + installPath.string());
    return {true, ""};
}

} // namespace dearsql::oracle
