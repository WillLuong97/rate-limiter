package com.triluong.ratelimiter.controller;

import com.triluong.ratelimiter.model.Student;
import com.triluong.ratelimiter.service.RedisService;
import lombok.extern.slf4j.Slf4j;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.web.bind.annotation.*;

import java.util.List;


@RestController
@Slf4j
public class RedisController {

    @Autowired
    RedisService redisService;

    /**
     *
     * Endpoint to get the value from the key of the redis store
     *
     *
     * **/
    @GetMapping("/getvalue/{id}")
    public Student getValueFromKey(@PathVariable("id") String id){
        return redisService.getAStudent(id);

    }


    @PostMapping("/addkey")
    public void writeValue(@RequestBody Student student) {
        //saving user into the store
        redisService.writeStudent(student);
        log.info("Successfully writing into the object");
    }

    @GetMapping("/allkey")
    public List<Student> listAllStudent() {
        log.info("Proceed to get all key");
        return redisService.listAllStudent();

    }

}
