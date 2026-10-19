# pragma once 

struct RateLimitDecision {
    bool allowed; 
    int remaining; 
    int retry_after_seconds; 
}; 

/**
 * The abstract base RateLimiter class that can be extended to different rate limiting algorithm implementation 
 * ***/
class RateLimiter {
public:
    virtual ~RateLimiter() = default;

    //Returns whether the request is allowed, plus remaining tokens and retry-after seconds.
    virtual RateLimitDecision consume(int tokens = 1) = 0;
};
