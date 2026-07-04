#pragma once

#include "network/fetcher.h"

#include <set>
#include <string>

namespace vertex::downloads {

struct DownloadRecord {
    std::string url;
    std::string path;
    std::string filename;
    std::string contentType;
    size_t bytes = 0;
    bool success = false;
    std::string error;
};

std::string SuggestFilename(const std::string& url,
                            const std::string& contentDisposition,
                            const std::string& downloadAttribute = "");

std::string DefaultDownloadsDirectory();

std::string MakeUniquePath(const std::string& directory,
                           const std::string& filename);

std::string MakeUniquePathForTests(const std::string& directory,
                                   const std::string& filename,
                                   const std::set<std::string>& existing);

DownloadRecord SaveFetchedBody(const std::string& url,
                               const FetchResult& result,
                               const std::string& downloadAttribute = "",
                               const std::string& directory = "");

} // namespace vertex::downloads
