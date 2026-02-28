#include <iostream>
#include <boost/program_options.hpp>

namespace po = boost::program_options;

using namespace std;
#include <boost/redis/src.hpp>   

int main(int argc, char * argv[])
{
   auto conn = std::make_shared<connection>(co_await net::this_coro::executor);
   conn->async_run(cfg, {}, net::consign(net::detached, conn));

   // A request containing only a ping command.
   request req;
   req.push("PING", "Hello world");

   // Response object.
   response<std::string> resp;

   // Executes the request.
   co_await conn->async_exec(req, resp);
   conn->cancel();

   std::cout << "PING: " << std::get<0>(resp).value() << std::endl;
}