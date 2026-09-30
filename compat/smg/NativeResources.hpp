#pragma once
// The game's files are big-endian, for the Wii's CPU; the 3DS's is little-endian. Where original code reads a
// format's structures, these turn them to native order in place first (source/compat/NativeResources.cpp). Each
// recognises data it already turned, so a second call changes nothing.
void smgNativeRarc(void* archive);
