#pragma once

#include <chrono>
#include "rate_limiter.h"
#include <stdexcept>
#include <hiredis/hiredis.h>
#include <string> 
#include <sw/redis++/redis++.h>

class SlidingWindow : public RateLimiter {
    public: 
        SlidingWindow(int capacity, 
                int window_seconds,
                sw::redis::RedisCluster& redis_cluster);

        // Attempt to consume `tokens` tokens.
       //Returns whether the request is allowed, plus remaining tokens and retry-after seconds.
        RateLimitDecision consume(const std::string& key, int token) override;

    private:
        // ── Private class members ──────────────────────────────────────────────────────
        RateLimitDecision execute_token_consumption_script(const std::string& key, int token_requested);
        int capacity_;
        int window_seconds_;
        // The shared client created in main(). The limiter only borrows it;
        // main() owns it and keeps it alive for the whole process.        
        sw::redis::RedisCluster& redis_cluster_; 

        //Lua scripts to support sliding window algorithm
        // ── Variable to store the actual lua script command ──────────────────────────────────────────────────────
        // Sliding window log: one sorted-set entry per allowed request,
        // scored by its Redis-server timestamp in milliseconds.
        //
        // KEYS[1]  = the Redis key        e.g. "ip:203.0.113.42:login"
        // ARGV[1]  = capacity             e.g. 5
        // ARGV[2]  = window_seconds       e.g. 60
        // ARGV[3]  = tokens_requested     e.g. 1
        //
        // Returns: {allowed (1/0), remaining, retry_after seconds}
        static constexpr const char* CONSUME_SCRIPT = R"(
        local key = KEYS[1]
        local capacity = tonumber(ARGV[1])
        local time_window_ms =  tonumber(ARGV[2]) * 1000
        local req = tonumber(ARGV[3])

        -- - Step 0: Getting the current time from the redis server ───────────────────────────
        -- TIME returns {unix_seconds, microseconds} as strings
        local redis_current_time = redis.call('TIME')
        local now_ms = tonumber(redis_current_time[1]) * 1000 + math.floor(tonumber(redis_current_time[2]) / 1000)   -- (2) (3)
        -- - Step 1: Remove any requests that is older than the current time window ───────────────────────────
        redis.call('ZREMRANGEBYSCORE', key, '-inf', now_ms - time_window_ms)

        -- - Step 2: Count all the requests that are currently in the time window ───────────────────────────
        -- - and check if they are allowed to go through or not
        local count = redis.call('ZCARD', key)
        
        --allowed request, will add this request timestamp into the redis entry
        if count + req <= capacity then 
            for i = 1, req do
                -- We are adding to the set, the current redis server time of the request as the score
                redis.call('ZADD', key, now_ms, redis_current_time[1] .. ':' .. redis_current_time[2] .. ':' .. (count+i))
            
            end
            -- Set a key ttl to be any keys that fell outside of the current time window
            redis.call('PEXPIRE', key, time_window_ms)
            return {1, capacity - count - req, 0}

        end 

        --rejected requests, do not add this into the redis entry but return how long the user has to wait before retrying
        local oldest = redis.call('ZRANGE', key, 0, 0, 'WITHSCORES')
        local retry = tonumber(ARGV[2]) 
        if oldest[2] then 
            retry = math.ceil((tonumber(oldest[2]) + time_window_ms - now_ms) / 1000)
        end
        return {0, capacity - count, math.max(1, retry)}
        )";
}; 