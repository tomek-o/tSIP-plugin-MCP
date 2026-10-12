# tSIP MCP server plugin (plugin interface v2) - command line build
#
# Usage (MinGW, e.g. Code::Blocks 25.03 bundled GCC 14.2 32-bit):
#   mingw32-make              release build -> bin/Release/MCP.dll
#   mingw32-make CONFIG=debug debug build   -> bin/Debug/MCP.dll
#   mingw32-make clean
# or build.bat (sets MinGW path, runs make, copies DLL to tSIP).
#
# Keep source list and flags in sync with MCP.cbp (Code::Blocks project).

# recipes use cmd.exe syntax regardless of sh.exe being in PATH
SHELL = cmd.exe

CONFIG ?= release

CXX = g++

SOURCES = \
	main.cpp \
	Activity.cpp \
	CallWatch.cpp \
	SettingsDialog.cpp \
	Prompts.cpp \
	Config.cpp \
	HttpServer.cpp \
	McpServer.cpp \
	Tools.cpp \
	jsoncpp/src/lib_json/json_reader.cpp \
	jsoncpp/src/lib_json/json_value.cpp \
	jsoncpp/src/lib_json/json_writer.cpp

# plugin interface header: single copy in tSIP source tree (sibling directory)
TSIP_PHONE_DIR = ../tSIP/tSIP/phone

CXXFLAGS = -std=c++17 -Wall -Wextra -Ijsoncpp/include -I$(TSIP_PHONE_DIR) -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0601
LDFLAGS = -shared -static -Wl,--add-stdcall-alias
LIBS = -lws2_32 -lbcrypt -luser32

ifeq ($(CONFIG),debug)
	OUTDIR = bin/Debug
	OBJDIR = obj/Debug
	CXXFLAGS += -g
else
	OUTDIR = bin/Release
	OBJDIR = obj/Release
	CXXFLAGS += -Os
	LDFLAGS += -s
endif

TARGET = $(OUTDIR)/MCP.dll
OBJECTS = $(addprefix $(OBJDIR)/,$(SOURCES:.cpp=.o))

# create directory (cmd.exe syntax, backslashes)
mkdir_p = if not exist "$(subst /,\,$(1))" mkdir "$(subst /,\,$(1))"

.PHONY: all clean testhost

all: $(TARGET)

$(TARGET): $(OBJECTS)
	@$(call mkdir_p,$(OUTDIR))
	$(CXX) $(LDFLAGS) -o $@ $(OBJECTS) $(LIBS)

# bundled third-party jsoncpp: do not report its (harmless) unused parameter warnings
$(OBJDIR)/jsoncpp/%.o: CXXFLAGS += -Wno-unused-parameter

$(OBJDIR)/%.o: %.cpp
	@$(call mkdir_p,$(dir $@))
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# test host: loads MCP.dll like tSIP, with fake Invoke() results (see test/TestHost.cpp)
testhost: $(OUTDIR)/TestHost.exe

$(OUTDIR)/TestHost.exe: test/TestHost.cpp $(TSIP_PHONE_DIR)/tSIP_plugin_v2.h
	@$(call mkdir_p,$(OUTDIR))
	$(CXX) -std=c++17 -Wall -Wextra -static -I$(TSIP_PHONE_DIR) -o $@ test/TestHost.cpp

clean:
	-@if exist obj rmdir /s /q obj
	-@if exist bin rmdir /s /q bin

-include $(OBJECTS:.o=.d)
