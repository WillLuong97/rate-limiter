#include "aegis-server/aegis_grpc_server.h" 
#include <iostream> 


//Alias for for Envoy response code 
using Code = envoy::service::ratelimit::v3::RateLimitResponse::Code;

//Constructor 
RateLimitServiceImpl::RateLimitServiceImpl(
    ConfigProvider& config_provider,
    const std::string& cluster_nodes) 
      : cluster_nodes_(cluster_nodes),
        config_provider_(config_provider)
{}


/**
 * 
 *  This is the method Envoy calls via gRPC for every incoming request.
 It receives a RateLimitRequest, unpacks the IP address from the descriptors,
 runs the token bucket logic, and writes OK or OVER_LIMIT into the response.
 * ***/
grpc::Status RateLimitServiceImpl::ShouldRateLimit(
            grpc::ServerContext* context, 
            const RateLimitRequest* request, 
            RateLimitResponse* response)
{

    //Unpack the request from Envoy and extract the IP address that we want to rate limit on 
    std::string incoming_ip = extract_ip(request); 

    //TODO: This would potentially be a vulnerability here, we are essentially allowing spoof IPs to go through if the Envoy cannot include the IP address in their request 
    // We will have to resume this, but in the meantime, we will let it fail open and allow through, given that Envoy is smart enough to always send a request with an IP address 
    if (incoming_ip.empty()) {
        //No IP address is found in the request, return an error code to Envoy -- log and fail open (Unable to perform rate limiting, so we will let the request to go through)
        std::cerr << "Error: no IP address is found in the Envoy rate limit request, allowing request to go through\n";
        response->set_overall_code(RateLimitResponse::OK);  
        return grpc::Status::OK; 
    };

    std::cout << "INFO: IP address found! Begin rate limiting...";

    //Construct a key for the rate limiter bucket 
    std::string bucket_key  = build_bucket_key_from_ip(incoming_ip);
    std::string rule_key = resolve_rule_key_from_request(request); 

    //The limiter will be a pointer to a specific rate limiter class 
    RateLimiter* limiter = get_or_create_limiter(bucket_key, rule_key);

    if (limiter->consume()) {
        response->set_overall_code(RateLimitResponse::OK);
        std::cout << "Info: Token for IP " 
                 << incoming_ip 
                 << " still available with " 
                 << limiter->available() 
                 << " tokens left, allowing through!\n"; 
    } else {
        response->set_overall_code(RateLimitResponse::OVER_LIMIT);  
        std::cerr << "Error: Rate Limit Exceeded! "
                  << "Token for IP " 
                  << incoming_ip 
                  << " is no longer with " 
                  << limiter->available() 
                  << " tokens left, blocking access \n";  
        
        //Set header to 429, too many requests back to Envoy to let them know that the request has exceeded 
        //And let the client to retry. Envoy injects this header automatically.
        auto* header = response->add_response_headers_to_add(); 
        header->set_key("X-RateLimit-RetryAfter"); 
        header->set_value("5"); // retry after 5 second 


        //Add an additional header to let the customer know their current rate limit token count 
        auto* additional_header = response->add_response_headers_to_add(); 
       additional_header->set_key("X-RateLimit-Remaining"); 
       additional_header->set_value("0"); 
    }

    return grpc::Status::OK;
}


//Private helpers 

//Build a redis key for the token bucket 
std::string RateLimitServiceImpl::build_bucket_key_from_ip(const std::string& ip) {
    return "ip:" + ip + ":tokens"; 
}


//Helper method to extract an ip address 
std::string RateLimitServiceImpl::extract_ip(const RateLimitRequest* request) {
    // Walk the descriptor entries Envoy sent and find "remote_address"
    //
    // Envoy descriptor structure:
    //   request
    //     └── descriptors[]
    //           └── entries[]
    //                 ├── key:   "remote_address"
    //                 └── value: "203.0.113.42"
    for (const auto& descriptor : request->descriptors()) {
        for (auto& entry : descriptor.entries()) {
            if (entry.key() == "remote_address") {
                return entry.value(); 
            }
        }
    }
    return ""; //no ip address found
}

//Helper method to call a rate limiter algorithm and perform rate limting on the request IP 
RateLimiter* RateLimitServiceImpl::get_or_create_limiter(const std::string bucket_key, const std::string rule_key) {  
    //put a lock on the current rate limiting thread 
    std::lock_guard<std::mutex> lock(limiter_mutex); 
    
    //return existing rate limiters if one is already created 
    auto it = limiters_.find(bucket_key); 
    if (it != limiters_.end()) {
        return limiters_[bucket_key].get();
    }; 


    // ── Ask ConfigProvider for this rule's config ─────────────────────────────
    // Phase 1: reads from in-memory map in ConfigProvider
    // Phase 2: reads from LRU cache → DynamoDB on miss
    // grpc_server.cpp never changes between phases — only ConfigProvider does

    auto config = config_provider_.get_config(rule_key); 

    if (!config.has_value()) {
        //If no config is found, we will default it default config 
        std::cerr << "[grpc_server] WARNING: no config found for rule '"
                  << rule_key << "' or 'default' — using hardcoded fallback\n";
        
        config = RateLimitConfig{}; //default constructor  
    }

    //Create the right rate limiting algorithm based on the config 
    std::cout << "[grpc_server] Creating limiter for: " << bucket_key
              << " | rule: "      << rule_key
              << " | algorithm: " << (config->algorithm ==
                                      RateLimitConfig::TOKEN_BUCKET
                                      ? "TOKEN_BUCKET" : "SLIDING_WINDOW")
              << " | capacity: "  << config->capacity
              << " | rate: "      << config->refill_rate << "\n";


    if (config->algorithm == RateLimitConfig::TOKEN_BUCKET) {
        try {
            //A token bucket currently does not exist for the current key, starting a new ones 
            std::cout << "Info: Creating a new token bucket for key: " << bucket_key << "\n"; 
            limiters_[bucket_key] = std::make_unique<TokenBucket>(
                bucket_key,
                config->capacity, 
                config->refill_rate,
                cluster_nodes_
            );
        } catch (const std::exception& e) {
            std::cerr << "[get_or_create_limiter] TokenBucket construction failed: "
                       << e.what() << "\n";
            throw;   // re-throw so behavior is unchanged — this just logs before it escapes
        }
    } else if (config->algorithm == RateLimitConfig::SLIDING_WINDOW) {
        try {
            //A token bucket currently does not exist for the current key, starting a new ones 
            std::cout << "Info: Creating a sliding window supported rate limiter for: " << bucket_key << "\n"; 
            limiters_[bucket_key] = std::make_unique<SlidingWindow>( //TODO: add sliding window implementation here
                bucket_key,
                config->capacity, 
                config->refill_rate,
                cluster_nodes_
            );
        } catch (const std::exception& e) {
            std::cerr << "[get_or_create_limiter] TokenBucket construction failed: "
                       << e.what() << "\n";
            throw;   // re-throw so behavior is unchanged — this just logs before it escapes
        } 
    } else {
        std::cerr << "[aegis_grpc_server] Unsupported rate limiter algorithm for: " << bucket_key << "\n"; 
        throw; 
    }

    return limiters_[bucket_key].get(); 
}

//Helper method to find the config key for aegis from the hardcoded config map 
std::string RateLimitServiceImpl::resolve_rule_key_from_request(const RateLimitRequest* request) {
        // Phase 1: always use the generic "ip" rule
    // Phase 2: extract endpoint path from request descriptors and
    //          return "login", "payment", "export" etc. so ConfigProvider
    //          can return the right algorithm per endpoint
    //
    // e.g. Phase 2 logic:
    //   for (const auto& descriptor : request->descriptors())
    //       for (const auto& entry : descriptor.entries())
    //           if (entry.key() == "path") {
    //               if (entry.value() == "/api/login")   return "login";
    //               if (entry.value() == "/api/payment") return "payment";
    //           }
    return "ip"; 
}