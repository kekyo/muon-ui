# muon - Multi-platform GUI application framework that uses CEF as its backend
# Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
# Under MIT.
# https://github.com/kekyo/muon-ui

COMMON := ../common
OUT := .build/portable
CPPFLAGS += -I../../sdk/include -I$(COMMON)/src
CXXFLAGS += -std=c++20 -Wall -Wextra -Wpedantic -Werror -pthread
CFLAGS += -std=c99 -Wall -Wextra -Wpedantic -Werror
RPC_SOURCES := $(COMMON)/src/config/muon_glob.cpp \
  $(COMMON)/src/plugins/muon_plugin_policy.cpp \
  $(COMMON)/src/plugins/muon_type_metadata.cpp \
  $(COMMON)/src/rpc/muon_rpc.cpp \
  $(COMMON)/src/rpc/muon_rpc_host.cpp
YYJSON_SOURCE_DIR := ../cef/.deps/src/yyjson-0.12.0/src
SERVICE_VALUES := $(COMMON)/src/services/muon_service_value.cpp $(COMMON)/src/services/muon_service_interface.cpp
SERVICE_HEADERS := $(COMMON)/src/services/muon_native_service_host.h $(COMMON)/src/services/muon_service_interface.h $(COMMON)/src/services/muon_service_value.h ../../sdk/include/muon_service_api.h ../../sdk/include/muon_service_value.h
CPPFLAGS += -I$(YYJSON_SOURCE_DIR)
TESTS := $(OUT)/rpc $(OUT)/rpc-portable $(OUT)/function-lifetime $(OUT)/plugin-api $(OUT)/service-lifecycle $(OUT)/native-service-host $(OUT)/native-service-calls $(OUT)/native-service-library $(OUT)/service-controller $(OUT)/service-value $(OUT)/service-interface $(OUT)/service-broker
QUICKJS_VERSION := $(shell node -p "require('../common/quickjs-source.json').version")
QUICKJS_SOURCE_DIR := $(COMMON)/.deps/quickjs/quickjs-$(QUICKJS_VERSION)
QUICKJS_OBJECTS := $(addprefix $(OUT)/quickjs/,quickjs.o dtoa.o libregexp.o libunicode.o cutils.o)
TESTS += $(OUT)/quickjs-engine

.PHONY: all test clean service-driver quickjs-source
all: $(TESTS)

$(OUT):
	mkdir -p $@

quickjs-source:
	node $(COMMON)/prepare-quickjs-source.mjs

$(OUT)/quickjs:
	mkdir -p $@

# The verified dependency is immutable; its profile and preparation script are
# the build inputs. Do not modify the downloaded third-party source directory.
$(QUICKJS_OBJECTS): $(OUT)/quickjs/%.o: $(COMMON)/quickjs-source.json $(COMMON)/prepare-quickjs-source.mjs Makefile | quickjs-source $(OUT)/quickjs
	$(CC) -std=gnu11 -O2 -fwrapv -D_GNU_SOURCE -DCONFIG_VERSION='"$(QUICKJS_VERSION)"' -c $(QUICKJS_SOURCE_DIR)/$*.c -o $@

$(OUT)/quickjs-engine: muon_quickjs_engine_test.cpp $(COMMON)/src/quickjs/muon_quickjs_engine.cpp $(COMMON)/src/quickjs/muon_quickjs_engine.h $(COMMON)/src/quickjs/muon_quickjs_module.cpp $(COMMON)/src/quickjs/muon_quickjs_module.h $(QUICKJS_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -isystem $(QUICKJS_SOURCE_DIR) $(filter %.cpp %.o,$^) -lm -o $@

$(OUT)/rpc: muon_rpc_test.cpp $(RPC_SOURCES) $(COMMON)/src/plugins/muon_plugin_metadata.cpp | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(OUT)/rpc-portable: muon_rpc_portable_test.cpp $(RPC_SOURCES) | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(OUT)/function-lifetime: muon_function_wrapper_lifecycle_test.cpp $(COMMON)/src/plugins/muon_function_wrapper_lifecycle.cpp | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(OUT)/plugin-api: muon_plugin_api_header_test.c | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

$(OUT)/service-lifecycle: muon_service_lifecycle_test.cpp $(COMMON)/src/services/muon_service_lifecycle.cpp | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(OUT)/service-controller: muon_service_controller_test.cpp $(COMMON)/src/services/muon_service_controller.cpp $(COMMON)/src/services/muon_service_lifecycle.cpp | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(OUT)/service-value: muon_service_value_test.cpp $(COMMON)/src/services/muon_service_value.cpp $(COMMON)/src/services/muon_service_value.h ../../sdk/include/muon_service_value.h | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(filter %.cpp,$^) -o $@

$(YYJSON_SOURCE_DIR)/yyjson.c $(YYJSON_SOURCE_DIR)/yyjson.h:
	bash ../cef/build_yyjson.sh

$(OUT)/yyjson.o: $(YYJSON_SOURCE_DIR)/yyjson.c $(YYJSON_SOURCE_DIR)/yyjson.h | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/service-interface: muon_service_interface_test.cpp $(COMMON)/src/services/muon_service_interface.cpp $(COMMON)/src/services/muon_service_interface.h $(COMMON)/src/services/muon_service_value.h ../../sdk/include/muon_service_value.h $(OUT)/yyjson.o | $(OUT)
	$(CXX) $(CPPFLAGS) -I$(YYJSON_SOURCE_DIR) $(CXXFLAGS) $(filter %.cpp %.o,$^) -o $@

service-driver: $(OUT)/service-driver

$(OUT)/native-service-host: muon_native_service_host_test.cpp $(COMMON)/src/services/muon_native_service_host.cpp $(SERVICE_VALUES) $(SERVICE_HEADERS) $(OUT)/yyjson.o | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(filter %.cpp %.o,$^) -o $@

$(OUT)/native-service-calls: muon_native_service_calls_test.cpp $(COMMON)/src/services/muon_native_service_host.cpp $(SERVICE_VALUES) $(SERVICE_HEADERS) $(OUT)/yyjson.o | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(filter %.cpp %.o,$^) -o $@

$(OUT)/service-broker: muon_service_broker_test.cpp $(COMMON)/src/services/muon_service_broker.cpp $(COMMON)/src/services/muon_service_broker.h $(COMMON)/src/services/muon_native_service_host.cpp $(COMMON)/src/services/muon_service_controller.cpp $(COMMON)/src/services/muon_service_lifecycle.cpp $(SERVICE_VALUES) $(SERVICE_HEADERS) $(OUT)/yyjson.o | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(filter %.cpp %.o,$^) -o $@

$(OUT)/service-fixture.so: muon_service_fixture.c ../../sdk/include/muon_service_api.h | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -shared $< -o $@

$(OUT)/native-service-library: muon_native_service_library_test.cpp $(COMMON)/src/services/muon_native_service_host.cpp $(COMMON)/src/services/muon_native_service_library.cpp $(COMMON)/src/services/muon_service_controller.cpp $(COMMON)/src/services/muon_service_lifecycle.cpp $(COMMON)/src/muon_filesystem.cpp $(SERVICE_VALUES) $(SERVICE_HEADERS) $(OUT)/yyjson.o $(OUT)/service-fixture.so | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -DMUON_SERVICE_FIXTURE_PATH='"$(abspath $(OUT)/service-fixture.so)"' $(filter %.cpp %.o,$^) -ldl -o $@

$(OUT)/service-driver: muon_service_driver.cpp $(COMMON)/src/services/muon_service_lifecycle.cpp | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

test: all
	@set -e; for executable in $(TESTS); do "$$executable"; done

clean:
	rm -rf $(OUT)
