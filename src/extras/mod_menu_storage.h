#pragma once

#include "mod_menu_state.h"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ModMenu {
namespace StorageDetail {
namespace fs = std::filesystem;

// Only the dedicated sidecar is ever writable. No INI or mod files are
// renamed, removed or edited. Open the sidecar without following symlinks.
inline bool ValidStatePath(const fs::path& file)
{
    return !file.parent_path().empty() && file.filename() == ".revc-disabled";
}

inline bool Parse(const std::string& bytes, DisabledSet& output)
{
    std::istringstream input(bytes);
    return ParseDisabled(input, output);
}

inline unsigned NextTemporaryId()
{
    static std::atomic<unsigned> next{0};
    return next.fetch_add(1, std::memory_order_relaxed);
}

#ifdef _WIN32
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE value) : value(value) {}
    ~Handle() { Close(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    bool Close()
    {
        if (value == INVALID_HANDLE_VALUE) return true;
        const HANDLE old = value;
        value = INVALID_HANDLE_VALUE;
        return CloseHandle(old) != FALSE;
    }
};

inline bool Missing(DWORD error)
{
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

inline bool IsDiskObject(HANDLE file, bool directory)
{
    BY_HANDLE_FILE_INFORMATION info{};
    return GetFileType(file) == FILE_TYPE_DISK && GetFileInformationByHandle(file, &info) &&
        !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        !!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == directory;
}

inline HANDLE OpenParent(const fs::path& file)
{
    return CreateFileW(file.parent_path().c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
}

inline bool ReadFileState(const fs::path& path, DisabledSet& output)
{
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) {
        if (!Missing(GetLastError())) return false;
        output.clear();
        return true;
    }
    LARGE_INTEGER size{};
    if (!IsDiskObject(file.value, false) || !GetFileSizeEx(file.value, &size) || size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) > MaxStateBytes) return false;
    std::string bytes;
    char buffer[4096];
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(file.value, buffer, sizeof(buffer), &count, nullptr) || count > MaxStateBytes - bytes.size()) return false;
        if (!count) break;
        bytes.append(buffer, count);
    }
    return Parse(bytes, output);
}

inline bool ReplaceState(const fs::path& path, const std::string& bytes)
{
    const fs::path parent = path.parent_path();
    for (int attempt = 0; attempt < 32; ++attempt) {
        const fs::path temporary = parent / (L".revc-disabled.tmp." + std::to_wstring(GetCurrentProcessId()) +
            L"." + std::to_wstring(NextTemporaryId()));
        Handle output(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (output.value == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS) continue;
            return false;
        }
        DWORD written = 0;
        const bool flushed = WriteFile(output.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
            written == bytes.size() && FlushFileBuffers(output.value);
        const bool closed = output.Close();
        if (flushed && closed && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        DeleteFileW(temporary.c_str()); // The exact file this call created with CREATE_NEW.
        return false;
    }
    return false;
}
#else
struct Handle {
    int value = -1;
    explicit Handle(int value) : value(value) {}
    ~Handle() { Close(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    bool Close()
    {
        if (value < 0) return true;
        const int old = value;
        value = -1;
        return close(old) == 0;
    }
};

inline bool ReadFileState(int parent, DisabledSet& output)
{
    Handle file(openat(parent, ".revc-disabled", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (file.value < 0) {
        if (errno != ENOENT) return false;
        output.clear();
        return true;
    }
    struct stat status{};
    if (fstat(file.value, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0 ||
        static_cast<unsigned long long>(status.st_size) > MaxStateBytes) return false;
    std::string bytes;
    char buffer[4096];
    for (;;) {
        const ssize_t count = read(file.value, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 || static_cast<std::size_t>(count) > MaxStateBytes - bytes.size()) return false;
        if (!count) break;
        bytes.append(buffer, static_cast<std::size_t>(count));
    }
    return Parse(bytes, output);
}

inline bool ReplaceState(int parent, const std::string& bytes)
{
    for (int attempt = 0; attempt < 32; ++attempt) {
        const std::string temporary = ".revc-disabled.tmp." + std::to_string(getpid()) + "." + std::to_string(NextTemporaryId());
        Handle output(openat(parent, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (output.value < 0) {
            if (errno == EEXIST) continue;
            return false;
        }
        std::size_t position = 0;
        while (position < bytes.size()) {
            const ssize_t written = write(output.value, bytes.data() + position, bytes.size() - position);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) break;
            position += static_cast<std::size_t>(written);
        }
        const bool flushed = position == bytes.size() && fsync(output.value) == 0;
        const bool closed = output.Close();
        if (flushed && closed && renameat(parent, temporary.c_str(), parent, ".revc-disabled") == 0) {
            fsync(parent); // Rename succeeded; directory sync is best effort.
            return true;
        }
        unlinkat(parent, temporary.c_str(), 0); // Only our O_EXCL temporary.
        return false;
    }
    return false;
}
#endif
} // namespace StorageDetail

// Missing is an empty state. Malformed, unreadable or linked files fail closed
// and never replace the caller's existing state with a partial parse.
inline bool ReadDisabled(const std::filesystem::path& file, DisabledSet& output)
{
    using namespace StorageDetail;
    if (!ValidStatePath(file)) return false;
#ifdef _WIN32
    Handle parent(OpenParent(file));
    if (parent.value == INVALID_HANDLE_VALUE) {
        if (!Missing(GetLastError())) return false;
        output.clear();
        return true;
    }
    return IsDiskObject(parent.value, true) && ReadFileState(file, output);
#else
    Handle parent(open(file.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (parent.value < 0) {
        if (errno != ENOENT) return false;
        output.clear();
        return true;
    }
    return ReadFileState(parent.value, output);
#endif
}

inline bool SetEnabled(const std::filesystem::path& file, const std::string& id, bool enabled)
{
    using namespace StorageDetail;
    if (!ValidStatePath(file) || !IsValidId(id) || NormalizeId(id) != id) return false;
    DisabledSet state;
#ifdef _WIN32
    Handle parent(OpenParent(file));
    if (parent.value == INVALID_HANDLE_VALUE || !IsDiskObject(parent.value, true)) return false;
    // Serialize read/modify/replace between game instances. No sharing until
    // our handle closes; the harmless hidden file is never overwritten.
    Handle lock(CreateFileW((file.parent_path() / L".revc-disabled.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (lock.value == INVALID_HANDLE_VALUE || !IsDiskObject(lock.value, false) || !ReadFileState(file, state)) return false;
#else
    Handle parent(open(file.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (parent.value < 0 || flock(parent.value, LOCK_EX | LOCK_NB) != 0 || !ReadFileState(parent.value, state)) return false;
#endif
    if (enabled ? state.erase(id) == 0 : !state.insert(id).second) return true;
    std::string bytes;
    if (!EncodeDisabled(state, bytes)) return false;
#ifdef _WIN32
    return ReplaceState(file, bytes);
#else
    return ReplaceState(parent.value, bytes);
#endif
}

} // namespace ModMenu
