#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace midi {

// Thrown for any malformed or unsupported input. The parser never trusts the
// file: every read is bounds-checked and reports problems through this type.
class ParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A single played note, with times in MIDI ticks (not seconds).
struct Note {
    std::uint64_t start_tick;
    std::uint64_t duration_ticks;
    std::uint8_t pitch;     // 0..127, 60 = middle C
    std::uint8_t velocity;  // 1..127, how hard the key was hit (our "dynamics")
    std::uint8_t channel;   // 0..15
};

struct TempoChange {
    std::uint64_t tick;
    std::uint32_t micros_per_quarter;  // microseconds per quarter note
};

struct Song {
    std::uint16_t ticks_per_quarter = 480;
    std::vector<TempoChange> tempo_map;  // sorted by tick
    std::vector<Note> notes;             // sorted by start_tick, then pitch
};

// Bounds-checked cursor over a block of bytes. Public so it can be unit-tested.
class ByteReader {
public:
    ByteReader(const std::uint8_t* data, std::size_t size);

    std::size_t remaining() const;
    bool empty() const;

    std::uint8_t read_u8();
    std::uint16_t read_u16();  // big-endian
    std::uint32_t read_u32();  // big-endian
    std::uint32_t read_vlq();  // MIDI variable-length quantity, at most 4 bytes
    void skip(std::size_t n);

    // Consumes n bytes and returns an independent reader over exactly them.
    ByteReader sub_reader(std::size_t n);

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
};

Song parse(const std::vector<std::uint8_t>& data);
Song parse_file(const std::string& path);

// Converts an absolute tick position to seconds, honouring tempo changes.
double ticks_to_seconds(const Song& song, std::uint64_t tick);

}  // namespace midi
