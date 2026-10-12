#include "CallWatch.h"
#include "Host.h"
#include "tSIP_plugin_v2.h"

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>

namespace
{

enum
{
	STATE_CLOSED = 0,		// values as in tSIP Callback::ua_state_e
	STATE_INCOMING = 1,
	STATE_ESTABLISHED = 6
};

enum { MAX_CLOSED_CALLS = 50 };

struct CallState
{
	int state;
	std::string stateName;
	bool incoming;
	std::string peer;
	std::string peerName;
	unsigned long long seq;		///< event sequence number of last update
	CallState(void): state(STATE_CLOSED), incoming(false), seq(0) {}
};

std::mutex mutex;
std::condition_variable cv;
bool enabled = false;
unsigned long long eventSeq = 0;
std::map<unsigned int, CallState> calls;
unsigned int lastIncomingUid = 0;
unsigned long long lastIncomingSeq = 0;

void Update(CallState &cs, const Json::Value &jv)
{
	cs.state = jv.get("state", STATE_CLOSED).asInt();
	cs.stateName = jv.get("stateName", "").asString();
	if (jv["incoming"].isBool())
		cs.incoming = jv["incoming"].asBool();
	if (jv["peer"].isString())
		cs.peer = jv["peer"].asString();
	if (jv["peerName"].isString())
		cs.peerName = jv["peerName"].asString();
}

/** \brief Remove oldest closed calls - limit memory; caller holds lock */
void Prune(void)
{
	unsigned int closedCount = 0;
	for (std::map<unsigned int, CallState>::const_iterator it = calls.begin(); it != calls.end(); ++it)
	{
		if (it->second.state == STATE_CLOSED)
			closedCount++;
	}
	// uids are increasing: lowest uids are the oldest
	for (std::map<unsigned int, CallState>::iterator it = calls.begin();
		it != calls.end() && closedCount > MAX_CLOSED_CALLS; )
	{
		if (it->second.state == STATE_CLOSED)
		{
			calls.erase(it++);
			closedCount--;
		}
		else
		{
			++it;
		}
	}
}

/** \brief Merge GetCalls snapshot: only calls not updated by event after snapshot was requested */
void Sync(unsigned long long seqBefore)
{
	Json::Value result;
	if (HostInvoke("GetCalls", Json::Value(Json::objectValue), result) != TSIP_OK || !result["calls"].isArray())
		return;
	std::lock_guard<std::mutex> lock(mutex);
	const Json::Value &jcalls = result["calls"];
	for (unsigned int i = 0; i < jcalls.size(); i++)
	{
		unsigned int uid = jcalls[i].get("uid", 0).asUInt();
		if (uid == 0)
			continue;
		CallState &cs = calls[uid];
		if (cs.seq <= seqBefore)
			Update(cs, jcalls[i]);
	}
}

void FillCall(Json::Value &result, unsigned int uid, const CallState &cs)
{
	result["uid"] = uid;
	result["state"] = cs.state;
	result["stateName"] = cs.stateName;
	result["incoming"] = cs.incoming;
	result["peer"] = cs.peer;
	result["peerName"] = cs.peerName;
}

}	// namespace


void CallWatch::Start(void)
{
	std::lock_guard<std::mutex> lock(mutex);
	enabled = true;
	calls.clear();
	lastIncomingUid = 0;
	lastIncomingSeq = 0;
}

void CallWatch::Shutdown(void)
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		enabled = false;
	}
	cv.notify_all();
}

void CallWatch::OnCallStateEvent(const char *json)
{
	Json::Value jv;
	Json::Reader reader;
	if (json == NULL || !reader.parse(json, jv) || !jv.isObject())
		return;
	unsigned int uid = jv.get("uid", 0).asUInt();
	if (uid == 0)
		return;
	{
		std::lock_guard<std::mutex> lock(mutex);
		eventSeq++;
		CallState &cs = calls[uid];
		Update(cs, jv);
		cs.seq = eventSeq;
		if (cs.state == STATE_INCOMING)
		{
			lastIncomingUid = uid;
			lastIncomingSeq = eventSeq;
		}
		if (cs.state == STATE_CLOSED)
			Prune();
	}
	cv.notify_all();
}

std::string CallWatch::Wait(const std::string &condition, unsigned int uid, int timeoutS, Json::Value &result)
{
	if (condition != "established" && condition != "ended" && condition != "incoming")
		return "condition must be one of: established, ended, incoming";
	if (timeoutS <= 0)
		timeoutS = DEFAULT_TIMEOUT_S;
	if (timeoutS > MAX_TIMEOUT_S)
		timeoutS = MAX_TIMEOUT_S;

	unsigned long long seqBefore;
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (!enabled)
			return "server is stopping";
		seqBefore = eventSeq;
	}
	// calls existing before plugin start / before first event
	Sync(seqBefore);

	std::unique_lock<std::mutex> lock(mutex);

	if (condition != "incoming")
	{
		if (uid == 0)
		{
			// only active call
			unsigned int found = 0;
			unsigned int count = 0;
			for (std::map<unsigned int, CallState>::const_iterator it = calls.begin(); it != calls.end(); ++it)
			{
				if (it->second.state != STATE_CLOSED)
				{
					found = it->first;
					count++;
				}
			}
			if (count != 1)
				return count == 0 ? "no active call" : "more than one active call - specify uid";
			uid = found;
		}
		else if (calls.find(uid) == calls.end())
		{
			// not active and not seen since plugin start (or long ago) - most likely wrong uid
			return "unknown call uid (no such active call and not seen recently)";
		}
	}

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutS);
	for (;;)
	{
		if (!enabled)
		{
			result["result"] = "stopping";
			return "";
		}
		if (condition == "incoming")
		{
			if (lastIncomingSeq > seqBefore)
			{
				result["result"] = "incoming";
				FillCall(result, lastIncomingUid, calls[lastIncomingUid]);
				return "";
			}
		}
		else
		{
			const CallState &cs = calls[uid];
			if (cs.state == STATE_CLOSED)
			{
				result["result"] = "ended";
				FillCall(result, uid, cs);
				return "";
			}
			if (condition == "established" && cs.state == STATE_ESTABLISHED)
			{
				result["result"] = "established";
				FillCall(result, uid, cs);
				return "";
			}
		}
		if (cv.wait_until(lock, deadline) == std::cv_status::timeout)
		{
			result["result"] = "timeout";
			if (condition != "incoming")
				FillCall(result, uid, calls[uid]);
			return "";
		}
	}
}
