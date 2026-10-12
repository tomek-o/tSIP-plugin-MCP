/** \file
	\brief MCP tools exposed by tSIP plugin
*/
#ifndef ToolsH
#define ToolsH

#include <json/json.h>
#include <string>

struct Config;

namespace Tools
{
	/** \brief Tool definitions for tools/list result ("tools" array) */
	Json::Value List(const Config &cfg);

	/** \brief Execute tool
		\return MCP CallToolResult: {"content":[{"type":"text","text":...}],"isError":bool}
		\param found set to false if tool does not exist / is disabled
	*/
	Json::Value Call(const Config &cfg, const std::string &name, const Json::Value &args, bool &found);
}

#endif
