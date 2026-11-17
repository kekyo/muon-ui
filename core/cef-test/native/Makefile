CC = gcc
OUTPUT ?= /tmp/muon-gtk-runtime-probe

.PHONY: all
all:
	$(CC) -std=c99 -Wall -Wextra -Werror gtk-runtime-probe.c $$(pkg-config --cflags --libs gtk+-3.0) -o "$(OUTPUT)"

CXX ?= g++
CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic -Werror -O2
OUT := .build

.PHONY: notification-toggle-test
notification-toggle-test: $(OUT)/notification-toggle-test

$(OUT)/notification-toggle-test: muon_notification_toggle_test.cpp muon_notification_toggle.h
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) muon_notification_toggle_test.cpp -o $@
