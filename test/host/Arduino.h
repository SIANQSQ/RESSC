#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <cstdarg>
#include <cstdio>
#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define INPUT_PULLUP 2
extern uint32_t fakeUs;
struct Edge { uint32_t at; uint8_t pin; int level; };
extern std::vector<Edge> edges;
extern int levels[64];
inline uint32_t micros() { return fakeUs; }
inline uint32_t millis() { return fakeUs / 1000; }
inline int pinModes[64] = {};
inline void pinMode(uint8_t pin, int mode) { pinModes[pin] = mode; }
inline int digitalRead(uint8_t pin) { return levels[pin]; }
inline void digitalWrite(uint8_t pin, int value) {
  if (levels[pin] != value) edges.push_back({fakeUs, pin, value});
  levels[pin] = value;
}
struct MockSerial {
  std::string output, incoming;
  size_t cursor = 0;
  void begin(int) {}
  int available() { return static_cast<int>(incoming.size() - cursor); }
  int read() { return incoming[cursor++]; }
  template<class T> void println(const T&) {}
  void print(const char *s) { output += s; }
  void println(const char *s) { output += s; output += '\n'; }
  void printf(const char *fmt, ...) {
    char buffer[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    output += buffer;
  }
};
extern MockSerial Serial;
