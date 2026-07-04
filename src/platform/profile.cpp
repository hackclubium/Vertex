#include "platform/profile.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace vertex::profile {
namespace {

std::string JoinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    char last = a.back();
    if (last == '/' || last == '\\') return a + b;
    return a + "/" + b;
}

std::string EscapeTsvField(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '\t': out += "\\t"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        default: out += c; break;
        }
    }
    return out;
}

std::string UnescapeTsvField(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '\\' && i + 1 < input.size()) {
            char next = input[++i];
            switch (next) {
            case 't': out += '\t'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case '\\': out += '\\'; break;
            default: out += next; break;
            }
        } else {
            out += input[i];
        }
    }
    return out;
}

std::vector<std::string> SplitTsvLine(const std::string& line) {
    std::vector<std::string> fields;
    size_t start = 0;
    for (;;) {
        size_t tab = line.find('\t', start);
        fields.push_back(UnescapeTsvField(line.substr(start,
            tab == std::string::npos ? std::string::npos : tab - start)));
        if (tab == std::string::npos) break;
        start = tab + 1;
    }
    return fields;
}

#ifdef _WIN32
std::string KnownFolderPath(REFKNOWNFOLDERID id) {
    PWSTR wide = nullptr;
    if (SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &wide) != S_OK || !wide)
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), n, nullptr, nullptr);
    CoTaskMemFree(wide);
    return out;
}
#endif

} // namespace

ProfilePaths ResolvePaths(const std::string& dataRoot,
                          const std::string& cacheRoot,
                          const std::string& profileName) {
    ProfilePaths paths;
    paths.dataRoot = dataRoot;
    paths.cacheRoot = cacheRoot.empty() ? dataRoot : cacheRoot;
    paths.profileRoot = JoinPath(paths.dataRoot, profileName);
    paths.cacheProfileRoot = JoinPath(paths.cacheRoot, profileName);
    paths.historyFile = JoinPath(paths.profileRoot, "history.tsv");
    paths.bookmarksFile = JoinPath(paths.profileRoot, "bookmarks.tsv");
    paths.downloadsFile = JoinPath(paths.profileRoot, "downloads.tsv");
    paths.settingsFile = JoinPath(paths.profileRoot, "settings.json");
    paths.cookiesFile = JoinPath(paths.profileRoot, "cookies.tsv");
    paths.localStorageDir = JoinPath(paths.profileRoot, "local_storage");
    paths.sessionRestoreFile = JoinPath(paths.profileRoot, "session_restore.json");
    return paths;
}

ProfilePaths DefaultPaths(const std::string& profileName) {
#ifdef _WIN32
    std::string dataRoot = KnownFolderPath(FOLDERID_LocalAppData);
    if (dataRoot.empty()) {
        const char* local = std::getenv("LOCALAPPDATA");
        if (local && *local) dataRoot = local;
    }
    if (dataRoot.empty()) {
        const char* profile = std::getenv("USERPROFILE");
        if (profile && *profile) dataRoot = std::string(profile) + "\\AppData\\Local";
    }
    std::string localRoot = dataRoot.empty() ? "." : dataRoot;
    dataRoot = JoinPath(localRoot, "Vertex/User Data");
    std::string cacheRoot = JoinPath(localRoot, "Vertex/Cache");
    return ResolvePaths(dataRoot, cacheRoot, profileName);
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    std::string base = home && *home ? home : ".";
    return ResolvePaths(base + "/Library/Application Support/Vertex",
                        base + "/Library/Caches/Vertex",
                        profileName);
#else
    const char* xdgConfig = std::getenv("XDG_CONFIG_HOME");
    const char* xdgCache = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    std::string base = home && *home ? home : ".";
    std::string dataRoot = xdgConfig && *xdgConfig ? std::string(xdgConfig) + "/Vertex"
                                                   : base + "/.config/Vertex";
    std::string cacheRoot = xdgCache && *xdgCache ? std::string(xdgCache) + "/Vertex"
                                                  : base + "/.cache/Vertex";
    return ResolvePaths(dataRoot, cacheRoot, profileName);
#endif
}

bool EnsureDirectories(const ProfilePaths& paths) {
    try {
        std::filesystem::create_directories(std::filesystem::u8path(paths.profileRoot));
        std::filesystem::create_directories(std::filesystem::u8path(paths.cacheProfileRoot));
        std::filesystem::create_directories(std::filesystem::u8path(paths.localStorageDir));
        return true;
    } catch (...) {
        return false;
    }
}

void AppendTsvRow(const std::string& path, const std::vector<std::string>& fields) {
    std::filesystem::create_directories(std::filesystem::u8path(path).parent_path());
    std::ofstream out(std::filesystem::u8path(path), std::ios::binary | std::ios::app);
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i) out << '\t';
        out << EscapeTsvField(fields[i]);
    }
    out << '\n';
}

std::vector<std::vector<std::string>> ReadTsvRows(const std::string& path, size_t maxRows) {
    std::vector<std::vector<std::string>> rows;
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        rows.push_back(SplitTsvLine(line));
        if (maxRows && rows.size() >= maxRows) break;
    }
    return rows;
}

} // namespace vertex::profile
