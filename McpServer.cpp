#include "McpServer.h"
#include "Host.h"
#include "Prompts.h"
#include "Tools.h"

#include <algorithm>
#include <cctype>

namespace
{

const char* const SERVER_NAME = "tSIP";
const char* const SERVER_VERSION = "0.1";

/** \brief Supported protocol versions, newest first */
const char* const PROTOCOL_VERSIONS[] = { "2025-06-18", "2025-03-26", "2024-11-05" };

enum
{
	JSONRPC_PARSE_ERROR = -32700,
	JSONRPC_INVALID_REQUEST = -32600,
	JSONRPC_METHOD_NOT_FOUND = -32601,
	JSONRPC_INVALID_PARAMS = -32602
};

std::string ToLower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

std::string ToJson(const Json::Value &jv)
{
	Json::FastWriter writer;
	std::string text = writer.write(jv);
	if (!text.empty() && text[text.size() - 1] == '\n')
		text.erase(text.size() - 1);
	return text;
}

Json::Value ErrorResponse(const Json::Value &id, int code, const std::string &message)
{
	Json::Value error(Json::objectValue);
	error["code"] = code;
	error["message"] = message;
	Json::Value resp(Json::objectValue);
	resp["jsonrpc"] = "2.0";
	resp["id"] = id;
	resp["error"] = error;
	return resp;
}

/** \brief Compare without early exit (token check) */
bool ConstantTimeEquals(const std::string &a, const std::string &b)
{
	if (a.size() != b.size())
		return false;
	unsigned char diff = 0;
	for (size_t i = 0; i < a.size(); i++)
		diff |= (unsigned char)(a[i] ^ b[i]);
	return diff == 0;
}

/** \brief Host name from "host:port" or "scheme://host:port/path" */
std::string ExtractHostName(std::string value)
{
	size_t scheme = value.find("://");
	if (scheme != std::string::npos)
		value = value.substr(scheme + 3);
	size_t slash = value.find('/');
	if (slash != std::string::npos)
		value = value.substr(0, slash);
	if (!value.empty() && value[0] == '[')
	{
		size_t end = value.find(']');
		return ToLower(value.substr(0, end == std::string::npos ? value.size() : end + 1));
	}
	size_t colon = value.find(':');
	if (colon != std::string::npos)
		value = value.substr(0, colon);
	return ToLower(value);
}

}	// namespace

McpServer::McpServer(void)
{
}

McpServer::~McpServer(void)
{
	Stop();
}

int McpServer::Start(const Config &cfg, std::string &errorText)
{
	this->cfg = cfg;
	return http.Start((unsigned short)cfg.port,
		[this](const HttpRequest &req, HttpResponse &resp) { HandleHttp(req, resp); },
		errorText);
}

void McpServer::Stop(void)
{
	http.Stop();
}

bool McpServer::IsLocalHostHeader(const std::string &value) const
{
	std::string host = ExtractHostName(value);
	return host == "127.0.0.1" || host == "localhost" || host == "[::1]";
}

bool McpServer::IsAuthorized(const HttpRequest &req) const
{
	std::string auth = req.GetHeader("authorization");
	const std::string prefix = "Bearer ";
	if (auth.size() <= prefix.size() || ToLower(auth.substr(0, prefix.size())) != "bearer ")
		return false;
	return ConstantTimeEquals(auth.substr(prefix.size()), cfg.token);
}

void McpServer::HandleHttp(const HttpRequest &req, HttpResponse &resp)
{
	// DNS rebinding protection: only local Host / Origin
	std::string host = req.GetHeader("host");
	std::string origin = req.GetHeader("origin");
	if ((!host.empty() && !IsLocalHostHeader(host)) || (!origin.empty() && !IsLocalHostHeader(origin)))
	{
		HostLog("rejected request with Host [%s], Origin [%s]", host.c_str(), origin.c_str());
		resp.status = 403;
		return;
	}

	if (req.path != "/mcp" && req.path != "/mcp/")
	{
		resp.status = 404;
		return;
	}

	if (!IsAuthorized(req))
	{
		HostLog("rejected unauthorized request (missing or invalid bearer token)");
		resp.status = 401;
		resp.headers.push_back(std::make_pair("WWW-Authenticate", "Bearer"));
		return;
	}

	if (req.method != "POST")
	{
		// no server-initiated SSE stream (GET) and no session termination (DELETE)
		resp.status = 405;
		resp.headers.push_back(std::make_pair("Allow", "POST"));
		return;
	}

	Json::Value request;
	Json::Reader reader;
	if (!reader.parse(req.body, request))
	{
		resp.status = 400;
		resp.contentType = "application/json";
		resp.body = ToJson(ErrorResponse(Json::Value(), JSONRPC_PARSE_ERROR, "Parse error"));
		return;
	}
	if (request.isArray())
	{
		resp.status = 400;
		resp.contentType = "application/json";
		resp.body = ToJson(ErrorResponse(Json::Value(), JSONRPC_INVALID_REQUEST, "Batch requests are not supported"));
		return;
	}

	Json::Value response;
	if (HandleRpc(request, response))
	{
		resp.status = 200;
		resp.contentType = "application/json";
		resp.body = ToJson(response);
	}
	else
	{
		resp.status = 202;	// notification or client response: accepted, no body
	}
}

bool McpServer::HandleRpc(const Json::Value &request, Json::Value &response)
{
	if (!request.isObject() || request["jsonrpc"] != "2.0")
	{
		response = ErrorResponse(Json::Value(), JSONRPC_INVALID_REQUEST, "Invalid Request");
		return true;
	}
	if (!request.isMember("id") || !request.isMember("method"))
	{
		// notification (e.g. notifications/initialized) or response from client
		return false;
	}

	const Json::Value &id = request["id"];
	const std::string method = request["method"].isString() ? request["method"].asString() : "";
	const Json::Value &params = request["params"];

	Json::Value result(Json::objectValue);

	if (method == "initialize")
	{
		std::string requested = params["protocolVersion"].isString() ? params["protocolVersion"].asString() : "";
		std::string version = PROTOCOL_VERSIONS[0];
		for (size_t i = 0; i < sizeof(PROTOCOL_VERSIONS) / sizeof(PROTOCOL_VERSIONS[0]); i++)
		{
			if (requested == PROTOCOL_VERSIONS[i])
			{
				version = requested;
				break;
			}
		}
		Json::Value tools(Json::objectValue);
		tools["listChanged"] = false;
		Json::Value prompts(Json::objectValue);
		prompts["listChanged"] = false;
		Json::Value capabilities(Json::objectValue);
		capabilities["tools"] = tools;
		capabilities["prompts"] = prompts;
		Json::Value serverInfo(Json::objectValue);
		serverInfo["name"] = SERVER_NAME;
		serverInfo["version"] = SERVER_VERSION;

		result["protocolVersion"] = version;
		result["capabilities"] = capabilities;
		result["serverInfo"] = serverInfo;
		result["instructions"] =
			"Controls the tSIP softphone running on this PC: calls (make, answer, hang up, hold, mute, transfer, "
			"DTMF, recording, waiting for call state), call history, phonebook, text messages. "
			"Making calls, transfers and sending messages may require confirmation by the user and some numbers "
			"may be blocked by configuration. Use wait_for_call instead of polling get_calls.";

		std::string clientName = params["clientInfo"]["name"].isString() ? params["clientInfo"]["name"].asString() : "?";
		HostLog("initialize from client [%s], protocol version %s", clientName.c_str(), version.c_str());
	}
	else if (method == "ping")
	{
		// empty result
	}
	else if (method == "tools/list")
	{
		result["tools"] = Tools::List(cfg);
	}
	else if (method == "tools/call")
	{
		if (!params["name"].isString())
		{
			response = ErrorResponse(id, JSONRPC_INVALID_PARAMS, "Missing tool name");
			return true;
		}
		std::string name = params["name"].asString();
		Json::Value args = params["arguments"].isObject() ? params["arguments"] : Json::Value(Json::objectValue);
		HostLog("tools/call %s %s", name.c_str(), ToJson(args).c_str());

		bool found = false;
		result = Tools::Call(cfg, name, args, found);
		if (!found)
		{
			response = ErrorResponse(id, JSONRPC_INVALID_PARAMS, "Unknown tool: " + name);
			return true;
		}
	}
	else if (method == "prompts/list")
	{
		result["prompts"] = Prompts::List(cfg);
	}
	else if (method == "prompts/get")
	{
		if (!params["name"].isString())
		{
			response = ErrorResponse(id, JSONRPC_INVALID_PARAMS, "Missing prompt name");
			return true;
		}
		std::string name = params["name"].asString();
		Json::Value args = params["arguments"].isObject() ? params["arguments"] : Json::Value(Json::objectValue);
		HostLog("prompts/get %s", name.c_str());
		bool found = false;
		std::string error;
		result = Prompts::Get(cfg, name, args, found, error);
		if (!found)
		{
			response = ErrorResponse(id, JSONRPC_INVALID_PARAMS, "Unknown prompt: " + name);
			return true;
		}
		if (!error.empty())
		{
			response = ErrorResponse(id, JSONRPC_INVALID_PARAMS, error);
			return true;
		}
	}
	else
	{
		response = ErrorResponse(id, JSONRPC_METHOD_NOT_FOUND, "Method not found: " + method);
		return true;
	}

	response = Json::Value(Json::objectValue);
	response["jsonrpc"] = "2.0";
	response["id"] = id;
	response["result"] = result;
	return true;
}
