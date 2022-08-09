#include <iostream>
#include <string>
#include <map>
#include <set>
#include <list>
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
#include "Logger.hpp"

using namespace std;

template<class ... Args>
void Log(Args ... args)
{
	utils::Log("Server.cpp: ", args...);
}

constexpr string_view UDP_PACKET_BEGIN {"proteyclient"};

constexpr size_t TCP_READ_BUFFER_SIZE     = 20;
constexpr uint8_t  MAX_IPv4_SIZE          = 15;
constexpr uint16_t MAX_UDP_PACKET_SIZE    = 512;
constexpr uint16_t UDP_PH_QTY_POS         = UDP_PACKET_BEGIN.size();
constexpr uint16_t UDP_PH_SEQ_NUM_POS     = UDP_PH_QTY_POS + 2;
constexpr uint16_t UDP_PH_SIZE_POS        = UDP_PH_SEQ_NUM_POS + 2;
constexpr uint16_t UDP_PACKET_HEADER_SIZE = UDP_PH_SIZE_POS + 2;

void input_handler(string& in, bool IsTcp)
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
	char                   ip[MAX_IPv4_SIZE + 1] = {'\0'};
	uint16_t               port;
	uint16_t               packet_size;
	mutable uint16_t       packets_qty;
	mutable list<uint16_t> packet_sn_list;
	mutable string         msg;
	mutable bool           is_rsp_in_progress;
	
	bool operator<(udp_message_info const& other) const
	{
		string_view ip_sv{ip};
		string_view other_ip_sv{other.ip};
		return ip_sv != other_ip_sv ? (ip_sv < other_ip_sv) : (port < other.port);
	}
};

struct tcp_message_info
{
	size_t rsp_start_pos;
	string msg;
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

using tcp_msg_storage_t = map<int, tcp_message_info>;

bool tcp_write_handler(int& tcp_desc, tcp_message_info& msg_info, tcp_msg_storage_t& storage, size_t start_pos)
{
	while(1)
	{
		int written_bytes = send(tcp_desc, msg_info.msg.data() + start_pos, msg_info.msg.size() - start_pos, 0);
		if (written_bytes >= 0)
		{
			Log("Send ", written_bytes, " bytes in response");
			start_pos += written_bytes;
			if (start_pos == msg_info.msg.size()) {
				msg_info.msg.clear();
				msg_info.rsp_start_pos = 0;
				Log("Response is entirely sent by TCP socket ", tcp_desc);
				return true;
			}
		}
		else//-1 case
		{
			if (EAGAIN == errno || EWOULDBLOCK == errno) {
				Log("EAGAIN or EWOULDBLOCK while writing to TCP socket ", tcp_desc); 
				msg_info.rsp_start_pos = start_pos;
				return true;
			}	
			if (EINTR  == errno) {
				Log("EINTR");
				continue;
			}
			if (ECONNRESET == errno) {
				Log("TCP client disconnected for socket ", tcp_desc);
				storage.erase(tcp_desc);
				close(tcp_desc);
				tcp_desc = -1;
				return false;
			}
			Log("Write failed: ", strerror(errno));
			msg_info.msg.clear();
			msg_info.rsp_start_pos = 0;
			return false;
		}
	}	
}

using udp_storage_t = set<udp_message_info>;

//return is_rsp_in_progress
bool udp_write_handler(
int udp_desc, 
udp_message_info const& msg_info,
uint16_t ost,
sockaddr_in const& from,
socklen_t const& from_len)
{
	for (auto packet_it = msg_info.packet_sn_list.begin(); packet_it != msg_info.packet_sn_list.end(); )
	{
		char wbuff[MAX_UDP_PACKET_SIZE];
		memcpy(wbuff, UDP_PACKET_BEGIN.data(), UDP_PACKET_BEGIN.size());
		uint16_t packets_qty_net = htons(msg_info.packets_qty);
		memcpy(wbuff + UDP_PH_QTY_POS, &packets_qty_net, sizeof(packets_qty_net));
		uint16_t seq_num_net = htons(*packet_it);
		memcpy(wbuff + UDP_PH_SEQ_NUM_POS, &seq_num_net, sizeof(seq_num_net));
		uint16_t ps_net = htons(msg_info.packet_size);
		memcpy(wbuff + UDP_PH_SIZE_POS, &ps_net, sizeof(ps_net));
		
		size_t payload_len;
		if ((msg_info.packets_qty - 1) == (*packet_it) and ost > 0) payload_len = ost;
		else payload_len = msg_info.packet_size - UDP_PACKET_HEADER_SIZE;

		memcpy(&wbuff[UDP_PACKET_HEADER_SIZE], &msg_info.msg[(*packet_it)*(msg_info.packet_size - UDP_PACKET_HEADER_SIZE)], payload_len);
		
		int written_bytes = sendto(udp_desc, wbuff, UDP_PACKET_HEADER_SIZE + payload_len, 0, (sockaddr const*)&from, from_len);
		if (written_bytes > 0)
		{
			Log("Packet ", *packet_it, " of response for client ", msg_info.ip, ":", msg_info.port, " is sent");
			packet_it = msg_info.packet_sn_list.erase(packet_it);
		}
		else
		{
			if (0 == written_bytes || errno == EAGAIN || errno == EWOULDBLOCK) {
				Log("EAGAIN or EWOULDBLOCK or no bytes are sent while responding by UDP");
				if (not msg_info.is_rsp_in_progress) {
					msg_info.is_rsp_in_progress = true;
				}
				return true;
			}
			else if (errno == EINTR) {
				Log("EINTR while responding");
				continue;
			}
			else {
				Log("Response sending failed");
				return false;
			}
		}
	}
	
	Log("Response for client ", msg_info.ip, ":", msg_info.port, " is entirely sent");
	return true;
}
	
Server::~Server()
{
	Log("Server DTOR");
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
		Log("Creation of an unbound TCP socket and get file descriptor failed: ", strerror(errno));
		if ((errno == ENFILE)or(errno == EMFILE)or(errno == ENOBUFS)or(errno == ENOMEM))
		{
			Log("You can try later");
			return Res_e::TEMPORARY_UNSUFFICIENT_RESOURCES;
		}
		return Res_e::FAILURE;
	}
	Log("TCP unbound socket ", m_tcp_desc, " is created");
	
	m_udp_desc = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (-1 == m_udp_desc)
	{
		Log("Creation of an unbound UDP socket and get file descriptor failed: ", strerror(errno));
		close(m_tcp_desc);
		m_tcp_desc = -1;
		if ((errno == ENFILE)or(errno == EMFILE)or(errno == ENOBUFS)or(errno == ENOMEM))
		{
			Log("You can try later");
			return Res_e::TEMPORARY_UNSUFFICIENT_RESOURCES;
		}
		return Res_e::FAILURE;
	}
	Log("UDP unbound socket ", m_udp_desc, " is created");
	
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
	
	Log("UDP and TCP sockets are bound");
	
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
		close(m_tcp_desc);
		m_tcp_desc = -1;
		close(m_udp_desc);
		m_udp_desc = -1;
		return Res_e::FAILURE;
	}	
	
	epoll_event tcp_ev;
	tcp_ev.events = EPOLLIN;
	tcp_ev.data.fd = m_tcp_desc;
	if (-1 == epoll_ctl(epfd, EPOLL_CTL_ADD, m_tcp_desc, &tcp_ev))
	{
		close(m_tcp_desc);
		m_tcp_desc = -1;
		close(m_udp_desc);
		m_udp_desc = -1;
		return Res_e::FAILURE;
	}
	
	if (-1 == set_nonblocking(m_udp_desc))
	{
		Log("Set NON BLOCKING mode for UDP socket ", m_udp_desc, " failed: ", strerror(errno));
		close(m_tcp_desc);
		m_tcp_desc = -1;
		close(m_udp_desc);
		m_udp_desc = -1;
		return Res_e::FAILURE;	
	}
	
	epoll_event udp_ev;
	udp_ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
	udp_ev.data.fd = m_udp_desc;
	if (-1 == epoll_ctl(epfd, EPOLL_CTL_ADD, m_udp_desc, &udp_ev))
	{
		close(m_tcp_desc);
		m_tcp_desc = -1;
		close(m_udp_desc);
		m_udp_desc = -1;
		return Res_e::FAILURE;
	}
	//first field in pair is position in message for write next part of message
	tcp_msg_storage_t tcp_messages;
	udp_storage_t udp_messages;
	
	constexpr size_t MAX_EVENTS = 15;
	epoll_event events[MAX_EVENTS];
	
	for(;;)
	{
		int const nfds = epoll_wait(epfd, events, MAX_EVENTS, ms_timeout);
		if (-1 == nfds)
		{
			Log("epoll_wait failed: ", strerror(errno));
			return Res_e::FAILURE;	
		}
		if (0 == nfds)
		{
			Log("epoll_wait timeout");
			break;
		}
		else
		{
			Log("epoll_wait return ", nfds); 
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
			if (EPOLLOUT&events[i].events)
			{
				Log("EPOLLOUT Event for ", (events[i].data.fd != m_udp_desc ? "TCP" : "UDP"), " socket ", events[i].data.fd);
				if (events[i].data.fd != m_udp_desc)
				{
					if (auto it = tcp_messages.find(events[i].data.fd); tcp_messages.cend() != it) {
						int sd = events[i].data.fd;
						tcp_write_handler(sd, it->second, tcp_messages, it->second.rsp_start_pos);
					}
				}
				else
				{
					for (auto it = udp_messages.begin(); it != udp_messages.end(); )
					{
						if (it->is_rsp_in_progress)
						{
							Log("UDP socket ", events[i].data.fd, " is going to continue sending response for ", it->ip, ":", it->port); 
							sockaddr_in from;
							socklen_t from_len = sizeof(from);
							memset(&from, 0, from_len);
							inet_pton(AF_INET, it->ip, &from.sin_addr);
							from.sin_port = htons(it->port);
							from.sin_family = AF_INET;
							
							uint16_t ost = it->msg.size()%(it->packet_size - UDP_PACKET_HEADER_SIZE);
							if (not udp_write_handler(m_udp_desc, *it, ost, from, from_len) or it->packet_sn_list.empty()) {
								it = udp_messages.erase(it);
							}
							else ++it;
						}
						else ++it;
					}
				}
			}
			if (EPOLLIN&events[i].events)
			{
				Log("EPOLLIN Event for ", (events[i].data.fd != m_udp_desc ? "TCP" : "UDP"), " socket ", events[i].data.fd);
				if (events[i].data.fd == m_tcp_desc)
				{
					sockaddr_in from;
					socklen_t from_len = sizeof(from);
					memset(&from, 0, from_len);
					int conn_desc = accept(m_tcp_desc, (sockaddr*)&from, &from_len);
					if (-1 == conn_desc)
					{
						Log("accept failed: ", strerror(errno));
						continue;	
					}
					char from_ip[MAX_IPv4_SIZE + 1];
					inet_ntop(AF_INET, &from.sin_addr, from_ip, sizeof(from_ip));
					auto const from_port = ntohs(from.sin_port);
					Log("TCP connection socket ", conn_desc, " is created for TCP client ", from_ip, ":", from_port);
					if (-1 == set_nonblocking(conn_desc))
					{
						Log("Set NON BLOCKING mode for socket ", conn_desc, " failed: ", strerror(errno));
						close(conn_desc);
						continue;	
					}
					epoll_event event_connected_sd;
					event_connected_sd.data.fd = conn_desc;
					event_connected_sd.events = EPOLLIN | EPOLLOUT | EPOLLHUP | EPOLLRDHUP | EPOLLET;
					if (-1 == epoll_ctl(epfd, EPOLL_CTL_ADD, conn_desc, &event_connected_sd))
					{
						Log("epoll_ctl for socket ", conn_desc, " failed: ", strerror(errno));
						close(conn_desc);
						continue;	
					}
				}
				else if (events[i].data.fd == m_udp_desc)
				{
					char buffer[MAX_UDP_PACKET_SIZE];
					while(1)
					{
						sockaddr_in from;
						socklen_t from_len = sizeof(from);
						int read_num = recvfrom(m_udp_desc, buffer, MAX_UDP_PACKET_SIZE, 0, (sockaddr*)&from, &from_len);
						if (read_num > 0)
						{
							if (read_num < (UDP_PACKET_HEADER_SIZE))
							{
								Log("Packet is not from protey client");
								continue;
							}
							if (string_view{buffer, UDP_PACKET_BEGIN.size()} != UDP_PACKET_BEGIN)
							{
								Log("Packet is not from protey client");
								continue;
							}
							
							uint16_t packet_size;
							memcpy(&packet_size, buffer + UDP_PH_SIZE_POS, sizeof(packet_size));
							packet_size = ntohs(packet_size);
							if (packet_size > MAX_UDP_PACKET_SIZE) {
								Log("Packet size ", packet_size, " bytes is too large");
								continue;
							}
							
							udp_message_info mi;
							inet_ntop(AF_INET, &from.sin_addr, mi.ip, sizeof(mi.ip));
							mi.port = ntohs(from.sin_port);
							mi.packet_size = packet_size;
							Log("Packet received from client ", mi.ip, ":", mi.port);
							auto [it, res] = udp_messages.emplace(mi);
							if (res)
							{	
								Log("Packet size is ", it->packet_size);
								memcpy(&it->packets_qty, buffer + UDP_PH_QTY_POS, sizeof(it->packets_qty));
								it->packets_qty = ntohs(it->packets_qty);
								Log("Packet qty is ", it->packets_qty);
								it->msg.resize(it->packets_qty * (it->packet_size - UDP_PACKET_HEADER_SIZE));
								it->is_rsp_in_progress = false;
							}
							else
							{
								if (it->is_rsp_in_progress)
								{
									Log("Duplicated packet is received after sending response starts");
									continue;
								}
							}
							uint16_t packet_number;
							memcpy(&packet_number, buffer + UDP_PH_SEQ_NUM_POS, sizeof(packet_number));
							packet_number = ntohs(packet_number);
							Log("Packet number is ", packet_number);

							if (find(it->packet_sn_list.begin(), it->packet_sn_list.end(), packet_number) != it->packet_sn_list.end())
							{
								Log("Duplicated packet");
								continue;
							}
							it->packet_sn_list.push_back(packet_number);
							if (packet_number == (it->packets_qty - 1))
							{
								Log("Last packet of message is received");
								it->msg.resize((it->packets_qty - 1)*(it->packet_size - UDP_PACKET_HEADER_SIZE) + (read_num - UDP_PACKET_HEADER_SIZE));
							}
							else
							{
								if (read_num != it->packet_size)
								{
									Log("Damaged packet");
									continue;
								}
							}
							
							memcpy(&it->msg[packet_number*(it->packet_size - UDP_PACKET_HEADER_SIZE)], &buffer[UDP_PACKET_HEADER_SIZE], read_num - UDP_PACKET_HEADER_SIZE);
							
							if (it->packets_qty == it->packet_sn_list.size())
							{
								Log("All packets of message are received");
								input_handler(it->msg, false);
								it->packets_qty = it->msg.size()/(it->packet_size - UDP_PACKET_HEADER_SIZE);
								uint16_t ost = it->msg.size()%(it->packet_size - UDP_PACKET_HEADER_SIZE);
								if (ost > 0){ ++(it->packets_qty); }
								Log("Packets qty for response is ", it->packets_qty);
								
								
								it->packet_sn_list.clear();
								for (size_t i = 0; i < it->packets_qty; ++i){ it->packet_sn_list.push_back(i); }
								
								if (not udp_write_handler(m_udp_desc, *it, ost, from, from_len) or it->packet_sn_list.empty())
								{
									udp_messages.erase(it);
								}
							}
						}
						else if (0 == read_num)
						{
							Log("Read 0 bytes");
							break;
						}
						else
						{
							if (errno == EAGAIN || errno == EWOULDBLOCK) {
								Log("EAGAIN or EWOULDBLOCK while reading from UDP socket ", m_udp_desc);
							}
							else {
								Log("Read failed: ", strerror(errno));
							}
							break;
						}
					}
				}	
				else
				{
					char buffer[TCP_READ_BUFFER_SIZE];
					
					auto [it, is_new] = tcp_messages.try_emplace(events[i].data.fd);
					if (is_new)
					{
						it->second.rsp_start_pos = 0;
					}
					else if (0 != it->second.rsp_start_pos)
					{
						Log("Cached response message can not be modified by input request");
						continue;
					}
					auto& msg = it->second.msg;
					
					int read_bytes;
					size_t read_counter = 0;
					while(1)
					{
						read_bytes = read(events[i].data.fd, buffer, TCP_READ_BUFFER_SIZE);
						
						if (read_bytes == -1)
						{
							if (errno == EINTR) {
								Log("EINTR");
								continue;
							}
							if (errno == EAGAIN || errno == EWOULDBLOCK) {
								Log("EAGAIN or EWOULDBLOCK while reading from TCP socket ", events[i].data.fd);
								break;
							}
							
							Log("Read finished with ERROR: ", strerror(errno));
							msg.clear();
							break;
						}
						else if ((read_bytes == 0) and (0 == read_counter))
						{
							Log("Client disconnected for socket ", events[i].data.fd);
							tcp_messages.erase(it);
							close(events[i].data.fd);
							break;
						}
						else
						{
							Log("Read ", read_bytes, " bytes by TCP");
							++read_counter;
							
							if (it->second.rsp_start_pos){
								Log("Socket ", events[i].data.fd, " is busy by sending response to previous message. This request will be damaged or lost");
								continue;
							}
							
							int sd = events[i].data.fd;
							auto end = buffer + read_bytes;
							for (auto curr = buffer; curr != end; )
							{
								auto msg_end = find(curr, end, '\n');
								msg.append(curr, msg_end);
							
								if (msg_end != end)
								{
									Log("Message END symbol is detected");
									
									input_handler(msg, true);
									//Actions with back() symbol because input_handling function is used both TCP and UDP
									//And '\n' symbol is used as message end only in TCP
									msg.push_back('\n');
									
									size_t start_pos = 0;
									
									if (tcp_write_handler(sd, it->second, tcp_messages, start_pos)){
										if (0 != it->second.rsp_start_pos){
											curr = ++msg_end;
										}
										else break;
									}
									else{
										if (-1 != sd){
											curr = ++msg_end;
										}
										else break;
									}
								}
								else break;	
							}
							if (-1 == sd) break;
						}
					}
				}
			}
			if ((EPOLLRDHUP | EPOLLHUP)&events[i].events)
			{
				Log("EPOLLRDHUP or EPOLLHUP, client is disconnected for socket ", events[i].data.fd);
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

