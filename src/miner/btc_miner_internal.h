// ============================================================================
//  btc_miner_internal.h
//
//  Internal state shared between btc_miner.cpp (protocol / hash) and
//  btc_ui.cpp (rendering / reels). Not exposed to main.cpp - main only
//  uses the small public API in btc_miner.h.
// ============================================================================
#pragma once

#include <stdint.h>
#include <string>

enum MinerPhase {
    MINER_OFF,
    MINER_SETUP,        // showing captive portal, waiting for config
    MINER_WIFI_CONN,    // attempting WiFi connect
    MINER_POOL_CONN,    // attempting Stratum TCP connect
    MINER_SUBSCRIBE,    // sent mining.subscribe, awaiting reply
    MINER_AUTHORIZE,    // sent mining.authorize, awaiting reply
    MINER_MINING,       // happy path - shares are getting hashed
    MINER_ERROR,        // disconnected / bad config, retrying soon
};

// Coin/mode selector cycled by right-button long-press.
// Cycle: BTC SOLO -> BTC POOL -> BCH SOLO -> DGB SOLO -> back
//
// BTC SOLO:  public-pool.io:21496      — lottery, proven ESP32 compatible
// BTC POOL:  btc.hmpool.io:3337        — PPLNS proportional, diff 0.01, real payouts
// BCH SOLO:  bitcoincash.nerdpool.xyz  — NMMiner-confirmed BCH solo
// DGB SOLO:  dgb-stratum.solominer.net — NMMiner-confirmed DGB solo
enum ActivePool {
    POOL_BTC,       // Bitcoin solo        (lottery jackpot)
    POOL_BTC_POOL,  // Bitcoin proportional (HashedMax PPLNS, real payouts)
    POOL_BCH,       // Bitcoin Cash solo   (realistic solo win)
    POOL_DGB_SOLO,  // DigiByte solo       (low-diff solo)
    POOL_DGB = POOL_DGB_SOLO,   // legacy alias
    POOL_COUNT = 4,
    POOL_DGB_POOL = POOL_DGB_SOLO, // removed - aliased to DGB_SOLO
};

// Pool mode labels shown in Zone 1 of the miner UI
static const char* const POOL_LABELS[] = {
    "BTC SOLO",
    "BTC POOL",
    "BCH SOLO",
    "DGB SOLO",
};

// Coin ticker for the active pool
static const char* const POOL_TICKERS[] = {
    "BTC", "BTC", "BCH", "DGB",
};

struct MinerConfig {
    std::string wifi_ssid;
    std::string wifi_password;
    std::string btc_address;
    std::string bch_address;
    std::string dgb_address;
    // Pool host/port for each slot
    std::string btc_host;
    uint16_t    btc_port;
    std::string btcp_host;       // BTC proportional pool (HashedMax)
    uint16_t    btcp_port;
    std::string bch_host;
    uint16_t    bch_port;
    std::string dgb_host;
    uint16_t    dgb_port;
    // legacy aliases
    std::string dgbp_host;       // unused, kept for NVS compat
    uint16_t    dgbp_port;
    bool        use_dgb_fallback;
};

struct MinerStats {
    uint32_t  session_started_at;
    uint64_t  hashes_total;
    uint32_t  hashes_since_rate;
    uint32_t  last_rate_at;
    float     hashrate_kHs;
    uint32_t  shares_submitted;
    uint32_t  shares_accepted;
    uint32_t  shares_rejected;
    uint32_t  spins_total;       // reel spins (every quick-filter pass)
    uint32_t  jobs_received;
    double    last_share_diff;
    double    session_best_diff;
    double    all_time_best_diff;
    uint32_t  blocks_found;
};

struct MinerState {
    bool         running = false;
    bool         portalActive = false;
    bool         requestReconnect = false;
    bool         requestCoinSwitch = false;  // set true to cycle to nextPool after job
    bool         coinSwitchPending  = false;  // waiting for current job boundary
    ActivePool   nextPool           = POOL_BTC;
    bool         resetConfirmPending = false; // left-long confirm overlay
    uint32_t     resetConfirmShownAt = 0;
    bool         btcConfigured = false;
    bool         dgbConfigured = false;
    MinerPhase   phase = MINER_OFF;
    uint32_t     phaseEnteredAt = 0;
    char         status_line[48] = "";
    ActivePool   activePool = POOL_BTC;
    std::string  ap_ssid;
    std::string  ap_ip;
    MinerConfig  cfg;
    MinerStats   stats;
    bool         uiSpinTrigger = false;   // set true to fire a reel-spin event
    uint32_t     blockWinFlash = 0;       // millis() at which we hit a block
    // Button hold progress (ms held, 0 when not held) - read by btc_ui.cpp
    uint32_t     rightHoldMs = 0;         // for coin-cycle progress bar
    uint32_t     leftHoldMs  = 0;         // for reset-confirm progress bar

    // Live jackpot data (fetched via HTTP every 90s while mining)
    float        coin_price_usd  = 0.0f;  // current USD price of active coin
    float        jackpot_usd     = 0.0f;  // block_reward * price
    float        odds_days       = 0.0f;  // expected days to find a block
    uint32_t     priceLastFetch  = 0;     // millis() of last successful fetch
    bool         priceFetching   = false; // HTTP request in flight
};

extern MinerState g_miner;

bool minerConfigComplete(const MinerConfig &c);
