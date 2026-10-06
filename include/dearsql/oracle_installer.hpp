#pragma once

#include "database.hpp"
#include <functional>
#include <string>

namespace dearsql::oracle {

// Synchronously downloads and extracts Oracle Instant Client Basic Lite for
// the current platform/arch into a user-local cache directory. The Oracle
// backend picks it up from there on next connect (and, unless
// ClientOptions::autoInstall is off, runs this itself when it is missing).
//
// The downloads are direct from download.oracle.com — no Oracle account
// needed for Basic Lite. The cache lives at <root>/instantclient_<version>/,
// root defaulting to ~/.dearsql/oracle-client (%USERPROFILE% on Windows); see
// setInstallRoot().
//
// On Linux, also bundles a libaio.so.1 with the SONAME that Oracle's
// libclntsh.so demands (Ubuntu 24.04+ ships libaio.so.1t64 with a SONAME
// libclntsh refuses).

// Path the installer writes to / reads from. Returns the canonical install
// directory if one exists, otherwise the path it WOULD use for a fresh
// install. Empty string on unsupported platforms.
std::string installDir();

// Returns true if a usable Oracle Instant Client is already present at
// installDir(). Checks for libclntsh.{dylib,so} (POSIX) or oci.dll (Windows).
bool isInstalled();

// Diagnostic: the URL that install() would download from on this platform.
// Empty string on unsupported platforms.
std::string downloadUrl();

// Root the client is installed under (instantclient_<version>/ goes inside).
// Default ~/.dearsql/oracle-client. Set it before the first connect.
void setInstallRoot(const std::string& dir);

// How the Oracle backend gets the client loaded on connect.
struct ClientOptions {
    // run install() inside the first connect when the client is missing
    bool autoInstall = true;
    // linux: re-exec the process with installDir() on LD_LIBRARY_PATH so
    // libclntsh finds libnnz/libclntshcore. Hosts that turn this off must put
    // installDir() on LD_LIBRARY_PATH themselves (launcher script, AppImage).
    bool reexecForLibraryPath = true;
};
void setClientOptions(const ClientOptions& options);

// true when the ODPI-C context cannot be created (client missing or not
// loadable). Tries once without installing; a failure is remembered until
// resetContext() or until the client shows up in installDir().
bool needsClientInstall();

// forget a failed context init (and the auto-install attempt) so the next
// connect retries; call after install(). A live context is kept.
void resetContext();

// Optional progress callback. Phase is one of "downloading", "extracting",
// "installing-libaio", "done", "error". Bytes are 0 if not applicable.
struct Progress {
    std::string phase;
    int64_t bytesDownloaded = 0;
    int64_t bytesTotal = 0;
    std::string message;
};
using ProgressCallback = std::function<void(const Progress&)>;

// Blocking download + extract. Returns {true, ""} on success, {false, error}
// on failure. Several minutes on a fresh install; idempotent if already
// installed (returns success without re-downloading).
// shouldCancel is polled during the download; a cancelled install removes the
// partial archive and returns {false, "cancelled"}.
Status install(const ProgressCallback& onProgress = {},
               const std::function<bool()>& shouldCancel = {});

#if defined(__linux__)
// Ensures libaio.so.1 with the correct SONAME is present in `installDir`.
// Tries (in order): existing bundled copy, system libaio with correct
// SONAME, then downloads the Ubuntu 22.04 libaio1 deb. No-op if a usable
// libaio.so.1 is already in place. Idempotent; safe to call on every
// connect to recover from a previous install that placed libclntsh.so but
// failed to fetch libaio.
void ensureLibaio(const std::string& installDir);
#endif

} // namespace dearsql::oracle
