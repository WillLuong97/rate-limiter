#pragma once

#include <chrono>
#include "rate_limiter.h"
#include <stdexcept>
#include <hiredis/hiredis.h>
#include <string> 
#include <sw/redis++/redis++.h>
#include <mutex>

using namespace std; 

class SlidingWindow : public RateLimiter {
    public: 
        SlidingWindow(const string& key, 
                int capacity, 
                int refill_rate,
                const string& cluster_uri = "tcp://127.0.0.1:7001");  //place holders

        // Attempt to consume `tokens` tokens.
       //Returns whether the request is allowed, plus remaining tokens and retry-after seconds.
        RateLimitDecision consume(int consumed_token = 1) override;

    private:
        // ── Private class members ──────────────────────────────────────────────────────
        string key_; 
        int capacity_;
        int refill_rate_;
        mutex mutex_;
        // RedisCluster — NOT plain Redis but for it from Redis plus plus— handles slot routing,
        // MOVED/ASK redirects, and per-node connection pooling internally
        sw::redis::RedisCluster cluster_;
}; 