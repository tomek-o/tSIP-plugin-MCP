/** \file
	\brief Test host for MCP plugin - loads MCP.dll like tSIP (plugin interface v2)
	with fake Invoke() results, so MCP protocol and tools can be tested without tSIP.

	Usage: TestHost.exe <path to MCP.dll> [seconds to run, default 60]
	Current directory is reported as tSIP profile directory - MCP.cfg (with token) is created there.
	While running, send requests e.g. with curl to http://127.0.0.1:<port>/mcp
	or run test/smoke_test.py.

	Fake behavior:
	- one incoming call (uid 5) is always present in GetCalls,
	- Call creates call uid 7: "call_state" event ESTABLISHED after 0.5 s,
	  Hangup of uid 7 sends CLOSED event,
	- RunScript: script containing "error" fails (as Lua error), otherwise returns
	  {"return":"42","output":"test output\n"},
	- PlayAudioFile: fake 1 s long file (GetCallAudioState audioErrorCount 0 -> 1),
	- GetCallAudioState rxLevel: alternating 1 s silence (100) / 1 s speech (5000),
	- unknown uid (other than 5 and 7) -> TSIP_ERR_NOT_AVAILABLE.
*/

#include "tSIP_plugin_v2.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{

struct TSIP_PLUGIN_API plugin;
CRITICAL_SECTION eventLock;		///< OnEvent calls serialized, as in tSIP (main thread)
bool call7Active = false;
DWORD audioPlayStart = 0;

void TSIP_V2_CALL Log(void *cookie, const char *text)
{
	(void)cookie;
	printf("[log] %s", text);
	fflush(stdout);
}

void SendCallState(unsigned int uid, int state, const char *stateName)
{
	char json[256];
	snprintf(json, sizeof(json), "{\"uid\":%u,\"state\":%d,\"stateName\":\"%s\",\"incoming\":false,"
		"\"peer\":\"sip:7@example.com\",\"peerName\":\"Test Peer\"}", uid, state, stateName);
	EnterCriticalSection(&eventLock);
	if (plugin.OnEvent)
		plugin.OnEvent("call_state", json);
	LeaveCriticalSection(&eventLock);
	printf("[event] call_state %s\n", json);
	fflush(stdout);
}

DWORD WINAPI EstablishThread(LPVOID)
{
	Sleep(500);
	if (call7Active)
		SendCallState(7, 6, "ESTABLISHED");
	return 0;
}

/** \brief uid from params: 0 = "current call" (uid 5 or 7) */
unsigned int GetUid(const char *jsonParams)
{
	const char *p = jsonParams ? strstr(jsonParams, "\"uid\":") : NULL;
	return p ? static_cast<unsigned int>(atoi(p + 6)) : 0;
}

bool ValidUid(unsigned int uid)
{
	return uid == 0 || uid == 5 || (uid == 7 && call7Active);
}

int TSIP_V2_CALL Invoke(void *cookie, const char *method, const char *jsonParams, char **jsonResult)
{
	(void)cookie;
	printf("[invoke] %s %s\n", method, jsonParams ? jsonParams : "NULL");
	fflush(stdout);
	std::string m = method;
	std::string params = jsonParams ? jsonParams : "{}";
	std::string result = "{}";
	int status = TSIP_OK;

	if (m == "GetAppInfo")
	{
		// profile directory = current directory (MCP.cfg with token is created there)
		char dir[MAX_PATH] = "";
		GetCurrentDirectoryA(sizeof(dir), dir);
		std::string escaped;
		for (const char *c = dir; *c; c++)
		{
			if (*c == '\\')
				escaped += "\\\\";
			else
				escaped += *c;
		}
		result = "{\"exeName\":\"TestHost.exe\",\"pluginInterface\":2,\"profileDir\":\"" + escaped + "\"}";
	}
	else if (m == "GetCalls")
	{
		result = "{\"calls\":[{\"incoming\":true,\"peer\":\"sip:123@example.com\",\"peerName\":\"Zażółć Gęślą\","
			"\"state\":1,\"stateName\":\"INCOMING\",\"uid\":5}";
		if (call7Active)
			result += ",{\"incoming\":false,\"peer\":\"sip:7@example.com\",\"peerName\":\"Test Peer\","
				"\"state\":3,\"stateName\":\"TRYING\",\"uid\":7}";
		result += "]}";
	}
	else if (m == "GetRegistration")
		result = "{\"accounts\":[{\"accountId\":0,\"registered\":true,\"state\":2,\"stateName\":\"REGISTER_OK\"}]}";
	else if (m == "GetHistory")
		result = "{\"entries\":[{\"timestamp\":\"2026-01-02 03:04:05\",\"incoming\":true,\"missed\":true,"
			"\"duration\":0,\"uri\":\"123\",\"peerName\":\"Zażółć\",\"contactName\":\"Test Contact\"}]}";
	else if (m == "SearchContacts")
		result = "{\"contacts\":[{\"name\":\"Test Contact\",\"numbers\":[\"123\",\"sip:123@example.com\"]}]}";
	else if (m == "GetMessages")
		result = "{\"messages\":[{\"timestamp\":\"2026-01-02 03:04:05\",\"incoming\":true,"
			"\"contentType\":\"text/plain\",\"body\":\"hello\"}]}";
	else if (m == "Call")
	{
		call7Active = true;
		result = "{\"uid\":7}";
		CloseHandle(CreateThread(NULL, 0, EstablishThread, NULL, 0, NULL));
	}
	else if (m == "Answer" || m == "SendDtmf" || m == "Transfer" || m == "SendMessage")
	{
		if (!ValidUid(GetUid(jsonParams)))
			status = TSIP_ERR_NOT_AVAILABLE;
	}
	else if (m == "Hangup")
	{
		unsigned int uid = GetUid(jsonParams);
		if (!ValidUid(uid))
			status = TSIP_ERR_NOT_AVAILABLE;
		else if (uid == 7)
		{
			call7Active = false;
			SendCallState(7, 0, "CLOSED");
		}
	}
	else if (m == "Hold")
		result = (params.find("true") != std::string::npos) ? "{\"hold\":true}" : "{\"hold\":false}";
	else if (m == "Mute")
		result = (params.find("true") != std::string::npos) ? "{\"mute\":true}" : "{\"mute\":false}";
	else if (m == "Sidetone")
	{
		if (!ValidUid(GetUid(jsonParams)))
			status = TSIP_ERR_NOT_AVAILABLE;
		else
			result = (params.find("\"state\":true") != std::string::npos) ? "{\"sidetone\":true,\"level\":50}" :
				"{\"sidetone\":false,\"level\":50}";
	}
	else if (m == "StartRecording")
		result = "{\"recording\":true,\"file\":\"C:\\\\test\\\\rec.wav\"}";
	else if (m == "RunScript")
	{
		if (params.find("\"async\":true") != std::string::npos)
			result = "{}";
		else if (params.find("error") != std::string::npos)
		{
			result = "{\"error\":\"[string \\\"error\\\"]:1: test error\",\"output\":\"before error\\n\"}";
			status = TSIP_ERR_GENERIC;
		}
		else
			result = "{\"return\":\"42\",\"output\":\"test output\\n\"}";
	}
	else if (m == "ListAudioDevices")
		result = "{\"modules\":[{\"module\":\"wasapi\",\"in\":[\"Mic 1\"],\"out\":[\"Speakers\",\"Headset\"]}],"
			"\"configured\":{\"input\":{\"module\":\"wasapi\",\"device\":\"Mic 1\"},"
			"\"output\":{\"module\":\"wasapi\",\"device\":\"Speakers\"}}}";
	else if (m == "SetAudioInput" || m == "SetAudioOutput")
	{
		if (!ValidUid(GetUid(jsonParams)))
			status = TSIP_ERR_NOT_AVAILABLE;
		else if (params.find("configured") == std::string::npos && params.find("Headset") == std::string::npos &&
			params.find("Mic 1") == std::string::npos)
		{
			status = TSIP_ERR_INVALID_ARG;
			result = "{\"error\":\"device not found for this module\"}";
		}
	}
	else if (m == "PlayAudioFile")
	{
		if (params.find("\\\\") != std::string::npos || params.find("..") != std::string::npos)
		{
			status = TSIP_ERR_INVALID_ARG;
			result = "{\"error\":\"file: WAV file name only\"}";
		}
		else
		{
			audioPlayStart = GetTickCount();
			result = "{\"uid\":5,\"file\":\"C:\\\\test\\\\audio\\\\test.wav\",\"audioErrorCount\":0}";
		}
	}
	else if (m == "SendCustomRequest")
	{
		if (params.find("\"uri\"") == std::string::npos && !ValidUid(GetUid(jsonParams)))
			status = TSIP_ERR_NOT_AVAILABLE;
		else
			result = "{\"requestUid\":3}";
	}
	else if (m == "GetCustomRequest")
	{
		// reply after ~0.3 s
		static DWORD first = 0;
		if (first == 0)
			first = GetTickCount();
		if (GetTickCount() - first < 300)
			result = "{\"requestUid\":3,\"haveReply\":false}";
		else
		{
			first = 0;
			result = "{\"requestUid\":3,\"haveReply\":true,\"transportError\":0,\"sipStatusCode\":200,"
				"\"reply\":\"SIP/2.0 200 OK\\r\\n\\r\\n\"}";
		}
	}
	else if (m == "DeleteCustomRequest")
	{
		result = "{}";
	}
	else if (m == "GetCallInvite")
	{
		if (!ValidUid(GetUid(jsonParams)))
			status = TSIP_ERR_NOT_AVAILABLE;
		else
			result = "{\"uid\":5,\"incoming\":true,\"invite\":\"INVITE sip:100@127.0.0.1 SIP/2.0\\r\\n"
				"f: \\\"Alice\\\" <sip:200@127.0.0.1>;tag=1\\r\\nUser-Agent: test\\r\\nX-Case-Id: 42\\r\\n"
				"Content-Length: 0\\r\\n\\r\\n\"}";
	}
	else if (m == "GetCallAudioState")
	{
		// fake WAV file: 1 s long
		unsigned int count = (audioPlayStart && GetTickCount() - audioPlayStart > 1000) ? 1 : 0;
		// fake received audio: silence for 0.5 s after start, then speech
		static DWORD levelStart = GetTickCount();
		unsigned int level = (GetTickCount() - levelStart) % 2000 < 1000 ? 100 : 5000;
		result = "{\"uid\":5,\"audioErrorCount\":" + std::to_string(count) +
			",\"rxLevel\":" + std::to_string(level) + "}";
	}
	else if (m == "RestartPlugin")
		result = "{}";
	else
	{
		status = TSIP_ERR_UNKNOWN_METHOD;
		result = "";
	}

	if (jsonResult)
		*jsonResult = result.empty() ? NULL : _strdup(result.c_str());
	return status;
}

void TSIP_V2_CALL Free(void *cookie, void *ptr)
{
	(void)cookie;
	free(ptr);
}

int TSIP_V2_CALL SetVariable(void *cookie, const char *name, const char *value)
{
	(void)cookie;
	printf("[variable] %s = %s\n", name, value);
	fflush(stdout);
	return TSIP_OK;
}

}	// namespace

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		printf("Usage: %s <MCP.dll> [seconds]\n", argv[0]);
		return 1;
	}
	int seconds = (argc >= 3) ? atoi(argv[2]) : 60;
	InitializeCriticalSection(&eventLock);

	HMODULE dll = LoadLibraryA(argv[1]);
	if (!dll)
	{
		printf("LoadLibrary failed, error %lu\n", GetLastError());
		return 1;
	}
	pfTSipPluginInit init = (pfTSipPluginInit)(void*)GetProcAddress(dll, TSIP_PLUGIN_INIT_NAME);
	if (!init)
	{
		printf("%s not exported\n", TSIP_PLUGIN_INIT_NAME);
		return 1;
	}

	struct TSIP_HOST_API host;
	memset(&host, 0, sizeof(host));
	host.size = sizeof(host);
	host.cookie = &host;
	host.Log = Log;
	host.Invoke = Invoke;
	host.Free = Free;
	host.SetVariable = SetVariable;

	memset(&plugin, 0, sizeof(plugin));
	plugin.size = sizeof(plugin);

	int status = init(&host, &plugin);
	printf("init -> %d\n", status);
	if (status != 0)
		return 1;
	if (plugin.GetMetadata)
		printf("metadata: %s\n", plugin.GetMetadata());

	status = plugin.Start ? plugin.Start() : -1;
	printf("Start -> %d\n", status);
	fflush(stdout);

	Sleep(seconds * 1000);

	if (plugin.Stop)
		printf("Stop -> %d\n", plugin.Stop());
	if (plugin.Shutdown)
		plugin.Shutdown();
	FreeLibrary(dll);
	DeleteCriticalSection(&eventLock);
	printf("done\n");
	return 0;
}
