#pragma once
struct MockMDNS {
  bool begin(const char*) { return true; }
  void addService(const char*, const char*, unsigned) {}
  void end() {}
};
inline MockMDNS MDNS;
