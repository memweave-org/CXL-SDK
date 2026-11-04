#pragma once
#include <cstddef>
void configure_arena(void* base, size_t bytes);
size_t arena_used();
