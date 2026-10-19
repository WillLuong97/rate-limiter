#include "token_bucket.h"

#include <vector> 
#include <iterator> 
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
RateLimitDecision TokenBucket::consume(int tokens) {
    return execute_token_consumption_script(tokens); 
}

// -----------------------------------------------------------------------------
// Private helpers
// -----------------------------------------------------------------------------
RateLimitDecision TokenBucket::execute_token_consumption_script(int token_requested) {
    std::lock_guard<std::mutex> lock(mutex_);

    try {
        vector<long long> results;
        cluster_.eval(
            CONSUME_SCRIPT, 
            { key_ }, 
            {
                to_string(capacity_),
                to_string(refill_rate_), 
                to_string(token_requested)
            },
            back_inserter(results) 
        ); 

        //base case: if the response comes back and does not have the same format
        if (results.size() != 3) {
            cerr << "[token_bucket]  Unexpected script reply size: " << results.size()
            << " ----failing open\n"; 
            return {true, -1, 0};
        } 

        return {
            results[0] == 1,
            static_cast<int>(results[1]), 
            static_cast<int>(results[2])

        }; 

    } catch (const sw::redis::Error & e) {
        cerr << "[token_bucket] Redis cluster error: " << e.what()
            << " --failing open\n"; 
        return {true, -1, 0}; //Fail open, report the error but still let the request to go through
    }
}; 