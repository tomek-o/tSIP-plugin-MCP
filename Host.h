/** \file
	\brief Access to tSIP host functions (plugin interface v2), implemented in main.cpp
*/
#ifndef HostH
#define HostH

#include <json/json.h>
#include <string>

/** \brief Log to tSIP log (printf-like), adds "MCP: " prefix and newline */
void HostLog(const char *fmt, ...);

/** \brief Call host Invoke()
	\param result parsed JSON result (object), valid if TSIP_OK returned
	\return TSIP_OK or TSIP_ERR_* code
*/
int HostInvoke(const char *method, const Json::Value &params, Json::Value &result);

/** \brief Set tSIP (Lua) variable - direct, thread-safe host call
	\return true on success
*/
bool HostSetVariable(const char *name, const std::string &value);

/** \brief Human readable name of TSIP_ERR_* code */
const char* HostErrorName(int status);

#endif
