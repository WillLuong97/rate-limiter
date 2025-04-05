#include <string>
#include <iostream>
#include <map> 

using namespace std; 
class BackendConfig {
    public: 
    void AddOrUpdateToBackendConfigMap(string backend_id, map<string, string> backend_config); 

    pair<string, string> GetBackendConfig(string backend_id); 

    void RemoveBackendConfig(string ip);
    
    inline size_t Size() {return backend_config_map.size(); }

    private:
    map<string , pair<string, string>> backend_config_map; 
};