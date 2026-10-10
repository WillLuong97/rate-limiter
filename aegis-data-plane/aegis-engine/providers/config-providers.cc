#include "config-providers.h"
#include <iostream> 

ConfigProvider::ConfigProvider() {
    // Phase 1: hardcoded rules — will be replaced by DynamoDB fetch in Phase 2
    config_map_["default"] = {
        RateLimitConfig::TOKEN_BUCKET,
        100,
        10,
        true,
        "Default token bucket rule"
    }; 

    config_map_["ip"] = {
        RateLimitConfig::TOKEN_BUCKET, 
        100, 
        10, 
        true, 
        "Per-IP token bucket"
    }; 

    config_map_["login"] = {
        RateLimitConfig::SLIDING_WINDOW, 
        5, //5 requests 
        60, //window is 60 seconds
        true, 
        "Login endpoint slidign window for 5 request per 60 seconds time window"
    };

    config_map_["payment"] = {
        RateLimitConfig::SLIDING_WINDOW,
        10,
        3600,
        true,
        "Payment endpoint sliding window — 10 requests per hour"
    };
}


optional<RateLimitConfig> ConfigProvider::get_config(const string& key) {
    std::lock_guard<mutex> lock(mutex_); 


    //For phase 1, we are just reading the config straight from the hard coded config map above 
    //With phase 2, we will read these results from config agent from DynamoDB database 
    auto it = config_map_.find(key); 

    if (it == config_map_.end()) {
        cout << "[config_provider] No config found for key: " << key << endl;
        return nullopt; 
    }

    return it->second; 
}


void ConfigProvider::warm_up() {
    // Phase 1: no-op
    // Phase 2: fetch all rules from DynamoDB and populate LRU cache
    std::cout << "[config_provider] warm_up() — "
              << "Phase 2: will fetch from DynamoDB\n";

}

void ConfigProvider::refresh() {
    // Phase 1: no-op
    // Phase 2: re-fetch stale rules from DynamoDB and update LRU cache
    std::cout << "[config_provider] refresh() — "
              << "Phase 2: will re-fetch from DynamoDB\n";

}