#pragma once
#include <cstddef>
#include <cstdint>

namespace smg {
// A BMG message file turned native in place for the game's own readers (MessageData, MessageEditorMessageTag,
// TalkNodeCtrl): header, INF1, the DAT1 text walked message by message, FLW1. Returns why it cannot be, leaving the
// data as it was, or null once it is native (a second call changes nothing).
const char* nativeBmg(std::uint8_t* data, std::size_t size);
} // namespace smg
