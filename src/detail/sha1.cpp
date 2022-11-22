#include "sha1.h"
#include <stdint.h>
#include <memory>
#include <cassert>
#include <string.h>


static constexpr bool isBigEndian = false;

namespace Crypto
{
  class Sha1
  {
  public:
    Sha1() { reset(); }
    Sha1(const unsigned char* message_array, size_t length)
    {
      reset();
      input(message_array, length);
    }
    void input(const unsigned char* message_array, size_t length);
    struct sha1hash hash();
  private:
    void reset();
    void processMessageBlock();
    void padmessage();
    inline uint32_t circularShift(int bits, uint32_t word)
    {
      return ((word << bits) & 0xFFFFFFFF) | ((word & 0xFFFFFFFF) >> (32 - bits));
    }

    unsigned H[5] =                           // Message digest buffers
    {
      0x67452301,
      0xEFCDAB89,
      0x98BADCFE,
      0x10325476,
      0xC3D2E1F0
    };

    uint32_t _lengthLow = 0;                  // Message length in bits
    uint32_t _length_high = 0;                 // Message length in bits

    unsigned char _messageBlock[64] = { 0 };    // 512-bit message blocks
    int _messageBlockIndex = 0;              // Index into message block array

    bool _computed = false;                    // Is the digest computed?
    bool _corrupted = false;                   // Is the message digest corruped?

  };

  void Sha1::reset()
  {
    _lengthLow = 0;
    _length_high = 0;
    _messageBlockIndex = 0;

    H[0] = 0x67452301;
    H[1] = 0xEFCDAB89;
    H[2] = 0x98BADCFE;
    H[3] = 0x10325476;
    H[4] = 0xC3D2E1F0;

    _computed = false;
    _corrupted = false;
  }

  void Sha1::input(const unsigned char* message_array, size_t length)
  {
    if (length == 0)
    {
      return;
    }

    if (_computed || _corrupted)
    {
      _corrupted = true;
      return;
    }

    if (_messageBlockIndex >= 64)
    {
      return;
    }

    while (length-- && !_corrupted)
    {
      _messageBlock[_messageBlockIndex++] = (*message_array & 0xFF);

      _lengthLow += 8;
      _lengthLow &= 0xFFFFFFFF;               // Force it to 32 bits
      if (_lengthLow == 0)
      {
        _length_high++;
        _length_high &= 0xFFFFFFFF;          // Force it to 32 bits
        if (_length_high == 0)
        {
          _corrupted = true;               // Message is too long
        }
      }

      if (_messageBlockIndex == 64)
      {
        processMessageBlock();
      }

      message_array++;
    }
  }

  void Sha1::processMessageBlock()
  {
    const unsigned K[] = {               // Constants defined for SHA-1
         0x5A827999,
         0x6ED9EBA1,
         0x8F1BBCDC,
         0xCA62C1D6
    };
    int         t;                          // Loop counter
    unsigned    temp;                       // Temporary word value
    unsigned    W[80];                      // Word sequence
    unsigned    A, B, C, D, E;              // Word buffers

    /*
     *  Initialize the first 16 words in the array W
     */
    for (t = 0; t < 16; t++)
    {
      W[t] = ((unsigned)_messageBlock[t * 4]) << 24;
      W[t] |= ((unsigned)_messageBlock[t * 4 + 1]) << 16;
      W[t] |= ((unsigned)_messageBlock[t * 4 + 2]) << 8;
      W[t] |= ((unsigned)_messageBlock[t * 4 + 3]);
    }

    for (t = 16; t < 80; t++)
    {
      W[t] = circularShift(1, W[t - 3] ^ W[t - 8] ^ W[t - 14] ^ W[t - 16]);
    }

    A = H[0];
    B = H[1];
    C = H[2];
    D = H[3];
    E = H[4];

    for (t = 0; t < 20; t++)
    {
      temp = circularShift(5, A) + ((B & C) | ((~B) & D)) + E + W[t] + K[0];
      temp &= 0xFFFFFFFF;
      E = D;
      D = C;
      C = circularShift(30, B);
      B = A;
      A = temp;
    }

    for (t = 20; t < 40; t++)
    {
      temp = circularShift(5, A) + (B ^ C ^ D) + E + W[t] + K[1];
      temp &= 0xFFFFFFFF;
      E = D;
      D = C;
      C = circularShift(30, B);
      B = A;
      A = temp;
    }

    for (t = 40; t < 60; t++)
    {
      temp = circularShift(5, A) +
        ((B & C) | (B & D) | (C & D)) + E + W[t] + K[2];
      temp &= 0xFFFFFFFF;
      E = D;
      D = C;
      C = circularShift(30, B);
      B = A;
      A = temp;
    }

    for (t = 60; t < 80; t++)
    {
      temp = circularShift(5, A) + (B ^ C ^ D) + E + W[t] + K[3];
      temp &= 0xFFFFFFFF;
      E = D;
      D = C;
      C = circularShift(30, B);
      B = A;
      A = temp;
    }

    H[0] = (H[0] + A) & 0xFFFFFFFF;
    H[1] = (H[1] + B) & 0xFFFFFFFF;
    H[2] = (H[2] + C) & 0xFFFFFFFF;
    H[3] = (H[3] + D) & 0xFFFFFFFF;
    H[4] = (H[4] + E) & 0xFFFFFFFF;

    _messageBlockIndex = 0;
  }

  void Sha1::padmessage()
  {
    /*
       *  Check to see if the current message block is too small to hold
       *  the initial padding bits and length.  If so, we will pad the
       *  block, process it, and then continue padding into a second block.
       */
    if (_messageBlockIndex > 55)
    {
      _messageBlock[_messageBlockIndex++] = 0x80;
      while (_messageBlockIndex < 64)
      {
        _messageBlock[_messageBlockIndex++] = 0;
      }

      processMessageBlock();

      while (_messageBlockIndex < 56)
      {
        _messageBlock[_messageBlockIndex++] = 0;
      }
    }
    else
    {
      _messageBlock[_messageBlockIndex++] = 0x80;
      while (_messageBlockIndex < 56)
      {
        _messageBlock[_messageBlockIndex++] = 0;
      }

    }

    /*
     *  Store the message length as the last 8 octets
     */
    _messageBlock[56] = (_length_high >> 24) & 0xFF;
    _messageBlock[57] = (_length_high >> 16) & 0xFF;
    _messageBlock[58] = (_length_high >> 8) & 0xFF;
    _messageBlock[59] = (_length_high) & 0xFF;
    _messageBlock[60] = (_lengthLow >> 24) & 0xFF;
    _messageBlock[61] = (_lengthLow >> 16) & 0xFF;
    _messageBlock[62] = (_lengthLow >> 8) & 0xFF;
    _messageBlock[63] = (_lengthLow) & 0xFF;

    processMessageBlock();
  }

  struct sha1hash Sha1::hash()
  {
    padmessage();

    struct sha1hash r;
    int i = 0;

    r.bytes[i++] = (H[0] >> 24) & 0xFF;
    r.bytes[i++] = (H[0] >> 16) & 0xFF;
    r.bytes[i++] = (H[0] >> 8) & 0xFF;
    r.bytes[i++] = (H[0]) & 0xFF;

    r.bytes[i++] = (H[1] >> 24) & 0xFF;
    r.bytes[i++] = (H[1] >> 16) & 0xFF;
    r.bytes[i++] = (H[1] >> 8) & 0xFF;
    r.bytes[i++] = (H[1]) & 0xFF;

    r.bytes[i++] = (H[2] >> 24) & 0xFF;
    r.bytes[i++] = (H[2] >> 16) & 0xFF;
    r.bytes[i++] = (H[2] >> 8) & 0xFF;
    r.bytes[i++] = (H[2]) & 0xFF;

    r.bytes[i++] = (H[3] >> 24) & 0xFF;
    r.bytes[i++] = (H[3] >> 16) & 0xFF;
    r.bytes[i++] = (H[3] >> 8) & 0xFF;
    r.bytes[i++] = (H[3]) & 0xFF;

    r.bytes[i++] = (H[4] >> 24) & 0xFF;
    r.bytes[i++] = (H[4] >> 16) & 0xFF;
    r.bytes[i++] = (H[4] >> 8) & 0xFF;
    r.bytes[i++] = (H[4]) & 0xFF;
    
    reset();

    return r;
  }


  struct sha1hash sha1(const char* text, size_t len)
  {
    Sha1 x((const unsigned char*)(text), len);
    return x.hash();
  }

  uint32_t swapOrder32(uint32_t n)
  {
    if (!isBigEndian)
    {
      return
        ((n >> 24) & 0x000000FF) |
        ((n >> 8) & 0x0000FF00) |
        ((n << 8) & 0x00FF0000) |
        ((n << 24) & 0xFF000000);
    }
    else
    {
      return n;
    }
  }

  uint16_t swapOrder16(uint16_t n)
  {
    if (!isBigEndian)
    {
      return
        ((n >> 8) & 0x00FF) |
        ((n << 8) & 0xFF00);
    }
    return n;
  }

  /* Name string is a fully-qualified domain name */
  uuid_object NameSpace_DNS = { /* 6ba7b810-9dad-11d1-80b4-00c04fd430c8 */
      0x6ba7b810,
      0x9dad,
      0x11d1,
      0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8
  };

  uuid_object create_sha1_guid_from_name(const char* name, size_t namelen)
  {

    uuid_object uuid;

    /*put name space ID in network byte order so it hashes the same
      no matter what endian machine we're on */
      // namespace DNS
    uuid_object net_nsid = { /* 6ba7b810-9dad-11d1-80b4-00c04fd430c8 */
      0x6ba7b810,
      0x9dad,
      0x11d1,
      0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8
    };

    net_nsid.time_low = swapOrder32(net_nsid.time_low);
    net_nsid.time_mid = swapOrder16(net_nsid.time_mid);
    net_nsid.time_hi_and_version = swapOrder16(net_nsid.time_hi_and_version);

    Sha1 c;
    c.input((const uint8_t*)&net_nsid, sizeof(net_nsid));
    c.input((const uint8_t*)name, namelen);
    auto hash = c.hash();

    /* convert UUID to local byte order */
    memcpy(&uuid, hash.bytes, sizeof(uuid));
    uuid.time_low = swapOrder32(uuid.time_low);
    uuid.time_mid = swapOrder16(uuid.time_mid);
    uuid.time_hi_and_version = swapOrder16(uuid.time_hi_and_version);

    const auto v = 5;
    /* put in the variant and version bits */
    uuid.time_hi_and_version &= 0x0FFF;
    uuid.time_hi_and_version |= (v << 12);
    uuid.clock_seq_hi_and_reserved &= 0x3F;
    uuid.clock_seq_hi_and_reserved |= 0x80;

    return uuid;
  }
}
