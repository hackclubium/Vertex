#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace vertex::profile {

struct ProfilePaths {
    std::string dataRoot;
    std::string cacheRoot;
    std::string profileRoot;
    std::string cacheProfileRoot;
    std::string historyFile;
    std::string bookmarksFile;
    std::string downloadsFile;
    std::string settingsFile;
    std::string cookiesFile;
    std::string localStorageDir;
    std::string sessionRestoreFile;
};

ProfilePaths ResolvePaths(const std::string& dataRoot,
                          const std::string& cacheRoot,
                          const std::string& profileName = "Default");

ProfilePaths DefaultPaths(const std::string& profileName = "Default");

bool EnsureDirectories(const ProfilePaths& paths);

void AppendTsvRow(const std::string& path, const std::vector<std::string>& fields);

std::vector<std::vector<std::string>> ReadTsvRows(const std::string& path,
                                                  size_t maxRows = 10000);

} // namespace vertex::profile
