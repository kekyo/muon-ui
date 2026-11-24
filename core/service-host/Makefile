# muon - Multi-platform GUI application framework that uses CEF as its backend
# Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
# Under MIT.
# https://github.com/kekyo/muon-ui

COMMON := ../common/src
.DEFAULT_GOAL := all
YYJSON := ../cef/.deps/src/yyjson-0.12.0/src
OUT := .build
CPPFLAGS += -I../../sdk/include -I../common/include -I$(COMMON) -I$(YYJSON) -I. -I../cef/src
CPPFLAGS += -I../../node/native -I../cef/src/platform -DNOMINMAX
LLHTTP := ../cef/.deps/src/llhttp-9.4.3
LLHTTP_OBJECTS := $(addprefix $(OUT)/llhttp/,api.o http.o llhttp.o)
CPPFLAGS += -I$(LLHTTP)/include
$(OUT)/llhttp/%.o: $(LLHTTP)/src/%.c
	mkdir -p $(@D)
	$(CC) -std=c99 -I$(LLHTTP)/include -c $< -o $@

WSLAY := ../cef/.deps/src/wslay-1.1.1/lib
WSLAY_UNITS := wslay_event wslay_frame wslay_net wslay_queue wslay_stack
WSLAY_OBJECTS := $(addprefix $(OUT)/wslay/,$(addsuffix .o,$(WSLAY_UNITS)))
CPPFLAGS += -I$(WSLAY)/includes -I../../builder/src -DWSLAY_VERSION='"1.1.1"'
SERVER := $(COMMON)/services/muon_service_server.cpp $(COMMON)/muon_json_helpers.cpp $(wildcard $(COMMON)/server/*.cpp)
$(OUT)/wslay/%.o: $(WSLAY)/%.c
	mkdir -p $(@D)
	$(CC) -std=c99 $(CPPFLAGS) -DHAVE_ARPA_INET_H -DHAVE_NETINET_IN_H -c $< -o $@
$(OUT)/sha1.o: ../../builder/src/sha1.c | $(OUT)
	$(CC) -std=c99 -I../../builder/src -c $< -o $@

NODE_SOURCES := muon_node_service_host.cpp ../../node/native/node_process.cpp
SERVICE_API := ../../sdk/include/muon_service_io.h ../../sdk/include/muon_service_api.h ../../sdk/include/muon_service_lifecycle_types.h ../../sdk/include/muon_service_value.h
CXXFLAGS += -std=c++20 -Wall -Wextra -Wpedantic -Werror
CFLAGS += -std=c99 -Wall -Wextra -Wpedantic
LIFECYCLE := $(COMMON)/services/muon_service_lifecycle.cpp $(COMMON)/services/muon_service_controller.cpp
CARDIO_PC := $(abspath ../../sdk/native/linux-amd64/lib/pkgconfig)
CARDIO_CFLAGS = $(shell PKG_CONFIG_PATH=$(CARDIO_PC) pkg-config --cflags muon-plugin-cardio)
CARDIO_LIBS = $(shell PKG_CONFIG_PATH=$(CARDIO_PC) pkg-config --libs muon-plugin-cardio)
NATIVE := $(SERVER) $(COMMON)/services/muon_service_http.cpp muon_service_io_desktop.cpp $(COMMON)/services/muon_service_io.cpp $(COMMON)/services/muon_service_files.cpp $(COMMON)/services/muon_native_service_host.cpp $(COMMON)/services/muon_native_service_library.cpp $(COMMON)/services/muon_service_value.cpp $(COMMON)/services/muon_service_interface.cpp $(COMMON)/services/muon_service_broker.cpp $(COMMON)/muon_filesystem.cpp
QUICKJS_VERSION := $(shell node -p "require('../common/quickjs-source.json').version")
QUICKJS_SOURCE_DIR := ../common/.deps/quickjs/quickjs-$(QUICKJS_VERSION)
QUICKJS_SOURCES := $(COMMON)/quickjs/muon_quickjs_engine.cpp $(COMMON)/services/muon_quickjs_service_host.cpp $(COMMON)/services/muon_service_execution_router.cpp
QUICKJS_SOURCES += $(COMMON)/quickjs/muon_quickjs_module.cpp $(COMMON)/services/muon_service_plugin_pool.cpp $(COMMON)/services/muon_quickjs_plugin_bindings.cpp $(COMMON)/services/muon_quickjs_service_plugins.cpp
PLUGIN_SOURCES := muon_service_plugins.cpp $(COMMON)/plugins/muon_plugin_runtime.cpp $(COMMON)/plugins/muon_plugin_metadata.cpp $(COMMON)/plugins/muon_traffic_type_metadata.cpp $(COMMON)/plugins/muon_function_wrapper_lifecycle.cpp $(COMMON)/muon_sha256.cpp $(COMMON)/config/muon_glob.cpp $(COMMON)/plugins/muon_plugin_policy.cpp $(COMMON)/plugins/muon_type_metadata.cpp $(COMMON)/rpc/muon_rpc.cpp $(COMMON)/rpc/muon_rpc_host.cpp
CPPFLAGS += -I../../deps/tra-ffic/include -I../../deps/sha2
QUICKJS_OBJECTS := $(addprefix $(OUT)/quickjs/,quickjs.o dtoa.o libregexp.o libunicode.o cutils.o)
QUICKJS_GLIBC_HEADER := $(COMMON)/quickjs/muon_quickjs_glibc.h
CPPFLAGS += -isystem $(QUICKJS_SOURCE_DIR)
LOGIN := muon_service_login.cpp muon_service_startup.cpp ../cef/src/config/muon_autostart.cpp ../cef/src/config/muon_paths.cpp ../cef/src/config/muon_startup.cpp ../cef/src/muon_string_helpers.cpp

.PHONY: all test clean windows-amd64 windows-i686 quickjs-source

quickjs-source:
	node ../common/prepare-quickjs-source.mjs

$(OUT)/quickjs:
	mkdir -p $@

$(OUT)/quickjs/quickjs.o: $(QUICKJS_GLIBC_HEADER)
$(OUT)/quickjs/quickjs.o: private QUICKJS_CPPFLAGS := -include $(QUICKJS_GLIBC_HEADER)
$(QUICKJS_OBJECTS): $(OUT)/quickjs/%.o: ../common/quickjs-source.json ../common/prepare-quickjs-source.mjs Makefile | quickjs-source $(OUT)/quickjs
	$(CC) -std=gnu11 -O2 -fwrapv -D_GNU_SOURCE -DCONFIG_VERSION='"$(QUICKJS_VERSION)"' $(QUICKJS_CPPFLAGS) -c $(QUICKJS_SOURCE_DIR)/$*.c -o $@
all: $(OUT)/server-service.so $(OUT)/http-service.so $(OUT)/muon-service-host $(OUT)/probe-service.so $(OUT)/linux-client-driver $(OUT)/linux-process-driver $(OUT)/process-lock-driver

$(OUT):
	mkdir -p $@

$(OUT)/yyjson.o: $(YYJSON)/yyjson.c | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OUT)/protocol-test: muon_service_protocol_test.cpp muon_service_protocol.cpp $(LIFECYCLE) $(OUT)/yyjson.o | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(OUT)/libcardio.so: ../../sdk/native/linux-amd64/lib/libcardio.so | $(OUT)
	cp $< $@

$(OUT)/sha2.o: ../../deps/sha2/sha2.c | $(OUT)
	$(CC) -std=c99 -D_DEFAULT_SOURCE -I../../deps/sha2 -c $< -o $@

$(OUT)/muon-service-host: muon_service_http_linux.cpp $(WSLAY_OBJECTS) $(OUT)/sha1.o $(LLHTTP_OBJECTS) muon_service_host_linux.cpp muon_service_session_monitor.cpp muon_service_policy_linux.cpp muon_service_protocol.cpp $(LOGIN) $(LIFECYCLE) $(NATIVE) $(NODE_SOURCES) $(QUICKJS_SOURCES) $(PLUGIN_SOURCES) $(QUICKJS_OBJECTS) $(OUT)/yyjson.o $(OUT)/sha2.o $(OUT)/libcardio.so $(SERVICE_API) $(COMMON)/services/muon_service_reason.h | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CARDIO_CFLAGS) $(shell pkg-config --cflags libffi) $(filter-out %.h %.so,$^) $(CARDIO_LIBS) $(shell pkg-config --libs libffi) -ldl -lm -Wl,-rpath,'$$ORIGIN' -o $@

$(OUT)/http-service.so: ../../sdk/examples/services/native-http.c $(SERVICE_API) | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror -shared -fPIC $< -o $@

$(OUT)/server-service.so: ../../sdk/examples/services/native-server.c $(SERVICE_API) | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror -shared -fPIC $< -o $@

$(OUT)/probe-service.so: ../android-test/service-driver/src/main/cpp/muon_service_fixture.cpp $(SERVICE_API) $(OUT)/libcardio.so | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CARDIO_CFLAGS) -shared $< $(CARDIO_LIBS) -Wl,-rpath,'$$ORIGIN' -o $@

$(OUT)/client-test: ../common-test/muon_service_client_test.cpp $(COMMON)/services/muon_service_client.cpp $(OUT)/yyjson.o $(OUT)/libcardio.so | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CARDIO_CFLAGS) $(filter-out %.so,$^) $(CARDIO_LIBS) -Wl,-rpath,'$$ORIGIN' -o $@

$(OUT)/linux-client-driver: linux_client_driver.cpp muon_service_transport_linux.cpp $(COMMON)/services/muon_service_client.cpp $(OUT)/yyjson.o $(OUT)/libcardio.so | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CARDIO_CFLAGS) $(filter-out %.so,$^) $(CARDIO_LIBS) -Wl,-rpath,'$$ORIGIN' -o $@

$(OUT)/linux-process-driver: linux_process_driver.cpp muon_service_process_linux.cpp muon_service_process_lock_linux.cpp muon_service_transport_linux.cpp $(COMMON)/services/muon_service_session.cpp $(COMMON)/services/muon_service_client.cpp $(OUT)/yyjson.o $(OUT)/libcardio.so | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CARDIO_CFLAGS) $(filter-out %.so,$^) $(CARDIO_LIBS) -Wl,-rpath,'$$ORIGIN' -o $@

$(OUT)/process-lock-driver: process_lock_driver.cpp muon_service_process_lock_linux.cpp $(OUT)/libcardio.so | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CARDIO_CFLAGS) $(filter-out %.so,$^) $(CARDIO_LIBS) -Wl,-rpath,'$$ORIGIN' -o $@

$(OUT)/login-test: muon_service_login_test.cpp muon_service_login.cpp $(OUT)/libcardio.so | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CARDIO_CFLAGS) $(filter-out %.so,$^) $(CARDIO_LIBS) -Wl,-rpath,'$$ORIGIN' -o $@

$(OUT)/startup-test: muon_service_startup_test.cpp muon_service_policy_linux.cpp $(LOGIN) $(COMMON)/muon_filesystem.cpp $(OUT)/yyjson.o $(OUT)/libcardio.so | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CARDIO_CFLAGS) $(filter-out %.so,$^) $(CARDIO_LIBS) -Wl,-rpath,'$$ORIGIN' -o $@

$(OUT)/session-monitor-test: session_monitor_test.cpp muon_service_session_monitor.cpp muon_service_session_monitor.h | $(OUT)
	$(CXX) $(CXXFLAGS) $(filter %.cpp,$^) $(shell pkg-config --cflags --libs gio-2.0) -o $@

test: $(OUT)/session-monitor-test $(OUT)/protocol-test $(OUT)/client-test $(OUT)/login-test $(OUT)/startup-test
	dbus-run-session -- $(OUT)/session-monitor-test
	$(OUT)/protocol-test
	$(OUT)/client-test
	$(OUT)/login-test
	$(OUT)/startup-test

# Each architecture uses its SDK-provided shared cardio runtime. Keep compiler
# runtimes shared too: native factories and the host exchange C++ cardio state.
define WINDOWS_RULES
WIN_$(1)_QUICKJS := $(addprefix $(OUT)/windows-$(1)/quickjs/,quickjs.o dtoa.o libregexp.o libunicode.o cutils.o)
WIN_$(1)_PROTOCOL := $(addprefix $(OUT)/windows-$(1)/llhttp/,api.o http.o llhttp.o) $(addprefix $(OUT)/windows-$(1)/wslay/,$(addsuffix .o,$(WSLAY_UNITS))) $(OUT)/windows-$(1)/sha1.o
$(OUT)/windows-$(1)/llhttp/%.o: $(LLHTTP)/src/%.c
	mkdir -p $$(@D)
	$$(WIN_$(1)_CC) -std=c99 -I$(LLHTTP)/include -c $$< -o $$@
$(OUT)/windows-$(1)/wslay/%.o: $(WSLAY)/%.c
	mkdir -p $$(@D)
	$$(WIN_$(1)_CC) -std=c99 $(CPPFLAGS) -DHAVE_WINSOCK2_H -c $$< -o $$@
$(OUT)/windows-$(1)/sha1.o: ../../builder/src/sha1.c | $(OUT)/windows-$(1)
	$$(WIN_$(1)_CC) -std=c99 -I../../builder/src -c $$< -o $$@

WIN_$(1)_CC := $(2)-gcc
WIN_$(1)_CXX := $(2)-g++
WIN_$(1)_PC := $(abspath ../../sdk/native/windows-$(1)/lib/pkgconfig)
WIN_$(1)_FLAGS = $$(shell PKG_CONFIG_PATH=$$(WIN_$(1)_PC) pkg-config --cflags muon-plugin-cardio)
WIN_$(1)_LIBS = $$(shell PKG_CONFIG_PATH=$$(WIN_$(1)_PC) pkg-config --libs muon-plugin-cardio)
WIN_$(1)_FFI := ../cef/.deps/libffi-$(if $(filter i686,$(1)),mingw32,mingw64)

windows-$(1): $(OUT)/windows-$(1)/server-service.dll $(OUT)/windows-$(1)/http-service.dll $(OUT)/windows-$(1)/muon-service-host.exe $(OUT)/windows-$(1)/simple-service.dll $(OUT)/windows-$(1)/probe-service.dll $(OUT)/windows-$(1)/session-end.exe $(OUT)/windows-$(1)/transport-test.exe $(OUT)/windows-$(1)/process-driver.exe $(OUT)/windows-$(1)/process-lock-driver.exe

$(OUT)/windows-$(1):
	mkdir -p $$@

$(OUT)/windows-$(1)/quickjs:
	mkdir -p $$@

$$(WIN_$(1)_QUICKJS): $(OUT)/windows-$(1)/quickjs/%.o: ../common/quickjs-source.json ../common/prepare-quickjs-source.mjs Makefile | quickjs-source $(OUT)/windows-$(1)/quickjs
	$$(WIN_$(1)_CC) -std=gnu11 -O2 -fwrapv $(if $(filter i686,$(1)),-msse2 -mfpmath=sse) -D__USE_MINGW_ANSI_STDIO -D_GNU_SOURCE -DCONFIG_VERSION='"$(QUICKJS_VERSION)"' -c $(QUICKJS_SOURCE_DIR)/$$*.c -o $$@

$(OUT)/windows-$(1)/yyjson.o: $(YYJSON)/yyjson.c | $(OUT)/windows-$(1)
	# yyjson's i686 floating-point branch leaves its static power table unused.
	$$(WIN_$(1)_CC) $(CPPFLAGS) $(CFLAGS) -Wno-unused-const-variable -c $$< -o $$@

$(OUT)/windows-$(1)/windows-path.o: $(COMMON)/muon_windows_path.c | $(OUT)/windows-$(1)
	$$(WIN_$(1)_CC) $(CPPFLAGS) $(CFLAGS) -Werror -c $$< -o $$@

$(OUT)/windows-$(1)/runtimes: ../../sdk/native/windows-$(1)/lib/libcardio.dll | $(OUT)/windows-$(1)
	cp $$< $(OUT)/windows-$(1)/
	cp $$$$($$(WIN_$(1)_CXX) -print-file-name=libstdc++-6.dll) $(OUT)/windows-$(1)/
	cp $$$$($$(WIN_$(1)_CXX) -print-file-name=$(3)) $(OUT)/windows-$(1)/
	cp $$$$($$(WIN_$(1)_CXX) -print-file-name=libwinpthread-1.dll) $(OUT)/windows-$(1)/
	touch $$@

$$(WIN_$(1)_FFI)/lib/libffi.a:
	bash ../cef/build_libffi_mingw32.sh $(if $(filter i686,$(1)),mingw32,mingw64)

$(OUT)/windows-$(1)/sha2.o: ../../deps/sha2/sha2.c | $(OUT)/windows-$(1)
	$$(WIN_$(1)_CC) -std=c99 -D_DEFAULT_SOURCE -I../../deps/sha2 -c $$< -o $$@

$(OUT)/windows-$(1)/node-job.o: ../cef/src/platform/muon_windows_job_process.cpp ../cef/src/platform/muon_windows_job_process.h | $(OUT)/windows-$(1)
	# Match the CEF job helper's Windows API contract for atomic child ownership.
	$$(WIN_$(1)_CXX) $(CPPFLAGS) $(CXXFLAGS) -D_WIN32_WINNT=0x0A00 -c $$< -o $$@

$(OUT)/windows-$(1)/muon-service-host.exe: $$(WIN_$(1)_PROTOCOL) $(OUT)/windows-$(1)/node-job.o muon_service_http_windows.cpp muon_service_host_windows.cpp muon_service_policy_windows.cpp muon_service_protocol.cpp $(LOGIN) $(LIFECYCLE) $(NATIVE) $(NODE_SOURCES) $(QUICKJS_SOURCES) $(PLUGIN_SOURCES) $$(WIN_$(1)_QUICKJS) $(OUT)/windows-$(1)/yyjson.o $(OUT)/windows-$(1)/sha2.o $(OUT)/windows-$(1)/windows-path.o $(OUT)/windows-$(1)/runtimes $$(WIN_$(1)_FFI)/lib/libffi.a $(SERVICE_API) $(COMMON)/services/muon_service_reason.h
	$$(WIN_$(1)_CXX) $(CPPFLAGS) $(CXXFLAGS) $$(WIN_$(1)_FLAGS) -I$$(WIN_$(1)_FFI)/include -D_WIN32_WINNT=0x0601 -municode $$(filter-out %.h %/runtimes,$$^) $$(WIN_$(1)_LIBS) -lwinpthread -ladvapi32 -luser32 -lole32 -lws2_32 -lwinhttp -lbcrypt -o $$@

$(OUT)/windows-$(1)/simple-service.dll: ../common-test/muon_service_fixture.c $(SERVICE_API) | $(OUT)/windows-$(1)
	$$(WIN_$(1)_CC) $(CPPFLAGS) $(CFLAGS) -Werror -shared $$< -o $$@

$(OUT)/windows-$(1)/http-service.dll: ../../sdk/examples/services/native-http.c $(SERVICE_API) | $(OUT)/windows-$(1)
	$$(WIN_$(1)_CC) $(CPPFLAGS) $(CFLAGS) -Werror -shared $$< -o $$@

$(OUT)/windows-$(1)/server-service.dll: ../../sdk/examples/services/native-server.c $(SERVICE_API) | $(OUT)/windows-$(1)
	$$(WIN_$(1)_CC) $(CPPFLAGS) $(CFLAGS) -Werror -shared $$< -o $$@

$(OUT)/windows-$(1)/probe-service.dll: ../android-test/service-driver/src/main/cpp/muon_service_fixture.cpp $(SERVICE_API) $(OUT)/windows-$(1)/runtimes | $(OUT)/windows-$(1)
	$$(WIN_$(1)_CXX) $(CPPFLAGS) $(CXXFLAGS) $$(WIN_$(1)_FLAGS) -shared $$< $$(WIN_$(1)_LIBS) -lws2_32 -o $$@

$(OUT)/windows-$(1)/session-end.exe: windows_session_test.cpp | $(OUT)/windows-$(1)
	$$(WIN_$(1)_CXX) $(CXXFLAGS) -municode $$< -luser32 -ladvapi32 -o $$@

$(OUT)/windows-$(1)/transport-test.exe: windows_transport_test.cpp muon_service_transport_windows.cpp muon_service_process_lock_windows.cpp $(COMMON)/services/muon_service_client.cpp $(COMMON)/muon_filesystem.cpp $(OUT)/windows-$(1)/yyjson.o $(OUT)/windows-$(1)/windows-path.o $(OUT)/windows-$(1)/runtimes
	$$(WIN_$(1)_CXX) $(CPPFLAGS) $(CXXFLAGS) $$(WIN_$(1)_FLAGS) -D_WIN32_WINNT=0x0601 -municode $$(filter-out %/runtimes,$$^) $$(WIN_$(1)_LIBS) -ladvapi32 -luser32 -lshell32 -lole32 -luuid -o $$@

$(OUT)/windows-$(1)/process-driver.exe: windows_process_driver.cpp muon_service_process_windows.cpp muon_service_transport_windows.cpp muon_service_process_lock_windows.cpp $(COMMON)/services/muon_service_session.cpp $(COMMON)/services/muon_service_client.cpp $(COMMON)/muon_filesystem.cpp $(OUT)/windows-$(1)/yyjson.o $(OUT)/windows-$(1)/windows-path.o $(OUT)/windows-$(1)/runtimes
	$$(WIN_$(1)_CXX) $(CPPFLAGS) $(CXXFLAGS) $$(WIN_$(1)_FLAGS) -D_WIN32_WINNT=0x0601 -municode $$(filter-out %/runtimes,$$^) $$(WIN_$(1)_LIBS) -ladvapi32 -luser32 -lshell32 -lole32 -luuid -o $$@

$(OUT)/windows-$(1)/process-lock-driver.exe: process_lock_driver.cpp muon_service_process_lock_windows.cpp $(OUT)/windows-$(1)/runtimes
	$$(WIN_$(1)_CXX) $(CPPFLAGS) $(CXXFLAGS) $$(WIN_$(1)_FLAGS) -D_WIN32_WINNT=0x0601 $$(filter-out %/runtimes,$$^) $$(WIN_$(1)_LIBS) -ladvapi32 -lshell32 -lole32 -luuid -o $$@

endef

$(eval $(call WINDOWS_RULES,amd64,x86_64-w64-mingw32,libgcc_s_seh-1.dll))
$(eval $(call WINDOWS_RULES,i686,i686-w64-mingw32,libgcc_s_dw2-1.dll))

clean:
	rm -rf $(OUT)
