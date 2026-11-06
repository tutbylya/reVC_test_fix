// Data-only regression: does not load Wine, a PE image, an ASI or game assets.
#include "page_protection.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>

namespace {
unsigned checks;
unsigned failures;

void Check(bool condition, const char* label)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", label);
    }
}
} // namespace

int main()
{
    using LegacyAsiTest::MatchesProtection;
    constexpr std::uint32_t NoAccess = 0x01;
    constexpr std::uint32_t ReadOnly = 0x02;
    constexpr std::uint32_t ReadWrite = 0x04;
    constexpr std::uint32_t WriteCopy = 0x08;
    constexpr std::uint32_t Execute = 0x10;
    constexpr std::uint32_t ExecuteRead = 0x20;
    constexpr std::uint32_t ExecuteReadWrite = 0x40;
    constexpr std::uint32_t ExecuteWriteCopy = 0x80;
    constexpr std::uint32_t Guard = 0x100;
    constexpr std::uint32_t NoCache = 0x200;
    constexpr std::uint32_t WriteCombine = 0x400;
    constexpr std::uint32_t bases[] = {0, NoAccess, ReadOnly, ReadWrite, WriteCopy,
        Execute, ExecuteRead, ExecuteReadWrite, ExecuteWriteCopy, 0x03, 0xFF};
    constexpr std::uint32_t modifiers[] = {0, NoCache, WriteCombine, Guard, Guard | NoCache};

    for (bool image : {false, true}) {
        for (const std::uint32_t modifier : modifiers) {
            for (const std::uint32_t base : bases) {
                const bool guarded = (modifier & Guard) != 0;
                Check(MatchesProtection(base | modifier, ReadWrite, image) ==
                    (!guarded && (base == ReadWrite || (image && base == WriteCopy))),
                    "RW accepts only RW or image COW, never executable or guarded data");
                for (const std::uint32_t expected : {NoAccess, ReadOnly, ExecuteRead}) {
                    Check(MatchesProtection(base | modifier, expected, image) == (!guarded && base == expected),
                        "NOACCESS/R/RX remain exact base permissions");
                }
                for (const std::uint32_t forbidden : {0u, WriteCopy, Execute, ExecuteReadWrite, ExecuteWriteCopy, 0x03u}) {
                    Check(!MatchesProtection(base | modifier, forbidden, image),
                        "unsupported expectation cannot enable executable-write permissions");
                }
            }
        }
    }
    Check(MatchesProtection(WriteCopy, ReadWrite, true), "Wine image COW is writable non-executable data");
    Check(!MatchesProtection(WriteCopy, ReadWrite, false), "private/mapped COW is not accepted as owned image data");
    Check(!MatchesProtection(ExecuteWriteCopy, ReadWrite, true), "execute-copy never qualifies as data");
    Check(!MatchesProtection(ExecuteReadWrite, ReadWrite, true), "RWX never qualifies as data");
    Check(!MatchesProtection(ReadWrite | Guard, ReadWrite, true), "guarded RW remains rejected");
    Check(!MatchesProtection(WriteCopy | Guard, ReadWrite, true), "guarded image COW remains rejected");
    std::printf("Legacy ASI page protections: %u checks, %u failures.\n", checks, failures);
    return failures ? 1 : 0;
}
