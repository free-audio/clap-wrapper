#include "util.h"

#include "detail/shared/util.h"

#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <algorithm>
#include <unordered_set>

/*
    The functions in this module generate the various identifier values required by AAX.
    Instead of reyling on hardcoded IDs we derive deterministic FourCC identifiers by
    hashing well-defined strings, retrieved from the CLAP.
*/

// clang_format off

// fnv1a_keogh is an implementation of a Fowler-Noll-Vo hash function
// see https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function
uint32_t fnv1a_keogh(const char *input)
{
  uint32_t hash = 0x811c9dc5;

  while (*input)
  {
    hash ^= *input++;
    hash *= 0x01000193;

    // LCG
    hash = (0x19660d * hash) + 0x3c6ef35f;
  }

  return hash;
}

static inline char gen(uint32_t m)
{
  return "0123456789abcdef"[m & 0xF];
}

// creates a AAX string based id, which must not be larger than 32 characters.
std::string createAAXId(clap_id id)
{
  std::string result;
  uint32_t n = 32;
  while (n > 0)
  {
    n -= 4;
    result.push_back(gen(id >> n));
  }
  return result;
}

// an AAXID is a FourCC
static const char _map[64] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz$";
uint32_t AAXIDfromString(const char *str)
{
  // re-use the standard hash function
  auto p = fnv1a_keogh(str);

  // use only 24 bit of it
  uint32_t res = _map[((p >> 0) & 0x3f)] << 24 | _map[((p >> 6) & 0x3f)] << 16 |
                 _map[((p >> 12) & 0x3f)] << 8 | _map[((p >> 18) & 0x3f)];
  return res;
}

uint32_t AAXIDfromString(const std::string &str)
{
  return AAXIDfromString(str.c_str());
}

std::vector<std::string> generateShortStrings(const std::string &input)
{
  std::vector<std::string> result;

  // Original String
  result.push_back(input);

  // Removing Spaces
  std::string noSpaces = input;
  noSpaces.erase(std::remove(noSpaces.begin(), noSpaces.end(), ' '), noSpaces.end());
  result.push_back(noSpaces);

  std::istringstream iss(input);
  std::string word;
  std::string camelCase;

  std::string threeword;
  while (iss >> word)
  {
    threeword += word[0];
    if (word.length() > 1)
    {
      threeword += word[1];
    }
    if (word.length() > 2)
    {
      threeword += word[2];
    }
  }
  result.push_back(threeword);

  // Only the first and last characters of a word
  std::string firstLast;
  iss.clear();
  iss.str(input);
  while (iss >> word)
  {
    firstLast += word[0];
    if (word.length() > 1)
    {
      firstLast += word[word.length() - 1];
    }
  }
  result.push_back(firstLast);

  // Only the first characters of a word
  std::string initials;
  iss.clear();
  iss.str(input);
  while (iss >> word)
  {
    initials += word[0];
  }
  result.push_back(initials);

  // if there is no two character short string, make one
  if (result.back().length() > 2)
  {
    iss.clear();
    iss.str(input);
    iss >> word;
    firstLast += word[0];
    if (word.length() > 1)
    {
      firstLast += word[1];
    }
  }

  return result;
}

// Function to shorten a string to a 4-character string
std::string ShortenString(const std::string &input)
{
  std::istringstream iss(input);
  std::vector<std::string> words;
  std::string word;

  // Split the string into words
  while (iss >> word)
  {
    words.push_back(word);
  }

  std::string result;

  // Add the first letters of the words
  for (const auto &w : words)
  {
    result += w[0];
  }

  // If the resulting string is shorter than 4 characters, add more letters
  if (result.length() < 4)
  {
    for (const auto &w : words)
    {
      for (size_t i = 1; i < w.length() && result.length() < 4; ++i)
      {
        result += w[i];
      }
    }
  }

  // If the resulting string is longer than 4 characters, shorten it to 4 characters
  if (result.length() > 4)
  {
    // Remove vowels if necessary
    std::unordered_set<char> vowels = {'a', 'e', 'i', 'o', 'u', 'A', 'E', 'I', 'O', 'U'};
    std::string temp;
    for (char c : result)
    {
      if (vowels.find(c) == vowels.end() || temp.length() >= 4)
      {
        temp += c;
      }
    }
    result = temp.substr(0, 4);
  }

  return result;
}

// clang_format on