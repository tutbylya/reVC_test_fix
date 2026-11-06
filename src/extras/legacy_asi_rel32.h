#pragma once

#include <cstdint>

namespace LegacyAsi {

// In IA-32 mode CALL/JMP rel32 add their displacement to the next 32-bit EIP.
// That addition wraps modulo 2^32, so a high x86 DLL is not out of range merely
// because its mathematical distance from a low game address exceeds INT32_MAX.
// This helper describes 32-bit mode only, not the limited rel32 reach in x64.
constexpr std::uint32_t Rel32Displacement32(std::uint32_t instruction,
                                          std::uint32_t target) noexcept
{
    return target - (instruction + std::uint32_t{5});
}

} // namespace LegacyAsi
