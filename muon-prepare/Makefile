OUT_DIR ?= ./dist
PREPARE_TARGET ?= $(OUT_DIR)/muon-prepare
BOOTSTRAP_TARGET ?= $(OUT_DIR)/muon-bootstrap
LIB_TARGET ?= $(OUT_DIR)/libmuon-prepare.a
CPPFLAGS ?=
CFLAGS ?= -std=c99 -Os -DNDEBUG -Wall -Wextra -pedantic
LDFLAGS ?= -s -static
BOOTSTRAP_CPPFLAGS ?=
BOOTSTRAP_LDFLAGS ?= $(LDFLAGS)
BOOTSTRAP_LDLIBS ?=
VERSION_HEADER ?=
RUNTIME_INFO_HEADER ?=
AR ?= ar
YYJSON_SOURCE_DIR ?= ./.deps/src/yyjson-0.12.0/src
LDLIBS ?=

PREPARE_OBJECTS := $(OUT_DIR)/prepare.o $(OUT_DIR)/prepare_cef.o $(OUT_DIR)/bootstrap_config.o $(OUT_DIR)/common.o $(OUT_DIR)/sha1.o $(OUT_DIR)/yyjson.o

.PHONY: all clean

all: $(LIB_TARGET) $(PREPARE_TARGET) $(BOOTSTRAP_TARGET)

$(OUT_DIR)/prepare.o: src/prepare.c src/prepare.h src/prepare_progress.h src/prepare_cef.h src/bootstrap_config.h src/common.h src/sha1.h $(YYJSON_SOURCE_DIR)/yyjson.h $(VERSION_HEADER) $(RUNTIME_INFO_HEADER)
	mkdir -p "$(OUT_DIR)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o "$@" "$<"

$(OUT_DIR)/prepare_cef.o: src/prepare_cef.c src/prepare_cef.h src/prepare_progress.h src/common.h src/sha1.h $(YYJSON_SOURCE_DIR)/yyjson.h $(VERSION_HEADER)
	mkdir -p "$(OUT_DIR)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o "$@" "$<"

$(OUT_DIR)/bootstrap_config.o: src/bootstrap_config.c src/bootstrap_config.h src/common.h
	mkdir -p "$(OUT_DIR)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o "$@" "$<"

$(OUT_DIR)/common.o: src/common.c src/common.h src/prepare_progress.h src/sha1.h $(YYJSON_SOURCE_DIR)/yyjson.h $(VERSION_HEADER)
	mkdir -p "$(OUT_DIR)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o "$@" "$<"

$(OUT_DIR)/sha1.o: src/sha1.c src/sha1.h
	mkdir -p "$(OUT_DIR)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o "$@" "$<"

$(OUT_DIR)/yyjson.o: $(YYJSON_SOURCE_DIR)/yyjson.c $(YYJSON_SOURCE_DIR)/yyjson.h
	mkdir -p "$(OUT_DIR)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o "$@" "$<"

$(OUT_DIR)/bootstrap_progress.o: src/bootstrap_progress.c src/bootstrap_progress.h src/prepare_progress.h
	mkdir -p "$(OUT_DIR)"
	$(CC) $(CPPFLAGS) $(BOOTSTRAP_CPPFLAGS) $(CFLAGS) -c -o "$@" "$<"

$(LIB_TARGET): $(PREPARE_OBJECTS)
	rm -f "$@"
	$(AR) rcs "$@" $(PREPARE_OBJECTS)

$(PREPARE_TARGET): src/muon_prepare.c src/prepare.h src/prepare_progress.h $(LIB_TARGET)
	mkdir -p "$(OUT_DIR)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" src/muon_prepare.c "$(LIB_TARGET)" $(LDLIBS)

$(BOOTSTRAP_TARGET): src/muon_bootstrap.c src/bootstrap_progress.h src/prepare.h src/prepare_progress.h $(OUT_DIR)/bootstrap_progress.o $(LIB_TARGET)
	mkdir -p "$(OUT_DIR)"
	$(CC) $(CPPFLAGS) $(BOOTSTRAP_CPPFLAGS) $(CFLAGS) $(BOOTSTRAP_LDFLAGS) -o "$@" src/muon_bootstrap.c "$(OUT_DIR)/bootstrap_progress.o" "$(LIB_TARGET)" $(LDLIBS) $(BOOTSTRAP_LDLIBS)

clean:
	rm -f "$(PREPARE_TARGET)" "$(BOOTSTRAP_TARGET)" "$(LIB_TARGET)" "$(OUT_DIR)/bootstrap_progress.o" $(PREPARE_OBJECTS)
