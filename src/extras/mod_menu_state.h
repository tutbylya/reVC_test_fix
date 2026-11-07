#pragma once

#include <cstddef>
#include <istream>
#include <set>
#include <string>

// C++14: this data-only part is shared with the separately built Mod Loader.
namespace ModMenu {

using DisabledSet = std::set<std::string>;
constexpr std::size_t MaxStateBytes = 64 * 1024;
constexpr std::size_t MaxIdBytes = 1020;
constexpr std::size_t MaxStateEntries = 2048;

struct ResourceRow {
    std::string id, name;
    bool loaded = false;
    bool enabled = false;
    bool manageable = false;
    bool pending = false;
};

inline std::string NormalizeId(std::string id)
{
    for (char& ch : id)
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    return id;
}

inline bool IsValidId(const std::string& id)
{
    if (id.empty() || id.size() > MaxIdBytes || id == "." || id == ".." ||
        id.find_first_of("<>:\"/\\|?*") != std::string::npos) return false;
    for (std::size_t pos = 0; pos < id.size();) {
        const unsigned char lead = static_cast<unsigned char>(id[pos++]);
        unsigned value = lead, tail = 0, minimum = 0;
        if (lead >= 0x80) {
            if (lead >= 0xC2 && lead <= 0xDF) { value = lead & 0x1F; tail = 1; minimum = 0x80; }
            else if (lead >= 0xE0 && lead <= 0xEF) { value = lead & 0x0F; tail = 2; minimum = 0x800; }
            else if (lead >= 0xF0 && lead <= 0xF4) { value = lead & 7; tail = 3; minimum = 0x10000; }
            else return false;
            if (tail > id.size() - pos) return false;
            while (tail--) {
                const unsigned char byte = static_cast<unsigned char>(id[pos++]);
                if ((byte & 0xC0) != 0x80) return false;
                value = (value << 6) | (byte & 0x3F);
            }
            if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) return false;
        }
        if (value < 0x20 || (value >= 0x7F && value <= 0x9F)) return false;
    }
    return true;
}

inline int HexDigit(char digit)
{
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
    if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
    return -1;
}

// One hex-encoded, ASCII-case-folded UTF-8 filename per line. Blank lines are
// harmless. Strictly bounded before allocation; never use a partial bad list.
inline bool ParseDisabled(std::istream& stream, DisabledSet& output)
{
    DisabledSet parsed;
    std::string line;
    const auto finish = [&]() {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) return true;
        if (line.size() % 2 || line.size() > MaxIdBytes * 2 || parsed.size() >= MaxStateEntries) return false;
        std::string id;
        id.reserve(line.size() / 2);
        for (std::size_t i = 0; i < line.size(); i += 2) {
            const int high = HexDigit(line[i]), low = HexDigit(line[i + 1]);
            if (high < 0 || low < 0) return false;
            id.push_back(static_cast<char>((high << 4) | low));
        }
        if (!IsValidId(id)) return false;
        return parsed.insert(NormalizeId(id)).second;
    };
    std::size_t bytes = 0;
    char ch;
    while (stream.get(ch)) {
        if (++bytes > MaxStateBytes) return false;
        if (ch == '\n') {
            if (!finish()) return false;
            line.clear();
        } else {
            if (line.size() >= MaxIdBytes * 2 + 1) return false;
            line.push_back(ch);
        }
    }
    if (!stream.eof() || stream.bad() || !finish()) return false;
    output.swap(parsed);
    return true;
}

inline bool EncodeDisabled(const DisabledSet& state, std::string& output)
{
    if (state.size() > MaxStateEntries) return false;
    static const char hex[] = "0123456789abcdef";
    std::string encoded;
    for (const auto& id : state) {
        if (!IsValidId(id) || NormalizeId(id) != id || id.size() * 2 + 1 > MaxStateBytes - encoded.size()) return false;
        for (unsigned char byte : id) {
            encoded.push_back(hex[byte >> 4]);
            encoded.push_back(hex[byte & 15]);
        }
        encoded.push_back('\n');
    }
    output.swap(encoded);
    return true;
}

} // namespace ModMenu
