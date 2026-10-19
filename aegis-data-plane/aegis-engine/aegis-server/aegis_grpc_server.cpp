#include "aegis-server/aegis_grpc_server.h" 
#include <iostream> 


//Alias for for Envoy response code 
using Code = envoy::service::ratelimit::v3::RateLimitResponse::Code;
using LimiterFactory = std::function<std::unique_ptr<RateLimiter>(const std::string&, int, int, const std::string&)>;

//Constructor 
RateLimitServiceImpl::RateLimitServiceImpl(
    ConfigProvider& config_provider,
    const std::string& cluster_nodes) 
      : config_provider_(config_provider),
        cluster_nodes_(cluster_nodes)
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
    std::string config_key = resolve_rule_key_from_request(request); 
    std::string bucket_key  = build_bucket_key_from_ip(incoming_ip, config_key);
    

    //The limiter will be a pointer to a specific rate limiter class 
    RateLimiter* limiter = get_or_create_limiter(bucket_key, config_key);

    //The final decision after the applying rate limiting logic on the current request
    RateLimitDecision decision = limiter->consume();

    if (decision.remaining >= 0) {
        auto* remaining_header = response->add_response_headers_to_add(); 
        remaining_header->set_key("X-RateLimit-Remaining"); 
        remaining_header->set_value(std::to_string(decision.remaining));
    }

    if (decision.allowed) {
        response->set_overall_code(RateLimitResponse::OK);
        std::cout << "Info: Token for IP "
                 << incoming_ip
                 << " is available, allowing through!\n";
    } else {
        response->set_overall_code(RateLimitResponse::OVER_LIMIT);  
        std::cerr << "Error: Rate Limit Exceeded! "
                  << "Token for IP " 
                  << incoming_ip
                  << " is no longer available, blocking access \n";
        
        //Set header to 429, too many requests back to Envoy to let them know that the request has exceeded 
        //And let the client to retry. Envoy injects this header automatically.
        auto* retry_header = response->add_response_headers_to_add(); 
        retry_header->set_key("X-RateLimit-RetryAfter"); 
        retry_header->set_value(std::to_string(decision.retry_after_seconds));
    }

    return grpc::Status::OK;
}


//Private helpers 

//Build a redis key for the token bucket 
std::string RateLimitServiceImpl::build_bucket_key_from_ip(const std::string& ip, const std::string& config_key) {
    return "ip:" + ip + ":" + config_key; 
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
    //put a lock on the current rate limiting thread, RAII 
    std::lock_guard<std::mutex> lock(limiter_mutex); 
    
    //return existing rate limiters if one is already created 
    auto it = limiters_.find(bucket_key); 
    if (it != limiters_.end()) {
        return it->second.get();
    }


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

    //This store the rate limiting algorithm name to the class implementation of that algorithm
    static const unordered_map<
        RateLimitConfig::RateLimitAlgorithm, 
        LimiterFactory> algorithm_factory = {
            {
                RateLimitConfig::TOKEN_BUCKET, [](auto& key, auto capacity, auto refill_rate, auto& cluster_uri) {
                    return std::make_unique<TokenBucket>(key, capacity, refill_rate, cluster_uri);
                }
            }, 
            {  
                RateLimitConfig::SLIDING_WINDOW, [](auto& key, auto capacity, auto refill_rate, auto& cluster_uri) {
                    return std::make_unique<SlidingWindow>(key, capacity, refill_rate, cluster_uri);
                }
            }
        };

    auto algorithm_it = algorithm_factory.find(config->algorithm);
    
    if (algorithm_it == algorithm_factory.end()) {
        throw std::invalid_argument("Unsupported Algorithm");
    }
    
    limiters_[bucket_key] = algorithm_it->second(bucket_key, config->capacity, config->refill_rate, cluster_nodes_);
    return limiters_[bucket_key].get(); 
}

//Helper method to find the config key for aegis from the hardcoded config map 
std::string RateLimitServiceImpl::resolve_rule_key_from_request(const RateLimitRequest* request) {
    for(const auto& descriptor : request->descriptors()) {
        for (const auto& entry: descriptor.entries()) {
            if (entry.key() == "request_type") {
                return entry.value(); 
            }
        }
    }
    return "default"; 
}