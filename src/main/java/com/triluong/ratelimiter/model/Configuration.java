package com.triluong.ratelimiter.model;


import lombok.Data;
import org.springframework.data.redis.core.RedisHash;

import java.sql.Timestamp;

/**
 *
 * Represent the model of the configuration for the rate limiter for each of the request
 * this shows how the rate limiter will limit the request rate from each of the request
 *
 * This will be stored in a Redis cache key stored
 *
 * **/
@Data
@RedisHash("Configuration")
public class Configuration {
    //Todo: add the attributes for the configuration of the rate-limiter here
    private int id;

    private int userId;

    private int capacity;

    private Timestamp timeWindow;
}
