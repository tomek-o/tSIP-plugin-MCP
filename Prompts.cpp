#include "Prompts.h"
#include "Config.h"

namespace
{

struct PromptArg
{
	const char *name;
	const char *description;
	bool required;
};

struct PromptDef
{
	const char *name;
	const char *title;
	const char *description;
	const char *requiredTools;	///< comma separated; prompt listed only if all are enabled
	PromptArg args[3];			///< terminated by entry with name == NULL
	const char *text;			///< prompt text, {arg} placeholders replaced with arguments
};

const PromptDef prompts[] = {
	{
		"callback_last_missed", "Call back last missed call",
		"Find the most recent missed call and call that number back.",
		"get_history,search_contacts,make_call,wait_for_call",
		{ {NULL, NULL, false} },
		"Using tSIP tools: find my most recent missed call (get_history with filter \"missed\", limit 1). "
		"Tell me who it was (number, display name, phonebook name - use search_contacts if useful) and when. "
		"Then call this number back with make_call and use wait_for_call to report whether the call "
		"was answered. If there is no missed call, just tell me."
	},
	{
		"missed_calls_summary", "Summarize missed calls",
		"Summarize recent missed calls, grouped by caller.",
		"get_history,search_contacts",
		{ {"period", "Time period, e.g. \"today\", \"last 3 days\" (default: today)", false}, {NULL, NULL, false} },
		"Using tSIP tools: list my missed calls for period: {period}. Use get_history with filter "
		"\"missed\" (increase limit if needed; timestamps are local time). Group calls by caller, show "
		"caller name (phonebook name, display name or search_contacts result), number of attempts and the "
		"time of the last attempt, most recent first. Do not call anyone."
	},
	{
		"call_contact", "Call contact by name",
		"Find contact in tSIP phonebook and call it.",
		"search_contacts,make_call,wait_for_call",
		{ {"name", "Contact name (or part of it)", true}, {NULL, NULL, false} },
		"Using tSIP tools: find \"{name}\" in my phonebook with search_contacts. If there is exactly one "
		"matching contact with one number, call it with make_call. If there are more contacts or numbers, "
		"show them and ask me which one to call. After calling, use wait_for_call to report whether the "
		"call was answered."
	},
	{
		"message_contact", "Send text message to contact",
		"Find contact in tSIP phonebook and send a text message.",
		"search_contacts,send_message",
		{ {"name", "Contact name (or part of it), number or SIP URI", true},
		  {"text", "Message text", true}, {NULL, NULL, false} },
		"Using tSIP tools: send text message \"{text}\" to \"{name}\". If it is not a number or SIP URI, "
		"find the contact with search_contacts first; if there is more than one match, ask me which one. "
		"Send with send_message and tell me to whom it was sent."
	},
	{
		"call_and_navigate_ivr", "Call and navigate voice menu",
		"Call a number and send DTMF digits to navigate an automated voice menu (IVR).",
		"make_call,wait_for_call,send_dtmf",
		{ {"target", "Number or SIP URI to call", true},
		  {"digits", "DTMF digits to send after call is answered, e.g. \"1\" or \"2#\"", true},
		  {NULL, NULL, false} },
		"Using tSIP tools: call {target} with make_call, wait until it is answered with wait_for_call "
		"(condition \"established\"), then send DTMF digits \"{digits}\" with send_dtmf. Leave the call "
		"connected and tell me the result; if the call is not answered, tell me."
	},
};

bool ToolsEnabled(const Config &cfg, const std::string &list)
{
	std::string item;
	for (size_t i = 0; i <= list.size(); i++)
	{
		char c = (i < list.size()) ? list[i] : ',';
		if (c == ',')
		{
			if (!item.empty() && !cfg.IsToolEnabled(item))
				return false;
			item.clear();
		}
		else
		{
			item += c;
		}
	}
	return true;
}

std::string Replace(std::string text, const std::string &from, const std::string &to)
{
	size_t pos = 0;
	while ((pos = text.find(from, pos)) != std::string::npos)
	{
		text.replace(pos, from.size(), to);
		pos += to.size();
	}
	return text;
}

}	// namespace


Json::Value Prompts::List(const Config &cfg)
{
	Json::Value list(Json::arrayValue);
	for (size_t i = 0; i < sizeof(prompts) / sizeof(prompts[0]); i++)
	{
		const PromptDef &p = prompts[i];
		if (!ToolsEnabled(cfg, p.requiredTools))
			continue;
		Json::Value jp(Json::objectValue);
		jp["name"] = p.name;
		jp["title"] = p.title;
		jp["description"] = p.description;
		Json::Value args(Json::arrayValue);
		for (const PromptArg *a = p.args; a->name; a++)
		{
			Json::Value ja(Json::objectValue);
			ja["name"] = a->name;
			ja["description"] = a->description;
			ja["required"] = a->required;
			args.append(ja);
		}
		jp["arguments"] = args;
		list.append(jp);
	}
	return list;
}

Json::Value Prompts::Get(const Config &cfg, const std::string &name, const Json::Value &args, bool &found, std::string &error)
{
	found = false;
	for (size_t i = 0; i < sizeof(prompts) / sizeof(prompts[0]); i++)
	{
		const PromptDef &p = prompts[i];
		if (name != p.name || !ToolsEnabled(cfg, p.requiredTools))
			continue;
		found = true;
		std::string text = p.text;
		for (const PromptArg *a = p.args; a->name; a++)
		{
			std::string value = args[a->name].isString() ? args[a->name].asString() : "";
			if (value.empty())
			{
				if (a->required)
				{
					error = std::string("Missing required argument: ") + a->name;
					return Json::Value();
				}
				if (std::string(a->name) == "period")
					value = "today";
			}
			text = Replace(text, std::string("{") + a->name + "}", value);
		}
		Json::Value content(Json::objectValue);
		content["type"] = "text";
		content["text"] = text;
		Json::Value message(Json::objectValue);
		message["role"] = "user";
		message["content"] = content;
		Json::Value messages(Json::arrayValue);
		messages.append(message);
		Json::Value result(Json::objectValue);
		result["description"] = p.description;
		result["messages"] = messages;
		return result;
	}
	return Json::Value();
}
