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

bool SlidingWindow::consume(int consumed_token){ 
    //Todo: implement the sliding window algorithm here
    return true; 
}  

void SlidingWindow::consume_blocking(int consumed_token) {
    //Todo: Implement the sliding window logics for consume_blocking here
}  

// Returns the current number of available tokens.
int SlidingWindow::available() {
    //Placeholders for now, but still need to implement the sliding window logics here
    return -1; 
}