// Pure arithmetic/byte fixtures. No DLL, game, mapped address or branch runs.
#include "../../../src/extras/legacy_asi_rel32.h"

#include <cstdint>
#include <cstdio>

namespace {
unsigned checks;
unsigned failures;

void Check(bool condition, const char* name)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", name);
    }
}

struct Case {
    std::uint32_t instruction;
    std::uint32_t target;
    std::uint32_t displacement;
};
}

int main()
{
    constexpr Case cases[] = {
        {0x00001000, 0x00002000, 0x00000FFB},
        {0x00002000, 0x00001000, 0xFFFFEFFB},
        {0x00001000, 0x00001000, 0xFFFFFFFB},
        {0x00000000, 0x80000004, 0x7FFFFFFF},
        {0x00000000, 0x80000005, 0x80000000},
        {0x00000000, 0x80000006, 0x80000001},
        {0x80000000, 0x00000005, 0x80000000},
        {0x10000000, 0xF0000000, 0xDFFFFFFB},
        {0xF0000000, 0x10000000, 0x1FFFFFFB},
        {0xFFFFFFFB, 0x00000000, 0x00000000},
        {0xFFFFFFFC, 0x00000000, 0xFFFFFFFF},
        {0xFFFFFFFE, 0x00000010, 0x0000000D},
        {0x00000000, 0xFFFFFFFF, 0xFFFFFFFA},
    };
    for (const Case& fixture : cases) {
        const std::uint32_t displacement = LegacyAsi::Rel32Displacement32(fixture.instruction, fixture.target);
        Check(displacement == fixture.displacement, "exact rel32 bits, including high target and wrap");
        // Decode independently as a signed immediate added to the next EIP.
        const std::int64_t signedImmediate = (displacement & 0x80000000U) ?
            static_cast<std::int64_t>(displacement) - 0x100000000LL : displacement;
        const std::int64_t sum = static_cast<std::int64_t>(fixture.instruction) + 5 + signedImmediate;
        Check(static_cast<std::uint32_t>(sum) == fixture.target, "32-bit EIP decode reaches exact target");
        unsigned char bytes[4];
        for (unsigned index = 0; index != 4; ++index) {
            bytes[index] = static_cast<unsigned char>(displacement >> (index * 8));
            Check(bytes[index] == static_cast<unsigned char>(fixture.displacement >> (index * 8)),
                  "little-endian immediate byte matches fixture");
        }
        const std::uint32_t reconstructed = static_cast<std::uint32_t>(bytes[0]) |
            (static_cast<std::uint32_t>(bytes[1]) << 8) |
            (static_cast<std::uint32_t>(bytes[2]) << 16) |
            (static_cast<std::uint32_t>(bytes[3]) << 24);
        Check(reconstructed == fixture.displacement, "immediate bytes reconstruct exact displacement");
    }
    std::printf("Legacy x86 rel32: %u checks, %u failures. No branch or ASI executed.\n", checks, failures);
    return failures ? 1 : 0;
}
