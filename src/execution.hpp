#pragma once
#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include <string>
#include <vector>
#include <unordered_map>
#include <iostream>
#include <cmath>
#include <stdexcept>

#include "symbol_filters.hpp"
#include "binance_um.hpp"
#include "kraken_futures.hpp"

// --------------------
// Order model
// --------------------
struct ExecOrder {
  std::string symbol;
  std::string side;   // BUY / SELL
  double qty{0.0};    // Binance: base qty; Kraken: contracts
  double notional{0.0};
};

using ExecPlan = std::vector<ExecOrder>;

// --------------------
// Binance helpers
// --------------------
static inline double account_equity_usdt_um(const json& acct) {
  // Best effort: futures account often has totalMarginBalance / totalWalletBalance
  auto pick = [&](const char* k)->double{
    if(!acct.contains(k)) return 0.0;
    if(acct[k].is_string()) return std::stod(acct[k].get<std::string>());
    if(acct[k].is_number()) return acct[k].get<double>();
    return 0.0;
  };
  double v = pick("totalMarginBalance");
  if(v<=0) v = pick("totalWalletBalance");
  if(v<=0) v = pick("availableBalance");
  return v;
}

static inline std::unordered_map<std::string,double> positions_notional_um(const json& posr) {
  std::unordered_map<std::string,double> out;
  if(!posr.is_array()) return out;
  for(auto& p : posr){
    std::string sym = p.value("symbol", "");
    if(sym.empty()) continue;
    std::string amtS = p.value("positionAmt", "0");
    double amt = std::stod(amtS);
    if(amt == 0.0) continue;

    double mp = 0.0;
    if(p.contains("markPrice")){
      if(p["markPrice"].is_string()) mp = std::stod(p["markPrice"].get<std::string>());
      else if(p["markPrice"].is_number()) mp = p["markPrice"].get<double>();
    }
    if(mp <= 0) mp = binance_um::mark_price(sym);
    out[sym] = amt * mp; // signed USDT notional
  }
  return out;
}

// --------------------
// Build plan (generic idea: alloc -> target notionals -> delta -> market orders)
// --------------------
static inline ExecPlan build_plan_binance_um(
  const json& cfg,
  const json& alloc,
  double equity_usdt,
  const std::unordered_map<std::string,double>& cur_notional,
  const std::unordered_map<std::string, SymbolFilters>& filters_by_symbol
){
  ExecPlan plan;

  bool dry_run = cfg.value("dry_run", true);

  double max_abs_exposure = 1.0;
  double max_order_notional = 0.0;
  double min_trade_notional = 0.0;
  double cash_reserve_pct = cfg.value("cash_reserve_pct", 0.0);

  if(cfg.contains("risk_limits")){
    auto rl = cfg["risk_limits"];
    max_abs_exposure   = rl.value("max_abs_exposure", 1.0);
    max_order_notional = rl.value("max_order_notional", 0.0);
    min_trade_notional = rl.value("min_trade_notional", 0.0);
  }

  double investable = equity_usdt * (1.0 - clamp(cash_reserve_pct, 0.0, 0.95));

  for(auto it = alloc.begin(); it != alloc.end(); ++it){
    const std::string sym = it.key();
    double w = 0.0;
    try{ w = it.value().get<double>(); } catch(...) { continue; }

    w = clamp(w, -max_abs_exposure, +max_abs_exposure);

    double target = w * investable;
    double cur = 0.0;
    auto itc = cur_notional.find(sym);
    if(itc != cur_notional.end()) cur = itc->second;

    double delta = target - cur;
    if(std::fabs(delta) < min_trade_notional) continue;

    if(max_order_notional > 0.0){
      delta = clamp(delta, -max_order_notional, +max_order_notional);
      if(std::fabs(delta) < min_trade_notional) continue;
    }

    // price
    double px = 0.0;
    try { px = binance_um::mark_price(sym); }
    catch(...) { continue; }
    if(px <= 0) continue;

    // qty
    double qty = std::fabs(delta) / px;

    // filters
    auto sf = filters_by_symbol.find(sym);
    if(sf != filters_by_symbol.end()){
      if(sf->second.stepSize > 0) qty = round_down_to_step(qty, sf->second.stepSize);
      if(sf->second.minQty > 0 && qty < sf->second.minQty) continue;
    }

    if(qty <= 0) continue;

    ExecOrder o;
    o.symbol = sym;
    o.side = (delta > 0 ? "BUY" : "SELL");
    o.qty = qty;
    o.notional = qty * px;

    plan.push_back(o);

    if(dry_run){
      std::cout << "[exec/binance] plan " << o.symbol
                << " " << o.side << " qty=" << o.qty
                << " ~notional=" << o.notional << "\n";
    }
  }

  return plan;
}

static inline void execute_plan_binance_um(const json& cfg, const ExecPlan& plan){
  bool dry_run = cfg.value("dry_run", true);
  std::string api_key, api_secret;
  if(cfg.contains("cex_key_local")){
    api_key = cfg["cex_key_local"].value("api_key", "");
    api_secret = cfg["cex_key_local"].value("api_secret", "");
  }
  if(api_key.empty() || api_secret.empty()){
    std::cout << "[exec/binance] missing keys -> skip\n";
    return;
  }

  for(const auto& o : plan){
    if(dry_run){
      std::cout << "[exec/binance] DRY_RUN " << o.symbol << " " << o.side << " qty=" << o.qty << "\n";
      continue;
    }
    binance_um::order_market(o.symbol, o.side, o.qty, api_key, api_secret);
    std::cout << "[exec/binance] sent " << o.symbol << " " << o.side << " qty=" << o.qty << "\n";
  }
}

// --------------------
// Kraken Futures plan/execution
// --------------------
static inline ExecPlan build_plan_kraken_futures(
  const json& cfg,
  const json& alloc,
  double equity_quote,
  const std::unordered_map<std::string,double>& cur_notional,
  const std::unordered_map<std::string, kraken_fut::SymbolSpec>& specs
){
  ExecPlan plan;

  bool dry_run = cfg.value("dry_run", true);

  double max_abs_exposure = 1.0;
  double max_order_notional = 0.0;
  double min_trade_notional = 0.0;
  double cash_reserve_pct = cfg.value("cash_reserve_pct", 0.0);

  if(cfg.contains("risk_limits")){
    auto rl = cfg["risk_limits"];
    max_abs_exposure   = rl.value("max_abs_exposure", 1.0);
    max_order_notional = rl.value("max_order_notional", 0.0);
    min_trade_notional = rl.value("min_trade_notional", 0.0);
  }

  double investable = equity_quote * (1.0 - clamp(cash_reserve_pct, 0.0, 0.95));

  for(auto it = alloc.begin(); it != alloc.end(); ++it){
    const std::string sym = it.key();
    double w = 0.0;
    try{ w = it.value().get<double>(); } catch(...) { continue; }

    w = clamp(w, -max_abs_exposure, +max_abs_exposure);

    double target = w * investable;

    double cur = 0.0;
    auto itc = cur_notional.find(sym);
    if(itc != cur_notional.end()) cur = itc->second;

    double delta = target - cur;
    if(std::fabs(delta) < min_trade_notional) continue;

    if(max_order_notional > 0.0){
      delta = clamp(delta, -max_order_notional, +max_order_notional);
      if(std::fabs(delta) < min_trade_notional) continue;
    }

    // price
    double px = 0.0;
    try { px = kraken_fut::mark_price_for(cfg, sym); }
    catch(...) { continue; }
    if(px <= 0) continue;

    // spec
    auto spIt = specs.find(sym);
    kraken_fut::SymbolSpec sp;
    if(spIt != specs.end()) sp = spIt->second;

    // contracts qty
    double contracts = std::fabs(delta) / (px * (sp.contract_size > 0 ? sp.contract_size : 1.0));

    if(sp.lot_step > 0) contracts = round_down_to_step(contracts, sp.lot_step);
    if(sp.min_size > 0 && contracts < sp.min_size) continue;

    if(contracts <= 0) continue;

    ExecOrder o;
    o.symbol = sym;
    o.side = (delta > 0 ? "BUY" : "SELL");
    o.qty = contracts;
    o.notional = contracts * px * (sp.contract_size > 0 ? sp.contract_size : 1.0);

    plan.push_back(o);

    if(dry_run){
      std::cout << "[exec/kraken] plan " << o.symbol
                << " " << o.side << " contracts=" << o.qty
                << " ~notional=" << o.notional << "\n";
    }
  }

  return plan;
}

static inline void execute_plan_kraken_futures(const json& cfg, const ExecPlan& plan){
  bool dry_run = cfg.value("dry_run", true);

  std::string api_key, api_secret;
  if(cfg.contains("cex_key_local")){
    api_key = cfg["cex_key_local"].value("api_key", "");
    api_secret = cfg["cex_key_local"].value("api_secret", "");
  }
  if(api_key.empty() || api_secret.empty()){
    std::cout << "[exec/kraken] missing keys -> skip\n";
    return;
  }

  for(const auto& o : plan){
    if(dry_run){
      std::cout << "[exec/kraken] DRY_RUN " << o.symbol << " " << o.side << " contracts=" << o.qty << "\n";
      continue;
    }
    kraken_fut::send_order_market(cfg, api_key, api_secret, o.symbol, o.side, o.qty);
    std::cout << "[exec/kraken] sent " << o.symbol << " " << o.side << " contracts=" << o.qty << "\n";
  }
}