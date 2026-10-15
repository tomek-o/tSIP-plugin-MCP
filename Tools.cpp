#include "Tools.h"
#include "Activity.h"
#include "CallWatch.h"
#include "Config.h"
#include "Host.h"
#include "tSIP_plugin_v2.h"

#include <windows.h>

namespace
{

Json::Value TextResult(const std::string &text, bool isError)
{
	Json::Value item(Json::objectValue);
	item["type"] = "text";
	item["text"] = text;
	Json::Value content(Json::arrayValue);
	content.append(item);
	Json::Value result(Json::objectValue);
	result["content"] = content;
	result["isError"] = isError;
	return result;
}

std::string ToJsonText(const Json::Value &jv)
{
	Json::StyledWriter writer;
	return writer.write(jv);
}

/** \brief Execute host method, convert outcome to tool result */
Json::Value InvokeResult(const char *method, const Json::Value &params)
{
	Json::Value result;
	int status = HostInvoke(method, params, result);
	if (status != TSIP_OK)
	{
		std::string text = std::string("tSIP returned error: ") + HostErrorName(status);
		if (result["error"].isString())
			text += ": " + result["error"].asString();	// details, e.g. Lua error
		if (result["output"].isString())
			text += "\n\nScript output before error:\n" + result["output"].asString();
		return TextResult(text, true);
	}
	return TextResult(ToJsonText(result), false);
}

/** \brief Full SIP header name for compact form (RFC 3261 7.3.3 and extensions), or name itself */
std::string FullHeaderName(const std::string &name)
{
	if (name.size() != 1)
		return name;
	static const char* const compact[][2] = {
		{"a", "Accept-Contact"}, {"b", "Referred-By"}, {"c", "Content-Type"}, {"d", "Request-Disposition"},
		{"e", "Content-Encoding"}, {"f", "From"}, {"i", "Call-ID"}, {"j", "Reject-Contact"},
		{"k", "Supported"}, {"l", "Content-Length"}, {"m", "Contact"}, {"n", "Identity-Info"},
		{"o", "Event"}, {"r", "Refer-To"}, {"s", "Subject"}, {"t", "To"}, {"u", "Allow-Events"},
		{"v", "Via"}, {"x", "Session-Expires"}, {"y", "Identity"}
	};
	for (unsigned int i = 0; i < sizeof(compact)/sizeof(compact[0]); i++)
	{
		if (_stricmp(name.c_str(), compact[i][0]) == 0)
			return compact[i][1];
	}
	return name;
}

Json::Value Property(const char *type, const char *description)
{
	Json::Value p(Json::objectValue);
	p["type"] = type;
	p["description"] = description;
	return p;
}

Json::Value Tool(const char *name, const char *title, const char *description, const Json::Value &properties,
	const Json::Value &required, bool readOnly, bool destructive)
{
	Json::Value schema(Json::objectValue);
	schema["type"] = "object";
	schema["properties"] = properties;
	if (required.size() > 0)
		schema["required"] = required;

	Json::Value annotations(Json::objectValue);
	annotations["title"] = title;
	annotations["readOnlyHint"] = readOnly;
	annotations["destructiveHint"] = destructive;
	annotations["openWorldHint"] = !readOnly;

	Json::Value tool(Json::objectValue);
	tool["name"] = name;
	tool["title"] = title;
	tool["description"] = description;
	tool["inputSchema"] = schema;
	tool["annotations"] = annotations;
	return tool;
}

bool GetUid(const Json::Value &args, Json::Value &params, std::string &error)
{
	const Json::Value &uid = args["uid"];
	if (uid.isNull())
		return true;	// current call
	if (!uid.isIntegral() || uid.asInt() <= 0)
	{
		error = "uid must be a positive integer";
		return false;
	}
	params["uid"] = uid.asUInt();
	return true;
}

/** \brief Check target against prefixes, ask user if configured
	\param action e.g. "make a call to"
	\return empty string if allowed, otherwise error text for tool result
*/
std::string CheckTarget(const Config &cfg, const char *tool, const char *action, const std::string &target)
{
	std::string reason;
	if (!cfg.IsNumberAllowed(target, reason))
	{
		HostLog("%s to %s blocked: %s", tool, target.c_str(), reason.c_str());
		return "Not allowed: " + reason;
	}
	if (cfg.confirmCalls)
	{
		// UTF-8 text, wide API for proper display
		std::string text = std::string("MCP client (AI agent) wants to ") + action + ":\n\n" + target + "\n\nAllow this?";
		int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, NULL, 0);
		std::wstring wtext(len > 0 ? len : 1, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &wtext[0], len);
		int answer = MessageBoxW(NULL, wtext.c_str(), L"tSIP MCP plugin",
			MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_TOPMOST | MB_SETFOREGROUND);
		if (answer != IDYES)
		{
			HostLog("%s to %s rejected by user", tool, target.c_str());
			return "Rejected by the user.";
		}
	}
	return "";
}

Json::Value AudioSwitchProps(void)
{
	Json::Value props(Json::objectValue);
	props["uid"] = Property("integer", "Call uid (from get_calls); current call if omitted");
	props["module"] = Property("string", "Audio module: wasapi, winwave2, winwave or portaudio (see list_audio_devices)");
	props["device"] = Property("string", "Device name exactly as returned by list_audio_devices");
	props["configured"] = Property("boolean", "true: switch back to device configured in tSIP settings (module/device ignored)");
	return props;
}

/** \brief Check "headers" argument against allowedCallHeaders
	\return error text, empty if OK
*/
std::string CheckHeaders(const Config &cfg, const Json::Value &headers)
{
	if (headers.isNull())
		return "";
	if (!headers.isObject())
		return "headers must be an object {\"Name\":\"value\"}";
	Json::Value::Members names = headers.getMemberNames();
	for (size_t i = 0; i < names.size(); i++)
	{
		if (!cfg.IsCallHeaderAllowed(names[i]))
			return "header \"" + names[i] + "\" is not allowed by plugin configuration (allowedCallHeaders)";
		if (!headers[names[i]].isString())
			return "value of header \"" + names[i] + "\" must be a string";
	}
	return "";
}

/** \brief send_sip_request: send custom request, wait for final reply */
Json::Value SendSipRequest(const Config &cfg, const Json::Value &args)
{
	if (!args["method"].isString())
		return TextResult("method (string) is required", true);
	std::string method = args["method"].asString();
	if (!cfg.IsRequestMethodAllowed(method))
		return TextResult("method \"" + method + "\" is not allowed by plugin configuration (allowedRequestMethods)", true);
	std::string error = CheckHeaders(cfg, args["headers"]);
	if (!error.empty())
		return TextResult(error, true);
	Json::Value params(Json::objectValue);
	params["method"] = method;
	if (args["headers"].isObject() && !args["headers"].empty())
		params["headers"] = args["headers"];
	if (args["accountId"].isIntegral())
	{
		if (!args["uri"].isString())
			return TextResult("accountId applies only to out-of-dialog request (uri); request within call uses account of the call", true);
		params["accountId"] = args["accountId"].asInt();
	}
	if (args["uri"].isString() && !args["uri"].asString().empty())
	{
		if (!args["uid"].isNull())
			return TextResult("use either uri (out-of-dialog request) or uid (request within call), not both", true);
		std::string uri = args["uri"].asString();
		// OPTIONS: harmless query (capabilities / keep-alive) - no confirmation, prefix rules only
		if (method == "OPTIONS")
		{
			std::string reason;
			if (!cfg.IsNumberAllowed(uri, reason))
				return TextResult("Not allowed: " + reason, true);
		}
		else
		{
			error = CheckTarget(cfg, "send_sip_request", (std::string("send SIP ") + method + " request to").c_str(), uri);
			if (!error.empty())
				return TextResult(error, true);
		}
		params["uri"] = uri;
	}
	else
	{
		if (!GetUid(args, params, error))
			return TextResult(error, true);
	}

	Json::Value sent;
	int status = HostInvoke("SendCustomRequest", params, sent);
	if (status != TSIP_OK)
	{
		std::string text = std::string("tSIP returned error: ") + HostErrorName(status);
		if (sent["error"].isString())
			text += ": " + sent["error"].asString();
		return TextResult(text, true);
	}

	// SIP transaction timeout (timer F / B) is 32 s
	enum { MAX_WAIT_S = 40 };
	int timeoutS = args["timeout"].isIntegral() ? args["timeout"].asInt() : 10;
	if (timeoutS <= 0 || timeoutS > MAX_WAIT_S)
		timeoutS = MAX_WAIT_S;
	Json::Value getParams(Json::objectValue);
	getParams["requestUid"] = sent["requestUid"];
	getParams["delete"] = true;
	DWORD start = GetTickCount();
	while (GetTickCount() - start < static_cast<DWORD>(timeoutS) * 1000)
	{
		Sleep(100);
		Json::Value state;
		// fails also when plugin is being stopped (host cancels Invoke) - no blocking
		if (HostInvoke("GetCustomRequest", getParams, state) != TSIP_OK)
			return TextResult("request status not available (plugin stopping or request deleted by script)", true);
		if (state["haveReply"].asBool())
		{
			Json::Value result(Json::objectValue);
			int transportError = state["transportError"].asInt();
			if (transportError != 0)
			{
				result["result"] = "error";
				result["transportError"] = transportError;	// errno, e.g. 60 (ETIMEDOUT): no reply
			}
			else
			{
				result["result"] = "reply";
				result["sipStatusCode"] = state["sipStatusCode"];
				result["reply"] = state["reply"];
			}
			return TextResult(ToJsonText(result), false);
		}
	}
	Json::Value delParams(Json::objectValue);
	delParams["requestUid"] = sent["requestUid"];
	Json::Value ignored;
	HostInvoke("DeleteCustomRequest", delParams, ignored);
	Json::Value result(Json::objectValue);
	result["result"] = "timeout";
	return TextResult(ToJsonText(result), false);
}

/** \brief play_audio_file: switch call input to WAV file, optionally wait for end and restore input */
Json::Value PlayAudioFile(const Json::Value &args)
{
	if (!args["file"].isString() || args["file"].asString().empty())
		return TextResult("file (string) is required", true);
	Json::Value params(Json::objectValue);
	std::string error;
	if (!GetUid(args, params, error))
		return TextResult(error, true);
	params["file"] = args["file"].asString();

	Json::Value played;
	int status = HostInvoke("PlayAudioFile", params, played);
	if (status != TSIP_OK)
	{
		std::string text = std::string("tSIP returned error: ") + HostErrorName(status);
		if (played["error"].isString())
			text += ": " + played["error"].asString();
		return TextResult(text, true);
	}
	bool wait = args["wait"].isBool() ? args["wait"].asBool() : false;
	if (!wait)
		return TextResult(ToJsonText(played), false);

	enum { MAX_WAIT_S = 300 };
	int timeoutS = args["timeout"].isIntegral() ? args["timeout"].asInt() : 60;
	if (timeoutS <= 0 || timeoutS > MAX_WAIT_S)
		timeoutS = MAX_WAIT_S;
	bool restore = args["restore_input"].isBool() ? args["restore_input"].asBool() : true;

	Json::Value stateParams(Json::objectValue);
	stateParams["uid"] = played["uid"];
	unsigned int startCount = played["audioErrorCount"].asUInt();
	std::string outcome = "timeout";
	DWORD start = GetTickCount();
	while (GetTickCount() - start < static_cast<DWORD>(timeoutS) * 1000)
	{
		Sleep(200);
		Json::Value state;
		// fails also when plugin is being stopped (host cancels Invoke) - no blocking
		if (HostInvoke("GetCallAudioState", stateParams, state) != TSIP_OK)
		{
			outcome = "call ended";
			restore = false;
			break;
		}
		if (state["audioErrorCount"].asUInt() != startCount)
		{
			outcome = "finished";
			break;
		}
	}
	Json::Value result(Json::objectValue);
	result["result"] = outcome;
	result["file"] = played["file"];
	result["uid"] = played["uid"];
	if (restore)
	{
		Json::Value restoreParams(Json::objectValue);
		restoreParams["uid"] = played["uid"];
		restoreParams["configured"] = true;
		Json::Value restored;
		result["inputRestored"] = (HostInvoke("SetAudioInput", restoreParams, restored) == TSIP_OK);
	}
	return TextResult(ToJsonText(result), false);
}

/** \brief wait_for_audio: poll received audio level until speech / silence lasts for given time */
Json::Value WaitForAudio(const Json::Value &args)
{
	const std::string condition = args["condition"].isString() ? args["condition"].asString() : "";
	if (condition != "speech" && condition != "silence")
		return TextResult("condition must be \"speech\" or \"silence\"", true);
	Json::Value params(Json::objectValue);
	std::string error;
	if (!GetUid(args, params, error))
		return TextResult(error, true);

	enum { POLL_MS = 100, MAX_TIMEOUT_S = 120 };
	int threshold = args["threshold"].isIntegral() ? args["threshold"].asInt() : 1000;
	if (threshold < 1 || threshold > 32767)
		return TextResult("threshold must be 1..32767", true);
	int durationMs = args["duration_ms"].isIntegral() ? args["duration_ms"].asInt() :
		(condition == "speech" ? 300 : 1500);
	if (durationMs < POLL_MS)
		durationMs = POLL_MS;
	int timeoutS = args["timeout"].isIntegral() ? args["timeout"].asInt() : 30;
	if (timeoutS <= 0 || timeoutS > MAX_TIMEOUT_S)
		timeoutS = MAX_TIMEOUT_S;

	std::string outcome = "timeout";
	unsigned int maxLevel = 0;
	unsigned int lastLevel = 0;
	DWORD start = GetTickCount();
	DWORD matchStart = 0;
	bool matching = false;
	while (GetTickCount() - start < static_cast<DWORD>(timeoutS) * 1000)
	{
		Json::Value state;
		// fails also when plugin is being stopped (host cancels Invoke) - no blocking
		if (HostInvoke("GetCallAudioState", params, state) != TSIP_OK)
		{
			outcome = "call ended";
			break;
		}
		if (!state["rxLevel"].isIntegral())
			return TextResult("tSIP does not report audio level (version too old?)", true);
		lastLevel = state["rxLevel"].asUInt();
		if (lastLevel > maxLevel)
			maxLevel = lastLevel;
		bool match = (condition == "speech") ? (lastLevel >= static_cast<unsigned int>(threshold)) :
			(lastLevel < static_cast<unsigned int>(threshold));
		DWORD now = GetTickCount();
		if (match && !matching)
			matchStart = now;
		matching = match;
		if (matching && now - matchStart >= static_cast<DWORD>(durationMs))
		{
			outcome = condition;
			break;
		}
		Sleep(POLL_MS);
	}
	Json::Value result(Json::objectValue);
	result["result"] = outcome;
	result["elapsed_ms"] = static_cast<unsigned int>(GetTickCount() - start);
	result["lastLevel"] = lastLevel;
	result["maxLevel"] = maxLevel;
	return TextResult(ToJsonText(result), false);
}

}	// namespace


Json::Value Tools::List(const Config &cfg)
{
	Json::Value tools(Json::arrayValue);
	Json::Value none(Json::arrayValue);

	if (cfg.IsToolEnabled("get_app_info"))
	{
		tools.append(Tool("get_app_info", "Get tSIP info",
			"Get information about running tSIP softphone instance (executable, profile directory, plugin interface version).",
			Json::Value(Json::objectValue), none, true, false));
	}
	if (cfg.IsToolEnabled("get_calls"))
	{
		tools.append(Tool("get_calls", "List active calls",
			"List active calls in tSIP softphone: uid, direction (incoming), state, peer URI, peer display name "
			"and accountId (account used by call / account that received incoming call).",
			Json::Value(Json::objectValue), none, true, false));
	}
	if (cfg.IsToolEnabled("get_registration"))
	{
		tools.append(Tool("get_registration", "Get accounts and registration state",
			"Get configured SIP accounts of tSIP: accountId, name, user, server, enabled, current (account used "
			"by default for outgoing calls / messages) and registration state; also currentAccountId.",
			Json::Value(Json::objectValue), none, true, false));
	}
	if (cfg.IsToolEnabled("set_current_account"))
	{
		Json::Value props(Json::objectValue);
		props["accountId"] = Property("integer", "Account (accountId from get_registration)");
		Json::Value required(Json::arrayValue);
		required.append("accountId");
		tools.append(Tool("set_current_account", "Set current account",
			"Select account used by default for outgoing calls and messages in tSIP (same as ACCOUNT button).",
			props, required, false, false));
	}
	if (cfg.IsToolEnabled("get_history"))
	{
		Json::Value props(Json::objectValue);
		props["limit"] = Property("integer", "Maximum number of entries, default 20, max 200");
		Json::Value filter = Property("string", "Entry type filter, default \"all\"");
		Json::Value values(Json::arrayValue);
		values.append("all");
		values.append("incoming");
		values.append("outgoing");
		values.append("missed");
		filter["enum"] = values;
		props["filter"] = filter;
		props["search"] = Property("string", "Optional text to match (case-insensitive) in number/URI or names");
		tools.append(Tool("get_history", "Get call history",
			"Get tSIP call history, newest first: timestamp (local time), direction, missed flag, "
			"duration in seconds, peer URI/number, display name and phonebook contact name.",
			props, none, true, false));
	}
	if (cfg.IsToolEnabled("search_contacts"))
	{
		Json::Value props(Json::objectValue);
		props["query"] = Property("string", "Text to find (case-insensitive) in contact name, company, numbers or note");
		props["limit"] = Property("integer", "Maximum number of contacts, default 20, max 200");
		Json::Value required(Json::arrayValue);
		required.append("query");
		tools.append(Tool("search_contacts", "Search phonebook",
			"Search tSIP phonebook. Returns contact name, numbers/URIs (usable as make_call target), company and note.",
			props, required, true, false));
	}
	if (cfg.IsToolEnabled("make_call"))
	{
		Json::Value props(Json::objectValue);
		props["target"] = Property("string", "Number or SIP URI to call, e.g. \"123\" or \"sip:alice@example.com\"");
		if (!cfg.allowedCallHeaders.empty())
		{
			std::string allowed;
			for (size_t i = 0; i < cfg.allowedCallHeaders.size(); i++)
				allowed += (i ? ", " : "") + cfg.allowedCallHeaders[i];
			std::string description =
				"Optional extra SIP headers for INVITE as {\"Name\":\"value\"}, e.g. {\"Subject\":\"Order 123\"} or "
				"{\"Call-Info\":\"<sip:x>;answer-after=0\"} (auto-answer request, if supported by called device). "
				"Allowed names: " + allowed;
			Json::Value headers = Property("object", description.c_str());
			headers["additionalProperties"] = Property("string", "Header value");
			props["headers"] = headers;
		}
		props["accountId"] = Property("integer", "Account (accountId from get_registration) used for the call; "
			"current account if omitted");
		Json::Value required(Json::arrayValue);
		required.append("target");
		tools.append(Tool("make_call", "Make a call",
			"Start an outgoing call from tSIP softphone. Depending on plugin configuration the user may be asked "
			"to confirm the call and some numbers may be blocked. Returns uid of the new call.",
			props, required, false, false));
	}
	if (cfg.IsToolEnabled("answer_call"))
	{
		Json::Value props(Json::objectValue);
		props["uid"] = Property("integer", "Call uid (from get_calls); current call if omitted");
		tools.append(Tool("answer_call", "Answer a call",
			"Answer incoming (ringing) call in tSIP softphone.",
			props, none, false, false));
	}
	if (cfg.IsToolEnabled("hangup_call"))
	{
		Json::Value props(Json::objectValue);
		props["uid"] = Property("integer", "Call uid (from get_calls); current call if omitted");
		props["sipCode"] = Property("integer", "SIP code used when rejecting incoming call, 400..699, default 486");
		props["reason"] = Property("string", "SIP reason phrase used when rejecting incoming call, default \"Busy Here\"");
		tools.append(Tool("hangup_call", "Hang up a call",
			"End (hang up) a call or reject incoming call in tSIP softphone.",
			props, none, false, true));
	}
	if (cfg.IsToolEnabled("wait_for_call"))
	{
		Json::Value props(Json::objectValue);
		Json::Value condition = Property("string",
			"established: call answered (returns also if call ends first); ended: call finished; "
			"incoming: new incoming call arrives (uid not used)");
		Json::Value values(Json::arrayValue);
		values.append("established");
		values.append("ended");
		values.append("incoming");
		condition["enum"] = values;
		props["condition"] = condition;
		props["uid"] = Property("integer", "Call uid (from make_call / get_calls); if omitted: the only active call");
		props["timeout"] = Property("integer", "Timeout in seconds, default 30, max 60");
		Json::Value required(Json::arrayValue);
		required.append("condition");
		tools.append(Tool("wait_for_call", "Wait for call state",
			"Wait until call is answered or ended, or until new incoming call arrives. Returns immediately "
			"if condition is already met. Result: \"established\", \"ended\", \"incoming\", \"timeout\" "
			"or \"stopping\" with call uid, state and peer. Use after make_call instead of polling get_calls.",
			props, required, true, false));
	}
	if (cfg.IsToolEnabled("send_dtmf"))
	{
		Json::Value props(Json::objectValue);
		props["uid"] = Property("integer", "Call uid (from get_calls); current call if omitted");
		props["digits"] = Property("string", "DTMF digits to send: 0-9 * # A-D (R = hook flash), max 32");
		Json::Value required(Json::arrayValue);
		required.append("digits");
		tools.append(Tool("send_dtmf", "Send DTMF",
			"Send DTMF digits in active call, e.g. to navigate voice menu (IVR).",
			props, required, false, false));
	}
	if (cfg.IsToolEnabled("hold_call"))
	{
		Json::Value props(Json::objectValue);
		props["uid"] = Property("integer", "Call uid (from get_calls); current call if omitted");
		props["hold"] = Property("boolean", "true: put call on hold, false: resume");
		Json::Value required(Json::arrayValue);
		required.append("hold");
		tools.append(Tool("hold_call", "Hold / resume call",
			"Put call on hold or resume it. Returns actual hold state (hold is not possible before call has media).",
			props, required, false, false));
	}
	if (cfg.IsToolEnabled("transfer_call"))
	{
		Json::Value props(Json::objectValue);
		props["uid"] = Property("integer", "Call uid (from get_calls); current call if omitted");
		props["target"] = Property("string", "Number or SIP URI to transfer call to");
		Json::Value required(Json::arrayValue);
		required.append("target");
		tools.append(Tool("transfer_call", "Transfer call",
			"Blind transfer of a call to another number or SIP URI. Same restrictions as make_call apply "
			"(user confirmation, blocked numbers).",
			props, required, false, true));
	}
	if (cfg.IsToolEnabled("list_audio_devices"))
	{
		tools.append(Tool("list_audio_devices", "List audio devices",
			"List audio input/output devices for each audio module (wasapi, winwave2, portaudio) and devices "
			"configured in tSIP settings. Use names from this list with set_call_audio_input/output.",
			Json::Value(Json::objectValue), Json::Value(Json::arrayValue), true, false));
	}
	if (cfg.IsToolEnabled("set_call_audio_input"))
	{
		tools.append(Tool("set_call_audio_input", "Switch call audio input",
			"Switch audio input (microphone) of a call to another device, or back to configured one. "
			"To play a WAV file into the call use play_audio_file.",
			AudioSwitchProps(), Json::Value(Json::arrayValue), false, false));
	}
	if (cfg.IsToolEnabled("set_call_audio_output"))
	{
		tools.append(Tool("set_call_audio_output", "Switch call audio output",
			"Switch audio output (speaker / headset) of a call to another device, or back to configured one.",
			AudioSwitchProps(), Json::Value(Json::arrayValue), false, false));
	}
	if (cfg.IsToolEnabled("play_audio_file"))
	{
		Json::Value props(Json::objectValue);
		props["uid"] = Property("integer", "Call uid (from get_calls); current call if omitted");
		props["file"] = Property("string", "WAV file name (no path) from \"audio\" subdirectory of tSIP profile "
			"or profile directory");
		props["wait"] = Property("boolean", "Wait until file ends (default false)");
		props["timeout"] = Property("integer", "Max wait in seconds when wait = true, default 60, max 300");
		props["restore_input"] = Property("boolean", "When wait = true: switch input back to configured "
			"microphone after file ends (default true)");
		Json::Value required(Json::arrayValue);
		required.append("file");
		tools.append(Tool("play_audio_file", "Play WAV file into call",
			"Play WAV file to the other party of a call (call audio input is switched to the file). "
			"Without wait: returns immediately, microphone stays replaced until set_call_audio_input "
			"with configured = true. With wait: returns \"finished\", \"timeout\" or \"call ended\".",
			props, required, false, false));
	}
	if (cfg.IsToolEnabled("send_sip_request"))
	{
		std::string methods;
		for (size_t i = 0; i < cfg.allowedRequestMethods.size(); i++)
			methods += (i ? ", " : "") + cfg.allowedRequestMethods[i];
		Json::Value props(Json::objectValue);
		std::string methodDescription = "SIP method, allowed: " + methods;
		props["method"] = Property("string", methodDescription.c_str());
		props["uri"] = Property("string", "Target SIP URI for out-of-dialog request (e.g. OPTIONS to check if "
			"server / device responds); omit to send request within call dialog (uid)");
		props["uid"] = Property("integer", "Call uid for request within call dialog (e.g. INFO); current call if "
			"omitted (and uri not given)");
		std::string headersDescription = "Optional extra headers {\"Name\":\"value\"}, e.g. {\"X-Command\":\"test\"}; "
			"same allowed names as for make_call";
		Json::Value headers = Property("object", headersDescription.c_str());
		headers["additionalProperties"] = Property("string", "Header value");
		props["headers"] = headers;
		props["timeout"] = Property("integer", "Seconds to wait for final reply, default 10, max 40");
		props["accountId"] = Property("integer", "Account (accountId from get_registration) used for out-of-dialog "
			"request; current account if omitted");
		Json::Value required(Json::arrayValue);
		required.append("method");
		tools.append(Tool("send_sip_request", "Send SIP request",
			"Send custom SIP request (no body) out-of-dialog to uri or within existing call, wait for final reply. "
			"Result: \"reply\" with sipStatusCode and full reply text, \"error\" (transport error / no reply) "
			"or \"timeout\". Requests other than OPTIONS to uri may require confirmation by the user.",
			props, required, false, false));
	}
	if (cfg.IsToolEnabled("get_call_invite"))
	{
		Json::Value props(Json::objectValue);
		props["uid"] = Property("integer", "Call uid (from get_calls); current call if omitted");
		Json::Value headers = Property("array", "Optional: return only these headers (case-insensitive names, "
			"e.g. [\"From\",\"P-Asserted-Identity\",\"User-Agent\"]) instead of full message");
		headers["items"] = Property("string", "Header name");
		props["headers"] = headers;
		tools.append(Tool("get_call_invite", "Get incoming INVITE",
			"Get full text of initial INVITE received for incoming call (SIP headers and SDP body), e.g. to check "
			"caller identity headers, User-Agent, Alert-Info, custom X- headers or offered codecs. "
			"Not available for outgoing calls.",
			props, Json::Value(Json::arrayValue), true, false));
	}
	if (cfg.IsToolEnabled("get_audio_level"))
	{
		Json::Value props(Json::objectValue);
		props["uid"] = Property("integer", "Call uid (from get_calls); current call if omitted");
		tools.append(Tool("get_audio_level", "Get received audio level",
			"Get peak level of audio received in a call over last ~100 ms: rxLevel 0..32767 (linear; "
			"roughly: < 300 silence / noise, > 1000 speech - depends on line). Level is measured for "
			"received audio in general, not separately for each call.",
			props, Json::Value(Json::arrayValue), true, false));
	}
	if (cfg.IsToolEnabled("wait_for_audio"))
	{
		Json::Value props(Json::objectValue);
		props["uid"] = Property("integer", "Call uid (from get_calls); current call if omitted");
		Json::Value condition = Property("string",
			"speech: wait until other party speaks (level >= threshold for duration_ms); "
			"silence: wait until other party is silent (level < threshold for duration_ms), e.g. end of announcement");
		Json::Value values(Json::arrayValue);
		values.append("speech");
		values.append("silence");
		condition["enum"] = values;
		props["condition"] = condition;
		props["threshold"] = Property("integer", "Level threshold 1..32767, default 1000");
		props["duration_ms"] = Property("integer", "How long condition has to last, default 300 ms (speech) / 1500 ms (silence)");
		props["timeout"] = Property("integer", "Timeout in seconds, default 30, max 120");
		Json::Value required(Json::arrayValue);
		required.append("condition");
		tools.append(Tool("wait_for_audio", "Wait for speech / silence",
			"Wait until the other party starts speaking or becomes silent, based on received audio level - "
			"e.g. wait for IVR announcement to finish before send_dtmf, or detect that call was answered by "
			"a person. Result: \"speech\", \"silence\", \"timeout\" or \"call ended\", with elapsed time and levels.",
			props, required, true, false));
	}
	if (cfg.IsToolEnabled("mute_call"))
	{
		Json::Value props(Json::objectValue);
		props["uid"] = Property("integer", "Call uid (from get_calls); current call if omitted");
		props["mute"] = Property("boolean", "true: mute microphone (transmitted audio), false: unmute");
		Json::Value required(Json::arrayValue);
		required.append("mute");
		tools.append(Tool("mute_call", "Mute / unmute call",
			"Mute or unmute microphone (transmitted audio) in a call. Returns actual mute state.",
			props, required, false, false));
	}
	if (cfg.IsToolEnabled("set_sidetone"))
	{
		Json::Value props(Json::objectValue);
		props["uid"] = Property("integer", "Call uid (from get_calls); current call if omitted");
		props["enabled"] = Property("boolean", "true: mix transmitted audio into local playback");
		props["level"] = Property("integer", "Sidetone level in percent of transmitted audio, 0..200 (default 50 or previous value)");
		Json::Value required(Json::arrayValue);
		required.append("enabled");
		tools.append(Tool("set_sidetone", "Sidetone on / off",
			"Enable or disable sidetone for a call: user hears locally what is transmitted (e.g. WAV file "
			"played with play_audio_file, radio input). Sidetone is not recorded. Avoid with microphone + "
			"loudspeakers (acoustic feedback). Returns actual state.",
			props, required, false, false));
	}
	if (cfg.IsToolEnabled("start_recording"))
	{
		Json::Value props(Json::objectValue);
		props["uid"] = Property("integer", "Call uid (from get_calls); current call if omitted");
		tools.append(Tool("start_recording", "Start call recording",
			"Start recording a call using recording settings from tSIP configuration (directory, format). "
			"Fails if recording is disabled in tSIP settings. Returns recording state and file name. "
			"Note: recording calls may require consent of the other party.",
			props, Json::Value(Json::arrayValue), false, false));
	}
	if (cfg.IsToolEnabled("send_message"))
	{
		Json::Value props(Json::objectValue);
		props["target"] = Property("string", "Number or SIP URI");
		props["text"] = Property("string", "Message text");
		props["accountId"] = Property("integer", "Account (accountId from get_registration) used for sending; "
			"current account if omitted");
		Json::Value required(Json::arrayValue);
		required.append("target");
		required.append("text");
		tools.append(Tool("send_message", "Send text message",
			"Send text message (SIP MESSAGE). Message is sent through tSIP message window (visible to user); "
			"delivery status is not returned. Same restrictions as make_call apply (user confirmation, "
			"blocked numbers).",
			props, required, false, false));
	}
	if (cfg.IsToolEnabled("get_messages"))
	{
		Json::Value props(Json::objectValue);
		props["peer"] = Property("string", "Number or SIP URI of the other party");
		props["limit"] = Property("integer", "Maximum number of messages (most recent), default 20, max 200");
		Json::Value required(Json::arrayValue);
		required.append("peer");
		tools.append(Tool("get_messages", "Get text messages",
			"Get text message (SIP MESSAGE) history with given peer, oldest first: timestamp, direction, body.",
			props, required, true, false));
	}
	if (cfg.IsToolEnabled("run_script"))
	{
		Json::Value props(Json::objectValue);
		props["script"] = Property("string", "Lua script source; see tSIP scripting documentation for available functions; "
			"use return to pass values back");
		Json::Value required(Json::arrayValue);
		required.append("script");
		tools.append(Tool("run_script", "Run Lua script",
			"Run Lua script in tSIP (synchronously, in main thread). Gives full access to tSIP scripting API. "
			"Result: \"return\" - values returned by script (e.g. return GetVariable(\"x\")), "
			"\"output\" - text printed with print(); Lua errors are returned as tool error. "
			"Do not use functions showing dialogs (MessageBox, ShowMessage) - they block tSIP until closed.",
			props, required, false, true));
	}
	if (cfg.IsToolEnabled("get_log"))
	{
		Json::Value props(Json::objectValue);
		props["lines"] = Property("integer", "Number of last lines, default 50, max 1000");
		tools.append(Tool("get_log", "Get tSIP log lines",
			"Returns the last lines of the tSIP log window as text (newest last). Useful to check results of commands and errors.",
			props, Json::Value(Json::arrayValue), true, false));
	}
	return tools;
}

Json::Value Tools::Call(const Config &cfg, const std::string &name, const Json::Value &args, bool &found)
{
	found = true;
	if (!cfg.IsToolEnabled(name))
	{
		found = false;
		return Json::Value();
	}

	{
		bool readOnly = (name.compare(0, 4, "get_") == 0 || name.compare(0, 5, "list_") == 0 ||
			name == "search_contacts" || name.compare(0, 9, "wait_for_") == 0);
		std::string detail;
		if (args["target"].isString())
			detail = args["target"].asString();
		else if (args["query"].isString())
			detail = args["query"].asString();
		if (detail.size() > 40)
			detail = detail.substr(0, 40) + "...";
		Activity::Report(name, detail, readOnly);
	}

	if (name == "get_app_info")
	{
		return InvokeResult("GetAppInfo", Json::Value(Json::objectValue));
	}
	else if (name == "get_calls")
	{
		return InvokeResult("GetCalls", Json::Value(Json::objectValue));
	}
	else if (name == "get_registration")
	{
		return InvokeResult("GetRegistration", Json::Value(Json::objectValue));
	}
	else if (name == "set_current_account")
	{
		if (!args["accountId"].isIntegral())
			return TextResult("accountId (integer) is required", true);
		Json::Value params(Json::objectValue);
		params["accountId"] = args["accountId"].asInt();
		return InvokeResult("SetCurrentAccount", params);
	}
	else if (name == "get_history")
	{
		Json::Value params(Json::objectValue);
		if (args["limit"].isIntegral())
			params["limit"] = args["limit"].asInt();
		if (args["filter"].isString())
			params["filter"] = args["filter"].asString();
		if (args["search"].isString())
			params["search"] = args["search"].asString();
		return InvokeResult("GetHistory", params);
	}
	else if (name == "search_contacts")
	{
		if (!args["query"].isString())
			return TextResult("query (string) is required", true);
		Json::Value params(Json::objectValue);
		params["query"] = args["query"].asString();
		if (args["limit"].isIntegral())
			params["limit"] = args["limit"].asInt();
		return InvokeResult("SearchContacts", params);
	}
	else if (name == "make_call")
	{
		const Json::Value &target = args["target"];
		if (!target.isString() || target.asString().empty())
			return TextResult("target (string) is required", true);
		std::string t = target.asString();
		Json::Value params(Json::objectValue);
		const Json::Value &headers = args["headers"];
		std::string error = CheckHeaders(cfg, headers);
		if (!error.empty())
			return TextResult(error, true);
		if (headers.isObject() && !headers.empty())
			params["headers"] = headers;
		error = CheckTarget(cfg, "make_call", "make a call to", t);
		if (!error.empty())
			return TextResult(error, true);
		params["target"] = t;
		if (args["accountId"].isIntegral())
			params["accountId"] = args["accountId"].asInt();
		return InvokeResult("Call", params);
	}
	else if (name == "wait_for_call")
	{
		if (!args["condition"].isString())
			return TextResult("condition (string) is required", true);
		unsigned int uid = 0;
		if (!args["uid"].isNull())
		{
			if (!args["uid"].isIntegral() || args["uid"].asInt() <= 0)
				return TextResult("uid must be a positive integer", true);
			uid = args["uid"].asUInt();
		}
		int timeout = args["timeout"].isIntegral() ? args["timeout"].asInt() : 0;
		Json::Value result(Json::objectValue);
		std::string error = CallWatch::Wait(args["condition"].asString(), uid, timeout, result);
		if (!error.empty())
			return TextResult(error, true);
		return TextResult(ToJsonText(result), false);
	}
	else if (name == "send_dtmf" || name == "hold_call" || name == "transfer_call")
	{
		Json::Value params(Json::objectValue);
		std::string error;
		if (!GetUid(args, params, error))
			return TextResult(error, true);
		if (name == "send_dtmf")
		{
			if (!args["digits"].isString())
				return TextResult("digits (string) is required", true);
			params["digits"] = args["digits"].asString();
			return InvokeResult("SendDtmf", params);
		}
		if (name == "hold_call")
		{
			if (!args["hold"].isBool())
				return TextResult("hold (boolean) is required", true);
			params["state"] = args["hold"].asBool();
			return InvokeResult("Hold", params);
		}
		if (!args["target"].isString() || args["target"].asString().empty())
			return TextResult("target (string) is required", true);
		std::string t = args["target"].asString();
		error = CheckTarget(cfg, "transfer_call", "transfer a call to", t);
		if (!error.empty())
			return TextResult(error, true);
		params["target"] = t;
		return InvokeResult("Transfer", params);
	}
	else if (name == "send_sip_request")
	{
		return SendSipRequest(cfg, args);
	}
	else if (name == "get_call_invite")
	{
		Json::Value params(Json::objectValue);
		std::string error;
		if (!GetUid(args, params, error))
			return TextResult(error, true);
		const Json::Value &filter = args["headers"];
		if (!filter.isNull() && !filter.isArray())
			return TextResult("headers must be an array of header names", true);
		Json::Value state;
		int status = HostInvoke("GetCallInvite", params, state);
		if (status != TSIP_OK)
			return TextResult(std::string("tSIP returned error: ") + HostErrorName(status), true);
		if (!state["invite"].isString())
			return TextResult("tSIP does not support GetCallInvite (version too old?)", true);
		std::string invite = state["invite"].asString();
		if (invite.empty())
			return TextResult(state["incoming"].asBool() ? "INVITE text not available for this call" :
				"call is outgoing - there is no received INVITE", true);
		if (filter.isNull() || filter.empty())
			return TextResult(invite, false);
		// selected headers only (header part, unfolded lines not handled - not used in practice)
		Json::Value result(Json::objectValue);
		size_t pos = 0;
		while (pos < invite.size())
		{
			size_t end = invite.find('\n', pos);
			if (end == std::string::npos)
				end = invite.size();
			std::string line = invite.substr(pos, end - pos);
			pos = end + 1;
			if (!line.empty() && line[line.size() - 1] == '\r')
				line.erase(line.size() - 1);
			if (line.empty())
				break;	// end of headers
			size_t colon = line.find(':');
			if (colon == std::string::npos)
				continue;
			std::string name = line.substr(0, colon);
			while (!name.empty() && name[name.size() - 1] == ' ')
				name.erase(name.size() - 1);
			std::string value = line.substr(colon + 1);
			while (!value.empty() && value[0] == ' ')
				value.erase(0, 1);
			for (unsigned int i = 0; i < filter.size(); i++)
			{
				if (filter[i].isString() &&
					_stricmp(FullHeaderName(filter[i].asString()).c_str(), FullHeaderName(name).c_str()) == 0)
				{
					result[filter[i].asString()].append(value);
					break;
				}
			}
		}
		return TextResult(ToJsonText(result), false);
	}
	else if (name == "get_audio_level")
	{
		Json::Value params(Json::objectValue);
		std::string error;
		if (!GetUid(args, params, error))
			return TextResult(error, true);
		Json::Value state;
		int status = HostInvoke("GetCallAudioState", params, state);
		if (status != TSIP_OK)
			return TextResult(std::string("tSIP returned error: ") + HostErrorName(status), true);
		if (!state["rxLevel"].isIntegral())
			return TextResult("tSIP does not report audio level (version too old?)", true);
		Json::Value result(Json::objectValue);
		result["uid"] = state["uid"];
		result["rxLevel"] = state["rxLevel"];
		return TextResult(ToJsonText(result), false);
	}
	else if (name == "wait_for_audio")
	{
		return WaitForAudio(args);
	}
	else if (name == "list_audio_devices")
	{
		return InvokeResult("ListAudioDevices", Json::Value(Json::objectValue));
	}
	else if (name == "set_call_audio_input" || name == "set_call_audio_output")
	{
		Json::Value params(Json::objectValue);
		std::string error;
		if (!GetUid(args, params, error))
			return TextResult(error, true);
		if (args["configured"].isBool() && args["configured"].asBool())
		{
			params["configured"] = true;
		}
		else
		{
			if (!args["module"].isString() || !args["device"].isString())
				return TextResult("module and device (strings) are required, or configured = true", true);
			params["module"] = args["module"].asString();
			params["device"] = args["device"].asString();
		}
		return InvokeResult(name == "set_call_audio_input" ? "SetAudioInput" : "SetAudioOutput", params);
	}
	else if (name == "play_audio_file")
	{
		return PlayAudioFile(args);
	}
	else if (name == "set_sidetone")
	{
		Json::Value params(Json::objectValue);
		std::string error;
		if (!GetUid(args, params, error))
			return TextResult(error, true);
		if (!args["enabled"].isBool())
			return TextResult("enabled (boolean) is required", true);
		params["state"] = args["enabled"].asBool();
		if (args["level"].isIntegral())
			params["level"] = args["level"].asInt();
		return InvokeResult("Sidetone", params);
	}
	else if (name == "mute_call" || name == "start_recording")
	{
		Json::Value params(Json::objectValue);
		std::string error;
		if (!GetUid(args, params, error))
			return TextResult(error, true);
		if (name == "start_recording")
			return InvokeResult("StartRecording", params);
		if (!args["mute"].isBool())
			return TextResult("mute (boolean) is required", true);
		params["state"] = args["mute"].asBool();
		return InvokeResult("Mute", params);
	}
	else if (name == "send_message")
	{
		if (!args["target"].isString() || args["target"].asString().empty())
			return TextResult("target (string) is required", true);
		if (!args["text"].isString() || args["text"].asString().empty())
			return TextResult("text (string) is required", true);
		std::string t = args["target"].asString();
		std::string error = CheckTarget(cfg, "send_message", "send a message to", t);
		if (!error.empty())
			return TextResult(error, true);
		Json::Value params(Json::objectValue);
		params["target"] = t;
		params["text"] = args["text"].asString();
		if (args["accountId"].isIntegral())
			params["accountId"] = args["accountId"].asInt();
		return InvokeResult("SendMessage", params);
	}
	else if (name == "get_messages")
	{
		if (!args["peer"].isString() || args["peer"].asString().empty())
			return TextResult("peer (string) is required", true);
		Json::Value params(Json::objectValue);
		params["peer"] = args["peer"].asString();
		if (args["limit"].isIntegral())
			params["limit"] = args["limit"].asInt();
		return InvokeResult("GetMessages", params);
	}
	else if (name == "run_script")
	{
		if (!args["script"].isString())
			return TextResult("script (string) is required", true);
		Json::Value params(Json::objectValue);
		params["script"] = args["script"].asString();
		params["async"] = false;
		return InvokeResult("RunScript", params);
	}
	else if (name == "answer_call" || name == "hangup_call")
	{
		Json::Value params(Json::objectValue);
		std::string error;
		if (!GetUid(args, params, error))
			return TextResult(error, true);
		if (name == "answer_call")
			return InvokeResult("Answer", params);

		if (args["sipCode"].isIntegral())
			params["sipCode"] = args["sipCode"].asInt();
		if (args["reason"].isString())
			params["reason"] = args["reason"].asString();
		return InvokeResult("Hangup", params);
	}

	else if (name == "get_log")
	{
		int lines = args["lines"].isIntegral() ? args["lines"].asInt() : 50;
		if (lines < 1)
			lines = 1;
		if (lines > 1000)
			lines = 1000;
		Json::Value params(Json::objectValue);
		params["script"] = "return GetLogLines(" + std::to_string(lines) + ")";
		params["async"] = false;
		return InvokeResult("RunScript", params);
	}
	found = false;
	return Json::Value();
}
