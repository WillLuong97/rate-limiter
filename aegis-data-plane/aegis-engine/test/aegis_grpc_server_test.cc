#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <sw/redis++/redis++.h>
#include "aegis-server/aegis_grpc_server.h"
#include "providers/config-providers.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
//
// These tests call RateLimitServiceImpl::ShouldRateLimit directly, the same
// function Envoy reaches over gRPC, but without starting a gRPC server. They
// check that building a limiter per request (no limiters_ cache) behaves the
// same as before. Like the limiter tests, they need a Redis Cluster:
//   AEGIS_TEST_REDIS_URI=tcp://172.20.0.11:6379 bazel test //:aegis_grpc_server_test
// If no cluster is reachable, every test is skipped instead of failing.
//
// Rules used (from ConfigProvider): login = SLIDING_WINDOW 5 per 60s,
// default = TOKEN_BUCKET 100 capacity.

static std::string redis_uri() {
    const char* uri = std::getenv("AEGIS_TEST_REDIS_URI");
    return uri ? uri : "tcp://127.0.0.1:7001";
}

class AegisGrpcServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        try {
            cluster_ = std::make_unique<sw::redis::RedisCluster>(redis_uri());
            cluster_->exists("aegis:test:ping");   // force a round trip
        } catch (const sw::redis::Error& e) {
            GTEST_SKIP() << "Redis cluster unreachable at " << redis_uri()
                         << " (" << e.what() << ")";
        }
        service_ = std::make_unique<RateLimitServiceImpl>(config_, *cluster_);
    }

    void TearDown() override {
        if (!cluster_) return;
        for (const auto& key : keys_) cluster_->del(key);
    }

    // A unique fake "IP" per test and per run. The 60s login window would
    // otherwise carry over between runs. The server doesn't validate the
    // format, it only uses it to build the Redis key.
    std::string unique_ip() {
        auto now = std::chrono::system_clock::now().time_since_epoch().count();
        return "test-" + std::to_string(now) + "-" + std::to_string(ip_counter_++);
    }

    // Build a request shaped like the one Envoy sends: one descriptor with
    // remote_address and (optionally) request_type entries.
    RateLimitRequest make_request(const std::string& ip, const std::string& request_type) {
        RateLimitRequest request;
        request.set_domain("api_gateway");
        auto* descriptor = request.add_descriptors();
        if (!ip.empty()) {
            auto* entry = descriptor->add_entries();
            entry->set_key("remote_address");
            entry->set_value(ip);
        }
        if (!request_type.empty()) {
            auto* entry = descriptor->add_entries();
            entry->set_key("request_type");
            entry->set_value(request_type);
        }
        // Remember the Redis key the server will create, so TearDown deletes it
        if (!ip.empty()) {
            keys_.push_back("ip:" + ip + ":" + (request_type.empty() ? "default" : request_type));
        }
        return request;
    }

    // Call ShouldRateLimit like Envoy would. The context isn't used by the
    // server, so nullptr is fine.
    RateLimitResponse call(const std::string& ip, const std::string& request_type) {
        RateLimitRequest request = make_request(ip, request_type);
        RateLimitResponse response;
        grpc::Status status = service_->ShouldRateLimit(nullptr, &request, &response);
        EXPECT_TRUE(status.ok());
        return response;
    }

    // Value of a response header, or "" if it isn't there
    static std::string header(const RateLimitResponse& response, const std::string& name) {
        for (const auto& h : response.response_headers_to_add()) {
            if (h.key() == name) return h.value();
        }
        return "";
    }

    ConfigProvider config_;
    std::unique_ptr<sw::redis::RedisCluster> cluster_;
    std::unique_ptr<RateLimitServiceImpl> service_;
    std::vector<std::string> keys_;
    int ip_counter_ = 0;
};

// ---------------------------------------------------------------------------
// 1. Decisions are the same without the limiters_ cache
// ---------------------------------------------------------------------------

TEST_F(AegisGrpcServerTest, LoginAllowsFiveThenBlocks) {
    // A new limiter is built for every call; the count must still carry over,
    // because it lives in Redis.
    std::string ip = unique_ip();
    for (int i = 1; i <= 5; ++i) {
        EXPECT_EQ(call(ip, "login").overall_code(), RateLimitResponse::OK)
            << "request " << i << " should be allowed";
    }
    EXPECT_EQ(call(ip, "login").overall_code(), RateLimitResponse::OVER_LIMIT);
}

TEST_F(AegisGrpcServerTest, RemainingHeaderCountsDown) {
    std::string ip = unique_ip();
    EXPECT_EQ(header(call(ip, "login"), "X-RateLimit-Remaining"), "4");
    EXPECT_EQ(header(call(ip, "login"), "X-RateLimit-Remaining"), "3");
}

TEST_F(AegisGrpcServerTest, OverLimitResponseHasRetryAfterHeader) {
    std::string ip = unique_ip();
    for (int i = 0; i < 5; ++i) call(ip, "login");
    RateLimitResponse response = call(ip, "login");
    EXPECT_EQ(response.overall_code(), RateLimitResponse::OVER_LIMIT);
    int retry_after = std::stoi(header(response, "X-RateLimit-RetryAfter"));
    EXPECT_GE(retry_after, 59);
    EXPECT_LE(retry_after, 60);
}

TEST_F(AegisGrpcServerTest, DifferentIpsAreLimitedIndependently) {
    std::string ip_a = unique_ip();
    std::string ip_b = unique_ip();
    for (int i = 0; i < 5; ++i) call(ip_a, "login");
    EXPECT_EQ(call(ip_a, "login").overall_code(), RateLimitResponse::OVER_LIMIT);
    EXPECT_EQ(call(ip_b, "login").overall_code(), RateLimitResponse::OK);
}

TEST_F(AegisGrpcServerTest, DifferentRulesForTheSameIpAreIndependent) {
    // ip:X:login and ip:X:default are separate Redis keys
    std::string ip = unique_ip();
    for (int i = 0; i < 5; ++i) call(ip, "login");
    EXPECT_EQ(call(ip, "login").overall_code(), RateLimitResponse::OVER_LIMIT);
    EXPECT_EQ(call(ip, "default").overall_code(), RateLimitResponse::OK);
}

TEST_F(AegisGrpcServerTest, MissingRequestTypeUsesDefaultRule) {
    // default = token bucket with capacity 100, so the first call leaves 99
    std::string ip = unique_ip();
    RateLimitResponse response = call(ip, "");
    EXPECT_EQ(response.overall_code(), RateLimitResponse::OK);
    EXPECT_EQ(header(response, "X-RateLimit-Remaining"), "99");
}

TEST_F(AegisGrpcServerTest, UnknownRequestTypeUsesHardcodedFallback) {
    // No "not_a_rule" config: the server falls back to RateLimitConfig{}
    // (token bucket, capacity 100) under the key ip:X:not_a_rule
    std::string ip = unique_ip();
    RateLimitResponse response = call(ip, "not_a_rule");
    EXPECT_EQ(response.overall_code(), RateLimitResponse::OK);
    EXPECT_EQ(header(response, "X-RateLimit-Remaining"), "99");
}

// ---------------------------------------------------------------------------
// 2. Fail open
// ---------------------------------------------------------------------------

TEST_F(AegisGrpcServerTest, MissingIpFailsOpen) {
    RateLimitResponse response = call("", "login");
    EXPECT_EQ(response.overall_code(), RateLimitResponse::OK);
    EXPECT_EQ(header(response, "X-RateLimit-Remaining"), "");   // no header sent
}

TEST_F(AegisGrpcServerTest, RedisErrorFailsOpenWithoutRemainingHeader) {
    // Put the wrong type at the key so the Lua script errors (WRONGTYPE)
    std::string ip = unique_ip();
    std::string redis_key = "ip:" + ip + ":login";
    cluster_->set(redis_key, "not-a-sorted-set");
    keys_.push_back(redis_key);

    RateLimitResponse response = call(ip, "login");
    EXPECT_EQ(response.overall_code(), RateLimitResponse::OK);
    EXPECT_EQ(header(response, "X-RateLimit-Remaining"), "");   // remaining = -1 → no header
}

// ---------------------------------------------------------------------------
// 3. Many new callers, no shared cache or lock
// ---------------------------------------------------------------------------

TEST_F(AegisGrpcServerTest, ManyNewIpsAreServedQuickly) {
    // Before the fix, every new IP built its own RedisCluster under a global
    // lock. Now a new IP costs one EVAL on the shared pool. 500 sequential
    // first-time callers should take well under a second on a local cluster.
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 500; ++i) {
        EXPECT_EQ(call(unique_ip(), "login").overall_code(), RateLimitResponse::OK);
    }
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, std::chrono::seconds(2));
}

TEST_F(AegisGrpcServerTest, ConcurrentRequestsFromOneIpRespectTheLimit) {
    // gRPC runs ShouldRateLimit on many threads at once. With no limiter_mutex,
    // exactly 5 of 40 concurrent login attempts from one IP may pass.
    // Build the request once, before the threads start: make_request() records
    // the key in keys_, and a std::vector must not be modified from many threads.
    const RateLimitRequest request = make_request(unique_ip(), "login");
    std::atomic<int> allowed{0};

    auto task = [&]() {
        for (int i = 0; i < 5; ++i) {
            RateLimitResponse response;
            service_->ShouldRateLimit(nullptr, &request, &response);
            if (response.overall_code() == RateLimitResponse::OK) ++allowed;
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) threads.emplace_back(task);
    for (auto& t : threads) t.join();

    EXPECT_EQ(allowed.load(), 5);
}

TEST_F(AegisGrpcServerTest, ConcurrentRequestsFromManyIpsAreAllAllowed) {
    // 8 threads x 25 brand-new IPs: every first request should pass.
    std::atomic<int> allowed{0};
    std::vector<std::vector<RateLimitRequest>> per_thread(8);
    for (auto& requests : per_thread) {
        for (int i = 0; i < 25; ++i) requests.push_back(make_request(unique_ip(), "login"));
    }

    std::vector<std::thread> threads;
    for (auto& requests : per_thread) {
        threads.emplace_back([&]() {
            for (const auto& request : requests) {
                RateLimitResponse response;
                service_->ShouldRateLimit(nullptr, &request, &response);
                if (response.overall_code() == RateLimitResponse::OK) ++allowed;
            }
        });
    }
    for (auto& t : threads) t.join();

    EXPECT_EQ(allowed.load(), 8 * 25);
}
