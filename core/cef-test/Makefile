WINDOWS_TARGET ?= windows-amd64
WINDOWS_CC = $(if $(filter windows-i686,$(WINDOWS_TARGET)),i686,x86_64)-w64-mingw32-gcc
LONG_PATH_FIXTURE = .run/windows-long-path/$(WINDOWS_TARGET)/fixture.exe
LONG_PATH_TEST = .run/windows-long-path/$(WINDOWS_TARGET)/path-test.exe

.PHONY: windows-long-path-fixture windows-path-test
windows-long-path-fixture: $(LONG_PATH_FIXTURE)
windows-path-test: $(LONG_PATH_FIXTURE) $(LONG_PATH_TEST)

$(LONG_PATH_FIXTURE): plugin-e2e/windows-long-path-fixture.c
	mkdir -p "$(@D)"
	$(WINDOWS_CC) -std=c99 -Wall -Wextra -Werror -pedantic -municode -static -o "$@" "$<"

$(LONG_PATH_TEST): plugin-e2e/windows-path-test.c ../common/src/muon_windows_path.c ../common/include/muon_windows_path.h
	mkdir -p "$(@D)"
	$(WINDOWS_CC) -std=c99 -Wall -Wextra -Werror -pedantic -municode -static -I../common/include -o "$@" "$<" ../common/src/muon_windows_path.c
