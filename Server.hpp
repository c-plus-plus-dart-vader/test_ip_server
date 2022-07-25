#pragma once

#include <arpa/inet.h>

class Server
{
	static constexpr uint8_t MAX_ATTEMPTS_NUM = 3;
public:
	enum class Res_e : uint8_t
	{
		INPUT_ATTEMPTS_NUMBER_EXCEEDED,
		TEMPORARY_UNSUFFICIENT_RESOURCES,
		ALREADY_STARTED,
		FAILURE,
		SUCCESS
	};
	~Server();
	Res_e Start(uint16_t port);
private:
	int         m_tcp_desc{-1};
	int         m_udp_desc{-1};
	sockaddr_in m_server_sa;
	bool        m_is_started{false};
};