// Loader-only regression host. Runs locally built source fixtures, never a game
// or a downloaded ASI. Each invocation tests exactly one fresh process state.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>

#if !defined(_M_IX86)
#error The legacy ASI loader regressions require MSVC x86.
#endif

#ifdef LOADER_FAILURE_FIXTURE

// Simulate an ASI whose DllMain performs work and then rejects loading. This
// proves that zero retained HMODULEs does not imply zero executed plugin code.
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        *reinterpret_cast<volatile float*>(0x0097F264) = 2.0f;
        return FALSE;
    }
    return TRUE;
}

#else

#include "legacy_asi_loader.h"
#include "legacy_asi_memory.h"
#include "canonical_test_arena.h"

#include <cstdio>
#include <cwchar>
#include <string>

namespace {
constexpr std::uintptr_t ProcessEvent = 0x004A5DA0;
constexpr std::uintptr_t TimeScale = 0x0097F264;
unsigned checks;
unsigned failures;
unsigned originalEvents;

void Check(bool condition, const char* message)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s (loader: %s; memory: %s; Windows: %lu)\n",
            message, LegacyAsi::LoaderError(), LegacyAsi::MemoryError(), GetLastError());
    }
}

void OriginalEvent() { ++originalEvents; }

void PrintAddressCollision()
{
    std::fprintf(stderr, "Cannot reserve original VC addresses: %s. No ASI was executed; do not retry for ASLR luck.\n",
        LegacyAsi::MemoryError());
    std::uintptr_t address = LegacyAsi::MemoryBegin;
    while (address < LegacyAsi::MemoryEnd) {
        MEMORY_BASIC_INFORMATION info = {};
        if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info))) break;
        const std::uintptr_t end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (info.State != MEM_FREE)
            std::fprintf(stderr, "Occupied %p..%p; owner=%p; state=0x%lX; type=0x%lX.\n",
                info.BaseAddress, reinterpret_cast<void*>(end), info.AllocationBase, info.State, info.Type);
        if (end <= address) break;
        address = end;
    }
}

unsigned FixtureValue(HMODULE module, const char* functionName)
{
    auto function = reinterpret_cast<unsigned (__cdecl*)()>(GetProcAddress(module, functionName));
    Check(function != nullptr, functionName);
    return function ? function() : 0;
}

void CheckFixture(HMODULE module, unsigned identity, unsigned eventCount)
{
    if (!module) return;
    Check(FixtureValue(module, "FixtureIdentity") == identity, "fixture identity");
    Check(FixtureValue(module, "FixtureInstallSucceeded") == 1, "fixture installed its original-address CALL hook");
    Check(FixtureValue(module, "FixtureEventCount") == eventCount, "fixture callback count");
}

void CheckSourceLocks(const std::wstring& directory, bool expectLocked)
{
    const std::wstring prefix = directory + L"\\legacy-asi\\";
    WIN32_FIND_DATAW entry = {};
    HANDLE search = FindFirstFileW((prefix + L"*.asi").c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        Check(code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND,
            "enumerate case-owned source files");
        return;
    }
    do {
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring path = prefix + entry.cFileName;
        HANDLE writable = CreateFileW(path.c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        const DWORD code = GetLastError();
        if (expectLocked) {
            Check(writable == INVALID_HANDLE_VALUE && code == ERROR_SHARING_VIOLATION,
                "executed-module source remains read-locked through process lifetime");
        } else {
            Check(writable != INVALID_HANDLE_VALUE,
                "preflight-only source handles were released without execution");
        }
        if (writable != INVALID_HANDLE_VALUE) CloseHandle(writable); // Never write to the file.
    } while (FindNextFileW(search, &entry));
    Check(GetLastError() == ERROR_NO_MORE_FILES, "complete source lock enumeration");
    FindClose(search);
}

bool Run(const wchar_t* directory, bool expectedSuccess, unsigned expectedCount, bool expectedExecution)
{
    using namespace LegacyAsi;
    // Preflight-only cases must not execute any DLL, so they must not depend
    // on the unrelated availability of the original game's virtual addresses.
    // Executing fixture cases still require the real arena and fail on a
    // collision; it is never treated as a pass or retried for ASLR luck.
    if (expectedExecution) {
        if (!InitializeMemory()) {
            PrintAddressCollision();
            return false;
        }
        const float initialScale = 1.0f;
        if (!MapMutableData(TimeScale, &initialScale, sizeof(initialScale)) ||
            !MapEvent(ProcessEvent, OriginalEvent) || !FinalizeMemory()) {
            Check(false, "prepare original timer scalar and process CALL site");
            ReleaseMemory();
            return false;
        }
    }
    Check(!ModuleExecutionAttempted() && !HaveLoadedModules(), "fresh process has no plugin execution");
    const bool result = LoadEnabledModules(directory);
    Check(result == expectedSuccess, "manifest result matches expected outcome");
    Check(LoadedModuleCount() == expectedCount, "retained module count matches expected outcome");
    Check(HaveLoadedModules() == (expectedCount != 0), "HaveLoadedModules agrees with count");
    Check(ModuleExecutionAttempted() == expectedExecution, "execution-attempt latch matches expected outcome");
    if (!result) Check(LoaderError()[0] != '\0', "failed load reports a concrete diagnostic");

    HMODULE first = GetModuleHandleW(L"fixture_one.asi");
    HMODULE second = GetModuleHandleW(L"fixture_two.asi");
    Check((first != nullptr) == (expectedCount >= 1), "first fixture was not loaded during failed preflight");
    Check((second != nullptr) == (expectedCount >= 2), "second fixture presence");
    CheckFixture(first, 1, 0);
    CheckFixture(second, 2, 0);

    Check(LoadEnabledModules(L"C:\\this-directory-is-not-used-on-a-second-call") == expectedSuccess,
        "second initialization never parses another directory or reloads modules");
    Check(LoadedModuleCount() == expectedCount && ModuleExecutionAttempted() == expectedExecution,
        "second initialization preserves module count and execution state");
    CheckFixture(first, 1, 0);
    CheckFixture(second, 2, 0);
    if (expectedExecution) {
        Check(DispatchEvent(ProcessEvent), "dispatch original process CALL after manifest handling");
        Check(originalEvents == 1, "hook chain invokes original event exactly once");
        CheckFixture(first, 1, 1);
        CheckFixture(second, 2, 1);
        const float increment = expectedCount == 2 ? 0.375f : expectedCount == 1 ? 0.125f : 0.0f;
        const float startingValue = !expectedSuccess ? 2.0f : 1.0f;
        const float* scale = static_cast<const float*>(ReadMemory(TimeScale, sizeof(float)));
        Check(scale && *scale == startingValue + increment,
            "unchanged fixture bytes perform original-address writes, including a rejecting DllMain");
    } else {
        Check(originalEvents == 0, "preflight-only case never called a fixture event");
    }
    CheckSourceLocks(directory, expectedExecution);

    // This harness follows the game's process-lifetime policy: never unload
    // these modules or unmap their callbacks merely to tidy up a test run.
    if (expectedExecution && !ModuleExecutionAttempted()) ReleaseMemory();
    return failures == 0;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc != 5) {
        std::fprintf(stderr, "Usage: loader_tests.exe ABSOLUTE_CASE_DIRECTORY expected-success expected-count expected-execution\n");
        return 2;
    }
    const bool expectedSuccess = std::wcscmp(argv[2], L"1") == 0;
    const unsigned expectedCount = static_cast<unsigned>(std::wcstoul(argv[3], nullptr, 10));
    const bool expectedExecution = std::wcscmp(argv[4], L"1") == 0;
    // The dedicated test PE owns .compat before Windows allocates the CRT's
    // stacks/heaps. Do not race those allocations with a later VirtualAlloc.
    // Preflight-only cases still do not initialize or execute an ABI facade.
    if (expectedExecution && !AdoptCanonicalTestArena()) {
        std::fprintf(stderr, "Cannot adopt the canonical test image (Windows: %lu). No ASI was executed.\n", GetLastError());
        return 1;
    }
    const bool completed = Run(argv[1], expectedSuccess, expectedCount, expectedExecution);
    std::printf("Loader case: %u checks, %u failures.\n", checks, failures);
    return completed ? 0 : 1;
}

#endif
