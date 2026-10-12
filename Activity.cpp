#include "Activity.h"
#include "Config.h"
#include "Host.h"

#include <windows.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace
{

/* Status text is passed through tSIP variable, script itself is constant:
   tool arguments (e.g. call target) never become part of Lua code.
   Scripts are queued asynchronously (async RunScript) - plugin threads never
   wait for tSIP main thread here, so Stop() (main thread) can join clearing
   thread without risk of blocking GUI.
*/
const char *VARIABLE_NAME = "mcp_activity";
const char *SCRIPT_SET = "SetAppStatus(\"MCP\", 20, (GetVariable(\"mcp_activity\")))";
const char *SCRIPT_CLEAR = "SetAppStatus(\"MCP\", 20, \"\")";

std::mutex mutex;
std::condition_variable cv;
std::thread thread;
bool running = false;
bool shown = false;
std::chrono::steady_clock::time_point clearTime;

Config::ActivityStatus mode = Config::ACTIVITY_ALL;
unsigned int seconds = 10;

/** \brief UTF-8 -> ANSI (Lua / tSIP status bar use ANSI) */
std::string Utf8ToAnsi(const std::string &text)
{
	int wlen = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, NULL, 0);
	if (wlen <= 0)
		return "";
	std::wstring wtext(wlen, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &wtext[0], wlen);
	int len = WideCharToMultiByte(CP_ACP, 0, wtext.c_str(), -1, NULL, 0, NULL, NULL);
	if (len <= 0)
		return "";
	std::string out(len, '\0');
	WideCharToMultiByte(CP_ACP, 0, wtext.c_str(), -1, &out[0], len, NULL, NULL);
	out.resize(len - 1);
	return out;
}

void RunScriptAsync(const char *script)
{
	Json::Value params(Json::objectValue);
	params["script"] = script;
	params["async"] = true;
	Json::Value result;
	HostInvoke("RunScript", params, result);
}

void ThreadProc(void)
{
	std::unique_lock<std::mutex> lock(mutex);
	while (running)
	{
		if (shown)
		{
			if (cv.wait_until(lock, clearTime) == std::cv_status::timeout && shown &&
				std::chrono::steady_clock::now() >= clearTime)
			{
				shown = false;
				lock.unlock();
				RunScriptAsync(SCRIPT_CLEAR);
				lock.lock();
			}
		}
		else
		{
			cv.wait(lock);
		}
	}
}

}	// namespace


void Activity::Start(const Config &cfg)
{
	std::lock_guard<std::mutex> lock(mutex);
	mode = cfg.activityStatus;
	seconds = cfg.activityStatusSeconds;
	shown = false;
	if (mode == Config::ACTIVITY_OFF)
		return;
	running = true;
	thread = std::thread(ThreadProc);
}

void Activity::Stop(void)
{
	bool wasShown;
	{
		std::lock_guard<std::mutex> lock(mutex);
		running = false;
		wasShown = shown;
		shown = false;
	}
	cv.notify_all();
	if (thread.joinable())
		thread.join();
	if (wasShown)
		RunScriptAsync(SCRIPT_CLEAR);
}

void Activity::Report(const std::string &tool, const std::string &detail, bool readOnly)
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (!running || mode == Config::ACTIVITY_OFF || (mode == Config::ACTIVITY_ACTIONS && readOnly))
			return;
		std::string text = "AI (MCP): " + tool;
		if (!detail.empty())
			text += " " + detail;
		if (!HostSetVariable(VARIABLE_NAME, Utf8ToAnsi(text)))
			return;
		shown = true;
		clearTime = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
	}
	cv.notify_all();
	RunScriptAsync(SCRIPT_SET);
}
