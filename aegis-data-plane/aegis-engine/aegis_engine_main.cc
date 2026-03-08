#include <iostream>
#include <boost/program_options.hpp>
#include "rate-limiting-algorithm/token_bucket.h"
#include <thread>
namespace po = boost::program_options;

using namespace std;


bool runAegisEngine(TokenBucket *bucket) {

   // If there is enough token, the request will be allowed through, otherwise, we will not allow it to go through
   if (bucket->available_tokens() <= 0) {
      cout << "No available tokens to complete the request " << endl;
      return false;
   } else {
      cout << "Available tokens: " << bucket->available_tokens() << " allowing request to through" << endl;
      return bucket->consume();
   };
}

int main(int argc, char * argv[])
{
   //Init the bucket, 5 bucket at full capacity and refil them 1 token per second
   TokenBucket bucket(5, 1);
   //Initialize the token bucket
   for (int request = 0; request <= 100; request++) {
      if (request % 2 == 0) {
         //Wait for 1 a second for every requests that is even so that we can 
         //keep the rate limiter a chance to refil the bucket
         std::this_thread::sleep_for(std::chrono::seconds(1));
      }
      if (runAegisEngine(&bucket)) {
         cout << "Request ID: " << request << " is forwarded!" << endl; 
      } else {
         cout << "Request ID: " << request << " has been dropped!" << endl; 
      }
   }; 
}