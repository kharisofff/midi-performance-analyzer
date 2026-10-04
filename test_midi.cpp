#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#include "midi.hpp"

using Bytes = std::vector<std::uint8_t>;

static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::cerr << __FILE__ << ":" << __LINE__ << "  CHECK failed: "  \
                      << #cond << "\n";                                      \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

template <class F>
static bool throws_parse_error(F f) {
    try {
        f();
    } catch (const midi::ParseError&) {
        return true;
    }
    return false;
}

static void put_u16(Bytes& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
}

static void put_u32(Bytes& b, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        b.push_back(static_cast<std::uint8_t>((v >> shift) & 0xFF));
    }
}

// Builds a complete format-0 MIDI file around a track body.
static Bytes make_midi(std::uint16_t ticks_per_quarter, const Bytes& track_body) {
    Bytes b = {'M', 'T', 'h', 'd'};
    put_u32(b, 6);
    put_u16(b, 0);  // format 0
    put_u16(b, 1);  // one track
    put_u16(b, ticks_per_quarter);
    b.insert(b.end(), {'M', 'T', 'r', 'k'});
    put_u32(b, static_cast<std::uint32_t>(track_body.size()));
    b.insert(b.end(), track_body.begin(), track_body.end());
    return b;
}

static const Bytes kEndOfTrack = {0x00, 0xFF, 0x2F, 0x00};
static const Bytes kTempo120 = {0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20};

static Bytes concat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

static void test_vlq() {
    {
        Bytes b = {0x00};
        midi::ByteReader r(b.data(), b.size());
        CHECK(r.read_vlq() == 0);
    }
    {
        Bytes b = {0x7F};
        midi::ByteReader r(b.data(), b.size());
        CHECK(r.read_vlq() == 127);
    }
    {
        Bytes b = {0x81, 0x00};
        midi::ByteReader r(b.data(), b.size());
        CHECK(r.read_vlq() == 128);
    }
    {
        Bytes b = {0xFF, 0xFF, 0xFF, 0x7F};
        midi::ByteReader r(b.data(), b.size());
        CHECK(r.read_vlq() == 268435455u);
    }
    {
        Bytes b = {0x80, 0x80, 0x80, 0x80, 0x00};  // 5 bytes: invalid
        CHECK(throws_parse_error([&] {
            midi::ByteReader r(b.data(), b.size());
            r.read_vlq();
        }));
    }
}

static void test_single_note() {
    Bytes track = concat({kTempo120,
                          {0x00, 0x90, 0x3C, 0x40},        // C4 on, velocity 64
                          {0x83, 0x60, 0x80, 0x3C, 0x00},  // +480 ticks: off
                          kEndOfTrack});
    midi::Song song = midi::parse(make_midi(480, track));
    CHECK(song.notes.size() == 1);
    CHECK(song.notes[0].start_tick == 0);
    CHECK(song.notes[0].duration_ticks == 480);
    CHECK(song.notes[0].pitch == 60);
    CHECK(song.notes[0].velocity == 64);
    CHECK(std::fabs(midi::ticks_to_seconds(song, 480) - 0.5) < 1e-9);
}

static void test_running_status_and_zero_velocity() {
    Bytes track = concat({{0x00, 0x90, 0x3C, 0x40},  // C4 on (full status)
                          {0x60, 0x3E, 0x40},        // +96: D4 on, status omitted
                          {0x60, 0x3C, 0x00},        // +96: C4 "on" with vel 0 = off
                          {0x60, 0x3E, 0x00},        // +96: D4 off
                          kEndOfTrack});
    midi::Song song = midi::parse(make_midi(480, track));
    CHECK(song.notes.size() == 2);
    CHECK(song.notes[0].pitch == 60 && song.notes[0].start_tick == 0 &&
          song.notes[0].duration_ticks == 192);
    CHECK(song.notes[1].pitch == 62 && song.notes[1].start_tick == 96 &&
          song.notes[1].duration_ticks == 192);
}

static void test_same_pitch_overlap_is_fifo() {
    Bytes track = concat({{0x00, 0x90, 0x3C, 0x40},  // tick 0: on, vel 64
                          {0x10, 0x90, 0x3C, 0x50},  // tick 16: on again, vel 80
                          {0x10, 0x80, 0x3C, 0x00},  // tick 32: first off
                          {0x10, 0x80, 0x3C, 0x00},  // tick 48: second off
                          kEndOfTrack});
    midi::Song song = midi::parse(make_midi(480, track));
    CHECK(song.notes.size() == 2);
    CHECK(song.notes[0].start_tick == 0 && song.notes[0].duration_ticks == 32 &&
          song.notes[0].velocity == 64);
    CHECK(song.notes[1].start_tick == 16 && song.notes[1].duration_ticks == 32 &&
          song.notes[1].velocity == 80);
}

static void test_hanging_note_closed_at_end_of_track() {
    Bytes track = concat({{0x00, 0x90, 0x3C, 0x40},
                          {0x64, 0xFF, 0x2F, 0x00}});  // end of track at tick 100
    midi::Song song = midi::parse(make_midi(480, track));
    CHECK(song.notes.size() == 1);
    CHECK(song.notes[0].duration_ticks == 100);
}

static void test_tempo_change() {
    Bytes track = concat({kTempo120,
                          {0x83, 0x60, 0xFF, 0x51, 0x03, 0x0F, 0x42, 0x40},  // tick 480: 60 BPM
                          {0x83, 0x60, 0xFF, 0x2F, 0x00}});                  // tick 960
    midi::Song song = midi::parse(make_midi(480, track));
    CHECK(song.tempo_map.size() == 2);
    // 480 ticks at 0.5 s per quarter + 480 ticks at 1.0 s per quarter.
    CHECK(std::fabs(midi::ticks_to_seconds(song, 960) - 1.5) < 1e-9);
    // Default tempo applies when the file has no tempo events.
    midi::Song plain = midi::parse(make_midi(480, kEndOfTrack));
    CHECK(std::fabs(midi::ticks_to_seconds(plain, 480) - 0.5) < 1e-9);
}

static void test_malformed_input() {
    // Wrong magic number.
    Bytes riff = {'R', 'I', 'F', 'F', 0, 0, 0, 0};
    CHECK(throws_parse_error([&] { midi::parse(riff); }));

    // Empty input.
    CHECK(throws_parse_error([&] { midi::parse(Bytes{}); }));

    // Truncated file: the track claims more bytes than exist.
    Bytes good = make_midi(480, concat({kTempo120, kEndOfTrack}));
    Bytes cut(good.begin(), good.end() - 3);
    CHECK(throws_parse_error([&] { midi::parse(cut); }));

    // Hostile chunk length must be rejected, not trusted.
    Bytes huge = {'M', 'T', 'h', 'd'};
    put_u32(huge, 6);
    put_u16(huge, 0);
    put_u16(huge, 1);
    put_u16(huge, 480);
    huge.insert(huge.end(), {'M', 'T', 'r', 'k', 0xFF, 0xFF, 0xFF, 0xFF, 0x00});
    CHECK(throws_parse_error([&] { midi::parse(huge); }));

    // Data byte before any status byte.
    Bytes no_status = make_midi(480, Bytes{0x00, 0x3C, 0x40});
    CHECK(throws_parse_error([&] { midi::parse(no_status); }));
}

int main() {
    test_vlq();
    test_single_note();
    test_running_status_and_zero_velocity();
    test_same_pitch_overlap_is_fifo();
    test_hanging_note_closed_at_end_of_track();
    test_tempo_change();
    test_malformed_input();

    if (failures == 0) {
        std::cout << "all tests passed\n";
        return 0;
    }
    std::cerr << failures << " check(s) failed\n";
    return 1;
}
