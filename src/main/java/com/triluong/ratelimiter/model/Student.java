package com.triluong.ratelimiter.model;

import lombok.Data;
import org.springframework.data.redis.core.RedisHash;

import java.io.Serializable;

@RedisHash("Student")
@Data
public class Student implements Serializable {
    private String id;
    private String name;

    private String gender;

    private int grade;

}
