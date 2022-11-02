#pragma once

/*

    Copyright (c) 2022 Timo Kaluza (defiantnerd)

    This file is part of the clap-wrappers project which is released under MIT License.
    See file LICENSE or go to https://github.com/defiantnerd/clap-wrapper for full license details.

*/

class CLAPVST3StreamAdapter
{
public:
  CLAPVST3StreamAdapter(Steinberg::IBStream* stream)
    : vst_stream(stream)
  {
  }
  operator const clap_istream_t* () const { return &in; }
  operator const clap_ostream_t* () const { return &out; }

  static int64_t read(const struct clap_istream* stream, void* buffer, uint64_t size)
  {
    auto self = static_cast<CLAPVST3StreamAdapter*>(stream->ctx);
    Steinberg::int32 bytesRead = 0;
    if (kResultOk == self->vst_stream->read(buffer, size, &bytesRead))
      return bytesRead;
    return -1;
  }
  static int64_t write(const struct clap_ostream* stream, const void* buffer, uint64_t size)
  {
    auto self = static_cast<CLAPVST3StreamAdapter*>(stream->ctx);
    Steinberg::int32 bytesWritten = 0;
    if (kResultOk == self->vst_stream->write(const_cast<void*>(buffer), size, &bytesWritten))
      return bytesWritten;
    return -1;
  }
private:
  Steinberg::IBStream* vst_stream = nullptr;
  clap_istream_t in = { this, read };
  clap_ostream_t out = { this, write };
};
