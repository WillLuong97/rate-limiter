#include <string>
#include <iostream>
#include <map>
#include <BackendConfig.h>

void 
BackendConfig::
AddOrUpdateToBackendConfigMap(string backend_id, map<string, uint16_t> backend_config) {
    //Check if the backend is already stored in the map, if true, then update, otherwise add new entry 
    if (backend_config_map.find(backend_id) != backend_config_map.end()) { 
        //not found! Inserting a new BE config entry
        backend_config_map.emplace(backend_id, backend_config);
    } else {
        //Otherwise, update the current config by adding a new key into the map and delete the older ones
        map<string, uint16_t> backend_config_to_be_updated = backend_config_map[backend_id];
        backend_config_to_be_updated.begin()->first = backend_config.begin()->first; 
        backend_config_to_be_updated.begin()->second = backend_config.begin()->second; 
    }    
}

pair<string, string>
BackendConfig::GetBackendConfig(string backend_id) { 

}

void 
BackendConfig::RemoveBackendConfig(string backend_id) {

}
