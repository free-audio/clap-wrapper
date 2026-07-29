#pragma once

/*
    Shared MIDI translation helpers

    Copyright (c) 2024 the clap-wrapper project

    This file is part of the clap-wrappers project which is released under MIT License.
    See file LICENSE or go to https://github.com/free-audio/clap-wrapper for full license details.

    Backend-agnostic, header-only helpers for the format-level parts of MIDI <-> CLAP
    translation that must behave identically across the AUv2 / AUv3 / AAX wrappers:
    note-dialect selection, MIDI 1.0 <-> MIDI 2.0 (Universal MIDI Packet) conversion,
    and UMP SysEx7 packing / reassembly.

    These functions are pure (no wrapper/host state) and deliberately do NOT touch any
    per-backend event container: each wrapper keeps its own event storage, sample-offset
    decoding, note-id policy and host I/O, and only calls into here for the fiddly,
    correctness-critical bit manipulation.
*/

#include <cstdint>
#include <vector>
#include <clap/clap.h>

namespace ClapWrapper::detail::shared
{

// Decide which note dialect to feed the plugin on the input path.
// A CLAP plugin's preferred_dialect is guaranteed to be one of its supported
// dialects, but be defensive: if the preferred dialect is somehow not offered
// (or no note-port info was captured), fall back to the first dialect the port
// does support, favouring typed CLAP notes over raw MIDI.
inline uint32_t chooseInputDialect(uint32_t preferred, uint32_t supported)
{
  if (supported == 0) return preferred;  // no note-port info: trust preferred
  if (supported & preferred) return preferred;
  for (uint32_t dialect : {CLAP_NOTE_DIALECT_CLAP, CLAP_NOTE_DIALECT_MIDI, CLAP_NOTE_DIALECT_MIDI_MPE,
                           CLAP_NOTE_DIALECT_MIDI2})
  {
    if (supported & dialect) return dialect;
  }
  return preferred;
}

// Number of 32-bit words in a Universal MIDI Packet message, derived from the
// message type in the high nibble of its first word (MIDI 2.0 UMP spec).
inline uint32_t umpMessageWordCount(uint32_t word0)
{
  switch ((word0 >> 28) & 0xFu)
  {
    case 0x0:  // utility
    case 0x1:  // system real time / common
    case 0x2:  // MIDI 1.0 channel voice
    case 0x6:
    case 0x7:
      return 1;
    case 0x3:  // data / SysEx7 (64-bit)
    case 0x4:  // MIDI 2.0 channel voice
    case 0x8:
    case 0x9:
    case 0xA:
      return 2;
    case 0xB:
    case 0xC:
      return 3;
    case 0x5:  // data (128-bit)
    case 0xD:
    case 0xE:
    case 0xF:
      return 4;
    default:
      return 1;
  }
}

// Wrap a MIDI 1.0 channel-voice message (status incl. channel, data1, data2) into
// a single MT 0x2 UMP word, group 0.
inline uint32_t midi1ToUmpWord(uint8_t status, uint8_t data1, uint8_t data2)
{
  return (0x2u << 28) | (0x0u << 24) | (static_cast<uint32_t>(status) << 16) |
         (static_cast<uint32_t>(data1) << 8) | static_cast<uint32_t>(data2);
}

// Down-convert a single MIDI 2.0 channel-voice UMP message (MT 0x4) to a MIDI 1.0
// 3-byte message. Returns the number of MIDI1 bytes written (0 if not convertible).
// Wide MIDI2 values are reduced by dropping the low bits (16->7, 32->7, 32->14).
inline int midi2ChannelVoiceToMidi1(const uint32_t data[4], uint8_t out[3])
{
  const uint32_t w0 = data[0];
  const uint32_t w1 = data[1];
  if (((w0 >> 28) & 0xFu) != 0x4u) return 0;  // only MIDI 2.0 channel voice

  const uint8_t status = static_cast<uint8_t>((w0 >> 16) & 0xF0u);
  const uint8_t channel = static_cast<uint8_t>((w0 >> 16) & 0x0Fu);
  const uint8_t index = static_cast<uint8_t>((w0 >> 8) & 0x7Fu);  // note / cc index

  switch (status)
  {
    case 0x80:  // note off
    case 0x90:  // note on
    {
      uint8_t vel = static_cast<uint8_t>((w1 >> 16) >> 9);  // 16-bit velocity -> 7-bit
      // MIDI 2.0 note-on keeps velocity semantics; guard against an accidental
      // MIDI1 note-off when a non-zero MIDI2 velocity scales down to 0.
      if (status == 0x90 && vel == 0) vel = 1;
      out[0] = static_cast<uint8_t>(status | channel);
      out[1] = index;
      out[2] = vel;
      return 3;
    }
    case 0xA0:  // poly pressure
    case 0xB0:  // control change
    {
      out[0] = static_cast<uint8_t>(status | channel);
      out[1] = index;
      out[2] = static_cast<uint8_t>(w1 >> 25);  // 32-bit -> 7-bit
      return 3;
    }
    case 0xC0:  // program change
    {
      out[0] = static_cast<uint8_t>(status | channel);
      out[1] = static_cast<uint8_t>((w1 >> 24) & 0x7Fu);
      out[2] = 0;
      return 2;
    }
    case 0xD0:  // channel pressure
    {
      out[0] = static_cast<uint8_t>(status | channel);
      out[1] = static_cast<uint8_t>(w1 >> 25);  // 32-bit -> 7-bit
      out[2] = 0;
      return 2;
    }
    case 0xE0:  // pitch bend
    {
      const uint32_t v14 = w1 >> 18;  // 32-bit -> 14-bit
      out[0] = static_cast<uint8_t>(status | channel);
      out[1] = static_cast<uint8_t>(v14 & 0x7Fu);
      out[2] = static_cast<uint8_t>((v14 >> 7) & 0x7Fu);
      return 3;
    }
    default:
      return 0;
  }
}

// Down-convert a CLAP note-expression event to a single MIDI 1.0 channel-voice
// message: PRESSURE with a key maps to polyphonic key pressure (0xA0), channel-wide
// PRESSURE (key < 0) to channel pressure (0xD0), and TUNING to pitch bend (0xE0,
// the conventional ±2 semitone range). Returns the number of MIDI1 bytes written
// (2 or 3), or 0 for expressions with no MIDI 1.0 equivalent (volume, pan,
// vibrato, brightness, …).
inline int noteExpressionToMidi1(const clap_event_note_expression_t &ne, uint8_t out[3])
{
  const uint8_t channel = static_cast<uint8_t>(ne.channel >= 0 ? ne.channel & 0x0F : 0);
  switch (ne.expression_id)
  {
    case CLAP_NOTE_EXPRESSION_PRESSURE:
    {
      double v = ne.value;
      if (v < 0.0) v = 0.0;
      if (v > 1.0) v = 1.0;
      if (ne.key >= 0)
      {
        out[0] = static_cast<uint8_t>(0xA0u | channel);
        out[1] = static_cast<uint8_t>(ne.key & 0x7F);
        out[2] = static_cast<uint8_t>(v * 127.0);
        return 3;
      }
      out[0] = static_cast<uint8_t>(0xD0u | channel);
      out[1] = static_cast<uint8_t>(v * 127.0);
      out[2] = 0;
      return 2;
    }
    case CLAP_NOTE_EXPRESSION_TUNING:
    {
      double normalized = ne.value / 2.0;
      if (normalized < -1.0) normalized = -1.0;
      if (normalized > 1.0) normalized = 1.0;
      uint32_t bend = static_cast<uint32_t>((normalized + 1.0) * 8192.0);
      if (bend > 16383u) bend = 16383u;
      out[0] = static_cast<uint8_t>(0xE0u | channel);
      out[1] = static_cast<uint8_t>(bend & 0x7Fu);
      out[2] = static_cast<uint8_t>((bend >> 7) & 0x7Fu);
      return 3;
    }
    default:
      return 0;
  }
}

// Pack a SysEx payload into MT 0x3 UMP SysEx7 packets. The 0xF0/0xF7 framing is
// stripped (UMP SysEx7 carries only the content bytes), up to 6 bytes per 64-bit
// packet, with a status nibble marking complete(0)/start(1)/continue(2)/end(3).
// `emit(word0, word1)` is invoked once per packet.
template <class EmitPacket>
inline void packSysEx7(const uint8_t *data, uint32_t size, EmitPacket &&emit)
{
  if (!data || size == 0) return;

  const uint8_t *p = data;
  uint32_t n = size;
  if (n > 0 && p[0] == 0xF0)
  {
    ++p;
    --n;
  }
  if (n > 0 && p[n - 1] == 0xF7) --n;

  const uint8_t group = 0;
  uint32_t offset = 0;
  do
  {
    const uint32_t remaining = n - offset;
    const uint32_t chunk = (remaining > 6) ? 6 : remaining;
    uint8_t statusNibble;
    if (n <= 6)
      statusNibble = 0x0;  // complete in a single packet
    else if (offset == 0)
      statusNibble = 0x1;  // start
    else if (offset + chunk >= n)
      statusNibble = 0x3;  // end
    else
      statusNibble = 0x2;  // continue

    uint8_t b[6] = {0, 0, 0, 0, 0, 0};
    for (uint32_t k = 0; k < chunk; ++k) b[k] = p[offset + k];

    uint32_t word0 = (0x3u << 28) | (static_cast<uint32_t>(group) << 24) |
                     (static_cast<uint32_t>(statusNibble) << 20) | (chunk << 16) |
                     (static_cast<uint32_t>(b[0]) << 8) | static_cast<uint32_t>(b[1]);
    uint32_t word1 = (static_cast<uint32_t>(b[2]) << 24) | (static_cast<uint32_t>(b[3]) << 16) |
                     (static_cast<uint32_t>(b[4]) << 8) | static_cast<uint32_t>(b[5]);
    emit(word0, word1);

    offset += chunk;
  } while (offset < n);
}

// Reassembles a UMP SysEx7 (MT 0x3) packet stream into a complete SysEx message.
// The status nibble marks complete(0)/start(1)/continue(2)/end(3); on complete or
// end the accumulated content is wrapped in 0xF0/0xF7 framing (matching the CLAP
// SysEx convention the wrappers use) and made available via framedMessage().
class SysEx7Reassembler
{
 public:
  // Feed one SysEx7 packet (two UMP words). Returns true when a full message is
  // ready, at which point framedMessage() holds it.
  bool feed(uint32_t w0, uint32_t w1)
  {
    const uint8_t statusNibble = (w0 >> 20) & 0xFu;
    const uint8_t numBytes = (w0 >> 16) & 0xFu;
    const uint8_t bytes[6] = {
        static_cast<uint8_t>((w0 >> 8) & 0xFFu),  static_cast<uint8_t>(w0 & 0xFFu),
        static_cast<uint8_t>((w1 >> 24) & 0xFFu), static_cast<uint8_t>((w1 >> 16) & 0xFFu),
        static_cast<uint8_t>((w1 >> 8) & 0xFFu),  static_cast<uint8_t>(w1 & 0xFFu)};

    if (statusNibble == 0x0 || statusNibble == 0x1) _content.clear();
    for (uint8_t k = 0; k < numBytes && k < 6; ++k) _content.push_back(bytes[k]);

    if (statusNibble == 0x0 || statusNibble == 0x3)
    {
      _framed.clear();
      _framed.reserve(_content.size() + 2);
      _framed.push_back(0xF0);
      _framed.insert(_framed.end(), _content.begin(), _content.end());
      _framed.push_back(0xF7);
      _content.clear();
      return true;
    }
    return false;
  }

  const std::vector<uint8_t> &framedMessage() const
  {
    return _framed;
  }

  void reset()
  {
    _content.clear();
    _framed.clear();
  }

 private:
  std::vector<uint8_t> _content;
  std::vector<uint8_t> _framed;
};

// Per-cycle pool of byte buffers owning SysEx payloads (clap_event_midi_sysex_t
// only borrows a pointer). acquire() copies into a recycled buffer whose heap
// capacity survives reset(), so steady-state audio-thread use does not allocate;
// the pool only grows when one cycle holds more SysEx messages than any before.
// Handed-out payload pointers stay valid while the pool grows: growth moves the
// inner vector objects, never their heap storage.
class SysExBufferPool
{
 public:
  // pre-size the pool (never shrinks) and mark all entries free
  void prepare(size_t entries)
  {
    if (_buffers.size() < entries) _buffers.resize(entries);
    _used = 0;
  }

  // copy a payload into the next free buffer and return it
  const std::vector<uint8_t> &acquire(const uint8_t *data, uint32_t size)
  {
    if (_used == _buffers.size()) _buffers.emplace_back();
    auto &b = _buffers[_used++];
    b.assign(data, data + size);
    return b;
  }

  // mark all entries free without releasing their storage
  void reset()
  {
    _used = 0;
  }

 private:
  std::vector<std::vector<uint8_t>> _buffers;
  size_t _used = 0;
};

}  // namespace ClapWrapper::detail::shared
