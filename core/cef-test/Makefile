WINDOWS_TARGET ?= windows-amd64
WINDOWS_CC = $(if $(filter windows-i686,$(WINDOWS_TARGET)),i686,x86_64)-w64-mingw32-gcc
LONG_PATH_FIXTURE = .run/windows-long-path/$(WINDOWS_TARGET)/fixture.exe
LONG_PATH_TEST = .run/windows-long-path/$(WINDOWS_TARGET)/path-test.exe
DOWNLOAD_FIXTURE = .run/windows-long-path/$(WINDOWS_TARGET)/download-fixture.exe
PATH_PLUGIN = .run/windows-long-path/$(WINDOWS_TARGET)/path-plugin.dll
PATH_PLUGIN_DEPENDENCY = .run/windows-long-path/$(WINDOWS_TARGET)/path-dependency.dll
PATH_PROCESS = .run/windows-long-path/$(WINDOWS_TARGET)/path-process.exe
PATH_PROGRESS = .run/windows-long-path/$(WINDOWS_TARGET)/path-progress.exe

.PHONY: windows-long-path-fixture windows-path-test windows-path-plugin windows-path-process windows-path-progress
windows-long-path-fixture: $(LONG_PATH_FIXTURE)
windows-path-test: $(LONG_PATH_FIXTURE) $(LONG_PATH_TEST) $(DOWNLOAD_FIXTURE)
windows-path-plugin: $(PATH_PLUGIN)
windows-path-process: $(PATH_PROCESS)
windows-path-progress: $(PATH_PROGRESS)

$(PATH_PROGRESS): plugin-e2e/windows-path-progress.c ../../builder/src/launcher_progress.c ../../builder/src/launcher_progress.h ../../builder/src/prepare_progress.h
	mkdir -p "$(@D)"
	$(WINDOWS_CC) -std=c99 -Wall -Wextra -Werror -pedantic -municode -static -I../../builder/src -o "$@" "$<" ../../builder/src/launcher_progress.c -lcomctl32 -lgdi32 -limm32

$(PATH_PROCESS): plugin-e2e/windows-path-process.c
	mkdir -p "$(@D)"
	$(WINDOWS_CC) -std=c99 -Wall -Wextra -Werror -pedantic -municode -static -o "$@" "$<"

$(PATH_PLUGIN_DEPENDENCY): plugin-e2e/windows-path-plugin.c
	mkdir -p "$(@D)"
	$(WINDOWS_CC) -std=c99 -Wall -Wextra -Werror -pedantic -shared -static -DMUON_PATH_DEPENDENCY -Wl,--out-implib,"$(@D)/path-dependency.a" -o "$@" "$<"

$(PATH_PLUGIN): plugin-e2e/windows-path-plugin.c ../../sdk/include/muon_plugin_api.h $(PATH_PLUGIN_DEPENDENCY)
	$(WINDOWS_CC) -std=c99 -Wall -Wextra -Werror -pedantic -shared -static -I../../sdk/include -o "$@" "$<" "$(@D)/path-dependency.a"

$(DOWNLOAD_FIXTURE): plugin-e2e/windows-download-fixture.c
	mkdir -p "$(@D)"
	$(WINDOWS_CC) -std=c99 -Wall -Wextra -Werror -pedantic -municode -static -o "$@" "$<" -lws2_32

$(LONG_PATH_FIXTURE): plugin-e2e/windows-long-path-fixture.c
	mkdir -p "$(@D)"
	$(WINDOWS_CC) -std=c99 -Wall -Wextra -Werror -pedantic -municode -static -o "$@" "$<"

$(LONG_PATH_TEST): plugin-e2e/windows-path-test.c ../common/src/muon_windows_path.c ../common/include/muon_windows_path.h
	mkdir -p "$(@D)"
	$(WINDOWS_CC) -std=c99 -Wall -Wextra -Werror -pedantic -municode -static -I../common/include -o "$@" "$<" ../common/src/muon_windows_path.c
