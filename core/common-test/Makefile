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
SERVICE_VALUES := $(COMMON)/src/services/muon_service_http.cpp $(COMMON)/src/services/muon_service_io.cpp $(COMMON)/src/services/muon_service_files.cpp $(COMMON)/src/services/muon_service_value.cpp $(COMMON)/src/services/muon_service_interface.cpp
SERVICE_HEADERS := $(COMMON)/src/services/muon_service_files.h $(COMMON)/src/services/muon_service_io.h ../../sdk/include/muon_service_io.h $(COMMON)/src/services/muon_service_reason.h ../../sdk/include/muon_service_lifecycle_types.h $(COMMON)/src/services/muon_service_execution.h $(COMMON)/src/services/muon_native_service_host.h $(COMMON)/src/services/muon_service_interface.h $(COMMON)/src/services/muon_service_value.h ../../sdk/include/muon_service_api.h ../../sdk/include/muon_service_value.h
CPPFLAGS += -I$(YYJSON_SOURCE_DIR)
TESTS := $(OUT)/service-files $(OUT)/rpc $(OUT)/rpc-portable $(OUT)/function-lifetime $(OUT)/plugin-api $(OUT)/service-lifecycle $(OUT)/native-service-host $(OUT)/native-service-calls $(OUT)/native-service-library $(OUT)/service-controller $(OUT)/service-value $(OUT)/service-interface $(OUT)/service-broker
QUICKJS_VERSION := $(shell node -p "require('../common/quickjs-source.json').version")
QUICKJS_SOURCE_DIR := $(COMMON)/.deps/quickjs/quickjs-$(QUICKJS_VERSION)
QUICKJS_OBJECTS := $(addprefix $(OUT)/quickjs/,quickjs.o dtoa.o libregexp.o libunicode.o cutils.o)
QUICKJS_GLIBC_HEADER := $(COMMON)/src/quickjs/muon_quickjs_glibc.h
TESTS += $(OUT)/quickjs-engine $(OUT)/quickjs-service $(OUT)/quickjs-service-sdk
TESTS += $(OUT)/service-execution-router
TESTS += $(OUT)/service-plugin-pool
PLUGIN_RUNTIME_SOURCES := $(COMMON)/src/plugins/muon_plugin_runtime.cpp \
  $(COMMON)/src/plugins/muon_plugin_metadata.cpp $(COMMON)/src/plugins/muon_traffic_type_metadata.cpp \
  $(COMMON)/src/plugins/muon_function_wrapper_lifecycle.cpp \
  $(COMMON)/src/muon_filesystem.cpp $(COMMON)/src/muon_sha256.cpp
# This portable driver runs before SDK staging. cardio's documented header-only
# profile avoids depending on generated native packages; production uses its
# existing process-shared cardio target.
PLUGIN_DRIVER_FLAGS := -I../../deps/cardio/include -I../../deps/tra-ffic/include \
  -I../../deps/sha2 -DCARDIO_WITH_LINUX_IO_URING=0 $(shell pkg-config --cflags libffi)

$(OUT)/sha2.o: ../../deps/sha2/sha2.c | $(OUT)
	$(CC) -std=c99 -D_DEFAULT_SOURCE -I../../deps/sha2 -c $< -o $@

$(OUT)/service-native-plugin-fixture.so: muon_service_native_plugin_fixture.c ../../sdk/include/muon_plugin_api.h | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -shared $< -o $@

$(OUT)/service-plugin-pool: muon_service_plugin_pool_test.cpp $(COMMON)/src/services/muon_service_plugin_pool.cpp $(COMMON)/src/services/muon_quickjs_plugin_bindings.cpp $(COMMON)/src/services/muon_quickjs_service_plugins.cpp $(COMMON)/src/services/muon_quickjs_service_host.cpp $(COMMON)/src/quickjs/muon_quickjs_engine.cpp $(COMMON)/src/quickjs/muon_quickjs_module.cpp $(PLUGIN_RUNTIME_SOURCES) $(RPC_SOURCES) $(SERVICE_VALUES) $(OUT)/sha2.o $(OUT)/yyjson.o $(QUICKJS_OBJECTS) $(OUT)/service-native-plugin-fixture.so | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(PLUGIN_DRIVER_FLAGS) -DMUON_PLUGIN_FIXTURE_PATH='"$(abspath $(OUT)/service-native-plugin-fixture.so)"' -isystem $(QUICKJS_SOURCE_DIR) $(filter %.cpp %.o,$^) $(shell pkg-config --libs libffi) -ldl -o $@

.PHONY: all test clean service-driver quickjs-source
all: $(TESTS)

$(OUT)/service-execution-router: muon_service_execution_router_test.cpp $(COMMON)/src/services/muon_service_execution_router.cpp $(COMMON)/src/services/muon_service_execution_router.h | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(filter %.cpp,$^) -o $@

$(OUT):
	mkdir -p $@

quickjs-source:
	node $(COMMON)/prepare-quickjs-source.mjs

$(OUT)/quickjs:
	mkdir -p $@

# The verified dependency is immutable; its profile and preparation script are
# the build inputs. Do not modify the downloaded third-party source directory.
$(OUT)/quickjs/quickjs.o: $(QUICKJS_GLIBC_HEADER)
$(OUT)/quickjs/quickjs.o: private QUICKJS_CPPFLAGS := -include $(QUICKJS_GLIBC_HEADER)
$(QUICKJS_OBJECTS): $(OUT)/quickjs/%.o: $(COMMON)/quickjs-source.json $(COMMON)/prepare-quickjs-source.mjs Makefile | quickjs-source $(OUT)/quickjs
	$(CC) -std=gnu11 -O2 -fwrapv -D_GNU_SOURCE -DCONFIG_VERSION='"$(QUICKJS_VERSION)"' $(QUICKJS_CPPFLAGS) -c $(QUICKJS_SOURCE_DIR)/$*.c -o $@

$(OUT)/quickjs-engine: muon_quickjs_engine_test.cpp $(COMMON)/src/quickjs/muon_quickjs_engine.cpp $(COMMON)/src/quickjs/muon_quickjs_engine.h $(COMMON)/src/quickjs/muon_quickjs_module.cpp $(COMMON)/src/quickjs/muon_quickjs_module.h $(QUICKJS_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -isystem $(QUICKJS_SOURCE_DIR) $(filter %.cpp %.o,$^) -lm -o $@

$(OUT)/quickjs-service: muon_quickjs_service_host_test.cpp $(COMMON)/src/services/muon_quickjs_service_host.cpp $(COMMON)/src/services/muon_quickjs_service_host.h $(COMMON)/src/quickjs/muon_quickjs_engine.cpp $(COMMON)/src/quickjs/muon_quickjs_engine.h $(SERVICE_VALUES) $(SERVICE_HEADERS) $(OUT)/yyjson.o $(QUICKJS_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -isystem $(QUICKJS_SOURCE_DIR) $(filter %.cpp %.o,$^) -lm -o $@

$(OUT)/quickjs-service-fixture.h: ../../sdk/scripts/build-service-runtime-fixture.mjs $(wildcard ../../sdk/src/service*.ts) ../../sdk/test/fixtures/quickjs-service.ts | $(OUT)
	node ../../sdk/scripts/build-service-runtime-fixture.mjs $(OUT)/quickjs-service-fixture.js $@

$(OUT)/quickjs-service-sdk: muon_quickjs_service_sdk_test.cpp $(OUT)/quickjs-service-fixture.h $(COMMON)/src/services/muon_quickjs_service_host.cpp $(COMMON)/src/services/muon_quickjs_service_host.h $(COMMON)/src/quickjs/muon_quickjs_engine.cpp $(COMMON)/src/quickjs/muon_quickjs_engine.h $(SERVICE_VALUES) $(SERVICE_HEADERS) $(OUT)/yyjson.o $(QUICKJS_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -I$(OUT) -isystem $(QUICKJS_SOURCE_DIR) $(filter %.cpp %.o,$^) -lm -o $@

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

$(OUT)/service-fixture.so: muon_service_fixture.c ../../sdk/include/muon_service_api.h ../../sdk/include/muon_service_lifecycle_types.h | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -shared $< -o $@

$(OUT)/native-service-library: muon_native_service_library_test.cpp $(COMMON)/src/services/muon_native_service_host.cpp $(COMMON)/src/services/muon_native_service_library.cpp $(COMMON)/src/services/muon_service_controller.cpp $(COMMON)/src/services/muon_service_lifecycle.cpp $(COMMON)/src/muon_filesystem.cpp $(SERVICE_VALUES) $(SERVICE_HEADERS) $(OUT)/yyjson.o $(OUT)/service-fixture.so | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -DMUON_SERVICE_FIXTURE_PATH='"$(abspath $(OUT)/service-fixture.so)"' $(filter %.cpp %.o,$^) -ldl -o $@

$(OUT)/service-driver: muon_service_driver.cpp $(COMMON)/src/services/muon_service_lifecycle.cpp | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

test: all
	@set -e; for executable in $(TESTS); do "$$executable"; done

clean:
	rm -rf $(OUT)

$(OUT)/service-files: muon_service_files_test.cpp $(SERVICE_VALUES) $(SERVICE_HEADERS) $(OUT)/yyjson.o | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(filter %.cpp %.o,$^) -o $@
