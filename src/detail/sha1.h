#pragma once
#include <cstdint>
#include <stddef.h>
#include <stdint.h>

namespace Crypto
{

  struct sha1hash
  {
    uint8_t bytes[20];
  };

  struct sha1hash sha1(char* text, size_t len);

  typedef struct {
    uint32_t  time_low;
    uint16_t  time_mid;
    uint16_t  time_hi_and_version;
    uint8_t   clock_seq_hi_and_reserved;
    uint8_t   clock_seq_low;
    uint8_t   node[6];
  } uuid_object;  

  uuid_object create_sha1_guid_from_name(const char* name, size_t namelen);
}

