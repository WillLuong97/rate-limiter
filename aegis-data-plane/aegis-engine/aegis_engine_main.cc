#include <chrono>
#include <cstdlib>      
#include <iostream>
#include <memory>
#include <grpcpp/grpcpp.h>
#include "aegis-server/aegis_grpc_server.h"
#include "providers/config-providers.h"
#include <optional>
#include <string> 
#include <sw/redis++/redis++.h>

using namespace std;

//helper function to read in any environment variable passed in from docker 
//Note: an anonymous namespace makes this function visible only inside this .cpp file
namespace {
   std::optional<std::string> read_docker_env(const char* docker_env_name) {
      const char* env_value = std::getenv(docker_env_name); //getenv will return a c-style string, which is a pointer to the first char of the string 
      
      if (env_value == nullptr || *env_value == '\0') {
         return std::nullopt; 
      } 
   
      return std::string(env_value); 
   }
}


int main(int argc, char * argv[])
{

   ConfigProvider configProvider;  
   configProvider.warm_up(); 

   std::cout << std::unitbuf;   // force cout to flush after every insertion   

   //redis hostname and port number are configured from the docker environment variable
   std::optional<std::string> redis_hostname = read_docker_env("AEGIS_REDIS_URI_HOST"); 
   std::optional<std::string> redis_port = read_docker_env("AEGIS_REDIS_URI_PORT"); 

   if (!redis_hostname || !redis_port) {
      cerr << "[aegis-main] AEGIS_REDIS_URI_HOST or AEGIS_REDIS_URI_PORT are not set" << "\n"; 
      return EXIT_FAILURE; 
   }

   //setting connection options to redis cluster
   sw::redis::ConnectionOptions redis_connection_opts; 
   redis_connection_opts.host = *redis_hostname; 

   try {
      redis_connection_opts.port = std::stoi(*redis_port); 
   } catch (const std::exception&){
      cerr << "[aegis-main] Aegis Redis port number is not a valid number " <<  *redis_port << " unable to start up redis" << "\n"; 
      return EXIT_FAILURE; 
   }

   // Without these, a Redis node that stops answering blocks a request until
   // the OS gives up (~70s). With them, a call fails fast and we fail open.
   redis_connection_opts.connect_timeout = std::chrono::milliseconds(100);
   redis_connection_opts.socket_timeout = std::chrono::milliseconds(100); 
   

   //setting up connection pools over the Redis master. gRPC threads to Redis share these connections pool 
   sw::redis::ConnectionPoolOptions redis_connection_pool_opts; 
   redis_connection_pool_opts.size = 16; //pool size of 16 
   redis_connection_pool_opts.wait_timeout = std::chrono::milliseconds(50);

   //Setting up redis cluster connection: 
   std::unique_ptr<sw::redis::RedisCluster> redisCluster; 
   try {
      redisCluster = std::make_unique<sw::redis::RedisCluster>(redis_connection_opts, redis_connection_pool_opts);
   } catch (const sw::redis::Error& e) {
      cerr << "[aegis-main] Unable to connect to redis cluster at " 
            << *redis_hostname << ":" << *redis_port << ": " << e.what() << "\n"; 
      return EXIT_FAILURE; 
   }

   RateLimitServiceImpl service(
      configProvider,
      *redisCluster
   ); 
   

   //Building and starting a gRPC server to wait for Envoy gRPC rate limting request
   grpc::ServerBuilder builder; 
   builder.AddListeningPort(
      "0.0.0.0:8081", 
      grpc::InsecureServerCredentials() //Todo: For production deployment, replace this with grpc::SslServerCredentials() for a more secure credential with mtls. 
   );
   builder.RegisterService(&service);

   unique_ptr<grpc::Server> server(builder.BuildAndStart());

   //If the server is not able to start, then we would get a nullpointer back and crash the program
   //if so, log the error and exit the program
   if (!server) {
      cerr << "[aegis-main] Unable to start up gRPC server on port :8081 (port in use?) \n"; 
      return EXIT_FAILURE; 
   }

   cout << "[aegis-main] Aegis Engine listening on :8081\n"; 
   server->Wait(); 
   return 0; 
}