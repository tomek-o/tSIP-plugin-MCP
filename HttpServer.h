/** \file
	\brief Minimal HTTP/1.1 server for local MCP transport

	- listens on 127.0.0.1 only,
	- one thread per connection, keep-alive supported,
	- request body only with Content-Length (no chunked encoding),
	- Stop() closes all sockets and joins all threads.
*/
#ifndef HttpServerH
#define HttpServerH

#include <winsock2.h>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct HttpRequest
{
	std::string method;
	std::string path;
	std::map<std::string, std::string> headers;	///< lowercase header names
	std::string body;

	std::string GetHeader(const std::string &lowercaseName) const;
};

struct HttpResponse
{
	int status;
	std::string contentType;
	std::string body;
	std::vector<std::pair<std::string, std::string> > headers;	///< additional headers
	HttpResponse(void):
		status(200)
	{}
};

class HttpServer
{
public:
	typedef std::function<void(const HttpRequest&, HttpResponse&)> Handler;

	HttpServer(void);
	~HttpServer(void);

	/** \return 0 on success */
	int Start(unsigned short port, Handler handler, std::string &errorText);
	void Stop(void);

private:
	struct Connection
	{
		SOCKET sock;
		std::thread thread;
		bool finished;
	};

	void AcceptLoop(void);
	void ServeConnection(Connection *conn);
	void ReapFinished(void);

	SOCKET listenSock;
	std::thread acceptThread;
	Handler handler;
	std::mutex mutex;
	std::vector<Connection*> connections;
	volatile bool running;
	bool wsaStarted;

	HttpServer(const HttpServer&);
	HttpServer& operator=(const HttpServer&);
};

#endif
