// Native Linux resource-folder backend. No PE loading, injection or Windows ABI.
#include "modloader.h"

#if defined(MODLOADER) && defined(__linux__)
#include "common.h"
#include "modloader_folder.h"
#include "CdStream.h"
#include "Directory.h"
#include "FileMgr.h"
#include "FileLoader.h"
#include "MoviePlayer.h"
#include "Streaming.h"
#include "TxdStore.h"
#include "main.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>

namespace {
struct Replacement {
    std::string path;
    uint32 stockPosition, stockSize;
    bool available = true;
};

struct NativeState {
    ModLoaderFolder::Index index;
    std::map<int, Replacement> models;
    std::map<std::string, Replacement> special;
    bool initialized = false;
    int nextModel = -1;
};

NativeState& State()
{
    static NativeState state;
    return state;
}

std::string Lower(std::string value)
{
    for (char& c : value)
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
}

bool Exists(const std::string& path)
{
    // Revalidate every ancestor too: a mod directory may have been replaced
    // with a symlink since the immutable startup scan.
    const char* current = State().index.Resolve(path.c_str());
    return current && path == current;
}

const char* Resolve(const char* path)
{
    if (State().initialized) {
        const char* replacement = State().index.Resolve(path);
        if (replacement) return replacement;
    }
    return path;
}

const char* ReadPath(const char* path, const char* mode)
{
    // Generated water data and other writes must never overwrite a mod.
    return mode && mode[0] == 'r' && !strchr(mode, '+') ? Resolve(path) : path;
}

struct EntryContext {
    CDirectory::DirectoryInfo entry;
    const ModLoaderFolder::File* file;
    bool consumed = false;
};

bool ReadEntry(void* user, void* entry, uint32_t bytes)
{
    auto& context = *static_cast<EntryContext*>(user);
    if (context.consumed || bytes != sizeof(context.entry)) return false;
    memcpy(entry, &context.entry, bytes);
    context.consumed = true;
    return true;
}

bool RegisterEntry(void* user, void* model, bool)
{
    auto& context = *static_cast<EntryContext*>(user);
    auto* info = static_cast<CStreamingInfo*>(model);
    const auto id = info - CStreaming::ms_aInfoForModel;
    if (id < 0 || id >= NUMSTREAMINFO) return true;
    State().models[(int)id] = {context.file->path, info->m_position, info->m_size};
    info->m_nextID = -1;
    return false;
}

void RegisterSpecialEntry(void* user, void* entry)
{
    auto& context = *static_cast<EntryContext*>(user);
    auto& directoryEntry = *static_cast<CDirectory::DirectoryInfo*>(entry);
    Replacement replacement{context.file->path, 0, 0};
    auto* directory = CStreaming::ms_pExtraObjectsDir;
    if (directory) {
        directory->FindItem(directoryEntry.name, replacement.stockPosition, replacement.stockSize);
        for (int i = 0; i < directory->numEntries; ++i) {
            if (strcasecmp(directory->entries[i].name, directoryEntry.name) == 0) {
                directory->entries[i].offset = directoryEntry.offset;
                directory->entries[i].size = directoryEntry.size;
                break;
            }
        }
    }
    State().special[Lower(directoryEntry.name)] = std::move(replacement);
}

void IndexLooseResources()
{
    auto& state = State();
    for (const auto& item : state.index.FilesByName()) {
        const auto dot = item.first.rfind('.');
        if (dot == std::string::npos) continue;
        const std::string extension = item.first.substr(dot);
        if (extension != ".dff" && extension != ".txd" && extension != ".col" && extension != ".ifp") continue;
        const uint64_t sectors = (item.second.size + CDSTREAM_SECTOR_SIZE - 1) / CDSTREAM_SECTOR_SIZE;
        if (dot > 20 || item.first.size() >= sizeof(CDirectory::DirectoryInfo::name) || !sectors ||
            sectors > INT_MAX / CDSTREAM_SECTOR_SIZE) {
            fprintf(stderr, "[modloader] Unsupported loose resource name/size: %s\n", item.second.path.c_str());
            continue;
        }
        EntryContext context{};
        context.file = &item.second;
        context.entry.offset = 0;
        context.entry.size = (uint32)sectors;
        memcpy(context.entry.name, item.first.c_str(), item.first.size() + 1);
        // Each loose resource is its own read, not a contiguous IMG batch.
        CStreaming::LoadCdDirectoryUsingCallbacks(&context, 0, ReadEntry, RegisterEntry, RegisterSpecialEntry);
    }
    for (int i = 0; i < NUMSTREAMINFO; ++i) {
        auto& info = CStreaming::ms_aInfoForModel[i];
        if (state.models.count(i) || state.models.count(info.m_nextID)) info.m_nextID = -1;
    }
    fprintf(stderr, "[modloader] Linux: %zu loose streaming replacements, %zu special models.\n",
        state.models.size(), state.special.size());
}
} // namespace

void ModLoader_Init()
{
    auto& state = State();
    if (state.initialized) return;
    std::error_code ec;
    const auto root = std::filesystem::current_path(ec);
    if (ec) return;
    state.initialized = state.index.Scan(root.string());
}

void ModLoader_Shutdown()
{
    auto& state = State();
    if (!state.initialized) return;
    // Includes early WinMain/main failures; POSIX shutdown is idempotent.
    CdStreamShutdown();
    state.nextModel = -1;
    state.models.clear();
    state.special.clear();
    state.index.Clear();
    state.initialized = false;
}

// Immutable startup index: install/remove mods and change profiles with the
// game closed. Avoid reallocating path storage while async reads are running.
void ModLoader_Tick() {}
void ModLoader_InitialiseGame() { InitialiseGame(); }
void ModLoader_PlayMovieInWindow_Logo(int, const char* file) { MoviePlayer::Play(Resolve(file)); }
void ModLoader_PlayMovieInWindow_GTAtitles(int, const char* file) { MoviePlayer::Play(Resolve(file)); }

#define NATIVE_OPEN(name) \
    int32_t name(const char* file, const char* mode) { return CFileMgr::OpenFile(ReadPath(file, mode), mode); }
NATIVE_OPEN(ModLoader_GtaDat)
NATIVE_OPEN(ModLoader_DefaultDat)
NATIVE_OPEN(ModLoader_CarcolsDat)
NATIVE_OPEN(ModLoader_PedGrpDat)
NATIVE_OPEN(ModLoader_MainScm)
NATIVE_OPEN(ModLoader_WaterproDat)
#undef NATIVE_OPEN

#define NATIVE_LOAD(name) \
    int name(const char* file, uint8_t* buffer, int bytes, const char* mode) { \
        const auto result = CFileMgr::LoadFile(ReadPath(file, mode), buffer, bytes, mode); \
        return result >= INT_MIN && result <= INT_MAX ? (int)result : -1; }
NATIVE_LOAD(ModLoader_FistfiteDat)
NATIVE_LOAD(ModLoader_HandlingCfg)
NATIVE_LOAD(ModLoader_PedDat)
NATIVE_LOAD(ModLoader_ObjectDat)
NATIVE_LOAD(ModLoader_PedStatsDat)
NATIVE_LOAD(ModLoader_WeaponDat)
NATIVE_LOAD(ModLoader_ParticleCfg)
NATIVE_LOAD(ModLoader_SurfaceDat)
NATIVE_LOAD(ModLoader_TimecycleDat)
NATIVE_LOAD(ModLoader_FlightDat)
NATIVE_LOAD(ModLoader_TracksDat)
#undef NATIVE_LOAD

#define NATIVE_TXD(name) \
    bool name(int slot, const char* file) { return CTxdStore::LoadTxd(slot, Resolve(file)); }
NATIVE_TXD(ModLoader_FontsTxd)
NATIVE_TXD(ModLoader_Fronten1Txd)
NATIVE_TXD(ModLoader_HudTxd)
NATIVE_TXD(ModLoader_Fronten2Txd)
NATIVE_TXD(ModLoader_ParticleTxd)
#undef NATIVE_TXD

void* ModLoader_ArrowDff(const char* file) { return CFileLoader::LoadAtomicFile2Return(Resolve(file)); }
void* ModLoader_ZonecylbDff(const char* file) { return CFileLoader::LoadAtomicFile2Return(Resolve(file)); }
void* ModLoader_AnimFile(int32_t type, int32_t access, const char* data)
{
    return RwStreamOpen((RwStreamType)type, (RwStreamAccessType)access,
        type == rwSTREAMFILENAME ? Resolve(data) : data);
}

bool ModLoader_CdStreamThread() { return false; }
bool ModLoader_StopCdStreamThread() { return false; }
void ModLoader_PrepareCdStreamThread() {}
void* ModLoader_AcquireNextModelFileHandle() { return nullptr; }

void ModLoader_LoadCdDirectory()
{
    State().models.clear();
    State().special.clear();
    State().nextModel = -1;
    CStreaming::LoadCdDirectory();
    if (State().initialized) IndexLooseResources();
}

void ModLoader_FetchCdDirectory(const char* path, int index) { CStreaming::LoadCdDirectory(path, index); }

void ModLoader_RegisterNextModelRead(int id)
{
    auto& state = State();
    state.nextModel = id;
    const auto replacement = state.models.find(id);
    if (replacement == state.models.end() || !replacement->second.available || Exists(replacement->second.path)) return;
    auto& info = CStreaming::ms_aInfoForModel[id];
    info.SetCdPosnAndSize(replacement->second.stockPosition, replacement->second.stockSize);
    info.m_nextID = -1;
    replacement->second.available = false;
    fprintf(stderr, "[modloader] Loose resource disappeared; restored stock model %d.\n", id);
}

int32_t ModLoader_CdStreamRead(int32_t channel, void* buffer, uint32_t offset, uint32_t sectors)
{
    auto& state = State();
    const auto replacement = state.models.find(state.nextModel);
    state.nextModel = -1;
    if (replacement != state.models.end()) {
        if (replacement->second.available || replacement->second.stockSize == 0)
            return CdStreamReadFile(channel, replacement->second.path.c_str(), buffer, 0, sectors);
        // A failed loose read may be retried with the previous channel size
        // and position; use the restored stock metadata, not stale arguments.
        return CdStreamRead(channel, buffer, replacement->second.stockPosition +
            CStreaming::GetCdImageOffset(CdStreamGetLastPosn()), replacement->second.stockSize);
    }
    return CdStreamRead(channel, buffer, offset, sectors);
}

void ModLoader_OnRequestSpecialModel(uint32_t id, const char* name, uint32_t, uint32_t)
{
    if (id >= NUMSTREAMINFO || !name) return;
    const auto replacement = State().special.find(Lower(name));
    if (replacement == State().special.end()) return;
    State().models[(int)id] = replacement->second;
    CStreaming::ms_aInfoForModel[id].m_nextID = -1;
}

#define NATIVE_PATH(name) const char* name(const char* path) { return Resolve(path); }
NATIVE_PATH(ModLoader_GetCdStreamPath_Unsafe)
NATIVE_PATH(ModLoader_GetCdDirectoryPath_Unsafe)
NATIVE_PATH(ModLoader_RegisterAndGetColFile_Unsafe)
NATIVE_PATH(ModLoader_RegisterAndGetAtomicFile_Unsafe)
NATIVE_PATH(ModLoader_RegisterAndGetClumpFile_Unsafe)
NATIVE_PATH(ModLoader_RegisterAndGetTexDiction_Unsafe)
NATIVE_PATH(ModLoader_RegisterAndGetIdeFile_Unsafe)
NATIVE_PATH(ModLoader_RegisterAndGetIplFile_Unsafe)
NATIVE_PATH(ModLoader_RegisterAndGetGxtFile_Unsafe)
NATIVE_PATH(ModLoader_RegisterAndGetPlayerBmpFile_Unsafe)
NATIVE_PATH(ModLoader_RegisterAndGetSplashFile_Unsafe)
NATIVE_PATH(ModLoader_GetAnimFile_Unsafe)
#undef NATIVE_PATH
#endif
