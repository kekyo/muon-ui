/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "ui/muon_ui_title_bar.h"

#include "yyjson.h"

#include <cstdlib>
#include <string>

namespace {

constexpr char kMuonTitleBarMode[] = "custom";
constexpr int kMuonTitleBarHeight = 36;
#if defined(_WIN32)
constexpr int kMuonTitleBarControlsWidth = 138;
#else
constexpr int kMuonTitleBarControlsWidth = 96;
#endif

constexpr char kMuonTitleBarHtml[] = R"HTML(
<div id="muon-title-bar" class="title-bar">
  <div id="muon-icon-slot" class="app-icon-slot"><img id="muon-icon" class="app-icon" alt=""></div>
  <div id="muon-title" class="title">Muon</div>
  <div id="muon-controls" class="controls" aria-label="Window controls">
    <button id="muon-minimize" class="control" data-action="minimize" aria-label="Minimize"><span class="icon minimize"></span></button>
    <button id="muon-maximize" class="control" data-action="maximize" aria-label="Maximize"><span class="icon maximize"></span></button>
    <button id="muon-close" class="control close" data-action="close" aria-label="Close"><span class="icon close"></span></button>
  </div>
</div>
)HTML";

constexpr char kMuonTitleBarCss[] = R"CSS(
:root {
  color-scheme: light dark;
  font-family: "Noto Sans", "Segoe UI", sans-serif;
  --muon-titlebar-bg-top: #f6f5f4;
  --muon-titlebar-bg-bottom: #e7e5e2;
  --muon-titlebar-bg-inactive-top: #eeeeec;
  --muon-titlebar-bg-inactive-bottom: #deddda;
  --muon-titlebar-fg: #2e3436;
  --muon-titlebar-fg-inactive: #77767b;
  --muon-titlebar-border: #b9b7b3;
  --muon-titlebar-button-hover: rgba(0, 0, 0, 0.08);
  --muon-titlebar-button-active: rgba(0, 0, 0, 0.14);"
  --muon-titlebar-close-hover: #6c0c0c;
  --muon-titlebar-close-active: #b00404;
  --muon-titlebar-close-fg: #ffffff;
  --muon-titlebar-icon-backdrop: #ebe9e6;
}

@media (prefers-color-scheme: dark) {
  :root {
    --muon-titlebar-bg-top: #3d3d3d;
    --muon-titlebar-bg-bottom: #303030;
    --muon-titlebar-bg-inactive-top: #333333;
    --muon-titlebar-bg-inactive-bottom: #292929;
    --muon-titlebar-fg: #eeeeec;
    --muon-titlebar-fg-inactive: #9a9996;
    --muon-titlebar-border: #1f1f1f;
    --muon-titlebar-button-hover: rgba(255, 255, 255, 0.12);
    --muon-titlebar-button-active: rgba(255, 255, 255, 0.18);
    --muon-titlebar-close-hover: #511010;
    --muon-titlebar-close-active: #8f1a1a;
    --muon-titlebar-icon-backdrop: #303030;
  }
}

html,
body {
  width: 100%;
  height: 100%;
  margin: 0;
  overflow: hidden;
  background: var(--muon-titlebar-bg-bottom);
}

body {
  -webkit-user-select: none;
  user-select: none;
}

.title-bar {
  height: 36px;
  box-sizing: border-box;
  display: flex;
  align-items: center;
  background: linear-gradient(
    var(--muon-titlebar-bg-top),
    var(--muon-titlebar-bg-bottom)
  );
  border-bottom: 1px solid var(--muon-titlebar-border);
  color: var(--muon-titlebar-fg);
}

.title-bar.inactive {
  background: linear-gradient(
    var(--muon-titlebar-bg-inactive-top),
    var(--muon-titlebar-bg-inactive-bottom)
  );
  color: var(--muon-titlebar-fg-inactive);
}

.title {
  min-width: 0;
  flex: 1 1 auto;
  padding: 0 12px 0 0;
  font-size: 13px;
  line-height: 35px;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

.app-icon-slot {
  flex: 0 0 36px;
  width: 36px;
  height: 35px;
  display: grid;
  place-items: center;
}

.app-icon {
  display: none;
  width: 22px;
  height: 22px;
  object-fit: contain;
}

.app-icon.visible {
  display: block;
}

.controls {
  flex: 0 0 138px;
  width: 138px;
  height: 35px;
  display: flex;
  justify-content: flex-end;
}

.control {
  width: 46px;
  height: 35px;
  margin: 0;
  border: 0;
  padding: 0;
  display: grid;
  place-items: center;
  background: transparent;
  color: inherit;
  -webkit-app-region: no-drag;
}

.control:hover {
  background: var(--muon-titlebar-button-hover);
}

.control:active {
  background: var(--muon-titlebar-button-active);
}

.control.close:hover {
  background: var(--muon-titlebar-close-hover);
  color: var(--muon-titlebar-close-fg);
}

.control.close:active {
  background: var(--muon-titlebar-close-active);
}

.icon {
  position: relative;
  display: block;
  width: 12px;
  height: 12px;
  box-sizing: border-box;
}

.icon.minimize::before {
  content: "";
  position: absolute;
  left: 2px;
  right: 2px;
  bottom: 2px;
  height: 1px;
  background: currentColor;
}

.icon.maximize::before {
  content: "";
  position: absolute;
  left: 3px;
  top: 3px;
  width: 6px;
  height: 6px;
  box-sizing: border-box;
  border: 1px solid currentColor;
}

.title-bar.maximized .icon.maximize::before {
  left: 3px;
  top: 1px;
  width: 8px;
  height: 8px;
}

.title-bar.maximized .icon.maximize::after {
  content: "";
  position: absolute;
  left: 1px;
  top: 3px;
  width: 8px;
  height: 8px;
  box-sizing: border-box;
  border: 1px solid currentColor;
  background: var(--muon-titlebar-icon-backdrop);
}

.icon.close::before,
.icon.close::after {
  content: "";
  position: absolute;
  left: 1px;
  top: 5px;
  width: 10px;
  height: 1px;
  background: currentColor;
}

.icon.close::before {
  transform: rotate(45deg);
}

.icon.close::after {
  transform: rotate(-45deg);
}
)CSS";

#if !defined(_WIN32)
constexpr char kMuonTitleBarLinuxCss[] = R"CSS(
:root {
  --muon-titlebar-round-button-bg: rgba(0, 0, 0, 0.16);
  --muon-titlebar-round-button-hover: rgba(0, 0, 0, 0.24);
  --muon-titlebar-round-button-active: rgba(0, 0, 0, 0.32);
  --muon-titlebar-close-hover: rgba(0, 0, 0, 0.24);
  --muon-titlebar-close-active: rgba(0, 0, 0, 0.32);
}

@media (prefers-color-scheme: dark) {
  :root {
    --muon-titlebar-round-button-bg: rgba(255, 255, 255, 0.12);
    --muon-titlebar-round-button-hover: rgba(255, 255, 255, 0.18);
    --muon-titlebar-round-button-active: rgba(255, 255, 255, 0.24);
    --muon-titlebar-close-hover: rgba(255, 255, 255, 0.18);
    --muon-titlebar-close-active: rgba(255, 255, 255, 0.24);
  }
}

.controls {
  flex: 0 0 96px;
  width: 96px;
  box-sizing: border-box;
  align-items: center;
  gap: 12px;
  padding-right: 8px;
}

.control {
  width: 20px;
  height: 20px;
  appearance: none;
  border-radius: 999px;
  background: var(--muon-titlebar-round-button-bg);
}

.control:hover {
  background: var(--muon-titlebar-round-button-hover);
}

.control:active {
  background: var(--muon-titlebar-round-button-active);
}

.control.close:hover {
  background: var(--muon-titlebar-close-hover);
  color: var(--muon-titlebar-close-fg);
}

.control.close:active {
  background: var(--muon-titlebar-close-active);
}
)CSS";
#endif

constexpr char kMuonTitleBarJs[] = R"JS(
(() => {
  const bar = document.getElementById("muon-title-bar");
  const icon = document.getElementById("muon-icon");
  const title = document.getElementById("muon-title");
  const sendAction = (action) => {
    globalThis.location.href =
      `https://muon.internal/title-bar/${action}?t=${Date.now()}`;
  };

  for (const button of document.querySelectorAll("[data-action]")) {
    button.addEventListener("click", (event) => {
      event.preventDefault();
      sendAction(button.dataset.action);
    });
  }

  globalThis.__muonTitleBar = {
    setTitle(nextTitle) {
      title.textContent = typeof nextTitle === "string" && nextTitle.length > 0
        ? nextTitle
        : "Muon";
    },
    setIcon(nextIcon) {
      if (typeof nextIcon === "string" && nextIcon.length > 0) {
        icon.src = nextIcon;
        icon.classList.add("visible");
        return;
      }
      icon.removeAttribute("src");
      icon.classList.remove("visible");
    },
    setState(state) {
      bar.classList.toggle("inactive", state?.active === false);
      bar.classList.toggle("maximized", state?.maximized === true);
    },
  };
})();
)JS";

static std::string CreateTitleBarCss() {
#if defined(_WIN32)
  return kMuonTitleBarCss;
#else
  return std::string(kMuonTitleBarCss) + kMuonTitleBarLinuxCss;
#endif
}

static std::string CreateTitleBarManifest() {
  yyjson_mut_doc* document = yyjson_mut_doc_new(nullptr);
  if (document == nullptr) {
    return R"({"mode":"native"})";
  }
  yyjson_mut_val* root = yyjson_mut_obj(document);
  yyjson_mut_doc_set_root(document, root);
  yyjson_mut_obj_add_str(document, root, "mode", kMuonTitleBarMode);
  yyjson_mut_obj_add_int(document, root, "height", kMuonTitleBarHeight);
  yyjson_mut_obj_add_int(
      document, root, "controlsWidth", kMuonTitleBarControlsWidth);
  yyjson_mut_obj_add_str(document, root, "html", kMuonTitleBarHtml);
  const auto css = CreateTitleBarCss();
  yyjson_mut_obj_add_str(document, root, "css", css.c_str());
  yyjson_mut_obj_add_str(document, root, "js", kMuonTitleBarJs);

  char* encoded = yyjson_mut_write(document, 0, nullptr);
  yyjson_mut_doc_free(document);
  if (encoded == nullptr) {
    return R"({"mode":"native"})";
  }
  std::string result(encoded);
  std::free(encoded);
  return result;
}

}  // namespace

extern "C" const char* muon_ui_title_bar_get_manifest(void) {
  static const std::string manifest = CreateTitleBarManifest();
  return manifest.c_str();
}
