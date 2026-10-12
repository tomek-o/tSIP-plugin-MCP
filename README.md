# tSIP MCP server plugin

[Model Context Protocol](https://modelcontextprotocol.io) (MCP) server for the
tSIP softphone: exposes tSIP functions as tools for AI clients (Claude Code,
Claude Desktop, IDE assistants, other MCP-capable agents).

Requires tSIP with plugin interface v2 (tSIP 0.3.16+). Written by Claude for Claude -
what am I doing here?

## Tools

| Tool          | Description                                              |
|---------------|----------------------------------------------------------|
| `get_app_info`| tSIP executable, profile directory, plugin interface     |
| `get_calls`   | active calls: uid, direction, state, peer URI and name   |
| `get_registration` | SIP registration state of accounts                  |
| `get_history` | call history, newest first (limit, filter: all/incoming/outgoing/missed, search) |
| `search_contacts` | phonebook search (query, limit): name, numbers, company, note |
| `make_call`   | start outgoing call (target: number or SIP URI)          |
| `answer_call` | answer incoming call (uid optional: current call)        |
| `hangup_call` | hang up / reject call (uid, sipCode, reason optional)    |
| `wait_for_call` | wait (max 60 s) until call is answered / ended or new incoming call arrives |
| `send_dtmf`   | send DTMF digits in call (IVR navigation)                |
| `hold_call`   | hold / resume call                                       |
| `transfer_call` | blind transfer; same checks as `make_call` (prefixes, confirmation) |
| `list_audio_devices` | audio devices per module, configured devices          |
| `set_call_audio_input` | switch call input (microphone) device / back to configured |
| `set_call_audio_output` | switch call output (speaker, headset) device / back to configured |
| `play_audio_file` | play WAV file into call (file name only, from `<profile>\audio` or profile dir); optional wait for end and input restore |
| `get_audio_level` | peak level of received audio (0..32767)                |
| `wait_for_audio` | wait until other party speaks / becomes silent (e.g. end of IVR announcement) |
| `mute_call`   | mute / unmute microphone in call                         |
| `set_sidetone` | sidetone on/off and level for call (hear transmitted audio, e.g. played file); not recorded |
| `start_recording` | start call recording (settings from tSIP configuration) |
| `send_message` | send text message (SIP MESSAGE); same checks as `make_call` |
| `get_messages` | text message history with peer                         |
| `run_script`  | run Lua script - **only if `allowRunScript` is true** (default: false); returns `return` values and `print()` output, Lua errors as tool errors |

`get_history` and `search_contacts` give the MCP client access to personal
data (who called, phonebook); disable them with `enabledTools` if not wanted.

Planned (needs tSIP plugin interface additions): messages, BLF / presence
state.

## Prompts

Templates for common tasks (MCP prompts; in Claude Code available as
`/mcp__tsip__<name>` commands). Listed only if tools they need are enabled.

| Prompt | Arguments | Description |
|--------|-----------|-------------|
| `callback_last_missed` | - | find last missed call and call back |
| `missed_calls_summary` | `period` (optional, default today) | missed calls grouped by caller |
| `call_contact` | `name` | find contact in phonebook and call |
| `message_contact` | `name`, `text` | find contact and send text message |
| `call_and_navigate_ivr` | `target`, `digits` | call, wait for answer, send DTMF |

## Transport and security

- MCP Streamable HTTP transport, JSON responses (no SSE stream):
  `POST http://127.0.0.1:<port>/mcp`.
- Listens on **127.0.0.1 only** (not reachable from network).
- Every request requires `Authorization: Bearer <token>`; token is generated
  randomly on first start and stored in the config file.
- `Host` / `Origin` headers must be local (`localhost`, `127.0.0.1`, `[::1]`) -
  protection against DNS rebinding from web pages.
- `make_call`:
  - `confirmCalls` (default: true): user confirms each call (and each
    `transfer_call` / `send_message`) in a message box,
  - `denyPrefixes` / `allowPrefixes`: block numbers (e.g. `"00"`, `"+"` for
    international calls) or allow only listed prefixes,
- `enabledTools`: limit available tools (empty = all).
- `activityStatus`: show tool calls in tSIP status bar and tray icon hint,
  e.g. "AI (MCP): make_call 123", for `activityStatusSeconds` (default 10)
  after last call; `"all"` (default), `"actions"` (only tools changing
  something - not get_* / search_contacts) or `"off"`.
- `allowRunScript` (default: false): `run_script` tool is not available unless
  enabled - Lua scripts have full access to tSIP (calls, settings, files,
  executing programs), so prefix checks and confirmation do not apply there.
- Every tool call, blocked number and rejected request is written to tSIP
  log (lines starting with `MCP:`).

Risks to keep in mind: an AI agent (or content it reads, e.g. a web page with
prompt injection) can try to make calls (toll fraud) or read call data
(privacy). Keep `confirmCalls` enabled unless the agent is fully trusted.

## Configuration

`MCP.cfg` (JSON) in tSIP profile directory (same directory as tSIP settings),
created on first start. If tSIP does not report profile directory, file next
to `MCP.dll` is used. Invalid file (e.g. JSON syntax error) is
not overwritten - server is not started until the file is fixed or deleted.

Why profile directory and not `phone\MCP.cfg` next to the DLL (as v1 plugins
do):

- profiles sharing one `phone` directory would share one config - same port
  (second instance fails with "port in use") and same token; for a server
  plugin port and token have to be per profile,
- program directory may not be writable (e.g. `Program Files`) - generated
  token could not be saved; profile directory is writable,
- config stays with other user data (backup, moving profile).

Plugin configured with older plugin version (`phone\MCP.cfg`): move the file
to profile directory, otherwise new token is generated and MCP client has to
be registered again.

```json
{
   "port" : 8765,
   "token" : "<generated>",
   "confirmCalls" : true,
   "allowRunScript" : false,
   "activityStatus" : "all",
   "activityStatusSeconds" : 10,
   "allowPrefixes" : [],
   "denyPrefixes" : [],
   "enabledTools" : []
}
```

Settings can be changed in tSIP: Settings / Plugins, select MCP plugin,
"Plugin settings..." - dialog saves the file and restarts the server
(also: token regeneration, copying Claude Code command). File can be
also edited manually - then disable and enable the plugin to apply.

## Connecting clients

Claude Code:

```
claude mcp add --transport http tsip http://127.0.0.1:8765/mcp --header "Authorization: Bearer <token>"
```

Other clients: configure HTTP (Streamable HTTP) MCP server with the URL above
and `Authorization: Bearer <token>` header.

## Building

Code::Blocks 25.03 with bundled MinGW (GCC 14.2, 32-bit) - three equivalent
ways:

- Code::Blocks: open `MCP.cbp`,
- `build.bat` (Release) / `build.bat debug` / `build.bat clean` - sets MinGW
  path (`MINGW_BIN`, default
  `G:\zrzut\codeblocks-25.03mingw-32bit-nosetup\MinGW\bin`), runs make,
  checks exports / dependencies and copies `MCP.dll` to tSIP
  `Debug_Build\phone` and `Release_Build\phone` (`TSIP_DIR`, default
  `..\tSIP\tSIP`),
- `mingw32-make` / `mingw32-make CONFIG=debug` (MinGW `bin` in `PATH`).

`Makefile` and `MCP.cbp` must be kept in sync (source list, flags).

Output: `bin/Release/MCP.dll` - statically linked (no MinGW runtime DLLs),
`tSipPluginInit` exported without stdcall decoration. Toolchain uses UCRT:
Windows 10/11, or Windows 7/8.1 with UCRT update (KB2999226).

## Testing without tSIP

`build.bat testhost` builds `bin/Release/TestHost.exe`, which loads `MCP.dll`
like tSIP does, with fake `Invoke()` results for all methods and simulated
call state events (see comment in `test/TestHost.cpp`).

Automated smoke test - starts TestHost in a temporary directory (own `MCP.cfg`,
port 18765), calls all tools and prompts, checks results (Python 3, standard
library only):

```
build.bat release
build.bat testhost
python test\smoke_test.py
```

Manual testing - current directory is reported as tSIP profile directory, so
`MCP.cfg` with token is created there:

```
cd bin\Release
TestHost.exe MCP.dll 60
```

Then send requests, e.g.:

```
curl -H "Authorization: Bearer <token>" -H "Content-Type: application/json" ^
  -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}" http://127.0.0.1:8765/mcp
```

## Files

- `main.cpp` - plugin interface v2 entry point, lifecycle, host helpers
- `McpServer.cpp` - MCP JSON-RPC handling, authorization, Host/Origin checks
- `HttpServer.cpp` - minimal local HTTP/1.1 server (thread per connection)
- `Tools.cpp` - tool definitions and execution (tSIP `Invoke` calls)
- `Activity.cpp` - tool activity shown in tSIP status
- `CallWatch.cpp` - call states from tSIP events, `wait_for_call`
- `Prompts.cpp` - MCP prompts (task templates)
- `SettingsDialog.cpp` - settings dialog (WinAPI, in-memory dialog template)
- `Config.cpp` - `MCP.cfg` handling, token generation, number checks
- plugin interface header `tSIP_plugin_v2.h` is used directly from tSIP
  source tree (`../tSIP/tSIP/phone`, expected as sibling directory)
- `jsoncpp/` - bundled JsonCpp (public domain / MIT)
- `test/TestHost.cpp` - test host
