#pragma once

/*
    Copyright (c) 2022 Timo Kaluza (defiantnerd)

    This file is part of the clap-wrappers project which is released under MIT License.
    See file LICENSE or go to https://github.com/free-audio/clap-wrapper for full license details.

*/

class CLAPVST3StreamAdapter
{
 public:
  CLAPVST3StreamAdapter(Steinberg::IBStream *stream) : vst_stream(stream)
  {
  }
  operator const clap_istream_t *() const
  {
    return &in;
  }
  operator const clap_ostream_t *() const
  {
    return &out;
  }

  static int64_t read(const struct clap_istream *stream, void *buffer, uint64_t size)
  {
    auto self = static_cast<CLAPVST3StreamAdapter *>(stream->ctx);
    Steinberg::int32 bytesRead = 0;
    const auto result = self->vst_stream->read(buffer, (int32)size, &bytesRead);
    if (kResultOk == result) return bytesRead;

    // Not every host reports the end of a stream the way the SDK's own
    // IBStream implementations do. Cubase answers a read that runs past the
    // end of the plug-in's chunk with kResultFalse rather than with kResultOk
    // and a short count - and a reader asking for more than is left is the
    // normal way to read a stream whose length it does not know, which is
    // every reader clap_istream has.
    //
    // CLAP has one code for "there is nothing more": 0. Reporting -1 instead
    // says "this stream is broken", and a plug-in that believes it throws away
    // the project state it was in the middle of restoring - which is exactly
    // what it did, on the very first read, so nothing was ever restored under
    // VST3 at all.
    //
    // A real I/O failure ends up here too and is reported as a short read. The
    // reader gets a truncated chunk and rejects it, which is where it was
    // going to end up anyway; what it does not do is lose a whole project to a
    // host that phrased the end of a stream differently.
    return bytesRead > 0 ? bytesRead : 0;
  }
  static int64_t write(const struct clap_ostream *stream, const void *buffer, uint64_t size)
  {
    auto self = static_cast<CLAPVST3StreamAdapter *>(stream->ctx);
    Steinberg::int32 bytesWritten = 0;
    if (kResultOk == self->vst_stream->write(const_cast<void *>(buffer), (int32)size, &bytesWritten))
      return bytesWritten;
    return -1;
  }

 private:
  Steinberg::IBStream *vst_stream = nullptr;
  clap_istream_t in = {this, read};
  clap_ostream_t out = {this, write};
};
