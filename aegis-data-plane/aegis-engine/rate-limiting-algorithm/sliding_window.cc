#include "sliding_window.h"

SlidingWindow::SlidingWindow(
    const string& key, 
    int capacity, 
    int refill_rate,
    const string& cluster_uri):key_(key), 
                capacity_(capacity),
                refill_rate_(refill_rate),
                cluster_(cluster_uri) // connects + discovers cluster topology automatically
{
    if (capacity <= 0) {
        throw std::invalid_argument("Capacity must be positive.");
    }
    
    if (refill_rate <= 0) {
        throw std::invalid_argument("Refill rate must be positive.");
    }
}

//Todo: implement the sliding window algorithm here
RateLimitDecision SlidingWindow::consume(int consumed_token){ 
    return {true, -1, 0}; //Placeholders
}