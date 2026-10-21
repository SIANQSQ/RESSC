#pragma once
#include <string>
#include <type_traits>
#include <cstdio>
class String {
  std::string value;
public:
  String() = default;
  String(const char *s) : value(s) {}
  String(const std::string &s) : value(s) {}
  String(double n, unsigned decimals) {
    char text[128]; std::snprintf(text, sizeof(text), "%.*f", static_cast<int>(decimals), n); value = text;
  }
  template<class T, typename std::enable_if<std::is_integral<T>::value,int>::type = 0>
  String(T n) : value(std::to_string(n)) {}
  const char *c_str() const { return value.c_str(); }
  size_t length() const { return value.length(); }
  String &operator+=(const String &s) { value += s.value; return *this; }
  String &operator+=(char c) { value += c; return *this; }
  friend String operator+(String a, const String &b) { return a += b; }
  friend bool operator==(const String &a, const String &b) { return a.value == b.value; }
};
