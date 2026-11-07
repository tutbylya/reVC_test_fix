/*
 * Native reVC Mod Loader bridge.
 * Based on the MIT-licensed LINK/2012 integration ported by Cowboy-69:
 * https://github.com/x87/gta-extended-2025/commit/615202600c578fa840d4779170f77aba1b0f2219
 * See modloader_reVC.h for the original license and ABI.
 */
#include "modloader.h"

#if defined(MODLOADER) && defined(_WIN32)
#define WITHWINDOWS
#include "common.h"
#include "crossplatform.h"
#include "modloader_reVC.h"
#include "mod_menu.h"
#include "mod_menu_storage.h"
#include "Streaming.h"
#include "Directory.h"
#include "CdStream.h"
#include "Timer.h"
#include "FileLoader.h"
#include "FileMgr.h"
#include "TxdStore.h"
#include "main.h"
#if defined(RW_GL3) || defined(RW_WGPU)
#include "MoviePlayer.h"
#else
extern void PlayMovieInWindow(int, const char*);
#endif

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <utility>

extern RwUInt32 gGameState;
extern BOOL _gbCdStreamOverlapped, _gbCdStreamAsync;
extern DWORD _gdwCdStreamFlags;
extern HANDLE gCdStreamSema;
extern Queue gChannelRequestQ;
extern struct CdReadInfo* gpReadInfo;

namespace {
HMODULE hModLoader;
std::atomic<bool> callbacksActive{false};
modloader_reVC_t modloader_reVC_share = {};
std::string previousEnvironment;
bool environmentChanged, environmentExisted;
std::filesystem::path menuStatePath;

using VisitMods = int (__cdecl*)(void (__cdecl*)(void*, const char*, const char*, unsigned), void*);

struct MenuRows {
    std::vector<ModMenu::ResourceRow> rows;
    ModMenu::DisabledSet nextDisabled;
    bool stateReadable = false;
    bool failed = false;
};

void __cdecl ReceiveMenuRow(void* opaque, const char* id, const char* label, unsigned flags)
{
    auto& snapshot = *static_cast<MenuRows*>(opaque);
    if (snapshot.failed) return;
    if (!id || !label || (flags & ~7u) || snapshot.rows.size() >= 4096) {
        snapshot.failed = true;
        return;
    }
    // Keep exceptions inside the host CRT rather than crossing the DLL ABI.
    try {
        ModMenu::ResourceRow row;
        row.id = id;
        row.name = label;
        row.loaded = (flags & 1u) != 0;
        const bool blocked = (flags & 2u) != 0;
        const bool startupDisabled = (flags & 4u) != 0;
        const bool nextDisabled = snapshot.stateReadable
            ? snapshot.nextDisabled.count(row.id) != 0 : startupDisabled;
        row.manageable = !blocked && snapshot.stateReadable &&
            ModMenu::IsValidId(row.id) && ModMenu::NormalizeId(row.id) == row.id;
        row.enabled = !blocked && !nextDisabled;
        row.pending = row.manageable && startupDisabled != nextDisabled;
        snapshot.rows.push_back(std::move(row));
    } catch (...) {
        snapshot.failed = true;
    }
}

void OriginalMovie(int show, const char* filename)
{
#if defined(RW_GL3) || defined(RW_WGPU)
    (void)show;
    MoviePlayer::Play(filename);
#else
    PlayMovieInWindow(show, filename);
#endif
}

int OriginalLoadFile(const char* filename, uint8_t* buffer, int length, const char* mode)
{
    // The plugin ABI uses int even in x64; keep the game's ssize_t API intact.
    const ssize_t result = CFileMgr::LoadFile(filename, buffer, length, mode);
    return result >= INT_MIN && result <= INT_MAX ? (int)result : -1;
}

void* OriginalStreamOpen(int32_t type, int32_t access, const char* data)
{
    return RwStreamOpen((RwStreamType)type, (RwStreamAccessType)access, data);
}

void* OriginalAtomic(const char* filename)
{
    return CFileLoader::LoadAtomicFile2Return(filename);
}

bool DirectoryFind(void* directory, const char* name, uint32_t* offset, uint32_t* size)
{
    return directory && name && offset && size &&
        static_cast<CDirectory*>(directory)->FindItem(name, *offset, *size);
}

modloader_reVC_addr_table_t& OriginalFunctions()
{
    static modloader_reVC_addr_table_t table = [] {
        modloader_reVC_addr_table_t t = {};
        t.size = sizeof(t);
        t.p_gGameState = reinterpret_cast<int*>(&gGameState);
        t.PlayMovieInWindow = OriginalMovie;
        t.CFileMgr_OpenFile = &CFileMgr::OpenFile;
        t.CFileMgr_LoadFile = OriginalLoadFile;
        t.CTxdStore_LoadTxd = &CTxdStore::LoadTxd;
        t.CFileLoader_LoadAtomicFile2Return = OriginalAtomic;
        t.RwStreamOpen = OriginalStreamOpen;
        t.InitialiseGame = &InitialiseGame;
        t.ms_aInfoForModel = CStreaming::ms_aInfoForModel;
        t.uMaxResources = NUMSTREAMINFO;
        t.p_gCdStreamFlags = &_gdwCdStreamFlags;
        t.p_gCdStreamSema = &gCdStreamSema;
        t.p_gChannelRequestQ = &gChannelRequestQ;
        t.p_gpReadInfo = reinterpret_cast<void**>(&gpReadInfo);
        t.p_gbCdStreamOverlapped = &_gbCdStreamOverlapped;
        t.p_gbCdStreamAsync = &_gbCdStreamAsync;
        t.p_ms_pStreamingBuffer = reinterpret_cast<void**>(CStreaming::ms_pStreamingBuffer);
        t.p_ms_streamingBufferSize = reinterpret_cast<uint32_t*>(&CStreaming::ms_streamingBufferSize);
        t.CDirectory__FindItem4 = DirectoryFind;
        t.CStreaming__LoadCdDirectory0 = static_cast<void (*)()>(&CStreaming::LoadCdDirectory);
        t.CStreaming__LoadCdDirectory2 = static_cast<void (*)(const char*, int)>(&CStreaming::LoadCdDirectory);
        t.CdStreamRead = &CdStreamRead;
        t.LoadCdDirectoryUsingCallbacks = &CStreaming::LoadCdDirectoryUsingCallbacks;
        t.CTimer__Resume = &CTimer::Resume;
        t.CTimer__Suspend = &CTimer::Suspend;
        t.CStreaming__RequestModel = &CStreaming::RequestModel;
        t.CStreaming__RemoveModel = &CStreaming::RemoveModel;
        t.CStreaming__LoadAllRequestedModels = &CStreaming::LoadAllRequestedModels;
        t.CStreaming__FlushChannels = &CStreaming::FlushChannels;
        t.CStreaming__RemoveAllUnusedModels = &CStreaming::RemoveAllUnusedModels;
        t.CStreaming__RemoveLeastUsedModel2 = &CStreaming::RemoveLeastUsedModel;
        return t;
    }();
    return table;
}

bool FileNameIs(const char* path, const char* expected)
{
    if (!path) return false;
    const char* name = path;
    for (const char* p = path; *p; ++p)
        if (*p == '/' || *p == '\\') name = p + 1;
    return _stricmp(name, expected) == 0;
}

// Check PE metadata as read-only data, before DllMain can run. A stock GTA ASI
// may patch hard-coded executable addresses in DllMain and must not be executed
// just to discover that it lacks the reVC bootstrap export.
class ModuleImage
{
    HANDLE file = INVALID_HANDLE_VALUE, mapping = nullptr;
    const BYTE* data = nullptr;
    size_t length = 0;
public:
    explicit ModuleImage(const wchar_t* path)
    {
        file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        LARGE_INTEGER bytes = {};
        if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &bytes) ||
            bytes.QuadPart <= 0 || (uint64_t)bytes.QuadPart > (uint64_t)SIZE_MAX) return;
        length = (size_t)bytes.QuadPart;
        mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping) data = static_cast<const BYTE*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    }
    ~ModuleImage()
    {
        if (data) UnmapViewOfFile(data);
        if (mapping) CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
    template<typename T> bool Read(size_t offset, T& value) const
    {
        if (!data || offset > length || sizeof(T) > length - offset) return false;
        memcpy(&value, data + offset, sizeof(T));
        return true;
    }
    bool HasBootstrap() const
    {
        IMAGE_DOS_HEADER dos = {};
        DWORD signature = 0;
        IMAGE_FILE_HEADER coff = {};
        if (!Read(0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
            !Read((size_t)dos.e_lfanew, signature) || signature != IMAGE_NT_SIGNATURE ||
            !Read((size_t)dos.e_lfanew + sizeof(signature), coff) ||
            !(coff.Characteristics & IMAGE_FILE_DLL)) return false;
#if defined(_M_ARM64)
        const WORD machine = IMAGE_FILE_MACHINE_ARM64;
#elif defined(_WIN64)
        const WORD machine = IMAGE_FILE_MACHINE_AMD64;
#else
        const WORD machine = IMAGE_FILE_MACHINE_I386;
#endif
        if (coff.Machine != machine || coff.NumberOfSections == 0 || coff.NumberOfSections > 96) return false;
        const size_t optOffset = (size_t)dos.e_lfanew + sizeof(signature) + sizeof(coff);
        IMAGE_DATA_DIRECTORY exports = {};
        DWORD headerSize = 0;
#ifdef _WIN64
        IMAGE_OPTIONAL_HEADER64 optional = {};
        if (coff.SizeOfOptionalHeader < sizeof(optional) || !Read(optOffset, optional) ||
            optional.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || !optional.NumberOfRvaAndSizes) return false;
#else
        IMAGE_OPTIONAL_HEADER32 optional = {};
        if (coff.SizeOfOptionalHeader < sizeof(optional) || !Read(optOffset, optional) ||
            optional.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC || !optional.NumberOfRvaAndSizes) return false;
#endif
        exports = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        headerSize = optional.SizeOfHeaders;
        const size_t sections = optOffset + coff.SizeOfOptionalHeader;
        auto fileOffset = [&](DWORD rva, size_t bytes, size_t& offset) {
            if (rva < headerSize) {
                offset = rva;
                return offset <= length && bytes <= length - offset;
            }
            for (WORD i = 0; i < coff.NumberOfSections; ++i) {
                IMAGE_SECTION_HEADER section = {};
                if (!Read(sections + i * sizeof(section), section)) return false;
                if (rva < section.VirtualAddress) continue;
                const uint64_t within = (uint64_t)rva - section.VirtualAddress;
                if (within >= section.SizeOfRawData || bytes > section.SizeOfRawData - within) continue;
                const uint64_t raw = (uint64_t)section.PointerToRawData + within;
                if (raw > length || bytes > length - (size_t)raw) return false;
                offset = (size_t)raw;
                return true;
            }
            return false;
        };
        size_t directoryOffset = 0, namesOffset = 0, ordinalsOffset = 0, functionsOffset = 0;
        IMAGE_EXPORT_DIRECTORY directory = {};
        if (!exports.VirtualAddress || exports.Size < sizeof(directory) ||
            !fileOffset(exports.VirtualAddress, sizeof(directory), directoryOffset) ||
            !Read(directoryOffset, directory) || directory.NumberOfNames > length / sizeof(DWORD) ||
            directory.NumberOfFunctions > length / sizeof(DWORD) ||
            !fileOffset(directory.AddressOfNames, (size_t)directory.NumberOfNames * sizeof(DWORD), namesOffset) ||
            !fileOffset(directory.AddressOfNameOrdinals, (size_t)directory.NumberOfNames * sizeof(WORD), ordinalsOffset) ||
            !fileOffset(directory.AddressOfFunctions, (size_t)directory.NumberOfFunctions * sizeof(DWORD), functionsOffset)) return false;
        const char bootstrap[] = "modloader_InitFromREVC";
        for (DWORD i = 0; i < directory.NumberOfNames; ++i) {
            DWORD nameRva = 0, functionRva = 0;
            WORD ordinal = 0;
            size_t nameOffset = 0, codeOffset = 0;
            if (!Read(namesOffset + i * sizeof(DWORD), nameRva) ||
                !fileOffset(nameRva, sizeof(bootstrap), nameOffset) ||
                memcmp(data + nameOffset, bootstrap, sizeof(bootstrap)) != 0) continue;
            if (!Read(ordinalsOffset + i * sizeof(WORD), ordinal) || ordinal >= directory.NumberOfFunctions ||
                !Read(functionsOffset + ordinal * sizeof(DWORD), functionRva) || !functionRva) return false;
            // A forwarded bootstrap cannot establish the in-process reVC ABI.
            if ((uint64_t)functionRva >= exports.VirtualAddress &&
                (uint64_t)functionRva < (uint64_t)exports.VirtualAddress + exports.Size) return false;
            return fileOffset(functionRva, 1, codeOffset);
        }
        return false;
    }
};

void RestoreEnvironment()
{
    if (environmentChanged)
        SetEnvironmentVariableA("MODLOADER_REVC", environmentExisted ? previousEnvironment.c_str() : nullptr);
    environmentChanged = false;
}

void ResetLoader()
{
    callbacksActive.store(false, std::memory_order_release);
    modloader_reVC_share = {};
    if (hModLoader) FreeLibrary(hModLoader);
    hModLoader = nullptr;
    menuStatePath.clear();
    RestoreEnvironment();
}
} // namespace

#define ML_CALLBACK(member) \
    ((callbacksActive.load(std::memory_order_acquire) && modloader_reVC_share.callback_table && \
      modloader_reVC_share.callback_table->size >= offsetof(modloader_reVC_callback_table_t, member) + \
          sizeof(modloader_reVC_share.callback_table->member)) ? \
      modloader_reVC_share.callback_table->member : nullptr)

void ModLoader_Init()
{
    if (hModLoader) return;
    const DWORD required = GetFullPathNameW(L"modloader.asi", 0, nullptr, nullptr);
    if (!required) return;
    std::wstring path(required, L'\0');
    const DWORD written = GetFullPathNameW(L"modloader.asi", required, &path[0], nullptr);
    if (!written || written >= required) return;
    path.resize(written);
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    if (!ModuleImage(path.c_str()).HasBootstrap()) {
        fprintf(stderr, "[modloader] Ignoring incompatible ASI: expected a matching-architecture reVC build.\n");
        return;
    }
    menuStatePath = std::filesystem::path(path).parent_path() / L"modloader" / L".revc-disabled";

    const DWORD oldLength = GetEnvironmentVariableA("MODLOADER_REVC", nullptr, 0);
    environmentExisted = oldLength != 0;
    previousEnvironment.clear();
    if (oldLength) {
        previousEnvironment.resize(oldLength);
        const DWORD n = GetEnvironmentVariableA("MODLOADER_REVC", &previousEnvironment[0], oldLength);
        if (!n || n >= oldLength) return;
        previousEnvironment.resize(n);
    }
    if (!SetEnvironmentVariableA("MODLOADER_REVC", "1")) return;
    environmentChanged = true;
    hModLoader = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!hModLoader) {
        fprintf(stderr, "[modloader] Load failed (Windows error %lu).\n", GetLastError());
        ResetLoader();
        return;
    }
    auto init = reinterpret_cast<modloader_fInitFromREVC>(GetProcAddress(hModLoader, "modloader_InitFromREVC"));
    modloader_reVC_share = {};
    modloader_reVC_share.size = sizeof(modloader_reVC_share);
    modloader_reVC_share.reVC_version = 0;
    modloader_reVC_share.reVC_addr_table = &OriginalFunctions();
    if (!init || !init(&modloader_reVC_share)) {
        fprintf(stderr, "[modloader] The loader rejected this reVC interface.\n");
        ResetLoader();
        return;
    }
    callbacksActive.store(true, std::memory_order_release);
    if (!modloader_reVC_share.callback_table ||
        modloader_reVC_share.callback_table->size < sizeof(uint32_t) ||
        (ML_CALLBACK(CdStreamThread) &&
            (!ML_CALLBACK(StopCdStreamThread) || !ML_CALLBACK(PrepareCdStreamThread)))) {
        fprintf(stderr, "[modloader] Callback ABI is incompatible or lacks safe streaming shutdown.\n");
        ModLoader_Shutdown();
        return;
    }
    fprintf(stderr, "[modloader] Native reVC support enabled (%u-bit).\n", (unsigned)(sizeof(void*) * 8));
}

void ModLoader_Shutdown()
{
    if (!hModLoader) return;
    // Early WinMain failures may skip rsTERMINATE after streaming has started.
    // Join its worker while the plugin callbacks and code are still available.
    if (gpReadInfo) CdStreamShutdown();
    callbacksActive.store(false, std::memory_order_release);
    if (modloader_reVC_share.Shutdown) modloader_reVC_share.Shutdown(&modloader_reVC_share);
    ResetLoader();
}

void ModLoader_Tick()
{
    if (callbacksActive.load(std::memory_order_acquire) && modloader_reVC_share.Tick)
        modloader_reVC_share.Tick(&modloader_reVC_share);
}

bool ModLoader_GetMenuRows(std::vector<ModMenu::ResourceRow>& rows)
{
    rows.clear();
    if (!callbacksActive.load(std::memory_order_acquire) || !hModLoader) return true;
    const auto visit = reinterpret_cast<VisitMods>(GetProcAddress(hModLoader, "modloader_VisitReVCMods"));
    if (!visit) return false; // Old package: an unknown count is not zero.
    MenuRows snapshot;
    snapshot.stateReadable = !menuStatePath.empty() && ModMenu::ReadDisabled(menuStatePath, snapshot.nextDisabled);
    if (visit(ReceiveMenuRow, &snapshot) != 1 || snapshot.failed) return false;
    rows.swap(snapshot.rows);
    return true;
}

bool ModLoader_SetMenuEnabled(const std::string& id, bool enabled)
{
    if (menuStatePath.empty()) return false;
    std::vector<ModMenu::ResourceRow> rows;
    if (!ModLoader_GetMenuRows(rows)) return false;
    const auto row = std::find_if(rows.begin(), rows.end(), [&id](const ModMenu::ResourceRow& candidate) {
        return candidate.id == id && candidate.manageable;
    });
    return row != rows.end() && ModMenu::SetEnabled(menuStatePath, id, enabled);
}

bool ModLoader_StopCdStreamThread()
{
    auto stop = ML_CALLBACK(StopCdStreamThread);
    if (!stop) return false;
    stop();
    return true;
}

void ModLoader_PrepareCdStreamThread()
{
    auto prepare = ML_CALLBACK(PrepareCdStreamThread);
    if (prepare) prepare();
}

void
ModLoader_PlayMovieInWindow_Logo(int cmdshow, const char* filename)
{
    if (!ML_CALLBACK(PlayMovieInWindow_Logo)) return OriginalFunctions().PlayMovieInWindow(cmdshow, filename);

    return ML_CALLBACK(PlayMovieInWindow_Logo)(cmdshow, filename);
}

void ModLoader_PlayMovieInWindow_GTAtitles(int cmdshow, const char* filename)
{
    if (!ML_CALLBACK(PlayMovieInWindow_GTAtitles))
        return OriginalFunctions().PlayMovieInWindow(cmdshow, filename);

    return ML_CALLBACK(PlayMovieInWindow_GTAtitles)(cmdshow, filename);
}

int32_t
ModLoader_GtaDat(const char* file, const char* mode)
{
    if (!ML_CALLBACK(OpenFile_GtaDat))
        return OriginalFunctions().CFileMgr_OpenFile(file, mode);

    return ML_CALLBACK(OpenFile_GtaDat)(file, mode);
}

int32_t
ModLoader_DefaultDat(const char* file, const char* mode)
{
    if (!ML_CALLBACK(OpenFile_DefaultDat))
        return OriginalFunctions().CFileMgr_OpenFile(file, mode);

    return ML_CALLBACK(OpenFile_DefaultDat)(file, mode);
}

int32_t
ModLoader_CarcolsDat(const char* file, const char* mode)
{
    if (!ML_CALLBACK(OpenFile_CarcolsDat))
        return OriginalFunctions().CFileMgr_OpenFile(file, mode);

    return ML_CALLBACK(OpenFile_CarcolsDat)(file, mode);
}

int32_t ModLoader_PedGrpDat(const char* file, const char* mode)
{
    if (!ML_CALLBACK(OpenFile_PedGrpDat))
        return OriginalFunctions().CFileMgr_OpenFile(file, mode);

    return ML_CALLBACK(OpenFile_PedGrpDat)(file, mode);
}

int32_t ModLoader_MainScm(const char* file, const char* mode)
{
    if (!ML_CALLBACK(OpenFile_MainScm))
        return OriginalFunctions().CFileMgr_OpenFile(file, mode);

    return ML_CALLBACK(OpenFile_MainScm)(file, mode);
}

int32_t ModLoader_WaterproDat(const char* file, const char* mode)
{
    if (!ML_CALLBACK(OpenFile_WaterproDat))
        return OriginalFunctions().CFileMgr_OpenFile(file, mode);

    return ML_CALLBACK(OpenFile_WaterproDat)(file, mode);
}

int ModLoader_FistfiteDat(const char* file, uint8_t* buf, int maxlen, const char* mode)
{
    if (!ML_CALLBACK(LoadFile_FistfiteDat))
        return OriginalFunctions().CFileMgr_LoadFile(file, buf, maxlen, mode);

    return ML_CALLBACK(LoadFile_FistfiteDat)(file, buf, maxlen, mode);
}

int ModLoader_HandlingCfg(const char* file, uint8_t* buf, int maxlen, const char* mode)
{
    if (!ML_CALLBACK(LoadFile_HandlingCfg))
        return OriginalFunctions().CFileMgr_LoadFile(file, buf, maxlen, mode);

    return ML_CALLBACK(LoadFile_HandlingCfg)(file, buf, maxlen, mode);
}

int ModLoader_PedDat(const char* file, uint8_t* buf, int maxlen, const char* mode)
{
    if (!ML_CALLBACK(LoadFile_PedDat))
        return OriginalFunctions().CFileMgr_LoadFile(file, buf, maxlen, mode);

    return ML_CALLBACK(LoadFile_PedDat)(file, buf, maxlen, mode);
}

int ModLoader_ObjectDat(const char* file, uint8_t* buf, int maxlen, const char* mode)
{
    if (!ML_CALLBACK(LoadFile_ObjectDat))
        return OriginalFunctions().CFileMgr_LoadFile(file, buf, maxlen, mode);

    return ML_CALLBACK(LoadFile_ObjectDat)(file, buf, maxlen, mode);
}

int ModLoader_PedStatsDat(const char* file, uint8_t* buf, int maxlen, const char* mode)
{
    if (!ML_CALLBACK(LoadFile_PedStatsDat))
        return OriginalFunctions().CFileMgr_LoadFile(file, buf, maxlen, mode);

    return ML_CALLBACK(LoadFile_PedStatsDat)(file, buf, maxlen, mode);
}

int ModLoader_WeaponDat(const char* file, uint8_t* buf, int maxlen, const char* mode)
{
    if (!ML_CALLBACK(LoadFile_WeaponDat))
        return OriginalFunctions().CFileMgr_LoadFile(file, buf, maxlen, mode);

    return ML_CALLBACK(LoadFile_WeaponDat)(file, buf, maxlen, mode);
}

int ModLoader_ParticleCfg(const char* file, uint8_t* buf, int maxlen, const char* mode)
{
    if (!ML_CALLBACK(LoadFile_ParticleCfg))
        return OriginalFunctions().CFileMgr_LoadFile(file, buf, maxlen, mode);

    return ML_CALLBACK(LoadFile_ParticleCfg)(file, buf, maxlen, mode);
}

int ModLoader_SurfaceDat(const char* file, uint8_t* buf, int maxlen, const char* mode)
{
    if (!ML_CALLBACK(LoadFile_SurfaceDat))
        return OriginalFunctions().CFileMgr_LoadFile(file, buf, maxlen, mode);

    return ML_CALLBACK(LoadFile_SurfaceDat)(file, buf, maxlen, mode);
}

int ModLoader_TimecycleDat(const char* file, uint8_t* buf, int maxlen, const char* mode)
{
    if (!ML_CALLBACK(LoadFile_TimecycleDat))
        return OriginalFunctions().CFileMgr_LoadFile(file, buf, maxlen, mode);

    return ML_CALLBACK(LoadFile_TimecycleDat)(file, buf, maxlen, mode);
}

int ModLoader_FlightDat(const char* file, uint8_t* buf, int maxlen, const char* mode)
{
    if (FileNameIs(file, "flight.dat") && ML_CALLBACK(LoadFile_FlightDat))
        return ML_CALLBACK(LoadFile_FlightDat)(file, buf, maxlen, mode);
    if (FileNameIs(file, "flight2.dat") && ML_CALLBACK(LoadFile_Flight2Dat))
        return ML_CALLBACK(LoadFile_Flight2Dat)(file, buf, maxlen, mode);
    if (FileNameIs(file, "flight3.dat") && ML_CALLBACK(LoadFile_Flight3Dat))
        return ML_CALLBACK(LoadFile_Flight3Dat)(file, buf, maxlen, mode);
    return OriginalFunctions().CFileMgr_LoadFile(file, buf, maxlen, mode);
}

int ModLoader_TracksDat(const char* file, uint8_t* buf, int maxlen, const char* mode)
{
    if (FileNameIs(file, "tracks.dat") && ML_CALLBACK(LoadFile_TracksDat))
        return ML_CALLBACK(LoadFile_TracksDat)(file, buf, maxlen, mode);
    if (FileNameIs(file, "tracks2.dat") && ML_CALLBACK(LoadFile_Tracks2Dat))
        return ML_CALLBACK(LoadFile_Tracks2Dat)(file, buf, maxlen, mode);
    return OriginalFunctions().CFileMgr_LoadFile(file, buf, maxlen, mode);
}

bool ModLoader_FontsTxd(int slot, const char* file)
{
    if (FileNameIs(file, "fonts.txd") && ML_CALLBACK(LoadTxd_FontsTxd))
        return ML_CALLBACK(LoadTxd_FontsTxd)(slot, file);
    if (FileNameIs(file, "fonts_p.txd") && ML_CALLBACK(LoadTxd_FontsPTxd))
        return ML_CALLBACK(LoadTxd_FontsPTxd)(slot, file);
    if (FileNameIs(file, "fonts_r.txd") && ML_CALLBACK(LoadTxd_FontsRTxd))
        return ML_CALLBACK(LoadTxd_FontsRTxd)(slot, file);
    if (FileNameIs(file, "fonts_j.txd") && ML_CALLBACK(LoadTxd_FontsJTxd))
        return ML_CALLBACK(LoadTxd_FontsJTxd)(slot, file);
    return OriginalFunctions().CTxdStore_LoadTxd(slot, ModLoader_RegisterAndGetTexDiction_Unsafe(file));
}

bool ModLoader_Fronten1Txd(int slot, const char* file)
{
    if (!ML_CALLBACK(LoadTxd_Fronten1Txd))
        return OriginalFunctions().CTxdStore_LoadTxd(slot, file);

    return ML_CALLBACK(LoadTxd_Fronten1Txd)(slot, file);
}

bool ModLoader_HudTxd(int slot, const char* file)
{
    if (!ML_CALLBACK(LoadTxd_HudTxd))
        return OriginalFunctions().CTxdStore_LoadTxd(slot, file);

    return ML_CALLBACK(LoadTxd_HudTxd)(slot, file);
}

bool ModLoader_Fronten2Txd(int slot, const char* file)
{
    if (!ML_CALLBACK(LoadTxd_Fronten2Txd))
        return OriginalFunctions().CTxdStore_LoadTxd(slot, file);

    return ML_CALLBACK(LoadTxd_Fronten2Txd)(slot, file);
}

bool ModLoader_ParticleTxd(int slot, const char* file)
{
    if (!ML_CALLBACK(LoadTxd_ParticleTxd))
        return OriginalFunctions().CTxdStore_LoadTxd(slot, file);

    return ML_CALLBACK(LoadTxd_ParticleTxd)(slot, file);
}

void* ModLoader_ArrowDff(const char* file)
{
    if (!ML_CALLBACK(LoadAtomic2Return_ArrowDff))
        return OriginalFunctions().CFileLoader_LoadAtomicFile2Return(file);

    return ML_CALLBACK(LoadAtomic2Return_ArrowDff)(file);
}

void* ModLoader_ZonecylbDff(const char* file)
{
    if (!ML_CALLBACK(LoadAtomic2Return_ZonecylbDff))
        return OriginalFunctions().CFileLoader_LoadAtomicFile2Return(file);

    return ML_CALLBACK(LoadAtomic2Return_ZonecylbDff)(file);
}

void* ModLoader_AnimFile(int32_t type, int32_t accessType, const char* pData)
{
    if (!ML_CALLBACK(RwStreamOpen_AnimFile))
        return OriginalFunctions().RwStreamOpen(type, accessType, pData);

    return ML_CALLBACK(RwStreamOpen_AnimFile)(type, accessType, pData);
}

void ModLoader_InitialiseGame()
{
    if (!ML_CALLBACK(InitialiseGame))
        return OriginalFunctions().InitialiseGame();

    return ML_CALLBACK(InitialiseGame)();
}


bool
ModLoader_CdStreamThread()
{
    if (ML_CALLBACK(CdStreamThread)) {
        ML_CALLBACK(CdStreamThread)();
        return true;
    }
    return false;
}


void*
ModLoader_AcquireNextModelFileHandle()
{
    if (ML_CALLBACK(AcquireNextModelFileHandle))
        return ML_CALLBACK(AcquireNextModelFileHandle)();
    return INVALID_HANDLE_VALUE;
}

void
ModLoader_LoadCdDirectory()
{
    if (ML_CALLBACK(LoadCdDirectory0))
        return ML_CALLBACK(LoadCdDirectory0)();
    return OriginalFunctions().CStreaming__LoadCdDirectory0();
}

void
ModLoader_FetchCdDirectory(const char* filename, int cd_index)
{
    if (ML_CALLBACK(FetchCdDirectory))
        return ML_CALLBACK(FetchCdDirectory)(filename, cd_index);
    return OriginalFunctions().CStreaming__LoadCdDirectory2(filename, cd_index);
}

void
ModLoader_RegisterNextModelRead(int resource_id)
{
    if (ML_CALLBACK(RegisterNextModelRead))
        return ML_CALLBACK(RegisterNextModelRead)(resource_id);
}

int32_t
ModLoader_CdStreamRead(int32_t channel, void* buffer, uint32_t offset, uint32_t size)
{
    if (ML_CALLBACK(CdStreamRead))
        return ML_CALLBACK(CdStreamRead)(channel, buffer, offset, size);
    return OriginalFunctions().CdStreamRead(channel, buffer, offset, size);
}

void
ModLoader_OnRequestSpecialModel(uint32_t model_id, const char* model_name, uint32_t pos, uint32_t size)
{
    if (ML_CALLBACK(OnRequestSpecialModel))
        return ML_CALLBACK(OnRequestSpecialModel)(model_id, model_name, pos, size);
}

const char*
ModLoader_GetCdStreamPath_Unsafe(const char* filepath)
{
    auto callback = ML_CALLBACK(GetCdStreamPath_Unsafe);
    const char* resolved = callback ? callback(filepath) : nullptr;
    return resolved && *resolved ? resolved : filepath;
}

const char*
ModLoader_GetCdDirectoryPath_Unsafe(const char* filepath)
{
    auto callback = ML_CALLBACK(GetCdDirectoryPath_Unsafe);
    const char* resolved = callback ? callback(filepath) : nullptr;
    return resolved && *resolved ? resolved : filepath;
}

const char*
ModLoader_RegisterAndGetColFile_Unsafe(const char* filepath)
{
    auto callback = ML_CALLBACK(RegisterAndGetColFile_Unsafe);
    const char* resolved = callback ? callback(filepath) : nullptr;
    return resolved && *resolved ? resolved : filepath;
}

const char*
ModLoader_RegisterAndGetAtomicFile_Unsafe(const char* filepath)
{
    auto callback = ML_CALLBACK(RegisterAndGetAtomicFile_Unsafe);
    const char* resolved = callback ? callback(filepath) : nullptr;
    return resolved && *resolved ? resolved : filepath;
}

const char*
ModLoader_RegisterAndGetClumpFile_Unsafe(const char* filepath)
{
    auto callback = ML_CALLBACK(RegisterAndGetClumpFile_Unsafe);
    const char* resolved = callback ? callback(filepath) : nullptr;
    return resolved && *resolved ? resolved : filepath;
}

const char*
ModLoader_RegisterAndGetTexDiction_Unsafe(const char* filepath)
{
    auto callback = ML_CALLBACK(RegisterAndGetTexDiction_Unsafe);
    const char* resolved = callback ? callback(filepath) : nullptr;
    return resolved && *resolved ? resolved : filepath;
}

const char*
ModLoader_RegisterAndGetIdeFile_Unsafe(const char* filepath)
{
    auto callback = ML_CALLBACK(RegisterAndGetIdeFile_Unsafe);
    const char* resolved = callback ? callback(filepath) : nullptr;
    return resolved && *resolved ? resolved : filepath;
}

const char*
ModLoader_RegisterAndGetIplFile_Unsafe(const char* filepath)
{
    auto callback = ML_CALLBACK(RegisterAndGetIplFile_Unsafe);
    const char* resolved = callback ? callback(filepath) : nullptr;
    return resolved && *resolved ? resolved : filepath;
}

const char* ModLoader_RegisterAndGetGxtFile_Unsafe(const char* filepath)
{
    auto callback = ML_CALLBACK(RegisterAndGetGxtFile_Unsafe);
    const char* resolved = callback ? callback(filepath) : nullptr;
    return resolved && *resolved ? resolved : filepath;
}

const char* ModLoader_RegisterAndGetPlayerBmpFile_Unsafe(const char* filepath)
{
    auto callback = ML_CALLBACK(RegisterAndGetPlayerBmpFile_Unsafe);
    const char* resolved = callback ? callback(filepath) : nullptr;
    return resolved && *resolved ? resolved : filepath;
}

const char* ModLoader_RegisterAndGetSplashFile_Unsafe(const char* filepath)
{
    auto callback = ML_CALLBACK(RegisterAndGetSplashFile_Unsafe);
    const char* resolved = callback ? callback(filepath) : nullptr;
    return resolved && *resolved ? resolved : filepath;
}

const char* ModLoader_GetAnimFile_Unsafe(const char* filepath)
{
    auto callback = ML_CALLBACK(GetAnimFile_Unsafe);
    const char* resolved = callback ? callback(filepath) : nullptr;
    return resolved && *resolved ? resolved : filepath;
}

#undef ML_CALLBACK
#endif // MODLOADER

