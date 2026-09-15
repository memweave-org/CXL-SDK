#pragma once

#include <chrono>  // For std::chrono (to get current time)
#include <cstdio>  // For snprintf, if we decide to use it for initial formatting (not directly used here for {} parsing)
#include <iomanip>   // For std::put_time (to format time)
#include <iostream>  // For std::cerr and std::endl
#include <sstream>   // For std::stringstream (used for converting types to string)
#include <string>    // For std::string
#include <utility>   // For std::forward

// --- 1. Define Log Levels ---
enum LogLevel { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR };

// --- 2. ANSI Color Codes ---
// These are special sequences recognized by most modern terminals.
// They change the text color.
namespace LogColors {
const std::string RESET = "\033[0m";  // Resets all attributes
const std::string BLACK = "\033[30m";
const std::string RED = "\033[31m";
const std::string GREEN = "\033[32m";
const std::string YELLOW = "\033[33m";
const std::string BLUE = "\033[34m";
const std::string MAGENTA = "\033[35m";
const std::string CYAN = "\033[36m";
const std::string WHITE = "\033[37m";
const std::string BRIGHT_BLACK = "\033[90m";
const std::string BRIGHT_RED = "\033[91m";
const std::string BRIGHT_GREEN = "\033[92m";
const std::string BRIGHT_YELLOW = "\033[93m";
const std::string BRIGHT_BLUE = "\033[94m";
const std::string BRIGHT_MAGENTA = "\033[95m";
const std::string BRIGHT_CYAN = "\033[96m";
const std::string BRIGHT_WHITE = "\033[97m";
}  // namespace LogColors

// --- 3. Internal Helper: Convert LogLevel to String ---
inline const char* get_log_level_string(LogLevel level) {
    switch (level) {
        case LOG_DEBUG:
            return "DEBUG";
        case LOG_INFO:
            return "INFO";
        case LOG_WARN:
            return "WARN";
        case LOG_ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";  // Should not happen
    }
}

// --- 4. Internal Helper: Get Color Code for LogLevel ---
inline const std::string& get_log_level_color(LogLevel level) {
    switch (level) {
        case LOG_DEBUG:
            return LogColors::BRIGHT_BLACK;  // Or a less intense color
        case LOG_INFO:
            return LogColors::GREEN;
        case LOG_WARN:
            return LogColors::YELLOW;
        case LOG_ERROR:
            return LogColors::RED;
        default:
            return LogColors::RESET;  // Default to no color
    }
}

// --- 5. Internal Helper: Convert various types to std::string ---
// Base template for generic types that have operator<< defined for std::stringstream
template <typename T>
std::string to_string_helper(const T& value) {
    std::stringstream ss;
    ss << value;
    return ss.str();
}

inline std::string to_string_helper(const bool& value) {
    return value ? "true" : "false";
}

// Specialization for const char* to ensure it's treated as a string, not a pointer address
inline std::string to_string_helper(const char* value) {
    return (value != nullptr) ? std::string(value) : "(null)";
}

// Specialization for std::string itself
inline std::string to_string_helper(const std::string& value) {
    return value;
}

// --- 6. Variadic Template Function for {} style String Formatting ---

// Base case: No more arguments to format, return the current string
inline std::string format_string_impl(const std::string& format_str) {
    // If there are still "{}" placeholders left, it means more placeholders than arguments were
    // provided. We return the string as is, possibly with unreplaced placeholders.
    return format_str;
}

// Recursive case: Processes one argument and replaces the next "{}"
template <typename T, typename... Args>
std::string format_string_impl(const std::string& format_str, const T& arg, Args&&... args) {
    std::string placeholder = "{}";
    size_t pos = format_str.find(placeholder);

    if (pos == std::string::npos) {
        // No more placeholders found, return the current string.
        // Remaining arguments will be ignored. This could be an error case in a more robust
        // formatter.
        return format_str;
    }

    // Split the string into parts: before placeholder, and after placeholder
    std::string before_placeholder = format_str.substr(0, pos);
    std::string after_placeholder = format_str.substr(pos + placeholder.length());

    // Convert the current argument to string
    std::string current_arg_str = to_string_helper(arg);

    // Recursively call with the rest of the string and arguments
    // The result is concatenated to the 'before' part and the current argument's string
    // representation.
    return before_placeholder + current_arg_str +
           format_string_impl(after_placeholder, std::forward<Args>(args)...);
}

// --- 7. Main Logging Core Function ---
// This function receives the fully formatted message string.
// In a real logger, this is where you'd add mutex for thread safety,
// write to file, send to network, etc.
inline void actual_log(LogLevel level, const std::string& message, const char* file, int line) {
    // Get current time
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);

    // Format time (e.g., %Y-%m-%d %H:%M:%S)
    std::stringstream ss_time;
    ss_time << std::put_time(std::localtime(&in_time_t), "%H:%M:%S");

    // Get the color for the current log level
    const std::string& color = get_log_level_color(level);

    // Extract just the filename from the full path
    std::string filename_str = file;
    size_t last_slash = filename_str.find_last_of("/\\");
    std::string short_filename =
        (last_slash == std::string::npos) ? filename_str : filename_str.substr(last_slash + 1);

    // Output to standard error stream (commonly used for logs)
    // Prepend color code, then print message, then append reset code.
    std::stringstream log_ss;
    log_ss << color << "[" << ss_time.str() << "] "
           << "[" << get_log_level_string(level) << "] "
           << "[" << short_filename << ":" << line << "] "  // Added file and line
           << message << LogColors::RESET << std::endl;     // Reset color after printing
    std::cerr << log_ss.str();
}

// --- 8. User-Facing Macro for Logging ---
// This macro simplifies the call for the user.
// It uses ##__VA_ARGS__ to handle the case where no variadic arguments are provided
// (e.g., LOG(INFO, "Simple message")).
#define LOG(level, format, ...) \
    actual_log(level, format_string_impl(format, ##__VA_ARGS__), __FILE__, __LINE__)
#define LOG_INFO(format, ...) LOG(LOG_INFO, format, ##__VA_ARGS__)
#define LOG_DEBUG(format, ...) LOG(LOG_DEBUG, format, ##__VA_ARGS__)
#define LOG_WARN(format, ...) LOG(LOG_WARN, format, ##__VA_ARGS__)
#define LOG_ERROR(format, ...) LOG(LOG_ERROR, format, ##__VA_ARGS__)
