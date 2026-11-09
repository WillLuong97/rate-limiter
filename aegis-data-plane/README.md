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

3. Run all the tests on the cluster network
```
docker run --rm \
  --network aegis-engine_aegis-network \
  -v aegis-bazel-cache:/root/.cache/bazel \
  -e AEGIS_TEST_REDIS_URI=tcp://172.20.0.11:6379 \
  aegis-builder \
  bazel test //:token_bucket_test //:sliding_window_test //:aegis_grpc_server_test \
    --test_env=AEGIS_TEST_REDIS_URI --test_output=all --cache_test_results=no
```

| Target | What it covers |
|---|---|
| `//:token_bucket_test` | Token bucket limiter: limits, refill, retry-after, fail-open, concurrency |
| `//:sliding_window_test` | Sliding window limiter: limits, sliding, what's stored in Redis, fail-open, concurrency (takes ~6s because of deliberate sleeps) |
| `//:aegis_grpc_server_test` | `ShouldRateLimit` called directly: per-rule limits, response headers, rule fallback, missing IP, fail-open, concurrent callers |

To run only one suite, list just that target. Add `//:aegis-engine-main` to the list to also check that the server binary builds.

If the cluster isn't reachable, every test is skipped and Bazel still reports `PASSED`, so check the output for `Redis cluster unreachable`.

`--cache_test_results=no` matters here: Bazel doesn't know the tests depend on Redis, and the `aegis-bazel-cache` volume keeps results between runs, so without it an unchanged rerun prints `PASSED (cached)` without touching the cluster.

## run the tests on your current code without rebuilding the image
The `aegis-builder` image contains a copy of the sources from when it was built. To test the code in your working folder instead, mount it into the container read-only:
```
cd aegis-engine
docker run --rm \
  --network aegis-engine_aegis-network \
  -v aegis-bazel-cache:/root/.cache/bazel \
  -v "$PWD":/app:ro \
  -e AEGIS_TEST_REDIS_URI=tcp://172.20.0.11:6379 \
  aegis-builder \
  bazel test //:aegis-engine-main //:token_bucket_test //:sliding_window_test //:aegis_grpc_server_test \
    --symlink_prefix=/ --lockfile_mode=off \
    --test_env=AEGIS_TEST_REDIS_URI --test_output=errors --cache_test_results=no
```

- `-v "$PWD":/app:ro` replaces the image's copy of the sources with your folder. `:ro` (read-only) keeps the container from changing your files.
- `--symlink_prefix=/` stops Bazel from creating `bazel-bin`/`bazel-out` symlinks, which it can't write into a read-only folder.
- `--lockfile_mode=off` stops Bazel from trying to update `MODULE.bazel.lock`, for the same reason.
- `--test_output=errors` only prints the logs of failing tests. Use `--test_output=all` to see every test case.

The first run can take a long time (over an hour) because Bazel compiles gRPC into the `aegis-bazel-cache` volume. Later runs reuse that cache and finish in seconds.

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
--test_filter='SlidingWindowTest.*Thread*'                                                           # the sliding window concurrency tests
--test_filter='AegisGrpcServerTest.LoginAllowsFiveThenBlocks'                                        # one gRPC server test
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
Swap in `sliding_window_test` or `aegis_grpc_server_test` to list those suites' tests. This uses the image's copy of the sources, so rebuild the image first (step 2) if the tests are newer than the image.
