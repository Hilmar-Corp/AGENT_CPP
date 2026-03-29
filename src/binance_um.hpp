#pragma once
#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include <string>
#include <map>
#include <stdexcept>
#include <sstream>
#include <vector>
#include <cstdlib>

#include <curl/curl.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/sha.h>

namespace binance_um {

// ------------------------------------------------------------
// Minimal HTTP + signing (USDT-M Futures: fapi)
// ------------------------------------------------------------
static inline std::string base_url() { return "https://fapi.binance.com"; }

static inline size_t write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
  ((std::string*)userp)->append((char*)contents, size * nmemb);
  return size * nmemb;
}

static inline std::string url_encode(CURL* curl, const std::string& s) {
  char* out = curl_easy_escape(curl, s.c_str(), (int)s.size());
  if(!out) return s;
  std::string r(out);
  curl_free(out);
  return r;
}

static inline std::string qs_from_kv(CURL* curl, const std::map<std::string,std::string>& kv) {
  std::string q;
  bool first=true;
  for (auto& it : kv) {
    if(!first) q += "&";
    first=false;
    q += url_encode(curl, it.first) + "=" + url_encode(curl, it.second);
  }
  return q;
}

static inline std::string hmac_sha256_hex(const std::string& key, const std::string& msg) {
  unsigned int len = EVP_MAX_MD_SIZE;
  unsigned char out[EVP_MAX_MD_SIZE];
  HMAC(EVP_sha256(),
       key.data(), (int)key.size(),
       (const unsigned char*)msg.data(), (int)msg.size(),
       out, &len);
  static const char* hex = "0123456789abcdef";
  std::string s; s.reserve(len*2);
  for(unsigned int i=0;i<len;i++){
    s.push_back(hex[(out[i]>>4)&0xF]);
    s.push_back(hex[out[i]&0xF]);
  }
  return s;
}

static inline json http_json(const std::string& method,
                             const std::string& url,
                             const std::vector<std::string>& headers,
                             const std::string& body = "") {
  CURL* curl = curl_easy_init();
  if(!curl) throw std::runtime_error("curl init failed");

  std::string resp;
  struct curl_slist* hs = nullptr;
  for (auto& h : headers) hs = curl_slist_append(hs, h.c_str());

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hs);

  if(method == "POST"){
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  } else if(method == "DELETE"){
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
  } else if(method == "GET"){
    // default
  } else {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
  }

  CURLcode rc = curl_easy_perform(curl);
  long code=0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

  curl_slist_free_all(hs);
  curl_easy_cleanup(curl);

  if(rc != CURLE_OK){
    throw std::runtime_error(std::string("curl error: ") + curl_easy_strerror(rc));
  }
  if(code < 200 || code >= 300){
    std::ostringstream oss;
    oss << "HTTP " << code << " body=" << resp;
    throw std::runtime_error(oss.str());
  }

  try { return json::parse(resp); }
  catch(...) { return json{{"raw", resp}}; }
}

static inline long long now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

static inline json public_get(const std::string& path,
                              const std::map<std::string,std::string>& params) {
  CURL* curl = curl_easy_init();
  if(!curl) throw std::runtime_error("curl init failed");
  std::string q = qs_from_kv(curl, params);
  curl_easy_cleanup(curl);

  std::string url = base_url() + path + (q.empty()? "" : ("?" + q));
  return http_json("GET", url, {});
}

static inline json signed_request(const std::string& method,
                                  const std::string& path,
                                  std::map<std::string,std::string> params,
                                  const std::string& api_key,
                                  const std::string& api_secret) {
  params["timestamp"] = std::to_string(now_ms());

  CURL* curl = curl_easy_init();
  if(!curl) throw std::runtime_error("curl init failed");
  std::string q = qs_from_kv(curl, params);
  curl_easy_cleanup(curl);

  std::string sig = hmac_sha256_hex(api_secret, q);
  std::string url = base_url() + path + "?" + q + "&signature=" + sig;

  std::vector<std::string> headers = {
    "X-MBX-APIKEY: " + api_key,
    "Content-Type: application/x-www-form-urlencoded"
  };

  // Binance signed endpoints often accept body as empty (params in query)
  return http_json(method, url, headers, "");
}

// ------------------------------------------------------------
// Public endpoints we need
// ------------------------------------------------------------
static inline double ticker_price(const std::string& symbol) {
  auto j = public_get("/fapi/v1/ticker/price", {{"symbol", symbol}});
  std::string p = j.value("price", "0");
  return std::stod(p);
}

// Mark price endpoint (premiumIndex -> markPrice)
static inline double mark_price(const std::string& symbol) {
  try {
    auto j = public_get("/fapi/v1/premiumIndex", {{"symbol", symbol}});
    std::string mp = j.value("markPrice", "0");
    double v = std::stod(mp);
    if(v > 0) return v;
  } catch(...) {}
  return ticker_price(symbol);
}

static inline json exchange_info() {
  return public_get("/fapi/v1/exchangeInfo", {});
}

// ------------------------------------------------------------
// Private endpoints we need
// ------------------------------------------------------------
static inline json account(const std::string& api_key, const std::string& api_secret) {
  return signed_request("GET", "/fapi/v2/account", {}, api_key, api_secret);
}

static inline json position_risk(const std::string& api_key, const std::string& api_secret) {
  return signed_request("GET", "/fapi/v2/positionRisk", {}, api_key, api_secret);
}

static inline void set_leverage(const std::string& api_key, const std::string& api_secret,
                                const std::string& symbol, int leverage) {
  signed_request("POST", "/fapi/v1/leverage",
                 {{"symbol", symbol}, {"leverage", std::to_string(leverage)}},
                 api_key, api_secret);
}

static inline void new_order_market(const std::string& api_key, const std::string& api_secret,
                                    const std::string& symbol, const std::string& side,
                                    double qty, bool reduce_only=false) {
  std::map<std::string,std::string> p{
    {"symbol", symbol},
    {"side", side},              // BUY / SELL
    {"type", "MARKET"},
    {"quantity", std::to_string(qty)},
    {"newOrderRespType", "RESULT"}
  };
  if(reduce_only) p["reduceOnly"] = "true";
  signed_request("POST", "/fapi/v1/order", p, api_key, api_secret);
}

// Convenience wrapper (matches what execution.hpp calls)
static inline void order_market(const std::string& symbol, const std::string& side, double qty,
                                const std::string& api_key, const std::string& api_secret) {
  new_order_market(api_key, api_secret, symbol, side, qty, false);
}

} // namespace binance_um