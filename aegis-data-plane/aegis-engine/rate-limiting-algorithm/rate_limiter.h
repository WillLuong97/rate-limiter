# pragma once 
/**
 * The abstract base RateLimiter class that can be extended to different rate limiting algorithm implementation 
 * ***/
class RateLimiter {
public:
    virtual ~RateLimiter() = default;

    // Returns true if the request is allowed, false if rate limited
    virtual bool consume(int tokens = 1) = 0;

    // Returns the current number of available tokens/capacity
    virtual double available() = 0;
};
