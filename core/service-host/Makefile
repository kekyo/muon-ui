# muon - Multi-platform GUI application framework that uses CEF as its backend
# Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
# Under MIT.
# https://github.com/kekyo/muon-ui

COMMON := ../common/src
YYJSON := ../cef/.deps/src/yyjson-0.12.0/src
OUT := .build
CPPFLAGS += -I../../sdk/include -I../common/include -I$(COMMON) -I$(YYJSON) -I. -I../cef/src
CXXFLAGS += -std=c++20 -Wall -Wextra -Wpedantic -Werror
CFLAGS += -std=c99 -Wall -Wextra -Wpedantic
LIFECYCLE := $(COMMON)/services/muon_service_lifecycle.cpp $(COMMON)/services/muon_service_controller.cpp
CARDIO_PC := $(abspath ../../sdk/native/linux-amd64/lib/pkgconfig)
CARDIO_CFLAGS := $(shell PKG_CONFIG_PATH=$(CARDIO_PC) pkg-config --cflags muon-plugin-cardio)
CARDIO_LIBS := $(shell PKG_CONFIG_PATH=$(CARDIO_PC) pkg-config --libs muon-plugin-cardio)
NATIVE := $(COMMON)/services/muon_native_service_host.cpp $(COMMON)/services/muon_native_service_library.cpp $(COMMON)/muon_filesystem.cpp
LOGIN := muon_service_login.cpp muon_service_startup.cpp ../cef/src/config/muon_autostart.cpp ../cef/src/config/muon_paths.cpp ../cef/src/config/muon_startup.cpp ../cef/src/muon_string_helpers.cpp

.PHONY: all test clean windows-amd64 windows-i686
all: $(OUT)/muon-service-host $(OUT)/probe-service.so $(OUT)/linux-client-driver $(OUT)/linux-process-driver $(OUT)/process-lock-driver

$(OUT):
	mkdir -p $@

$(OUT)/yyjson.o: $(YYJSON)/yyjson.c | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OUT)/protocol-test: muon_service_protocol_test.cpp muon_service_protocol.cpp $(LIFECYCLE) $(OUT)/yyjson.o | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(OUT)/libcardio.so: ../../sdk/native/linux-amd64/lib/libcardio.so | $(OUT)
	cp $< $@

$(OUT)/muon-service-host: muon_service_host_linux.cpp muon_service_policy_linux.cpp muon_service_protocol.cpp $(LOGIN) $(LIFECYCLE) $(NATIVE) $(OUT)/yyjson.o $(OUT)/libcardio.so | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CARDIO_CFLAGS) $(filter-out %.so,$^) $(CARDIO_LIBS) -ldl -Wl,-rpath,'$$ORIGIN' -o $@

$(OUT)/probe-service.so: ../android-test/service-driver/src/main/cpp/muon_service_fixture.cpp $(OUT)/libcardio.so | $(OUT)
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

test: $(OUT)/protocol-test $(OUT)/client-test $(OUT)/login-test
	$(OUT)/protocol-test
	$(OUT)/client-test
	$(OUT)/login-test

# Each architecture uses its SDK-provided shared cardio runtime. Keep compiler
# runtimes shared too: native factories and the host exchange C++ cardio state.
define WINDOWS_RULES
WIN_$(1)_CC := $(2)-gcc
WIN_$(1)_CXX := $(2)-g++
WIN_$(1)_PC := $(abspath ../../sdk/native/windows-$(1)/lib/pkgconfig)
WIN_$(1)_FLAGS := $$(shell PKG_CONFIG_PATH=$$(WIN_$(1)_PC) pkg-config --cflags muon-plugin-cardio)
WIN_$(1)_LIBS := $$(shell PKG_CONFIG_PATH=$$(WIN_$(1)_PC) pkg-config --libs muon-plugin-cardio)

windows-$(1): $(OUT)/windows-$(1)/muon-service-host.exe $(OUT)/windows-$(1)/simple-service.dll $(OUT)/windows-$(1)/probe-service.dll $(OUT)/windows-$(1)/session-end.exe $(OUT)/windows-$(1)/transport-test.exe $(OUT)/windows-$(1)/process-driver.exe $(OUT)/windows-$(1)/process-lock-driver.exe

$(OUT)/windows-$(1):
	mkdir -p $$@

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

$(OUT)/windows-$(1)/muon-service-host.exe: muon_service_host_windows.cpp muon_service_policy_windows.cpp muon_service_protocol.cpp $(LOGIN) $(LIFECYCLE) $(NATIVE) $(OUT)/windows-$(1)/yyjson.o $(OUT)/windows-$(1)/windows-path.o $(OUT)/windows-$(1)/runtimes
	$$(WIN_$(1)_CXX) $(CPPFLAGS) $(CXXFLAGS) $$(WIN_$(1)_FLAGS) -D_WIN32_WINNT=0x0601 -municode $$(filter-out %/runtimes,$$^) $$(WIN_$(1)_LIBS) -ladvapi32 -luser32 -lole32 -o $$@

$(OUT)/windows-$(1)/simple-service.dll: ../common-test/muon_service_fixture.c | $(OUT)/windows-$(1)
	$$(WIN_$(1)_CC) $(CPPFLAGS) $(CFLAGS) -Werror -shared $$< -o $$@

$(OUT)/windows-$(1)/probe-service.dll: ../android-test/service-driver/src/main/cpp/muon_service_fixture.cpp $(OUT)/windows-$(1)/runtimes | $(OUT)/windows-$(1)
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
