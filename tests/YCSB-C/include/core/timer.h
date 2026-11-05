//
//  timer.h
//  YCSB-C
//
//  Created by Jinglei Ren on 12/19/14.
//  Copyright (c) 2014 Jinglei Ren <jinglei@ren.systems>.
//

#ifndef YCSB_C_TIMER_H_
#define YCSB_C_TIMER_H_

#include <chrono>
#ifdef SNIPER
#include "utils/sim_api.h"
#endif

namespace utils {

template <typename T> class Timer {
public:
  void Start() {
    time_ = getNow();
  }

  T End() {
    Duration span;
    Clock::time_point t = getNow();
    span = std::chrono::duration_cast<Duration>(t - time_);
    return span.count();
  }

private:
  // Elapsed transaction time must not follow wall-clock corrections (NTP or
  // manual clock changes). high_resolution_clock can alias system_clock.
  typedef std::chrono::steady_clock Clock;
  typedef std::chrono::duration<T> Duration;

  Clock::time_point getNow() {
#ifdef SNIPER
    auto dur =
        std::chrono::nanoseconds(static_cast<long long>(SimGetEmuTime()));
    return Clock::time_point(std::chrono::duration_cast<Clock::duration>(dur));
#else
    return Clock::now();
#endif
  }

  Clock::time_point time_;
};

} // namespace utils

#endif // YCSB_C_TIMER_H_
