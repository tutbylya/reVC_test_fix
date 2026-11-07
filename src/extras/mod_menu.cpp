#include "common.h"
#include "mod_menu.h"
#include "legacy_asi_loader.h"

#include <utility>

namespace ModMenu {

Snapshot Read()
{
    Snapshot snapshot;
#ifdef MODLOADER
    std::vector<ResourceRow> resources;
    snapshot.resourceInventoryKnown = ModLoader_GetMenuRows(resources);
    for (auto& row : resources) {
        snapshot.entries.push_back({Kind::Resource, std::move(row.id), std::move(row.name),
                                    row.loaded, row.enabled, row.manageable, row.pending});
    }
#endif
#if defined(REVC_LEGACY_ASI) && defined(_WIN32) && defined(_MSC_VER) && defined(_M_IX86)
    LegacyAsi::AppendModMenuEntries(snapshot.entries);
#endif
    for (const auto& entry : snapshot.entries)
        if (entry.loaded) ++snapshot.loadedCount;
    return snapshot;
}

bool SetEnabled(const Entry& entry, bool enabled)
{
    if (!entry.canToggle) return false;
    (void)enabled; // Both loaders are optional at build time.
    switch (entry.kind) {
    case Kind::Resource:
#ifdef MODLOADER
        return ModLoader_SetMenuEnabled(entry.id, enabled);
#else
        return false;
#endif
    case Kind::Legacy:
#if defined(REVC_LEGACY_ASI) && defined(_WIN32) && defined(_MSC_VER) && defined(_M_IX86)
        return LegacyAsi::SetModMenuEnabled(entry.id, enabled);
#else
        return false;
#endif
    }
    return false;
}

} // namespace ModMenu
