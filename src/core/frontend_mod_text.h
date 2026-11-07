#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace FrontendModText {

enum class Atlas { Latin, Russian, Ukrainian };

// Vice City's fonts use glyph slots, not Unicode code points. Never pass file
// names directly to CFont: a tilde opens a formatting token, even in width
// measurement, and arbitrary code points may index outside the font table.
inline std::uint16_t Glyph(std::uint32_t cp, Atlas atlas)
{
    if (cp == '~') return '_';
    if (atlas == Atlas::Ukrainian && cp == 't') return 'y';
    if (cp >= 0x20 && cp <= 0x7E) return static_cast<std::uint16_t>(cp);
    if (atlas == Atlas::Latin) return '?';
    if (cp == 0x0401) return 'E'; // Yo shares the E glyph in these atlases.
    if (cp == 0x0451) return 'e';
    if (atlas == Atlas::Ukrainian) {
        switch (cp) {
        case 0x0404: return 0x94;
        case 0x0406: return 'I';
        case 0x0407: return 0x96;
        case 0x0454: return 0xAB;
        case 0x0456: return 'i';
        case 0x0457: return 0xAF;
        case 0x0442: return 't';
        case 0x042D: case 0x042A: case 0x044D: case 0x044A: return '?';
        default: break;
        }
    }
    // utils/gxt/rus_ukr/tables/vc_table_rus_1c.txt, U+0410..U+044F.
    static constexpr std::uint16_t russian[] = {
        0x41,0x80,0x81,0x82,0x83,0x45,0x84,0x85,0x86,0x87,0x4B,0x88,0x89,0x8A,0x4F,0x8B,
        0x50,0x43,0x54,0x8C,0x8D,0x58,0x8E,0x8F,0x90,0x91,0x96,0x92,0x93,0x94,0x95,0xAD,
        0x61,0x97,0x98,0x99,0x9A,0x65,0x9B,0x9C,0x9D,0x9E,0x6B,0x9F,0xA0,0xA1,0x6F,0xA2,
        0x70,0x63,0x79,0xA3,0xA4,0x78,0xA5,0xA6,0xA7,0xA8,0xAF,0xA9,0xAA,0xAB,0xAC,0xAE,
    };
    return cp >= 0x0410 && cp <= 0x044F ? russian[cp - 0x0410] : '?';
}

struct Text {
    static constexpr std::size_t Capacity = 96;
    std::uint16_t glyphs[Capacity + 1]{};
    std::size_t size = 0;

    void Append(const char16_t* value, Atlas atlas)
    {
        while (*value && size < Capacity) glyphs[size++] = Glyph(*value++, atlas);
        glyphs[size] = 0;
    }

    void AppendUtf8(const std::string& value, Atlas atlas)
    {
        for (std::size_t i = 0; i < value.size() && size < Capacity;) {
            const unsigned lead = static_cast<unsigned char>(value[i++]);
            std::uint32_t cp = lead;
            if (lead >= 0x80) {
                const unsigned count = lead >= 0xC2 && lead <= 0xDF ? 1 :
                    lead >= 0xE0 && lead <= 0xEF ? 2 : lead >= 0xF0 && lead <= 0xF4 ? 3 : 0;
                const std::uint32_t minimum = count == 1 ? 0x80 : count == 2 ? 0x800 : 0x10000;
                cp = count ? lead & (0x7F >> (count + 1)) : 0xFFFD;
                bool valid = count && count <= value.size() - i;
                for (unsigned n = 0; valid && n < count; ++n) {
                    const unsigned next = static_cast<unsigned char>(value[i + n]);
                    if ((next & 0xC0) != 0x80) valid = false;
                    else cp = (cp << 6) | (next & 0x3F);
                }
                if (valid) {
                    i += count;
                    valid = cp >= minimum && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF);
                }
                if (!valid) cp = 0xFFFD;
            }
            if (!size && cp == '*') glyphs[size++] = ' '; // CFont hides strings starting with '*'.
            glyphs[size++] = Glyph(cp, atlas);
        }
        glyphs[size] = 0;
    }
};

} // namespace FrontendModText
