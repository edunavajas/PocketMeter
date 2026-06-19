#pragma once
#include <Arduino.h>

#define MAX_PROVIDERS    12
#define PROVIDER_NAME_LEN 16

// Single provider data
struct ProviderData {
    char  name[PROVIDER_NAME_LEN]; // "claude", "codex", "gemini", "copilot", etc.
    float session_pct;             // Primary window utilization (0-100)
    int   session_reset_mins;      // Minutes until primary reset
    float weekly_pct;              // Secondary window utilization (0-100)
    int   weekly_reset_mins;       // Minutes until secondary reset
    char  status[16];              // "allowed", "limited", "approaching"
    char  plan_type[20];           // "pro", "plus", "free", "api", etc.
    float credits_balance;         // Credit balance (if applicable)
    bool  has_credits;             // Has credit/cost system
    bool  metrics_available;       // False when auth works but the provider exposes no usage windows
    bool  simulated;               // True if data is mocked/fallback
    bool  configured;              // Credentials/key/session exists locally
    bool  ok;                      // Data fetch succeeded
    char  note[128];               // Informational note for non-metric providers
    char  error[96];               // Human-readable error when ok=false
};

// Aggregated usage data from all providers
struct UsageData {
    ProviderData  providers[MAX_PROVIDERS];
    int           provider_count;   // How many entries are populated
    unsigned long timestamp;        // Last update timestamp (epoch s)
    bool          valid;            // false until first successful parse
};

#define CURRENT_PROVIDER 0
