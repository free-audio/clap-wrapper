#pragma once

#include <iostream>
#include <iomanip>
#include <atomic>
#define TRACE std::cout << __FILE__ << ":" << __LINE__ << ": (" << __func__ << ")" << std::endl
#define LOG std::cout << __FILE__ << ":" << __LINE__ << ": (" << __func__ << ") "
