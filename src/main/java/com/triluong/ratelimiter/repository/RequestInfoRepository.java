package com.triluong.ratelimiter.repository;

import org.apache.coyote.RequestInfo;
import org.springframework.data.repository.CrudRepository;

import java.util.Optional;

/**
 *
 * This interface will help to set up the CRUD operations with the
 * dynamoDB with respect to the request info
 *
 * **/
public interface RequestInfoRepository extends CrudRepository<RequestInfo, String> {
    //find the current request from the db based on its id
    Optional<RequestInfo> findById(String id);
}

