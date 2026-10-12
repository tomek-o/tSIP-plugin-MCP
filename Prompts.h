/** \file
	\brief MCP prompts (templates for common tasks) exposed by tSIP plugin
*/
#ifndef PromptsH
#define PromptsH

#include <json/json.h>
#include <string>

struct Config;

namespace Prompts
{
	/** \brief Prompt definitions for prompts/list result ("prompts" array);
		prompts requiring disabled tools are not listed
	*/
	Json::Value List(const Config &cfg);

	/** \brief prompts/get result: {"description":..., "messages":[...]}
		\param found set to false if prompt does not exist / is not available
		\param error set if required argument is missing
	*/
	Json::Value Get(const Config &cfg, const std::string &name, const Json::Value &args, bool &found, std::string &error);
}

#endif
