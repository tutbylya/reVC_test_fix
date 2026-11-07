#include "../../src/extras/mod_menu_storage.h"
#include "../../src/extras/modloader_folder.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
unsigned checks;

void Check(bool condition, const char* message)
{
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

fs::path FromUtf8(const std::string& name)
{
#if defined(__cpp_lib_char8_t)
    return fs::path(std::u8string(name.begin(), name.end()));
#else
    return fs::u8path(name);
#endif
}

std::string Utf8(const fs::path& path)
{
    const auto value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

void Write(const fs::path& path, const std::string& data)
{
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(data.data(), static_cast<std::streamsize>(data.size()));
    stream.close();
    Check(!!stream, "write own test fixture");
}

std::string Read(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream bytes;
    bytes << stream.rdbuf();
    return bytes.str();
}

const ModMenu::ResourceRow& Row(const std::vector<ModMenu::ResourceRow>& rows, const std::string& name)
{
    const auto found = std::find_if(rows.begin(), rows.end(), [&](const ModMenu::ResourceRow& row) { return row.name == name; });
    Check(found != rows.end(), "mod row exists");
    return *found;
}

std::size_t Loaded(const std::vector<ModMenu::ResourceRow>& rows)
{
    return static_cast<std::size_t>(std::count_if(rows.begin(), rows.end(), [](const auto& row) { return row.loaded; }));
}
} // namespace

int main()
{
    using namespace ModMenu;
    // Data-only fixtures, each run has its own directory; no recursive cleanup.
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::absolute(fs::path("build/mod-menu-state-tests") / std::to_string(tick));
    Check(fs::create_directories(root), "new test root");
    const fs::path mods = root / "modloader", sidecar = mods / ".revc-disabled";
    const std::string utf8Name = "\xD0\x9C\xD0\xBE\xD0\xB4 Pack";
    const std::string utf8Id = NormalizeId(utf8Name);
    Check(utf8Id == "\xD0\x9C\xD0\xBE\xD0\xB4 pack" && IsValidId(utf8Id), "UTF-8 name and ASCII case normalization");

    DisabledSet state{"old"};
    Check(ReadDisabled(sidecar, state) && state.empty(), "missing directory means empty state");
    ModLoaderFolder::Index empty;
    std::vector<ResourceRow> rows;
    Check(empty.Scan(Utf8(root)) && empty.GetMenuRows(rows) && rows.empty(), "missing modloader folder is a known empty inventory");
    fs::create_directory(mods);
    Check(ReadDisabled(sidecar, state) && state.empty(), "missing sidecar means empty state");
    std::istringstream valid("4d4f44\r\n\n7061636b");
    Check(ParseDisabled(valid, state) && state == DisabledSet{"mod", "pack"}, "CRLF/LF and no final newline");
    for (const std::string& bad : {std::string("0"), std::string("zz"), std::string("2e2e"),
        std::string("2f"), std::string("5c"), std::string("00"), std::string("ff"), std::string("eda080"),
        std::string("c080"), std::string("f4908080"), std::string("6d6f64\n4d4f44"),
        std::string(MaxStateBytes + 1, '\n')}) {
        std::istringstream invalid(bad);
        Check(!ParseDisabled(invalid, state) && state == DisabledSet{"mod", "pack"}, "bad state fails without partial results");
    }
    Check(!SetEnabled(sidecar, "../mod", false), "traversal ID rejected");
    Check(!SetEnabled(sidecar, "UPPER", false), "noncanonical ID rejected");
    Check(!SetEnabled(mods / "modloader.ini", "other", false), "cannot write existing profile/config name");
    Check(SetEnabled(sidecar, utf8Id, false) && SetEnabled(sidecar, "other", false), "persist exact UTF-8 and independent switches");
    Check(ReadDisabled(sidecar, state) && state == DisabledSet{"other", utf8Id}, "two entries preserved");
    Check(SetEnabled(sidecar, utf8Id, true) && ReadDisabled(sidecar, state) && state == DisabledSet{"other"}, "enable removes just one entry");

    const std::string profile = "[Profiles.Default.Priority]\nShadow=10\nWinner=80\n"
        "[Profiles.Default.IgnoreMods]\nBlocked\n[Profiles.Default.IgnoreFiles]\nignored.txd\n";
    Write(mods / "modloader.ini", profile);
    Write(mods / "Shadow" / "models" / "same.txd", "shadow");
    Write(mods / "Winner" / "other" / "same.txd", "winner");
    Write(mods / FromUtf8(utf8Name) / "second.txd", "unique");
    Write(mods / "Blocked" / "blocked.txd", "blocked");
    Write(mods / "DocOnly" / "README.txt", "doc");
    Write(mods / "IgnoredFile" / "ignored.txd", "ignored");
    Write(mods / ".hidden" / "hidden.txd", "hidden");
    Write(mods / "_ignore" / "ignored-folder.txd", "ignored-folder");
    ModLoaderFolder::Index initial;
    Check(initial.Scan(Utf8(root)) && initial.GetMenuRows(rows), "scan startup resource inventory");
    Check(Loaded(rows) == 2 && rows.size() == 6, "count winning resource owners only, no hidden folders");
    Check(Row(rows, "Winner").loaded && !Row(rows, "Shadow").loaded, "basename priority agrees with Resolve");
    Check(!Row(rows, "Blocked").manageable && !Row(rows, "Blocked").enabled, "profile-blocked mod cannot toggle");
    Check(!initial.SetMenuEnabled("blocked", true), "cannot override existing profile");
    Check(Row(rows, utf8Name).id == utf8Id && Row(rows, utf8Name).loaded, "name and ID retain UTF-8");
    const char* original = initial.Resolve("models/same.txd");
    Check(original && std::string(original).find("/Winner/") != std::string::npos, "highest priority basename is used");
    Check(initial.SetMenuEnabled("winner", false), "disable loaded mod for next startup");
    Check(initial.GetMenuRows(rows) && Row(rows, "Winner").loaded && !Row(rows, "Winner").enabled &&
        Row(rows, "Winner").pending && initial.Resolve("models/same.txd") == original, "pending toggle leaves live index intact");
    Check(Read(mods / "modloader.ini") == profile, "profile is unchanged by menu state");
    Check(ReadDisabled(sidecar, state) && state == DisabledSet{"other", "winner"}, "toggle preserves an unknown record");
    ModLoaderFolder::Index restarted;
    Check(restarted.Scan(Utf8(root)) && restarted.GetMenuRows(rows), "fresh startup applies persisted choice");
    Check(!Row(rows, "Winner").loaded && !Row(rows, "Winner").pending && Row(rows, "Shadow").loaded,
        "disabled row remains discoverable; fallback resource owner now wins");
    Check(restarted.SetMenuEnabled("winner", true) && restarted.GetMenuRows(rows) && Row(rows, "Winner").pending &&
        !Row(rows, "Winner").loaded && Row(rows, "Winner").enabled, "enable request is also restart-only");

    const auto conflict = mods / "Alias";
    fs::create_directory(conflict);
    if (fs::create_directory(mods / "alias")) {
        Check(restarted.Scan(Utf8(root)) && restarted.GetMenuRows(rows), "scan case-colliding directories");
        Check(!Row(rows, "Alias").manageable && !Row(rows, "alias").manageable &&
            !restarted.SetMenuEnabled("alias", false), "ambiguous folded IDs cannot disable both folders accidentally");
    }

    Write(sidecar, "malformed\n");
    Check(initial.GetMenuRows(rows) && Loaded(rows) == 2 && !Row(rows, "Winner").manageable,
        "bad sidecar locks edits without hiding known running inventory");
    Check(!initial.SetMenuEnabled("winner", false) && Read(sidecar) == "malformed\n", "malformed file not overwritten");
    Check(!restarted.Scan(Utf8(root)) && !restarted.Resolve("models/same.txd"), "malformed startup state fails closed");

    const fs::path links = root / "links", target = root / "owned-target.txt";
    fs::create_directory(links);
    Write(target, "6f74686572\n");
    std::error_code linkError;
    fs::create_symlink(target, links / ".revc-disabled", linkError);
    if (!linkError) {
        Check(!ReadDisabled(links / ".revc-disabled", state) && !SetEnabled(links / ".revc-disabled", "winner", false),
            "sidecar symlink is rejected for both reading and writing");
        Check(Read(target) == "6f74686572\n", "link target is not modified");
    }
    fs::create_directory_symlink(mods, root / "parent-link", linkError);
    if (!linkError) {
        Check(!ReadDisabled(root / "parent-link" / ".revc-disabled", state) &&
            !SetEnabled(root / "parent-link" / ".revc-disabled", "winner", false), "sidecar parent symlink rejected");
    }
    std::printf("Mod menu state: %u checks passed. Fixtures: %s\n", checks, Utf8(root).c_str());
    return 0;
}
