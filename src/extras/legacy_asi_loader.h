#pragma once

#if defined(REVC_LEGACY_ASI) && defined(_WIN32) && defined(_MSC_VER) && defined(_M_IX86)

#include <string>
#include <vector>

namespace ModMenu { struct Entry; }

namespace LegacyAsi {

// Startup/main-thread API. Only explicitly listed, SHA-256 pinned modules in
// <absoluteGameDirectory>/legacy-asi/enabled.ini may execute. All entries are
// checked before the first DLL is loaded. Missing/empty manifests enable none.
// This function is one-shot: subsequent calls never discover or reload ASIs.
bool LoadEnabledModules(const wchar_t* absoluteGameDirectory);
bool HaveLoadedModules();
// A failed first DllMain may already have installed hooks or started a thread.
// Retain the ABI arena on failure once execution was attempted, even if no DLL
// ultimately remained loaded. This is not equivalent to HaveLoadedModules().
bool ModuleExecutionAttempted();
unsigned LoadedModuleCount();
const char* LoaderError();
void AppendModMenuEntries(std::vector<ModMenu::Entry>& entries);
bool SetModMenuEnabled(const std::string& id, bool enabled);

// Successfully loaded ASIs and their read-locked source files intentionally
// remain alive until process exit. There is no unsafe generic unload API for
// arbitrary DLL-installed hooks or threads. A false return requires startup to
// abort, including a partial load failure; it is not a recoverable hot reload.

} // namespace LegacyAsi

#endif
