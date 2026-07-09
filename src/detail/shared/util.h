#pragma once

#include <cstdint>
#include <string>

uint32_t fnv1a_keogh(const char *input);

// Function to shorten a string to a 4-character string
std::string ShortenString(const std::string &input);
