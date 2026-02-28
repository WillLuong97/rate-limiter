# Aegis

Rate limiting is a strategy for limiting the network traffic accessing an application server. It helps to prevent attacks that aims at overloading a system or service by putting a cap on how often a client can repeat a request to the server, i.e logging into the services or requesting a resource in a given time. Rate limiter is the best approach to prevent distributed denial-of-service (DDoS). 

A recent stress testing event, which simulates a DDoS attack, on some of our internal applications such as the [Covid19 tracker](https://github.com/WillLuong97/Corona-Virus-Tracker), [RabbitMQ cluster](https://github.com/WillLuong97/rabbitmqclustermessageprovider), [LRU Cache](https://github.com/WillLuong97/LRU-Cache-Implementation) has reported a significant amount of latency in the API performance with some even crashes the application entirely due to the server being overloaded with so many requests exceeding its threshold. 

To migitate the issue, we tried to restart the server and even scaled up the applications by adding more instances. However, due to the overwhelming requests that also come in at a high speed, manual mitigations becomes too tie comsuming, and most importantly, too expensive due to the additional cost of adding more server instances. 

Therefore, project Aegis is created to provide a Rate limiting solution to help protect our application by automatically detecting and enforcing a cap on a client number of request in a given time frame. 

## Requirements

- The rate limiting logic works by dropping all requests coming from a client that has exceeded the allowed number of requests in a given time window. If the number of requests have not exceeded, the request will then be routed to the original application backend that was intended for these requests.

- Aegis should identify the users by its ID, IP address (IPv4, IPv6), or API keys.

- Aegis need to return proper error headers and status codes (429 - too many request) for requests that are dropped. 

- Aegis will support all major rate limiting algorithms, such as Token Bucket, Leaky Bucket, Fixed Window counter, Sliding Window counter to give more options on how certain applications can choose to rate limit themselves.

- Similar to the algorithms, the number of requests allowed and the time windows are configurable. 

## Scale
- Aegis is an internal service to protect other internal applications ([Covid19 tracker](https://github.com/WillLuong97/Corona-Virus-Tracker), [RabbitMQ cluster](https://github.com/WillLuong97/rabbitmqclustermessageprovider), [LRU Cache](https://github.com/WillLuong97/LRU-Cache-Implementation)), we estimated the scale of each the service above to be: 
    1. Covid19 tracker: 
        a. DAU: 1000
        b. 10,000 rps
    2. RabbitMQ cluster
        a. DAU: 1000
        b. 100,000 rps
    3. LRU Cache
        a. DAU: 10000
        b. 1M rps

- From the above stats, we are expected to scale up to 10000 DAU and 1M RPS. 

### CAP Theorem
- The primary goal of Aegis is to be the first line of defense for our services in the event of a DDoS attack. These attacks also happened over a long period of time, spaning over multiple days, even weeks, rendering our services useless within those times. Because of this, Aegis is designed to prioritize availability first as it needs to be able to protect our application at all times. 

- To maintain availability, Aegis will follow a variety of standard best practices such as, redundancy, load balancing, failover mechanisms, distributed systems, replication, and health checks, which would be covered in the Architecture section.

- Futhermore, a trade-off for maintaining availability is consistency, this means that if there are configuration changes for rate limting rules, or expected maintainance downtime, Aegis would continue to stay up and process rate limting with outdated rules until the changes are propagated. The CICD pipeline for Aegis and configuration changes will be discusses further in the Continuous Integration and Continuous Delivery section

- Aegis is also expected to provide low latency rate limit checks to avoid adding extra wait time on the request from our users. Due to the expectation of 1M rps, Aegis rate limiting process should only add less than 10ms to the overall request processing time from the clients.

- Similarly, Partition Tolerance would be another priority for Aegis as our system would still stay up despite 1 or 2 nodes of Aegis's cluster fail to communicate with each other due to a network loss.


## Architecture
![Alt text](<media/Rate Limiter Architecture Design.png>)

## Implementation 

### Core entities: 
- Requests
- Client (IP, userId, Api key) --- Who is making the request?
- Rules (Rate limiting rules)  --- How to limit the requests?

### System interface
- isRequestAllowed(clientId,rulesId) -> {isAllowedThrough: boolean, remaining: number, resetTimes: timestamp} : When a new request comes in, this function will be called to trigger the rate limiting against a clientId and the rulesId. 

- getHealhChecks(aegisInstanceIp) -> {status: (ACTIVE|FAILED), time: timeStamp} : We will have a separate health checker to periodically ping the current Aegis instance IP to check for its health, this is to ensure that our service would stay up.


- Aegis will have 2 main components, the Control plane and the Data plane. 
- The control plane provdes a platform for our depdending services to configure the behavior of their Rate Limiting instances via a set of CRUDL (Create, Read, Update, Delete and List) APIs, which would then get sent to the dataplane to propagate the config onto the actual Rate Limiting instance

- The data plane provides the primary functionality of Aegis, which handles setting up the right Rate Limiting instance that our customer wants, implement the appropriate algorithm to rate limit request, and drop packets that violates the rate limiting rules or forward them to the backend application. 


### High level design 

1. Where should our rate limiter in our architecture? 





2. How should we identify our clients? 






### Algorithm and Data storage design

This section will cover how Aegis will implement each of the mentioned rate limiting algorithms as well as the design strategy for the database as well. The database and algorithm 
designed are combined into a single section because each of the algorithm has different ways of storing and using the data. 

1. Token Bucket  

2. Leaky Bucket

3. Fixed Window counter

4. Sliding Window counter



## Continuous Integration and Continuous Delivery (CICD/Devops)






## References
<a id="1">[1]</a> 
King, Evan. "Rate Limiter" https://www.hellointerview.com/learn/system-design/problem-breakdowns/distributed-rate-limiter 