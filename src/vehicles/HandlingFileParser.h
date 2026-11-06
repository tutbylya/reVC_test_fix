#pragma once

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace HandlingFile {

enum class LineResult { End, Ready, TooLong, EmbeddedNul };
class Lines {
    const char* data_;
    std::size_t length_, position_ = 0;
public:
    Lines(const char* data, std::size_t length) : data_(data), length_(length) {}
    LineResult Next(char* line, std::size_t capacity)
    {
        if (!data_ || position_ >= length_ || !line || !capacity) return LineResult::End;
        std::size_t copied = 0;
        bool longLine = false, nul = false;
        while (position_ < length_ && data_[position_] != '\n') {
            const char c = data_[position_++];
            if (!c) nul = true;
            if (copied + 1 < capacity) line[copied++] = c;
            else longLine = true;
        }
        if (position_ < length_) ++position_;
        if (copied && line[copied - 1] == '\r') --copied;
        line[copied] = 0;
        return nul ? LineResult::EmbeddedNul : longLine ? LineResult::TooLong : LineResult::Ready;
    }
};

enum class Kind { Ignore, End, Main, Bike, Boat, Flying, Invalid };
struct Record {
    Kind kind = Kind::Invalid;
    char* fields[33] = {};
    unsigned count = 0;
    unsigned NameIndex() const { return kind == Kind::Main ? 0 : 1; }
};

inline bool VehicleMatches(Kind kind, int id, int count, int bikeBegin, int bikeEnd,
    int boatBegin, int boatEnd, int flyingBegin, int flyingEnd)
{
    return id >= 0 && id < count &&
        (kind != Kind::Bike || (id >= bikeBegin && id <= bikeEnd)) &&
        (kind != Kind::Boat || (id >= boatBegin && id <= boatEnd)) &&
        (kind != Kind::Flying || (id >= flyingBegin && id <= flyingEnd));
}

inline bool Space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f'; }
inline bool Integer(const char* token)
{
    char* end;
    errno = 0;
    const long n = std::strtol(token, &end, 10);
    return token != end && !*end && errno != ERANGE && n >= INT_MIN && n <= INT_MAX;
}

// Tokenize/validate a complete record before the caller mutates handling data
// or converts its units. The old strtok/atof permissiveness is unsafe for a
// malformed mod, but no arbitrary physics ranges are imposed on valid floats.
inline Record Parse(char* line)
{
    Record record;
    while (Space(*line)) ++line;
    if (!*line) { record.kind = Kind::Ignore; return record; }
    if (*line == ';') {
        char* end = line + std::strlen(line);
        while (end > line && Space(end[-1])) --end;
        *end = 0;
        record.kind = std::strcmp(line, ";the end") == 0 ? Kind::End : Kind::Ignore;
        return record;
    }
    record.kind = *line == '!' ? Kind::Bike : *line == '%' ? Kind::Boat : *line == '$' ? Kind::Flying : Kind::Main;
    for (char* token = line; *token;) {
        while (Space(*token)) ++token;
        if (!*token || *token == ';') break;
        if (record.count == 33) { record.kind = Kind::Invalid; return record; }
        record.fields[record.count++] = token;
        while (*token && !Space(*token)) ++token;
        if (*token) *token++ = 0;
    }
    const unsigned expected = record.kind == Kind::Main ? 33 : record.kind == Kind::Bike ? 17 : record.kind == Kind::Boat ? 16 : 20;
    if (record.count != expected || (record.kind != Kind::Main && std::strlen(record.fields[0]) != 1)) {
        record.kind = Kind::Invalid;
        return record;
    }
    for (unsigned i = record.NameIndex() + 1; i < record.count; ++i) {
        const char* token = record.fields[i];
        if (record.kind == Kind::Main && (i == 15 || i == 16)) {
            const char* allowed = i == 15 ? "FR4fr" : "PDEpde";
            if (token[1] || !std::strchr(allowed, token[0])) { record.kind = Kind::Invalid; return record; }
            continue;
        }
        char* end;
        errno = 0;
        if (record.kind == Kind::Main && i == 30) {
            const unsigned long flags = std::strtoul(token, &end, 16);
            if (token == end || *end || errno == ERANGE || flags > 0xFFFFFFFFUL || *token == '-') { record.kind = Kind::Invalid; return record; }
            continue;
        }
        const double number = std::strtod(token, &end);
        if (token == end || *end || errno == ERANGE || !std::isfinite(number) || !std::isfinite(static_cast<float>(number))) {
            record.kind = Kind::Invalid;
            return record;
        }
        if (record.kind == Kind::Main && (i == 8 || i == 12 || i == 19 || i == 25 || i == 31 || i == 32) && !Integer(token)) {
            record.kind = Kind::Invalid;
            return record;
        }
        // These three bounds prevent division by zero / int8 wrap / indexing
        // beyond the five forward transmission gears, not stylistic tuning.
        if (record.kind == Kind::Main && ((i == 1 && number <= 0) ||
            (i == 8 && (number <= 0 || number > 127)) || (i == 12 && (number < 1 || number > 5)))) {
            record.kind = Kind::Invalid;
            return record;
        }
    }
    return record;
}

} // namespace HandlingFile
