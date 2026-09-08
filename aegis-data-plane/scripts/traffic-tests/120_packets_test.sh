#!/bin/bash

for i in $(seq 1 120); do
  curl -s -o /dev/null -w "%{http_code}\n" http://localhost:8080/api/test
done | sort | uniq -c
