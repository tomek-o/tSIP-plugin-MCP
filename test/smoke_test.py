"""Smoke test for MCP plugin: starts TestHost with MCP.dll (fake tSIP),
calls all tools and prompts over HTTP, checks results.

Usage (from plugin directory, after build.bat release + build.bat testhost):
    python test/smoke_test.py [bin\\Release]

Uses a temporary working directory with its own MCP.cfg (confirmation disabled,
run_script allowed, "900" denied prefix), port 18765 - does not touch tSIP config.
Python standard library only.
"""

import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

PORT = 18765
TOKEN = "smoketest0123456789abcdef"
URL = "http://127.0.0.1:%d/mcp" % PORT

failures = []
request_id = 0


def rpc(method, params=None, token=TOKEN):
    global request_id
    request_id += 1
    body = {"jsonrpc": "2.0", "id": request_id, "method": method}
    if params is not None:
        body["params"] = params
    req = urllib.request.Request(URL, data=json.dumps(body).encode("utf-8"), method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("Accept", "application/json, text/event-stream")
    if token:
        req.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return resp.status, json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        return e.code, None


def check(name, condition, detail=""):
    print("%s %s %s" % ("OK  " if condition else "FAIL", name, detail))
    if not condition:
        failures.append(name)


def tool(name, args=None):
    status, resp = rpc("tools/call", {"name": name, "arguments": args or {}})
    if status != 200 or resp is None or "result" not in resp:
        return None, True, resp
    result = resp["result"]
    text = result["content"][0]["text"]
    data = None
    try:
        data = json.loads(text)
    except ValueError:
        pass
    return data, result.get("isError", False), text


def main():
    bin_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join("bin", "Release")
    dll = os.path.abspath(os.path.join(bin_dir, "MCP.dll"))
    host = os.path.abspath(os.path.join(bin_dir, "TestHost.exe"))
    for f in (dll, host):
        if not os.path.exists(f):
            print("missing %s (build.bat release, build.bat testhost)" % f)
            return 2

    work = tempfile.mkdtemp(prefix="mcp_smoke_")
    with open(os.path.join(work, "MCP.cfg"), "w") as f:
        json.dump({"port": PORT, "token": TOKEN, "confirmCalls": False, "allowRunScript": True,
                   "activityStatus": "all", "denyPrefixes": ["900"]}, f)
    log = open(os.path.join(work, "testhost.log"), "w")
    proc = subprocess.Popen([host, dll, "60"], cwd=work, stdout=log, stderr=subprocess.STDOUT)
    try:
        for _ in range(50):
            try:
                if rpc("ping")[0] == 200:
                    break
            except OSError:
                pass
            time.sleep(0.1)

        # protocol / security
        check("unauthorized request rejected", rpc("ping", token="wrong")[0] == 401)
        status, resp = rpc("initialize", {"protocolVersion": "2025-06-18", "clientInfo": {"name": "smoke"}})
        caps = resp["result"]["capabilities"] if resp else {}
        check("initialize", status == 200 and "tools" in caps and "prompts" in caps)
        status, resp = rpc("tools/list")
        names = [t["name"] for t in resp["result"]["tools"]]
        check("tools/list", "run_script" in names and "send_message" in names, "%d tools" % len(names))
        check("unknown method", rpc("no/such/method")[1].get("error", {}).get("code") == -32601)

        # read-only tools
        data, err, _ = tool("get_app_info")
        check("get_app_info", not err and data["pluginInterface"] == 2)
        data, err, _ = tool("get_calls")
        check("get_calls (UTF-8)", not err and data["calls"][0]["peerName"] == u"Zażółć Gęślą")
        data, err, _ = tool("get_registration")
        check("get_registration", not err and data["accounts"][0]["registered"])
        data, err, _ = tool("get_history", {"filter": "missed", "limit": 5})
        check("get_history", not err and data["entries"][0]["missed"])
        data, err, _ = tool("search_contacts", {"query": "test"})
        check("search_contacts", not err and data["contacts"][0]["name"] == "Test Contact")
        data, err, _ = tool("get_messages", {"peer": "123"})
        check("get_messages", not err and data["messages"][0]["body"] == "hello")

        # call flow with simulated events
        data, err, _ = tool("make_call", {"target": "123"})
        check("make_call", not err and data["uid"] == 7)
        data, err, _ = tool("wait_for_call", {"condition": "established", "uid": 7, "timeout": 5})
        check("wait_for_call established", not err and data["result"] == "established")
        for name, args, key, value in (
                ("hold_call", {"uid": 7, "hold": True}, "hold", True),
                ("mute_call", {"uid": 7, "mute": True}, "mute", True),
                ("start_recording", {"uid": 7}, "recording", True)):
            data, err, _ = tool(name, args)
            check(name, not err and data[key] == value)
        _, err, _ = tool("send_dtmf", {"uid": 7, "digits": "12#"})
        check("send_dtmf", not err)
        _, err, _ = tool("transfer_call", {"uid": 7, "target": "456"})
        check("transfer_call", not err)
        _, err, _ = tool("hangup_call", {"uid": 7})
        check("hangup_call", not err)
        data, err, _ = tool("wait_for_call", {"condition": "ended", "uid": 7, "timeout": 5})
        check("wait_for_call ended", not err and data["result"] == "ended")
        _, err, _ = tool("answer_call", {"uid": 5})
        check("answer_call", not err)
        _, err, _ = tool("send_message", {"target": "123", "text": u"zażółć"})
        check("send_message", not err)

        # restrictions and errors
        _, err, text = tool("make_call", {"target": "900123"})
        check("denied prefix blocked", err and "Not allowed" in text, text)
        _, err, text = tool("send_message", {"target": "900123", "text": "x"})
        check("denied prefix blocked (message)", err and "Not allowed" in text, text)
        _, err, text = tool("send_dtmf", {"digits": "12"})
        check("send_dtmf current call", not err, text)
        _, err, text = tool("hangup_call", {"uid": 999})
        check("unknown uid error", err and "not available" in text, text)
        _, err, text = tool("wait_for_call", {"condition": "ended", "uid": 999})
        check("wait_for_call unknown uid", err and "unknown call uid" in text, text)
        _, err, text = tool("wait_for_call", {"condition": "bad"})
        check("wait_for_call bad condition", err, text)
        data, err, text = tool("wait_for_call", {"condition": "incoming", "timeout": 1})
        check("wait_for_call timeout", not err and data["result"] == "timeout", text)

        # audio
        data, err, text = tool("set_sidetone", {"uid": 5, "enabled": True, "level": 50})
        check("set_sidetone", not err and data["sidetone"] is True, text)
        data, err, _ = tool("list_audio_devices")
        check("list_audio_devices", not err and data["modules"][0]["module"] == "wasapi")
        _, err, _ = tool("set_call_audio_output", {"uid": 5, "module": "wasapi", "device": "Headset"})
        check("set_call_audio_output", not err)
        _, err, text = tool("set_call_audio_output", {"uid": 5, "module": "wasapi", "device": "Nope"})
        check("set_call_audio_output unknown device", err and "device not found" in text, text)
        _, err, _ = tool("set_call_audio_input", {"uid": 5, "configured": True})
        check("set_call_audio_input configured", not err)
        data, err, text = tool("play_audio_file", {"uid": 5, "file": "test.wav", "wait": True, "timeout": 5})
        check("play_audio_file wait", not err and data["result"] == "finished" and data["inputRestored"], text)
        _, err, text = tool("play_audio_file", {"uid": 5, "file": "..\\x.wav"})
        check("play_audio_file path rejected", err, text)

        data, err, text = tool("get_audio_level", {"uid": 5})
        check("get_audio_level", not err and "rxLevel" in data, text)
        data, err, text = tool("wait_for_audio", {"uid": 5, "condition": "speech", "timeout": 5})
        check("wait_for_audio speech", not err and data["result"] == "speech", text)
        data, err, text = tool("wait_for_audio", {"uid": 5, "condition": "silence", "duration_ms": 500, "timeout": 5})
        check("wait_for_audio silence", not err and data["result"] == "silence", text)
        data, err, text = tool("wait_for_audio", {"uid": 5, "condition": "silence", "duration_ms": 1500, "timeout": 3})
        check("wait_for_audio timeout", not err and data["result"] == "timeout", text)

        # run_script
        data, err, _ = tool("run_script", {"script": "return 42"})
        check("run_script", not err and data["return"] == "42" and "output" in data)
        _, err, text = tool("run_script", {"script": "error()"})
        check("run_script error", err and "test error" in text and "before error" in text, text)

        # prompts
        status, resp = rpc("prompts/list")
        prompts = [p["name"] for p in resp["result"]["prompts"]]
        check("prompts/list", "call_contact" in prompts, "%d prompts" % len(prompts))
        status, resp = rpc("prompts/get", {"name": "call_contact", "arguments": {"name": "Jan"}})
        check("prompts/get", "Jan" in resp["result"]["messages"][0]["content"]["text"])
        status, resp = rpc("prompts/get", {"name": "call_contact"})
        check("prompts/get missing argument", "error" in resp)
        status, resp = rpc("prompts/get", {"name": "missed_calls_summary"})
        check("prompts/get default argument", "today" in resp["result"]["messages"][0]["content"]["text"])
    finally:
        proc.terminate()
        proc.wait()
        log.close()

    print("")
    if failures:
        print("FAILED: %d (%s); TestHost log: %s" % (len(failures), ", ".join(failures),
                                                   os.path.join(work, "testhost.log")))
        return 1
    print("all tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
