#include "util.h"

#include <sstream>
#include <vector>
#include <unordered_set>

// fnv1a_keogh is an implementation of a Fowler-Noll-Vo hash function
// see https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function
//
// note: this variant applies an additional LCG step per input byte, so it is
// NOT interchangeable with a standard FNV-1a. It is used to derive stable
// plugin identifiers (AAX FourCCs, AUv3 subtypes) - changing it changes
// shipped IDs.
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
