/** \file
	\brief tSIP MCP server plugin - plugin interface v2 entry point

	MCP (Model Context Protocol) server exposing tSIP functions as tools for
	AI clients. Streamable HTTP transport on 127.0.0.1, bearer token auth.
	See README.md.
*/

#include "Activity.h"
#include "CallWatch.h"
#include "Config.h"
#include "Host.h"
#include "SettingsDialog.h"
#include "McpServer.h"
#include "tSIP_plugin_v2.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{

const struct TSIP_HOST_API *host = NULL;
McpServer *server = NULL;
Config config;
std::string configPath;
bool serverRunning = false;
bool started = false;	///< Start() called by host, Stop() not called yet

/** \brief Config file location: tSIP profile directory (as reported by host),
	DLL path with .cfg extension as fallback */
std::string DetermineConfigPath(void)
{
	Json::Value result;
	if (HostInvoke("GetAppInfo", Json::Value(Json::objectValue), result) == TSIP_OK &&
		result["profileDir"].isString() && !result["profileDir"].asString().empty())
	{
		// UTF-8 from host; ANSI file API used for config - convert
		std::string dir = result["profileDir"].asString();
		int wlen = MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), -1, NULL, 0);
		if (wlen > 0)
		{
			std::wstring wdir(wlen, L'\0');
			MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), -1, &wdir[0], wlen);
			int len = WideCharToMultiByte(CP_ACP, 0, wdir.c_str(), -1, NULL, 0, NULL, NULL);
			if (len > 0)
			{
				std::string adir(len, '\0');
				WideCharToMultiByte(CP_ACP, 0, wdir.c_str(), -1, &adir[0], len, NULL, NULL);
				adir.resize(len - 1);
				return adir + "\\MCP.cfg";
			}
		}
	}
	return GetDllConfigPath();
}

std::string GetUrl(void)
{
	char buf[64];
	snprintf(buf, sizeof(buf), "http://127.0.0.1:%u/mcp", config.port);
	return buf;
}

const char* TSIP_V2_CALL GetMetadata(void)
{
	return "{\"name\":\"MCP server\",\"version\":\"0.1\","
		"\"description\":\"Model Context Protocol server: tSIP functions as tools for AI clients\"}";
}

int TSIP_V2_CALL Start(void)
{
	started = true;
	config = Config();	// plugin may be restarted: do not keep values from previous run
	configPath = DetermineConfigPath();
	const char *cfgPath = configPath.c_str();
	int loadStatus = config.Load(configPath);
	if (loadStatus == Config::LOAD_INVALID)
	{
		// do not overwrite user's file (e.g. JSON syntax error) with defaults
		HostLog("config file %s is invalid (not a JSON object?) - fix or delete it; server not started", cfgPath);
		return -1;
	}
	if (loadStatus == Config::LOAD_NOT_FOUND)
		HostLog("config file %s not found, using defaults", cfgPath);
	if (config.EnsureToken())
	{
		if (config.Save(configPath) == 0)
			HostLog("generated new access token, saved to %s", cfgPath);
		else
			HostLog("failed to save config to %s", cfgPath);
	}

	CallWatch::Start();
	server = new McpServer();
	std::string error;
	if (server->Start(config, error) != 0)
	{
		HostLog("failed to start server: %s", error.c_str());
		delete server;
		server = NULL;
		serverRunning = false;
		return -1;
	}
	serverRunning = true;
	Activity::Start(config);
	HostLog("server listening on %s (confirm calls: %s)", GetUrl().c_str(), config.confirmCalls ? "yes" : "no");
	return 0;
}

int TSIP_V2_CALL Stop(void)
{
	started = false;
	// wake threads waiting in wait_for_call first - otherwise joining them
	// would block tSIP main thread until wait timeout
	CallWatch::Shutdown();
	if (server)
	{
		server->Stop();	// joins all server threads
		delete server;
		server = NULL;
	}
	Activity::Stop();
	serverRunning = false;
	HostLog("server stopped");
	return 0;
}

void TSIP_V2_CALL Shutdown(void)
{
	host = NULL;
}

void TSIP_V2_CALL OnEvent(const char *type, const char *json)
{
	if (type && strcmp(type, "call_state") == 0)
		CallWatch::OnCallStateEvent(json);
}

void TSIP_V2_CALL ShowSettings(void *parentHwnd)
{
	HWND parent = static_cast<HWND>(parentHwnd);
	if (configPath.empty())
		configPath = DetermineConfigPath();
	std::string status = serverRunning ? ("running, " + GetUrl()) : std::string("not running (see tSIP log)");

	// edit copy of current config file contents (not values from running server only)
	Config cfg;
	if (cfg.Load(configPath) == Config::LOAD_INVALID)
	{
		if (MessageBoxA(parent, ("Config file " + configPath + " is invalid.\n\n"
			"Edit default settings and overwrite it?").c_str(), "MCP plugin settings",
			MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
		{
			return;
		}
		cfg = Config();
	}
	cfg.EnsureToken();

	if (!SettingsDialog::Show(parent, cfg, status, configPath))
		return;
	if (cfg.Save(configPath) != 0)
	{
		MessageBoxA(parent, ("Failed to save " + configPath).c_str(), "MCP plugin settings", MB_ICONERROR);
		return;
	}
	HostLog("settings saved to %s", configPath.c_str());
	if (started)
	{
		// apply: host restarts plugin (Stop + Start) after returning from ShowSettings;
		// restarting here directly could block on threads waiting for main thread
		Json::Value result;
		if (HostInvoke("RestartPlugin", Json::Value(Json::objectValue), result) != TSIP_OK)
		{
			MessageBoxA(parent, "Settings saved. Disable and enable the plugin to apply them "
				"(this tSIP version cannot restart plugins).", "MCP plugin settings", MB_ICONINFORMATION);
		}
	}
}

}	// namespace


void HostLog(const char *fmt, ...)
{
	const struct TSIP_HOST_API *h = host;
	if (!h || !TSIP_HOST_HAS(h, Log) || !h->Log)
		return;
	char buf[2048] = "MCP: ";
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf + 5, sizeof(buf) - 7, fmt, ap);
	va_end(ap);
	buf[sizeof(buf) - 2] = '\0';
	strcat(buf, "\n");
	h->Log(h->cookie, buf);
}

int HostInvoke(const char *method, const Json::Value &params, Json::Value &result)
{
	const struct TSIP_HOST_API *h = host;
	if (!h || !TSIP_HOST_HAS(h, Invoke) || !h->Invoke)
		return TSIP_ERR_NOT_AVAILABLE;

	Json::FastWriter writer;
	std::string jsonParams = writer.write(params);
	if (!jsonParams.empty() && jsonParams[jsonParams.size() - 1] == '\n')
		jsonParams.erase(jsonParams.size() - 1);
	char *jsonResult = NULL;
	int status = h->Invoke(h->cookie, method, jsonParams.c_str(), &jsonResult);
	result = Json::Value(Json::objectValue);
	if (jsonResult)
	{
		Json::Reader reader;
		reader.parse(jsonResult, result);
		if (TSIP_HOST_HAS(h, Free) && h->Free)
			h->Free(h->cookie, jsonResult);
	}
	return status;
}

bool HostSetVariable(const char *name, const std::string &value)
{
	const struct TSIP_HOST_API *h = host;
	if (!h || !TSIP_HOST_HAS(h, SetVariable) || !h->SetVariable)
		return false;
	return h->SetVariable(h->cookie, name, value.c_str()) == TSIP_OK;
}

const char* HostErrorName(int status)
{
	switch (status)
	{
	case TSIP_OK: return "OK";
	case TSIP_ERR_GENERIC: return "generic error";
	case TSIP_ERR_INVALID_ARG: return "invalid argument";
	case TSIP_ERR_UNKNOWN_METHOD: return "unknown method (tSIP version too old?)";
	case TSIP_ERR_TIMEOUT: return "timeout";
	case TSIP_ERR_NOT_AVAILABLE: return "not available (e.g. no such call)";
	default: return "unknown error";
	}
}

extern "C" __declspec(dllexport) int TSIP_V2_CALL tSipPluginInit(const struct TSIP_HOST_API *hostApi, struct TSIP_PLUGIN_API *plugin)
{
	if (hostApi == NULL || plugin == NULL)
		return -1;
	if (!TSIP_HOST_HAS(hostApi, Log) || !TSIP_HOST_HAS(hostApi, Invoke) || !TSIP_HOST_HAS(hostApi, Free))
		return -2;
	host = hostApi;

	if (TSIP_PLUGIN_HAS(plugin, GetMetadata))
		plugin->GetMetadata = GetMetadata;
	if (TSIP_PLUGIN_HAS(plugin, Start))
		plugin->Start = Start;
	if (TSIP_PLUGIN_HAS(plugin, Stop))
		plugin->Stop = Stop;
	if (TSIP_PLUGIN_HAS(plugin, Shutdown))
		plugin->Shutdown = Shutdown;
	if (TSIP_PLUGIN_HAS(plugin, OnEvent))
		plugin->OnEvent = OnEvent;
	if (TSIP_PLUGIN_HAS(plugin, ShowSettings))
		plugin->ShowSettings = ShowSettings;
	return 0;
}
