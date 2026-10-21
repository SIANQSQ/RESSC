#pragma once
#include "WString.h"
#include <functional>
#include <map>
#include <vector>
constexpr int HTTP_GET = 0;
class WebServer {
public:
  std::vector<std::pair<String,String>> query;
  std::map<std::string,std::function<void()>> handlers;
  std::function<void()> notFound;
  int code = 0;
  String body, cacheHeader;
  explicit WebServer(unsigned) {}
  void on(const char *path, int, std::function<void()> handler) { handlers[path] = handler; }
  void onNotFound(std::function<void()> handler) { notFound = handler; }
  int args() { return static_cast<int>(query.size()); }
  String argName(int i) { return query[i].first; }
  bool hasArg(const String &key) { for (const auto &v : query) if (v.first == key) return true; return false; }
  String arg(const String &key) { for (const auto &v : query) if (v.first == key) return v.second; return ""; }
  void sendHeader(const char *name, const char *value) { if (std::string(name) == "Cache-Control") cacheHeader = value; }
  void send(int status, const char*, const String &data) { code = status; body = data; }
  void begin() {}
  void stop() {}
  void handleClient() {}
};
