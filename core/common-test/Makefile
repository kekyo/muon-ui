# muon - Multi-platform GUI application framework that uses CEF as its backend
# Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
# Under MIT.
# https://github.com/kekyo/muon-ui

COMMON := ../common
OUT := .build/portable
CPPFLAGS += -I$(COMMON)/include -I$(COMMON)/src
CXXFLAGS += -std=c++20 -Wall -Wextra -Wpedantic -Werror -pthread
CFLAGS += -std=c99 -Wall -Wextra -Wpedantic -Werror
RPC_SOURCES := $(COMMON)/src/config/muon_glob.cpp \
  $(COMMON)/src/plugins/muon_plugin_policy.cpp \
  $(COMMON)/src/plugins/muon_type_metadata.cpp \
  $(COMMON)/src/rpc/muon_rpc.cpp \
  $(COMMON)/src/rpc/muon_rpc_host.cpp
TESTS := $(OUT)/rpc $(OUT)/rpc-portable $(OUT)/function-lifetime $(OUT)/plugin-api

.PHONY: all test clean
all: $(TESTS)

$(OUT):
	mkdir -p $@

$(OUT)/rpc: muon_rpc_test.cpp $(RPC_SOURCES) $(COMMON)/src/plugins/muon_plugin_metadata.cpp | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(OUT)/rpc-portable: muon_rpc_portable_test.cpp $(RPC_SOURCES) | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(OUT)/function-lifetime: muon_function_wrapper_lifecycle_test.cpp $(COMMON)/src/plugins/muon_function_wrapper_lifecycle.cpp | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(OUT)/plugin-api: muon_plugin_api_header_test.c | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

test: all
	@set -e; for executable in $(TESTS); do "$$executable"; done

clean:
	rm -rf $(OUT)
