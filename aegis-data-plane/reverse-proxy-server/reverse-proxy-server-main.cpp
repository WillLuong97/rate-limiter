/***
 This class will build out a reverse proxy that would intercept all 
 traffic trying to hit our customer backend. Once all traffics are intercepted, they will be queued up 
 and sent to the Aegis or rate-limiting logic for rate limtiting processing 
*/
#include <iostream> 
#include <string>
#include <boost/program_options.hpp>
#include <vector>

using namespace std;
namespace po = boost::program_options

class ReverseProxyServer { 
    public: 
        

    private:


};

//Todo: Implement the logic to start the reverse proxy server that sends traffic to 
//the customer BE's ip and port

//The server will take in the BE information in 2 ways 
//1 - The BE ip and port number will be passed in as a CLI argument
//This is useful for starting admin or dev team to debug or default a BE 

//2 - The BE ip and port will be passed in as Json file, this is so that we can get
//customer BE dynamically from the Control Plane when the customer configured their rate limiter. This will 
//Implemented programactically, as part of the proxy server start up, we will call an endpoint to grab the config json 
//
int main(int argc, char *argv[]) { 
    //Setting the options from CLI variables
    po::options_description desc("Allowed options");
    desc.add_options()
    ("-help", "display the message with all alllowed options")
    ("backend-ip", "the IP of the Back End host to route traffic to")
    ("backend-port", "the port number for the Back End to host to route traffic to")
    ;

    po::variables_map vm;
    po::store(po::parse_command_line(ac, av, desc), vm);
    po::notify(vm);  

    if (vm.count("help")) {
        cout << desc << "\n";
        return 1;
    }
    else if (vm.count("backend-ip")) {
        std::cout << "Backend IP is set to " << vm["backend-ip"] << std::endl; 
    } 
    else if (vm.count("backend-port")) {
        std::cout << "Backend Port is set to " << vm["backend-port"] << std::endl; 
    } 
    else { 
        std::cout << "Invalid options! Unable to start the server" << std::endl;
    }
    
    //After getting the backend information, we will store them into a local, in memory data structure 
    //So that Aegis can route the data to after the rate limiting process is done 

}