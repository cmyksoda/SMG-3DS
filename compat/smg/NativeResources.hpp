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
