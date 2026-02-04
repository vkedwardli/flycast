#include "gdxsv_network.h"

#include <chrono>
#include <cmath>
#include <random>
#include <thread>

#include "oslib/http_client.h"
#include "sleep.h"
#include "gdxsv.h"

#ifndef _WIN32
#include <sys/ioctl.h>
#endif

static std::vector<std::string> v4_urls = {"https://api.ipify.org", "https://ipv4.seeip.org"};

static std::vector<std::string> v6_urls = {"https://api64.ipify.org", "https://api.seeip.org"};

std::future<std::pair<bool, std::string>> get_public_ip_address(bool ipv6) {
	return std::async(std::launch::async, [ipv6]() -> std::pair<bool, std::string> {
		std::vector<u8> myip;
		std::string dummy;
		http::init();

		const auto urls = ipv6 ? v6_urls : v4_urls;
		int rc = 0;
		for (const auto &url : urls) {
			rc = http::get(url, myip, dummy);
			if (!http::success(rc)) {
				continue;
			}
			break;
		}

		if (!http::success(rc)) {
			return {false, "HTTP Request failed 1: " + std::to_string(rc)};
		}

		if (ipv6 && std::count(myip.begin(), myip.end(), '.') == 3) {
			return {false, "No IPv6 address used"};
		}

		return {true, std::string(myip.begin(), myip.end())};
	});
}

std::future<std::string> test_udp_port_connectivity(int port, bool ipv6) {
	return std::async(std::launch::async, [port, ipv6]() -> std::string {
		UdpClient udp;
		if (!udp.Bind(port)) {
			return "Bind failed";
		}

		auto public_addr = get_public_ip_address(ipv6);
		public_addr.wait();
		auto [ok, msg] = public_addr.get();

		if (!ok) {
			return msg;
		}

		const auto myip = msg;
		std::vector<u8> content;
		std::string content_type;
		std::vector<http::PostField> fields;
		auto test_addr = myip + ":" + std::to_string(port);
		if (ipv6) {
			test_addr = "[" + myip + "]:" + std::to_string(port);
		}
		fields.emplace_back("addr", test_addr);
		int rc = http::post("https://asia-northeast1-gdxsv-274515.cloudfunctions.net/udptest", fields);
		if (!http::success(rc)) {
			return "HTTP Request failed: " + std::to_string(rc);
		}

		for (int t = 0; t < 30; t++) {
			char buf[128];
			sockaddr_storage sender{};
			socklen_t addrlen = sizeof(sockaddr_storage);
			int n = udp.RecvFrom(buf, sizeof(buf), &sender, &addrlen);
			if (0 < n) {
				if (std::string(buf, 5) == "Hello") {
					return "Success";
				}
			}

			sleep_us(100 * 1000);
		}

		return "Failed (Timeout)";
	});
}

std::future<std::map<std::string, int>> gcp_ping_test() {
	auto fn = []() -> std::map<std::string, int> {
		std::map<std::string, int> test_result;

		// powered by https://github.com/GoogleCloudPlatform/gcping
		const std::string get_path = "/api/ping";
		const std::tuple<std::string, std::string, std::string> gcp_region_hosts[] = {
//			{"asia-south1", "asia-south1-5tkroniexa-el.a.run.app", "India"},
			{"australia-southeast1", "australia-southeast1-5tkroniexa-ts.a.run.app", "Australia"},
//			{"europe-north1", "europe-north1-5tkroniexa-lz.a.run.app", "Finland"},
//			{"europe-west1", "europe-west1-5tkroniexa-ew.a.run.app", "Belgium"},
			{"europe-west2", "europe-west2-5tkroniexa-nw.a.run.app", "London"},
//			{"europe-west3", "europe-west3-5tkroniexa-ey.a.run.app", "Germany"},
//			{"europe-west4", "europe-west4-5tkroniexa-ez.a.run.app", "Netherlands"},
//			{"europe-west6", "europe-west6-5tkroniexa-oa.a.run.app", "Switzerland"},
			{"northamerica-northeast1", "northamerica-northeast1-5tkroniexa-nn.a.run.app", "Canada"},
			{"southamerica-east1", "southamerica-east1-5tkroniexa-rj.a.run.app", "Brazil"},
			{"us-central1", "us-central1-5tkroniexa-uc.a.run.app", "US Central - Iowa"},
//			{"us-east1", "us-east1-5tkroniexa-ue.a.run.app", "US East - South Carolina"},
			{"us-east4", "us-east4-5tkroniexa-uk.a.run.app", "US East - Virginia"},
//			{"us-west1", "us-west1-5tkroniexa-uw.a.run.app", "US West - Oregon"},
			{"us-west2", "us-west2-5tkroniexa-wl.a.run.app", "US West - California"},
//			{"us-west3", "us-west3-5tkroniexa-wm.a.run.app", "US West - Utah"},
			{"asia-southeast1", "asia-southeast1-5tkroniexa-as.a.run.app", "Singapore"},
			{"asia-east1", "asia-east1-5tkroniexa-de.a.run.app", "Taiwan"},
			{"asia-east2", "asia-east2-5tkroniexa-df.a.run.app", "Hong Kong"},
			{"asia-northeast1", "asia-northeast1-5tkroniexa-an.a.run.app", "Tokyo"},
			{"asia-northeast2", "asia-northeast2-5tkroniexa-dt.a.run.app", "Osaka"},
			{"asia-northeast3", "asia-northeast3-5tkroniexa-du.a.run.app", "Seoul"},
		};

		// Function to test a single region
		auto test_region = [&get_path](const std::tuple<std::string, std::string, std::string> &region_host) -> std::pair<std::string, int> {
			TcpClient client;
			if (!client.Connect(std::get<1>(region_host).c_str(), 80)) {
				ERROR_LOG(COMMON, "connect failed : %s", std::get<0>(region_host).c_str());
				return {"", -1};
			}

			int min_rtt = -1;
			for (int i = 0; i < 3; i++) {
				std::stringstream ss;
				ss << "HEAD " << get_path << " HTTP/1.1"
				   << "\r\n";
				ss << "Host: " << std::get<1>(region_host) << "\r\n";
				ss << "User-Agent: flycast for gdxsv"
				   << "\r\n";
				ss << "Connection: keep-alive"
				   << "\r\n";
				ss << "Accept: */*"
				   << "\r\n";
				ss << "\r\n";  // end of header

				auto request_header = ss.str();
				auto t1 = std::chrono::high_resolution_clock::now();
				int n = client.Send(request_header.c_str(), (int)request_header.size());
				if (n < request_header.size()) {
					ERROR_LOG(COMMON, "send failed : %s (attempt %d)", std::get<0>(region_host).c_str(), i + 1);
					break;
				}

				char buf[1024] = {0};
				n = client.Recv(buf, 1024);
				if (n <= 0) {
					ERROR_LOG(COMMON, "recv failed : %s (attempt %d)", std::get<0>(region_host).c_str(), i + 1);
					break;
				}

				auto t2 = std::chrono::high_resolution_clock::now();
				int rtt = (int)std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
				const std::string response_header(buf, n);
				if (response_header.find("200 OK") == std::string::npos && response_header.find("302 Found") == std::string::npos) {
					ERROR_LOG(COMMON, "error response : %s (attempt %d)", response_header.c_str(), i + 1);
					break;
				}

				NOTICE_LOG(COMMON, "%s : attempt %d: %d[ms]", std::get<2>(region_host).c_str(), i + 1, rtt);

				if (min_rtt == -1 || rtt < min_rtt) {
					min_rtt = rtt;
				}
			}

			client.Close();

			if (min_rtt == -1) {
				ERROR_LOG(COMMON, "Ping test failed for all attempts : %s", std::get<0>(region_host).c_str());
				return {"", -1};
			}

			char latency_str[256];
			snprintf(latency_str, 256, "%s : %d[ms]", std::get<2>(region_host).c_str(), min_rtt);
			NOTICE_LOG(COMMON, "%s", latency_str);
			gdxsv.SetPingResult(std::string(latency_str));

			return {std::get<0>(region_host), min_rtt};
		};

		// Execute ping tests with max 6 parallel workers
		const int max_parallel = 6;
		const int total_regions = std::size(gcp_region_hosts);

		std::vector<std::future<std::pair<std::string, int>>> futures;
		int next_region_idx = 0;

		// Start initial batch of workers (up to max_parallel)
		for (int i = 0; i < std::min(max_parallel, total_regions); i++) {
			futures.push_back(std::async(std::launch::async, test_region, gcp_region_hosts[next_region_idx++]));
		}

		// Process results as they complete and start new tasks
		while (!futures.empty()) {
			// Wait for at least one future to complete (blocking on first future)
			futures.front().wait_for(std::chrono::milliseconds(1));

			// Check all futures for completion
			for (auto it = futures.begin(); it != futures.end();) {
				if (it->wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
					// Get result and store it
					auto result = it->get();
					if (!result.first.empty() && result.second >= 0) {
						test_result[result.first] = result.second;
					}

					// Remove this completed future
					it = futures.erase(it);

					// Start a new task if there are more regions to process
					if (next_region_idx < total_regions) {
						futures.push_back(std::async(std::launch::async, test_region, gcp_region_hosts[next_region_idx++]));
					}
				} else {
					++it;
				}
			}
		}

		gdxsv.SetPingResult("Done");
		return test_result;
	};

	return std::async(std::launch::async, fn);
}

int get_random_port_number() {
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_int_distribution<> dist(29700, 29800);
	return dist(gen);
}

std::string sockaddr_to_string(const sockaddr *addr) {
	if (addr == nullptr) {
		return "";
	}
	if (addr->sa_family == AF_INET) {
		const auto a = reinterpret_cast<const sockaddr_in *>(addr);
		char addrbuf[INET_ADDRSTRLEN];
		::inet_ntop(AF_INET, &a->sin_addr, addrbuf, sizeof(addrbuf));
		return std::string(addrbuf) + ":" + std::to_string(ntohs(a->sin_port));
	}
	if (addr->sa_family == AF_INET6) {
		const auto a = reinterpret_cast<const sockaddr_in6 *>(addr);
		char addrbuf[INET6_ADDRSTRLEN];
		::inet_ntop(AF_INET6, &a->sin6_addr, addrbuf, sizeof(addrbuf));
		return "[" + std::string(addrbuf) + "]:" + std::to_string(ntohs(a->sin6_port));
	}
	return "";
}

bool is_loopback_addr(const sockaddr *addr) {
	if (addr == nullptr) {
		return false;
	}
	if (addr->sa_family == AF_INET) {
		const auto a = reinterpret_cast<const sockaddr_in *>(addr);
		return a->sin_addr.s_addr == htonl(INADDR_LOOPBACK);
	}
	if (addr->sa_family == AF_INET6) {
		const auto a = reinterpret_cast<const sockaddr_in6 *>(addr);
		return IN6_IS_ADDR_LOOPBACK(&a->sin6_addr);
	}
	return false;
}

bool is_private_addr(const sockaddr *addr) {
	if (addr == nullptr) {
		return false;
	}
	if (addr->sa_family == AF_INET) {
		const auto a = reinterpret_cast<const sockaddr_in *>(addr);
		const auto ip4 = reinterpret_cast<const uint8_t *>(&a->sin_addr);
		return ip4[0] == 10 || (ip4[0] == 172 && (ip4[1] & 0xf0) == 16) || (ip4[0] == 192 && ip4[1] == 168);
	}
	if (addr->sa_family == AF_INET6) {
		const auto a = reinterpret_cast<const sockaddr_in6 *>(addr);
		const auto ip6 = reinterpret_cast<const uint8_t *>(&a->sin6_addr);
		return (ip6[0] & 0xfe) == 0xfc;
	}
	return false;
}

bool is_same_addr(const sockaddr *addr1, const sockaddr *addr2) {
	if (addr1->sa_family != addr2->sa_family) {
		return false;
	}
	if (addr1->sa_family == AF_INET) {
		const auto a = reinterpret_cast<const sockaddr_in *>(addr1);
		const auto b = reinterpret_cast<const sockaddr_in *>(addr2);
		return memcmp(&a->sin_addr, &b->sin_addr, sizeof(a->sin_addr)) == 0 && a->sin_port == b->sin_port;
	}
	if (addr1->sa_family == AF_INET6) {
		const auto a = reinterpret_cast<const sockaddr_in6 *>(addr1);
		const auto b = reinterpret_cast<const sockaddr_in6 *>(addr2);
		return memcmp(&a->sin6_addr, &b->sin6_addr, sizeof(a->sin6_addr)) == 0 && a->sin6_port == b->sin6_port;
	}
	return false;
}

std::string mask_ip_address(std::string addr) {
	if (2 <= std::count(addr.begin(), addr.end(), ':')) {
		int cnt = 0;
		const int last_colon = addr.rfind(':');
		for (int i = 0; i < last_colon; i++) {
			if (addr[i] == ':') cnt++;
			if (cnt < 3) continue;
			if (('0' <= addr[i] && addr[i] <= '9') || ('a' <= addr[i] && addr[i] <= 'f')) addr[i] = 'x';
		}
	} else {
		for (int i = addr.find('.'); i < addr.size(); i++) {
			if ('0' <= addr[i] && addr[i] <= '9') addr[i] = 'x';
			if (addr[i] == ':') break;
		}
	}
	return addr;
}

bool TcpClient::Connect(const char *host, int port) {
	NOTICE_LOG(COMMON, "TCP Connect: %s:%d", host, port);

	sock_t new_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (new_sock == INVALID_SOCKET) {
		WARN_LOG(COMMON, "Connect fail 1 %d", get_last_error());
		return false;
	}
	auto host_entry = gethostbyname(host);
	if (host_entry == nullptr || host_entry->h_addr_list[0] == nullptr) {
		WARN_LOG(COMMON, "Connect fail 2 gethostbyname");
		return false;
	}

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
#ifdef _WIN32
	addr.sin_addr = *((LPIN_ADDR)host_entry->h_addr_list[0]);
#else
	memcpy(&addr.sin_addr, host_entry->h_addr_list[0], host_entry->h_length);
#endif
	addr.sin_port = htons(port);

	fd_set setW, setE;
	struct timeval timeout = {5, 0};
	int res;

	auto set_blocking_mode = [](const int &socket, bool is_blocking) {
#ifdef _WIN32
		u_long flags = is_blocking ? 0 : 1;
		ioctlsocket(socket, FIONBIO, &flags);
#else
		const int flags = fcntl(socket, F_GETFL, 0);
		fcntl(socket, F_SETFL, is_blocking ? flags ^ O_NONBLOCK : flags | O_NONBLOCK);
#endif
	};

	set_blocking_mode(new_sock, false);

	if (::connect(new_sock, (const sockaddr *)&addr, sizeof(addr)) != 0) {
		if (get_last_error() != EINPROGRESS && get_last_error() != L_EWOULDBLOCK) {
			WARN_LOG(COMMON, "Connect fail 2 %d", get_last_error());
			return false;
		} else {
			do {
				FD_ZERO(&setW);
				FD_SET(new_sock, &setW);
				FD_ZERO(&setE);
				FD_SET(new_sock, &setE);

				res = select(new_sock + 1, NULL, &setW, &setE, &timeout);
				if (res < 0 && errno != EINTR) {
					WARN_LOG(COMMON, "Connect fail 3 %d", get_last_error());
					return false;
				} else if (res > 0) {
					int error;
					socklen_t l = sizeof(int);
#ifdef _WIN32
					if (getsockopt(new_sock, SOL_SOCKET, SO_ERROR, (char *)&error, &l) < 0 || error) {
#else
					if (getsockopt(new_sock, SOL_SOCKET, SO_ERROR, &error, &l) < 0 || error) {
#endif
						WARN_LOG(COMMON, "Connect fail 4 %d", error);
						return false;
					}

					if (FD_ISSET(new_sock, &setE)) {
						WARN_LOG(COMMON, "Connect fail 5 %d", get_last_error());
						return false;
					}

					break;
				} else {
					WARN_LOG(COMMON, "Timeout in select() - Cancelling!");
					return false;
				}
			} while (1);
		}
		set_blocking_mode(new_sock, true);
	}

	if (sock_ != INVALID_SOCKET) {
		closesocket(sock_);
	}

	set_tcp_nodelay(new_sock);

	sock_ = new_sock;
	host_ = std::string(host);
	port_ = port;

	{
		sockaddr_in name{};
		socklen_t namelen = sizeof(name);
		if (getsockname(new_sock, reinterpret_cast<sockaddr *>(&name), &namelen) != 0) {
			WARN_LOG(COMMON, "getsockname failed");
		} else {
			char buf[INET_ADDRSTRLEN];
			local_ip_ = std::string(inet_ntop(AF_INET, &name.sin_addr, buf, INET_ADDRSTRLEN));
		}
	}

	NOTICE_LOG(COMMON, "TCP Connect: %s:%d ok", host, port);
	return true;
}

int TcpClient::IsConnected() const { return sock_ != INVALID_SOCKET; }

void TcpClient::SetNonBlocking() {
	set_recv_timeout(sock_, 1);
	set_send_timeout(sock_, 1);
	set_non_blocking(sock_);
}

int TcpClient::Recv(char *buf, int len) {
	int n = ::recv(sock_, buf, len, 0);
	if (n < 0 && get_last_error() != L_EAGAIN && get_last_error() != L_EWOULDBLOCK) {
		WARN_LOG(COMMON, "TCP Recv failed. errno=%d", get_last_error());
		this->Close();
	}
	if (n < 0) return 0;
	return n;
}

int TcpClient::Send(const char *buf, int len) {
	if (len == 0) return 0;
	int n = ::send(sock_, buf, len, 0);
	if (n < 0 && get_last_error() != L_EAGAIN && get_last_error() != L_EWOULDBLOCK) {
		WARN_LOG(COMMON, "TCP Send failed. errno=%d", get_last_error());
		this->Close();
	}
	if (n < 0) return 0;
	return n;
}

u32 TcpClient::ReadableSize() const {
	u_long n = 0;
#ifndef _WIN32
	ioctl(sock_, FIONREAD, &n);
#else
	ioctlsocket(sock_, FIONREAD, &n);
#endif
	return u32(n);
}

void TcpClient::Close() {
	if (sock_ != INVALID_SOCKET) {
		closesocket(sock_);
		sock_ = INVALID_SOCKET;
	}
}

bool UdpRemote::Open(const char *host, int port) {
	verify(0 < port && port < 65536);
	addrinfo *res = nullptr;
	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;	  // IPv4 or IPv6
	hints.ai_socktype = SOCK_DGRAM;	  // UDP
	hints.ai_flags = AI_NUMERICSERV;  // service is port no
	char service[10] = {};
	snprintf(service, sizeof(service), "%d", port);
	const auto err = getaddrinfo(host, service, &hints, &res);
	if (err != 0) {
		ERROR_LOG(COMMON, "UDP Remote::Open failed. getaddrinfo %s err %d", host, err);
		return false;
	}

	for (auto info = res; info != nullptr; info = info->ai_next) {
		if (info->ai_family == AF_INET || info->ai_family == AF_INET6) {
			memcpy(&net_addr_, info->ai_addr, info->ai_addrlen);
			net_addr_len_ = info->ai_addrlen;
			break;
		}
	}

	freeaddrinfo(res);

	if (net_addr_len_ == 0) {
		ERROR_LOG(COMMON, "UDP Remote::Open failed. no address available");
		return false;
	}

	return true;
}

bool UdpRemote::Open(const std::string &ip_port) {
	if (std::count(ip_port.begin(), ip_port.end(), ':') != 1) {
		return false;
	}
	size_t colon_pos = ip_port.rfind(':');
	std::string host = ip_port.substr(0, colon_pos);
	int port = std::stoi(ip_port.substr(colon_pos + 1));
	return Open(host.c_str(), port);
}

bool UdpRemote::Open(const sockaddr *addr, socklen_t addrlen) {
	net_addr_len_ = addrlen;
	memcpy(&net_addr_, addr, addrlen);
	return true;
}

void UdpRemote::Close() {
	net_addr_len_ = 0;
	memset(&net_addr_, 0, sizeof(net_addr_));
}

bool UdpClient::Bind(int port) {
	assert(0 <= port);

	if (sock_v4_ != INVALID_SOCKET) {
		closesocket(sock_v4_);
		sock_v4_ = INVALID_SOCKET;
	}
	if (sock_v6_ != INVALID_SOCKET) {
		closesocket(sock_v6_);
		sock_v6_ = INVALID_SOCKET;
	}

	for (int i = 0; i < 2; i++) {
		const int af = (i == 0) ? AF_INET : AF_INET6;

		// create socket
		sock_t sock = socket(af, SOCK_DGRAM, IPPROTO_UDP);
		if (sock == INVALID_SOCKET) {
			WARN_LOG(COMMON, "UDP socket create failed %d", get_last_error());
			break;
		}

		// sockopt
		int optval = 1;
		if (port != 0) {
			setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&optval, sizeof optval);
		}
		optval = 0;
		setsockopt(sock, SOL_SOCKET, SO_LINGER, (const char *)&optval, sizeof optval);
		if (af == AF_INET6) {
			optval = 1;
			if (setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&optval, sizeof optval)) {
				closesocket(sock);
				break;
			}
		}

		sockaddr_storage addr_storage{};
		socklen_t addrlen = 0;

		if (af == AF_INET) {
			auto addr = reinterpret_cast<sockaddr_in *>(&addr_storage);
			addr->sin_port = htons(port);
			addr->sin_family = AF_INET;
			addr->sin_addr.s_addr = htonl(INADDR_ANY);
			addrlen = sizeof(sockaddr_in);
		} else {
			auto addr = reinterpret_cast<sockaddr_in6 *>(&addr_storage);
			addr->sin6_port = htons(port);
			addr->sin6_family = AF_INET6;
			addr->sin6_addr = in6addr_any;
			addrlen = sizeof(sockaddr_in6);
		}

		// bind
		if (::bind(sock, reinterpret_cast<sockaddr *>(&addr_storage), addrlen) < 0) {
			ERROR_LOG(COMMON, "gdxsv: bind() failed. errno=%d", get_last_error());
			closesocket(sock);
			break;
		}

		set_recv_timeout(sock, 1);
		set_send_timeout(sock, 1);
		set_non_blocking(sock);

		if (af == AF_INET) {
			sock_v4_ = sock;
			NOTICE_LOG(COMMON, "bound v4 :%d", port);
		} else {
			sock_v6_ = sock;
			NOTICE_LOG(COMMON, "bound v6 :%d", port);
		}
	}

	if (sock_v4_ == INVALID_SOCKET) {
		NOTICE_LOG(COMMON, "UDP Initialize failed");
		return false;
	}

	bound_port_ = port;
	NOTICE_LOG(COMMON, "UDP Initialize ok: :%d", bound_port_);
	return true;
}

bool UdpClient::Initialized() const { return sock_v4_ != INVALID_SOCKET; }

int UdpClient::RecvFrom(char *buf, int len, sockaddr_storage *from_addr, socklen_t *addrlen) {
	if (sock_v4_ != INVALID_SOCKET) {
		int n = ::recvfrom(sock_v4_, buf, len, 0, (struct sockaddr *)from_addr, addrlen);
		if (n < 0 && get_last_error() != L_EAGAIN && get_last_error() != L_EWOULDBLOCK) {
			ERROR_LOG(COMMON, "UDP4 Recv failed. errno=%d", get_last_error());
		}
		if (0 < n) {
			return n;
		}
	}

	if (sock_v6_ != INVALID_SOCKET) {
		int n = ::recvfrom(sock_v6_, buf, len, 0, (struct sockaddr *)from_addr, addrlen);
		if (n < 0 && get_last_error() != L_EAGAIN && get_last_error() != L_EWOULDBLOCK) {
			ERROR_LOG(COMMON, "UDP6 Recv failed. errno=%d", get_last_error());
		}
		if (0 < n) {
			return n;
		}
	}

	return 0;
}

int UdpClient::SendTo(const char *buf, int len, const UdpRemote &remote) {
	sock_t sock = remote.is_v6() ? sock_v6_ : sock_v4_;
	if (sock == INVALID_SOCKET) {
		return 0;
	}
	int n = ::sendto(sock, buf, len, 0, remote.net_addr(), remote.net_addr_len());
	if (n < 0 && get_last_error() != L_EAGAIN && get_last_error() != L_EWOULDBLOCK) {
		WARN_LOG(COMMON, "UDP Send failed. errno=%d", get_last_error());
		return 0;
	}
	if (0 < n) return n;
	return 0;
}

void UdpClient::Close() {
	if (sock_v4_ != INVALID_SOCKET) {
		closesocket(sock_v4_);
		sock_v4_ = INVALID_SOCKET;
	}
	if (sock_v6_ != INVALID_SOCKET) {
		closesocket(sock_v6_);
		sock_v6_ = INVALID_SOCKET;
	}
}

MessageBuffer::MessageBuffer() { Clear(); }

bool MessageBuffer::CanPush() const { return packet_.battle_data().size() < kBufSize; }

void MessageBuffer::SessionId(const std::string &session_id) { packet_.set_session_id(session_id.c_str()); }

bool MessageBuffer::PushBattleMessage(const std::string &user_id, u8 *body, u32 body_length) {
	if (!CanPush()) {
		// buffer full
		return false;
	}

	auto msg = packet_.add_battle_data();
	msg->set_seq(msg_seq_);
	msg->set_user_id(user_id);
	msg->set_body(body, body_length);
	packet_.set_seq(msg_seq_);
	msg_seq_++;
	return true;
}

const proto::Packet &MessageBuffer::Packet() { return packet_; }

void MessageBuffer::ApplySeqAck(u32 seq, u32 ack) {
	if (snd_seq_ <= ack) {
		packet_.mutable_battle_data()->DeleteSubrange(0, ack - snd_seq_ + 1);
		snd_seq_ = ack + 1;
	}
	if (packet_.ack() < seq) {
		packet_.set_ack(seq);
	}
}

void MessageBuffer::Clear() {
	packet_.Clear();
	packet_.set_type(proto::MessageType::Battle);
	msg_seq_ = 1;
	snd_seq_ = 1;
}

bool MessageFilter::IsNextMessage(const proto::BattleMessage &msg) {
	auto last_seq = recv_seq[msg.user_id()];
	if (last_seq == 0 || msg.seq() == last_seq + 1) {
		recv_seq[msg.user_id()] = msg.seq();
		return true;
	}
	return false;
}

void MessageFilter::Clear() { recv_seq.clear(); }

void UdpPingPong::Start(uint32_t session_id, uint8_t peer_id, int port, int duration_ms) {
	if (running_) return;
	verify(peer_id < N);
	if (!client_.Bind(port)) {
		ERROR_LOG(COMMON, "UdpPingPong.Start client_.Bind failed");
		return;
	}
	running_ = true;
	int network_delay = 0;
	const auto delay_option = std::getenv("GGPO_NETWORK_DELAY");
	if (delay_option != nullptr) {
		network_delay = atoi(delay_option);
		NOTICE_LOG(COMMON, "GGPO_NETWORK_DELAY is %d", network_delay);
	}

	peer_id_ = peer_id;
	std::thread([this, session_id, peer_id, duration_ms, network_delay]() {
		WARN_LOG(COMMON, "Start UdpPingPong Thread");
		start_time_ = std::chrono::high_resolution_clock::now();

		for (int loop_count = 0; running_; loop_count++) {
			auto now = std::chrono::high_resolution_clock::now();
			auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time_).count();

			while (true) {
				Packet recv{};
				sockaddr_storage sender_storage{};
				auto *sender = reinterpret_cast<sockaddr *>(&sender_storage);
				socklen_t addrlen = sizeof(sender_storage);

				int n = client_.RecvFrom(reinterpret_cast<char *>(&recv), sizeof(Packet), &sender_storage, &addrlen);
				if (n <= 0) {
					break;
				}

				if (recv.magic != MAGIC) {
					WARN_LOG(COMMON, "invalid magic");
					continue;
				}

				if (recv.session_id != session_id) {
					WARN_LOG(COMMON, "invalid session_id");
					continue;
				}

				if (recv.to_peer_id != peer_id) {
					WARN_LOG(COMMON, "invalid to_peer_id");
					continue;
				}

				if (recv.from_peer_id == recv.to_peer_id) {
					WARN_LOG(COMMON, "invalid peer_id");
					continue;
				}

				if (recv.type == PING) {
					DEBUG_LOG(COMMON, "Recv PING from %d", recv.from_peer_id);
					std::lock_guard<std::recursive_mutex> lock(mutex_);

					Packet p{};
					p.magic = MAGIC;
					p.type = PONG;
					p.session_id = session_id;
					p.from_peer_id = peer_id;
					p.to_peer_id = recv.from_peer_id;
					p.candidate_idx = recv.candidate_idx;
					p.send_timestamp =
						std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now().time_since_epoch())
							.count();
					p.ping_timestamp = recv.send_timestamp;
					p.ping_timestamp -= network_delay;
					memcpy(p.rtt_matrix, rtt_matrix_, sizeof(rtt_matrix_));

					auto it = std::find_if(candidates_.begin(), candidates_.end(), [&recv, &sender](const Candidate &c) {
						return c.peer_id == recv.from_peer_id && is_same_addr(sender, c.remote.net_addr());
					});
					if (it != candidates_.end()) {
						client_.SendTo(reinterpret_cast<const char *>(&p), sizeof(p), it->remote);
					} else {
						Candidate c{};
						if (c.remote.Open(sender, addrlen)) {
							c.peer_id = recv.from_peer_id;
							candidates_.push_back(c);
							client_.SendTo(reinterpret_cast<const char *>(&p), sizeof(p), c.remote);
						}
					}
				}

				if (recv.type == PONG) {
					std::lock_guard<std::recursive_mutex> lock(mutex_);
					const auto now =
						std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now().time_since_epoch())
							.count();
					auto rtt = static_cast<int>(now - recv.ping_timestamp);
					if (rtt <= 0) rtt = 1;
					DEBUG_LOG(COMMON, "Recv PONG from Peer%d %d[ms] %s", recv.from_peer_id, rtt,
							  mask_ip_address(sockaddr_to_string(sender)).c_str());

					// Pong may come from an address different from one which Ping sent, so update the pong_count based on candidate_idx
					if (recv.candidate_idx < candidates_.size() && candidates_[recv.candidate_idx].peer_id == recv.from_peer_id) {
						auto &c = candidates_[recv.candidate_idx];
						c.rtt = float(c.pong_count * c.rtt + rtt) / float(c.pong_count + 1);
						c.pong_count++;
						rtt_matrix_[peer_id][recv.from_peer_id] = static_cast<uint8_t>(std::min(255, (int)std::ceil(c.rtt)));
						for (int j = 0; j < N; j++) {
							rtt_matrix_[recv.from_peer_id][j] = recv.rtt_matrix[recv.from_peer_id][j];
						}
					}

					// if the remote address not in candidates, add this.
					auto it = std::find_if(candidates_.begin(), candidates_.end(), [&recv, &sender](const Candidate &c) {
						return c.peer_id == recv.from_peer_id && is_same_addr(sender, c.remote.net_addr());
					});
					if (it == candidates_.end()) {
						Candidate c{};
						if (c.remote.Open(sender, addrlen)) {
							c.peer_id = recv.from_peer_id;
							candidates_.push_back(c);
						}
					}
				}
			}

			if (elapsed_ms + 500 < duration_ms && loop_count % 100 == 0) {
				std::lock_guard<std::recursive_mutex> lock(mutex_);
				for (int i = 0; i < std::min<int>(255, candidates_.size()); i++) {
					auto &c = candidates_[i];
					DEBUG_LOG(COMMON, "Send PING to Peer%d %s", c.peer_id, c.remote.masked_addr().c_str());
					if (c.remote.is_open()) {
						Packet p{};
						p.magic = MAGIC;
						p.type = PING;
						p.session_id = session_id;
						p.from_peer_id = peer_id;
						p.to_peer_id = c.peer_id;
						p.candidate_idx = i;
						p.send_timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
											   std::chrono::high_resolution_clock::now().time_since_epoch())
											   .count();
						p.send_timestamp -= network_delay;
						p.ping_timestamp = 0;
						memcpy(p.rtt_matrix, rtt_matrix_, sizeof(rtt_matrix_));
						client_.SendTo(reinterpret_cast<const char *>(&p), sizeof(p), c.remote);
						c.ping_count++;
					}
				}
			}

			if (duration_ms < elapsed_ms) {
				break;
			}

			sleep_us(1000);
		}

		PrintRttMatrix();

		NOTICE_LOG(COMMON, "End UdpPingPong Thread");
		client_.Close();
		running_ = false;
	}).detach();
}

void UdpPingPong::Stop() { running_ = false; }

void UdpPingPong::Reset() {
	running_ = false;
	start_time_ = std::chrono::high_resolution_clock::time_point{};
	client_.Close();

	std::lock_guard<std::recursive_mutex> lock(mutex_);
	memset(rtt_matrix_, 0, sizeof(rtt_matrix_));
	candidates_.clear();
	user_to_peer_.clear();
	peer_to_user_.clear();
}

bool UdpPingPong::Running() const { return running_; }

int UdpPingPong::ElapsedMs() const {
	auto now = std::chrono::high_resolution_clock::now();
	return std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time_).count();
}

void UdpPingPong::AddCandidate(const std::string &user_id, uint8_t peer_id, const std::string &ip, int port) {
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	user_to_peer_[user_id] = peer_id;
	peer_to_user_[peer_id] = user_id;
	Candidate c{};
	c.peer_id = peer_id;
	if (c.remote.Open(ip.c_str(), port)) {
		candidates_.emplace_back(c);
	}
}

bool UdpPingPong::GetAvailableAddress(uint8_t peer_id, sockaddr_storage *dst, float *rtt) {
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	std::vector<std::pair<float, int>> score_to_index;

	for (int i = 0; i < candidates_.size(); i++) {
		const auto &c = candidates_[i];
		if (c.peer_id == peer_id && 0 < c.pong_count && 0 < c.rtt) {
			float score = 10000.f - c.rtt;
			if (is_loopback_addr(c.remote.net_addr())) {
				score += 100.f;
			}
			if (is_private_addr(c.remote.net_addr())) {
				score += 50.f;
			}
			if (c.remote.is_v6()) {
				score += 20.f;
			}
			score_to_index.emplace_back(score, i);
		}
	}

	if (score_to_index.empty()) {
		return false;
	}

	const int i = std::max_element(score_to_index.begin(), score_to_index.end())->second;
	const auto &c = candidates_[i];
	memset(dst, 0, sizeof(sockaddr_storage));
	memcpy(dst, c.remote.net_addr(), c.remote.net_addr_len());
	*rtt = c.rtt;
	return true;
}

void UdpPingPong::GetRttMatrix(uint8_t matrix[N][N]) {
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	memcpy(matrix, rtt_matrix_, sizeof(rtt_matrix_));
}

void UdpPingPong::PrintRttMatrix() {
	std::lock_guard<std::recursive_mutex> lock(mutex_);

	NOTICE_LOG(COMMON, "UdpPingTest Finish");
	NOTICE_LOG(COMMON, "RTT MATRIX");
	NOTICE_LOG(COMMON, "  %4d%4d%4d%4d", 0, 1, 2, 3);
	for (int i = 0; i < 4; i++) {
		NOTICE_LOG(COMMON, "%d>%4d%4d%4d%4d", i, rtt_matrix_[i][0], rtt_matrix_[i][1], rtt_matrix_[i][2], rtt_matrix_[i][3]);
	}

	NOTICE_LOG(COMMON, "CANDIDATES");
	for (const auto& c : candidates_) {
		NOTICE_LOG(COMMON, "[%s] Peer%d %s: ping=%d pong=%d rtt=%.2f addr=%s", 0 < c.pong_count ? "x" : " ", c.peer_id,
			peer_to_user_[c.peer_id].c_str(), c.ping_count, c.pong_count, c.rtt, c.remote.masked_addr().c_str());
	}
}

void UdpPingPong::DebugUnreachable(uint8_t peer_id, uint8_t remote_peer_id) {
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	rtt_matrix_[peer_id][remote_peer_id] = 0;
	if (peer_id != peer_id_) return;
	for (auto& c : candidates_) {
		if (c.peer_id == remote_peer_id) {
			c.pong_count = 0;
			c.rtt = 0;
		}
	}
}

void UdpPingPong::DebugSetRtt(uint8_t peer_id, uint8_t remote_peer_id, uint8_t rtt) {
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	rtt_matrix_[peer_id][remote_peer_id] = rtt;
	if (peer_id != peer_id_) return;
	for (auto& c : candidates_) {
		if (c.peer_id == remote_peer_id) {
			c.rtt = rtt;
		}
	}
}
