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
