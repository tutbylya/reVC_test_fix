#include "legacy_asi.h"

#ifdef REVC_LEGACY_ASI
#define WITHWINDOWS
#include "common.h"
#include "crossplatform.h"
#include "Clock.h"
#include "Camera.h"
#include "CutsceneMgr.h"
#include "Draw.h"
#include "Game.h"
#include "Hud.h"
#include "Timer.h"
#include "Weather.h"
#include "legacy_asi_loader.h"
#include "legacy_asi_memory.h"
#include "legacy_asi_objects.h"
#include "legacy_asi_rw.h"
#include "legacy_asi_input.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <type_traits>
#include <utility>

extern void Render2dStuff();

namespace LegacyAsi {
namespace {

static_assert(sizeof(bool) == 1 && sizeof(float) == 4 && sizeof(uint32) == 4,
    "The scalar facade requires the original Win32 scalar widths");

// Original Vice City 1.0 EN addresses, verified against Plugin-SDK's
// plugin_vc/game_vc/{CTimer,CClock,CWeather,CDraw,CHud}.cpp and shared/Events.h.
// No current game object is aliased to an original camera/entity/RW struct.
constexpr std::uintptr_t InitRwEvent = 0x4A5B6B;
constexpr std::uintptr_t ShutdownRwEvent = 0x4A5B8E;
constexpr std::uintptr_t ProcessEvent = 0x4A5DA0;
constexpr std::uintptr_t DrawingEvent = 0x4A61D6;
constexpr std::uintptr_t HudEvent = 0x4A64D0;
bool ready;
bool attempted;
bool initializedSuccessfully;
bool reserved;

template<class T> bool ValidScalar(std::uintptr_t address, T value)
{
    if constexpr (std::is_same_v<T, float>) {
        if (!std::isfinite(value)) return false;
        switch (address) {
        case 0x97F264: return value >= 0.0f && value <= 100.0f;
        case 0x975424: case 0x9753A8: return value >= 0.0f && value <= 1000.0f;
        case 0x696658: return value > 1.0f && value < 179.0f;
        case 0x94DD38: return value > 0.0f && value <= 32.0f;
        case 0x978534: case 0xA10678: return value > 0.0f;
        case 0x97F274: return value >= 0.0f;
        case 0x94DBD0: case 0xA0D964: return value >= 0.0f && value <= 32.0f;
        }
    } else if constexpr (std::is_same_v<T, uint32>) {
        if (address == 0x97F2B4) return value != 0; // CClock::Update divides by this.
    } else if constexpr (std::is_same_v<T, uint8>) {
        if (address == 0xA10B6B || address == 0xA10B33) return value < 24;
        if (address == 0xA10B92 || address == 0xA10B5D) return value < 60;
    } else if constexpr (std::is_same_v<T, uint16>) {
        if (address == 0xA10A3C || address == 0xA10A5E) return value < 60;
    } else if constexpr (std::is_same_v<T, int16>) {
        if (address == 0xA10AAA || address == 0xA10A2E) return value >= 0 && value < WEATHER_TOTAL;
        if (address == 0xA10A42) return value >= WEATHER_RANDOM && value < WEATHER_TOTAL;
    }
    return true;
}

template<class T> bool Decode(const void* bytes, T& value)
{
    if constexpr (std::is_same_v<T, bool>) {
        const unsigned char raw = *static_cast<const unsigned char*>(bytes);
        if (raw > 1) return false;
        value = raw != 0;
    } else {
        std::memcpy(&value, bytes, sizeof(value));
        if constexpr (std::is_floating_point_v<T>)
            if (!std::isfinite(value)) return false;
    }
    return true;
}

struct GlobalField {
    const char* name;
    std::uintptr_t address;
    std::size_t size;
    void (*read)(void*);
    bool (*write)(const void*);
    unsigned char previous[4];
};

// Copy back only bytes changed by a plugin. An unchanged mirror must never
// overwrite newer native state (including an API call made by that plugin).
#define FIELD(Type, Name, Address, Getter, Setter) \
    {Name, Address, sizeof(Type), \
     [](void* bytes) { const Type value = Getter; std::memcpy(bytes, &value, sizeof(value)); }, \
     [](const void* bytes) { Type value; if (!Decode(bytes, value) || !ValidScalar(Address, value)) return false; Setter; return true; }, {}}
GlobalField globals[] = {
    FIELD(uint32, "CTimer::timeMs", 0x974B2C, CTimer::GetTimeInMilliseconds(), CTimer::SetTimeInMilliseconds(value)),
    FIELD(uint32, "CTimer::pauseMs", 0x691014, CTimer::GetTimeInMillisecondsPauseMode(), CTimer::SetTimeInMillisecondsPauseMode(value)),
    FIELD(uint32, "CTimer::previousMs", 0x978D8C, CTimer::GetPreviousTimeInMilliseconds(), CTimer::SetPreviousTimeInMilliseconds(value)),
    FIELD(uint32, "CTimer::nonClippedMs", 0xA0D894, CTimer::GetTimeInMillisecondsNonClipped(), CTimer::SetTimeInMillisecondsNonClipped(value)),
    FIELD(float, "CTimer::timeScale", 0x97F264, CTimer::GetTimeScale(), CTimer::SetTimeScale(value)),
    FIELD(float, "CTimer::timeStep", 0x975424, CTimer::GetTimeStep(), CTimer::SetTimeStep(value)),
    FIELD(float, "CTimer::timeStepNonClipped", 0x9753A8, CTimer::GetTimeStepNonClipped(), CTimer::SetTimeStepNonClipped(value)),
    FIELD(uint32, "CTimer::frameCounter", 0xA0D898, CTimer::GetFrameCounter(), CTimer::SetFrameCounter(value)),
    FIELD(bool, "CTimer::userPause", 0xA10B36, CTimer::m_UserPause, CTimer::m_UserPause = value),
    FIELD(bool, "CTimer::codePause", 0xA10B76, CTimer::m_CodePause, CTimer::SetCodePause(value)),
    FIELD(uint32, "CClock::msPerMinute", 0x97F2B4, CClock::ms_nMillisecondsPerGameMinute, CClock::ms_nMillisecondsPerGameMinute = value),
    FIELD(uint32, "CClock::lastTick", 0xA0FDEC, CClock::ms_nLastClockTick, CClock::ms_nLastClockTick = value),
    FIELD(uint16, "CClock::seconds", 0xA10A3C, CClock::ms_nGameClockSeconds, CClock::ms_nGameClockSeconds = value),
    FIELD(uint16, "CClock::storedSeconds", 0xA10A5E, CClock::ms_Stored_nGameClockSeconds, CClock::ms_Stored_nGameClockSeconds = value),
    FIELD(uint8, "CClock::storedHours", 0xA10B33, CClock::ms_Stored_nGameClockHours, CClock::ms_Stored_nGameClockHours = value),
    FIELD(uint8, "CClock::storedMinutes", 0xA10B5D, CClock::ms_Stored_nGameClockMinutes, CClock::ms_Stored_nGameClockMinutes = value),
    FIELD(uint8, "CClock::hours", 0xA10B6B, CClock::ms_nGameClockHours, CClock::ms_nGameClockHours = value),
    FIELD(uint8, "CClock::minutes", 0xA10B92, CClock::ms_nGameClockMinutes, CClock::ms_nGameClockMinutes = value),
    FIELD(bool, "CClock::hasStored", 0xA10B3C, CClock::ms_bClockHasBeenStored, CClock::ms_bClockHasBeenStored = value),
    FIELD(int16, "CWeather::old", 0xA10AAA, CWeather::OldWeatherType, CWeather::OldWeatherType = value),
    FIELD(int16, "CWeather::new", 0xA10A2E, CWeather::NewWeatherType, CWeather::NewWeatherType = value),
    FIELD(int16, "CWeather::forced", 0xA10A42, CWeather::ForcedWeatherType, CWeather::ForcedWeatherType = value),
    FIELD(bool, "CWeather::lightningFlash", 0xA10B67, CWeather::LightningFlash, CWeather::LightningFlash = value),
    FIELD(bool, "CWeather::lightningBurst", 0xA10B72, CWeather::LightningBurst, CWeather::LightningBurst = value),
    FIELD(uint32, "CWeather::whenSound", 0x9B5EAC, CWeather::WhenToPlayLightningSound, CWeather::WhenToPlayLightningSound = value),
    FIELD(uint32, "CWeather::flashLastChange", 0x975350, CWeather::LightningFlashLastChange, CWeather::LightningFlashLastChange = value),
    FIELD(uint32, "CWeather::lightningStart", 0x9B5F84, CWeather::LightningStart, CWeather::LightningStart = value),
    FIELD(int32, "CWeather::soundHandle", 0x699EDC, CWeather::SoundHandle, CWeather::SoundHandle = value),
    FIELD(int32, "CWeather::streamAfterRain", 0x699EE0, CWeather::StreamAfterRainTimer, CWeather::StreamAfterRainTimer = value),
    FIELD(float, "CDraw::FOV", 0x696658, CDraw::GetFOV(), CDraw::SetFOV(value)),
    FIELD(float, "CDraw::aspect", 0x94DD38, CDraw::GetAspectRatio(), CDraw::SetAspectRatio(value)),
    FIELD(float, "CDraw::nearClip", 0x978534, CDraw::GetNearClipZ(), CDraw::SetNearClipZ(value)),
    FIELD(float, "CDraw::farClip", 0xA10678, CDraw::GetFarClipZ(), CDraw::SetFarClipZ(value)),
    FIELD(float, "CDraw::LOD", 0x97F274, CDraw::ms_fLODDistance, CDraw::ms_fLODDistance = value),
    FIELD(uint8, "CDraw::fadeR", 0xA10B4B, CDraw::FadeRed, CDraw::FadeRed = value),
    FIELD(uint8, "CDraw::fadeG", 0xA10B21, CDraw::FadeGreen, CDraw::FadeGreen = value),
    FIELD(uint8, "CDraw::fadeB", 0xA10AED, CDraw::FadeBlue, CDraw::FadeBlue = value),
    FIELD(uint8, "CDraw::fadeValue", 0xA10B16, CDraw::FadeValue, CDraw::FadeValue = value),
    FIELD(bool, "CHud::wantsHUD", 0xA10B45, CHud::m_Wants_To_Draw_Hud, CHud::m_Wants_To_Draw_Hud = value),
    FIELD(bool, "CHud::wants3DMarkers", 0xA10AFC, CHud::m_Wants_To_Draw_3dMarkers, CHud::m_Wants_To_Draw_3dMarkers = value),
    FIELD(bool, "CHud::helpQuick", 0xA10AB4, CHud::m_HelpMessageQuick, CHud::m_HelpMessageQuick = value),
    FIELD(bool, "CHud::helpForever", 0xA10B17, CHud::m_HelpMessageDisplayForever, CHud::m_HelpMessageDisplayForever = value),
    FIELD(int16, "CHud::itemFlash", 0xA10A44, CHud::m_ItemToFlash, CHud::m_ItemToFlash = value),
    FIELD(float, "CCamera::mouseAccelH", 0x94DBD0, CCamera::m_fMouseAccelHorzntl, CCamera::m_fMouseAccelHorzntl = value),
    FIELD(float, "CCamera::mouseAccelV", 0xA0D964, CCamera::m_fMouseAccelVertical, CCamera::m_fMouseAccelVertical = value),
    FIELD(bool, "CCutsceneMgr::running", 0xA10AB2, CCutsceneMgr::IsRunning(), if (value != CCutsceneMgr::IsRunning()) return false),
};
#undef FIELD

void PushGlobals()
{
    for (GlobalField& field : globals) {
        field.read(field.previous);
        std::memcpy(MutableMemory(field.address, field.size), field.previous, field.size);
    }
}

void PullGlobals()
{
    for (GlobalField& field : globals) {
        const void* bytes = ReadMemory(field.address, field.size);
        if (std::memcmp(bytes, field.previous, field.size) && !field.write(bytes))
            std::fprintf(stderr, "[legacy-asi] Rejected invalid scalar value: %s\n", field.name);
    }
}

void PushState()
{
    PushGlobals();
    PushRenderWare();
    PushObjects();
}

void PullState()
{
    PullGlobals();
    PullObjects();
}

// Function adapters also synchronize: writes immediately before a legacy API
// call must be visible to it, and its result must survive the outer callback.
template<class F, class... Args> auto NativeCall(F function, Args... args)
{
    PullState();
    if constexpr (std::is_void_v<decltype(function(args...))>) {
        function(args...);
        PushState();
    } else {
        auto result = function(args...);
        PushState();
        return result;
    }
}

void __cdecl Suspend() { NativeCall(&CTimer::Suspend); }
void __cdecl Resume() { NativeCall(&CTimer::Resume); }
void __cdecl Stop() { NativeCall(&CTimer::Stop); }
void __cdecl StartUserPause() { NativeCall(&CTimer::StartUserPause); }
void __cdecl EndUserPause() { NativeCall(&CTimer::EndUserPause); }
int __cdecl CyclesPerMs() { return static_cast<int>(NativeCall(&CTimer::GetCyclesPerMillisecond)); }
uint32 __cdecl CurrentCycles() { return NativeCall(&CTimer::GetCurrentTimeInCycles); }
bool __cdecl SlowMotion() { return NativeCall(&CTimer::GetIsSlowMotionActive); }
void __cdecl SetClock(uint8 h, uint8 m) { NativeCall(&CClock::SetGameClock, h, m); }
void __cdecl StoreClock() { NativeCall(&CClock::StoreClock); }
void __cdecl RestoreClock() { NativeCall(&CClock::RestoreClock); }
uint32 __cdecl MinutesUntil(uint8 h, uint8 m) { return static_cast<uint32>(NativeCall(&CClock::GetGameClockMinutesUntil, h, m)); }
bool __cdecl TimeInRange(uint8 h, uint8 m) { return NativeCall(&CClock::GetIsTimeInRange, h, m); }
void __cdecl ForceWeather(int16 weather)
{
    if (ValidScalar(0xA10A42, weather)) NativeCall(&CWeather::ForceWeather, weather);
    else std::fprintf(stderr, "[legacy-asi] Rejected invalid ForceWeather argument.\n");
}
void __cdecl ForceWeatherNow(int16 weather)
{
    if (ValidScalar(0xA10A2E, weather)) NativeCall(&CWeather::ForceWeatherNow, weather);
    else std::fprintf(stderr, "[legacy-asi] Rejected invalid ForceWeatherNow argument.\n");
}
void __cdecl ReleaseWeather() { NativeCall(&CWeather::ReleaseWeather); }
void __cdecl SetFov(float fov)
{
    if (ValidScalar(0x696658, fov)) NativeCall(&CDraw::SetFOV, fov);
    else std::fprintf(stderr, "[legacy-asi] Rejected invalid SetFOV argument.\n");
}
bool __cdecl NativeInitRw() { return NativeCall(&CGame::InitialiseRenderWare); }
void __cdecl NativeShutdownRw() { NativeCall(&CGame::ShutdownRenderWare); }
void __cdecl NativeProcess() { NativeCall(&CGame::Process); }
void __cdecl NativeDrawing() { NativeCall(&::Render2dStuff); }
void __cdecl NativeHud() { NativeCall(&CHud::Draw); }

bool BuildFacade()
{
    if (!ReserveAddressSpace()) return false;
    // A canonical executable-header view for old module-relative SDK/injector
    // readers. It belongs only to this compatibility arena; neither the real
    // executable headers nor Windows' module list/PEB are modified.
    unsigned char header[512] = {};
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(header);
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(header + dos->e_lfanew);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER32);
    nt->FileHeader.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_32BIT_MACHINE;
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    nt->OptionalHeader.ImageBase = static_cast<DWORD>(MemoryBegin);
    nt->OptionalHeader.AddressOfEntryPoint = 0x667BF0 - static_cast<DWORD>(MemoryBegin);
    nt->OptionalHeader.SizeOfImage = static_cast<DWORD>(MemoryEnd - MemoryBegin);
    nt->OptionalHeader.SizeOfHeaders = sizeof(header);
    nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    nt->OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_GUI;
    if (!UsesCanonicalImage() && !MapReadOnlyData(MemoryBegin, header, sizeof(header))) return false;
    // Plugin-SDK GTAVC's documented version probe (not executable code).
    const uint32 vc10Signature = 0x53E58955;
    if (!MapReadOnlyData(0x667BF0, &vc10Signature, sizeof(vc10Signature))) return false;
    for (GlobalField& field : globals) {
        field.read(field.previous);
        if (!MapMutableData(field.address, field.previous, field.size)) return false;
    }
    struct Function { std::uintptr_t address; void (*adapter)(); };
#define FUNCTION(Address, Name) {Address, reinterpret_cast<void (*)()>(&Name)}
    const Function functions[] = {
        FUNCTION(0x4D0ED0, Suspend), FUNCTION(0x4D0E50, Resume), FUNCTION(0x4D0DB0, Stop),
        FUNCTION(0x4D0DA0, StartUserPause), FUNCTION(0x4D0D90, EndUserPause),
        FUNCTION(0x4D0E30, CyclesPerMs), FUNCTION(0x4D0DF0, CurrentCycles), FUNCTION(0x4D0DC0, SlowMotion),
        FUNCTION(0x487160, SetClock), FUNCTION(0x486FE0, StoreClock), FUNCTION(0x486FB0, RestoreClock),
        FUNCTION(0x487130, MinutesUntil), FUNCTION(0x4870F0, TimeInRange),
        FUNCTION(0x57D570, ForceWeather), FUNCTION(0x57D550, ForceWeatherNow), FUNCTION(0x57D540, ReleaseWeather),
        FUNCTION(0x54A2E0, SetFov),
    };
#undef FUNCTION
    for (const Function& function : functions)
        if (!MapFunction(function.address, function.adapter)) return false;
    if (!MapRenderWareAndUI(&PullState, &PushState)) {
        std::fprintf(stderr, "[legacy-asi] RenderWare mapping failed: %s\n", RenderWareError());
        return false;
    }
    if (!MapObjects(&PullState, &PushState, &CameraToLegacy)) {
        std::fprintf(stderr, "[legacy-asi] Object mapping failed: %s\n", ObjectsError());
        return false;
    }
    if (!MapOriginalInput(&PullState, &PushState)) return false;
    return MapEvent(InitRwEvent, reinterpret_cast<void (*)()>(&NativeInitRw)) &&
        MapEvent(ShutdownRwEvent, &NativeShutdownRw) &&
        MapEvent(ProcessEvent, &NativeProcess) && MapEvent(DrawingEvent, &NativeDrawing) &&
        MapEvent(HudEvent, &NativeHud) && FinalizeMemory();
}

void Invoke(std::uintptr_t event)
{
    PushState();
    DispatchEvent(event);
    PullState();
}
} // namespace

bool ReserveAddressSpace()
{
    if (reserved) return true;
    reserved = InitializeMemory();
    if (!reserved)
        std::fprintf(stderr, "[legacy-asi] Cannot reserve original addresses: %s\n", MemoryError());
    return reserved;
}

bool Initialize()
{
    if (attempted) return initializedSuccessfully;
    attempted = true;
    wchar_t executable[32768];
    const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (!length || length >= std::size(executable)) {
        std::fprintf(stderr, "[legacy-asi] Cannot determine executable path (Windows error %lu).\n", GetLastError());
        return false;
    }
    const std::wstring path(executable, length);
    const std::size_t separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        std::fprintf(stderr, "[legacy-asi] Executable path has no directory.\n");
        return false;
    }
    if (!BuildFacade()) {
        std::fprintf(stderr, "[legacy-asi] Initialization refused: %s\n", MemoryError());
        ReleaseMemory();
        reserved = false;
        return false;
    }
    ready = true; // DllMain may call a supported API while a module is loading.
    PushState();
    if (!LoadEnabledModules(path.substr(0, separator).c_str())) {
        std::fprintf(stderr, "[legacy-asi] Module loading refused: %s\n", LoaderError());
        ready = false;
        // An already loaded DLL may retain hooks or threads. Never unmap its
        // addresses or FreeLibrary it as a failed-load 'rollback'. Abort startup.
        if (!ModuleExecutionAttempted()) { ReleaseMemory(); reserved = false; }
        return false;
    }
    PullState();
    if (!HaveLoadedModules()) {
        ready = false;
        ReleaseMemory();
        reserved = false;
        std::fprintf(stderr, "[legacy-asi] No explicitly enabled modules; running without the facade.\n");
    } else {
        std::fprintf(stderr, "[legacy-asi] Experimental vc10-camera-rw-v2 facade active; not full GTA binary compatibility.\n");
    }
    SetOriginalInputActive(ready);
    initializedSuccessfully = true;
    return true;
}

bool InitialiseRenderWare()
{
    if (!ready) return CGame::InitialiseRenderWare();
    PushState();
    // Preserve the original BOOL result through Plugin-SDK before/after hooks.
    const auto entry = reinterpret_cast<bool (__cdecl*)()>(const_cast<void*>(ReadMemory(InitRwEvent, 6)));
    const bool result = entry();
    PullState();
    return result;
}

void ShutdownRenderWare()
{
    if (ready) Invoke(ShutdownRwEvent);
    else CGame::ShutdownRenderWare();
}

void ProcessGame()
{
    if (ready) Invoke(ProcessEvent);
    else CGame::Process();
}

void Draw2D()
{
    if (ready) Invoke(DrawingEvent);
    else ::Render2dStuff();
}

void DrawHud()
{
    if (ready) Invoke(HudEvent);
    else CHud::Draw();
}
} // namespace LegacyAsi
#endif
