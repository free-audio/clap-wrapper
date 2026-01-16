#pragma once

#include <cstdint>
#include <string>

#if __cplusplus >= 202002L
#include <bit>
#endif

uint32_t fnv1a_keogh(const char *input);

// Function to shorten a string to a 4-character string
std::string ShortenString(const std::string &input);

inline uint32_t popcount64(uint64_t x)
{
#if __cplusplus >= 202002L
  return std::popcount(x);
#else
  x = x - ((x >> 1) & 0x5555555555555555ULL);
  x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
  x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
  return (x * 0x0101010101010101ULL) >> 56;
#endif
}
