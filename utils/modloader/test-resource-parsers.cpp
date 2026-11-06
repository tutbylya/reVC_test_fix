#include "../../src/core/FileReadBuffer.h"
#include "../../src/vehicles/HandlingFileParser.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
unsigned checks;
void Check(bool condition, const char* reason)
{
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", reason); std::exit(1); }
}
const char Car[] = "BANSHEE 2350.0 2.2 4.5 1.5 0.0 0.0 -0.3 70 1.15 0.90 0.48 5 250.0 35.0 R P 18.5 0.50 0 30.0 2.0 0.20 0.40 0.05 895000 0.3 -0.18 0.5 0.5 1008181 0 0";
HandlingFile::Record Parse(std::string& text) { return HandlingFile::Parse(&text[0]); }

struct Reader {
    std::vector<unsigned char> data;
    std::size_t position = 0;
    std::ptrdiff_t operator()(unsigned char* destination, std::size_t size)
    {
        const std::size_t bytes = std::min(size, data.size() - position);
        if (bytes) std::copy_n(data.data() + position, bytes, destination);
        position += bytes;
        return static_cast<std::ptrdiff_t>(bytes);
    }
};

void BufferTests()
{
    for (const std::size_t fileSize : {std::size_t(0), std::size_t(1), std::size_t(16383), std::size_t(16384), std::size_t(19999), std::size_t(20000), std::size_t(40000)}) {
        Reader reader{std::vector<unsigned char>(fileSize, 0x7A)};
        std::vector<unsigned char> guarded(20002, 0xCC);
        const auto loaded = FileReadBuffer::Read(guarded.data() + 1, 20000,
            [&](unsigned char* target, std::size_t bytes) { return reader(target, bytes); });
        Check(guarded.front() == 0xCC && guarded.back() == 0xCC, "bounded read preserves canaries");
        Check(fileSize < 20000 ? loaded == static_cast<std::ptrdiff_t>(fileSize) : loaded == -1, "exact fit/oversize byte count");
        Check(guarded[fileSize < 20000 ? fileSize + 1 : 1] == 0, "success/failure is NUL terminated");
    }
    unsigned char tiny = 0xCC;
    Reader empty;
    Check(FileReadBuffer::Read(&tiny, 1, [&](unsigned char* p, std::size_t n) { return empty(p, n); }) == 0 && tiny == 0, "one-byte empty buffer");
    Check(FileReadBuffer::Read(&tiny, 0, [](unsigned char*, std::size_t) { return 0; }) == -1, "zero capacity rejected");
    Check(FileReadBuffer::Read(nullptr, 10, [](unsigned char*, std::size_t) { return 0; }) == -1, "null buffer rejected");
    unsigned char errorBuffer[8] = {0xCC};
    Check(FileReadBuffer::Read(errorBuffer, 8, [](unsigned char*, std::size_t) { return -1; }) == -1 && !errorBuffer[0], "read error rejected");
    Check(FileReadBuffer::Read(errorBuffer, 8, [](unsigned char*, std::size_t n) { return static_cast<std::ptrdiff_t>(n + 1); }) == -1, "invalid reader count rejected");
}

void LineTests()
{
    std::string text = "one\r\n\nlast";
    HandlingFile::Lines lines(text.data(), text.size());
    char line[8];
    Check(lines.Next(line, sizeof(line)) == HandlingFile::LineResult::Ready && std::string(line) == "one", "CRLF trimmed");
    Check(lines.Next(line, sizeof(line)) == HandlingFile::LineResult::Ready && !line[0], "empty line");
    Check(lines.Next(line, sizeof(line)) == HandlingFile::LineResult::Ready && std::string(line) == "last", "final line needs no LF");
    Check(lines.Next(line, sizeof(line)) == HandlingFile::LineResult::End, "bounded final EOF");
    text = std::string(1000, 'x') + "\nOK\n";
    HandlingFile::Lines oversized(text.data(), text.size());
    Check(oversized.Next(line, sizeof(line)) == HandlingFile::LineResult::TooLong, "oversized line rejected");
    Check(oversized.Next(line, sizeof(line)) == HandlingFile::LineResult::Ready && std::string(line) == "OK", "oversized line advances safely");
    text = std::string("bad\0line\nOK", 11);
    HandlingFile::Lines embedded(text.data(), text.size());
    Check(embedded.Next(line, sizeof(line)) == HandlingFile::LineResult::EmbeddedNul, "embedded NUL rejected");
    Check(embedded.Next(line, sizeof(line)) == HandlingFile::LineResult::Ready && std::string(line) == "OK", "NUL line advances safely");
}

void RecordTests()
{
    std::string text = Car;
    auto record = Parse(text);
    Check(record.kind == HandlingFile::Kind::Main && record.count == 33, "Ferrari 33-field record accepted");
    text = std::string(Car) + " ; harmless comment";
    Check(Parse(text).kind == HandlingFile::Kind::Main, "inline comment accepted");
    text = std::string(Car).substr(0, std::string(Car).find_last_of(' '));
    Check(Parse(text).kind == HandlingFile::Kind::Invalid, "partial record rejected before conversion");
    text = std::string(Car) + " 9";
    Check(Parse(text).kind == HandlingFile::Kind::Invalid, "extra fields rejected");
    for (const char* bad : {"nan", "inf", "1e309", "1e100", "wrong"}) {
        text = Car;
        text.replace(text.find("2350.0"), 6, bad);
        Check(Parse(text).kind == HandlingFile::Kind::Invalid, "invalid float rejected");
    }
    text = Car;
    text.replace(text.find(" 5 250.0"), 2, " 6");
    Check(Parse(text).kind == HandlingFile::Kind::Invalid, "sixth forward gear rejected");
    text = "! BIKE 0.33 0.15 0.28 0.15 45 38 0.93 0.70 0.5 0.1 35 -40 -0.009 0.7 0.6";
    Check(Parse(text).kind == HandlingFile::Kind::Bike, "bike 17-field record");
    text = "% PREDATOR 0.79 0.5 0.6 7 0.6 -1.9 4 0.8 0.998 0.998 0.85 0.98 0.97 4";
    Check(Parse(text).kind == HandlingFile::Kind::Boat, "boat 16-field record");
    text = "$ SEAPLANE 0.0032 0.97 -0.00018 0.006 0.10 0.002 -0.007 0.0002 0.008 0.012 0.19 1 0.998 0.997 0.990 0 200 0";
    Check(Parse(text).kind == HandlingFile::Kind::Flying, "flying 20-field record");
    text = "  ;the end  ";
    Check(Parse(text).kind == HandlingFile::Kind::End, "EOF marker with whitespace");
    text = " \t";
    Check(Parse(text).kind == HandlingFile::Kind::Ignore, "whitespace ignored");
    Check(!HandlingFile::VehicleMatches(HandlingFile::Kind::Main, 106, 106, 81, 85, 86, 96, 96, 105), "unknown name sentinel rejected");
    Check(!HandlingFile::VehicleMatches(HandlingFile::Kind::Bike, 27, 106, 81, 85, 86, 96, 96, 105), "car name cannot index bike table");
    Check(!HandlingFile::VehicleMatches(HandlingFile::Kind::Flying, -1, 106, 81, 85, 86, 96, 96, 105), "negative ID rejected");
    Check(HandlingFile::VehicleMatches(HandlingFile::Kind::Boat, 96, 106, 81, 85, 86, 96, 96, 105) &&
        HandlingFile::VehicleMatches(HandlingFile::Kind::Flying, 96, 106, 81, 85, 86, 96, 96, 105), "seaplane supports boat and flying records");
}

void StockTests(const char* filename)
{
    std::ifstream input(filename, std::ios::binary);
    Check(static_cast<bool>(input), "open optional stock handling");
    const std::string data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    HandlingFile::Lines lines(data.data(), data.size());
    char line[201];
    unsigned main = 0, bike = 0, boat = 0, flying = 0;
    for (;;) {
        const auto status = lines.Next(line, sizeof(line));
        if (status == HandlingFile::LineResult::End) break;
        if (status == HandlingFile::LineResult::TooLong && line[0] == ';') continue;
        Check(status == HandlingFile::LineResult::Ready, "stock line bounded");
        const std::string original(line);
        const auto record = HandlingFile::Parse(line);
        if (record.kind == HandlingFile::Kind::Invalid) std::fprintf(stderr, "Rejected stock line: %s\n", original.c_str());
        Check(record.kind != HandlingFile::Kind::Invalid, "stock record remains valid");
        if (record.kind == HandlingFile::Kind::End) break;
        if (record.kind == HandlingFile::Kind::Main) ++main;
        if (record.kind == HandlingFile::Kind::Bike) ++bike;
        if (record.kind == HandlingFile::Kind::Boat) ++boat;
        if (record.kind == HandlingFile::Kind::Flying) ++flying;
    }
    Check(main == 106 && bike == 5 && boat == 11 && flying == 10, "all stock record families preserved");
}
} // namespace

int main(int argc, char** argv)
{
    BufferTests(); LineTests(); RecordTests();
    if (argc > 1) StockTests(argv[1]);
    std::printf("Resource parser checks passed: %u.\n", checks);
}
