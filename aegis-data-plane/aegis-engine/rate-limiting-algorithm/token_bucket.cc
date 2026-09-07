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
                const string& redis_uri)
                :key_(key), 
                capacity_(capacity),
                refill_rate_(refill_rate),
                cluster_(redis_uri) // connects + discovers cluster topology automatically
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
    //Todo: Add specific token consumption states of token consumption here to let us know whether the token is being allowed, denied or unable to consume it. We should add enums such as, ALLOWED, DENIED, ERROR
    return execute_token_consumption_script(tokens); 
}

//Block token consumption until we have refilled the buckets to enough tokens
void TokenBucket::consume_blocking(int tokens_requested) {
    //Todo: Add specific token consumption states here to let us know whether the token is being allowed, denied or unable to consume it. We should add enums such as, ALLOWED, DENIED, ERROR
    while (!execute_token_consumption_script(tokens_requested)) {
        int available_tokens = available();
        int deficit = tokens_requested -  available_tokens;
        //Calcuate how long we should wait to fill up the deficits and then retry consuming the tokens

        long long wait_ms = 1000; // fallback if refill_rate_ can't cover it this tick
        if (refill_rate_ > 0) {
            wait_ms = static_cast<long long>((static_cast<double>(deficit) / refill_rate_) * 1000);
        }
        this_thread::sleep_for(chrono::milliseconds(std::max(wait_ms, 1LL)));
    }
}

//Function to get all currently avaialble tokens in the bucket 
int TokenBucket::available() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    //use redis client to get available tokens and the last time it was refilled
    auto raw_available_tokens = cluster_.hget(key_, "tokens"); 
    auto raw_last_refill = cluster_.hget(key_, "last_refill"); 

    int tokens = raw_available_tokens ? stoi(*raw_available_tokens) : capacity_;
    double last_refill = raw_last_refill ? stod(*raw_last_refill) : unix_time_now();

    double elapsed = unix_time_now() - last_refill;
    double refilled = std::min(static_cast<double>(capacity_), tokens + elapsed * refill_rate_);
    return static_cast<int>(refilled);
}

// -----------------------------------------------------------------------------
// Private helpers
// -----------------------------------------------------------------------------
bool TokenBucket::execute_token_consumption_script(int token_requested) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now = unix_time_now();

    try {
        long long result = cluster_.eval<long long> (
            CONSUME_SCRIPT, 
            { key_ }, 
            {
                to_string(capacity_),
                to_string(refill_rate_), 
                to_string(token_requested), 
                to_string(now)
            }
        ); 
        return result == 0; //redis script returned status code 0
    } catch (const sw::redis::Error & e) {
        cerr << "[token_bucket] Redis cluster error: " << e.what()
            << " --failing open\n"; 
        return true; //Fail open, report the error but still let the request to go through
    }
}; 