#include <iostream>
#include <boost/program_options.hpp>
#include "rate-limiting-algorithm/token_bucket.h"
#include "rate-limiting-algorithm/rate_limiter.h"
 #include <grpcpp/grpcpp.h>
#include "aegis-server/aegis_grpc_server.h"
#include <iostream>

#include <thread>
namespace po = boost::program_options;

using namespace std;


bool runAegisEngine(RateLimiter& limiter) {

   // If there is enough token, the request will be allowed through, otherwise, we will not allow it to go through
   if (limiter.available() <= 0) {
      cout << "No available tokens to complete the request " << endl;
      return false;
   } else {
      cout << "Available tokens: " << limiter.available() << " allowing request to through" << endl;
      return limiter.consume();   
   };
}

int main(int argc, char * argv[])
{
   std::cout << std::unitbuf;   // force cout to flush after every insertion

   //todo: If you want genuine multi-seed startup resilience later, 
   //that requires the other RedisCluster overload (ConnectionOptions/ClusterOptions),
  const std::string cluster_nodes = "tcp://172.20.0.11:6379"; 

   RateLimitServiceImpl service(
      100,    // capacity:    100 tokens per IP
      10,      // refill_rate: 10 tokens per second
      cluster_nodes
   ); 
   

   //Building and starting a gRPC server to wait for Envoy gRPC rate limting request
   grpc::ServerBuilder builder; 
   builder.AddListeningPort(
      "0.0.0.0:8081", 
      grpc::InsecureServerCredentials() //Todo: For production deployment, replace this with grpc::SslServerCredentials() for a more secure credential with mtls. 
   );
   builder.RegisterService(&service);

   unique_ptr<grpc::Server> server(builder.BuildAndStart()); 
   cout << "[aegis-main] Aegis Engine listening on :8081\n"; 
   server->Wait(); 
   return 0; 
}