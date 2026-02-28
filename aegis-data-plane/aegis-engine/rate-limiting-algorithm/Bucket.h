#include <iostream> 
#include <string> 
using namespace std; 

//This bucket data structure will then be written into the redis cache
class Bucket 
{
    public:
    //Constructor: 
    Bucket(uint64_t time_stamp_, string token_) 
    {
       time_stamp = time_stamp_; 
       token = token_;
    }  

    //Getter
    uint64_t getTimeStamp() const { return time_stamp; }
    string getToken() const { return token; } 

    //Setter 
    void setTimeStamp(uint64_t time_stamp_) { time_stamp = time_stamp_; }
    void setToken(string token_) { token = token_; }

    private:
    uint64_t time_stamp; 
    string token;  
};