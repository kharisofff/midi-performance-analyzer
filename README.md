# midi-performance-analyzer

A C++17 tool that reads a MIDI recording of a musician's performance and, in later
stages, will analyse rhythm, dynamics and articulation and suggest what to improve.

## Status

- [x] Stage 1: MIDI parser (notes, velocities, tempo map) with unit tests
- [ ] Stage 2: metrics (timing deviation, dynamics, legato/staccato)
- [ ] Stage 3: textual report with practice advice
- [ ] Stage 4: HTTP API and database storage

## Build and run

    make                      # builds ./midi_analyzer
    ./midi_analyzer examples/demo_scale.mid
    make test                 # unit tests, built with ASan + UBSan

Requires a C++17 compiler and make. No third-party dependencies.

## Design notes

- `ByteReader` bounds-checks every read, so a corrupt or hostile file raises
  `ParseError` instead of reading out of range.
- Supports MIDI format 0 and 1 with ticks-per-quarter timing. Format 2 and SMPTE
  timing are rejected explicitly.
- Handles running status, "note on with velocity 0 = note off", tempo changes,
  overlapping notes on the same pitch (paired first-in-first-out) and notes that
  are still held at the end of a track.

## How this project is made

The first stage (the MIDI parser and its tests) was written with the help of an AI
assistant (Claude). I am going through the code step by step to understand it, and the
following stages (metrics, report, API) I am writing and extending myself.
