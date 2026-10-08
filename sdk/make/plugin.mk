# muon native plugin build. Include from an application-owned Makefile.
# Set SOURCES, OUTPUT and optionally LANGUAGE=c and CARDIO=0.
LANGUAGE ?= c++
CARDIO ?= $(if $(filter c,$(LANGUAGE)),0,1)
MUON_PACKAGE := $(if $(filter 1,$(CARDIO)),muon-plugin-cardio,muon-plugin)
MUON_COMPILER := $(if $(filter c,$(LANGUAGE)),$(CC),$(CXX))
MUON_STANDARD := $(if $(filter c,$(LANGUAGE)),c99,c++20)
MUON_CFLAGS := $(shell pkg-config --cflags $(MUON_PACKAGE))
MUON_LIBS := $(shell pkg-config --libs $(MUON_PACKAGE))
comma := ,
MUON_LOADER_FLAGS := $(if $(findstring windows-,$(MUON_TARGET)),,-Wl$(comma)-soname$(comma)$(notdir $(OUTPUT)))
.PHONY: all
all: $(OUTPUT)
$(OUTPUT): $(SOURCES)
	@mkdir -p "$(dir $(OUTPUT))"
	$(MUON_COMPILER) -std=$(MUON_STANDARD) -Wall -Wextra -Wpedantic -Werror $(CPPFLAGS) $(CFLAGS) $(CXXFLAGS) $(MUON_CFLAGS) -shared $(SOURCES) -o "$@" $(LDFLAGS) $(MUON_LOADER_FLAGS) $(MUON_LIBS) $(LDLIBS)
