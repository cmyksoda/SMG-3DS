#pragma once
// The game's files are big-endian, for the Wii's CPU; the 3DS's is little-endian. Where original code reads a
// format's structures, these turn them to native order in place first (source/compat/NativeResources.cpp). Each
// recognises data it already turned, so a second call changes nothing.
void smgNativeRarc(void* archive);
// The table to read: this one, turned in place, or a repacked copy where ARM cannot load its fields where they sit.
const void* smgNativeBcsv(void* table);
// Where a repacked copy goes: the free store, unless the whole-game library sets it beside the original.
extern void* (*smgBcsvAllocator)(const void* beside, u32 size);
// Why a table cannot be turned in place, or null when it can (or already is); repacking handles misplaced fields.
const char* smgNativeBcsvProblem(const void* table);
// A BMG message file turned in place (smg::nativeBmg); one the port cannot turn stops, naming why.
void smgNativeBmg(void* bmg, u32 size);
// A layout archive's file turned in place as LayoutHolder mounts it (BRLYT, BRLAN and TPL, by name; others stay as
// they are). One the port cannot turn stops, naming why.
void smgNativeLayoutFile(const char* name, void* data, u32 size);
// A font (BRFNT) turned in place as ResFont takes it, sized by its own header.
void smgNativeFont(void* brfnt);
// The music-event table (.bmt) and sequence index (.bme), turned in place (smg::nativeMeTable, nativeMeSequences).
void smgNativeMeTable(void* table, u32 size);
void smgNativeMeSequences(void* sequences, u32 size);
// A chord table (.cit) before its first relocation, and the remix sequence (.brs), turned in place.
void smgNativeChordTable(void* table, u32 size);
void smgNativeRemixSequence(void* sequence, u32 size);
// The Wii Remote speaker's table (.bct) and waves (.csw), turned in place.
void smgNativeSpeakerTable(void* table, u32 size);
void smgNativeSpeakerWaves(void* waves, u32 size);
// JAudio2's audio archive (SMR.szs), its parts turned in place; `size` is the heap block holding it (negative if none).
void smgNativeAudioArchive(void* archive, s32 size);
// A J3D model file (BMD, BDL, BMT) turned in place the first time a loader meets it; its header gives its size.
void smgNativeJ3DModel(void* file);
// A J3D animation file (BCK, BTK, BRK, BPK, BTP, BVA) turned in place the first time the loader meets it.
void smgNativeJ3DAnimation(void* file);
