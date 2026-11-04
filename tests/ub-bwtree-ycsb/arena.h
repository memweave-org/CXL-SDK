#pragma once
#include <cstddef>
void configure_arena(void* base, size_t bytes);
void configure_nc_arena(void* base, size_t bytes, bool owner);
size_t arena_used();
size_t nc_arena_used();
