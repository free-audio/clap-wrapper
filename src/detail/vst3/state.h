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

    // IBStream counts in int32. A caller asking for more than that is not an
    // error - it is a reader with no length, which is every clap_istream
    // reader - so give it what fits and let it come back for the rest.
    const int32 wanted = size > static_cast<uint64_t>(Steinberg::kMaxInt32)
                             ? Steinberg::kMaxInt32
                             : static_cast<Steinberg::int32>(size);

    Steinberg::int32 bytesRead = 0;
    const auto result = self->vst_stream->read(buffer, wanted, &bytesRead);
    if (kResultOk == result) return bytesRead;

    // kResultFalse, and ONLY kResultFalse, is how a host may phrase "that ran
    // past the end of the chunk". Cubase answers that way rather than with
    // kResultOk and a short count, and asking for more than is left is the
    // normal way to read a stream whose length you were never told - the
    // extension gives a reader no way to ask.
    //
    // CLAP has one code for "there is nothing more" and it is 0
    // (clap/stream.h: "0 indicates end of file and -1 a read error"), so that
    // is what this returns. Reporting -1 for it said "this stream is broken",
    // and a plug-in that believed it threw away the project state it was in
    // the middle of restoring - on the very first read, so under VST3 nothing
    // was ever restored at all.
    if (kResultFalse == result) return bytesRead;

    // Anything else is a real failure - kOutOfMemory, kInternalError,
    // kInvalidArgument - and has to stay one. Reporting end-of-file for these
    // is worse than reporting nothing: a reader that loops until it has the
    // bytes it needs (the pattern in this repository's own conformance
    // fixtures) never terminates, and one that treats "no bytes" as "no state"
    // reports a successful load and lets the next save write its defaults over
    // the user's project.
    return -1;
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
