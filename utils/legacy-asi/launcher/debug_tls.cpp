#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "debug_tls.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace LegacyLauncher {
namespace {
TlsPreparation Fail(std::wstring& message, const wchar_t* reason)
{
    message = reason;
    return TlsPreparation::Failed;
}

bool Remote(HANDLE process, ULONG_PTR address, void* data, SIZE_T bytes)
{
    SIZE_T read = 0;
    return ReadProcessMemory(process, reinterpret_cast<const void*>(address), data, bytes, &read) && read == bytes;
}

bool Disk(HANDLE file, std::uint64_t length, std::uint64_t offset, void* data, DWORD bytes)
{
    if (offset > length || bytes > length - offset || offset > INT64_MAX) return false;
    LARGE_INTEGER where;
    where.QuadPart = static_cast<LONGLONG>(offset);
    DWORD read = 0;
    return SetFilePointerEx(file, where, nullptr, FILE_BEGIN) && ReadFile(file, data, bytes, &read, nullptr) && read == bytes;
}

struct DiskImage {
    HANDLE file;
    std::uint64_t length;
    IMAGE_OPTIONAL_HEADER32 optional;
    std::vector<IMAGE_SECTION_HEADER> sections;

    bool ReadRva(DWORD rva, void* bytes, DWORD count) const
    {
        if (rva < optional.SizeOfHeaders)
            return count <= optional.SizeOfHeaders - rva && Disk(file, length, rva, bytes, count);
        for (const IMAGE_SECTION_HEADER& section : sections) {
            if (rva < section.VirtualAddress) continue;
            const std::uint64_t within = static_cast<std::uint64_t>(rva) - section.VirtualAddress;
            if (within <= section.SizeOfRawData && count <= section.SizeOfRawData - within)
                return Disk(file, length, static_cast<std::uint64_t>(section.PointerToRawData) + within, bytes, count);
        }
        return false;
    }

    bool Initialize()
    {
        LARGE_INTEGER size;
        IMAGE_DOS_HEADER dos;
        DWORD signature;
        IMAGE_FILE_HEADER coff;
        if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 512LL * 1024 * 1024) return false;
        length = static_cast<std::uint64_t>(size.QuadPart);
        if (!Disk(file, length, 0, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
            dos.e_lfanew < static_cast<LONG>(sizeof(dos))) return false;
        const std::uint64_t nt = static_cast<DWORD>(dos.e_lfanew);
        if (!Disk(file, length, nt, &signature, sizeof(signature)) || signature != IMAGE_NT_SIGNATURE ||
            !Disk(file, length, nt + sizeof(signature), &coff, sizeof(coff)) ||
            coff.Machine != IMAGE_FILE_MACHINE_I386 || !(coff.Characteristics & IMAGE_FILE_DLL) ||
            coff.NumberOfSections == 0 || coff.NumberOfSections > 96 || coff.SizeOfOptionalHeader < sizeof(optional) ||
            !Disk(file, length, nt + sizeof(signature) + sizeof(coff), &optional, sizeof(optional)) ||
            optional.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC || !optional.SizeOfImage ||
            optional.SizeOfImage > 512UL * 1024 * 1024 || optional.NumberOfRvaAndSizes > IMAGE_NUMBEROF_DIRECTORY_ENTRIES)
            return false;
        sections.resize(coff.NumberOfSections);
        return Disk(file, length, nt + sizeof(signature) + sizeof(coff) + coff.SizeOfOptionalHeader,
            sections.data(), static_cast<DWORD>(sections.size() * sizeof(sections[0])));
    }

    bool HasHighLowRelocation(DWORD field, bool& found) const
    {
        found = false;
        if (optional.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_BASERELOC) return true;
        const IMAGE_DATA_DIRECTORY directory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (!directory.VirtualAddress || !directory.Size) return true;
        if (directory.VirtualAddress >= optional.SizeOfImage || directory.Size > optional.SizeOfImage - directory.VirtualAddress ||
            directory.Size > 32UL * 1024 * 1024) return false;
        DWORD consumed = 0;
        while (consumed < directory.Size) {
            IMAGE_BASE_RELOCATION block;
            if (directory.Size - consumed < sizeof(block) ||
                !ReadRva(directory.VirtualAddress + consumed, &block, sizeof(block)) ||
                block.SizeOfBlock < sizeof(block) || block.SizeOfBlock > directory.Size - consumed ||
                (block.SizeOfBlock - sizeof(block)) % sizeof(WORD)) return false;
            const DWORD entries = (block.SizeOfBlock - sizeof(block)) / sizeof(WORD);
            for (DWORD i = 0; i < entries; ++i) {
                WORD entry;
                if (!ReadRva(directory.VirtualAddress + consumed + sizeof(block) + i * sizeof(entry), &entry, sizeof(entry))) return false;
                const DWORD type = entry >> 12;
                const DWORD offset = entry & 0xFFF;
                if (block.VirtualAddress > UINT32_MAX - offset) return false;
                if (block.VirtualAddress + offset == field) {
                    if (type != IMAGE_REL_BASED_HIGHLOW || found) return false;
                    found = true;
                }
            }
            consumed += block.SizeOfBlock;
        }
        return consumed == directory.Size;
    }
};
} // namespace

TlsPreparation PrependTlsCallback(HANDLE process, HANDLE file, const void* imageBase,
    ULONG_PTR callback, std::wstring& diagnostic)
{
    DiskImage source{file, 0, {}, {}};
    if (!source.Initialize()) return Fail(diagnostic, L"The reviewed ASI has invalid PE32 TLS metadata.");
    const DWORD imageSize = source.optional.SizeOfImage;
    const ULONG_PTR base = reinterpret_cast<ULONG_PTR>(imageBase);
    if (!base || imageSize > UINT32_MAX - base || !callback)
        return Fail(diagnostic, L"The reviewed ASI or native callback address is invalid.");
    if (source.optional.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_TLS) return TlsPreparation::NoTls;
    const IMAGE_DATA_DIRECTORY tlsInfo = source.optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (!tlsInfo.VirtualAddress && !tlsInfo.Size) return TlsPreparation::NoTls;
    if (!tlsInfo.VirtualAddress || tlsInfo.Size < sizeof(IMAGE_TLS_DIRECTORY32) ||
        tlsInfo.VirtualAddress >= imageSize || sizeof(IMAGE_TLS_DIRECTORY32) > imageSize - tlsInfo.VirtualAddress)
        return Fail(diagnostic, L"This profile requires an existing PE32 TLS directory.");
    IMAGE_TLS_DIRECTORY32 diskTls, loadedTls;
    if (!source.ReadRva(tlsInfo.VirtualAddress, &diskTls, sizeof(diskTls)) ||
        !Remote(process, base + tlsInfo.VirtualAddress, &loadedTls, sizeof(loadedTls)))
        return Fail(diagnostic, L"Cannot read the reviewed ASI's TLS directory.");
    if (diskTls.AddressOfIndex < source.optional.ImageBase ||
        diskTls.AddressOfIndex - source.optional.ImageBase > imageSize - sizeof(DWORD))
        return Fail(diagnostic, L"The reviewed ASI's TLS index is not image-owned.");
    const DWORD delta = static_cast<DWORD>(base - source.optional.ImageBase);
    const DWORD relocatedIndex = diskTls.AddressOfIndex + delta;
    const bool pending = base != source.optional.ImageBase && loadedTls.AddressOfIndex == diskTls.AddressOfIndex;
    if (!pending && loadedTls.AddressOfIndex != relocatedIndex)
        return Fail(diagnostic, L"The ASI's relocation state cannot be determined safely.");
    if (loadedTls.AddressOfCallBacks != (pending ? diskTls.AddressOfCallBacks :
        diskTls.AddressOfCallBacks ? diskTls.AddressOfCallBacks + delta : 0))
        return Fail(diagnostic, L"The ASI's TLS callback pointer does not match its image.");
    std::vector<DWORD> callbacks;
    callbacks.push_back(static_cast<DWORD>(callback));
    if (diskTls.AddressOfCallBacks) {
        if (diskTls.AddressOfCallBacks < source.optional.ImageBase ||
            diskTls.AddressOfCallBacks - source.optional.ImageBase > imageSize - sizeof(DWORD))
            return Fail(diagnostic, L"The ASI's existing TLS callback array is not image-owned.");
        const DWORD arrayRva = diskTls.AddressOfCallBacks - source.optional.ImageBase;
        bool terminated = false;
        for (DWORD i = 0; i < 64; ++i) {
            DWORD original;
            if (i * sizeof(DWORD) > imageSize - arrayRva - sizeof(DWORD) ||
                !Remote(process, base + arrayRva + i * sizeof(DWORD), &original, sizeof(original)))
                return Fail(diagnostic, L"Cannot read a bounded existing TLS callback array.");
            if (!original) { terminated = true; break; }
            const DWORD target = pending ? original + delta : original;
            if (target < base || target - base >= imageSize)
                return Fail(diagnostic, L"The ASI has a TLS callback outside its own image.");
            callbacks.push_back(target);
        }
        if (!terminated) return Fail(diagnostic, L"The ASI's existing TLS callback array is too large.");
    }
    callbacks.push_back(0);
    const DWORD fieldRva = tlsInfo.VirtualAddress + offsetof(IMAGE_TLS_DIRECTORY32, AddressOfCallBacks);
    bool pointerRelocates = false;
    if (!source.HasHighLowRelocation(fieldRva, pointerRelocates))
        return Fail(diagnostic, L"The ASI's TLS callback relocation metadata is unsupported.");
    const SIZE_T bytes = callbacks.size() * sizeof(callbacks[0]);
    void* remoteArray = VirtualAllocEx(process, nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!remoteArray) return Fail(diagnostic, L"Cannot allocate the child-owned TLS callback pointer array.");
    SIZE_T written = 0;
    DWORD previousArray = 0;
    if (!WriteProcessMemory(process, remoteArray, callbacks.data(), bytes, &written) || written != bytes ||
        !VirtualProtectEx(process, remoteArray, bytes, PAGE_READONLY, &previousArray)) {
        VirtualFreeEx(process, remoteArray, 0, MEM_RELEASE);
        return Fail(diagnostic, L"Cannot finalize the child-owned read-only TLS callback pointer array.");
    }
    DWORD replacement = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(remoteArray));
    // LOAD_DLL may arrive before the normal loader applies PE relocations.
    // Compensate only if this exact TLS pointer field has a HIGHLOW fixup.
    // The new callback array is outside the image and contains actual VAs.
    if (pending && pointerRelocates) replacement -= delta;
    void* fieldAddress = reinterpret_cast<void*>(base + fieldRva);
    DWORD oldProtection = 0;
    if (!VirtualProtectEx(process, fieldAddress, sizeof(replacement), PAGE_READWRITE, &oldProtection)) {
        VirtualFreeEx(process, remoteArray, 0, MEM_RELEASE);
        return Fail(diagnostic, L"Cannot temporarily write the child ASI's TLS pointer field.");
    }
    written = 0;
    const bool copied = WriteProcessMemory(process, fieldAddress, &replacement, sizeof(replacement), &written) && written == sizeof(replacement);
    DWORD ignored = 0;
    const bool restored = VirtualProtectEx(process, fieldAddress, sizeof(replacement), oldProtection, &ignored) != FALSE;
    if (!copied || !restored)
        return Fail(diagnostic, L"The child ASI's TLS pointer update failed; its startup must abort.");
    // The ASI may use these callbacks until process teardown. Its owner keeps
    // this non-executable data allocation alive; Windows reclaims it at exit.
    return TlsPreparation::Patched;
}
} // namespace LegacyLauncher
