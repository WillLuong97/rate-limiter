#pragma once 

#include <string> 
#include <optional>
#include <unordered_map> 
#include <mutex> 

using namespace std; 
//The rate limit configuration for one rule (algorithm, capacity, refill rate)
// This is what gets stored in DynamoDB/MongoDB eventually,
// and what the LRU cache holds in memory.
// For the current implementation, they are currently hardcoded as a hashmap
struct RateLimitConfig { 
    enum RateLimitAlgorithm {
        TOKEN_BUCKET,
        SLIDING_WINDOW, 
    }; 

    RateLimitAlgorithm algorithm = RateLimitAlgorithm::TOKEN_BUCKET; 
    int capacity = 100; // capacity:    100 tokens per IP
    int refill_rate = 10; //refill_rate: 10 tokens per second
    
    bool enabled = true;  // kill switch per rule 
    std::string description; //human readable label, for logging and auditing to let us know which type of configuration this is     
}; 

//Abstract Interface 
class ConfigProvider {
    public:
        ConfigProvider(); 
   
        optional<RateLimitConfig> get_config(const string& key); 

        // Phase 2: pre-warm cache from DynamoDB at startup
        // No-op for now — just logs a message
        void warm_up() {}

        // Phase 2: refresh stale entries from DynamoDB
        // No-op for now — just logs a message
        void refresh() {} 

    private:
        // Phase 1: simple in-memory map — replaced by LRU cache in Phase 2
        unordered_map<string, RateLimitConfig> config_map_;
        mutex mutex_; 
}; 