#include <cstdio>
#include <iostream>

#include "midi.hpp"

namespace {

std::string pitch_name(std::uint8_t pitch) {
    static const char* const names[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                          "F#", "G",  "G#", "A",  "A#", "B"};
    int octave = static_cast<int>(pitch) / 12 - 1;  // MIDI 60 is C4
    return std::string(names[pitch % 12]) + std::to_string(octave);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <file.mid>\n";
        return 2;
    }
    try {
        midi::Song song = midi::parse_file(argv[1]);
        std::printf("ticks per quarter: %u, tempo changes: %zu, notes: %zu\n\n",
                    static_cast<unsigned>(song.ticks_per_quarter),
                    song.tempo_map.size(), song.notes.size());
        std::printf("%10s %10s  %-5s %s\n", "start(s)", "length(s)", "note", "velocity");
        for (const midi::Note& n : song.notes) {
            double start = midi::ticks_to_seconds(song, n.start_tick);
            double end = midi::ticks_to_seconds(song, n.start_tick + n.duration_ticks);
            std::printf("%10.3f %10.3f  %-5s %u\n", start, end - start,
                        pitch_name(n.pitch).c_str(), static_cast<unsigned>(n.velocity));
        }
    } catch (const midi::ParseError& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
