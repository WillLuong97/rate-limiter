#include "fastpath_config.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace aegis::fastpath;

namespace {

bool parse(std::vector<std::string> args, FastpathConfig* cfg, std::string* err) {
    args.insert(args.begin(), "aegis-fastpath");
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    return parse_fastpath_args(static_cast<int>(argv.size()), argv.data(), cfg, err);
}

}  // namespace

TEST(FastpathConfig, Defaults) {
    FastpathConfig cfg;
    std::string err;
    ASSERT_TRUE(parse({}, &cfg, &err)) << err;
    EXPECT_EQ(cfg.rate_pps, 1000u);
    EXPECT_EQ(cfg.burst, 2000u);
    EXPECT_EQ(cfg.on_table_full, TableFullPolicy::kAllow);
}

TEST(FastpathConfig, AllOptions) {
    FastpathConfig cfg;
    std::string err;
    ASSERT_TRUE(parse({"--rate", "5000", "--burst", "100", "--flows", "65536",
                       "--on-table-full", "drop", "--stats-interval", "0"},
                      &cfg, &err))
        << err;
    EXPECT_EQ(cfg.rate_pps, 5000u);
    EXPECT_EQ(cfg.burst, 100u);
    EXPECT_EQ(cfg.flows_per_core, 65536u);
    EXPECT_EQ(cfg.on_table_full, TableFullPolicy::kDrop);
    EXPECT_EQ(cfg.stats_interval_s, 0u);
}

TEST(FastpathConfig, Rejects) {
    for (const auto& args : std::vector<std::vector<std::string>>{
             {"--rate", "0"},
             {"--rate"},
             {"--rate", "-5"},
             {"--rate", "12abc"},
             {"--burst", "0"},
             {"--flows", "1000"},
             {"--on-table-full", "maybe"},
             {"--bogus"}}) {
        FastpathConfig cfg;
        std::string err;
        EXPECT_FALSE(parse(args, &cfg, &err)) << args[0];
        EXPECT_FALSE(err.empty());
    }
}
