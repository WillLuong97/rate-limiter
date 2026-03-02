/**
 * Header file containing the fucntions to implement the token bucket algorithm for the Aegis 
 * rate limiting capability. 
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
#include <mutex>
#include <stdexcept>

class TokenBucket {
public:
    // capacity     : max tokens the bucket can hold
    // refill_rate  : tokens added per second
    TokenBucket(double capacity, double refill_rate);

    // Attempt to consume `tokens` tokens.
    // Returns true if successful, false if not enough tokens.
    bool consume(double tokens = 1.0);

    // Block until `tokens` tokens are available, then consume them.
    void consume_blocking(double tokens = 1.0);

    // Returns the current number of available tokens.
    double available_tokens();

private:
    // Refills tokens based on elapsed time since last refill.
    // NOTE: Must be called with mutex_ already held.
    void refill();

    double capacity_;
    double refill_rate_;
    double tokens_;
    std::chrono::steady_clock::time_point last_refill_;
    std::mutex mutex_;
};
