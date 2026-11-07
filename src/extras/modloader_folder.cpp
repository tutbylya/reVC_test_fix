#include "modloader_folder.h"

// The game uses this backend on Linux. Windows also builds the same code for
// portable data-only tests. Don't require desktop filesystem APIs elsewhere.
#if defined(__linux__) || defined(_WIN32)
#include "mod_menu_storage.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace ModLoaderFolder {
namespace {
namespace fs = std::filesystem;

fs::path Utf8Path(const std::string& text)
{
#if defined(__cpp_lib_char8_t)
    return fs::path(std::u8string(text.begin(), text.end()));
#else
    return fs::u8path(text);
#endif
}

std::string PathText(const fs::path& path)
{
    // C++17 returns string and C++20 returns u8string; preserve UTF-8 in both.
    const auto text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string Fold(std::string text)
{
    for (char& c : text) {
        if (c == '\\') c = '/';
        else if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    }
    return text;
}

std::string Trim(const std::string& text)
{
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

std::string IniText(const std::string& text)
{
    char quote = 0;
    size_t end = text.size();
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quote) {
            if (c == quote) quote = 0;
        } else if (c == '\'' || c == '"') quote = c;
        else if (c == ';' || c == '#') { end = i; break; }
    }
    std::string result = Trim(text.substr(0, end));
    if (result.size() >= 2 && (result.front() == '\'' || result.front() == '"') &&
        result.find(result.front(), 1) == result.size() - 1) result = result.substr(1, result.size() - 2);
    return result;
}

void Warn(const char* message, const std::string& path)
{
    std::fprintf(stderr, "[modloader] %s: %s\n", message, path.c_str());
}

bool Alphabetical(const fs::path& a, const fs::path& b)
{
    const std::string left = PathText(a), right = PathText(b);
    const std::string lowerLeft = Fold(left), lowerRight = Fold(right);
    return lowerLeft == lowerRight ? left < right : lowerLeft < lowerRight;
}

bool Wildcard(const std::string& pattern, const std::string& text)
{
    size_t p = 0, t = 0, star = std::string::npos, retry = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) { ++p; ++t; }
        else if (p < pattern.size() && pattern[p] == '*') { star = p++; retry = t; }
        else if (star != std::string::npos) { p = star + 1; t = ++retry; }
        else return false;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

bool Matches(const std::vector<std::string>& patterns, const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    const std::string name = path.substr(slash == std::string::npos ? 0 : slash + 1);
    for (const std::string& pattern : patterns)
        if (Wildcard(pattern, pattern.find('/') == std::string::npos ? name : path)) return true;
    return false;
}

bool RelativeKey(const std::string& text, std::string& result)
{
    result.clear();
    size_t begin = 0;
    while (begin <= text.size()) {
        const size_t end = text.find('/', begin);
        const std::string part = text.substr(begin, end == std::string::npos ? end : end - begin);
        if (part == ".." || part.find(':') != std::string::npos) return false;
        if (!part.empty() && part != ".") {
            if (!result.empty()) result += '/';
            result += Fold(part);
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return !result.empty();
}

bool RegularWithoutSymlinks(const fs::path& root, const fs::path& path)
{
    const fs::path relative = path.lexically_relative(root);
    if (relative.empty() || relative.is_absolute()) return false;
    std::error_code error;
    fs::path current = root;
    fs::file_status status = fs::symlink_status(current, error);
    if (error || !fs::is_directory(status) || fs::is_symlink(status)) return false;
    for (const fs::path& part : relative) {
        if (part == "..") return false;
        current /= part;
        status = fs::symlink_status(current, error);
        if (error || fs::is_symlink(status)) return false;
    }
    return fs::is_regular_file(status);
}

struct Profile {
    bool defined = false;
    bool hasIgnoreAll = false, ignoreAll = false;
    bool hasExcludeAll = false, excludeAll = false;
    std::vector<std::string> parents;
    std::map<std::string, int> priorities;
    std::vector<std::string> ignoreMods, ignoreFiles, includeMods;
};

struct Configuration {
    std::string selected = "default";
    std::map<std::string, Profile> profiles;
    bool supported = true;
};

bool Boolean(const std::string& text, bool& value)
{
    const std::string lower = Fold(Trim(text));
    if (lower == "true" || lower == "yes" || lower == "on" || lower == "1") value = true;
    else if (lower == "false" || lower == "no" || lower == "off" || lower == "0") value = false;
    else return false;
    return true;
}

void Unsupported(Configuration& config, const std::string& detail)
{
    config.supported = false;
    Warn("Unsupported/invalid folder configuration; mod loading disabled", detail);
}

void ReadConfiguration(const fs::path& path, Configuration& config, bool selectProfile)
{
    std::error_code error;
    const fs::file_status status = fs::symlink_status(path, error);
    if (error || !fs::exists(status)) {
        if (error && error != std::errc::no_such_file_or_directory) Unsupported(config, PathText(path) + " (cannot inspect)");
        return;
    }
    if (!fs::is_regular_file(status) || fs::is_symlink(status)) {
        Unsupported(config, PathText(path) + " (config must be a regular file, not a symlink)");
        return;
    }
    const uintmax_t size = fs::file_size(path, error);
    if (error || size > 4 * 1024 * 1024) { Unsupported(config, PathText(path) + " (cannot read/too large)"); return; }
    std::ifstream input(path);
    if (!input) { Unsupported(config, PathText(path) + " (cannot open)"); return; }
    std::string section, line;
    size_t lineNumber = 0;
    while (std::getline(input, line)) {
        if (++lineNumber == 1 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
        line = IniText(line);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') { section = Fold(Trim(line.substr(1, line.size() - 2))); continue; }
        const size_t equals = line.find('=');
        const std::string key = Fold(IniText(line.substr(0, equals)));
        const std::string value = equals == std::string::npos ? std::string() : IniText(line.substr(equals + 1));
        if (section == "folder.config") {
            if (selectProfile && key == "profile" && !value.empty()) config.selected = Fold(value);
            continue;
        }
        if (section.compare(0, 9, "profiles.") != 0) continue;
        const size_t dot = section.find_last_of('.');
        if (dot <= 9 || dot == section.size() - 1) { Unsupported(config, section); continue; }
        Profile& profile = config.profiles[section.substr(9, dot - 9)];
        profile.defined = true;
        const std::string kind = section.substr(dot + 1);
        const std::string detail = PathText(path) + ':' + std::to_string(lineNumber) + " [" + section + "] " + key;
        if (kind == "config") {
            if (key == "parents") {
                profile.parents.clear();
                std::istringstream parents(value);
                std::set<std::string> parentKeys;
                std::string name;
                while (std::getline(parents, name, ',')) {
                    name = Fold(IniText(name));
                    if (!name.empty() && parentKeys.insert(name).second) profile.parents.push_back(name);
                }
            } else if (key == "ignoreallmods" || key == "ignoreallfiles") {
                profile.hasIgnoreAll = Boolean(value, profile.ignoreAll);
                if (!profile.hasIgnoreAll) Unsupported(config, detail);
            } else if (key == "excludeallmods") {
                profile.hasExcludeAll = Boolean(value, profile.excludeAll);
                if (!profile.hasExcludeAll) Unsupported(config, detail);
            } else Unsupported(config, detail);
        } else if (kind == "priority") {
            bool valid = !value.empty();
            int priority = 0;
            for (char c : value) {
                if (c < '0' || c > '9' || priority > 100) { valid = false; break; }
                priority = priority * 10 + c - '0';
            }
            if (!valid || priority > 100 || key.empty()) Unsupported(config, detail);
            else profile.priorities[key] = priority;
        } else if (kind == "ignoremods" || kind == "ignorefiles" || kind == "includemods") {
            bool enabled = true;
            bool parsedValue = true;
            if (equals != std::string::npos && Boolean(value, parsedValue)) enabled = parsedValue;
            if (!enabled || key.empty()) continue;
            std::vector<std::string>& list = kind == "ignoremods" ? profile.ignoreMods :
                kind == "ignorefiles" ? profile.ignoreFiles : profile.includeMods;
            list.push_back(key);
        } else Unsupported(config, detail);
    }
    if (input.bad()) Unsupported(config, PathText(path) + " (read failed)");
}

bool Merge(Profile& target, const Profile& source, size_t& budget)
{
    const size_t entries = source.priorities.size() + source.ignoreMods.size() + source.ignoreFiles.size() + source.includeMods.size();
    if (entries > budget) return false;
    budget -= entries;
    if (source.hasIgnoreAll) { target.hasIgnoreAll = true; target.ignoreAll = source.ignoreAll; }
    if (source.hasExcludeAll) { target.hasExcludeAll = true; target.excludeAll = source.excludeAll; }
    for (const auto& priority : source.priorities) target.priorities[priority.first] = priority.second;
    auto appendUnique = [](std::vector<std::string>& list, const std::vector<std::string>& added) {
        std::set<std::string> unique(list.begin(), list.end());
        for (const std::string& item : added) if (unique.insert(item).second) list.push_back(item);
    };
    appendUnique(target.ignoreMods, source.ignoreMods);
    appendUnique(target.ignoreFiles, source.ignoreFiles);
    appendUnique(target.includeMods, source.includeMods);
    return true;
}

bool Inherit(const Configuration& config, const std::string& name, Profile& output,
             std::set<std::string>& visiting, std::map<std::string, Profile>& resolved, size_t& budget)
{
    const auto cached = resolved.find(name);
    if (cached != resolved.end()) { output = cached->second; return true; }
    const auto found = config.profiles.find(name);
    if (found == config.profiles.end() || !found->second.defined || visiting.size() >= 64 || !visiting.insert(name).second) {
        Warn("Missing/cyclic profile inheritance; mod loading disabled", name);
        return false;
    }
    const Profile& source = found->second;
    if (std::find(source.parents.begin(), source.parents.end(), "$none") == source.parents.end()) {
        for (const std::string& parentName : source.parents) {
            Profile parent;
            const std::string& parentKey = parentName == "$current" ? config.selected : parentName;
            if (!Inherit(config, parentKey, parent, visiting, resolved, budget) || !Merge(output, parent, budget)) {
                Warn("Profile inheritance is invalid/too large; mod loading disabled", name);
                visiting.erase(name);
                return false;
            }
        }
    }
    if (!Merge(output, source, budget)) {
        Warn("Profile inheritance is too large; mod loading disabled", name);
        visiting.erase(name);
        return false;
    }
    visiting.erase(name);
    resolved[name] = output;
    return true;
}

std::vector<fs::path> Children(const fs::path& path, bool directories)
{
    std::vector<fs::path> result;
    std::error_code error;
    fs::directory_iterator entry(path, fs::directory_options::skip_permission_denied, error), end;
    if (error) Warn("Cannot list directory", PathText(path));
    while (entry != end) {
        const fs::path child = entry->path();
        const fs::file_status status = entry->symlink_status(error);
        if (error) Warn("Cannot inspect entry", PathText(child));
        else if (!fs::is_symlink(status) && (directories ? fs::is_directory(status) : fs::is_regular_file(status)))
            result.push_back(child);
        entry.increment(error);
        if (error) { Warn("Cannot continue directory scan", PathText(path)); error.clear(); }
    }
    std::sort(result.begin(), result.end(), Alphabetical);
    return result;
}

void Select(std::map<std::string, File>& index, const std::string& key, const File& file)
{
    const auto previous = index.find(key);
    if (previous == index.end() || previous->second.priority <= file.priority) index[key] = file;
}

bool IsResource(const std::string& key)
{
    // Count indexed resource packs, not documentation-only or empty folders.
    // Leave lookup behavior unchanged for custom extensions referenced by DAT.
    const auto dot = key.rfind('.');
    if (dot == std::string::npos) return false;
    const auto extension = key.substr(dot);
    return extension == ".dff" || extension == ".txd" || extension == ".col" ||
        extension == ".ifp" || extension == ".img" || extension == ".dir" ||
        extension == ".ide" || extension == ".ipl" || extension == ".dat" ||
        extension == ".cfg" || extension == ".scm" || extension == ".gxt" ||
        extension == ".bmp" || extension == ".mpg" || extension == ".mpeg";
}
} // namespace

void Index::Clear()
{
    filesByPath_.clear();
    filesByName_.clear();
    menuRows_.clear();
    root_.clear();
    modloaderRoot_.clear();
}

bool Index::Scan(const std::string& gameRoot)
{
    Clear();
    if (gameRoot.empty()) { Warn("Invalid game directory", gameRoot); return false; }
    std::error_code error;
    const fs::path root = fs::absolute(Utf8Path(gameRoot), error).lexically_normal();
    if (error || !fs::is_directory(root, error)) { Warn("Invalid game directory", gameRoot); return false; }
    root_ = PathText(root);
    while (root_.size() > 1 && root_.back() == '/') root_.pop_back();
    const fs::path mods = root / "modloader";
    modloaderRoot_ = PathText(mods);
    const fs::file_status modsStatus = fs::symlink_status(mods, error);
    if (error == std::errc::no_such_file_or_directory || (!error && !fs::exists(modsStatus))) return true;
    if (error || !fs::is_directory(modsStatus) || fs::is_symlink(modsStatus)) { Warn("Invalid modloader directory", modloaderRoot_); return false; }

    ModMenu::DisabledSet disabled;
    if (!ModMenu::ReadDisabled(mods / ".revc-disabled", disabled)) {
        Warn("Invalid mod menu state; mod loading disabled", modloaderRoot_);
        return false;
    }

    Configuration config;
    config.profiles["default"].defined = true;
    const fs::path profileDirectory = mods / ".profiles";
    const fs::file_status profileStatus = fs::symlink_status(profileDirectory, error);
    if (!error && fs::is_directory(profileStatus) && !fs::is_symlink(profileStatus)) {
        for (const fs::path& path : Children(profileDirectory, false))
            if (Fold(PathText(path.extension())) == ".ini") ReadConfiguration(path, config, false);
    } else if (error && error != std::errc::no_such_file_or_directory) Warn("Cannot inspect .profiles directory", PathText(profileDirectory));
    ReadConfiguration(mods / "modloader.ini", config, true);
    Profile selected;
    std::set<std::string> visiting;
    std::map<std::string, Profile> resolved;
    size_t inheritanceBudget = 262144;
    if (!config.supported || !Inherit(config, config.selected, selected, visiting, resolved, inheritanceBudget)) return false;
    const auto folders = Children(mods, true);
    std::map<std::string, unsigned> idCounts;
    for (const auto& folder : folders) {
        const std::string id = ModMenu::NormalizeId(PathText(folder.filename()));
        if (ModMenu::IsValidId(id)) ++idCounts[id];
    }
    for (const fs::path& mod : folders) {
        const std::string displayName = PathText(mod.filename());
        const std::string name = Fold(displayName);
        if (name.empty() || name.front() == '.' || name == "_ignore") continue;
        const auto priorityEntry = selected.priorities.find(name);
        const int priority = priorityEntry == selected.priorities.end() ? 50 : priorityEntry->second;
        const bool profileEnabled = !selected.ignoreAll && priority != 0 &&
            !Matches(selected.ignoreMods, name) && (!selected.excludeAll || Matches(selected.includeMods, name));
        const std::string id = ModMenu::NormalizeId(displayName);
        const bool canIdentify = ModMenu::IsValidId(id);
        ModMenu::ResourceRow row{id, displayName, false,
            profileEnabled && !disabled.count(id), profileEnabled && canIdentify && idCounts[id] == 1, false};
        if (!row.enabled) {
            menuRows_.push_back(std::move(row));
            continue;
        }
        std::vector<fs::path> files;
        fs::recursive_directory_iterator entry(mod, fs::directory_options::skip_permission_denied, error), end;
        if (error) Warn("Cannot scan mod", PathText(mod));
        while (entry != end) {
            const fs::path path = entry->path();
            const fs::file_status status = entry->symlink_status(error);
            if (error) { Warn("Cannot inspect mod resource", PathText(path)); entry.disable_recursion_pending(); }
            else if (fs::is_symlink(status)) entry.disable_recursion_pending();
            else if (fs::is_directory(status)) {
                const std::string leaf = PathText(path.filename());
                if ((!leaf.empty() && leaf.front() == '.') || entry.depth() >= 64) entry.disable_recursion_pending();
            } else if (fs::is_regular_file(status)) files.push_back(path);
            entry.increment(error);
            if (error) { Warn("Cannot continue mod scan", PathText(mod)); error.clear(); }
        }
        std::sort(files.begin(), files.end(), Alphabetical);
        for (const fs::path& path : files) {
            File file;
            file.path = PathText(path);
            file.relative = PathText(path.lexically_relative(mod));
            std::string key;
            if (!RelativeKey(Fold(file.relative), key) || Matches(selected.ignoreFiles, key)) continue;
            const uintmax_t size = fs::file_size(path, error);
            if (error || size > std::numeric_limits<uint64_t>::max()) { Warn("Cannot measure mod resource", file.path); continue; }
            file.size = static_cast<uint64_t>(size);
            file.priority = priority;
            file.menuRow = menuRows_.size();
            Select(filesByPath_, key, file);
            Select(filesByName_, Fold(PathText(path.filename())), file);
        }
        menuRows_.push_back(std::move(row));
    }
    // Attribute winning resources only. A higher-priority basename also
    // overrides a lower-priority exact path (same rule as Resolve()).
    for (const auto& item : filesByName_)
        if (item.second.size && IsResource(item.first)) menuRows_[item.second.menuRow].loaded = true;
    for (const auto& item : filesByPath_) {
        if (!item.second.size || !IsResource(item.first)) continue;
        const auto slash = item.first.find_last_of('/');
        const auto named = filesByName_.find(item.first.substr(slash == std::string::npos ? 0 : slash + 1));
        if (named == filesByName_.end() || named->second.priority <= item.second.priority)
            menuRows_[item.second.menuRow].loaded = true;
    }
    return true;
}

bool Index::GetMenuRows(std::vector<ModMenu::ResourceRow>& rows) const
{
    ModMenu::DisabledSet disabled;
    if (modloaderRoot_.empty() || !ModMenu::ReadDisabled(Utf8Path(modloaderRoot_) / ".revc-disabled", disabled)) {
        // Still report the known startup inventory; a broken sidecar locks
        // changes rather than pretending those mods were unloaded.
        rows = menuRows_;
        for (auto& row : rows) row.manageable = false;
        return !modloaderRoot_.empty();
    }
    rows = menuRows_;
    for (auto& row : rows) {
        if (!row.manageable) continue;
        const bool enabled = !disabled.count(row.id);
        row.pending = enabled != row.enabled;
        row.enabled = enabled;
    }
    return true;
}

bool Index::SetMenuEnabled(const std::string& id, bool enabled) const
{
    const auto row = std::find_if(menuRows_.begin(), menuRows_.end(), [&](const ModMenu::ResourceRow& candidate) {
        return candidate.id == id && candidate.manageable;
    });
    if (row == menuRows_.end()) return false;
    std::error_code error;
    const auto directory = Utf8Path(modloaderRoot_);
    const auto status = fs::symlink_status(directory / Utf8Path(row->name), error);
    return !error && fs::is_directory(status) && !fs::is_symlink(status) &&
        ModMenu::SetEnabled(directory / ".revc-disabled", id, enabled);
}

const char* Index::Resolve(const char* request) const
{
    if (!request || !*request || root_.empty()) return nullptr;
    std::string text = request;
    std::replace(text.begin(), text.end(), '\\', '/');
    if (text.front() == '/' || (text.size() >= 2 && text[1] == ':')) {
        const std::string prefix = Fold(root_) + (root_.back() == '/' ? "" : "/");
        if (Fold(text).compare(0, prefix.size(), prefix) != 0) return nullptr;
        text.erase(0, prefix.size());
    }
    std::string key;
    if (!RelativeKey(text, key)) return nullptr;
    const File* file = nullptr;
    const auto exact = filesByPath_.find(key);
    if (exact != filesByPath_.end()) file = &exact->second;
    const size_t slash = key.find_last_of('/');
    const auto named = filesByName_.find(key.substr(slash == std::string::npos ? 0 : slash + 1));
    if (named != filesByName_.end() && (!file || named->second.priority > file->priority)) file = &named->second;
    if (!file || !RegularWithoutSymlinks(Utf8Path(modloaderRoot_), Utf8Path(file->path))) return nullptr;
    return file->path.c_str();
}

} // namespace ModLoaderFolder

#endif
