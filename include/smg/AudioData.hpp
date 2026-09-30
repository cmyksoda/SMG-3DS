#pragma once
#include <cstddef>
#include <cstdint>

namespace smg {
// The game's audio data turned native in place for the readers that overlay it with structs; each returns why a file
// cannot be turned, leaving it as it was, or null once it is native. Each is turned once, where the game takes it.
// JAudio2's own byte-stream readers (the audio archive interpreter, the sequence reader) read big-endian instead.

// A music-event table (JaiMe.arc's .bmt, AudMePlayingParamsHolder): a count, the entries' and names' offsets, then
// 8-byte entries (one 16-bit field) and name offsets the game relocates.
const char* nativeMeTable(std::uint8_t* data, std::size_t size);
// A music-event sequence file (.bme, AudMeTable): a count and each sequence's start; the sequences are bytecode.
const char* nativeMeSequences(std::uint8_t* data, std::size_t size);
// A chord table (JaiChord.arc's .cit, AudChordTable), before the game relocates it: a flag, "CITS", its size, two
// 16-bit counts, then offsets to 8-byte chords and to scales (two offsets each, to byte runs).
const char* nativeChordTable(std::uint8_t* data, std::size_t size);
// The remix sequence (JaiRemixSeq.arc's .brs, AudRemixMgr): 32-bit words only, read as groups of tracks of notes.
const char* nativeRemixSequence(std::uint8_t* data, std::size_t size);
// The Wii Remote speaker's sound table (SpkRes.arc's .bct, SpkTable): a count, the entries' and names' offsets, a
// relocated flag, then 8-byte entries (16-bit wave, two bytes, 16-bit release) and name offsets.
const char* nativeSpeakerTable(std::uint8_t* data, std::size_t size);
// Its waves (.csw, SpkWave): the file's size, a count, offsets, then per wave its byte size, loop points and PCM16.
const char* nativeSpeakerWaves(std::uint8_t* data, std::size_t size);
// JAudio2's audio archive (SMR.szs, read by JAUAudioArcInterpreter): its command stream stays big-endian for the
// interpreter, and what each command points at turns native for its parser: the sound table (BST), sound names
// (BSTN), sequence collection (BSC), wave systems (WSYS) and instrument banks (IBNK, version 1). Sequences and wave
// samples stay big-endian, as JAudio2's sequence reader and the DSP read them.
const char* nativeAudioArchive(std::uint8_t* data, std::size_t size);
} // namespace smg
