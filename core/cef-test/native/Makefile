CC = gcc
OUTPUT ?= /tmp/muon-gtk-runtime-probe

.PHONY: all
all:
	$(CC) -std=c99 -Wall -Wextra -Werror gtk-runtime-probe.c $$(pkg-config --cflags --libs gtk+-3.0) -o "$(OUTPUT)"
