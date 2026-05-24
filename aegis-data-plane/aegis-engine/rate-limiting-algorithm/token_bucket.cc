#include "token_bucket.h"

#include <algorithm>   // std::min
#include <thread>
#include <iostream>

// -----------------------------------------------------------------------------
// Constructor
// -----------------------------------------------------------------------------

TokenBucket::TokenBucket(int capacity, int refill_rate)
    : capacity_(capacity),
      refill_rate_(refill_rate),
      currently_available_tokens(capacity),   // start at full capacity
      last_refill_(std::chrono::steady_clock::now())
{
    if (capacity <= 0 || refill_rate <= 0)
        throw std::invalid_argument("Capacity and refill rate must be positive.");
}

// -----------------------------------------------------------------------------
// Public interface
// -----------------------------------------------------------------------------
bool TokenBucket::consume(int consumed_token) {
    std::lock_guard<std::mutex> lock(mutex_);
    refill();
    if (currently_available_tokens >= consumed_token) {
        currently_available_tokens -= consumed_token;
        return true;
    }
    return false;
}

double TokenBucket::available() {
    std::lock_guard<std::mutex> lock(mutex_);
    refill();
    return currently_available_tokens;
}

// -----------------------------------------------------------------------------
// Private helpers
// -----------------------------------------------------------------------------

void TokenBucket::refill() {
    // Caller must hold mutex_.
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - last_refill_).count();
    currently_available_tokens = std::min(capacity_, currently_available_tokens + elapsed * refill_rate_);
    last_refill_ = now;
}