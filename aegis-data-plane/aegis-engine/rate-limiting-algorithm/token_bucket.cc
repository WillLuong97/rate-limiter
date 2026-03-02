#include "token_bucket.h"

#include <algorithm>   // std::min
#include <thread>
#include <iostream>

// -----------------------------------------------------------------------------
// Constructor
// -----------------------------------------------------------------------------

TokenBucket::TokenBucket(double capacity, double refill_rate)
    : capacity_(capacity),
      refill_rate_(refill_rate),
      tokens_(capacity),   // start full
      last_refill_(std::chrono::steady_clock::now())
{
    if (capacity <= 0 || refill_rate <= 0)
        throw std::invalid_argument("Capacity and refill rate must be positive.");
}

// -----------------------------------------------------------------------------
// Public interface
// -----------------------------------------------------------------------------

bool TokenBucket::consume(double tokens) {
    std::lock_guard<std::mutex> lock(mutex_);
    refill();
    if (tokens_ >= tokens) {
        tokens_ -= tokens;
        return true;
    }
    return false;
}

void TokenBucket::consume_blocking(double tokens) {
    while (!consume(tokens)) {
        // Snapshot the deficit outside the lock to calculate sleep duration.
        double deficit;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            deficit = tokens - tokens_;
        }
        double wait_seconds = deficit / refill_rate_;
        auto wait_ms = static_cast<long long>(wait_seconds * 1000);
        std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
    }
}

double TokenBucket::available_tokens() {
    std::lock_guard<std::mutex> lock(mutex_);
    refill();
    return tokens_;
}

// -----------------------------------------------------------------------------
// Private helpers
// -----------------------------------------------------------------------------

void TokenBucket::refill() {
    // Caller must hold mutex_.
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - last_refill_).count();
    tokens_ = std::min(capacity_, tokens_ + elapsed * refill_rate_);
    last_refill_ = now;
}