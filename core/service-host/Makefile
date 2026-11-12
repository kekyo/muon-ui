# muon - Multi-platform GUI application framework that uses CEF as its backend
# Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
# Under MIT.
# https://github.com/kekyo/muon-ui

COMMON := ../common/src
YYJSON := ../cef/.deps/src/yyjson-0.12.0/src
OUT := .build
CPPFLAGS += -I../../sdk/include -I$(COMMON) -I$(YYJSON) -I.
CXXFLAGS += -std=c++20 -Wall -Wextra -Wpedantic -Werror
CFLAGS += -std=c99 -Wall -Wextra -Wpedantic
LIFECYCLE := $(COMMON)/services/muon_service_lifecycle.cpp $(COMMON)/services/muon_service_controller.cpp

.PHONY: test clean
$(OUT):
	mkdir -p $@

$(OUT)/yyjson.o: $(YYJSON)/yyjson.c | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OUT)/protocol-test: muon_service_protocol_test.cpp muon_service_protocol.cpp $(LIFECYCLE) $(OUT)/yyjson.o | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

test: $(OUT)/protocol-test
	$(OUT)/protocol-test

clean:
	rm -rf $(OUT)
