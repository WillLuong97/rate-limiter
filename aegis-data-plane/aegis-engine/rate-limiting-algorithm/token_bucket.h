/**
 * Header file containing the fucntions to implement the token bucket algorithm for the Aegis 
 * rate limiting algorithm. 
 * 
 * The Token Bucket Algorithm is a simple yet effective technique used to control the rate at which requests are made to a system or server.
 * Picture it as an actual bucket that gets filled with tokens over time.
 *  Each token represents permission to make one request. 
 * When a client wants to make an API request, it must possess a token from the bucket.
 * If tokens are available, the request is granted, and a token is consumed. 
 * If the bucket is empty, the request is denied until more tokens are added over time. (https://medium.com/@surajshende247/token-bucket-algorithm-rate-limiting-db4c69502283)
 *  
 * **/
#pragma once

#include <chrono>
#include "rate_limiter.h"
#include <mutex>
#include <stdexcept>
#include <hiredis/hiredis.h>
#include <string> 
#include <sw/redis++/redis++.h>

using namespace std; 

class TokenBucket : public RateLimiter {
public:
    TokenBucket(const string& key, 
                int capacity, 
                int refill_rate,
                // any single node in the cluster works as the entry point —
                // redis-plus-plus will then discovers the rest via CLUSTER SLOTS internally after first connection
                const string& cluster_uri = "tcp://127.0.0.1:7001"
                );

    // Attempt to consume `tokens` tokens.
    // Returns true if successful, false if not enough tokens.
    bool consume(int consumed_token = 1) override; 

    void consume_blocking(int consumed_token = 1) override; 

    // Returns the current number of available tokens.
    int available() override; 


private:
    // ── Redis operations ──────────────────────────────────────────────────────
    //This is main logic to execute the hiredis Lua script to write tokens into 
    //redis bucket or consume from it.
    //Returns true if token were successfully consumed, otherwise, denies them 
    bool execute_token_consumption_script(int token_requested);

    // ── Private class members ──────────────────────────────────────────────────────
    string key_; 
    int capacity_;
    int refill_rate_;
    mutex mutex_;
    // RedisCluster — NOT plain Redis but for it from Redis plus plus— handles slot routing,
    // MOVED/ASK redirects, and per-node connection pooling internally
    sw::redis::RedisCluster cluster_;

    
    // ── Private class members ──────────────────────────────────────────────────────


    // ── Variable to store the actual lua script command ──────────────────────────────────────────────────────
    // This script runs atomically on the Redis server.
    // Redis is single-threaded so while this script runs,
    // no other command from any server can execute — preventing race conditions.
    //
    // KEYS[1]  = the Redis key        e.g. "ip:203.0.113.42:tokens"
    // ARGV[1]  = capacity             e.g. 100
    // ARGV[2]  = refill_rate          e.g. 10
    // ARGV[3]  = tokens_requested     e.g. 1
    // ARGV[4]  = current unix time    e.g. 1717000000.123
    //
    // Returns: 0 if allowed, 1 if denied
    static constexpr const char* CONSUME_SCRIPT = R"(
        local key = KEYS[1]
        local capacity = tonumber(ARGV[1])
        local refill_rate = tonumber(ARGV[2])
        local token_req = tonumber(ARGV[3])
        local now = tonumber(ARGV[4])

        -- ── Step 1: Read current state from Redis ────────────────────────────
        -- HGET returns nil if the key doesn't exist yet (first request from IP)
        local stored_tokens = redis.call('HGET', key, 'tokens')
        local stored_last_refill = redis.call('HGET', key, 'last_refill')

        -- ── Step 2: Get or initialize the extracted keys if they don't exist────────────────────────────
        local tokens = stored_tokens and tonumber(stored_tokens) or capacity
        local last_refill = stored_last_refill and tonumber(stored_last_refill) or now

    
        -- ── Step 3: Refill the tokens since elapsed times ────────────────────────────
        -- How long since we last refill the tokens for this IP? 
        local elasped = now - last_refill -- would be 0 for the first time 

        -- Refill the tokens in the bucket based on the elapsed time.
        -- Add tokens proportional to elapsed time, capped at capacity
        -- e.g. 2 seconds elapsed at 10 tokens/sec = +20 tokens
        tokens = math.min(capacity, tokens + (elasped * refill_rate))

        -- ── Step 4: Consumed the tokens ────────────────────────────
        if tokens >= token_req then 
            -- Enough token, so remove the requested tokens from the current token and return 1
            tokens = tokens - token_req

            -- Update the redis bucket with the new tokens value 
            redis.call('HSET', key, 'tokens', tokens)
            redis.call('HSET', key, 'last_refill', now)

            -- Auto-expired the key for this IP after 1 hour of inactivity, meaning the key has not been hit 
            redis.call('EXPIRE', key, 3600)
            return 0

        else
            -- Not enough tokens — update last_refill but do not consume
            -- We still write back so the refill timestamp stays accurate
            redis.call('HSET', key, 'tokens', tokens)
            redis.call('HSET', key, 'last_refill', now)

            -- Auto-expired the key for this IP after 1 hour of inactivity, meaning the key has not been hit 
            redis.call('EXPIRE', key, 3600)

            return 1
    )";
    
};
