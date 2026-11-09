#include "aegis-server/aegis_grpc_server.h" 
#include <iostream> 


//Alias for for Envoy response code 
using Code = envoy::service::ratelimit::v3::RateLimitResponse::Code;
using LimiterFactory = std::function<std::unique_ptr<RateLimiter>(int, int, sw::redis::RedisCluster&)>;

//Constructor 
RateLimitServiceImpl::RateLimitServiceImpl(
    ConfigProvider& config_provider,
    sw::redis::RedisCluster& redis_cluster) 
      : config_provider_(config_provider),
        redis_cluster_(redis_cluster)
{}


/**
 * 
 *  This is the method Envoy calls via gRPC for every incoming request.
 It receives a RateLimitRequest, unpacks the IP address from the descriptors,
 runs the rate limiting logic, and writes OK or OVER_LIMIT into the response.
 * ***/
grpc::Status RateLimitServiceImpl::ShouldRateLimit(
            grpc::ServerContext* context, 
            const RateLimitRequest* request, 
            RateLimitResponse* response)
{

    //Unpack the request from Envoy and extract the IP address that we want to rate limit on 
    std::string incoming_ip = extract_ip(request); 

    if (incoming_ip.empty()) {
        //No IP address is found in the request, return an error code to Envoy -- log and fail open (Unable to perform rate limiting, so we will let the request to go through)
        std::cerr << "Error: no IP address is found in the Envoy rate limit request, allowing request to go through\n";
        response->set_overall_code(RateLimitResponse::OK);  
        return grpc::Status::OK; 
    };

    std::cout << "INFO: IP address found! Begin rate limiting...\n";

    //Construct a key for the rate limiter bucket
    std::string config_key = resolve_rule_key_from_request(request); 
    std::string bucket_key  = build_bucket_key_from_ip(incoming_ip, config_key);
    
    //The final decision after applying rate limiting logic on the current request
    RateLimitDecision decision;

    try {
        std::unique_ptr<RateLimiter> limiter = create_limiter(config_key); 
        decision = limiter->consume(bucket_key);       
    } catch (const std::exception& e) {
        // Last line of defence: limiters fail open on Redis errors themselves,
        // this covers limiter creation and anything a limiter doesn't handle
        std::cerr << "[aegis-server] Error: Rate Limiter failed for " 
                    <<  bucket_key 
                    << ": " << e.what() << "--failing open\n";
        response->set_overall_code(RateLimitResponse::OK);
        return grpc::Status::OK; 
    }; 

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
std::unique_ptr<RateLimiter> RateLimitServiceImpl::create_limiter(const std::string& rule_key) {  

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

    //This store the rate limiting algorithm name to the class implementation of that algorithm
    static const unordered_map<
        RateLimitConfig::RateLimitAlgorithm, 
        LimiterFactory> algorithm_factory = {
            {
                RateLimitConfig::TOKEN_BUCKET, [](int capacity, int refill_rate, sw::redis::RedisCluster& redis_cluster) {
                    return std::make_unique<TokenBucket>(capacity, refill_rate, redis_cluster);
                }
            }, 
            {  
                RateLimitConfig::SLIDING_WINDOW, [](int capacity, int window_seconds, sw::redis::RedisCluster& redis_cluster) {
                    return std::make_unique<SlidingWindow>(capacity, window_seconds, redis_cluster);
                }
            }
        };

    auto algorithm_it = algorithm_factory.find(config->algorithm);
    
    if (algorithm_it == algorithm_factory.end()) {
        throw std::invalid_argument("Unsupported Algorithm");
    }
    
    return algorithm_it->second(config->capacity, config->refill_rate, redis_cluster_);

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