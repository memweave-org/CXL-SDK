#pragma once
// Alignment-safe NT memcpy; no explicit source or destination cache flush.
#include <cstddef>
#include <cstdint>
#include "utils/platform.h"
#ifdef __x86_64__
#include <immintrin.h>
#endif

namespace cxl_transport::nt_copy {
inline void stream_copy(void* dst, const void* src, size_t bytes) {
#ifdef SHM_LIB_WITH_UBSM
  auto* out=static_cast<volatile unsigned char*>(dst);
  auto* in=static_cast<const volatile unsigned char*>(src);
  for(size_t i=0;i<bytes;++i) out[i]=in[i];
  shm_platform::full_fence();
#elif defined(__x86_64__)
  auto* out = static_cast<unsigned char*>(dst);
  const auto* in = static_cast<const unsigned char*>(src);
  size_t i = 0;
  // Volatile byte fragments prevent the compiler from replacing the fallback
  // with libc memcpy. Never read past a requested short range.
  for (; i < bytes && (uintptr_t(out + i) & 15); ++i)
    static_cast<volatile unsigned char*>(dst)[i] =
        static_cast<const volatile unsigned char*>(src)[i];
  for (; i + 16 <= bytes; i += 16) {
    auto* p = reinterpret_cast<const __m128i*>(in + i);
    const auto value = (uintptr_t(p) & 15)
        ? _mm_loadu_si128(p)
        : _mm_stream_load_si128(const_cast<__m128i*>(p));
    _mm_stream_si128(reinterpret_cast<__m128i*>(out + i), value);
  }
  if (i + 8 <= bytes) {
    const auto value = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(in + i));
    _mm_stream_si64(reinterpret_cast<long long*>(out + i), _mm_cvtsi128_si64(value));
    i += 8;
  }
  for (; i < bytes; ++i)
    static_cast<volatile unsigned char*>(dst)[i] =
        static_cast<const volatile unsigned char*>(src)[i];
  _mm_sfence();
#elif defined(__aarch64__)
  // ARM fallback uses explicit cache maintenance, not x86 streaming opcodes.
  clflush(src,bytes);
  auto* out=static_cast<volatile unsigned char*>(dst);
  const auto* in=static_cast<const volatile unsigned char*>(src);
  for(size_t i=0;i<bytes;++i) out[i]=in[i];
  clwb(dst,bytes);
  shm_platform::full_fence();
#else
#error Unsupported architecture
#endif
}

}
