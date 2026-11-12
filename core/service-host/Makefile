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
CARDIO_PC := $(abspath ../../sdk/native/linux-amd64/lib/pkgconfig)
CARDIO_CFLAGS := $(shell PKG_CONFIG_PATH=$(CARDIO_PC) pkg-config --cflags muon-plugin-cardio)
CARDIO_LIBS := $(shell PKG_CONFIG_PATH=$(CARDIO_PC) pkg-config --libs muon-plugin-cardio)
NATIVE := $(COMMON)/services/muon_native_service_host.cpp $(COMMON)/services/muon_native_service_library.cpp $(COMMON)/muon_filesystem.cpp

.PHONY: all test clean
all: $(OUT)/muon-service-host $(OUT)/probe-service.so

$(OUT):
	mkdir -p $@

$(OUT)/yyjson.o: $(YYJSON)/yyjson.c | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OUT)/protocol-test: muon_service_protocol_test.cpp muon_service_protocol.cpp $(LIFECYCLE) $(OUT)/yyjson.o | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(OUT)/libcardio.so: ../../sdk/native/linux-amd64/lib/libcardio.so | $(OUT)
	cp $< $@

$(OUT)/muon-service-host: muon_service_host_linux.cpp muon_service_protocol.cpp $(LIFECYCLE) $(NATIVE) $(OUT)/yyjson.o $(OUT)/libcardio.so | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CARDIO_CFLAGS) $(filter-out %.so,$^) $(CARDIO_LIBS) -ldl -Wl,-rpath,'$$ORIGIN' -o $@

$(OUT)/probe-service.so: ../android-test/service-driver/src/main/cpp/muon_service_fixture.cpp $(OUT)/libcardio.so | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CARDIO_CFLAGS) -shared $< $(CARDIO_LIBS) -Wl,-rpath,'$$ORIGIN' -o $@

test: $(OUT)/protocol-test
	$(OUT)/protocol-test

clean:
	rm -rf $(OUT)
