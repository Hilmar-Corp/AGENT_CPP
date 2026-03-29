#pragma once
#include <string>
#include <sstream>
#include <stdexcept>
#include <cmath>
#include <ctime>

#include <curl/curl.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

namespace bnf {

static inline std::string base_url() { return "https://fapi.binance.com"; }

static inline std::string hex(const unsigned char* data, unsigned int len) {
  static const char* digits = "0123456789abcdef";
  std::string out; out.reserve(len*2);
  for (unsigned int i=0;i<len;i++){
    out.push_back(digits[(data[i]>>4)&0xF]);
    out.push_back(digits[data[i]&0xF]);
  }
  return out;
}

static inline std::string hmac_sha256_hex(const std::string& key, const std::string& msg) {
  unsigned int len = 0;
  unsigned char mac[EVP_MAX_MD_SIZE];
  HMAC(EVP_sha256(),
       reinterpret_cast<const unsigned char*>(key.data()), (int)key.size(),
       reinterpret_cast<const unsigned char*>(msg.data()), (int)msg.size(),
       mac, &len);
  return hex(mac, len);
}

static inline size_t curl_write(void* contents, size_t size, size_t nmemb, void* userp) {
  size_t total = size * nmemb;
  std::string* s = reinterpret_cast<std::string*>(userp);
  s->append(reinterpret_cast<char*>(contents), total);
  return total;
}

static inline std::string http_raw(
    const std::string& method,
    const std::string& url,
    const std::string& api_key,
    const std::string& body = ""
) {
  CURL* curl = curl_easy_init();
  if (!curl) throw std::runtime_error("curl init failed");

  std::string resp;
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
  if (!api_key.empty()) {
    headers = curl_slist_append(headers, ("X-MBX-APIKEY: " + api_key).c_str());
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);

  if (method == "POST") {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  } else if (method == "GET") {
    // default
  } else {
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    throw std::runtime_error("unsupported method");
  }

  CURLcode res = curl_easy_perform(curl);
  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_cleanup(curl);
  curl_slist_free_all(headers);

  if (res != CURLE_OK) {
    throw std::runtime_error(std::string("curl error: ") + curl_easy_strerror(res));
  }
  if (code >= 400) {
    throw std::runtime_error("http " + std::to_string(code) + " body=" + resp);
  }
  return resp;
}

static inline long long now_ms() {
  return (long long) (std::time(nullptr) * 1000LL);
}

// Build signed query: query + "&timestamp=...&signature=..."
static inline std::string signed_query(
    const std::string& query,
    const std::string& api_secret
) {
  std::string q = query;
  if (!q.empty()) q += "&";
  q += "timestamp=" + std::to_string(now_ms());
  std::string sig = hmac_sha256_hex(api_secret, q);
  q += "&signature=" + sig;
  return q;
}

static inline json signed_get(const std::string& path, const std::string& api_key, const std::string& api_secret, const std::string& query="") {
  std::string q = signed_query(query, api_secret);
  std::string url = base_url() + path + "?" + q;
  auto resp = http_raw("GET", url, api_key);
  return resp.empty() ? json::object() : json::parse(resp);
}

static inline json signed_post(const std::string& path, const std::string& api_key, const std::string& api_secret, const std::string& form_query) {
  std::string q = signed_query(form_query, api_secret);
  std::string url = base_url() + path;
  auto resp = http_raw("POST", url, api_key, q);
  return resp.empty() ? json::object() : json::parse(resp);
}

static inline json public_get(const std::string& path, const std::string& query="") {
  std::string url = base_url() + path;
  if (!query.empty()) url += "?" + query;
  auto resp = http_raw("GET", url, "");
  return resp.empty() ? json::object() : json::parse(resp);
}

// -------- Helpers we need --------

static inline double mark_price(const std::string& symbol) {
  // premiumIndex includes markPrice
  auto j = public_get("/fapi/v1/premiumIndex", "symbol=" + symbol);
  return std::stod(j.value("markPrice", "0"));
}

static inline json account(const std::string& api_key, const std::string& api_secret) {
  return signed_get("/fapi/v2/account", api_key, api_secret);
}

static inline double wallet_balance_usdt(const json& acct) {
  // Prefer totalWalletBalance (USDT) from /fapi/v2/account
  // It’s a string.
  if (acct.contains("totalWalletBalance")) {
    return std::stod(acct.value("totalWalletBalance", "0"));
  }
  return 0.0;
}

static inline double position_amt(const json& acct, const std::string& symbol) {
  // acct["positions"] contains positionAmt strings
  if (!acct.contains("positions")) return 0.0;
  for (const auto& p : acct["positions"]) {
    if (p.value("symbol","") == symbol) {
      return std::stod(p.value("positionAmt", "0"));
    }
  }
  return 0.0;
}

static inline void set_leverage(const std::string& api_key, const std::string& api_secret, const std::string& symbol, int leverage) {
  if (leverage <= 0) return;
  std::ostringstream q;
  q << "symbol=" << symbol << "&leverage=" << leverage;
  (void) signed_post("/fapi/v1/leverage", api_key, api_secret, q.str());
}

static inline json place_market_order(
    const std::string& api_key,
    const std::string& api_secret,
    const std::string& symbol,
    const std::string& side,     // BUY / SELL
    double qty
) {
  // MARKET order
  std::ostringstream q;
  q.setf(std::ios::fixed); q.precision(8);
  q << "symbol=" << symbol
    << "&side=" << side
    << "&type=MARKET"
    << "&quantity=" << qty;
  return signed_post("/fapi/v1/order", api_key, api_secret, q.str());
}

static inline double floor_to_step(double x, double step) {
  if (step <= 0) return x;
  return std::floor(x / step) * step;
}

static inline double lot_step_size(const std::string& symbol) {
  // Minimal filter: LOT_SIZE -> stepSize
  auto info = public_get("/fapi/v1/exchangeInfo");
  if (!info.contains("symbols")) return 0.0;

  for (auto& s : info["symbols"]) {
    if (s.value("symbol","") != symbol) continue;
    if (!s.contains("filters")) continue;
    for (auto& f : s["filters"]) {
      if (f.value("filterType","") == "LOT_SIZE") {
        return std::stod(f.value("stepSize","0"));
      }
    }
  }
  return 0.0;
}

} // namespace bnf