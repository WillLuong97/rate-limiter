#include <iostream>
#include <boost/program_options.hpp>
#include "rate-limiting-algorithm/token_bucket.h"

namespace po = boost::program_options;

using namespace std;


int main(int argc, char * argv[])
{
   //Initialize the token bucket contents

   // Bucket holds 5 tokens, refills at 2 tokens/second
   TokenBucket bucket(5, 2.0);
   
   //If the current token has been used up, then we block all requests until the token is refilled 
   //and then started again 
   if (bucket.available_tokens() < 0) {
      cout << "Availble tokens have been used up, waiting for refill" << endl;
      bucket.consume_blocking();
   } else {
      cout << "Availble tokens: " << bucket.available_tokens() << " allowing request to go through" << endl;
      bucket.consume();
   }
}