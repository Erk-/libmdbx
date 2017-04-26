/*
 * Copyright 2017 Leonid Yuriev <leo@yuriev.ru>
 * and other libmdbx authors: please see AUTHORS file.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 */

#include "test.h"
#include <float.h>
#ifndef _MSC_VER
#include <ieee754.h>
#endif

std::string format(const char *fmt, ...) {
  va_list ap, ones;
  va_start(ap, fmt);
  va_copy(ones, ap);
#ifdef _MSC_VER
  int needed = _vscprintf(fmt, ap);
#else
  int needed = vsnprintf(nullptr, 0, fmt, ap);
#endif
  assert(needed >= 0);
  va_end(ap);
  std::string result;
  result.reserve((size_t)needed + 1);
  result.resize((size_t)needed, '\0');
  int actual = vsnprintf((char *)result.data(), result.capacity(), fmt, ones);
  assert(actual == needed);
  (void)actual;
  va_end(ones);
  return result;
}

std::string data2hex(const void *ptr, size_t bytes, simple_checksum &checksum) {
  std::string result;
  if (bytes > 0) {
    const uint8_t *data = (const uint8_t *)ptr;
    checksum.push(data, bytes);
    result.reserve(bytes * 2);
    const uint8_t *const end = data + bytes;
    do {
      char h = *data >> 4;
      char l = *data & 15;
      result.push_back((l < 10) ? l + '0' : l - 10 + 'a');
      result.push_back((h < 10) ? h + '0' : h - 10 + 'a');
    } while (++data < end);
  }
  assert(result.size() == bytes * 2);
  return result;
}

bool hex2data(const char *hex_begin, const char *hex_end, void *ptr,
              size_t bytes, simple_checksum &checksum) {
  if (bytes * 2 != (size_t)(hex_end - hex_begin))
    return false;

  uint8_t *data = (uint8_t *)ptr;
  for (const char *hex = hex_begin; hex != hex_end; hex += 2, ++data) {
    unsigned l = hex[0], h = hex[1];

    if (l >= '0' && l <= '9')
      l = l - '0';
    else if (l >= 'A' && l <= 'F')
      l = l - 'A' + 10;
    else if (l >= 'a' && l <= 'f')
      l = l - 'a' + 10;
    else
      return false;

    if (h >= '0' && h <= '9')
      h = h - '0';
    else if (h >= 'A' && h <= 'F')
      h = h - 'A' + 10;
    else if (h >= 'a' && h <= 'f')
      h = h - 'a' + 10;
    else
      return false;

    uint32_t c = l + (h << 4);
    checksum.push(c);
    *data = c;
  }
  return true;
}

//-----------------------------------------------------------------------------

#ifdef __mips__
static uint64_t *mips_tsc_addr;

__cold static void mips_rdtsc_init() {
  int mem_fd = open("/dev/mem", O_RDONLY | O_SYNC, 0);
  HIPPEUS_ENSURE(mem_fd >= 0);

  mips_tsc_addr = mmap(nullptr, pagesize, PROT_READ, MAP_SHARED, mem_fd,
                       0x10030000 /* MIPS_ZBUS_TIMER */);
  close(mem_fd);
}
#endif /* __mips__ */

uint64_t entropy_ticks(void) {
#if defined(__GNUC__) || defined(__clang__)
#if defined(__ia64__)
  uint64_t ticks;
  __asm("mov %0=ar.itc" : "=r"(ticks));
  return ticks;
#elif defined(__hppa__)
  uint64_t ticks;
  __asm("mfctl 16, %0" : "=r"(ticks));
  return ticks;
#elif defined(__s390__)
  uint64_t ticks;
  __asm("stck 0(%0)" : : "a"(&(ticks)) : "memory", "cc");
  return ticks;
#elif defined(__alpha__)
  uint64_t ticks;
  __asm("rpcc %0" : "=r"(ticks));
  return ticks;
#elif defined(__sparc_v9__)
  uint64_t ticks;
  __asm("rd %%tick, %0" : "=r"(ticks));
  return ticks;
#elif defined(__powerpc64__) || defined(__ppc64__)
  uint64_t ticks;
  __asm("mfspr %0, 268" : "=r"(ticks));
  return ticks;
#elif defined(__ppc__) || defined(__powerpc__)
  unsigned tbl, tbu;

  /* LY: Here not a problem if a high-part (tbu)
   * would been updated during reading. */
  __asm("mftb %0" : "=r"(tbl));
  __asm("mftbu %0" : "=r"(tbu));

  return (((uin64_t)tbu0) << 32) | tbl;
#elif defined(__mips__)
  if (mips_tsc_addr != MAP_FAILED) {
    if (unlikely(!mips_tsc_addr)) {
      static pthread_once_t is_initialized = PTHREAD_ONCE_INIT;
      int rc = pthread_once(&is_initialized, mips_rdtsc_init);
      if (unlikely(rc))
        failure_perror("pthread_once()", rc);
    }
    if (mips_tsc_addr != MAP_FAILED)
      return *mips_tsc_addr;
  }
#elif defined(__x86_64__) || defined(__i386__)
  unsigned lo, hi;

  /* LY: Using the "a" and "d" constraints is important for correct code. */
  __asm("rdtsc" : "=a"(lo), "=d"(hi));

  return (((uint64_t)hi) << 32) + lo;
#endif /* arch selector */

#elif defined(_M_IX86) || defined(_M_X64)
  return __rdtsc();
#endif /* __GNUC__ || __clang__ */

#if defined(_WIN32) || defined(_WIN64) || defined(_WINDOWS)
  LARGE_INTEGER PerformanceCount;
  if (QueryPerformanceCounter(&PerformanceCount))
    return PerformanceCount.QuadPart;
  return GetTickCount64();
#else
  struct timespec ts;
#if defined(CLOCK_MONOTONIC_COARSE)
  clockid_t clock = CLOCK_MONOTONIC_COARSE;
#elif defined(CLOCK_MONOTONIC_RAW)
  clockid_t clock = CLOCK_MONOTONIC_RAW;
#else
  clockid_t clock = CLOCK_MONOTONIC;
#endif
  int rc = clock_gettime(clock, &ts);
  if (unlikely(rc))
    failure_perror("clock_gettime()", rc);

  return (((uint64_t)ts.tv_sec) << 32) + ts.tv_nsec;
#endif
}

//-----------------------------------------------------------------------------

static __inline uint64_t bleach64(uint64_t dirty) {
  dirty = mul_64x64_high(bswap64(dirty), UINT64_C(17048867929148541611));
  return dirty;
}

static __inline uint32_t bleach32(uint32_t dirty) {
  return (uint32_t)(
      (bswap32(dirty) * UINT64_C(/*3080105489, 4267077937 */ 2175734609)) >>
      32);
}

uint64_t prng64_careless(uint64_t &state) {
  state = state * UINT64_C(6364136223846793005) + 1;
  return state;
}

uint64_t prng64_white(uint64_t &state) {
  state = state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
  return bleach64(state);
}

uint32_t prng32(uint64_t &state) {
  return (uint32_t)(prng64_careless(state) >> 32);
}

uint64_t entropy_white() { return bleach64(entropy_ticks()); }

double double_from_lower(uint64_t salt) {
#ifdef IEEE754_DOUBLE_BIAS
  ieee754_double r;
  r.ieee.negative = 0;
  r.ieee.exponent = IEEE754_DOUBLE_BIAS;
  r.ieee.mantissa0 = (unsigned)(salt >> 32);
  r.ieee.mantissa1 = (unsigned)salt;
  return r.d;
#else
  const uint64_t top = (UINT64_C(1) << DBL_MANT_DIG) - 1;
  const double scale = 1.0 / (double)top;
  return (salt & top) * scale;
#endif
}

double double_from_upper(uint64_t salt) {
#ifdef IEEE754_DOUBLE_BIAS
  ieee754_double r;
  r.ieee.negative = 0;
  r.ieee.exponent = IEEE754_DOUBLE_BIAS;
  salt >>= 64 - DBL_MANT_DIG;
  r.ieee.mantissa0 = (unsigned)(salt >> 32);
  r.ieee.mantissa1 = (unsigned)salt;
  return r.d;
#else
  const uint64_t top = (UINT64_C(1) << DBL_MANT_DIG) - 1;
  const double scale = 1.0 / (double)top;
  return (salt >> (64 - DBL_MANT_DIG)) * scale;
#endif
}

bool flipcoin() { return bleach32((uint32_t)entropy_ticks()) & 1; }

bool jitter(unsigned probability_percent) {
  const uint32_t top = UINT32_MAX - UINT32_MAX % 100;
  uint32_t dice, edge = (top) / 100 * probability_percent;
  do
    dice = bleach32((uint32_t)entropy_ticks());
  while (dice >= top);
  return dice < edge;
}

void jitter_delay(bool extra) {
  unsigned dice = entropy_white() & 3;
  if (dice == 0) {
    log_trace("== jitter.no-delay");
  } else {
    log_trace(">> jitter.delay: dice %u", dice);
    do {
      cpu_relax();
      memory_barrier();
      cpu_relax();
      if (dice > 1) {
        osal_yield();
        cpu_relax();
        if (dice > 2) {
          unsigned us = entropy_white() &
                        (extra ? 0xfffff /* 1.05 s */ : 0x3ff /* 1 ms */);
          log_trace("== jitter.delay: %0.6f", us / 1000000.0);
          osal_udelay(us);
        }
      }
    } while (flipcoin());
    log_trace("<< jitter.delay: dice %u", dice);
  }
}
