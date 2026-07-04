#include "platform/downloads.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>

namespace vertex::downloads {
namespace {

bool StartsWithNoCase(const std::string& value, const char* prefix) {
    for (size_t i = 0; prefix[i]; ++i) {
        if (i >= value.size()) return false;
        if (std::tolower((unsigned char)value[i]) != std::tolower((unsigned char)prefix[i]))
            return false;
    }
    return true;
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

std::string PercentDecodePathPart(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '%' && i + 2 < input.size()) {
            int hi = HexValue(input[i + 1]);
            int lo = HexValue(input[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back((char)((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(input[i]);
    }
    return out;
}

std::string Trim(std::string value) {
    while (!value.empty() && std::isspace((unsigned char)value.front()))
        value.erase(value.begin());
    while (!value.empty() && std::isspace((unsigned char)value.back()))
        value.pop_back();
    return value;
}

std::string StripQuotes(std::string value) {
    value = Trim(std::move(value));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        value = value.substr(1, value.size() - 2);
    return value;
}

std::string FilenameFromContentDisposition(const std::string& header) {
    size_t start = 0;
    while (start < header.size()) {
        size_t semi = header.find(';', start);
        std::string part = Trim(header.substr(start, semi == std::string::npos ? std::string::npos : semi - start));
        size_t eq = part.find('=');
        if (eq != std::string::npos) {
            std::string name = Trim(part.substr(0, eq));
            std::string value = StripQuotes(part.substr(eq + 1));
            std::transform(name.begin(), name.end(), name.begin(),
                [](unsigned char c) { return (char)std::tolower(c); });
            if (name == "filename*" && StartsWithNoCase(value, "utf-8''"))
                return PercentDecodePathPart(value.substr(7));
            if (name == "filename")
                return value;
        }
        if (semi == std::string::npos) break;
        start = semi + 1;
    }
    return {};
}

std::string FilenameFromUrl(const std::string& url) {
    if (StartsWithNoCase(url, "data:"))
        return "download.txt";
    size_t end = url.find_first_of("?#");
    std::string path = url.substr(0, end);
    while (!path.empty() && path.back() == '/') path.pop_back();
    size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    name = PercentDecodePathPart(name);
    return name.empty() ? "download" : name;
}

std::string SanitizeFilename(std::string name) {
    name = StripQuotes(std::move(name));
    size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos)
        name = name.substr(slash + 1);
    for (char& c : name) {
        unsigned char uc = (unsigned char)c;
        if (uc < 32 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/'
            || c == '\\' || c == '|' || c == '?' || c == '*')
            c = '_';
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '.'))
        name.pop_back();
    if (name.empty() || name == "." || name == "..")
        name = "download";
    return name;
}

std::string JoinPath(const std::string& directory, const std::string& filename) {
    if (directory.empty()) return filename;
    char last = directory.back();
    if (last == '/' || last == '\\') return directory + filename;
    return directory + "/" + filename;
}

std::string UniquePathWithExists(const std::string& directory,
                                 const std::string& filename,
                                 const std::function<bool(const std::string&)>& exists) {
    const std::string safe = SanitizeFilename(filename);
    std::string candidate = JoinPath(directory, safe);
    if (!exists(candidate)) return candidate;

    std::string stem = safe;
    std::string ext;
    size_t dot = safe.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        stem = safe.substr(0, dot);
        ext = safe.substr(dot);
    }
    for (int i = 1; i < 10000; ++i) {
        candidate = JoinPath(directory, stem + " (" + std::to_string(i) + ")" + ext);
        if (!exists(candidate)) return candidate;
    }
    return JoinPath(directory, stem + " (copy)" + ext);
}

} // namespace

std::string SuggestFilename(const std::string& url,
                            const std::string& contentDisposition,
                            const std::string& downloadAttribute) {
    if (!downloadAttribute.empty())
        return SanitizeFilename(downloadAttribute);
    std::string fromHeader = FilenameFromContentDisposition(contentDisposition);
    if (!fromHeader.empty())
        return SanitizeFilename(fromHeader);
    return SanitizeFilename(FilenameFromUrl(url));
}

std::string DefaultDownloadsDirectory() {
#ifdef _WIN32
    const char* profile = std::getenv("USERPROFILE");
    if (profile && *profile) return std::string(profile) + "\\Downloads";
#else
    const char* home = std::getenv("HOME");
    if (home && *home) return std::string(home) + "/Downloads";
#endif
    return "Downloads";
}

std::string MakeUniquePath(const std::string& directory,
                           const std::string& filename) {
    return UniquePathWithExists(directory, filename, [](const std::string& path) {
        return std::filesystem::exists(std::filesystem::u8path(path));
    });
}

std::string MakeUniquePathForTests(const std::string& directory,
                                   const std::string& filename,
                                   const std::set<std::string>& existing) {
    return UniquePathWithExists(directory, filename, [&](const std::string& path) {
        return existing.find(path) != existing.end();
    });
}

DownloadRecord SaveFetchedBody(const std::string& url,
                               const FetchResult& result,
                               const std::string& downloadAttribute,
                               const std::string& directory) {
    DownloadRecord record;
    record.url = url;
    record.contentType = result.contentType;
    record.bytes = result.body.size();
    record.filename = SuggestFilename(url, result.contentDisposition, downloadAttribute);
    std::string dir = directory.empty() ? DefaultDownloadsDirectory() : directory;
    record.path = MakeUniquePath(dir, record.filename);

    if (!result.success) {
        record.error = result.error.empty() ? "Download failed" : result.error;
        return record;
    }

    try {
        std::filesystem::create_directories(std::filesystem::u8path(dir));
        std::ofstream out(std::filesystem::u8path(record.path), std::ios::binary);
        if (!out) {
            record.error = "Could not create file";
            return record;
        }
        out.write(result.body.data(), (std::streamsize)result.body.size());
        if (!out) {
            record.error = "Could not write file";
            return record;
        }
        record.success = true;
    } catch (const std::exception& e) {
        record.error = e.what();
    } catch (...) {
        record.error = "Could not save file";
    }
    return record;
}

} // namespace vertex::downloads
