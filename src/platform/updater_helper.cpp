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

static bool replaceFileWithRetry(const std::string& updatePath, const std::string& targetPath) {
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
        if (i == 79) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

#ifdef _WIN32
    if (!MoveFileA(updatePath.c_str(), targetPath.c_str())) {
        MoveFileA(backupPath.c_str(), targetPath.c_str());
        return false;
    }
#else
    if (std::rename(updatePath.c_str(), targetPath.c_str()) != 0) {
        std::rename(backupPath.c_str(), targetPath.c_str());
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

int main(int argc, char** argv) {
    std::string targetPath = argValue(argc, argv, "--target");
    std::string updatePath = argValue(argc, argv, "--update");
    if (targetPath.empty() || updatePath.empty() || !fileLooksUsable(updatePath))
        return 2;

    waitForProcessExit(argValue(argc, argv, "--pid"));
    if (!replaceFileWithRetry(updatePath, targetPath))
        return 3;

    if (hasFlag(argc, argv, "--restart"))
        restartTarget(targetPath);
    return 0;
}
