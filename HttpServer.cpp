#include "HttpServer.h"

#include <ws2tcpip.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace
{

enum
{
	MAX_HEADER_SIZE = 16 * 1024,
	MAX_BODY_SIZE = 1024 * 1024,
	IDLE_TIMEOUT_MS = 30000,
	MAX_CONNECTIONS = 16
};

std::string ToLower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

std::string Trim(const std::string &s)
{
	size_t b = s.find_first_not_of(" \t");
	if (b == std::string::npos)
		return "";
	size_t e = s.find_last_not_of(" \t");
	return s.substr(b, e - b + 1);
}

const char* StatusText(int status)
{
	switch (status)
	{
	case 200: return "OK";
	case 202: return "Accepted";
	case 204: return "No Content";
	case 400: return "Bad Request";
	case 401: return "Unauthorized";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 411: return "Length Required";
	case 413: return "Payload Too Large";
	case 500: return "Internal Server Error";
	case 501: return "Not Implemented";
	case 503: return "Service Unavailable";
	default: return "Unknown";
	}
}

bool SendAll(SOCKET s, const std::string &data)
{
	size_t sent = 0;
	while (sent < data.size())
	{
		int n = send(s, data.data() + sent, (int)(data.size() - sent), 0);
		if (n <= 0)
			return false;
		sent += n;
	}
	return true;
}

bool SendResponse(SOCKET s, const HttpResponse &resp, bool keepAlive)
{
	std::string out;
	char line[128];
	snprintf(line, sizeof(line), "HTTP/1.1 %d %s\r\n", resp.status, StatusText(resp.status));
	out += line;
	if (!resp.contentType.empty())
		out += "Content-Type: " + resp.contentType + "\r\n";
	snprintf(line, sizeof(line), "Content-Length: %u\r\n", (unsigned int)resp.body.size());
	out += line;
	for (size_t i = 0; i < resp.headers.size(); i++)
		out += resp.headers[i].first + ": " + resp.headers[i].second + "\r\n";
	out += keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
	out += "\r\n";
	out += resp.body;
	return SendAll(s, out);
}

}	// namespace

std::string HttpRequest::GetHeader(const std::string &lowercaseName) const
{
	std::map<std::string, std::string>::const_iterator it = headers.find(lowercaseName);
	return (it != headers.end()) ? it->second : std::string();
}

HttpServer::HttpServer(void):
	listenSock(INVALID_SOCKET),
	running(false),
	wsaStarted(false)
{
}

HttpServer::~HttpServer(void)
{
	Stop();
}

int HttpServer::Start(unsigned short port, Handler handler, std::string &errorText)
{
	if (running)
		return 0;

	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		errorText = "WSAStartup failed";
		return -1;
	}
	wsaStarted = true;

	listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listenSock == INVALID_SOCKET)
	{
		errorText = "socket() failed";
		Stop();
		return -1;
	}

	// do not allow other process to take over the port
	BOOL exclusive = TRUE;
	setsockopt(listenSock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&exclusive, sizeof(exclusive));

	sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);	// local only
	addr.sin_port = htons(port);
	if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) != 0)
	{
		char buf[96];
		snprintf(buf, sizeof(buf), "bind() to 127.0.0.1:%u failed, error %d", port, WSAGetLastError());
		errorText = buf;
		Stop();
		return -1;
	}
	if (listen(listenSock, SOMAXCONN) != 0)
	{
		errorText = "listen() failed";
		Stop();
		return -1;
	}

	this->handler = handler;
	running = true;
	acceptThread = std::thread(&HttpServer::AcceptLoop, this);
	return 0;
}

void HttpServer::Stop(void)
{
	running = false;
	if (listenSock != INVALID_SOCKET)
	{
		closesocket(listenSock);	// unblocks accept()
		listenSock = INVALID_SOCKET;
	}
	if (acceptThread.joinable())
		acceptThread.join();

	std::vector<Connection*> conns;
	{
		std::lock_guard<std::mutex> lock(mutex);
		conns.swap(connections);
		for (size_t i = 0; i < conns.size(); i++)
		{
			// unblocks recv() in connection threads
			shutdown(conns[i]->sock, SD_BOTH);
			closesocket(conns[i]->sock);
			conns[i]->sock = INVALID_SOCKET;
		}
	}
	for (size_t i = 0; i < conns.size(); i++)
	{
		if (conns[i]->thread.joinable())
			conns[i]->thread.join();
		delete conns[i];
	}

	if (wsaStarted)
	{
		WSACleanup();
		wsaStarted = false;
	}
}

void HttpServer::ReapFinished(void)
{
	std::vector<Connection*> done;
	{
		std::lock_guard<std::mutex> lock(mutex);
		for (std::vector<Connection*>::iterator it = connections.begin(); it != connections.end(); )
		{
			if ((*it)->finished)
			{
				done.push_back(*it);
				it = connections.erase(it);
			}
			else
			{
				++it;
			}
		}
	}
	for (size_t i = 0; i < done.size(); i++)
	{
		if (done[i]->thread.joinable())
			done[i]->thread.join();
		delete done[i];
	}
}

void HttpServer::AcceptLoop(void)
{
	while (running)
	{
		SOCKET s = accept(listenSock, NULL, NULL);
		if (s == INVALID_SOCKET)
			break;	// listening socket closed by Stop()

		ReapFinished();

		std::lock_guard<std::mutex> lock(mutex);
		if (!running || connections.size() >= MAX_CONNECTIONS)
		{
			closesocket(s);
			continue;
		}
		DWORD timeout = IDLE_TIMEOUT_MS;
		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

		Connection *conn = new Connection;
		conn->sock = s;
		conn->finished = false;
		connections.push_back(conn);
		conn->thread = std::thread(&HttpServer::ServeConnection, this, conn);
	}
}

void HttpServer::ServeConnection(Connection *conn)
{
	SOCKET s = conn->sock;
	std::string buffer;
	char chunk[4096];

	while (running)
	{
		// read headers
		size_t headerEnd;
		while ((headerEnd = buffer.find("\r\n\r\n")) == std::string::npos)
		{
			if (buffer.size() > MAX_HEADER_SIZE)
				goto done;
			int n = recv(s, chunk, sizeof(chunk), 0);
			if (n <= 0)
				goto done;	// closed, error or idle timeout
			buffer.append(chunk, n);
		}

		HttpRequest req;
		HttpResponse resp;
		bool keepAlive = true;
		{
			std::string head = buffer.substr(0, headerEnd);
			buffer.erase(0, headerEnd + 4);

			size_t lineEnd = head.find("\r\n");
			std::string requestLine = head.substr(0, lineEnd);
			size_t sp1 = requestLine.find(' ');
			size_t sp2 = requestLine.find(' ', sp1 + 1);
			if (sp1 == std::string::npos || sp2 == std::string::npos)
			{
				resp.status = 400;
				SendResponse(s, resp, false);
				goto done;
			}
			req.method = requestLine.substr(0, sp1);
			req.path = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
			size_t q = req.path.find('?');
			if (q != std::string::npos)
				req.path = req.path.substr(0, q);

			size_t pos = (lineEnd == std::string::npos) ? head.size() : lineEnd + 2;
			while (pos < head.size())
			{
				size_t e = head.find("\r\n", pos);
				if (e == std::string::npos)
					e = head.size();
				std::string line = head.substr(pos, e - pos);
				size_t colon = line.find(':');
				if (colon != std::string::npos)
					req.headers[ToLower(Trim(line.substr(0, colon)))] = Trim(line.substr(colon + 1));
				pos = e + 2;
			}
		}

		if (ToLower(req.GetHeader("connection")) == "close")
			keepAlive = false;

		if (!req.GetHeader("transfer-encoding").empty())
		{
			resp.status = 501;	// chunked request body not supported
			SendResponse(s, resp, false);
			goto done;
		}

		{
			std::string cl = req.GetHeader("content-length");
			size_t contentLength = cl.empty() ? 0 : strtoul(cl.c_str(), NULL, 10);
			if (contentLength > MAX_BODY_SIZE)
			{
				resp.status = 413;
				SendResponse(s, resp, false);
				goto done;
			}
			while (buffer.size() < contentLength)
			{
				int n = recv(s, chunk, sizeof(chunk), 0);
				if (n <= 0)
					goto done;
				buffer.append(chunk, n);
			}
			req.body = buffer.substr(0, contentLength);
			buffer.erase(0, contentLength);
		}

		try
		{
			handler(req, resp);
		}
		catch (...)
		{
			resp = HttpResponse();
			resp.status = 500;
		}

		if (!SendResponse(s, resp, keepAlive) || !keepAlive)
			goto done;
	}

done:
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (conn->sock != INVALID_SOCKET)
		{
			closesocket(conn->sock);
			conn->sock = INVALID_SOCKET;
		}
		conn->finished = true;
	}
}
