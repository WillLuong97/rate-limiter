#include <string>
#include <iostream>
#include <map> 

/***
 * 
 * This data structure will store the client backend configuration
 * that we want our reverse proxy to intercept and route traffic to
 * 
 * The data structure is a key-value pair with the key being the id of the backend and the value 
 * being another key-value pair with its key is the ip address and value being the port number of the 
 * backend we want to route traffic to
 * **/
using namespace std; 
class BackendConfig {
    public: 
    void AddOrUpdateToBackendConfigMap(string backend_id, map<string, uint16_t> backend_config); 

    map<string, string> GetBackendConfig(string backend_id); 

    void RemoveBackendConfig(string ip);
    
    inline size_t Size() {return backend_config_map.size(); }

    private:
    map<string , map<string, uint16_t>> backend_config_map; 
}; 

class BackendConfigMap { 
    public: 
    string backend_id; 
    string ip_address; 
    uint16_t port_number; 
}; 