# Set MUON_SDK, MUON_TARGET, CC, CXX and PKG_CONFIG_PATH with muon-plugin env.
MUON_PLUGIN_PROJECT := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
SOURCES := $(MUON_PLUGIN_PROJECT)plugin.cpp
LANGUAGE := c++
OUTPUT ?= $(if $(MUON_OUTPUT),$(MUON_OUTPUT),$(MUON_PLUGIN_PROJECT).muon-plugin/$(MUON_TARGET)/$(if $(findstring android-,$(MUON_TARGET)),lib,)counter$(if $(findstring windows-,$(MUON_TARGET)),.dll,.so))
include $(MUON_SDK)/make/plugin.mk
