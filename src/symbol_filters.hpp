#pragma once
#include <string>
#include <unordered_map>
#include <cmath>
#include <stdexcept>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

struct SymbolFilters {
  double stepSize{0.0};     // LOT_SIZE step
  double minQty{0.0};       // LOT_SIZE minQty
  double minNotional{0.0};  // MIN_NOTIONAL (if present)
};

inline double _to_double_safe(const json& j, const std::string& key, double def=0.0) {
  if (!j.contains(key)) return def;
  if (j[key].is_string()) return std::stod(j[key].get<std::string>());
  if (j[key].is_number()) return j[key].get<double>();
  return def;
}

inline std::unordered_map<std::string, SymbolFilters>
parse_exchange_info_filters_um(const json& exchange_info) {
  std::unordered_map<std::string, SymbolFilters> out;
  if (!exchange_info.contains("symbols")) return out;

  for (const auto& sym : exchange_info["symbols"]) {
    if (!sym.contains("symbol")) continue;
    std::string symbol = sym["symbol"].get<std::string>();
    SymbolFilters f;

    if (sym.contains("filters") && sym["filters"].is_array()) {
      for (const auto& flt : sym["filters"]) {
        std::string t = flt.value("filterType", "");
        if (t == "LOT_SIZE" || t == "MARKET_LOT_SIZE") {
          f.stepSize = _to_double_safe(flt, "stepSize", f.stepSize);
          f.minQty   = _to_double_safe(flt, "minQty",   f.minQty);
        } else if (t == "MIN_NOTIONAL") {
          f.minNotional = _to_double_safe(flt, "notional", f.minNotional);
          // parfois c'est "minNotional"
          if (f.minNotional == 0.0) f.minNotional = _to_double_safe(flt, "minNotional", f.minNotional);
        } else if (t == "NOTIONAL") {
          // futures peut exposer NOTIONAL filter
          double mn = _to_double_safe(flt, "minNotional", 0.0);
          if (mn > 0.0) f.minNotional = mn;
        }
      }
    }
    out[symbol] = f;
  }
  return out;
}

inline double round_down_to_step(double qty, double step) {
  if (step <= 0.0) return qty;
  double n = std::floor(qty / step);
  return n * step;
}

inline double clamp(double x, double lo, double hi) {
  if (x < lo) return lo;
  if (x > hi) return hi;
  return x;
}