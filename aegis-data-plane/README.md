# Building and running Aegis data plane 

## To build the project 

```
bazel build //:aegis-engine-main
```

## To run the project after it got built 
```
bazel run //:aegis-engine-main
```

## run all tests
```
bazel test //rate_limiter:token_bucket_test
```

## run with detailed output
```
bazel test //rate_limiter:token_bucket_test --test_output=all
```

## run a specific test by name
```
bazel test //rate_limiter:token_bucket_test --test_filter="TokenBucketTest.ConsumeDeductsTokens"
```
