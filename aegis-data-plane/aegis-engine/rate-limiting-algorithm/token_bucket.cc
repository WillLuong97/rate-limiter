#include "token_bucket.h"

#include <algorithm>   // std::min
#include <thread>
#include <iostream>

using namespace std::chrono;

// -----------------------------------------------------------------------------
// Constructor
// -----------------------------------------------------------------------------
TokenBucket::TokenBucket(const string& key, 
                int capacity, 
                int refill_rate,
                const string& redis_uri, 
                )
                :key_(key), 
                capacity_(capacity),
                refill_rate_(refill_rate),
                redis_uri_(redis_uri) // connects + discovers cluster topology automatically
{
    if (capacity <= 0) {
        throw std::invalid_argument("Capacity must be positive.");
    }
        
    if (refill_rate <= 0) {
        throw std::invalid_argument("Refill rate must be positive.");

    }

}

// ── Helpers ───────────────────────────────────────────────────────────────────

// Returns the current Unix time as a double with sub-second precision
// e.g. 1717000000.123
// This is passed into the Lua script as ARGV[4] so the refill
// calculation uses the same clock on every server
static double unix_time_now() {
    auto now = steady_clock::now().time_since_epoch();
    return duration<double>(now).count();
}


// -----------------------------------------------------------------------------
// Token bucket interface implementation 
// -----------------------------------------------------------------------------
bool TokenBucket::consume(int tokens) {
    std::lock_guard<std::mutex> lock(mutex_);
    return execute_token_consumption_script(tokens); 
}

//Block token consumption until we have refilled the buckets to enough tokens
void TokenBucket::consume_blocking(int tokens_requested) {
    while (execute_token_consumption_script(tokens)) {
        int available_tokens = available();
        int deficit = tokens_requested -  available_tokens;
        //Calcuate how long we should wait to fill up the deficits and then retry consuming the tokens
        auto wait_ms = static_cast<long long>((deficit / available_tokens) * 1000); 
        this_thread::sleep_for(chrono::milliseconds(wait_ms)); 
    }
}

//Function to get all currently avaialble tokens in the bucket 
int TokenBucket::available() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    //use redis client to get available tokens and the last time it was refilled
    auto raw_available_tokens = cluster_.hget(key_, "tokens"); 
    auto raw_last_refill = cluster_.hget(key_, "last_refill"); 

    int tokens = raw_available_tokens ? stoi(*raw_available_tokens) : capacity_;
    double last_refill = raw_last_refill ? stoi(*raw_last_refill) : unix_time_now();

    double elapsed = unix_time_now() - last_refill; 
    return min(capacity_ , tokens + elapsed * last_refill); 
}

// -----------------------------------------------------------------------------
// Private helpers
// -----------------------------------------------------------------------------
bool execute_token_consumption_script(int token_requested) {
//Todo: implement logics to get, consume and update redis cluster
}; 