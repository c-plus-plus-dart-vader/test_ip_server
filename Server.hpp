#pragma once

#include <arpa/inet.h>
#include <mutex>

class Server
{
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
	Res_e Start(uint16_t port, int ms_timeout = -1);
	void Stop();
private:
	std::mutex  m_mtx;
	bool        m_stop{false};
	int         m_tcp_desc{-1};
	int         m_udp_desc{-1};
	sockaddr_in m_server_sa;
};