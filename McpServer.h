/** \file
	\brief MCP server (Streamable HTTP transport, JSON responses, no SSE stream)

	Endpoint: POST http://127.0.0.1:<port>/mcp
	Required header: Authorization: Bearer <token>
*/
#ifndef McpServerH
#define McpServerH

#include "Config.h"
#include "HttpServer.h"
#include <json/json.h>
#include <string>

class McpServer
{
public:
	McpServer(void);
	~McpServer(void);

	/** \return 0 on success */
	int Start(const Config &cfg, std::string &errorText);
	void Stop(void);

private:
	void HandleHttp(const HttpRequest &req, HttpResponse &resp);
	/** \return true if response should be sent (request with id) */
	bool HandleRpc(const Json::Value &request, Json::Value &response);

	bool IsAuthorized(const HttpRequest &req) const;
	bool IsLocalHostHeader(const std::string &value) const;

	Config cfg;
	HttpServer http;
};

#endif
