#include <iostream>
#include <boost/program_options.hpp>
#include "rate-limiting-algorithm/token_bucket.h"
#include "rate-limiting-algorithm/rate_limiter.h"
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
   // swap any of these in and process_requests() works identically
   std::unique_ptr<RateLimiter> limiter;

   //Init the bucket, 5 buckets at full capacity and refil them 1 token per second
   limiter = std::make_unique<TokenBucket>(5, 1);
   
   //Initialize the token bucket
   for (int request = 0; request <= 100; request++) {
      if (request % 2 == 0) {
         //Wait for 1 a second for every requests that is even so that we can 
         //keep the rate limiter a chance to refil the bucket
         std::this_thread::sleep_for(std::chrono::seconds(1));
      }
      if (runAegisEngine(*limiter)) {
         cout << "Request ID: " << request << " is forwarded!" << endl; 
      } else {
         cout << "Request ID: " << request << " has been dropped!" << endl; 
      }
   }; 
}