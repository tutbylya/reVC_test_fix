#include "legacy_asi_identity.h"

#if defined(REVC_LEGACY_ASI) && defined(_WIN32) && defined(_MSC_VER) && defined(_M_IX86)

#include <cstddef>
#include <cstdint>
#include <intrin.h>

#pragma comment(linker, "/EXPORT:reVC_LegacyAsiPreInit=_reVC_LegacyAsiPreInit@12")

namespace LegacyAsi {
namespace {
constexpr ULONG_PTR CanonicalBase = 0x00400000;
constexpr ULONG_PTR CanonicalEnd = 0x00A20000;
constexpr LONG MaxModules = 32;

struct Record {
    HMODULE module;
    volatile LONG state; // 0 preparing, 1 ready, 2 failed
    DWORD error;
};
// Only POD and process-lifetime storage: callbacks run under the loader lock.
Record records[MaxModules];
volatile LONG recordCount;
HMODULE realMain;
char realGameNameA[MAX_PATH];
wchar_t realGameNameW[MAX_PATH];

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; }
wchar_t Lower(wchar_t c) { return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + (L'a' - L'A')) : c; }

template<class C> bool Equal(const C* a, const C* b)
{
    if (!a || !b) return false;
    while (*a && *b) {
        if (Lower(*a) != Lower(*b)) return false;
        ++a; ++b;
    }
    return *a == *b;
}

template<class C> const C* Basename(const C* name)
{
    const C* result = name;
    if (name)
        for (; *name; ++name)
            if (*name == static_cast<C>('\\') || *name == static_cast<C>('/')) result = name + 1;
    return result;
}

bool GameName(const char* name)
{
    if (!name) return true;
    const char* base = Basename(name);
    return Equal(base, "gta-vc.exe") || Equal(base, "reVC.exe") || Equal(base, realGameNameA);
}

bool GameName(const wchar_t* name)
{
    if (!name) return true;
    const wchar_t* base = Basename(name);
    return Equal(base, L"gta-vc.exe") || Equal(base, L"reVC.exe") || Equal(base, realGameNameW);
}

bool Canonical(HMODULE module) { return reinterpret_cast<ULONG_PTR>(module) == CanonicalBase; }
HMODULE GameHandle() { return reinterpret_cast<HMODULE>(CanonicalBase); }

HMODULE WINAPI GameGetModuleHandleA(LPCSTR name)
{
    return GameName(name) ? GameHandle() : ::GetModuleHandleA(name);
}

HMODULE WINAPI GameGetModuleHandleW(LPCWSTR name)
{
    return GameName(name) ? GameHandle() : ::GetModuleHandleW(name);
}

template<class C> bool CanonicalQuery(DWORD flags, const C* name)
{
    if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) {
        const ULONG_PTR address = reinterpret_cast<ULONG_PTR>(name);
        return address >= CanonicalBase && address < CanonicalEnd;
    }
    return GameName(name);
}

bool CanonicalModuleEx(DWORD flags, HMODULE* module)
{
    constexpr DWORD valid = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if (!module || (flags & ~valid) ||
        ((flags & GET_MODULE_HANDLE_EX_FLAG_PIN) && (flags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT))) {
        if (module) *module = nullptr;
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    // The main image has process lifetime. There is no unloadable fake DLL.
    *module = GameHandle();
    return true;
}

BOOL WINAPI GameGetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE* module)
{
    return CanonicalQuery(flags, name) ? CanonicalModuleEx(flags, module) : ::GetModuleHandleExA(flags, name, module);
}

BOOL WINAPI GameGetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE* module)
{
    return CanonicalQuery(flags, name) ? CanonicalModuleEx(flags, module) : ::GetModuleHandleExW(flags, name, module);
}

DWORD WINAPI GameGetModuleFileNameA(HMODULE module, LPSTR path, DWORD size)
{
    return ::GetModuleFileNameA(Canonical(module) ? realMain : module, path, size);
}

DWORD WINAPI GameGetModuleFileNameW(HMODULE module, LPWSTR path, DWORD size)
{
    return ::GetModuleFileNameW(Canonical(module) ? realMain : module, path, size);
}

FARPROC WINAPI GameGetProcAddress(HMODULE module, LPCSTR name)
{
    if (!Canonical(module)) return ::GetProcAddress(module, name);
    // The original VC EXE has no export table. Do not expose reVC's bootstrap
    // exports through a synthetic original-game handle.
    SetLastError(ERROR_PROC_NOT_FOUND);
    return nullptr;
}

BOOL WINAPI GameFreeLibrary(HMODULE module)
{
    if (!Canonical(module)) return ::FreeLibrary(module);
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}

ULONG_PTR Replacement(const char* name)
{
#define API(Name, Shim) if (Equal(name, #Name)) return reinterpret_cast<ULONG_PTR>(&Shim)
    API(GetModuleHandleA, GameGetModuleHandleA);
    API(GetModuleHandleW, GameGetModuleHandleW);
    API(GetModuleHandleExA, GameGetModuleHandleExA);
    API(GetModuleHandleExW, GameGetModuleHandleExW);
    API(GetModuleFileNameA, GameGetModuleFileNameA);
    API(GetModuleFileNameW, GameGetModuleFileNameW);
    API(GetProcAddress, GameGetProcAddress);
    API(FreeLibrary, GameFreeLibrary);
#undef API
    return 0;
}

bool SystemLoaderImport(const char* library)
{
    if (Equal(library, "kernel32.dll") || Equal(library, "kernelbase.dll")) return true;
    constexpr char prefix[] = "api-ms-win-core-libraryloader-";
    for (std::size_t i = 0; i < sizeof(prefix) - 1; ++i)
        if (!library[i] || Lower(library[i]) != prefix[i]) return false;
    return true;
}

bool Readable(HMODULE module, const void* address, std::size_t size)
{
    ULONG_PTR position = reinterpret_cast<ULONG_PTR>(address);
    if (!size || size > static_cast<ULONG_PTR>(-1) - position) return false;
    const ULONG_PTR end = position + size;
    while (position < end) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery(reinterpret_cast<const void*>(position), &info, sizeof(info)) ||
            info.AllocationBase != module || info.Type != MEM_IMAGE || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        const ULONG_PTR limit = reinterpret_cast<ULONG_PTR>(info.BaseAddress) + info.RegionSize;
        if (limit <= position) return false;
        position = limit < end ? limit : end;
    }
    return true;
}

struct Image {
    HMODULE module;
    BYTE* base;
    DWORD size;
    void* Rva(DWORD rva, std::size_t length) const
    {
        if (rva >= size || length > size - rva) return nullptr;
        void* result = base + rva;
        return Readable(module, result, length) ? result : nullptr;
    }
    const char* String(DWORD rva) const
    {
        if (rva >= size) return nullptr;
        for (DWORD i = rva; i < size; ++i) {
            const char* byte = static_cast<const char*>(Rva(i, 1));
            if (!byte) return nullptr;
            if (!*byte) return reinterpret_cast<const char*>(base + rva);
        }
        return nullptr;
    }
};

bool WriteSlot(ULONG_PTR* slot, ULONG_PTR replacement, DWORD& error)
{
    DWORD protection;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection)) {
        error = GetLastError();
        return false;
    }
    InterlockedExchange(reinterpret_cast<volatile LONG*>(slot), static_cast<LONG>(replacement));
    DWORD unused;
    if (!VirtualProtect(slot, sizeof(*slot), protection, &unused) ||
        !FlushInstructionCache(GetCurrentProcess(), slot, sizeof(*slot))) {
        error = GetLastError();
        return false;
    }
    return true;
}

bool Patch(HMODULE module, DWORD& error)
{
    error = ERROR_BAD_EXE_FORMAT;
    const BYTE* base = reinterpret_cast<const BYTE*>(module);
    if (!Readable(module, base, sizeof(IMAGE_DOS_HEADER))) return false;
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < static_cast<LONG>(sizeof(*dos)) ||
        static_cast<ULONG_PTR>(dos->e_lfanew) > static_cast<ULONG_PTR>(-1) - reinterpret_cast<ULONG_PTR>(base)) return false;
    const IMAGE_NT_HEADERS32* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (!Readable(module, nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        !(nt->FileHeader.Characteristics & IMAGE_FILE_DLL) ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT ||
        nt->OptionalHeader.SizeOfImage < sizeof(*dos)) return false;
    Image image = {module, reinterpret_cast<BYTE*>(module), nt->OptionalHeader.SizeOfImage};
    const IMAGE_DATA_DIRECTORY directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress && !directory.Size) return true;
    if (!image.Rva(directory.VirtualAddress, directory.Size) || directory.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) return false;
    const DWORD descriptorCount = directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    for (DWORD descriptorIndex = 0; descriptorIndex < descriptorCount; ++descriptorIndex) {
        const auto descriptor = static_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
            image.Rva(directory.VirtualAddress + descriptorIndex * sizeof(IMAGE_IMPORT_DESCRIPTOR), sizeof(IMAGE_IMPORT_DESCRIPTOR)));
        if (!descriptor) return false;
        if (!descriptor->Name && !descriptor->FirstThunk && !descriptor->OriginalFirstThunk) return true;
        const char* library = image.String(descriptor->Name);
        if (!library) return false;
        if (!SystemLoaderImport(library)) continue;
        // Windows has already resolved FirstThunk. Retain the original name
        // table; do not guess names from bound addresses or patch another DLL.
        if (!descriptor->OriginalFirstThunk || !descriptor->FirstThunk) return false;
        for (DWORD index = 0; index < image.size / sizeof(IMAGE_THUNK_DATA32); ++index) {
            const std::uint64_t lookupRva = static_cast<std::uint64_t>(descriptor->OriginalFirstThunk) +
                static_cast<std::uint64_t>(index) * sizeof(IMAGE_THUNK_DATA32);
            const std::uint64_t slotRva = static_cast<std::uint64_t>(descriptor->FirstThunk) +
                static_cast<std::uint64_t>(index) * sizeof(IMAGE_THUNK_DATA32);
            if (lookupRva > MAXDWORD || slotRva > MAXDWORD) return false;
            const auto lookup = static_cast<const IMAGE_THUNK_DATA32*>(image.Rva(static_cast<DWORD>(lookupRva), sizeof(IMAGE_THUNK_DATA32)));
            auto slot = static_cast<IMAGE_THUNK_DATA32*>(image.Rva(static_cast<DWORD>(slotRva), sizeof(IMAGE_THUNK_DATA32)));
            if (!lookup || !slot) return false;
            if (!lookup->u1.AddressOfData) break;
            if (IMAGE_SNAP_BY_ORDINAL32(lookup->u1.Ordinal)) continue;
            if (lookup->u1.AddressOfData > MAXDWORD - sizeof(WORD)) return false;
            const char* name = image.String(lookup->u1.AddressOfData + sizeof(WORD));
            if (!name) return false;
            const ULONG_PTR replacement = Replacement(name);
            if (replacement && !WriteSlot(reinterpret_cast<ULONG_PTR*>(&slot->u1.Function), replacement, error)) return false;
            if (index == image.size / sizeof(IMAGE_THUNK_DATA32) - 1) return false;
        }
    }
    return false; // A bounded, zero-terminated descriptor table is required.
}

bool RememberMain()
{
    if (realMain) return true;
    HMODULE module = ::GetModuleHandleW(nullptr);
    if (!module) return false;
    // Buffers are POD, bounded, and initialized only once under loader lock.
    char pathA[32768];
    wchar_t pathW[32768];
    const DWORD countA = ::GetModuleFileNameA(module, pathA, static_cast<DWORD>(sizeof(pathA)));
    const DWORD countW = ::GetModuleFileNameW(module, pathW, static_cast<DWORD>(sizeof(pathW) / sizeof(pathW[0])));
    if (!countA || countA >= sizeof(pathA) || !countW || countW >= sizeof(pathW) / sizeof(pathW[0])) return false;
    const char* nameA = Basename(pathA);
    const wchar_t* nameW = Basename(pathW);
    std::size_t i = 0;
    for (; nameA[i] && i < MAX_PATH - 1; ++i) realGameNameA[i] = nameA[i];
    if (nameA[i]) return false;
    realGameNameA[i] = '\0';
    i = 0;
    for (; nameW[i] && i < MAX_PATH - 1; ++i) realGameNameW[i] = nameW[i];
    if (nameW[i]) return false;
    realGameNameW[i] = L'\0';
    realMain = module;
    return true;
}

Record* Find(HMODULE module)
{
    const LONG count = InterlockedCompareExchange(&recordCount, 0, 0);
    for (LONG i = 0; i < count && i < MaxModules; ++i)
        if (records[i].module == module) return &records[i];
    return nullptr;
}

__declspec(noreturn) void AbortBeforeConstructors(DWORD code)
{
    TerminateProcess(GetCurrentProcess(), code);
    // Never return into an unpatched ASI's CRT if termination is refused.
    __fastfail(FAST_FAIL_FATAL_APP_EXIT);
}

void PreInit(HMODULE module)
{
    if (Record* prior = Find(module)) {
        if (InterlockedCompareExchange(&prior->state, 0, 0) == 1) return;
        AbortBeforeConstructors(ERROR_BAD_EXE_FORMAT);
    }
    const LONG index = InterlockedIncrement(&recordCount) - 1;
    if (index >= MaxModules) {
        AbortBeforeConstructors(ERROR_TOO_MANY_MODULES);
    }
    Record& record = records[index];
    record.module = module;
    record.error = ERROR_BAD_EXE_FORMAT;
    if (!RememberMain() || !Patch(module, record.error)) {
        InterlockedExchange(&record.state, 2);
        OutputDebugStringA("[legacy-asi] Pre-CRT module identity patch failed; aborting before ASI constructors.\n");
        AbortBeforeConstructors(ERROR_BAD_EXE_FORMAT);
    }
    record.error = ERROR_SUCCESS;
    InterlockedExchange(&record.state, 1);
}
} // namespace

bool IdentityPatched(HMODULE module)
{
#ifdef REVC_LEGACY_ASI_CORE_DLL
    // The small main executable owns the pre-CRT callback and registry. The
    // game core is a separately initialized DLL; never consult a second copy.
    using Query = BOOL (__cdecl*)(HMODULE);
    const auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(nullptr), "reVC_LegacyAsiIdentityPatched"));
    return query && query(module) != FALSE;
#else
    Record* record = Find(module);
    return record && InterlockedCompareExchange(&record->state, 0, 0) == 1;
#endif
}

DWORD IdentityPatchError(HMODULE module)
{
#ifdef REVC_LEGACY_ASI_CORE_DLL
    return IdentityPatched(module) ? ERROR_SUCCESS : ERROR_NOT_READY;
#else
    Record* record = Find(module);
    return record ? record->error : ERROR_NOT_READY;
#endif
}
} // namespace LegacyAsi

extern "C" void WINAPI reVC_LegacyAsiPreInit(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) LegacyAsi::PreInit(module);
}
#ifndef REVC_LEGACY_ASI_CORE_DLL
#pragma comment(linker, "/EXPORT:reVC_LegacyAsiIdentityPatched=_reVC_LegacyAsiIdentityPatched")
extern "C" BOOL __cdecl reVC_LegacyAsiIdentityPatched(HMODULE module)
{
    return LegacyAsi::IdentityPatched(module) ? TRUE : FALSE;
}
#endif
#endif
