#pragma once

#include "database.hpp"
#include <functional>
#include <string>

namespace dearsql::oracle {

// Synchronously downloads and extracts Oracle Instant Client Basic Lite for
// the current platform/arch into a user-local cache directory. The Oracle
// backend automatically picks it up from there on next connect.
//
// The downloads are direct from download.oracle.com — no Oracle account
// needed for Basic Lite. The cache lives at:
//   ~/.dearsql/oracle-client/instantclient_<version>/  (POSIX)
//   %USERPROFILE%/.dearsql/oracle-client/instantclient_<version>/  (Windows)
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
Status install(const ProgressCallback& onProgress = {});

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
