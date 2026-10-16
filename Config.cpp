#include "Config.h"

#include <windows.h>
#include <ctype.h>
#include <bcrypt.h>
#include <json/json.h>
#include <fstream>
#include <sstream>

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif

namespace
{

void ReadStringArray(const Json::Value &jv, std::vector<std::string> &out)
{
	if (!jv.isArray())
		return;
	out.clear();
	for (unsigned int i = 0; i < jv.size(); i++)
	{
		if (jv[i].isString())
			out.push_back(jv[i].asString());
	}
}

Json::Value WriteStringArray(const std::vector<std::string> &in)
{
	Json::Value jv(Json::arrayValue);
	for (size_t i = 0; i < in.size(); i++)
		jv.append(in[i]);
	return jv;
}

/** \brief Keep only digits, '+', '*', '#' - for prefix comparison */
std::string NormalizeNumber(const std::string &number)
{
	std::string target = number;
	// strip "sip:" / "sips:" / "tel:" and host part
	size_t colon = target.find(':');
	if (colon != std::string::npos && colon < 5)
		target = target.substr(colon + 1);
	size_t at = target.find('@');
	if (at != std::string::npos)
		target = target.substr(0, at);
	std::string out;
	for (size_t i = 0; i < target.size(); i++)
	{
		char c = target[i];
		if ((c >= '0' && c <= '9') || c == '+' || c == '*' || c == '#')
			out += c;
	}
	return out;
}

bool StartsWith(const std::string &text, const std::string &prefix)
{
	return text.compare(0, prefix.size(), prefix) == 0;
}

}	// namespace

namespace {
std::string ToLower(const std::string &s)
{
	std::string ret = s;
	for (size_t i = 0; i < ret.size(); i++)
		ret[i] = static_cast<char>(tolower(static_cast<unsigned char>(ret[i])));
	return ret;
}
}	// namespace

Config::Config(void):
	port(8765),
	confirmCalls(true),
	allowRunScript(false),
	allowConfigModification(false),
	activityStatus(ACTIVITY_ALL),
	activityStatusSeconds(10)
{
	// headers carrying call metadata / auto-answer hints; identity headers
	// (P-Asserted-Identity, Privacy, Diversion, ...) need explicit configuration
	allowedCallHeaders.push_back("X-*");
	allowedCallHeaders.push_back("Subject");
	allowedCallHeaders.push_back("Call-Info");
	allowedCallHeaders.push_back("Alert-Info");
	allowedCallHeaders.push_back("Priority");
	allowedRequestMethods.push_back("OPTIONS");
	allowedRequestMethods.push_back("INFO");
}

int Config::Load(const std::string &path)
{
	if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES)
		return LOAD_NOT_FOUND;
	std::ifstream ifs(path.c_str(), std::ios::binary);
	if (!ifs)
		return LOAD_INVALID;
	std::stringstream ss;
	ss << ifs.rdbuf();

	Json::Value root;
	Json::Reader reader;
	if (!reader.parse(ss.str(), root) || !root.isObject())
		return LOAD_INVALID;

	if (root["port"].isUInt() || root["port"].isInt())
	{
		unsigned int p = root["port"].asUInt();
		if (p > 0 && p < 65536)
			port = p;
	}
	if (root["token"].isString())
		token = root["token"].asString();
	if (root["confirmCalls"].isBool())
		confirmCalls = root["confirmCalls"].asBool();
	if (root["allowRunScript"].isBool())
		allowRunScript = root["allowRunScript"].asBool();
	if (root["allowConfigModification"].isBool())
		allowConfigModification = root["allowConfigModification"].asBool();
	if (root["activityStatus"].isString())
	{
		std::string s = root["activityStatus"].asString();
		if (s == "off")
			activityStatus = ACTIVITY_OFF;
		else if (s == "actions")
			activityStatus = ACTIVITY_ACTIONS;
		else if (s == "all")
			activityStatus = ACTIVITY_ALL;
	}
	if (root["activityStatusSeconds"].isUInt() || root["activityStatusSeconds"].isInt())
	{
		int s = root["activityStatusSeconds"].asInt();
		if (s >= 1 && s <= 3600)
			activityStatusSeconds = s;
	}
	ReadStringArray(root["allowPrefixes"], allowPrefixes);
	ReadStringArray(root["denyPrefixes"], denyPrefixes);
	ReadStringArray(root["enabledTools"], enabledTools);
	if (root.isMember("allowedCallHeaders"))
	{
		allowedCallHeaders.clear();
		ReadStringArray(root["allowedCallHeaders"], allowedCallHeaders);
	}
	if (root.isMember("allowedRequestMethods"))
	{
		allowedRequestMethods.clear();
		ReadStringArray(root["allowedRequestMethods"], allowedRequestMethods);
	}
	return LOAD_OK;
}

int Config::Save(const std::string &path) const
{
	Json::Value root(Json::objectValue);
	root["port"] = port;
	root["token"] = token;
	root["confirmCalls"] = confirmCalls;
	root["allowRunScript"] = allowRunScript;
	root["allowConfigModification"] = allowConfigModification;
	static const char* const activityNames[] = { "off", "actions", "all" };
	root["activityStatus"] = activityNames[activityStatus];
	root["activityStatusSeconds"] = activityStatusSeconds;
	root["allowPrefixes"] = WriteStringArray(allowPrefixes);
	root["denyPrefixes"] = WriteStringArray(denyPrefixes);
	root["enabledTools"] = WriteStringArray(enabledTools);
	root["allowedCallHeaders"] = WriteStringArray(allowedCallHeaders);
	root["allowedRequestMethods"] = WriteStringArray(allowedRequestMethods);

	Json::StyledWriter writer;
	std::ofstream ofs(path.c_str(), std::ios::binary);
	if (!ofs)
		return -1;
	ofs << writer.write(root);
	return ofs.good() ? 0 : -2;
}

bool Config::EnsureToken(void)
{
	if (!token.empty())
		return false;
	unsigned char buf[24];
	if (BCryptGenRandom(NULL, buf, sizeof(buf), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != STATUS_SUCCESS)
		return false;
	static const char hex[] = "0123456789abcdef";
	for (size_t i = 0; i < sizeof(buf); i++)
	{
		token += hex[buf[i] >> 4];
		token += hex[buf[i] & 0x0F];
	}
	return true;
}

bool Config::IsToolEnabled(const std::string &name) const
{
	if (name == "run_script" && !allowRunScript)
		return false;
	if ((name == "get_settings" || name == "set_settings" || name == "get_button_settings" || name == "set_button_settings") && !allowConfigModification)
		return false;
	if (name == "send_sip_request" && allowedRequestMethods.empty())
		return false;
	if (enabledTools.empty())
		return true;
	for (size_t i = 0; i < enabledTools.size(); i++)
	{
		if (enabledTools[i] == name)
			return true;
	}
	return false;
}

bool Config::IsNumberAllowed(const std::string &number, std::string &reason) const
{
	std::string n = NormalizeNumber(number);
	if (n.empty())
	{
		// non-numeric SIP URI (e.g. sip:music@iptel.org) - prefixes do not apply
		// unless allow list is used
		if (!allowPrefixes.empty())
		{
			reason = "target is not a number and allowPrefixes list is configured";
			return false;
		}
		return true;
	}
	for (size_t i = 0; i < denyPrefixes.size(); i++)
	{
		if (!denyPrefixes[i].empty() && StartsWith(n, denyPrefixes[i]))
		{
			reason = "number matches denied prefix " + denyPrefixes[i];
			return false;
		}
	}
	if (allowPrefixes.empty())
		return true;
	for (size_t i = 0; i < allowPrefixes.size(); i++)
	{
		if (StartsWith(n, allowPrefixes[i]))
			return true;
	}
	reason = "number does not match any allowed prefix";
	return false;
}

std::string GetDllConfigPath(void)
{
	HMODULE hModule = NULL;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCSTR>(&GetDllConfigPath), &hModule);
	char path[MAX_PATH] = {0};
	GetModuleFileNameA(hModule, path, sizeof(path) - 1);
	std::string p = path;
	size_t dot = p.find_last_of('.');
	size_t slash = p.find_last_of("\\/");
	if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
		p = p.substr(0, dot);
	return p + ".cfg";
}

bool Config::IsRequestMethodAllowed(const std::string &method) const
{
	for (size_t i = 0; i < allowedRequestMethods.size(); i++)
	{
		if (allowedRequestMethods[i] == method)
			return true;
	}
	return false;
}

bool Config::IsCallHeaderAllowed(const std::string &name) const
{
	std::string n = ToLower(name);
	for (size_t i = 0; i < allowedCallHeaders.size(); i++)
	{
		std::string a = ToLower(allowedCallHeaders[i]);
		if (!a.empty() && a[a.size() - 1] == '*')
		{
			a.erase(a.size() - 1);
			if (n.size() > a.size() && n.compare(0, a.size(), a) == 0)
				return true;
		}
		else if (n == a)
		{
			return true;
		}
	}
	return false;
}
