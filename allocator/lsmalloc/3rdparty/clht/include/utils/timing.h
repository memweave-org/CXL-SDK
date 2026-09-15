#ifndef TIMING_H
#define TIMING_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <tbb/concurrent_hash_map.h>
#include <tbb/concurrent_vector.h>
#include <unordered_map>
#include <vector>

// #define QUEUE_LEN_PERF
// #define QUEUE_LATENCY
// #define TRX_LATENCY
// #define QUEUE_LATENCY_BREAKDOWN
// #define BWTREE_RETRY_COUNT
// #define CLEVEL_DOUBLE_READ_COUNT
// #define TRX_TYPE_STAT
// #define BWTREE_RETRY_COUNT

/*
 * class Timer - Measures time usage for testing purpose
 */
class Timer {
public:
    /*
     * Constructor
     *
     * It takes an argument, which denotes whether the timer should start
     * immediately. By default it is true
     */
    Timer(bool start = true) : start{}, end{}, elapsed_times_array{128} {
        if (start == true) {
            Start();
        }
        return;
    }

    /*
     * Start() - Starts timer until Stop() is called
     *
     * Calling this multiple times without stopping it first will reset and
     * restart
     */
    inline void Start() {
        start = std::chrono::system_clock::now();
        return;
    }

    /*
     * Stop() - Stops timer and returns the duration between the previous Start()
     *          and the current Stop()
     *
     * Return value is represented in double, and is seconds elapsed between
     * the last Start() and this Stop()
     */
    inline double Stop() {
        end = std::chrono::system_clock::now();
        return GetInterval();
    }

    /*
     * GetInterval() - Returns the length of the time interval between the latest
     *                 Start() and Stop()
     */
    inline double GetInterval() const {
        std::chrono::duration<double> elapsed_seconds = end - start;
        return elapsed_seconds.count();
    }

    inline void Track(size_t thread_id) {
        double elapsed = Stop();
        if (thread_id >= elapsed_times_array.size()) {
            elapsed_times_array.resize(thread_id + 1);
        }
        elapsed_times_array[thread_id].push_back(elapsed);
    }

    void printElapsed(const std::string& message = "Elapsed time: ") const {
        std::cout << message << GetInterval() << " seconds" << std::endl;
    }

    void printAverage() const {
        if (elapsed_times_array.empty()) {
            std::cout << "No elapsed times to display." << std::endl;
            return;
        }

        double sum = 0;
        size_t count = 0;
        for (const auto& thread_times : elapsed_times_array) {
            for (const auto& elapsed : thread_times) {
                if (elapsed < 0)
                    continue;
                sum += elapsed;
                count++;
            }
        }

        if (count == 0) {
            std::cout << "No valid elapsed times to display." << std::endl;
            return;
        }

        std::cout << "Average elapsed time: " << sum / count << " seconds" << std::endl;
    }

    void printHistogram() const {
        if (elapsed_times_array.empty()) {
            std::cout << "No elapsed times to display." << std::endl;
            return;
        }

        const int num_bins = 10;
        const double max_time = 10.0;
        std::vector<int> bins(num_bins, 0);

        for (const auto& thread_times : elapsed_times_array) {
            for (const auto& elapsed : thread_times) {
                if (elapsed < 0)
                    continue;
                int bin_index = static_cast<int>(elapsed / (max_time / num_bins));
                if (bin_index >= num_bins)
                    bin_index = num_bins - 1;
                bins[bin_index]++;
            }
        }

        std::cout << "Histogram of elapsed times:" << std::endl;
        for (size_t i = 0; i < bins.size(); ++i) {
            double bin_range_start = i * (max_time / num_bins);
            double bin_range_end = (i + 1) * (max_time / num_bins);
            double proportion = static_cast<double>(bins[i]) / (max_time / num_bins);

            std::cout << std::fixed << std::setprecision(2) << "[" << bin_range_start << ", "
                      << bin_range_end << "): " << proportion << " (" << bins[i] << ")"
                      << std::endl;
        }
    }

private:
    std::chrono::time_point<std::chrono::system_clock> start;
    std::chrono::time_point<std::chrono::system_clock> end;
    std::vector<std::vector<double>> elapsed_times_array;  // Store elapsed times
};

class Counter {
public:
    void inline Start() {
#ifdef COUNTING
        counter = 0;
#endif
    }

    void inline Increment() {
#ifdef COUNTING
        counter++;
#endif
    }

    void inline Stop(size_t thread_id) {
        (void)thread_id;
#ifdef COUNTING
        if (thread_id >= counter_array.size()) {
            counter_array.resize(thread_id + 1);
        }
        counter_array[thread_id].push_back(counter);
#endif
    }

    void printHistogram() const {
#ifdef COUNTING
        if (counter_array.empty()) {
            std::cout << "No counter to display." << std::endl;
            return;
        }

        const int num_bins = 10;
        const double max_time = 10.0;
        std::vector<int> bins(num_bins, 0);

        for (const auto& counters : counter_array) {
            for (const auto& counter : counters) {
                int bin_index = static_cast<int>(counter / (max_time / num_bins));
                if (bin_index >= num_bins)
                    bin_index = num_bins - 1;
                bins[bin_index]++;
            }
        }

        std::cout << "Histogram of counter:" << std::endl;
        for (size_t i = 0; i < bins.size(); ++i) {
            double bin_range_start = i * (max_time / num_bins);
            double bin_range_end = (i + 1) * (max_time / num_bins);
            double proportion = static_cast<double>(bins[i]) / (max_time / num_bins);

            std::cout << std::fixed << std::setprecision(2) << "[" << bin_range_start << ", "
                      << bin_range_end << "): " << proportion << " (" << bins[i] << ")"
                      << std::endl;
        }
#else
        std::cout << "COUNTING is disabled, please turn on." << std::endl;
#endif
    }

private:
    static thread_local uint64_t counter;              // per-thread counter
    std::vector<std::vector<uint64_t>> counter_array;  // couter per ops
};

/*
 * class TimerInterface - Interface for timer classes. Defines Start() and
 * Stop() to mark the beginning and end of a time interval. When Stop() is
 * called, Record() is called automatically.
 */
class TimerInterface {
public:
    TimerInterface(std::string name = "") : name_{name} {}

    virtual ~TimerInterface() = default;

    virtual void Start() { start_ = std::chrono::high_resolution_clock::now(); }
    virtual void Stop() {
        end_ = std::chrono::high_resolution_clock::now();
        Record();
    }

    virtual void Record() const = 0;

protected:
    std::string name_;
    std::chrono::high_resolution_clock::time_point start_, end_;
};

/*
 * class TimerAverage - Timer class that records average time
 */
class TimerAverage : public TimerInterface {
public:
    TimerAverage(std::string name = "")
        : TimerInterface(name),
          total_time_{elapsed_times_[name]},
          total_count_(elapsed_counts_[name]) {}

    void Record() const override {
        auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end_ - start_);
        total_time_.fetch_add(elapsed.count(), std::memory_order_relaxed);
        total_count_.fetch_add(1, std::memory_order_relaxed);
    }

    static void Print(std::string name, std::ostream& os) {
        auto it_time = elapsed_times_.find(name);
        auto it_count = elapsed_counts_.find(name);
        auto time = it_time->second.load(std::memory_order_relaxed);
        auto count = it_count->second.load(std::memory_order_relaxed);
        if (count != 0) {
            os << "Average time for " << name << ": " << static_cast<double>(time) / count << " ns"
               << std::endl;
            os << "Total count for " << name << ": " << count << std::endl;
        } else {
            std::cout << "No measurements taken for " << name << std::endl;
        }
    }

    static void AddNewEntry(const std::string& name) {
        elapsed_times_.emplace(name, 0);
        elapsed_counts_.emplace(name, 0);
    }

private:
    std::atomic<uint64_t>& total_time_;
    std::atomic<uint64_t>& total_count_;

    static std::unordered_map<std::string, std::atomic<uint64_t>> elapsed_times_;
    static std::unordered_map<std::string, std::atomic<uint64_t>> elapsed_counts_;
};

/*
 * class TimerDetailed - Timer class that records detailed time, can be used to
 * draw a histogram, adding other data analysis is also possible
 */
class TimerDetailed : public TimerInterface {
public:
    TimerDetailed(std::string name = "")
        : TimerInterface(name), cache_elapsed_times_(elapsed_times_[name]) {}

    void Record() const override {
        auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end_ - start_);
        cache_elapsed_times_.push_back(elapsed);
    }

    static void AddNewEntry(const std::string name) {
        elapsed_times_.emplace(name, tbb::concurrent_vector<std::chrono::nanoseconds>{});
    }

    static void PrintHistogram(const std::string name, int num_groups = 10,
                               int max_bar_length = 50) {
        auto it = elapsed_times_.find(name);
        if (it == elapsed_times_.end()) {
            std::cerr << "Error: No data found for " << name << std::endl;
            return;
        }

        const auto& times = it->second;
        if (times.empty()) {
            std::cerr << "Error: No data in the vector for " << name << std::endl;
            return;
        }

        auto minmax = std::minmax_element(times.begin(), times.end());
        auto min_time = *minmax.first;
        auto max_time = *minmax.second;
        auto range = max_time - min_time;
        auto group_size = range / num_groups;

        std::vector<int> histogram(num_groups, 0);
        for (const auto& time : times) {
            int group_index = (time - min_time) / group_size;
            if (group_index >= num_groups) {
                group_index = num_groups - 1;
            }
            histogram[group_index]++;
        }

        int max_count = *std::max_element(histogram.begin(), histogram.end());
        for (int i = 0; i < num_groups; ++i) {
            auto group_start = min_time + group_size * i;
            auto group_end = group_start + group_size;
            int bar_length = (histogram[i] * max_bar_length) / max_count;
            std::cout << "[" << group_start.count() << " ns - " << group_end.count()
                      << " ns]: " << std::string(bar_length, '*') << std::endl;
        }
    }

    static void Print(const std::string name, std::ostream& os) {
        auto it = elapsed_times_.find(name);
        if (it == elapsed_times_.end()) {
            std::cerr << "Error: No data found for " << name << std::endl;
            return;
        }

        const auto& times = it->second;
        if (times.empty()) {
            std::cerr << "Error: No data in the vector for " << name << std::endl;
            return;
        }

        os << "Elapsed times for " << name << ":" << std::endl;
        for (const auto& time : times) {
            os << time.count() << " ns" << std::endl;
        }
    }

    static void PrintAll(std::ostream& os) {
        for (const auto& entry : elapsed_times_) {
            const auto& name = entry.first;
            const auto& times = entry.second;
            os << "Elapsed times for " << name << ":" << std::endl;
            for (const auto& time : times) {
                os << time.count() << " ns" << std::endl;
            }
        }
    }

    static void ClearAll() { elapsed_times_.clear(); }

private:
    tbb::concurrent_vector<std::chrono::nanoseconds>& cache_elapsed_times_;
    static std::unordered_map<std::string, tbb::concurrent_vector<std::chrono::nanoseconds>>
        elapsed_times_;
};

/*
 * class CallCounter - Counter class that records the number of calls
 */
class CallCounter {
public:
    CallCounter(const char* name) : name_(name), count_(counts_[name]) {}
    CallCounter(std::string name = "") : name_(name), count_(counts_[name]) {}

    void Increment() const { count_.fetch_add(1, std::memory_order_relaxed); }

    static void Print(std::string name, std::ostream& os) {
        auto it = counts_.find(name);
        if (it == counts_.end()) {
            std::cerr << "Error: No data found for " << name << std::endl;
            return;
        }
        auto count = it->second.load(std::memory_order_relaxed);
        os << "Count for " << name << ": " << count << std::endl;
    }

    static void AddNewEntry(const std::string& name) { counts_.emplace(name, 0); }

private:
    std::string name_;
    std::atomic<uint64_t>& count_;

    static std::unordered_map<std::string, std::atomic<uint64_t>> counts_;
};

/*
 * class Logger - Logger class that records values, can be any type including
 * int and string
 */
template <typename T>
class Logger {
public:
    Logger(std::string name) : name_{name}, local_values_{values_[name]} {}

    void Log(const T& value) { local_values_.push_back(value); }

    static void Print(const std::string& name, std::ostream& os) {
        auto it = values_.find(name);
        if (it == values_.end()) {
            std::cerr << "Error: No data found for " << name << std::endl;
            return;
        }
        for (const auto& value : it->second) {
            os << value << std::endl;
        }
    }

    static void AddNewEntry(const std::string& name) {
        if (values_.find(name) == values_.end()) {
            values_.emplace(name, tbb::concurrent_vector<T>{});
        }
    }

private:
    std::string name_;
    tbb::concurrent_vector<T>& local_values_;
    static std::unordered_map<std::string, tbb::concurrent_vector<T>> values_;
};

extern void InitStatistics();
extern void PrintStatistics();

#endif  // TIMING_H
