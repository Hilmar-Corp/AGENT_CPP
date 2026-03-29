#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <stdexcept>
#include <map>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <unordered_map>
#include <mutex>

#include <curl/curl.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>

#include "hc_api.hpp"

namespace binance {

using json = nlohmann::json;

inline std::string hmac_sha256_hex(const std::string& key, const std::string& msg) {
  unsigned int len = 0;
  unsigned char out[EVP_MAX_MD_SIZE];
  HMAC(EVP_sha256(),
       key.data(), (int)key.size(),
       (const unsigned char*)msg.data(), msg.size(),
       out, &len);

  static const char* hex = "0123456789abcdef";
  std::string s;
  s.reserve(len * 2);
  for (unsigned int i = 0; i < len; i++) {
    s.push_back(hex[(out[i] >> 4) & 0xF]);
    s.push_back(hex[out[i] & 0xF]);
  }
  return s;
}

inline std::string url_encode(CURL* curl, const std::string& s) {
  char* out = curl_easy_escape(curl, s.c_str(), (int)s.size());
  if (!out) return "";
  std::string r(out);
  curl_free(out);
  return r;
}

inline json signed_get(const std::string& base,
                       const std::string& path,
                       const std::string& api_key,
                       const std::string& api_secret,
                       const std::string& query_no_sig) {
  CURL* curl = curl_easy_init();
  if (!curl) throw std::runtime_error("curl init failed");

  std::string qs = query_no_sig;
  std::string sig = hmac_sha256_hex(api_secret, qs);
  qs += "&signature=" + sig;

  std::string url = base + path + "?" + qs;

  std::string resp;
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, ("X-MBX-APIKEY: " + api_key).c_str());
  headers = curl_slist_append(headers, "User-Agent: HilmarCorp-Agent/1.0");

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, hc::write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

  CURLcode res = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) throw std::runtime_error(std::string("curl: ") + curl_easy_strerror(res));
  if (status >= 400) throw std::runtime_error("binance http " + std::to_string(status) + " body=" + resp);
  if (resp.empty()) return json::object();
  return json::parse(resp);
}

inline json public_get(const std::string& base, const std::string& path, const std::string& query="") {
  std::string url = base + path + (query.empty() ? "" : ("?" + query));
  return hc::http_json("GET", url, /*api_key=*/""); // hack: not used here
}

// ---- API helpers ----
inline json account_info(const std::string& api_key, const std::string& api_secret) {
  const std::string base = "https://api.binance.com";
  long long ts = (long long)(std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count());
  std::string qs = "timestamp=" + std::to_string(ts) + "&recvWindow=5000";
  return signed_get(base, "/api/v3/account", api_key, api_secret, qs);
}

inline double price_symbol(const std::string& symbol) {
  const std::string base = "https://api.binance.com";
  // /api/v3/ticker/price?symbol=BTCUSDT
  auto j = hc::http_json("GET", base + "/api/v3/ticker/price?symbol=" + symbol, /*api_key*/"", nullptr, 10);
  if (!j.contains("price")) return 0.0;
  return std::stod(j["price"].get<std::string>());
}

// ---- Symbol filters and caching ----
struct SymbolFilters {
  double step_size{0.0};
  double min_qty{0.0};
  double min_notional{0.0};
  double tick_size{0.0};
};

inline SymbolFilters parse_filters_from_exchange_info(const json& info) {
  SymbolFilters f;
  if (!info.contains("symbols") || !info["symbols"].is_array() || info["symbols"].empty()) return f;
  const auto& s = info["symbols"][0];
  if (!s.contains("filters") || !s["filters"].is_array()) return f;

  for (const auto& flt : s["filters"]) {
    const std::string t = flt.value("filterType", "");
    if (t == "LOT_SIZE") {
      // stepSize, minQty
      try {
        f.step_size = std::stod(flt.value("stepSize", "0"));
        f.min_qty = std::stod(flt.value("minQty", "0"));
      } catch (...) {}
    } else if (t == "MIN_NOTIONAL") {
      // minNotional
      try {
        f.min_notional = std::stod(flt.value("minNotional", "0"));
      } catch (...) {}
    } else if (t == "PRICE_FILTER") {
      try {
        f.tick_size = std::stod(flt.value("tickSize", "0"));
      } catch (...) {}
    }
  }
  return f;
}

inline json exchange_info_symbol(const std::string& symbol) {
  const std::string base = "https://api.binance.com";
  return hc::http_json("GET", base + "/api/v3/exchangeInfo?symbol=" + symbol, /*api_key*/"", nullptr, 10);
}

// ---- Futures (USDT-M Perpetual) ----
inline json futures_exchange_info_symbol(const std::string& symbol) {
  const std::string base = "https://fapi.binance.com";
  return hc::http_json("GET", base + "/fapi/v1/exchangeInfo?symbol=" + symbol, /*api_key*/"", nullptr, 10);
}

inline const SymbolFilters& futures_filters_for_symbol(const std::string& symbol) {
  static std::unordered_map<std::string, SymbolFilters> cache;
  static std::mutex m;
  std::lock_guard<std::mutex> lock(m);
  auto it = cache.find(symbol);
  if (it != cache.end()) return it->second;
  SymbolFilters f = parse_filters_from_exchange_info(futures_exchange_info_symbol(symbol));
  auto ins = cache.emplace(symbol, f);
  return ins.first->second;
}

inline double ceil_to_step(double qty, double step) {
  if (step <= 0.0) return qty;
  double k = std::ceil(qty / step);
  return k * step;
}

inline double round_to_tick(double price, double tick) {
  if (tick <= 0.0) return price;
  double k = std::floor(price / tick + 1e-12);
  return k * tick;
}

inline json futures_signed_get(const std::string& path,
                              const std::string& api_key,
                              const std::string& api_secret,
                              const std::string& query_no_sig) {
  const std::string base = "https://fapi.binance.com";
  return signed_get(base, path, api_key, api_secret, query_no_sig);
}

inline json futures_signed_post(const std::string& path,
                               const std::string& api_key,
                               const std::string& api_secret,
                               const std::string& query_no_sig) {
  const std::string base = "https://fapi.binance.com";
  return signed_post(base, path, api_key, api_secret, query_no_sig);
}

inline json futures_account(const std::string& api_key, const std::string& api_secret) {
  long long ts = now_ms();
  std::string qs = "timestamp=" + std::to_string(ts) + "&recvWindow=5000";
  return futures_signed_get("/fapi/v2/account", api_key, api_secret, qs);
}

inline json futures_position_risk(const std::string& api_key, const std::string& api_secret) {
  long long ts = now_ms();
  std::string qs = "timestamp=" + std::to_string(ts) + "&recvWindow=5000";
  return futures_signed_get("/fapi/v2/positionRisk", api_key, api_secret, qs);
}

inline double futures_mark_price(const std::string& symbol) {
  const std::string base = "https://fapi.binance.com";
  auto j = hc::http_json("GET", base + "/fapi/v1/premiumIndex?symbol=" + symbol, /*api_key*/"", nullptr, 10);
  if (j.contains("markPrice")) {
    // sometimes numeric as string
    if (j["markPrice"].is_string()) return std::stod(j["markPrice"].get<std::string>());
    if (j["markPrice"].is_number()) return j["markPrice"].get<double>();
  }
  return 0.0;
}

inline json futures_set_leverage(const std::string& api_key,
                                const std::string& api_secret,
                                const std::string& symbol,
                                int leverage) {
  // POST /fapi/v1/leverage
  std::string qs = "symbol=" + symbol + "&leverage=" + std::to_string(leverage);
  return futures_signed_post("/fapi/v1/leverage", api_key, api_secret, qs);
}

inline json futures_market_order(const std::string& api_key,
                                const std::string& api_secret,
                                const std::string& symbol,
                                const std::string& side,
                                double qty_abs) {
  // USDT-M futures uses quantity in base units
  const auto& f = futures_filters_for_symbol(symbol);
  double qty = qty_abs;
  if (f.step_size > 0.0) qty = floor_to_step(qty, f.step_size);

  if (f.min_qty > 0.0 && qty + 1e-12 < f.min_qty) {
    throw std::runtime_error("futures: qty below minQty for " + symbol);
  }

  double px = futures_mark_price(symbol);
  double notional = (px > 0.0) ? (qty * px) : 0.0;
  if (f.min_notional > 0.0 && notional + 1e-9 < f.min_notional) {
    throw std::runtime_error("futures: notional below MIN_NOTIONAL for " + symbol);
  }

  std::string qs = "symbol=" + symbol + "&side=" + side + "&type=MARKET&quantity=" + fmt_double(qty);
  return futures_signed_post("/fapi/v1/order", api_key, api_secret, qs);
}

// ---- Symbol filters and caching ----
inline const SymbolFilters& filters_for_symbol(const std::string& symbol) {
  static std::unordered_map<std::string, SymbolFilters> cache;
  static std::mutex m;
  std::lock_guard<std::mutex> lock(m);
  auto it = cache.find(symbol);
  if (it != cache.end()) return it->second;
  SymbolFilters f = parse_filters_from_exchange_info(exchange_info_symbol(symbol));
  auto ins = cache.emplace(symbol, f);
  return ins.first->second;
}

inline double floor_to_step(double qty, double step) {
  if (step <= 0.0) return qty;
  double k = std::floor(qty / step);
  return k * step;
}

inline bool validate_qty_notional(const std::string& symbol, double qty, double notional) {
  const auto& f = filters_for_symbol(symbol);
  if (f.min_qty > 0.0 && qty + 1e-12 < f.min_qty) return false;
  if (f.min_notional > 0.0 && notional + 1e-9 < f.min_notional) return false;
  return true;
}

// ---- Trading (SPOT) ----

inline long long now_ms() {
  return (long long)(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}

inline std::string fmt_double(double x, int precision = 8) {
  std::ostringstream oss;
  oss.setf(std::ios::fixed);
  oss << std::setprecision(precision) << x;
  std::string s = oss.str();
  // trim trailing zeros
  if (s.find('.') != std::string::npos) {
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
  }
  if (s.empty()) s = "0";
  return s;
}

inline json signed_post(const std::string& base,
                        const std::string& path,
                        const std::string& api_key,
                        const std::string& api_secret,
                        const std::string& query_no_sig) {
  CURL* curl = curl_easy_init();
  if (!curl) throw std::runtime_error("curl init failed");

  std::string qs = query_no_sig;
  if (!qs.empty() && qs.back() != '&') qs += "&";
  qs += "timestamp=" + std::to_string(now_ms()) + "&recvWindow=5000";

  std::string sig = hmac_sha256_hex(api_secret, qs);
  qs += "&signature=" + sig;

  std::string url = base + path + "?" + qs;

  std::string resp;
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, ("X-MBX-APIKEY: " + api_key).c_str());
  headers = curl_slist_append(headers, "User-Agent: HilmarCorp-Agent/1.0");

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, hc::write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

  CURLcode res = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) throw std::runtime_error(std::string("curl: ") + curl_easy_strerror(res));
  if (status >= 400) throw std::runtime_error("binance http " + std::to_string(status) + " body=" + resp);
  if (resp.empty()) return json::object();
  return json::parse(resp);
}

inline json market_buy_quote_qty(const std::string& api_key,
                                 const std::string& api_secret,
                                 const std::string& symbol,
                                 double quote_qty_usdt) {
  const std::string base = "https://api.binance.com";
  // Respect MIN_NOTIONAL if present
  if (!validate_qty_notional(symbol, /*qty*/0.0, quote_qty_usdt)) {
    throw std::runtime_error("binance: notional below MIN_NOTIONAL for " + symbol);
  }
  // BUY using quoteOrderQty (in USDT) avoids qty precision issues
  std::string qs = "symbol=" + symbol + "&side=BUY&type=MARKET&quoteOrderQty=" + fmt_double(quote_qty_usdt);
  return signed_post(base, "/api/v3/order", api_key, api_secret, qs);
}

inline json market_sell_base_qty(const std::string& api_key,
                                 const std::string& api_secret,
                                 const std::string& symbol,
                                 double base_qty) {
  const std::string base = "https://api.binance.com";
  const auto& f = filters_for_symbol(symbol);
  double qty = base_qty;
  if (f.step_size > 0.0) qty = floor_to_step(qty, f.step_size);

  // Validate minQty/minNotional using current price
  double px = price_symbol(symbol);
  double notional = (px > 0.0) ? (qty * px) : 0.0;
  if (!validate_qty_notional(symbol, qty, notional)) {
    throw std::runtime_error("binance: qty/notional below filters for " + symbol);
  }

  std::string qs = "symbol=" + symbol + "&side=SELL&type=MARKET&quantity=" + fmt_double(qty);
  return signed_post(base, "/api/v3/order", api_key, api_secret, qs);
}

} // namespace binance