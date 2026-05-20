#pragma once
#include <Arduino.h>

// Single provider data
struct ProviderData {
    char name[16];           // Provider name: "claude", "codex", etc.
    float session_pct;       // Primary window utilization (0-100)
    int session_reset_mins;  // Minutes until primary reset
    float weekly_pct;        // Secondary window utilization (0-100)
    int weekly_reset_mins;   // Minutes until secondary reset
    char status[16];         // "allowed", "limited", "approaching"
    char plan_type[16];      // "pro", "plus", "free", etc.
    float credits_balance;   // Credit balance (if applicable)
    bool has_credits;        // Has credit system
    bool simulated;          // True if data is mocked/fallback
    bool ok;                 // Data fetch succeeded
    char error[96];          // Human-readable error when ok=false
};

// Aggregated usage data from all providers
struct UsageData {
    ProviderData providers[2];  // Claude [0] and Codex [1]
    int provider_count;         // Number of configured providers
    unsigned long timestamp;    // Last update timestamp
    bool valid;                 // false until first successful parse
};

// Legacy support for single provider (Claude)
// Backwards compatibility with existing code
#define CURRENT_PROVIDER 0
