#pragma once

#include <cstdint>

namespace LegacyAsiTest {

// Pure Win32 protection-value comparison: no platform headers or API calls.
// The caller must separately verify committed state and allocation ownership.
// Wine reports PAGE_WRITECOPY for PAGE_READWRITE applied to SEC_IMAGE pages;
// that is still writable, non-executable image data, not an RWX exception.
constexpr bool MatchesProtection(std::uint32_t actual, std::uint32_t expected, bool image)
{
    constexpr std::uint32_t BaseMask = 0xFF;
    constexpr std::uint32_t Guard = 0x100;
    constexpr std::uint32_t NoAccess = 0x01;
    constexpr std::uint32_t ReadOnly = 0x02;
    constexpr std::uint32_t ReadWrite = 0x04;
    constexpr std::uint32_t WriteCopy = 0x08;
    constexpr std::uint32_t ExecuteRead = 0x20;
    if (actual & Guard) return false;
    const std::uint32_t base = actual & BaseMask;
    switch (expected) {
    case NoAccess:
    case ReadOnly:
    case ExecuteRead:
        return base == expected;
    case ReadWrite:
        return base == ReadWrite || (image && base == WriteCopy);
    default:
        // Execute, execute-write, execute-copy and unknown expectations are
        // never made acceptable by this data-page equivalence rule.
        return false;
    }
}

} // namespace LegacyAsiTest
