package com.triluong.ratelimiter.configuration.client;

import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;
import org.springframework.data.redis.connection.RedisStandaloneConfiguration;
import org.springframework.data.redis.connection.jedis.JedisConnectionFactory;
import org.springframework.data.redis.core.RedisTemplate;

//TODO: Add injectable config value into here instead of hardcoding it like this
@Configuration
public class RedisClientConfiguration {
    /**
     *
     * Defining the connection factory for the Jedis client, we are also overrding the default
     * redis connection with the custom port and hostname
     *
     *
     ***/
    @Bean
    JedisConnectionFactory jedisConnectionFactory() {
        RedisStandaloneConfiguration redisStandaloneConfiguration = new RedisStandaloneConfiguration("0.0.0.0", 6379);
        return new JedisConnectionFactory(redisStandaloneConfiguration);
    }

    /**
     *
     * Defined a RedisTemplate to query data with a custom repository
     * **/
    @Bean
    public RedisTemplate<String, Object> redisTemplate() {
        RedisTemplate<String, Object> template = new RedisTemplate<>();
        template.setConnectionFactory(jedisConnectionFactory());
        return template;
    }



}
