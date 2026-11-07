// Pure glyph conversion regression. No engine or game files required.
#include "../../src/core/frontend_mod_text.h"

#include <cassert>

int main()
{
    using namespace FrontendModText;
    Text plain;
    plain.AppendUtf8("one~r~two\n", Atlas::Latin);
    assert(plain.size == 10 && plain.glyphs[3] == '_' && plain.glyphs[5] == '_' &&
           plain.glyphs[9] == '?' && plain.glyphs[10] == 0);
    Text russian;
    russian.AppendUtf8("\xD0\x91\xD1\x91", Atlas::Russian);
    assert(russian.size == 2 && russian.glyphs[0] == 0x80 && russian.glyphs[1] == 'e');
    Text malformed;
    malformed.AppendUtf8(std::string("\xE2~\0", 3), Atlas::Latin);
    assert(malformed.size == 3 && malformed.glyphs[0] == '?' && malformed.glyphs[1] == '_' &&
           malformed.glyphs[2] == '?' && malformed.glyphs[3] == 0);
    Text reserved;
    reserved.AppendUtf8("*\xC1\xBE", Atlas::Latin); // Leading marker and overlong tilde.
    assert(reserved.size == 4 && reserved.glyphs[0] == ' ' && reserved.glyphs[1] == '*' &&
           reserved.glyphs[2] == '?' && reserved.glyphs[3] == '?' && reserved.glyphs[4] == 0);
    Text clipped;
    clipped.AppendUtf8(std::string(1000, 'x'), Atlas::Latin);
    assert(clipped.size == Text::Capacity && clipped.glyphs[Text::Capacity] == 0);
    assert(Glyph('t', Atlas::Ukrainian) == 'y' && Glyph(0x0442, Atlas::Ukrainian) == 't');
    assert(Glyph(0x10FFFF, Atlas::Russian) == '?');
}
