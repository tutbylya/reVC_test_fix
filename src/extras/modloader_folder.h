#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace ModLoaderFolder {

struct File {
    std::string path;       // Absolute path with its actual filesystem spelling.
    std::string relative;   // Path relative to the owning mod directory.
    uint64_t size = 0;
    int priority = 50;
};

class Index {
public:
    // Rebuild at startup. Higher priorities win; alphabetical order breaks ties
    // deterministically, with the later mod/path winning. No files are written.
    bool Scan(const std::string& gameRoot);
    // Returned storage belongs to this index and survives until Clear/Scan.
    // Exact relative paths win ties; a higher-priority basename override wins.
    // Missing resources, traversal requests and symlink replacements are rejected.
    const char* Resolve(const char* request) const;
    const std::map<std::string, File>& FilesByName() const { return filesByName_; }
    void Clear();

private:
    std::string root_;
    std::string modloaderRoot_;
    std::map<std::string, File> filesByPath_;
    std::map<std::string, File> filesByName_;
};

} // namespace ModLoaderFolder
