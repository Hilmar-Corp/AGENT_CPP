#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include <iostream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "utils.hpp"
#include "hc_api.hpp"
#include "binance_um.hpp"
#include "kraken_futures.hpp"
#include "symbol_filters.hpp"
#include "execution.hpp"

struct Config {
  std::string base_url;
  std::string api_version{"v1"};
  std::string client_id;
  std::string hc_key;

  std::string mode{"trade"};
  int poll_seconds{10};
  bool dry_run{true};

  std::string exchange{"binance_futures_usdtm"};
  std::string instrument{"perp"};
  std::string quote_currency{"USDT"};

  int leverage{1};
  double cash_reserve_pct{0.0};

  std::string cex_key;
  std::string cex_secret;

  std::unordered_set<std::string> symbols_whitelist;
};

static Config load_config(const std::string& path) {
  auto raw = read_file(path);
  auto j = json::parse(raw);

  Config c;
  c.base_url = j.value("hilmarcorp_base_url", "");
  c.api_version = j.value("api_version", "v1");
  c.client_id = j.value("client_id", "");
  c.hc_key = j.value("hilmarcorp_api_key", "");

  c.mode = j.value("mode", "trade");
  c.poll_seconds = j.value("poll_seconds", 10);
  c.dry_run = j.value("dry_run", true);

  c.exchange = j.value("exchange", "binance_futures_usdtm");
  c.instrument = j.value("instrument", "perp");
  c.quote_currency = j.value("quote_currency", "USDT");

  c.leverage = j.value("leverage", 1);
  c.cash_reserve_pct = j.value("cash_reserve_pct", 0.0);

  if (j.contains("cex_key_local")) {
    c.cex_key = j["cex_key_local"].value("api_key", "");
    c.cex_secret = j["cex_key_local"].value("api_secret", "");
  }

  if (j.contains("risk_limits")) {
    auto rl = j["risk_limits"];
    if (rl.contains("symbols_whitelist")) {
      for (auto& x : rl["symbols_whitelist"]) c.symbols_whitelist.insert(x.get<std::string>());
    }
  }

  if (c.base_url.empty() || c.client_id.empty() || c.hc_key.empty()) {
    throw std::runtime_error("config missing base_url/client_id/hilmarcorp_api_key");
  }
  return c;
}

static std::string hc_url(const Config& c, const std::string& path) {
  std::string b = c.base_url;
  while (!b.empty() && b.back()=='/') b.pop_back();
  return b + "/" + c.api_version + "/clients/" + c.client_id + path;
}

static inline bool is_binance_um(const std::string& ex){
  return (ex == "binance" || ex == "binance_um" || ex == "binance_futures_usdtm" || ex.find("binance") != std::string::npos);
}
static inline bool is_kraken_futures(const std::string& ex){
  return (ex == "kraken" || ex == "kraken_futures" || ex.find("kraken") != std::string::npos);
}

int main(int argc, char** argv) {
  if (argc < 3 || std::string(argv[1]) != "--config") {
    std::cerr << "Usage: ./hilmarcorp-agent --config ./config.json\n";
    return 2;
  }

  curl_global_init(CURL_GLOBAL_DEFAULT);

  Config cfg;
  json cfg_json;
  try {
    auto raw = read_file(argv[2]);
    cfg_json = json::parse(raw);
    cfg = load_config(argv[2]);
  } catch (const std::exception& e) {
    std::cerr << "Config error: " << e.what() << "\n";
    return 1;
  }

  std::cout << "[agent] started mode=" << cfg.mode
            << " poll=" << cfg.poll_seconds
            << " dry_run=" << (cfg.dry_run ? "true":"false")
            << " exchange=" << cfg.exchange
            << " quote=" << cfg.quote_currency
            << " leverage=" << cfg.leverage
            << "\n";

  // heartbeat
  try {
    json hb = {
      {"ts", now_utc_iso_z()},
      {"version", "cpp-1.0.0"},
      {"mode", cfg.mode},
#if defined(__APPLE__)
      {"os", "macos"},
#elif defined(_WIN32)
      {"os", "windows"},
#else
      {"os", "linux"},
#endif
#if defined(__aarch64__) || defined(__arm64__)
      {"arch", "arm64"}
#else
      {"arch", "x64"}
#endif
    };
    auto r = hc::http_json("POST", hc_url(cfg, "/agent/heartbeat"), cfg.hc_key, &hb);
    (void)r;
    std::cout << "[agent] heartbeat ok\n";
  } catch (const std::exception& e) {
    std::cerr << "[agent] heartbeat failed: " << e.what() << "\n";
  }

  // Binance exchangeInfo filters cache
  std::unordered_map<std::string, SymbolFilters> filters_by_symbol;
  if(is_binance_um(cfg.exchange)){
    try {
      auto ex = binance_um::exchange_info();
      filters_by_symbol = parse_exchange_info_filters_um(ex);
      std::cout << "[agent] exchangeInfo loaded: " << filters_by_symbol.size() << " symbols\n";
    } catch (const std::exception& e) {
      std::cerr << "[agent] exchangeInfo failed: " << e.what() << "\n";
    }
  }

  // Kraken specs (from config)
  auto kr_specs = kraken_fut::load_kraken_specs_optional(cfg_json);

  std::string last_exp_id;

  while (true) {
    try {
      // 1) pull target exposure
      auto exp = hc::http_json("GET", hc_url(cfg, "/target_exposure"), cfg.hc_key, nullptr);

      if (exp.contains("ok") && exp["ok"].get<bool>() && exp.contains("item")) {
        auto item = exp["item"];
        std::string exp_id = item.value("id", "");
        json alloc = item.value("alloc", json::object());

        if (!exp_id.empty() && exp_id != last_exp_id) {
          last_exp_id = exp_id;
          std::cout << "[agent] new target ts=" << item.value("ts", "")
                    << " alloc=" << alloc.dump() << "\n";
        }

        // whitelist: if set, drop others
        if (!cfg.symbols_whitelist.empty() && alloc.is_object()){
          for (auto it2 = alloc.begin(); it2 != alloc.end(); ) {
            if (cfg.symbols_whitelist.count(it2.key()) == 0) it2 = alloc.erase(it2);
            else ++it2;
          }
        }

        // 2) EXECUTE
        if(cfg.mode == "trade"){
          if (!alloc.is_object() || alloc.empty()) {
            std::cout << "[agent] alloc empty -> skip execution\n";
          } else if (is_binance_um(cfg.exchange)) {

            if(cfg.cex_key.empty() || cfg.cex_secret.empty()){
              std::cout << "[agent] missing binance futures keys -> skip execution\n";
            } else {
              // Set leverage for symbols in alloc (best effort)
              for(auto it = alloc.begin(); it != alloc.end(); ++it){
                try { binance_um::set_leverage(cfg.cex_key, cfg.cex_secret, it.key(), cfg.leverage); }
                catch(...) {}
              }

              auto acct = binance_um::account(cfg.cex_key, cfg.cex_secret);
              auto posr = binance_um::position_risk(cfg.cex_key, cfg.cex_secret);

              double equity = account_equity_usdt_um(acct);
              auto cur_notional = positions_notional_um(posr);

              auto plan = build_plan_binance_um(cfg_json, alloc, equity, cur_notional, filters_by_symbol);
              if(plan.empty()) std::cout << "[exec/binance] no trades needed\n";
              else execute_plan_binance_um(cfg_json, plan);
            }

          } else if (is_kraken_futures(cfg.exchange)) {

            if(cfg.cex_key.empty() || cfg.cex_secret.empty()){
              std::cout << "[agent] missing kraken futures keys -> skip execution\n";
            } else {
              auto acct = kraken_fut::accounts(cfg_json, cfg.cex_key, cfg.cex_secret);
              auto pos  = kraken_fut::open_positions(cfg_json, cfg.cex_key, cfg.cex_secret);

              double equity = kraken_fut::kraken_equity_quote(acct, cfg.quote_currency);
              auto cur_notional = kraken_fut::positions_notional_kraken(pos, cfg_json, kr_specs);

              auto plan = build_plan_kraken_futures(cfg_json, alloc, equity, cur_notional, kr_specs);
              if(plan.empty()) std::cout << "[exec/kraken] no trades needed\n";
              else execute_plan_kraken_futures(cfg_json, plan);
            }

          } else {
            std::cout << "[agent] unknown exchange=" << cfg.exchange << " -> skip execution\n";
          }
        }

      } else {
        std::cout << "[agent] no target_exposure available\n";
      }

    } catch (const std::exception& e) {
      std::cerr << "[agent] loop error: " << e.what() << "\n";
    }

    std::this_thread::sleep_for(std::chrono::seconds(cfg.poll_seconds));
  }

  curl_global_cleanup();
  return 0;
}