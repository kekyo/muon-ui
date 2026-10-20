OUT_DIR ?= ./dist
.DEFAULT_GOAL := all
PREPARE_TARGET ?= $(OUT_DIR)/muon-builder
LAUNCHER_TARGET ?= $(OUT_DIR)/muon-launcher
RUNTIME_HELPER_TARGET ?=
PLUGIN_INSPECTOR_TARGET ?= $(OUT_DIR)/muon-plugin-inspector
LIB_TARGET ?= $(OUT_DIR)/libmuon-builder.a
CPPFLAGS ?=
# Keep file sizes and directory inode numbers representable on 32-bit targets.
override CPPFLAGS += -D_FILE_OFFSET_BITS=64 -I../core/common/include
CFLAGS ?= -std=c99 -Os -DNDEBUG -Wall -Wextra -pedantic
LDFLAGS ?= -s -static
LAUNCHER_CPPFLAGS ?=
LAUNCHER_LDFLAGS ?= $(LDFLAGS)
LAUNCHER_LDLIBS ?=
PLUGIN_INSPECTOR_LDFLAGS ?= $(LDFLAGS)
PLUGIN_INSPECTOR_LDLIBS ?=
PREPARE_WINDOWS_RESOURCE_OBJECTS ?=
LAUNCHER_RESOURCE_OBJECTS ?=
PLUGIN_INSPECTOR_RESOURCE_OBJECTS ?=
WINDRES ?= windres
WINDOWS_FILESYSTEM_TEST_TARGET ?=
WINDOWS_NOTIFICATION_TEST_TARGET ?=
VERSION_HEADER ?=
RUNTIME_INFO_HEADER ?=
AR ?= ar
YYJSON_SOURCE_DIR ?= ./.deps/src/yyjson-0.12.0/src
ENGRAVER_SOURCE_DIR ?= ../deps/engraver
ENGRAVER_INCLUDE_DIR ?= $(ENGRAVER_SOURCE_DIR)/libengraver/include
SHA2_SOURCE_DIR ?= ../deps/sha2
SHA2_CPPFLAGS := -I"$(SHA2_SOURCE_DIR)"
LDLIBS ?=

PREPARE_OBJECTS := $(OUT_DIR)/prepare.o $(OUT_DIR)/prepare_cef.o $(OUT_DIR)/prepare_node.o $(OUT_DIR)/linux_module_cache.o $(OUT_DIR)/launcher_config.o $(OUT_DIR)/common.o $(OUT_DIR)/sha1.o $(OUT_DIR)/sha2.o $(OUT_DIR)/yyjson.o
PREPARE_RESOURCE_OBJECTS := $(OUT_DIR)/muon_prepare_resource.o $(OUT_DIR)/engraver.o

ifneq ($(strip $(PREPARE_WINDOWS_RESOURCE_OBJECTS)),)
PREPARE_OBJECTS += $(OUT_DIR)/muon_windows_path.o
LAUNCHER_NOTIFICATION_OBJECTS := $(OUT_DIR)/muon_notification_windows.o
LAUNCHER_NOTIFICATION_LIBS := -lruntimeobject -lole32 -lshell32 -ladvapi32
UNICODE_ENTRY_FLAGS := -municode
endif

$(OUT_DIR)/muon_notification_windows.o: src/muon_notification_windows.c src/muon_notification_windows.h
	mkdir -p "$(OUT_DIR)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o "$@" "$<"

$(OUT_DIR)/muon_windows_path.o: ../core/common/src/muon_windows_path.c ../core/common/include/muon_windows_path.h
	mkdir -p "$(OUT_DIR)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o "$@" "$<"

.PHONY: all clean

all: $(LIB_TARGET) $(PREPARE_TARGET) $(LAUNCHER_TARGET) $(RUNTIME_HELPER_TARGET) $(PLUGIN_INSPECTOR_TARGET) $(WINDOWS_FILESYSTEM_TEST_TARGET) $(WINDOWS_NOTIFICATION_TEST_TARGET)

.PHONY: test-linux-module-cache
test-linux-module-cache: $(OUT_DIR)/linux_module_cache_test
	"$<"

$(OUT_DIR)/linux_module_cache_test: test/linux_module_cache_test.c src/linux_module_cache.c src/linux_module_cache.h
	mkdir -p "$(OUT_DIR)"
	$(CC) $(CFLAGS) -Isrc -o "$@" test/linux_module_cache_test.c src/linux_module_cache.c

$(OUT_DIR)/prepare.o: src/prepare.c src/prepare.h src/prepare_progress.h src/prepare_cef.h src/prepare_node.h src/linux_module_cache.h src/launcher_config.h src/common.h src/sha1.h $(SHA2_SOURCE_DIR)/sha2.h $(YYJSON_SOURCE_DIR)/yyjson.h $(VERSION_HEADER) $(RUNTIME_INFO_HEADER)
	mkdir -p "$(OUT_DIR)"
	$(CC) $(CPPFLAGS) $(SHA2_CPPFLAGS) $(CFLAGS) -c -o "$@" "$<"

$(OUT_DIR)/prepare_cef.o: src/prepare_cef.c src/prepare_cef.h src/prepare_progress.h src/common.h src/sha1.h $(SHA2_SOURCE_DIR)/sha2.h $(YYJSON_SOURCE_DIR)/yyjson.h $(VERSION_HEADER)
	mkdir -p "$(OUT_DIR)"
	$(CC) $(CPPFLAGS) $(SHA2_CPPFLAGS) $(CFLAGS) -c -o "$@" "$<"

$(OUT_DIR)/prepare_node.o: src/prepare_node.c src/prepare_node.h src/prepare_progress.h src/common.h $(SHA2_SOURCE_DIR)/sha2.h $(YYJSON_SOURCE_DIR)/yyjson.h
	mkdir -p "$(OUT_DIR)"
	$(CC) $(CPPFLAGS) $(SHA2_CPPFLAGS) $(CFLAGS) -c -o "$@" "$<"

$(OUT_DIR)/linux_module_cache.o: src/linux_module_cache.c src/linux_module_cache.h
	mkdir -p "$(OUT_DIR)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o "$@" "$<"

$(OUT_DIR)/launcher_config.o: src/launcher_config.c src/launcher_config.h src/prepare_node.h src/prepare_progress.h src/common.h src/sha1.h $(SHA2_SOURCE_DIR)/sha2.h
	mkdir -p "$(OUT_DIR)"
	$(CC) $(CPPFLAGS) $(SHA2_CPPFLAGS) $(CFLAGS) -c -o "$@" "$<"

$(OUT_DIR)/common.o: src/common.c src/common.h src/prepare_progress.h src/sha1.h $(SHA2_SOURCE_DIR)/sha2.h $(YYJSON_SOURCE_DIR)/yyjson.h $(VERSION_HEADER) ../core/common/include/muon_windows_path.h
	mkdir -p "$(OUT_DIR)"
	$(CC) $(CPPFLAGS) $(SHA2_CPPFLAGS) $(CFLAGS) -c -o "$@" "$<"

$(OUT_DIR)/sha1.o: src/sha1.c src/sha1.h
	mkdir -p "$(OUT_DIR)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o "$@" "$<"

$(OUT_DIR)/sha2.o: $(SHA2_SOURCE_DIR)/sha2.c $(SHA2_SOURCE_DIR)/sha2.h
	mkdir -p "$(OUT_DIR)"
	$(CC) $(CPPFLAGS) $(SHA2_CPPFLAGS) $(CFLAGS) -c -o "$@" "$<"

$(OUT_DIR)/yyjson.o: $(YYJSON_SOURCE_DIR)/yyjson.c $(YYJSON_SOURCE_DIR)/yyjson.h
	mkdir -p "$(OUT_DIR)"
	# yyjson's floating-point lookup table is unused on x87 targets.
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-unused-const-variable -c -o "$@" "$<"

$(OUT_DIR)/launcher_progress.o: src/launcher_progress.c src/launcher_progress.h src/prepare_progress.h
	mkdir -p "$(OUT_DIR)"
	$(CC) $(CPPFLAGS) $(LAUNCHER_CPPFLAGS) $(CFLAGS) -c -o "$@" "$<"

$(OUT_DIR)/muon_prepare_resource.o: src/muon_prepare_resource.c src/muon_prepare_resource.h src/common.h src/sha1.h $(SHA2_SOURCE_DIR)/sha2.h src/engrave.h $(ENGRAVER_INCLUDE_DIR)/engraver.h
	mkdir -p "$(OUT_DIR)"
	$(CC) $(CPPFLAGS) $(SHA2_CPPFLAGS) -I"$(ENGRAVER_INCLUDE_DIR)" $(CFLAGS) -c -o "$@" "$<"

$(OUT_DIR)/engraver.o: $(ENGRAVER_SOURCE_DIR)/libengraver/src/engraver.c src/engrave.h $(ENGRAVER_INCLUDE_DIR)/engraver.h $(YYJSON_SOURCE_DIR)/yyjson.h
	mkdir -p "$(OUT_DIR)"
	$(CC) $(CPPFLAGS) -Isrc -I"$(ENGRAVER_INCLUDE_DIR)" -I"$(YYJSON_SOURCE_DIR)" $(CFLAGS) -c -o "$@" "$<"

$(LIB_TARGET): $(PREPARE_OBJECTS)
	rm -f "$@"
	$(AR) rcs "$@" $(PREPARE_OBJECTS)

ifneq ($(strip $(PREPARE_WINDOWS_RESOURCE_OBJECTS)),)
$(PREPARE_WINDOWS_RESOURCE_OBJECTS): $(OUT_DIR)/muon_prepare.rc src/muon_prepare.manifest
	$(WINDRES) -I src -I ../images "$<" "$@"
endif

ifneq ($(strip $(LAUNCHER_RESOURCE_OBJECTS)),)
$(LAUNCHER_RESOURCE_OBJECTS): $(OUT_DIR)/muon_launcher.rc src/muon_launcher.manifest
	$(WINDRES) -I src -I ../images "$<" "$@"
endif

ifneq ($(strip $(PLUGIN_INSPECTOR_RESOURCE_OBJECTS)),)
$(PLUGIN_INSPECTOR_RESOURCE_OBJECTS): $(OUT_DIR)/muon_plugin_inspector.rc src/muon_plugin_inspector.manifest
	$(WINDRES) -I src -I ../images "$<" "$@"
endif

$(PREPARE_TARGET): src/muon_prepare.c src/prepare.h src/prepare_progress.h src/muon_prepare_resource.h $(LIB_TARGET) $(PREPARE_RESOURCE_OBJECTS) $(PREPARE_WINDOWS_RESOURCE_OBJECTS)
	mkdir -p "$(OUT_DIR)"
	$(CC) $(CPPFLAGS) $(SHA2_CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(UNICODE_ENTRY_FLAGS) -o "$@" src/muon_prepare.c $(PREPARE_RESOURCE_OBJECTS) $(PREPARE_WINDOWS_RESOURCE_OBJECTS) "$(LIB_TARGET)" $(LDLIBS)

$(LAUNCHER_TARGET): src/muon_launcher.c src/launcher_progress.h src/launcher_config.h src/common.h src/sha1.h $(SHA2_SOURCE_DIR)/sha2.h src/prepare.h src/prepare_progress.h $(OUT_DIR)/launcher_progress.o $(LIB_TARGET) $(LAUNCHER_RESOURCE_OBJECTS) $(LAUNCHER_NOTIFICATION_OBJECTS)
	mkdir -p "$(OUT_DIR)"
	$(CC) $(CPPFLAGS) $(SHA2_CPPFLAGS) $(LAUNCHER_CPPFLAGS) $(CFLAGS) $(LAUNCHER_LDFLAGS) $(UNICODE_ENTRY_FLAGS) -o "$@" src/muon_launcher.c "$(OUT_DIR)/launcher_progress.o" $(LAUNCHER_NOTIFICATION_OBJECTS) $(LAUNCHER_RESOURCE_OBJECTS) "$(LIB_TARGET)" $(LDLIBS) $(LAUNCHER_LDLIBS) $(LAUNCHER_NOTIFICATION_LIBS)

ifneq ($(strip $(RUNTIME_HELPER_TARGET)),)
$(RUNTIME_HELPER_TARGET): src/muon_runtime_helper.c src/prepare.h src/common.h src/sha1.h $(SHA2_SOURCE_DIR)/sha2.h $(LIB_TARGET)
	mkdir -p "$(OUT_DIR)"
	$(CC) $(CPPFLAGS) $(SHA2_CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" src/muon_runtime_helper.c "$(LIB_TARGET)" $(LDLIBS)
endif

$(PLUGIN_INSPECTOR_TARGET): src/muon_plugin_inspector.c src/common.h ../sdk/include/muon_plugin_api.h $(SHA2_SOURCE_DIR)/sha2.h $(YYJSON_SOURCE_DIR)/yyjson.h $(LIB_TARGET) $(PLUGIN_INSPECTOR_RESOURCE_OBJECTS)
	mkdir -p "$(OUT_DIR)"
	$(CC) $(CPPFLAGS) $(SHA2_CPPFLAGS) -I"../sdk/include" $(CFLAGS) $(PLUGIN_INSPECTOR_LDFLAGS) $(UNICODE_ENTRY_FLAGS) -o "$@" src/muon_plugin_inspector.c $(PLUGIN_INSPECTOR_RESOURCE_OBJECTS) "$(LIB_TARGET)" $(LDLIBS) $(PLUGIN_INSPECTOR_LDLIBS)

ifneq ($(strip $(WINDOWS_FILESYSTEM_TEST_TARGET)),)
$(WINDOWS_FILESYSTEM_TEST_TARGET): test/windows_filesystem_test.c src/common.h $(LIB_TARGET) $(PREPARE_RESOURCE_OBJECTS)
	$(CC) $(CPPFLAGS) $(SHA2_CPPFLAGS) -Isrc $(CFLAGS) $(LDFLAGS) -municode -Wl,--wrap=fwrite,--wrap=muon_atomic_replace -o "$@" "$<" $(PREPARE_RESOURCE_OBJECTS) "$(LIB_TARGET)" $(LDLIBS)
endif

ifneq ($(strip $(WINDOWS_NOTIFICATION_TEST_TARGET)),)
$(WINDOWS_NOTIFICATION_TEST_TARGET): test/windows_notification_test.c src/muon_notification_windows.h $(OUT_DIR)/muon_notification_windows.o
	$(CC) $(CPPFLAGS) -Isrc $(CFLAGS) $(LDFLAGS) -o "$@" "$<" "$(OUT_DIR)/muon_notification_windows.o" -lruntimeobject -lole32 -lshell32 -ladvapi32
endif

clean:
	rm -f "$(PREPARE_TARGET)" "$(LAUNCHER_TARGET)" $(if $(strip $(RUNTIME_HELPER_TARGET)),"$(RUNTIME_HELPER_TARGET)") "$(PLUGIN_INSPECTOR_TARGET)" "$(LIB_TARGET)" "$(OUT_DIR)/launcher_progress.o" $(LAUNCHER_NOTIFICATION_OBJECTS) $(PREPARE_WINDOWS_RESOURCE_OBJECTS) $(LAUNCHER_RESOURCE_OBJECTS) $(PLUGIN_INSPECTOR_RESOURCE_OBJECTS) $(PREPARE_OBJECTS) $(PREPARE_RESOURCE_OBJECTS)
