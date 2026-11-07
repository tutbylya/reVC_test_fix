#pragma once

#include "mod_menu_state.h"

#include <string>
#include <vector>

namespace ModMenu {

enum class Kind { Resource, Legacy };

struct Entry {
    Kind kind;
    std::string id;
    std::string name; // UTF-8, not a GXT key or a format string.
    bool loaded;
    bool enabled;
    bool canToggle;
    bool pending;
};

struct Snapshot {
    std::vector<Entry> entries;
    unsigned loadedCount = 0;
    // An older Windows bridge may be active without an inventory export.
    bool resourceInventoryKnown = true;
};

Snapshot Read();
bool SetEnabled(const Entry& entry, bool enabled);

} // namespace ModMenu

// Implemented by the active resource backend when MODLOADER is compiled in.
bool ModLoader_GetMenuRows(std::vector<ModMenu::ResourceRow>& rows);
bool ModLoader_SetMenuEnabled(const std::string& id, bool enabled);
