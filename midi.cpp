#include "midi.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <fstream>
#include <iterator>
#include <tuple>

namespace midi {

// ---------------------------------------------------------------- ByteReader

ByteReader::ByteReader(const std::uint8_t* data, std::size_t size)
    : data_(data), size_(size) {}

std::size_t ByteReader::remaining() const { return size_ - pos_; }

bool ByteReader::empty() const { return pos_ == size_; }

std::uint8_t ByteReader::read_u8() {
    if (pos_ >= size_) throw ParseError("unexpected end of data");
    return data_[pos_++];
}

std::uint16_t ByteReader::read_u16() {
    // Two separate statements: the order of evaluation of operands of `|`
    // is unspecified in C++, so `(read_u8() << 8) | read_u8()` would be a bug.
    std::uint16_t high = read_u8();
    std::uint16_t low = read_u8();
    return static_cast<std::uint16_t>((high << 8) | low);
}

std::uint32_t ByteReader::read_u32() {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value = (value << 8) | read_u8();
    }
    return value;
}

std::uint32_t ByteReader::read_vlq() {
    // 7 data bits per byte; the high bit means "more bytes follow".
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        std::uint8_t byte = read_u8();
        value = (value << 7) | (byte & 0x7F);
        if ((byte & 0x80) == 0) return value;
    }
    throw ParseError("variable-length quantity is longer than 4 bytes");
}

void ByteReader::skip(std::size_t n) {
    // Compare against remaining(), never `pos_ + n > size_`: the addition
    // could overflow for a hostile n.
    if (n > remaining()) throw ParseError("skip past end of data");
    pos_ += n;
}

ByteReader ByteReader::sub_reader(std::size_t n) {
    if (n > remaining()) throw ParseError("chunk length exceeds file size");
    ByteReader sub(data_ + pos_, n);
    pos_ += n;
    return sub;
}

// ------------------------------------------------------------------- parsing

namespace {

struct ActiveNote {
    std::uint64_t start_tick;
    std::uint8_t velocity;
};

std::string read_tag(ByteReader& r) {
    std::string tag;
    for (int i = 0; i < 4; ++i) tag.push_back(static_cast<char>(r.read_u8()));
    return tag;
}

void parse_track(ByteReader r, Song& song) {
    std::uint64_t tick = 0;
    std::uint8_t running_status = 0;

    // Notes that have started but not yet ended, per channel and pitch.
    // A deque (FIFO) because the same pitch can be started twice before the
    // first one is released; we pair the first off with the first on.
    std::array<std::array<std::deque<ActiveNote>, 128>, 16> active;

    while (!r.empty()) {
        tick += r.read_vlq();  // delta time
        std::uint8_t first = r.read_u8();

        std::uint8_t status = first;
        bool have_data1 = false;
        std::uint8_t data1 = 0;
        if (first < 0x80) {
            // Running status: the status byte was omitted, `first` is already
            // the first data byte of a message that reuses the previous status.
            if (running_status == 0) {
                throw ParseError("data byte without a preceding status byte");
            }
            status = running_status;
            data1 = first;
            have_data1 = true;
        }

        if (status == 0xFF) {  // meta event
            running_status = 0;  // meta and sysex cancel running status
            std::uint8_t type = r.read_u8();
            std::uint32_t length = r.read_vlq();
            ByteReader data = r.sub_reader(length);
            if (type == 0x2F) break;  // end of track
            if (type == 0x51) {       // set tempo
                if (length != 3) throw ParseError("tempo event must have 3 bytes");
                std::uint32_t b0 = data.read_u8();
                std::uint32_t b1 = data.read_u8();
                std::uint32_t b2 = data.read_u8();
                std::uint32_t tempo = (b0 << 16) | (b1 << 8) | b2;
                if (tempo == 0) throw ParseError("tempo must be positive");
                song.tempo_map.push_back({tick, tempo});
            }
            continue;  // other meta events (names, lyrics, ...) are ignored
        }
        if (status == 0xF0 || status == 0xF7) {  // system exclusive
            running_status = 0;
            r.skip(r.read_vlq());
            continue;
        }
        if (status >= 0xF1) {
            throw ParseError("unsupported system message in track");
        }

        // Channel message.
        running_status = status;
        std::uint8_t type = status & 0xF0;
        std::uint8_t channel = status & 0x0F;
        if (!have_data1) data1 = r.read_u8();
        if (data1 >= 0x80) throw ParseError("data byte has its high bit set");

        std::uint8_t data2 = 0;
        bool two_data_bytes = (type != 0xC0 && type != 0xD0);  // program, pressure
        if (two_data_bytes) {
            data2 = r.read_u8();
            if (data2 >= 0x80) throw ParseError("data byte has its high bit set");
        }

        bool is_note_on = (type == 0x90 && data2 > 0);
        // A "note on" with velocity 0 is, by convention, a "note off".
        bool is_note_off = (type == 0x80 || (type == 0x90 && data2 == 0));

        if (is_note_on) {
            active[channel][data1].push_back({tick, data2});
        } else if (is_note_off) {
            auto& queue = active[channel][data1];
            if (queue.empty()) continue;  // stray note-off: ignore
            ActiveNote started = queue.front();
            queue.pop_front();
            song.notes.push_back(
                {started.start_tick, tick - started.start_tick, data1,
                 started.velocity, channel});
        }
        // Everything else (control change, pitch bend, ...) is skipped for now.
    }

    // Notes still held when the track ends are closed at the end of the track.
    for (std::uint8_t ch = 0; ch < 16; ++ch) {
        for (std::uint8_t pitch = 0; pitch < 128; ++pitch) {
            for (const ActiveNote& n : active[ch][pitch]) {
                song.notes.push_back(
                    {n.start_tick, tick - n.start_tick, pitch, n.velocity, ch});
            }
        }
    }
}

}  // namespace

Song parse(const std::vector<std::uint8_t>& data) {
    ByteReader r(data.data(), data.size());

    if (read_tag(r) != "MThd") throw ParseError("not a MIDI file (no MThd header)");
    std::uint32_t header_length = r.read_u32();
    if (header_length < 6) throw ParseError("MIDI header is too short");
    ByteReader header = r.sub_reader(header_length);

    std::uint16_t format = header.read_u16();
    header.read_u16();  // declared track count: we trust the chunks, not this
    std::uint16_t division = header.read_u16();

    if (format > 1) throw ParseError("MIDI format 2 is not supported");
    if (division & 0x8000) throw ParseError("SMPTE time division is not supported");
    if (division == 0) throw ParseError("ticks per quarter note must be positive");

    Song song;
    song.ticks_per_quarter = division;

    int tracks = 0;
    while (!r.empty()) {
        std::string tag = read_tag(r);
        std::uint32_t length = r.read_u32();
        ByteReader body = r.sub_reader(length);
        if (tag == "MTrk") {
            parse_track(body, song);
            ++tracks;
        }
        // Chunks with unknown tags must be skipped, per the specification.
    }
    if (tracks == 0) throw ParseError("MIDI file has no tracks");

    std::stable_sort(song.tempo_map.begin(), song.tempo_map.end(),
                     [](const TempoChange& a, const TempoChange& b) {
                         return a.tick < b.tick;
                     });
    std::sort(song.notes.begin(), song.notes.end(),
              [](const Note& a, const Note& b) {
                  return std::tie(a.start_tick, a.pitch, a.channel, a.duration_ticks) <
                         std::tie(b.start_tick, b.pitch, b.channel, b.duration_ticks);
              });
    return song;
}

Song parse_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw ParseError("cannot open file: " + path);
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
    return parse(data);
}

double ticks_to_seconds(const Song& song, std::uint64_t tick) {
    // Walk the tempo map; inside each segment the tempo is constant, so
    // seconds = ticks * (microseconds per quarter) / (1e6 * ticks per quarter).
    double seconds = 0.0;
    std::uint64_t segment_start = 0;
    std::uint32_t tempo = 500000;  // MIDI default: 120 BPM
    const double ticks_per_quarter = song.ticks_per_quarter;

    for (const TempoChange& change : song.tempo_map) {
        if (change.tick >= tick) break;
        seconds += static_cast<double>(change.tick - segment_start) * tempo /
                   (1e6 * ticks_per_quarter);
        segment_start = change.tick;
        tempo = change.micros_per_quarter;
    }
    seconds += static_cast<double>(tick - segment_start) * tempo /
               (1e6 * ticks_per_quarter);
    return seconds;
}

}  // namespace midi
