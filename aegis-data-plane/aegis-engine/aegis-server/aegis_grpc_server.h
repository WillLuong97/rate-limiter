/**
 * 
 * This contains the code to start a GRPC server to listen for any requests from 
 * the Envoy proxy server to run the rate limiting logic 
 * **/

#pragma once

#include <grpcpp/grpcpp.h>
#include "ratelimit.grpc.pb.h"       // generated from Envoy's proto
#include "rate-limiting-algorithm/rate_limiter.h"
#include "rate-limiting-algorithm/token_bucket.h"

#include <unordered_map>
#include <memory>
#include <mutex>
#include <string>

// Aliases for the long Envoy proto namespaces
using RateLimitRequest  = envoy::service::ratelimit::v3::RateLimitRequest;
using RateLimitResponse = envoy::service::ratelimit::v3::RateLimitResponse;
using RateLimitService  = envoy::service::ratelimit::v3::RateLimitService;


//Note: The RateLimitServiceImpl will implement the RateLimitService protobuf class from Envoy gRPC call 
class RateLimitServiceImpl final : public RateLimitService::Service {
    public: 
        //constructor, with explicit keyword, we are asking the 
        //compiler to enforce the user to this class to explicitly call out 
        // the RateLimitServiceImpl class type when calling it 
        explicit RateLimitServiceImpl (
            int capacity = 100;  //100 tokens
            int refill_rate = 10; //10 seconds refill rate 
            const std::string& redis_host = "127.0.0.1"; //default redis server host ip address
            int redis_port = 6379; //default redis server ip port 
        ); 


        grpc::Status ShouldRateLimit(
            grpc::ServerContext* context, 
            const RateLimitRequest* request, 
            RateLimitResponse* reponse
        ) override; 

    private: 
    //Extract the client IP from the Envoy gRPC rate limiting request
    std::string extract_ip(const RateLimitRequest* request); 

    // Build the Redis key from the IP address
    std::string build_bucket_key_from_ip(const std::string& ip);


    //Get or create a token bucket for a given redis key
    RateLimiter* get_or_create_limiter(const std::string key); 

    int capacity = 100;  //100 tokens
    int refill_rate = 10; //10 seconds refill rate 
    const std::string& redis_host = "127.0.0.1"; //default redis server host ip address
    int redis_port = 6379; //default redis server ip port 

    //One token bucket per IP address: 
    std::unordered_map<std::string, std::unique_ptr<RateLimiter>> limiters_; 
    std::mutex limiter_mutex;   
}