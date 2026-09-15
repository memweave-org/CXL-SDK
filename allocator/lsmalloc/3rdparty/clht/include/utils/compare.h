#pragma once

template <typename T>
bool key_equal(const T& a, const T& b);

template <>
bool key_equal<const char*>(const char* const& a, const char* const& b);

template <typename T>
int key_compare(const T& a, const T& b);

template <>
int key_compare<const char*>(const char* const& a, const char* const& b);

template <typename T>
void assign_to_shm(T& dst, const T& src);

template <>
void assign_to_shm<const char*>(const char*& dst, const char* const& src);

template <typename T>
void assign_to_local(T& dst, const T& src);

template <>
void assign_to_local<const char*>(const char*& dst, const char* const& src);