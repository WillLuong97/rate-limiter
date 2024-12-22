# Aegis

Rate limiting is a strategy for limiting the network traffic accessing an application server. It helps to prevent attacks that aims at overloading a system or service by putting a cap on how often a client can repeat a request to the server, i.e logging into the services or requesting a resource in a given time. Rate limiter is the best approach to prevent distributed denial-of-service (DDoS). 

A recent stress testing event, which simulates a DDoS attack, on some of our internal applications such as the [Covid19 tracker](https://github.com/WillLuong97/rabbitmqclustermessageprovider), [RabbitMQ cluster](https://github.com/WillLuong97/rabbitmqclustermessageprovider), [LRU Cache](https://github.com/WillLuong97/LRU-Cache-Implementation) has reported a significant amount of latency in the API performance with some even crashes the application entirely 
due to the server being overloaded with so many requests exceeding its threshold. 

To migitate the issue, we tried to restart the server and even scaled up the applications by adding more instances. However, due to the overwhelming requests that also come in at a high speed, manual mitigations becomes too time comsuming, and most importantly, too expensive due to the additional cost of adding more server instances. 

Therefore, project Aegis is created to provide a Rate limiting solution to help protect our application by automatically detecting and enforcing a cap on a client number of request in a given time frame. 

## Requirements

- Aegis's rate limiting logic works by dropping all requests coming from a client that has exceeded the allowed number of requests in a given time window. If the number of requests have not exceeded, the request will then be routed to the original application backend that was intended for these request.

- The rate limiting strategy will be based off the client's IP address (both IPV4 and IPV6), the number of application backend servers and each indvidual server's health.

- Aegis will sit in between the application backend server and the client, similar to a proxy service, but it will only handle dropping or forwarding the incoming traffic from the clients to the server. The reverse traffic from the server back to the client is out-of-scope.

- Aegis will not send back any response from the server to the customer as Aegis main goal is to protect the application backend from DDoS attack and forwarding the traffic. 

- Aegis will support all major rate limiting algorithms, such as Token Bucket, Leaky Bucket, Fixed Window counter, Sliding Window counter to give more options on how certain applications can choose to rate limit themselves.

- Similar to the algorithms, the number of requests allowed and the time windows are configurable to our depending services as they are  

- Aegis must maintain a highly available position to provide a full coverage on our depdending services backend. 

### Aegis Architecture Diagram
![Alt text](<media/Aegis architecture diagram.png>)


## Implementation 

- Aegis will have 2 main components, the Control plane and the Data plane. 
- The control plane provdes a platform for our depdending services to configure the behavior of their Rate Limiting instances via a set of CRUDL (Create, Read, Update, Delete and List) APIs, which would then get sent to the dataplane to propagate the config onto the actual Rate Limiting instance

- The data plane provides the primary functionality of Aegis, which handles setting up the right Rate Limiting instance that our customer wants, implement the appropriate algorithm to rate limit request, and drop packets that violates the rate limiting rules or forward them to the backend application. 

### Architecture diagram 



### Algorithm and Data storage design

This section will cover how Aegis will implement each of the mentioned rate limiting algorithms as well as the design strategy for the database as well. The database and algorithm 
designed are combined into a single section because each of the algorithm has different ways of storing and using the data. 

1. Token Bucket  

2. Leaky Bucket

3. Fixed Window counter

4. Sliding Window counter