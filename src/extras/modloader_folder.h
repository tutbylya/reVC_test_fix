#pragma once

#include "mod_menu_state.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ModLoaderFolder {

struct File {
    std::string path;       // Absolute path with its actual filesystem spelling.
    std::string relative;   // Path relative to the owning mod directory.
    uint64_t size = 0;
    int priority = 50;
    std::size_t menuRow = 0; // Owning startup row, even for case-colliding names.
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
    bool GetMenuRows(std::vector<ModMenu::ResourceRow>& rows) const;
    bool SetMenuEnabled(const std::string& id, bool enabled) const;
    void Clear();

private:
    std::string root_;
    std::string modloaderRoot_;
    std::map<std::string, File> filesByPath_;
    std::map<std::string, File> filesByName_;
    std::vector<ModMenu::ResourceRow> menuRows_;
};

} // namespace ModLoaderFolder
