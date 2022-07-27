#include <iostream>
#include <string>
#include <map>
#include <set>
#include <charconv>
#include <vector>
#include <cstring>
#include <cctype>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>
#include <algorithm>
#include <sys/epoll.h>
#include <fcntl.h>
#include "Server.hpp"

using namespace std;

constexpr string_view UDP_PACKET_BEGIN {"proteyclient"};

constexpr uint16_t TCP_READ_BUFFER_SIZE   = 25;
constexpr uint8_t  MAX_IPv4_SIZE          = 15;
constexpr uint16_t MAX_UDP_PACKET_SIZE    = 512;
constexpr uint8_t  UDP_PH_QTY_POS         = UDP_PACKET_BEGIN.size();
constexpr uint8_t  UDP_PH_SEQ_NUM_POS     = UDP_PH_QTY_POS + 2;
constexpr uint8_t  UDP_PH_SIZE_POS        = UDP_PH_SEQ_NUM_POS + 2;
constexpr uint8_t  UDP_PACKET_HEADER_SIZE = UDP_PH_SIZE_POS + 2;

void input_handling(string& in, bool IsTcp)
{
	vector<size_t> numbers;
	auto end_it = end(in);
	auto num_beg = end_it;//number beginning is unknown initially
	for(auto it = begin(in); ; ++it)
	{
		if (end_it == num_beg)
		{
			if (isdigit(*it)) num_beg = it;
		}
		else
		{
			if ((end_it == it) or not isdigit(*it))
			{
				auto& el = numbers.emplace_back();
				from_chars(&*num_beg, &*it, el);
				num_beg = end_it;
			}
		}
		
		if (end_it == it) break;
	}
	
	if (not numbers.empty())
	{
		sort(begin(numbers), end(numbers));
		
		in.clear();
		size_t sum = 0;
		for (auto const& num : numbers){ in+=to_string(num)+=" "; sum+=num; }
		if (IsTcp) { in.back()='\t'; }
		else { in.back()='\n'; }
		in+=to_string(sum);
	}
}

struct udp_message_info
{
	char ip[MAX_IPv4_SIZE + 1] = {'\0'};
	uint16_t port;
	mutable uint16_t packets_qty;
	mutable uint16_t packet_size;
	mutable vector<uint16_t> packet_sn_list;
	mutable string msg;
	
	bool operator<(udp_message_info const& other) const
	{
		string_view ip_sv{ip};
		string_view other_ip_sv{other.ip};
		return ip_sv != other_ip_sv ? (ip_sv < other_ip_sv) : (port < other.port);
	}
};

int set_nonblocking(int sockfd)
{
	int flags = fcntl(sockfd, F_GETFL, 0);
	if (-1 == flags) {
		return -1;
	}
	if (-1 == fcntl(sockfd, F_SETFL, flags | O_NONBLOCK)) {
		return -1;
	}
	return 0;
}
	
Server::~Server()
{
	cout<<"Server DTOR\n";
	if (-1 != m_tcp_desc) close(m_tcp_desc);
	if (-1 != m_udp_desc) close(m_udp_desc);
}

void Server::Stop()
{
	m_mtx.lock();
	m_stop = true;
	m_mtx.unlock();
}
	
Server::Res_e Server::Start(uint16_t port, int ms_timeout)
{
	m_tcp_desc = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (-1 == m_tcp_desc)
	{
		cout<<"Creation of an unbound TCP socket and get file descriptor failed: "<<strerror(errno)<<"\n";
		if ((errno == ENFILE)or(errno == EMFILE)or(errno == ENOBUFS)or(errno == ENOMEM))
		{
			cout<<"You can try later\n";
			return Res_e::TEMPORARY_UNSUFFICIENT_RESOURCES;
		}
		return Res_e::FAILURE;
	}
	cout<<"TCP unbound socket "<<m_tcp_desc<<" is created\n";
	
	m_udp_desc = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (-1 == m_udp_desc)
	{
		cout<<"Creation of an unbound UDP socket and get file descriptor failed: "<<strerror(errno)<<"\n";
		close(m_tcp_desc);
		m_tcp_desc = -1;
		if ((errno == ENFILE)or(errno == EMFILE)or(errno == ENOBUFS)or(errno == ENOMEM))
		{
			cout<<"You can try later\n";
			return Res_e::TEMPORARY_UNSUFFICIENT_RESOURCES;
		}
		return Res_e::FAILURE;
	}
	cout<<"UDP unbound socket "<<m_udp_desc<<" is created\n";
	
	memset(&m_server_sa, 0, sizeof(m_server_sa));
	m_server_sa.sin_family = AF_INET;
	m_server_sa.sin_addr.s_addr = htonl(INADDR_ANY);
	m_server_sa.sin_port = htons(port);
	
	if (-1 == bind(m_tcp_desc, (sockaddr const*)&m_server_sa, sizeof(m_server_sa)) or
	-1 == bind(m_udp_desc, (sockaddr const*)&m_server_sa, sizeof(m_server_sa)))
	{
		close(m_tcp_desc);
		m_tcp_desc = -1;
		close(m_udp_desc);
		m_udp_desc = -1;
		return Res_e::FAILURE;
	}
	
	if (-1 == listen(m_tcp_desc, 5))
	{
		close(m_tcp_desc);
		m_tcp_desc = -1;
		close(m_udp_desc);
		m_udp_desc = -1;
		return Res_e::FAILURE;
	}
	
	auto const epfd = epoll_create(10);
	if (-1 == epfd)
	{
		return Res_e::FAILURE;
	}	
	
	epoll_event tcp_ev;
	tcp_ev.events = EPOLLIN;
	tcp_ev.data.fd = m_tcp_desc;
	if (-1 == epoll_ctl(epfd, EPOLL_CTL_ADD, m_tcp_desc, &tcp_ev))
	{
		return Res_e::FAILURE;
	}
	
	epoll_event udp_ev;
	udp_ev.events = EPOLLIN;
	udp_ev.data.fd = m_udp_desc;
	if (-1 == epoll_ctl(epfd, EPOLL_CTL_ADD, m_udp_desc, &udp_ev))
	{
		return Res_e::FAILURE;
	}
	//first field in pair is position in message for write next part of message
	map<int, string> tcp_messages;
	set<udp_message_info> udp_messages;
	
	constexpr size_t MAX_EVENTS = 10;
	epoll_event events[MAX_EVENTS];
	epoll_event event_connected_sd;
	for(;;)
	{
		int const nfds = epoll_wait(epfd, events, MAX_EVENTS, ms_timeout);
		if (-1 == nfds)
		{
			cout<<"epoll_wait failed: "<<strerror(errno)<<"\n";
			return Res_e::FAILURE;	
		}
		if (0 == nfds)
		{
			cout<<"epoll_wait timeout\n";
			break;
		}
		else
		{
			m_mtx.lock();
			if (m_stop)
			{
				m_stop = false;
				m_mtx.unlock();
				break;
			}
			m_mtx.unlock();
		}
		
		for (int i = 0; i < nfds; ++i)
		{
			if (EPOLLIN&events[i].events)
			{
				cout<<"EPOLLIN Event for "<<(events[i].data.fd != m_udp_desc ? "TCP" : "UDP")<<" socket "<<events[i].data.fd<<"\n";
				if (events[i].data.fd == m_tcp_desc)
				{
					sockaddr_in from;
					socklen_t from_len = sizeof(from);
					memset(&from, 0, from_len);
					int conn_desc = accept(m_tcp_desc, (sockaddr*)&from, &from_len);
					if (-1 == conn_desc)
					{
						cout<<"accept failed: "<<strerror(errno)<<"\n";
						return Res_e::FAILURE;	
					}
					char from_ip[MAX_IPv4_SIZE + 1];
					inet_ntop(AF_INET, &from.sin_addr, from_ip, sizeof(from_ip));
					auto const from_port = ntohs(from.sin_port);
					cout<<"TCP connection socket "<<conn_desc<<" is created for TCP client "<<from_ip<<":"<<from_port<<"\n";
					if (-1 == set_nonblocking(conn_desc))
					{
						cout<<"Set NON BLOCKING mode for socket "<<conn_desc<<" failed: "<<strerror(errno)<<"\n";
						close(conn_desc);
						return Res_e::FAILURE;	
					}
					event_connected_sd.data.fd = conn_desc;
					event_connected_sd.events = EPOLLIN | EPOLLOUT | EPOLLHUP | EPOLLRDHUP | EPOLLET;
					if (-1 == epoll_ctl(epfd, EPOLL_CTL_ADD, conn_desc, &event_connected_sd))
					{
						cout<<"epoll_ctl for socket "<<conn_desc<<" failed: "<<strerror(errno)<<"\n";
						close(conn_desc);
						return Res_e::FAILURE;	
					}
				}
				else if (events[i].data.fd == m_udp_desc)
				{
					char buffer[MAX_UDP_PACKET_SIZE];
					sockaddr_in from;
					socklen_t from_len = sizeof(from);
					size_t read_num = recvfrom(m_udp_desc, buffer, MAX_UDP_PACKET_SIZE, 0, (sockaddr*)&from, &from_len);
					if (read_num > 0)
					{
						if (read_num < (UDP_PACKET_HEADER_SIZE + 1))
						{
							cout<<"Packet is not from protey client\n";
							continue;
						}
						if (string_view{buffer, UDP_PACKET_BEGIN.size()} != UDP_PACKET_BEGIN)
						{
							cout<<"Packet is not from protey client\n";
							continue;
						}
						udp_message_info mi;
						inet_ntop(AF_INET, &from.sin_addr, mi.ip, sizeof(mi.ip));
						mi.port = ntohs(from.sin_port);
						cout<<"Packet received from client "<<mi.ip<<":"<<mi.port<<"\n";
						auto [it, res] = udp_messages.emplace(mi);
						if (res)
						{
							it->packets_qty = ((uint16_t)buffer[UDP_PH_QTY_POS]<<8)|(uint16_t)buffer[UDP_PH_QTY_POS + 1];
							cout<<"Packet qty is "<<it->packets_qty<<"\n";
							it->packet_size = ((uint16_t)buffer[UDP_PH_SIZE_POS]<<8)|(uint16_t)buffer[UDP_PH_SIZE_POS + 1];
							cout<<"Packet size is "<<it->packet_size<<"\n";
							it->msg.resize(it->packets_qty * (it->packet_size - UDP_PACKET_HEADER_SIZE));
						}
						uint16_t packet_number = ((uint16_t)buffer[UDP_PH_SEQ_NUM_POS]<<8)|(uint16_t)buffer[UDP_PH_SEQ_NUM_POS + 1];
						cout<<"Packet number is "<<packet_number<<"\n";

						if (find(it->packet_sn_list.begin(), it->packet_sn_list.end(), packet_number) != it->packet_sn_list.end())
						{
							cout<<"Duplicated packet\n";
							continue;
						}
						it->packet_sn_list.push_back(packet_number);
						if (packet_number == (it->packets_qty - 1))
						{
							cout<<"Last packet of message is received\n";
							it->msg.resize((it->packets_qty - 1)*(it->packet_size - UDP_PACKET_HEADER_SIZE) + read_num - UDP_PACKET_HEADER_SIZE);
						}
						
						memcpy(&it->msg[packet_number*(it->packet_size - UDP_PACKET_HEADER_SIZE)], &buffer[UDP_PACKET_HEADER_SIZE], read_num - UDP_PACKET_HEADER_SIZE);
						
						if (it->packets_qty == it->packet_sn_list.size())
						{
							cout<<"All packets of message are received\n";
							input_handling(it->msg, false);
							uint16_t packet_qty = it->msg.size()/(it->packet_size - UDP_PACKET_HEADER_SIZE);
							uint16_t ost = it->msg.size()%(it->packet_size - UDP_PACKET_HEADER_SIZE);
							if (ost > 0){ ++packet_qty; }
							cout<<"PACKETS_QTY for response is "<<packet_qty<<"\n";
							
							for (size_t i = 0; i < packet_qty; ++i)
							{
								char wbuff[it->packet_size];
								memcpy(wbuff, UDP_PACKET_BEGIN.data(), UDP_PACKET_BEGIN.size());
								wbuff[UDP_PH_QTY_POS] = (packet_qty&0xff00)>>8;
								wbuff[UDP_PH_QTY_POS + 1] = packet_qty&0xff;
								
								wbuff[UDP_PH_SEQ_NUM_POS] = (i&0xff00)>>8;
								wbuff[UDP_PH_SEQ_NUM_POS + 1] = i&0xff;
								
								wbuff[UDP_PH_SIZE_POS] = (it->packet_size&0xff00)>>8;
								wbuff[UDP_PH_SIZE_POS + 1] = it->packet_size&0xff;
								
								size_t payload_len;
								if ((packet_qty - 1) == i and ost > 0) payload_len = ost;
								else payload_len = it->packet_size - UDP_PACKET_HEADER_SIZE;
			
								memcpy(&wbuff[UDP_PACKET_HEADER_SIZE], &it->msg[i*(it->packet_size - UDP_PACKET_HEADER_SIZE)], payload_len);
								
								sendto(m_udp_desc, wbuff, UDP_PACKET_HEADER_SIZE + payload_len, 0, (sockaddr const*)&from, from_len);
							}
							cout<<"Response is entirely sent\n";
							udp_messages.erase(it);
						}
					}	

				}	
				else
				{
					char buffer[TCP_READ_BUFFER_SIZE];
					
					auto [it, is_new] = tcp_messages.try_emplace(events[i].data.fd);
					auto& msg = it->second;
					
					int read_num;
					do
					{
						read_num = read(events[i].data.fd, buffer, TCP_READ_BUFFER_SIZE);
						cout<<"Read "<<read_num<<" bytes\n";
						
						if (read_num == -1)
						{
							if (errno == EINTR)
								cout<<"EINTR\n";
								continue;
							if (errno == EAGAIN)
								cout<<"EAGAIN. Wait END symbol('\n') message later\n";
								break;
							
							cout<<"Read finished with ERROR: "<<strerror(errno)<<"\n";
							msg.clear();
							break;
						}
						else if (read_num == 0)
						{
							cout<<"Client disconnected for socket "<<events[i].data.fd<<"\n";
							tcp_messages.erase(events[i].data.fd);
							close(events[i].data.fd);							
						}
						else if (read_num > 0)
						{
							msg.append(buffer, read_num);
							//cout<<"Message buffer:"<<msg.c_str();
							if (msg.back() == '\n')
							{
								cout<<"Message END symbol is detected\n";
								//actions with back() symbol because input_handling function is used both TCP and UDP
								//And '\n' symbol is used as message end only in TCP
								msg.pop_back();
								input_handling(msg, true);
								msg.push_back('\n');
								
								size_t start_pos = 0;
								while(1)
								{
									int written_bytes = write(events[i].data.fd, msg.data() + start_pos, msg.size() - start_pos);
									if (written_bytes >= 0)
									{
										cout<<"Send "<<written_bytes<<" bytes in response\n";
										if (written_bytes != msg.size() - start_pos)
										{
											start_pos += written_bytes;
										}
										else
										{
											msg.clear();
											cout<<"Response is sent entirely\n";
											break;
										}
									}
									else
									{
										cout<<"Write failed: "<<strerror(errno)<<"\n";;
										msg.clear();
										break;
									}
								}
							}
							else cout<<"\n";
						}
					}while(TCP_READ_BUFFER_SIZE == read_num);
				}
			}
			else if ((EPOLLRDHUP | EPOLLHUP)&events[i].events)
			{
				cout<<"Client disconnected for socket "<<events[i].data.fd<<"\n";
				tcp_messages.erase(events[i].data.fd);
				close(events[i].data.fd);
			}
		}
	}
	
	for (auto const& [desc, msg] : tcp_messages){ close(desc); }
	tcp_messages.clear();
	
	close(m_tcp_desc);
	m_tcp_desc = -1;
	close(m_udp_desc);
	m_udp_desc = -1;
	
	return Res_e::SUCCESS;
}

