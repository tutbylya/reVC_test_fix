#include "legacy_asi_loader.h"

#if defined(REVC_LEGACY_ASI) && defined(_WIN32) && defined(_MSC_VER) && defined(_M_IX86)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include "legacy_asi_arena_protocol.h"
#include "legacy_asi_identity.h"

#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace LegacyAsi {
namespace {

constexpr unsigned MaxModules = 32;
constexpr std::size_t MaxManifestBytes = 64 * 1024;
constexpr std::size_t HashBytes = 32;

// POD state has no static destructor. Arbitrary ASIs may leave process-wide
// hooks and threads behind; they cannot be safely unloaded in reverse order.
volatile LONG loadState; // 0 = never attempted, 1 = active, 2 = done, 3 = failed
volatile LONG reentrantAttempt;
volatile LONG loadedCount;
volatile LONG executionAttempted;
HANDLE retainedFiles[MaxModules];
HMODULE retainedModules[MaxModules];
char errorText[1024];

bool Fail(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::vsnprintf(errorText, sizeof(errorText), format, args);
    va_end(args);
    errorText[sizeof(errorText) - 1] = '\0';
    std::fprintf(stderr, "[legacy-asi] %s\n", errorText);
    return false;
}

class FileHandle {
    HANDLE handle = INVALID_HANDLE_VALUE;
public:
    FileHandle() = default;
    explicit FileHandle(HANDLE value) : handle(value) {}
    ~FileHandle() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    FileHandle(FileHandle&& other) noexcept : handle(other.Release()) {}
    FileHandle& operator=(FileHandle&& other) noexcept
    {
        if (this != &other) {
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
            handle = other.Release();
        }
        return *this;
    }
    HANDLE Get() const { return handle; }
    HANDLE Release()
    {
        HANDLE value = handle;
        handle = INVALID_HANDLE_VALUE;
        return value;
    }
};

struct Module {
    std::string nameUtf8;
    std::wstring name;
    std::wstring path;
    std::array<BYTE, HashBytes> expectedHash;
    FileHandle file;
};

bool NeedsCanonicalIdentity(const Module& module)
{
    // Exact, reviewed Amazing Snapshot VC v1.1. This binary mixes literal VC
    // addresses and module-relative SDK addresses during CRT initialization.
    // Refuse it before any ASI executes if the compatible launcher is absent.
    constexpr BYTE hash[HashBytes] = {
        0x12,0x74,0x30,0x8B,0x5A,0x1E,0xE9,0x36,0xE3,0xEC,0x68,0xAF,0x8F,0xC9,0x10,0x34,
        0xD2,0xBD,0x6D,0xA5,0xA6,0xBF,0xB9,0x9A,0x77,0xDD,0xA7,0xFA,0x30,0xA1,0x1B,0x03
    };
    constexpr BYTE wheel[HashBytes] = {
        0xF2,0x19,0xB9,0xC4,0x3D,0x7C,0x07,0xCE,0xE7,0xC7,0x8E,0x67,0x7E,0x28,0x64,0xA7,
        0x67,0x53,0x45,0x6F,0x4E,0x97,0x90,0x28,0x64,0x87,0x88,0x97,0xE8,0x8F,0x6C,0x92
    };
    return std::memcmp(module.expectedHash.data(), hash, sizeof(hash)) == 0 ||
        std::memcmp(module.expectedHash.data(), wheel, sizeof(wheel)) == 0;
}

bool HasNativeReplacement(const Module& module)
{
    // The exact 2012 HQ loading-screen patcher bundled with Remastered
    // Interface patches original executable instructions, not callable ABI.
    // Our native ScreenCount option implements that resource feature safely.
    constexpr BYTE legacySplash[HashBytes] = {
        0x43,0xBF,0xF5,0x7B,0xB5,0x8C,0xE5,0x53,0xC8,0xA4,0x1D,0xB1,0x62,0x21,0xE6,0x41,
        0x4F,0x31,0x66,0xA4,0xDD,0x82,0xD0,0x7A,0xEF,0x70,0x34,0xE4,0x21,0xAC,0x10,0x14
    };
    return std::memcmp(module.expectedHash.data(), legacySplash, sizeof(legacySplash)) == 0;
}

bool Slash(wchar_t value) { return value == L'\\' || value == L'/'; }

bool AbsoluteDirectory(const wchar_t* directory, std::wstring& normalized)
{
    if (!directory || !*directory) return Fail("The game directory is empty");
    const std::size_t length = std::wcslen(directory);
    const bool drive = length >= 3 &&
        ((directory[0] >= L'A' && directory[0] <= L'Z') ||
         (directory[0] >= L'a' && directory[0] <= L'z')) &&
        directory[1] == L':' && Slash(directory[2]);
    const bool unc = length >= 5 && Slash(directory[0]) && Slash(directory[1]);
    if ((!drive && !unc) || length >= 32767)
        return Fail("An absolute Windows game directory is required");
    const DWORD required = GetFullPathNameW(directory, 0, nullptr, nullptr);
    if (!required || required > 32767)
        return Fail("Cannot normalize the game directory (Windows error %lu)", GetLastError());
    normalized.resize(required);
    const DWORD written = GetFullPathNameW(directory, required, &normalized[0], nullptr);
    if (!written || written >= required)
        return Fail("Cannot normalize the game directory (Windows error %lu)", GetLastError());
    normalized.resize(written);
    while (normalized.size() > 3 && Slash(normalized.back())) normalized.pop_back();
    return true;
}

std::string Trim(const std::string& value)
{
    const std::size_t begin = value.find_first_not_of(" \t");
    if (begin == std::string::npos) return {};
    const std::size_t end = value.find_last_not_of(" \t");
    return value.substr(begin, end - begin + 1);
}

bool EqualName(const std::wstring& left, const std::wstring& right)
{
    return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()),
                                right.c_str(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool ReservedDeviceName(const std::wstring& name)
{
    std::wstring stem = name.substr(0, name.find(L'.'));
    for (wchar_t& c : stem)
        if (c >= L'a' && c <= L'z') c -= L'a' - L'A';
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL") return true;
    if (stem.size() != 4 || (stem.compare(0, 3, L"COM") && stem.compare(0, 3, L"LPT")))
        return false;
    return (stem[3] >= L'1' && stem[3] <= L'9') ||
           stem[3] == L'\x00B9' || stem[3] == L'\x00B2' || stem[3] == L'\x00B3';
}

bool Filename(const std::string& input, std::wstring& result, unsigned line)
{
    if (input.empty() || input.size() > 1024)
        return Fail("Manifest line %u has an empty or oversized module name", line);
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                                         static_cast<int>(input.size()), nullptr, 0);
    if (size <= 0 || size > 255)
        return Fail("Manifest line %u requires a valid UTF-8 filename of at most 255 characters", line);
    result.resize(size);
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                           static_cast<int>(input.size()), &result[0], size) != size)
        return Fail("Cannot decode the module name on manifest line %u", line);
    for (wchar_t c : result) {
        if (c < 0x20 || (c >= 0x7F && c <= 0x9F) || std::wcschr(L"<>:\"/\\|?*=", c))
            return Fail("Manifest line %u requires a flat filename without paths or control characters", line);
    }
    if (result.front() == L' ' || result.back() == L' ' || result.back() == L'.' ||
        result.size() < 5 ||
        !EqualName(result.substr(result.size() - 4), L".asi") || ReservedDeviceName(result))
        return Fail("Manifest line %u does not name a regular .asi file", line);
    return true;
}

int Hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ParseManifest(const std::string& input, std::vector<Module>& modules)
{
    if (input.find('\0') != std::string::npos)
        return Fail("enabled.ini contains NUL bytes; use UTF-8 rather than UTF-16");
    std::size_t position = input.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    bool section = false;
    unsigned lineNumber = 0;
    while (position < input.size()) {
        ++lineNumber;
        const std::size_t end = input.find('\n', position);
        std::string line = input.substr(position, end == std::string::npos ? end : end - position);
        position = end == std::string::npos ? input.size() : end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        line = Trim(line);
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (line.front() == '[') {
            if (section || _stricmp(line.c_str(), "[Modules]") != 0)
                return Fail("Manifest line %u: exactly one [Modules] section is supported", lineNumber);
            section = true;
            continue;
        }
        if (!section) return Fail("Manifest line %u appears outside [Modules]", lineNumber);
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos)
            return Fail("Manifest line %u must be filename.asi=64-hex-digit-SHA256", lineNumber);
        if (modules.size() == MaxModules)
            return Fail("enabled.ini exceeds the limit of %u explicitly enabled modules", MaxModules);
        Module module;
        module.nameUtf8 = Trim(line.substr(0, equals));
        if (!Filename(module.nameUtf8, module.name, lineNumber)) return false;
        for (const Module& prior : modules)
            if (EqualName(prior.name, module.name))
                return Fail("Manifest line %u repeats a module name (case-insensitive)", lineNumber);
        const std::string hash = Trim(line.substr(equals + 1));
        if (hash.size() != HashBytes * 2)
            return Fail("Manifest line %u requires exactly 64 hexadecimal SHA-256 digits", lineNumber);
        for (std::size_t i = 0; i < HashBytes; ++i) {
            const int high = Hex(hash[i * 2]), low = Hex(hash[i * 2 + 1]);
            if (high < 0 || low < 0)
                return Fail("Manifest line %u contains a non-hexadecimal SHA-256", lineNumber);
            module.expectedHash[i] = static_cast<BYTE>((high << 4) | low);
        }
        modules.push_back(std::move(module));
    }
    return true;
}

bool ReadAt(HANDLE file, std::uint64_t length, std::uint64_t offset, void* data, DWORD size)
{
    if (offset > length || size > length - offset || offset > INT64_MAX) return false;
    LARGE_INTEGER where;
    where.QuadPart = static_cast<LONGLONG>(offset);
    DWORD read = 0;
    return SetFilePointerEx(file, where, nullptr, FILE_BEGIN) &&
           ReadFile(file, data, size, &read, nullptr) && read == size;
}

template<typename T>
bool Read(HANDLE file, std::uint64_t length, std::uint64_t offset, T& value)
{
    return ReadAt(file, length, offset, &value, sizeof(value));
}

bool CheckImage(Module& module, std::uint64_t length)
{
    IMAGE_DOS_HEADER dos = {};
    DWORD signature = 0;
    IMAGE_FILE_HEADER coff = {};
    IMAGE_OPTIONAL_HEADER32 optional = {};
    if (!Read(module.file.Get(), length, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < static_cast<LONG>(sizeof(dos)))
        return Fail("%s is not a valid PE DLL (DOS header)", module.nameUtf8.c_str());
    const std::uint64_t nt = static_cast<std::uint32_t>(dos.e_lfanew);
    if (!Read(module.file.Get(), length, nt, signature) || signature != IMAGE_NT_SIGNATURE ||
        !Read(module.file.Get(), length, nt + sizeof(signature), coff) ||
        coff.Machine != IMAGE_FILE_MACHINE_I386 ||
        !(coff.Characteristics & IMAGE_FILE_DLL) ||
        !(coff.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) ||
        !coff.NumberOfSections || coff.NumberOfSections > 96 ||
        coff.SizeOfOptionalHeader < sizeof(optional))
        return Fail("%s must be a PE32 x86 DLL, not an x64/ARM module or an EXE", module.nameUtf8.c_str());
    const std::uint64_t opt = nt + sizeof(signature) + sizeof(coff);
    if (!Read(module.file.Get(), length, opt, optional) ||
        optional.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        !optional.SizeOfImage || !optional.SizeOfHeaders ||
        optional.SizeOfHeaders > length || optional.SizeOfHeaders > optional.SizeOfImage ||
        !optional.SectionAlignment || !optional.FileAlignment ||
        optional.AddressOfEntryPoint >= optional.SizeOfImage ||
        optional.NumberOfRvaAndSizes > IMAGE_NUMBEROF_DIRECTORY_ENTRIES)
        return Fail("%s has invalid PE32 image metadata", module.nameUtf8.c_str());
    const std::uint64_t sections = opt + coff.SizeOfOptionalHeader;
    if (sections > length || sizeof(IMAGE_SECTION_HEADER) * coff.NumberOfSections > length - sections ||
        sections + sizeof(IMAGE_SECTION_HEADER) * coff.NumberOfSections > optional.SizeOfHeaders)
        return Fail("%s has a truncated PE section table", module.nameUtf8.c_str());
    for (WORD i = 0; i < coff.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER section = {};
        if (!Read(module.file.Get(), length, sections + i * sizeof(section), section) ||
            section.PointerToRawData > length || section.SizeOfRawData > length - section.PointerToRawData ||
            section.VirtualAddress > optional.SizeOfImage ||
            section.Misc.VirtualSize > optional.SizeOfImage - section.VirtualAddress ||
            section.SizeOfRawData > optional.SizeOfImage - section.VirtualAddress)
            return Fail("%s has invalid or truncated PE sections", module.nameUtf8.c_str());
    }
    return true;
}

class Sha256 {
    HMODULE library = nullptr;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<BYTE> storage;
    decltype(&BCryptOpenAlgorithmProvider) openAlgorithm = nullptr;
    decltype(&BCryptGetProperty) getProperty = nullptr;
    decltype(&BCryptCreateHash) createHash = nullptr;
    decltype(&BCryptHashData) hashData = nullptr;
    decltype(&BCryptFinishHash) finishHash = nullptr;
    decltype(&BCryptDestroyHash) destroyHash = nullptr;
    decltype(&BCryptCloseAlgorithmProvider) closeAlgorithm = nullptr;
public:
    ~Sha256()
    {
        if (hash) destroyHash(hash);
        if (algorithm) closeAlgorithm(algorithm, 0);
        if (library) FreeLibrary(library);
    }
    bool Compute(Module& module, std::array<BYTE, HashBytes>& result)
    {
        // Do not import bcrypt.dll at process startup: its dependency/CRT
        // initialization can occupy original VC addresses before our entry
        // reservation. Load only the trusted system provider when hashing is
        // needed, after the compatibility owner has reserved its ABI arena.
        library = LoadLibraryExW(L"bcrypt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library)
            return Fail("Cannot load the system SHA-256 provider (Windows error %lu)", GetLastError());
#define BCRYPT_API(Name, Member) Member = reinterpret_cast<decltype(&Name)>(GetProcAddress(library, #Name))
        BCRYPT_API(BCryptOpenAlgorithmProvider, openAlgorithm);
        BCRYPT_API(BCryptGetProperty, getProperty);
        BCRYPT_API(BCryptCreateHash, createHash);
        BCRYPT_API(BCryptHashData, hashData);
        BCRYPT_API(BCryptFinishHash, finishHash);
        BCRYPT_API(BCryptDestroyHash, destroyHash);
        BCRYPT_API(BCryptCloseAlgorithmProvider, closeAlgorithm);
#undef BCRYPT_API
        if (!openAlgorithm || !getProperty || !createHash || !hashData ||
            !finishHash || !destroyHash || !closeAlgorithm)
            return Fail("The system bcrypt.dll lacks the required SHA-256 functions");
        NTSTATUS status = openAlgorithm(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        ULONG objectSize = 0, hashSize = 0, written = 0;
        if (status >= 0)
            status = getProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &written, 0);
        if (status >= 0)
            status = getProperty(algorithm, BCRYPT_HASH_LENGTH,
                reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize), &written, 0);
        if (status < 0 || hashSize != HashBytes || !objectSize)
            return Fail("Cannot initialize SHA-256 (NTSTATUS 0x%08lX)", static_cast<ULONG>(status));
        storage.resize(objectSize);
        status = createHash(algorithm, &hash, storage.data(), objectSize, nullptr, 0, 0);
        if (status < 0)
            return Fail("Cannot create SHA-256 state (NTSTATUS 0x%08lX)", static_cast<ULONG>(status));
        LARGE_INTEGER start = {};
        if (!SetFilePointerEx(module.file.Get(), start, nullptr, FILE_BEGIN))
            return Fail("Cannot rewind %s (Windows error %lu)", module.nameUtf8.c_str(), GetLastError());
        BYTE buffer[64 * 1024];
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(module.file.Get(), buffer, sizeof(buffer), &read, nullptr))
                return Fail("Cannot read %s (Windows error %lu)", module.nameUtf8.c_str(), GetLastError());
            if (!read) break;
            status = hashData(hash, buffer, read, 0);
            if (status < 0)
                return Fail("Cannot hash %s (NTSTATUS 0x%08lX)", module.nameUtf8.c_str(), static_cast<ULONG>(status));
        }
        status = finishHash(hash, result.data(), static_cast<ULONG>(result.size()), 0);
        if (status < 0)
            return Fail("Cannot finish hashing %s (NTSTATUS 0x%08lX)", module.nameUtf8.c_str(), static_cast<ULONG>(status));
        return true;
    }
};

bool Preflight(Module& module, const std::wstring& directory)
{
    module.path = directory + L"\\legacy-asi\\" + module.name;
    // No write/delete sharing: another process (or an existing writer) cannot
    // replace the checked source while the DLL is being loaded or stays alive.
    module.file = FileHandle(CreateFileW(module.path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (module.file.Get() == INVALID_HANDLE_VALUE)
        return Fail("Cannot read-lock %s (Windows error %lu)", module.nameUtf8.c_str(), GetLastError());
    BY_HANDLE_FILE_INFORMATION info = {};
    LARGE_INTEGER length = {};
    if (GetFileType(module.file.Get()) != FILE_TYPE_DISK ||
        !GetFileInformationByHandle(module.file.Get(), &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        !GetFileSizeEx(module.file.Get(), &length) || length.QuadPart < 0)
        return Fail("%s is not a readable regular disk file", module.nameUtf8.c_str());
    if (!CheckImage(module, static_cast<std::uint64_t>(length.QuadPart))) return false;
    std::array<BYTE, HashBytes> actual = {};
    Sha256 sha;
    if (!sha.Compute(module, actual)) return false;
    BYTE mismatch = 0;
    for (std::size_t i = 0; i < HashBytes; ++i)
        mismatch |= static_cast<BYTE>(actual[i] ^ module.expectedHash[i]);
    if (mismatch) return Fail("SHA-256 mismatch for %s; no ASI has been executed", module.nameUtf8.c_str());

    // Resolve junctions before the checked path is used a second time by the
    // Windows image loader. The source handle remains locked until exit.
    const DWORD needed = GetFinalPathNameByHandleW(module.file.Get(), nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!needed || needed >= 32767)
        return Fail("Cannot resolve the checked path of %s (Windows error %lu)", module.nameUtf8.c_str(), GetLastError());
    std::wstring finalPath(needed + 1, L'\0');
    const DWORD count = GetFinalPathNameByHandleW(module.file.Get(), &finalPath[0], needed + 1,
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!count || count > needed)
        return Fail("Cannot resolve the checked path of %s (Windows error %lu)", module.nameUtf8.c_str(), GetLastError());
    finalPath.resize(count);
    module.path.swap(finalPath);
    return true;
}

bool Load(const wchar_t* absoluteGameDirectory)
{
    std::wstring directory;
    if (!AbsoluteDirectory(absoluteGameDirectory, directory)) return false;
    const std::wstring manifestPath = directory + L"\\legacy-asi\\enabled.ini";
    FileHandle manifest(CreateFileW(manifestPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (manifest.Get() == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return true;
        return Fail("Cannot open legacy-asi/enabled.ini (Windows error %lu)", code);
    }
    LARGE_INTEGER length = {};
    if (GetFileType(manifest.Get()) != FILE_TYPE_DISK ||
        !GetFileSizeEx(manifest.Get(), &length) || length.QuadPart < 0 ||
        length.QuadPart > MaxManifestBytes)
        return Fail("enabled.ini is not a regular file of at most %u bytes", static_cast<unsigned>(MaxManifestBytes));
    std::string contents(static_cast<std::size_t>(length.QuadPart), '\0');
    DWORD read = 0;
    if (!contents.empty() &&
        (!ReadFile(manifest.Get(), &contents[0], static_cast<DWORD>(contents.size()), &read, nullptr) ||
         read != contents.size()))
        return Fail("enabled.ini could not be read completely (Windows error %lu)", GetLastError());
    std::vector<Module> modules;
    if (!ParseManifest(contents, modules)) return false;
    for (Module& module : modules)
        if (!Preflight(module, directory)) return false;
    for (const Module& module : modules) {
        if (HasNativeReplacement(module))
            return Fail("%s is an original-executable loading-screen patcher. Use the resource pack with [Loading] ScreenCount=14 instead. No ASI has executed.",
                module.nameUtf8.c_str());
        if (NeedsCanonicalIdentity(module) &&
            !(LegacyAsiLaunch::GetAdoptedCapabilities() & (LegacyAsiLaunch::CapabilityTlsPreInit | LegacyAsiLaunch::CapabilityCanonicalImage)))
            return Fail("%s requires the compatible reVC.exe launcher; start it instead of reVC-game.exe. No ASI has executed.",
                module.nameUtf8.c_str());
    }
    if (InterlockedCompareExchange(&reentrantAttempt, 0, 0))
        return Fail("Reentrant legacy ASI initialization was rejected before execution");

    // All syntax, images and hashes passed. Transfer every read lock before
    // executing any module so even a partial-load failure retains the locks.
    for (std::size_t i = 0; i < modules.size(); ++i)
        retainedFiles[i] = modules[i].file.Release();
    for (std::size_t i = 0; i < modules.size(); ++i) {
        InterlockedExchange(&executionAttempted, 1);
        HMODULE loaded = LoadLibraryExW(modules[i].path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!loaded)
            return Fail("Loading %s failed (Windows error %lu); %lu earlier ASI(s) remain loaded. Abort startup.",
                modules[i].nameUtf8.c_str(), GetLastError(), static_cast<unsigned long>(loadedCount));
        retainedModules[i] = loaded;
        InterlockedIncrement(&loadedCount);
        if (NeedsCanonicalIdentity(modules[i]) &&
            !(LegacyAsiLaunch::GetAdoptedCapabilities() & LegacyAsiLaunch::CapabilityCanonicalImage) && !IdentityPatched(loaded))
            return Fail("%s did not receive its pre-CRT identity bridge (Windows error %lu). Abort startup.",
                modules[i].nameUtf8.c_str(), IdentityPatchError(loaded));
        std::fprintf(stderr, "[legacy-asi] Loaded explicitly enabled module: %s\n", modules[i].nameUtf8.c_str());
        if (InterlockedCompareExchange(&reentrantAttempt, 0, 0))
            return Fail("Reentrant ASI initialization occurred during loading; loaded ASIs remain alive. Abort startup.");
    }
    return true;
}

} // namespace

bool LoadEnabledModules(const wchar_t* absoluteGameDirectory)
{
    const LONG previous = InterlockedCompareExchange(&loadState, 1, 0);
    if (previous == 2) return true;
    if (previous == 3) return false;
    if (previous == 1) {
        InterlockedExchange(&reentrantAttempt, 1);
        return Fail("Reentrant legacy ASI initialization is not supported");
    }
    bool result = false;
    try {
        result = Load(absoluteGameDirectory);
    } catch (const std::bad_alloc&) {
        result = Fail("Out of memory while preparing the legacy ASI manifest; abort startup");
    }
    InterlockedExchange(&loadState, result ? 2 : 3);
    return result;
}

bool HaveLoadedModules() { return LoadedModuleCount() != 0; }
bool ModuleExecutionAttempted() { return InterlockedCompareExchange(&executionAttempted, 0, 0) != 0; }
unsigned LoadedModuleCount() { return static_cast<unsigned>(InterlockedCompareExchange(&loadedCount, 0, 0)); }
const char* LoaderError() { return errorText; }

} // namespace LegacyAsi

#endif
