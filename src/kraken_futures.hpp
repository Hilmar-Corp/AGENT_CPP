#pragma once
#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include <string>
#include <stdexcept>
#include <sstream>
#include <vector>
#include <map>
#include <unordered_map>
#include <chrono>

#include <curl/curl.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>

namespace kraken_fut {

// --------------------
// Helpers HTTP
// --------------------
static inline size_t write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
  ((std::string*)userp)->append((char*)contents, size*nmemb);
  return size*nmemb;
}

static inline json http_json(const std::string& method,
                             const std::string& url,
                             const std::vector<std::string>& headers,
                             const std::string& body="") {
  CURL* curl = curl_easy_init();
  if(!curl) throw std::runtime_error("curl init failed");

  std::string resp;
  struct curl_slist* hs=nullptr;
  for (auto& h : headers) hs = curl_slist_append(hs, h.c_str());

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hs);

  if(method == "POST"){
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  } else if(method != "GET"){
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

// --------------------
// Base URL
// --------------------
static inline std::string default_base_url() {
  // Kraken Derivatives API base (commonly)
  return "https://futures.kraken.com/derivatives";
}
static inline std::string base_url_from_cfg(const json& cfg) {
  try{
    if(cfg.contains("kraken_futures") && cfg["kraken_futures"].contains("base_url")){
      return cfg["kraken_futures"]["base_url"].get<std::string>();
    }
  }catch(...) {}
  return default_base_url();
}

// --------------------
// Signing (simple HMAC SHA256 HEX for demo-style auth)
// NOTE: Kraken Futures auth scheme may differ depending on endpoint.
// Here we keep it consistent with what you already started: API key + signature.
// If you later want the exact production auth scheme, we’ll harden it.
// --------------------
static inline std::string hmac_sha256_hex(const std::string& key, const std::string& msg) {
  unsigned int len = EVP_MAX_MD_SIZE;
  unsigned char out[EVP_MAX_MD_SIZE];
  HMAC(EVP_sha256(),
       key.data(), (int)key.size(),
       (const unsigned char*)msg.data(), (int)msg.size(),
       out, &len);

  static const char* hex="0123456789abcdef";
  std::string s; s.reserve(len*2);
  for(unsigned int i=0;i<len;i++){
    s.push_back(hex[(out[i]>>4)&0xF]);
    s.push_back(hex[out[i]&0xF]);
  }
  return s;
}

// --------------------
// Contract specs (from config)
// --------------------
struct SymbolSpec {
  double contract_size{1.0}; // notional per 1 contract
  double lot_step{1.0};      // contract step
  double min_size{1.0};      // min contracts
};

static inline std::unordered_map<std::string, SymbolSpec> load_kraken_specs_optional(const json& cfg) {
  std::unordered_map<std::string, SymbolSpec> out;
  try{
    if(cfg.contains("kraken_futures") && cfg["kraken_futures"].contains("symbols")){
      auto s = cfg["kraken_futures"]["symbols"];
      if(s.is_object()){
        for(auto it = s.begin(); it != s.end(); ++it){
          SymbolSpec sp;
          if(it.value().is_object()){
            sp.contract_size = it.value().value("contract_size", 1.0);
            sp.lot_step      = it.value().value("lot_step", 1.0);
            sp.min_size      = it.value().value("min_size", 1.0);
          }
          out[it.key()] = sp;
        }
      }
    }
  }catch(...) {}
  return out;
}

// --------------------
// Public data
// --------------------
static inline double mark_price_for(const json& cfg, const std::string& symbol) {
  std::string base = base_url_from_cfg(cfg);
  // One common public endpoint style:
  // GET /api/v3/tickers?symbol=PF_XBTUSD
  // But formats vary; we try a few shapes robustly.
  try{
    auto j = http_json("GET", base + "/api/v3/tickers?symbol=" + symbol, {});
    // possible: {"tickers":[{"symbol":"PF_XBTUSD","markPrice":"..."}]}
    if(j.contains("tickers") && j["tickers"].is_array() && !j["tickers"].empty()){
      auto t = j["tickers"][0];
      std::string mp = t.value("markPrice", t.value("mark_price", t.value("last", "0")));
      return std::stod(mp);
    }
    // possible: direct object
    if(j.contains("markPrice")) return std::stod(j["markPrice"].get<std::string>());
  }catch(...) {}

  // fallback: if later you add another endpoint, we can add it here
  throw std::runtime_error("kraken mark_price unavailable for " + symbol);
}

// --------------------
// Private endpoints (minimal)
// --------------------
static inline json accounts(const json& cfg, const std::string& api_key, const std::string& api_secret) {
  std::string base = base_url_from_cfg(cfg);
  // You likely already have a real auth path in your file; here we provide a minimal stub pattern.
  // GET /api/v3/accounts
  std::string path = "/api/v3/accounts";
  std::string nonce = std::to_string(now_ms());
  std::string sig = hmac_sha256_hex(api_secret, nonce + "GET" + path);

  std::vector<std::string> headers = {
    "APIKey: " + api_key,
    "Nonce: " + nonce,
    "Authent: " + sig
  };
  return http_json("GET", base + path, headers);
}

static inline json open_positions(const json& cfg, const std::string& api_key, const std::string& api_secret) {
  std::string base = base_url_from_cfg(cfg);
  std::string path = "/api/v3/openpositions";
  std::string nonce = std::to_string(now_ms());
  std::string sig = hmac_sha256_hex(api_secret, nonce + "GET" + path);

  std::vector<std::string> headers = {
    "APIKey: " + api_key,
    "Nonce: " + nonce,
    "Authent: " + sig
  };
  return http_json("GET", base + path, headers);
}

static inline void send_order_market(const json& cfg,
                                     const std::string& api_key, const std::string& api_secret,
                                     const std::string& symbol,
                                     const std::string& side, // BUY/SELL
                                     double size_contracts) {
  std::string base = base_url_from_cfg(cfg);
  std::string path = "/api/v3/sendorder";

  json bodyj = {
    {"orderType", "mkt"},
    {"symbol", symbol},
    {"side", (side=="BUY" ? "buy" : "sell")},
    {"size", size_contracts}
  };
  std::string body = bodyj.dump();

  std::string nonce = std::to_string(now_ms());
  std::string sig = hmac_sha256_hex(api_secret, nonce + "POST" + path + body);

  std::vector<std::string> headers = {
    "Content-Type: application/json",
    "APIKey: " + api_key,
    "Nonce: " + nonce,
    "Authent: " + sig
  };

  (void)http_json("POST", base + path, headers, body);
}

// --------------------
// Portfolio helpers used by main/execution
// --------------------
static inline double kraken_equity_quote(const json& acct, const std::string& quote) {
  // Try many shapes:
  // 1) {"accounts":[{"currency":"USD","equity":"123.4"}]}
  if(acct.contains("accounts") && acct["accounts"].is_array()){
    for(auto& a : acct["accounts"]){
      std::string c = a.value("currency", a.value("asset", ""));
      if(c == quote){
        std::string e = a.value("equity", a.value("balance", a.value("available", "0")));
        return std::stod(e);
      }
    }
  }
  // 2) direct fields (best-effort)
  if(acct.contains("equity")) {
    if(acct["equity"].is_string()) return std::stod(acct["equity"].get<std::string>());
    if(acct["equity"].is_number()) return acct["equity"].get<double>();
  }
  throw std::runtime_error("kraken equity quote not found for quote=" + quote);
}

static inline std::unordered_map<std::string,double>
positions_notional_kraken(const json& pos,
                          const json& cfg,
                          const std::unordered_map<std::string, SymbolSpec>& specs) {
  std::unordered_map<std::string,double> out;

  auto handle_one = [&](const json& p){
    std::string sym = p.value("symbol", "");
    if(sym.empty()) return;

    double size = 0.0;
    // common names: "size", "quantity"
    if(p.contains("size")){
      if(p["size"].is_string()) size = std::stod(p["size"].get<std::string>());
      else if(p["size"].is_number()) size = p["size"].get<double>();
    } else if(p.contains("quantity")){
      if(p["quantity"].is_string()) size = std::stod(p["quantity"].get<std::string>());
      else if(p["quantity"].is_number()) size = p["quantity"].get<double>();
    }
    if(size == 0.0) return;

    double px = 0.0;
    if(p.contains("markPrice")){
      if(p["markPrice"].is_string()) px = std::stod(p["markPrice"].get<std::string>());
      else if(p["markPrice"].is_number()) px = p["markPrice"].get<double>();
    } else if(p.contains("mark_price")){
      if(p["mark_price"].is_string()) px = std::stod(p["mark_price"].get<std::string>());
      else if(p["mark_price"].is_number()) px = p["mark_price"].get<double>();
    } else {
      // fallback public mark price
      px = mark_price_for(cfg, sym);
    }

    auto it = specs.find(sym);
    double cs = (it==specs.end()? 1.0 : it->second.contract_size);
    out[sym] = size * px * cs; // signed notional
  };

  if(pos.is_array()){
    for(auto& p : pos) handle_one(p);
    return out;
  }
  if(pos.contains("openPositions") && pos["openPositions"].is_array()){
    for(auto& p : pos["openPositions"]) handle_one(p);
    return out;
  }
  // sometimes "open_positions"
  if(pos.contains("open_positions") && pos["open_positions"].is_array()){
    for(auto& p : pos["open_positions"]) handle_one(p);
    return out;
  }

  return out;
}

} // namespace kraken_fut