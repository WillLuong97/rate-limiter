#include "sliding_window.h"

#include <vector> 
#include <iterator> 
#include <iostream>

SlidingWindow::SlidingWindow(
    const string& key, 
    int capacity, 
    int window_seconds,
    const string& cluster_uri):key_(key), 
                capacity_(capacity),
                window_seconds_(window_seconds),
                cluster_(cluster_uri) // connects + discovers cluster topology automatically
{
    if (capacity <= 0) {
        throw std::invalid_argument("Capacity must be positive.");
    }
    
    if (window_seconds <= 0) {
        throw std::invalid_argument("Time window must be positive.");
    }
}

//Todo: implement the sliding window algorithm here
RateLimitDecision SlidingWindow::consume(int token){ 
    return execute_token_consumption_script(token); 
}

//helper method to run the lua script to redis for sliding window algorithm 
RateLimitDecision SlidingWindow::execute_token_consumption_script(int token_requested) {
    try {
        vector <long long> results; 
        cluster_.eval(
            CONSUME_SCRIPT,
            {key_}, 
            {
                to_string(capacity_), 
                to_string(window_seconds_), 
                to_string(token_requested)
            },
            back_inserter(results)
        ); 

        //base case: if the response comes back and does not have the same format
        if (results.size() != 3) {
            cerr << "[sliding_window]]  Unexpected script reply size: " << results.size()
            << " ----failing open\n"; 
            return {true, -1, 0};
        } 

        return {
            results[0] == 1,
            static_cast<int>(results[1]), 
            static_cast<int>(results[2])

        }; 

    } catch (const sw::redis::Error & e) {
        cerr << "[sliding_window] Redis cluster error: " << e.what() 
            << " --failing open \n"; 
        return {true, -1, 0}; 
    }
}
