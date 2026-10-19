#include <iostream>
#include <boost/program_options.hpp>
#include "rate-limiting-algorithm/token_bucket.h"
#include "rate-limiting-algorithm/rate_limiter.h"
 #include <grpcpp/grpcpp.h>
#include "aegis-server/aegis_grpc_server.h"
#include <iostream>
#include "providers/config-providers.h"

#include <thread>
namespace po = boost::program_options;

using namespace std;

int main(int argc, char * argv[])
{

   ConfigProvider configProvider;  
   configProvider.warm_up(); 

   std::cout << std::unitbuf;   // force cout to flush after every insertion

   //todo: If you want genuine multi-seed startup resilience later, 
   //that requires the other RedisCluster overload (ConnectionOptions/ClusterOptions),
  const std::string cluster_nodes = "tcp://172.20.0.11:6379"; 

   RateLimitServiceImpl service(
      configProvider,
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