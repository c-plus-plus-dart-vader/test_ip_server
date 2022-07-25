#include <iostream>
#include "Server.hpp"

int main(){
	std::cout<<"Specify port\n";
	uint16_t port;
	std::cin>>port;
	
	Server server;
	if (Server::Res_e::SUCCESS != server.Start(port))
	{
		return 0;
	}
	
	return 0;
};
