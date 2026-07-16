#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

// Minimal diagnostic trail: this helper runs invisibly (no console window)
// and its own process exits within seconds either way, so without a log file
// a failed update leaves absolutely no trace of what went wrong.
static std::string LogPath(const std::string& targetPath) {
    size_t slash = targetPath.find_last_of("/\\");
    std::string dir = slash == std::string::npos ? std::string() : targetPath.substr(0, slash + 1);
    return dir + "vertex_updater.log";
}

static void LogMsg(const std::string& logPath, const std::string& msg) {
    if (logPath.empty()) return;
    FILE* f = std::fopen(logPath.c_str(), "a");
    if (!f) return;
    std::fprintf(f, "%s\n", msg.c_str());
    std::fclose(f);
}

static std::string argValue(int argc, char** argv, const char* name) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0)
            return argv[i + 1];
    }
    return {};
}

static bool hasFlag(int argc, char** argv, const char* name) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0)
            return true;
    }
    return false;
}

static bool fileLooksUsable(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fclose(f);
    return size > 500 * 1024;
}

static bool bridgeLooksUsable(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fclose(f);
    return size > 100 * 1024;
}

static void waitForProcessExit(const std::string& pidText) {
    if (pidText.empty()) return;
    unsigned long pid = std::strtoul(pidText.c_str(), nullptr, 10);
    if (pid == 0) return;
#ifdef _WIN32
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (process) {
        WaitForSingleObject(process, 30000);
        CloseHandle(process);
        return;
    }
#else
    for (int i = 0; i < 300; ++i) {
        if (kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH)
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

static bool replaceFileWithRetry(const std::string& updatePath, const std::string& targetPath,
                                  const std::string& logPath) {
    const std::string backupPath = targetPath + ".old";
    std::remove(backupPath.c_str());

    for (int i = 0; i < 80; ++i) {
#ifdef _WIN32
        if (MoveFileA(targetPath.c_str(), backupPath.c_str()))
            break;
#else
        if (std::rename(targetPath.c_str(), backupPath.c_str()) == 0)
            break;
#endif
        if (i == 79) {
#ifdef _WIN32
            LogMsg(logPath, "rename target->backup failed after retries, GetLastError=" + std::to_string(GetLastError()));
#else
            LogMsg(logPath, "rename target->backup failed after retries, errno=" + std::to_string(errno));
#endif
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

#ifdef _WIN32
    if (!MoveFileA(updatePath.c_str(), targetPath.c_str())) {
        DWORD err = GetLastError();
        MoveFileA(backupPath.c_str(), targetPath.c_str());
        LogMsg(logPath, "rename update->target failed, GetLastError=" + std::to_string(err) + " (rolled back)");
        return false;
    }
#else
    if (std::rename(updatePath.c_str(), targetPath.c_str()) != 0) {
        int err = errno;
        std::rename(backupPath.c_str(), targetPath.c_str());
        LogMsg(logPath, "rename update->target failed, errno=" + std::to_string(err) + " (rolled back)");
        return false;
    }
    chmod(targetPath.c_str(), 0755);
#endif
    std::remove(backupPath.c_str());
    return true;
}

static void restartTarget(const std::string& targetPath) {
#ifdef _WIN32
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string command = "\"" + targetPath + "\"";
    if (CreateProcessA(targetPath.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
#else
    if (fork() == 0) {
        execl(targetPath.c_str(), targetPath.c_str(), nullptr);
        _exit(1);
    }
#endif
}

static std::string bridgePathForTarget(const std::string& targetPath) {
    size_t slash = targetPath.find_last_of("/\\");
    std::string dir = slash == std::string::npos ? std::string() : targetPath.substr(0, slash + 1);
#ifdef _WIN32
    return dir + "vertex_arti.dll";
#elif defined(__APPLE__)
    return dir + "libvertex_arti.dylib";
#else
    return dir + "libvertex_arti.so";
#endif
}

static void applyBridgeUpdate(const std::string& bridgeUpdatePath, const std::string& targetPath, const std::string& logPath) {
    if (bridgeUpdatePath.empty()) return;
    if (!bridgeLooksUsable(bridgeUpdatePath)) {
        LogMsg(logPath, "bridge update skipped: missing or too small");
        return;
    }
    const std::string bridgePath = bridgePathForTarget(targetPath);
#ifdef _WIN32
    if (!MoveFileExA(bridgeUpdatePath.c_str(), bridgePath.c_str(), MOVEFILE_REPLACE_EXISTING))
        LogMsg(logPath, "bridge update failed, GetLastError=" + std::to_string(GetLastError()));
#else
    if (std::rename(bridgeUpdatePath.c_str(), bridgePath.c_str()) != 0) {
        LogMsg(logPath, "bridge update failed, errno=" + std::to_string(errno));
    } else {
        chmod(bridgePath.c_str(), 0755);
    }
#endif
}

int main(int argc, char** argv) {
    std::string targetPath = argValue(argc, argv, "--target");
    std::string updatePath = argValue(argc, argv, "--update");
    std::string bridgeUpdatePath = argValue(argc, argv, "--bridge-update");
    std::string logPath = LogPath(targetPath);
    LogMsg(logPath, "--- update start: target=" + targetPath + " update=" + updatePath + " ---");

    if (targetPath.empty() || updatePath.empty() || !fileLooksUsable(updatePath)) {
        LogMsg(logPath, "aborting: missing --target/--update or update file not usable");
        return 2;
    }

    waitForProcessExit(argValue(argc, argv, "--pid"));
    if (!replaceFileWithRetry(updatePath, targetPath, logPath))
        return 3;
    applyBridgeUpdate(bridgeUpdatePath, targetPath, logPath);

    LogMsg(logPath, "update applied successfully");
    if (hasFlag(argc, argv, "--restart"))
        restartTarget(targetPath);
    return 0;
}
