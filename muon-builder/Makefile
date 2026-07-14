OUT_DIR ?= ./dist
PREPARE_TARGET ?= $(OUT_DIR)/muon-builder
LAUNCHER_TARGET ?= $(OUT_DIR)/muon-launcher
RUNTIME_HELPER_TARGET ?=
LIB_TARGET ?= $(OUT_DIR)/libmuon-builder.a
CPPFLAGS ?=
CFLAGS ?= -std=c99 -Os -DNDEBUG -Wall -Wextra -pedantic
LDFLAGS ?= -s -static
LAUNCHER_CPPFLAGS ?=
LAUNCHER_LDFLAGS ?= $(LDFLAGS)
LAUNCHER_LDLIBS ?=
PREPARE_WINDOWS_RESOURCE_OBJECTS ?=
LAUNCHER_RESOURCE_OBJECTS ?=
VERSION_HEADER ?=
RUNTIME_INFO_HEADER ?=
AR ?= ar
YYJSON_SOURCE_DIR ?= ./.deps/src/yyjson-0.12.0/src
ENGRAVER_SOURCE_DIR ?= ../deps/engraver
ENGRAVER_INCLUDE_DIR ?= $(ENGRAVER_SOURCE_DIR)/libengraver/include
SHA2_SOURCE_DIR ?= ../deps/sha2
SHA2_CPPFLAGS := -I"$(SHA2_SOURCE_DIR)"
LDLIBS ?=

PREPARE_OBJECTS := $(OUT_DIR)/prepare.o $(OUT_DIR)/prepare_cef.o $(OUT_DIR)/launcher_config.o $(OUT_DIR)/common.o $(OUT_DIR)/sha1.o $(OUT_DIR)/sha2.o $(OUT_DIR)/yyjson.o
PREPARE_RESOURCE_OBJECTS := $(OUT_DIR)/muon_prepare_resource.o $(OUT_DIR)/engraver.o

.PHONY: all clean

all: $(LIB_TARGET) $(PREPARE_TARGET) $(LAUNCHER_TARGET) $(RUNTIME_HELPER_TARGET)

$(OUT_DIR)/prepare.o: src/prepare.c src/prepare.h src/prepare_progress.h src/prepare_cef.h src/launcher_config.h src/common.h src/sha1.h $(SHA2_SOURCE_DIR)/sha2.h $(YYJSON_SOURCE_DIR)/yyjson.h $(VERSION_HEADER) $(RUNTIME_INFO_HEADER)
	mkdir -p "$(OUT_DIR)"
	$(CC) $(CPPFLAGS) $(SHA2_CPPFLAGS) $(CFLAGS) -c -o "$@" "$<"

$(OUT_DIR)/prepare_cef.o: src/prepare_cef.c src/prepare_cef.h src/prepare_progress.h src/common.h src/sha1.h $(SHA2_SOURCE_DIR)/sha2.h $(YYJSON_SOURCE_DIR)/yyjson.h $(VERSION_HEADER)
	mkdir -p "$(OUT_DIR)"
	$(CC) $(CPPFLAGS) $(SHA2_CPPFLAGS) $(CFLAGS) -c -o "$@" "$<"

$(OUT_DIR)/launcher_config.o: src/launcher_config.c src/launcher_config.h src/common.h src/sha1.h $(SHA2_SOURCE_DIR)/sha2.h
	mkdir -p "$(OUT_DIR)"
	$(CC) $(CPPFLAGS) $(SHA2_CPPFLAGS) $(CFLAGS) -c -o "$@" "$<"

$(OUT_DIR)/common.o: src/common.c src/common.h src/prepare_progress.h src/sha1.h $(SHA2_SOURCE_DIR)/sha2.h $(YYJSON_SOURCE_DIR)/yyjson.h $(VERSION_HEADER)
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
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o "$@" "$<"

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

$(PREPARE_TARGET): src/muon_prepare.c src/prepare.h src/prepare_progress.h src/muon_prepare_resource.h $(LIB_TARGET) $(PREPARE_RESOURCE_OBJECTS) $(PREPARE_WINDOWS_RESOURCE_OBJECTS)
	mkdir -p "$(OUT_DIR)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" src/muon_prepare.c $(PREPARE_RESOURCE_OBJECTS) $(PREPARE_WINDOWS_RESOURCE_OBJECTS) "$(LIB_TARGET)" $(LDLIBS)

$(LAUNCHER_TARGET): src/muon_launcher.c src/launcher_progress.h src/launcher_config.h src/common.h src/sha1.h $(SHA2_SOURCE_DIR)/sha2.h src/prepare.h src/prepare_progress.h $(OUT_DIR)/launcher_progress.o $(LIB_TARGET) $(LAUNCHER_RESOURCE_OBJECTS)
	mkdir -p "$(OUT_DIR)"
	$(CC) $(CPPFLAGS) $(SHA2_CPPFLAGS) $(LAUNCHER_CPPFLAGS) $(CFLAGS) $(LAUNCHER_LDFLAGS) -o "$@" src/muon_launcher.c "$(OUT_DIR)/launcher_progress.o" $(LAUNCHER_RESOURCE_OBJECTS) "$(LIB_TARGET)" $(LDLIBS) $(LAUNCHER_LDLIBS)

ifneq ($(strip $(RUNTIME_HELPER_TARGET)),)
$(RUNTIME_HELPER_TARGET): src/muon_runtime_helper.c src/prepare.h src/common.h src/sha1.h $(SHA2_SOURCE_DIR)/sha2.h $(LIB_TARGET)
	mkdir -p "$(OUT_DIR)"
	$(CC) $(CPPFLAGS) $(SHA2_CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" src/muon_runtime_helper.c "$(LIB_TARGET)" $(LDLIBS)
endif

clean:
	rm -f "$(PREPARE_TARGET)" "$(LAUNCHER_TARGET)" $(if $(strip $(RUNTIME_HELPER_TARGET)),"$(RUNTIME_HELPER_TARGET)") "$(LIB_TARGET)" "$(OUT_DIR)/launcher_progress.o" $(PREPARE_WINDOWS_RESOURCE_OBJECTS) $(LAUNCHER_RESOURCE_OBJECTS) $(PREPARE_OBJECTS) $(PREPARE_RESOURCE_OBJECTS)
