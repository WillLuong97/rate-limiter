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
#include <iostream> 
#include <string>
#include "Bucket.h"
using namespace std; 

class TokenBucket {
    public: 
    string* createBucket(); 
    Bucket getBucket(string* bucketId);
    Bucket generateToken(); 
    void refillTokenForBucket(string* bucketId, Bucket bucket);
    void consumeTokenFromBucket(string* bucketId); 
    int* bucketSize(string* bucketId);
    void deleteBucket(string* bucketId); 

    void dropRequest();
    void routeToBackend(string* requestId, string* backendId); 

    private: 
    Bucket *bucket;
    int* bucketId; 
    string* requestId; 
    string* backendId; 
};
