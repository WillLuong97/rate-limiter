#include "token_bucket.h"

#include <iostream>

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


// -----------------------------------------------------------------------------
// Token bucket interface implementation 
// -----------------------------------------------------------------------------
bool TokenBucket::consume(int tokens) {
    //Todo: Add specific token consumption states of token consumption here to let us know whether the token is being allowed, denied or unable to consume it. We should add enums such as, ALLOWED, DENIED, ERROR
    return execute_token_consumption_script(tokens); 
}

// -----------------------------------------------------------------------------
// Private helpers
// -----------------------------------------------------------------------------
bool TokenBucket::execute_token_consumption_script(int token_requested) {
    std::lock_guard<std::mutex> lock(mutex_);

    try {
        long long result = cluster_.eval<long long> (
            CONSUME_SCRIPT, 
            { key_ }, 
            {
                to_string(capacity_),
                to_string(refill_rate_), 
                to_string(token_requested)
            }
        ); 
        return result == 0; //redis script returned status code 0
    } catch (const sw::redis::Error & e) {
        cerr << "[token_bucket] Redis cluster error: " << e.what()
            << " --failing open\n"; 
        return true; //Fail open, report the error but still let the request to go through
    }
}; 