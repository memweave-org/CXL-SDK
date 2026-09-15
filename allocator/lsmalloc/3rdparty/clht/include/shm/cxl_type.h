#pragma once
#include <vector>
#include <string>
#include "mm.h"

template <typename T>
using cxl_string = std::basic_string<T, std::char_traits<T>, CXLAllocator<T>>;

template <typename T>
using cxl_vector = std::vector<T, CXLAllocator<T>>;