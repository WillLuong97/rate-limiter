/**
 * 
 * This contains the code to start a GRPC server to listen for any requests from 
 * the Envoy proxy server to run the rate limiting logic 
 * **/

#pragma once

#include <grpcpp/grpcpp.h>
#include "envoy/service/ratelimit/v3/rls.grpc.pb.h"
#include "rate-limiting-algorithm/rate_limiter.h"
#include "rate-limiting-algorithm/token_bucket.h"

#include <unordered_map>
#include <memory>
#include <mutex>
#include <string>
#include "providers/config-providers.h"

// Aliases for the long Envoy proto namespaces
using RateLimitRequest  = envoy::service::ratelimit::v3::RateLimitRequest;
using RateLimitResponse = envoy::service::ratelimit::v3::RateLimitResponse;
using RateLimitService  = envoy::service::ratelimit::v3::RateLimitService;
using Code              = envoy::service::ratelimit::v3::RateLimitResponse::Code;

//Note: The RateLimitServiceImpl will implement the RateLimitService protobuf class from Envoy gRPC call 
class RateLimitServiceImpl final : public RateLimitService::Service {
    public: 
        //constructor, with explicit keyword, we are asking the 
        //compiler to enforce the user to this class to explicitly call out 
        // the RateLimitServiceImpl class type when calling it 
        explicit RateLimitServiceImpl (
            ConfigProvider& config_provider,          
            const std::string& cluster_nodes = "tcp://127.0.0.1:7001"           
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

    // ── Rule key resolution ───────────────────────────────────────────────────
    // Determines which ConfigProvider rule key applies to this request.
    // e.g. "ip" for generic IP limiting, "login" for login endpoint
    // Expanded in Phase 2 to extract endpoint from request descriptors
    std::string resolve_rule_key_from_request(const RateLimitRequest* request); 


    //Get or create a token bucket for a given redis key
    RateLimiter* get_or_create_limiter(const std::string bucket_key, const std::string rule_key); 

    ConfigProvider& config_provider_;               // holds algorithm, capacity, rate per rule
    std::string cluster_nodes_; 

    //One token bucket per IP address: 
    std::unordered_map<std::string, std::unique_ptr<RateLimiter>> limiters_; 
    std::mutex limiter_mutex;   
};