#pragma once
//
// updater.h - auto-update check + download for Vertex.
//
// Vertex checks GitHub releases in the background. When a newer release exists,
// it downloads the platform portable binary next to the running executable as
// "Vertex-update" plus the platform extension. Pressing F12 launches the small
// VertexUpdater helper, exits Vertex, lets the helper replace the executable,
// and then restarts Vertex.
//

#include "platform/version.h"
#include "network/fetcher.h"
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <fstream>
#include <tuple>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

struct Updater {
    std::atomic<bool> updateAvailable{false};
    std::atomic<bool> checking{false};
    std::string updateVersion;
    std::string statusMessage;
    std::function<void()> onStatusChanged;

    static bool restartToUpdate(const std::string& exePath) {
        std::string updatePath = pendingPath(exePath);

        FILE* f = fopen(updatePath.c_str(), "rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);
        if (sz < 500 * 1024) { std::remove(updatePath.c_str()); return false; }

        std::string helper = helperPath(exePath);
        if (!fileExists(helper)) return false;

#ifdef _WIN32
        std::string command =
            quoteArg(helper)
            + " --pid " + std::to_string(GetCurrentProcessId())
            + " --target " + quoteArg(exePath)
            + " --update " + quoteArg(updatePath)
            + bridgeUpdateArg(exePath)
            + " --restart";
        std::vector<char> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back('\0');

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        // VertexUpdater is a console-subsystem helper; without CREATE_NO_WINDOW
        // it flashes a visible console window on every update.
        if (!CreateProcessA(helper.c_str(), mutableCommand.data(), NULL, NULL, FALSE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
            return false;
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        ExitProcess(0);
#else
        std::string pid = std::to_string(getpid());
        if (fork() == 0)
            execl(helper.c_str(), helper.c_str(),
                "--pid", pid.c_str(),
                "--target", exePath.c_str(),
                "--update", updatePath.c_str(),
                "--bridge-update", bridgePendingPath(exePath).c_str(),
                "--restart",
                nullptr);
        _exit(0);
#endif
        return true;
    }

#ifndef _WIN32
    // Minimal diagnostic log for a failed self-update apply (non-Windows
    // only — the Windows branch above already checks/rolls back inline).
    // Deliberately written to a system temp dir rather than next to the
    // target exePath (unlike updater_helper.cpp's LogPath()/LogMsg(), which
    // log beside the target) — the whole point of this log is to record
    // failures caused by exePath's directory not being writable, so logging
    // into that same directory would silently fail for the exact case it's
    // meant to catch.
    static void LogUpdaterEvent(const std::string& exePath, const std::string& msg) {
        FILE* f = std::fopen("/tmp/vertex_updater_apply.log", "a");
        if (!f) return;
        std::fprintf(f, "%s (target=%s)\n", msg.c_str(), exePath.c_str());
        std::fclose(f);
    }
#endif

    static void applyPendingUpdate(const std::string& exePath) {
        std::string updatePath = pendingPath(exePath);
        std::string oldPath = exePath + ".old";

        std::remove(oldPath.c_str());

        FILE* f = fopen(updatePath.c_str(), "rb");
        if (!f) return;
        fseek(f, 0, SEEK_END);
        long updateSize = ftell(f);
        fclose(f);
        if (updateSize < 500 * 1024) {
            std::remove(updatePath.c_str());
            return;
        }

#ifdef _WIN32
        if (MoveFileA(exePath.c_str(), oldPath.c_str())) {
            if (MoveFileA(updatePath.c_str(), exePath.c_str())) {
                applyPendingBridgeUpdate(exePath);
            } else {
                MoveFileA(oldPath.c_str(), exePath.c_str());
            }
        }
#else
        // Previously unchecked: a failed rename (e.g. no write permission on
        // the install directory) used to fail completely silently, leaving
        // the user on the old binary forever with zero indication anything
        // was wrong. std::rename() is atomic on POSIX — a failure here
        // leaves exePath untouched, so unlike the Windows branch above there
        // is nothing to roll back, but it's still worth logging so a stuck
        // update is diagnosable instead of invisible.
        if (std::rename(updatePath.c_str(), exePath.c_str()) == 0) {
            applyPendingBridgeUpdate(exePath);
        } else {
            LogUpdaterEvent(exePath, "applyPendingUpdate: rename failed, errno=" + std::to_string(errno));
        }
#endif
    }

    void checkForUpdateAsync(const std::string& exePath) {
        if (checking.exchange(true)) return;
        std::string exe = exePath;
        std::thread([this, exe]() {
            checkAndDownload(exe);
            checking = false;
        }).detach();
    }

private:
    void setStatus(const std::string& msg) {
        statusMessage = msg;
        if (onStatusChanged) onStatusChanged();
    }

    void checkAndDownload(const std::string& exePath) {
        setStatus("Checking for updates...");

        auto res = FetchUrl("https://api.github.com/repos/hackclubium/Vertex/releases/latest");
        if (!res.success || res.body.empty()) {
            setStatus("");
            return;
        }

        std::string tag;
        {
            size_t pos = res.body.find("\"tag_name\"");
            if (pos == std::string::npos) { setStatus(""); return; }
            size_t q1 = res.body.find('"', pos + 10);
            size_t q2 = res.body.find('"', q1 + 1);
            if (q1 == std::string::npos || q2 == std::string::npos) { setStatus(""); return; }
            tag = res.body.substr(q1 + 1, q2 - q1 - 1);
        }

        std::string remote = tag;
        if (!remote.empty() && remote[0] == 'v') remote = remote.substr(1);
        std::string local = VERTEX_VERSION;

        if (!isNewer(remote, local)) {
            setStatus("");
            return;
        }

        std::string assetName;
        std::string bridgeAssetName;
#ifdef _WIN32
        assetName = "Vertex-windows-portable.exe";
        bridgeAssetName = "Vertex-windows-vertex_arti.dll";
#elif defined(__APPLE__)
        assetName = "Vertex-macos-portable";
        bridgeAssetName = "Vertex-macos-libvertex_arti.dylib";
#else
        assetName = "Vertex-linux-portable";
        bridgeAssetName = "Vertex-linux-libvertex_arti.so";
#endif

        auto findAssetUrl = [&](const std::string& wanted) -> std::string {
            std::string found;
            std::string needle = "\"browser_download_url\"";
            size_t pos = 0;
            while (pos < res.body.size()) {
                size_t urlKey = res.body.find(needle, pos);
                if (urlKey == std::string::npos) break;
                size_t q1 = res.body.find('"', urlKey + needle.size());
                size_t q2 = (q1 != std::string::npos) ? res.body.find('"', q1 + 1) : std::string::npos;
                if (q1 != std::string::npos && q2 != std::string::npos) {
                    std::string url = res.body.substr(q1 + 1, q2 - q1 - 1);
                    if (url.find(wanted) != std::string::npos) { found = url; break; }
                }
                pos = urlKey + needle.size();
            }
            return found;
        };

        std::string downloadUrl = findAssetUrl(assetName);
        std::string bridgeUrl = findAssetUrl(bridgeAssetName);

        if (downloadUrl.empty()) { setStatus(""); return; }

        setStatus("Downloading Vertex " + tag + "...");

        auto dlRes = FetchUrl(downloadUrl);
        if (!dlRes.success || dlRes.body.size() < 500 * 1024) {
            setStatus("Update download failed.");
            return;
        }

        std::string updatePath = pendingPath(exePath);
        {
            std::ofstream out(updatePath, std::ios::binary);
            if (!out.is_open()) { setStatus("Failed to write update."); return; }
            out.write(dlRes.body.data(), dlRes.body.size());
        }

        if (!bridgeUrl.empty()) {
            auto bridgeRes = FetchUrl(bridgeUrl);
            if (bridgeRes.success && bridgeRes.body.size() > 100 * 1024) {
                std::ofstream bridgeOut(bridgePendingPath(exePath), std::ios::binary);
                if (bridgeOut.is_open()) bridgeOut.write(bridgeRes.body.data(), bridgeRes.body.size());
            }
        }

#ifndef _WIN32
        chmod(updatePath.c_str(), 0755);
#endif

        updateVersion = tag;
        updateAvailable = true;
        setStatus("Vertex " + tag + " ready. Press F12 to install.");
    }

    static std::string pendingPath(const std::string& exePath) {
#ifdef _WIN32
        size_t dot = exePath.rfind('.');
        if (dot != std::string::npos)
            return exePath.substr(0, dot) + "-update" + exePath.substr(dot);
        return exePath + "-update";
#else
        return exePath + "-update";
#endif
    }

    static bool fileExists(const std::string& path) {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return false;
        fclose(f);
        return true;
    }

    static void applyPendingBridgeUpdate(const std::string& exePath) {
        std::string pending = bridgePendingPath(exePath);
        if (!fileExists(pending)) return;
#ifdef _WIN32
        MoveFileExA(pending.c_str(), bridgePath(exePath).c_str(), MOVEFILE_REPLACE_EXISTING);
#else
        if (std::rename(pending.c_str(), bridgePath(exePath).c_str()) == 0)
            chmod(bridgePath(exePath).c_str(), 0755);
#endif
    }

    static std::string helperPath(const std::string& exePath) {
        size_t slash = exePath.find_last_of("/\\");
        std::string dir = slash == std::string::npos ? std::string() : exePath.substr(0, slash + 1);
#ifdef _WIN32
        return dir + "VertexUpdater.exe";
#else
        return dir + "VertexUpdater";
#endif
    }

    static std::string bridgePath(const std::string& exePath) {
        size_t slash = exePath.find_last_of("/\\");
        std::string dir = slash == std::string::npos ? std::string() : exePath.substr(0, slash + 1);
#ifdef _WIN32
        return dir + "vertex_arti.dll";
#elif defined(__APPLE__)
        return dir + "libvertex_arti.dylib";
#else
        return dir + "libvertex_arti.so";
#endif
    }

    static std::string bridgePendingPath(const std::string& exePath) {
        return bridgePath(exePath) + ".update";
    }

    static std::string bridgeUpdateArg(const std::string& exePath) {
        std::string pending = bridgePendingPath(exePath);
        return fileExists(pending) ? " --bridge-update " + quoteArg(pending) : "";
    }

    // Windows command-line quoting: a backslash is only special immediately
    // before a '"' (where N backslashes + '"' becomes N/2 backslashes + an
    // escaped quote). Backslashes elsewhere — the overwhelming majority in a
    // Windows path like "C:\Users\...\Vertex.exe" — must NOT be doubled, or
    // CommandLineToArgvW-style parsing reconstructs a mangled path.
    static std::string quoteArg(const std::string& arg) {
        std::string out = "\"";
        int backslashes = 0;
        for (char c : arg) {
            if (c == '\\') {
                ++backslashes;
            } else if (c == '"') {
                out.append(backslashes * 2 + 1, '\\');
                out.push_back('"');
                backslashes = 0;
            } else {
                out.append(backslashes, '\\');
                backslashes = 0;
                out.push_back(c);
            }
        }
        out.append(backslashes * 2, '\\'); // trailing backslashes must double before the closing quote
        out.push_back('"');
        return out;
    }

    static bool isNewer(const std::string& remote, const std::string& local) {
        auto parse = [](const std::string& s) -> std::tuple<int,int,int> {
            int major = 0, minor = 0, patch = 0;
            if (sscanf(s.c_str(), "%d.%d.%d", &major, &minor, &patch) < 1)
                return {0, 0, 0};
            return {major, minor, patch};
        };
        return parse(remote) > parse(local);
    }
};
