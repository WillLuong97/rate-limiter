# Building and running Aegis data plane 

## To build the project 

```
bazel build //:aegis-engine-main
```

## Command to build with Docker 
```
docker compose up -d --build
```

## To run the project after it got built 
```
bazel run //:aegis-engine-main
```

## run full test in docker
The unit tests need a live Redis Cluster. The cluster nodes only advertise their internal Docker addresses (`172.20.0.x`), which the host can't reach, so run the tests from a container on the cluster's network.

1. Start the stack and check the cluster is healthy (expected: `cluster_state:ok`)
```
cd aegis-engine
docker compose up -d
docker exec token-store-master-1 redis-cli cluster info | head -1
```

2. Build the `builder` stage image (has Bazel + sources; rebuild after changing code or tests)
```
docker build --target builder -t aegis-builder .
```

3. Run the tests on the cluster network
```
docker run --rm \
  --network aegis-engine_aegis-network \
  -v aegis-bazel-cache:/root/.cache/bazel \
  -e AEGIS_TEST_REDIS_URI=tcp://172.20.0.11:6379 \
  aegis-builder \
  bazel test //:token_bucket_test --test_env=AEGIS_TEST_REDIS_URI --test_output=all \
    --cache_test_results=no
```

If the cluster isn't reachable, every test is skipped and Bazel still reports `PASSED`, so check the output for `Redis cluster unreachable`.

`--cache_test_results=no` matters here: Bazel doesn't know the tests depend on Redis, and the `aegis-bazel-cache` volume keeps results between runs, so without it an unchanged rerun prints `PASSED (cached)` without touching the cluster.

## run a single test in docker
Same command as above, plus `--test_filter` (GoogleTest syntax: `*` wildcard, `:` separates patterns, `-` excludes). Quote the pattern so the shell doesn't expand `*`.
```
docker run --rm \
  --network aegis-engine_aegis-network \
  -v aegis-bazel-cache:/root/.cache/bazel \
  -e AEGIS_TEST_REDIS_URI=tcp://172.20.0.11:6379 \
  aegis-builder \
  bazel test //:token_bucket_test --test_env=AEGIS_TEST_REDIS_URI --test_output=all \
    --cache_test_results=no \
    --test_filter='TokenBucketTest.RetryAfterScalesWithDeficit'
```

Other filter examples:
```
--test_filter='TokenBucketTest.RetryAfterIsZeroWhenAllowed:TokenBucketTest.TokensRefillOverTime'   # several tests
--test_filter='TokenBucketTest.RetryAfter*'                                                          # a group by name
--test_filter='-TokenBucketTest.*Refill*'                                                            # everything except these
```

Repeat a timing-sensitive test to check for flakiness:
```
--test_filter='TokenBucketTest.TokensRefillOverTime' --test_arg=--gtest_repeat=20
```

List all test names (doesn't need Redis):
```
docker run --rm aegis-builder sh -c \
  'bazel build //:token_bucket_test && bazel-bin/token_bucket_test --gtest_list_tests'
```
