#pragma once
#include <string>
#include <stdexcept>
#include <fstream>
#include <sstream>
#include <chrono>
#include <ctime>
#include <iomanip>

inline std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open file: " + path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

inline std::string now_utc_iso_z() {
  using namespace std::chrono;
  auto now = system_clock::now();
  std::time_t t = system_clock::to_time_t(now);
  std::tm gmt{};
#if defined(_WIN32)
  gmtime_s(&gmt, &t);
#else
  gmtime_r(&t, &gmt);
#endif
  std::ostringstream ss;
  ss << std::put_time(&gmt, "%Y-%m-%dT%H:%M:%SZ");
  return ss.str();
}