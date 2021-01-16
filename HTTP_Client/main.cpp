#undef UNICODE

/* WINAPI stuff */
#define NOMINMAX
#include <Windows.h>
#include <winsock2.h>
#include <mstcpip.h>
#include <ws2tcpip.h>

/* WINAPI LIB */
#pragma comment (lib, "Ws2_32.lib")
#pragma comment (lib, "Fwpuclnt.lib")

/* STL stuff */
#include <exception>
#include <stdexcept>
#include <optional>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <memory>
#include <list>

#define DEFAULT_PORT "80"

class http_helper {
private:
	std::unique_ptr<std::string> raw_recieved_data;

	SOCKET		sock;
	addrinfo*	addr_info_ptr;

	std::string current_data;
	std::string host;
	std::string request;

	bool all_data_picked = false;

public:
	class socket_exception : public std::exception {
	private:
		std::string message;

	public:
		socket_exception(const std::string& context, const int error_code) : message(error_message(context, error_code)) { }

	public:
		std::string error_message(const std::string& context, const int error_code) {
			char buf[1024];

			FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, 0, error_code, 0, buf, sizeof(buf), 0);

			char* new_line = strchr(buf, '\r');
			if (new_line) *new_line = '\0';

			std::ostringstream oss;
			oss << "[Type] "        << type()     << std::endl
				<< "[Context] "     << context    << std::endl
				<< "[Error code] "  << error_code << std::endl
				<< "[Description] " << buf << std::endl;
				
			return oss.str();
		}
		const char* what() const noexcept override { return message.c_str(); }
		const char* type() const noexcept { return "Socket error"; }
	};
	
public:
	http_helper(const std::string& host, const std::string& request) : host(host), request(request) {
		WSADATA wsa_data;
		int result;

		/* Initialize Winsock */
		result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
		if (result)
			throw socket_exception("WSAStartUp()", result);

		/* Resolve the server address and port */
		result = getaddrinfo(host.c_str(), DEFAULT_PORT, 0, &addr_info_ptr);
		if (result)
			throw socket_exception("getaddrinfo()", result);

		/* Create the socket */
		create_socket();

		/* Connect to server */
		connect();

		/* Send request */
		send();
	}
	~http_helper() {
		freeaddrinfo(addr_info_ptr);
	}

public:
	void save() {
		if (!all_data_picked) {
			throw std::runtime_error("Saving with unfilled data buffer.");
		} else {
			std::string saved = std::move(current_data);
			raw_recieved_data = std::make_unique<std::string>(saved);
			current_data.clear();

			all_data_picked = false;
		}
	}
	bool empty() {
		if (current_data.empty())
			get_some();

		return get_content_size(current_data) > 0;
	}
	bool end_of_data() { return all_data_picked; }
	void get_some() {
		int result;
		char buffer[128];

		if (!all_data_picked) {
			result = recv(sock, buffer, sizeof(buffer), 0);
			if (result == SOCKET_ERROR)
				throw socket_exception("recv()", WSAGetLastError());
			if (result == 0) {
				all_data_picked = true;
				return;
			}
		}

		current_data += std::string(buffer, result);
	}
	void get_all() {
		while (!all_data_picked)
			get_some();
	}
	std::optional<std::string> get_raw_data() const noexcept {
		if (raw_recieved_data)
			return *raw_recieved_data;
		return { };
	}
	std::optional<std::string> get_data() const noexcept {
		if (raw_recieved_data)
			return remove_header(*raw_recieved_data);
		return { };
	}
	void close_and_cleanup() {
		close();
		cleanup();
	}
	void close() {
		int result = closesocket(sock);
		if (result == SOCKET_ERROR)
			throw socket_exception("closesocket()", WSAGetLastError());
	}
	void cleanup() {
		int result = WSACleanup();
		if (result == SOCKET_ERROR)
			throw socket_exception("WSACleanup()", WSAGetLastError());
	}
	void connect() {
		if (::connect(sock, addr_info_ptr->ai_addr, addr_info_ptr->ai_addrlen) != 0)
			throw socket_exception("connect()", WSAGetLastError());
	}
	void send() {
		if (::send(sock, request.c_str(), request.size(), 0) == SOCKET_ERROR)
			throw socket_exception("send()", WSAGetLastError());
		if (shutdown(sock, SD_SEND) == SOCKET_ERROR) {
			throw socket_exception("shutdown()", WSAGetLastError());
		}
	}
	void re_send() {
		create_socket();
		connect();
		send();
	}
	void send_and_connect() {
		send();
		connect();
	}
	void create_socket() {
		sock = socket(addr_info_ptr->ai_family, addr_info_ptr->ai_socktype, addr_info_ptr->ai_protocol);
		if (sock == INVALID_SOCKET)
			throw socket_exception("socket()", WSAGetLastError());
	}

public:
	static std::string make_request(const std::string& host, const std::string& path) {
		std::ostringstream oss;
		oss << "GET " << path << " HTTP/1.1\r\nHost: " << host << "\r\n\r\n";
		return oss.str();
	}
	static std::string remove_header(const std::string& data) {
		char* res;

		auto data_size_ptr = get_content_size_noexcept(data);
		if (data_size_ptr != nullptr) {
			auto data_size = *data_size_ptr;
			if (data_size != 0) {
				auto actual_data_ptr = strstr(data.c_str(), "\r\n\r\n") + 4;
				res = new char[data_size];

				for (int i = 0; actual_data_ptr != data.end()._Ptr; actual_data_ptr++, i++)
					res[i] = *actual_data_ptr;
				res[data_size - 1] = '\0';
			}
		}

		return res;
	}
	static int get_content_size(const std::string& data) {
		if (auto res = get_content_size_noexcept(data))
			return *res;
		else
			throw std::runtime_error("\"Content-Length: \" not found!");
	}

private:
	static int* get_content_size_noexcept(const std::string& data) noexcept {
		auto content_lenght_ptr = strstr(data.c_str(), "Content-Length: ");

		if (content_lenght_ptr) {
			content_lenght_ptr += 16;

			std::string size_str;
			for (; *content_lenght_ptr != '\n'; content_lenght_ptr++) {
				size_str.push_back(*content_lenght_ptr);
			}

			auto res = std::stoi(size_str);
			return &res;
		}
		return nullptr;
	}
};

void error_window(const char* title, const char* content) {
	MessageBoxA(nullptr, content, title, MB_OK | MB_ICONERROR);
}
void error_window(const std::exception& e) {
	error_window("Standard exception", e.what());
}

int main() {
	const std::string host    = "gameprogrammingpatterns.com";
	const std::string request = "/contents.html";

	http_helper* html;
	std::list<std::string> html_str_buffer;

	try {
		html = new http_helper(host, http_helper::make_request(host, request));
	} catch (const std::exception& e) {
		error_window(e);
	}

	bool end_of_getting_data = false;
	int get_count = 0;

	std::thread second_thread([&]() {
		try {
			while (!end_of_getting_data) {
				if (get_count < 100) {
					html->re_send();
					while (!html->end_of_data())
						html->get_some();

					html->save();
					if (auto data = html->get_data()) {
						html_str_buffer.push_back(*data);
						get_count++;
					}
				} else {
					html_str_buffer.pop_back();
					get_count--;
				}
			}
		} catch (const std::exception& e) {
			error_window(e);
		}
	});

	/* First thread */
	std::string current;
	std::string prev = "";
	for (;;) {
		std::string command;
		std::cout << "Input> ";
		std::cin >> command;
		if (command == "exit") {
			end_of_getting_data = true;
			break;
		}
		else if (command == "get") {
			std::cin >> command;
			if (command == "data") {
				if (html_str_buffer.size() > 0) {
					current = std::move(html_str_buffer.front());
					html_str_buffer.pop_front();

					if (current != prev)
						std::cout << current << std::endl;
					else
						std::cout << "Output> Same output (nothing changed).\n";

					prev = current;
				}
				else
					std::cout << "Output> Buffer is empty.\n";
			}
			else if (command == "data_anyway") {
				if (html_str_buffer.size() > 0) {
					current = std::move(html_str_buffer.front());
					html_str_buffer.pop_front();

					std::cout << current << std::endl;

					prev = current;
				}
				else
					std::cout << "Output> Buffer is empty.\n";
			}
			else if (command == "response_count") {
				std::cout << "Output> " << get_count << std::endl;
			}
		}
	}

	second_thread.join();

	try {
		html->close_and_cleanup();
	} catch (const std::exception& e) {
		error_window(e);
	}
	delete html;
}