# Android API対応方針

## 目的

この文書は、[plan1.md](docs/ja/plans/plan1.md)のステップ4「Android API対応表を定義する」の判断結果である。現行のMuon公開APIを、Android WebView版でも同じ契約で提供するもの、Android固有の制約を伴って提供するもの、別APIとして設計するもの、提供しないものに分類する。

判定対象は、現在の実装が公開する次のAPIである。

- [muon.browser](./core/cef/src/browser/muon_builtin_browser.cpp)
- [muon.launcher](./core/cef/src/plugins/builtin/muon_builtin_launcher.cpp)
- [muon.environments](./core/cef/src/plugins/builtin/muon_builtin_environments.cpp)
- [muon.executor](./core/cef/src/plugins/builtin/muon_builtin_executor.cpp)
- [muon.fs](./core/cef/src/plugins/builtin/muon_builtin_fs.cpp)
- [muon.fs.dialogs](./core/cef/src/plugins/builtin/muon_builtin_fs_dialogs_plugin.cpp)
- [Node.js sidecar](./docs/ja/nodejs-sidecar.md)
- `muon.json`のうち、上記APIやAndroidの実行モデルへ影響する設定

ネットワークとアセットoriginについては、[filter-limitation.md](./filter-limitation.md)の「Android WebView検証後の確定方針」を前提とする。Android版はCEF版のネットワークdeny-by-defaultを再現せず、構成されたアセットoriginのメインフレームだけにMuonプラグインRPCを公開する。

## 判定区分

| 区分 | 意味 |
|---|---|
| 互換 | 現行の関数名、引数、戻り値、主要な効果を維持してAndroidへ実装する |
| 制約付き互換 | 現行APIを公開するが、Androidのライフサイクル、UI、ストレージなどに由来する制約を明記する |
| Android向け別設計 | 似た機能は必要になり得るが、現行APIや設定へ割り当てると契約が変わるため、Android向けAPIまたは設定として別に設計する |
| 非対応 | Android版では公開しない |
| ステップ5 | Android NDKプラグイン対応で別途決定、実装する |

この表はAndroid製品版の目標仕様を示す。初期実装へ反映済みの範囲と、後続作業へ延期した範囲は次節で区別する。

## 公開時の共通規則

- 同名APIを公開するのは、呼び出しが成功したように見えるだけではなく、表に記載した契約を実現できる場合に限る。
- 非対応またはAndroid向け別設計とした関数は、Android版のプラグインメタデータへ登録しない。no-opや常に成功する代替実装は置かない。
- simple modeでは、非対応関数を`window.muon`へ作成しない。
- validate modeでは、Android targetに存在しない関数のvirtual module importをbuild時に拒否する。実行時まで失敗を遅らせない。
- Android向け別設計は、デスクトップAPIへAndroid固有の意味を追加するのではなく、Androidのライフサイクルや権限を型と関数名に表現する。
- 共通APIにAndroid固有の制約がある場合、未対応オプションを無言で無視しない。target build時または呼び出し時に診断する。

## 現在のAndroid実装状況

現在の[Android試作](./core/android/runtime/src/main/cpp/muon_android_rpc_jni.cpp)には、共通RPCを経由する次の組み込みAPIを実装した。simple modeで公開する関数一覧は[Android APIメタデータ](./core/android/renderer/android-api.ts)を唯一の公開リストとし、一覧にない関数を動的に作成しない。

- `muon.browser`: `reload()`, fullscreen 3関数、zoom 3関数、`close()`
- `muon.environments`: `getVariables()`, `getConfigValues()`, `getProcessId()`, `getRuntimeInfo()`
- `muon.fs`: 実pathを扱う22関数。対応表で対象としたread/write、metadata、directory操作、link操作、`watch()`を含む

`muon.launcher`、`muon.executor`、`muon.fs.dialogs`、Node.js sidecar、対応表で非対応またはAndroid向け別設計としたbrowser/environment関数は公開リストへ含めていない。`muon.fs.dialogs`は、先に`content://`を`muon.fs`で扱う契約を確定するまで公開しない。

[Android設定検証器](./core/android/renderer/android-config.ts)は、Androidで意味を持たない値を拒否し、受理しても適用しないnetwork設定と`plugin.pages`を警告する。これは製品用Android build pipelineへ組み込むための検証プリミティブであり、現在の試作には合成済み`muon.json`を読み込むbuild pipeline自体はまだない。
このため、`getConfigValues()`のrouteと型は実装済みだが、現在返す値は試作用に組み込んだ設定値である。製品buildで合成済み`muon.json`の`config`を渡す接続はbuild pipelineと同時に行う。

現在のGradle設定は`x86_64`だけを対象としている。実機用`arm64-v8a`とbuild-time同梱NDKプラグインはplanのステップ5で扱う。

## `muon.browser`

Androidではdesktop windowではなく、Activity内のWebViewを操作する。Activity、task、system barはOSが管理するため、名前が似ていてもdesktop window操作と同じ結果にならない機能は別APIまたは非対応とする。[Androidのtaskとback stack](https://developer.android.com/guide/components/activities/tasks-and-back-stack)

| 関数 | 判定 | Android版の契約または理由 |
|---|---|---|
| `reload()` | 互換 | 現在のWebViewを通常再読み込みする。WebViewの`reload()`へ対応付ける |
| `hardReload()` | 非対応 | WebViewには現在のdocumentだけをCEFと同じ意味でcache bypassする公開APIがない。`clearCache(true)`はアプリ内の全WebViewへ影響するため代用しない |
| `toggleFullscreen()`, `enterFullscreen()`, `exitFullscreen()` | 制約付き互換 | top-level windowの変更ではなく、system barを隠すimmersive表示として実装する。ユーザー操作でbarが一時表示されること、multi-windowやPicture-in-PictureではOSが最終状態を決めることを許容する。[immersive mode](https://developer.android.com/develop/ui/views/layout/immersive) |
| `zoomIn()`, `zoomOut()`, `resetZoom()` | 制約付き互換 | WebView単位のMuon管理zoomとして実装し、`resetZoom()`は100%へ戻す。初期実装では状態を外部から変更するpinch zoomを無効にし、API外のscale変更を契約へ含めない |
| `show()`, `hide()` | Android向け別設計 | Activityを前面へ出す、taskをbackgroundへ移す操作はwindow表示の切り替えではなく、background Activity start制限も受ける。app/task lifecycle APIとして別設計する |
| `focus()`, `blur()` | Android向け別設計 | WebView内のview focusとdesktop top-level windowのactivationは異なる。入力focusを扱う場合はview/IME用APIとして別設計する |
| `minimize()`, `maximize()`, `restore()` | 非対応 | Android taskにはdesktop window stateと同じminimized/maximized/restore契約がない |
| `setTitleBarVisibility()`, `setTitleBarIcon()` | 非対応 | Android ActivityにMuonのdesktop title barはない。app barはアプリUI、launcher iconはbuild-time resourceとして扱う |
| `getWindowBounds()` | Android向け別設計 | `WindowMetrics`で表示可能領域は取得できるが、desktop frameを含むDIP screen coordinatesではない。画面・insets情報のread-only APIとして別設計する |
| `setWindowBounds()` | 非対応 | taskの位置と大きさはOS、端末姿勢、multi-window環境が管理し、現行の任意bounds設定を保証できない。[WindowManager](https://developer.android.com/reference/android/view/WindowManager) |
| `setContextMenuItems()`, `clearContextMenuItems()` | Android向け別設計 | WebViewのlong-press、text selection、ActionModeはCEF context menuと項目、placement、`when`条件が一致しない。Android UI用menu APIまたはWeb UIとして別設計する |
| `createTray()`, `setTrayMenu()`, `setTrayIcon()`, `setTrayTooltip()`, `removeTray()` | Android向け別設計 | Androidにdesktop system trayはない。継続動作が必要な機能はnotificationとforeground serviceの権限・lifecycleを表す別APIにする。[foreground services](https://developer.android.com/develop/background-work/services) |
| `close()` | 制約付き互換 | 所有Activityへ`finish()`を要求する。Activityを閉じてもprocess終了は保証せず、OSがprocess lifecycleを管理する |
| `shutdown()` | 非対応 | Android app processを任意の終了codeで終了する公開契約を設けない。Activity終了やservice停止とは分離する |
| `recycle()` | 非対応 | ActivityのrecreateはMuon processの終了・launcher再起動と同じではない。OS管理processをdesktopのrecycle codeへ対応付けない |

WebViewが提供する通常再読み込み、段階zoom、cache削除の範囲は[WebView API](https://developer.android.com/reference/android/webkit/WebView)に従う。とくにcache削除は対象WebViewだけに閉じないため、`hardReload()`の代替にはしない。

## `muon.launcher`

| 関数 | 判定 | Android版の契約または理由 |
|---|---|---|
| `getSettings()`, `setSettings()`, `triggerUpdate()` | 非対応 | 現行APIはCEFとNode.js runtimeを次回の`muon-launcher`起動時に準備するdesktop契約である。AndroidのAPK/AAB、WebView provider、store updateを変更するAPIではない |

Androidアプリ自身の更新機能が必要な場合は、配布元の更新機構を使うAndroid向け別APIとして検討する。Google Play配布では[In-app updates](https://developer.android.com/guide/playcore/in-app-updates)が候補になるが、MuonがWebView providerや任意runtimeを置き換える機能とはしない。

## `muon.environments`

| 関数 | 判定 | Android版の契約または理由 |
|---|---|---|
| `getVariables()` | 互換 | 現在のAndroid app processから参照できる環境変数を返す。これは現行APIの「現在のprocess環境」という契約と一致する |
| `getConfigValues()` | 互換 | 合成済み`muon.json`のtop-level `config`を同じ`Record<string, string>`として返す。試作でPromise往復を確認済み |
| `getCommandLine()` | 非対応 | ActivityはIntentとsaved stateから起動され、利用者が指定したdesktop形式の`argv`を持たない。起動情報が必要ならAndroid Intent用の別APIを設計する |
| `getProcessId()` | 互換 | 現在のMuon app process IDをnumberで返す。Androidのprocess IDはprocess再生成で変わり得る |
| `getRuntimeInfo()` | 制約付き互換 | runtime情報を返す目的と関数名は維持するが、現行型の`cefReference`と`cefRuntime`を偽装しない。`MuonRuntimeInfo`を`backend: "cef" | "android-webview"`で分岐する型とし、AndroidではOS/API level、ABI、application ID/version、WebView package/versionを返す |
| `getAutostart()`, `setAutostart()` | 非対応 | XDG AutostartやWindows Run registryと同じ契約はない。boot broadcast、background execution制限、利用者権限を扱う必要がある場合は別APIとして設計する |

Androidは、必要に応じてapp processを終了し再生成できる。[Processes and app lifecycle](https://developer.android.com/guide/components/activities/process-lifecycle) `getProcessId()`と`getRuntimeInfo()`の値は、installationやapp sessionを一意に識別するものではない。

## `muon.executor`

| 関数 | 判定 | Android版の契約または理由 |
|---|---|---|
| `spawn()` | 非対応 | PATH、任意の実行ファイル、stdio、process tree、daemonを前提とするdesktop child process契約をAndroid app sandboxへ持ち込まない |
| `loadLibrary()` | 非対応 | 任意pathの`.so`を実行時に読み込むFFIは提供しない。外部取得したnative codeの動的loadを避け、APK/AABへbuild時に同梱した正式なMuonプラグインだけをステップ5で扱う |

Androidのsecurity guidanceは、多くの形態のdynamic code loadingを避け、必要なcodeをappへ静的に組み込むことを推奨している。[Dynamic Code Loading](https://developer.android.com/privacy-and-security/risks/dynamic-code-loading) したがって、`loadLibrary()`とステップ5の正式なNDKプラグインは別機能である。

## `muon.fs`

`muon.fs`はAndroid appが実際のfilesystem pathとしてアクセスできる領域に限定して実装する。APIはAndroid sandbox、scoped storage、filesystem mountの制約を越える権限を与えない。[App-specific storage](https://developer.android.com/training/data-storage/app-specific)

| 関数 | 判定 | Android版の契約または理由 |
|---|---|---|
| `readFile()`, `writeFile()`, `readTextFile()`, `writeTextFile()`, `appendFile()`, `appendTextFile()`, `truncate()` | 制約付き互換 | app内部またはapp-specific storage上の通常file pathを扱う。OSが許可しないpathは通常のfilesystem errorとしてrejectする |
| `stat()`, `lstat()`, `exists()`, `access()`, `readdir()` | 制約付き互換 | 実pathに対するmetadataとdirectory操作として提供する。provider固有のdocument metadataは対象外とする |
| `mkdir()`, `rm()`, `unlink()`, `rmdir()`, `rename()`, `copyFile()` | 制約付き互換 | OSとfilesystemが許すpathだけを操作する。別mount間のrenameなど、filesystemが保証しない操作はerrorとして返す |
| `realpath()`, `readlink()`, `symlink()` | 制約付き互換 | symlinkを提供するfilesystem上だけで動作する。Androidで意味を持たないWindowsの`junction`指定はrejectする |
| `watch()` | 制約付き互換 | app processが生存している間のpath監視として提供し、context解放時に停止する。process終了後の永続監視は保証しない |

AndroidのStorage Access Frameworkが返す`content://` URIはfilesystem pathではない。現在の初期実装は、この表の`muon.fs`関数へURIを渡すと明示的にrejectする。将来は`content://`を`muon.fs`へ直接指定できるようにするが、permission grant、URI寿命、provider固有機能と、pathでは成立しない操作の扱いを先に定義する必要があるため、今回の実装範囲には含めない。

すべての関数で、既存のsize limit、UTF-8検査、`AbortSignal`、capability判定を維持する。Androidのcancelはbest effortであり、OS呼び出しが完了済みの場合まで結果を巻き戻すものではない。

## `muon.fs.dialogs`

AndroidではStorage Access Frameworkを使用する。選択結果はlocal pathではなく、原則として`content://` URIになる。[Access documents and other files](https://developer.android.com/training/data-storage/shared/documents-files)

| 関数 | 判定 | Android版の契約または理由 |
|---|---|---|
| `selectFile()`, `selectFiles()` | 制約付き互換 | `ACTION_OPEN_DOCUMENT`相当で1件または複数件を選択し、URI stringを返す。利用可能な場合はpersistable URI permissionを取得する |
| `selectDirectory()` | 制約付き互換 | `ACTION_OPEN_DOCUMENT_TREE`相当で1つのtree URIを返す。providerが選択できる場所を制限する場合がある |
| `selectDirectories()` | 非対応 | Android標準pickerに複数のdirectory treeを1回で選ぶ同等契約がない。複数回pickerを開く動作へ暗黙に変更しない |
| `selectSaveFile()` | 制約付き互換 | `ACTION_CREATE_DOCUMENT`相当で保存先URIを返す。providerが既存名へsuffixを付ける場合があり、CEF/desktopと同じ上書き確認は保証しない |

Android pickerはproviderがUIを所有する。そのため`title`、`defaultPath`、`buttonLabel`、`showHidden`、extension filter、`confirmOverwrite`がdesktopと同じ表示や動作になるとは保証しない。GTKまたはWin32固有optionはAndroidでは使用できず、指定時に診断する。

このdialog APIはURIを返すが、現在のpath版`muon.fs`は`content://`を処理しない。Android実装でdialogを公開する前に、少なくとも選択URIのread、write、permission releaseを`muon.fs`で扱う契約を定義する。URIをpathへ変換するfallbackは設けない。

## Node.js sidecar

| APIまたは機能 | 判定 | Android版の契約または理由 |
|---|---|---|
| `window.muon.node.createNode()`, `muon:node` | 非対応 | Android初期版では、desktop用Node executableを別processとして準備・起動するsidecar modelを提供しない |
| `node.project` | 非対応 | Android targetではbuild errorにする。設定を受理してNode連携が存在するように見せない |

Node.js sidecarは将来nodejs-mobileを利用して実装する構想がある。ただし、Androidのprocess、service、package、lifecycleに合わせた設計と検証が必要であり、今回の初期API実装には含めない。現在のAndroid targetでdesktop sidecarが利用できるように見せるfallbackは設けない。

## ネイティブMuonプラグイン

| APIまたは機能 | 判定 | Android版の契約または理由 |
|---|---|---|
| build-timeに同梱した正式なMuon native plugin | ステップ5 | Android NDKでABI別にbuildし、APK/AABへ同梱する。初期対象は実機用`arm64-v8a`とemulator用`x86_64`とする |
| `plugin.path`からのruntime探索 | 非対応 | 任意directoryからnative libraryを探索しない。packageされたplugin registryをbuild時に確定する |
| 外部plugin fileのruntime signature確認 | 非対応 | runtimeに外部fileを受け入れない。成果物の署名、package integrity、build provenanceを境界にする |
| `plugin.plugins[].allow`, `plugin.plugins[].config` | ステップ5 | build-time同梱pluginにも既存のfunction allowlistとstring configを適用する |

Android NDKではABIごとに異なるnative libraryが必要である。[Android ABIs](https://developer.android.com/ndk/guides/abis) `arm64-v8a`と`x86_64`の双方で同じplugin metadata、RPC、cancel、binary ownership契約を満たすことをステップ5の完了条件に含める。

## `muon.json`のAndroid対応

この表は設定値を無言でdesktopと異なる意味へ変えないための方針である。アセットURLの詳細と移行手順は[filter-limitation.md](./filter-limitation.md)の確定方針に従う。

| 設定 | 判定 | Android版の扱い |
|---|---|---|
| top-level `iconPath` | 制約付き互換 | Android targetのbuild時にlauncher icon resourceの入力として使う。runtime title barやtray iconには使用しない |
| top-level `config` | 互換 | `getConfigValues()`へ同じ合成済みstring mapを渡す |
| `asset` storageとHTTPS URL template | 制約付き互換 | `https://{asset_name}.asset.muon.invalid/`を既定templateとして、構成済みhostをfail-closedでlocal assetへ割り当てる |
| `browser.startPage` | 互換 | HTTPS asset URLまたはWebViewが通常読み込みできるURLを使用する。既定値は`https://main.asset.muon.invalid/index.html`へ移行する |
| `browser.profilePath` | Android向け別設計 | 省略時はAndroid管理のWebView dataを使用し、現行fieldの明示指定はbuild errorにする。data isolationが必要な場合はAndroidのprofile/data directory modelに合わせて別設定にする。現行schemaにない`browser.profile`も受理しない |
| `browser.initialWindowState` | 制約付き互換 | `normal`と`fullscreen`だけを許可し、`hidden`、`minimized`、`maximized`はAndroid targetのbuild errorにする |
| `browser.backgroundColor` | 互換 | Activity windowとWebViewの初期背景色へ反映する。`system`はAndroid themeのlight/darkを使う |
| `browser.titleBarType`, `browser.initialTitleBarVisibility`, title bar icon | 非対応 | 現行fieldを明示したAndroid targetはbuild errorにする。app barとlauncher iconはAndroid resource/UIとして扱う |
| `browser.contextMenu.mode` | 制約付き互換 | 初期版は`standard`と`disabled`だけを扱い、CEF custom menuを必要とする`custom`は拒否する |
| `browser.keybind` | 非対応 | 空ではない設定を持つAndroid targetはbuild errorにする。物理keyboard対応が必要ならAndroid input APIとして別設計する |
| `browser.allowUnsafeJavaScriptParentAccess` | 非対応 | 空ではない設定を持つAndroid targetはbuild errorにする。CEF popup/opener用の互換設定をWebViewへ移植しない |
| `plugin.mode`, `plugin.pages`, capability allowlist | 制約付き互換 | simple/validateの公開方式とfunction allowlistを維持する。ただしAndroidでは構成済みasset originのmain frameだけがRPCを送信でき、`plugin.pages`で外部originへ公開範囲を拡張できない |
| `network.allow`, `network.authorizedOrigin`, `network.localAccess` | 非対応 | CEFとの共通設定として受理するが、明示値があるAndroid targetでは適用されないことを警告する。Android WebViewの通常通信へMuon allowlistを適用しない |
| `cdp` | 非対応 | `cdp.enable: true`のAndroid targetはbuild errorにする。CEF remote debugging portへ対応付けず、WebView debuggingはdebuggable buildに限定する別の開発設定とする |
| `node.project` | 非対応 | Android targetのbuild errorにする |
| `plugin.path`, external plugin `signature`/`salt` | 非対応 | 明示指定したAndroid targetはbuild errorにする。runtime external plugin loadを行わず、NDK pluginはステップ5のbuild-time package対象とする |

`network`は、CEF用の安全設定を同じ`muon.json`へ残せるようにする例外である。Android buildの成功はそのpolicyがAndroidで有効になったことを意味しないため、明示設定時の警告と利用者向け文書を必須とする。

それ以外に、Androidだけで無効な値を含む共通`muon.json`を許す必要が生じた場合は、無言で無視するのではなくplatform override構文を別途設計する。ステップ4ではoverride構文そのものは実装しない。

## Android固有設計が必要な領域

別設計とした機能と、共通API内でAndroid固有の型が必要な機能は、次の境界ごとに必要性を判断する。ここではdesktop APIの別名を先に確定しない。

| 領域 | 必須となる設計要素 |
|---|---|
| Activityとtask | foreground/background遷移、background start制限、複数Activity、configuration change、呼び出し失敗の表現 |
| Window情報 | current bounds、display metrics、system bar insets、orientation、multi-window。位置とsizeの強制設定は含めない |
| Android menu | long-press、selection ActionMode、touch/keyboard入力、Activity lifecycle |
| Notificationとforeground service | notification permission、channel、service type、ユーザーによる停止、background execution制限 |
| Document URI | URI permissionの取得・永続化・解放、read/write mode、provider error、Activity再生成 |
| App update | 配布store、利用可能性、利用者同意、immediate/flexible update、store外配布時の非対応 |
| Runtime情報 | backendのdiscriminated union、Android OS/API level、ABI、application ID/version、WebView package/version |

## 実装状況と残作業

初期API実装は、次の順序で実施した。完了済みとした項目は、TypeScriptの公開契約、Java/JNIのnative route、WebViewからの計装テストまでを含む。

1. 完了: `getConfigValues()`, `getVariables()`, `getProcessId()`, `getRuntimeInfo()`, `reload()`, fullscreen、zoom、`close()`。
2. 完了: app-specific pathを対象とする`muon.fs` 22関数。
3. 完了: Android設定の受理、拒否、警告規則を表す検証プリミティブと、CEF/Androidを分岐できる`MuonRuntimeInfo`型。
4. 延期: `content://`を直接扱う`muon.fs`契約と、その後の`muon.fs.dialogs`。
5. 延期: nodejs-mobileを用いるsidecarと、必要性が確認されたAndroid向け別設計。
6. planのステップ5: build-time同梱NDKプラグインと`arm64-v8a`対応。

各実装では、関数単位のcapability、asset originとmain frameのRPC境界、cancel、Activity再生成を既存の共通RPCテスト条件へ追加する。

## ステップ4の完了条件

ステップ4は、次をすべて満たした場合に完了とする。

- 現行の組み込み公開関数68件をすべて対応表へ含めている。
- 各関数について、互換、制約付き互換、Android向け別設計、非対応、ステップ5のいずれかを決めている。
- 同名APIを公開しない場合のsimple modeとvalidate modeの挙動を決めている。
- Androidで維持するfilesystem path APIと、Storage Access FrameworkのDocument URIを分離している。
- launcher updater、Node.js sidecar、executor、runtime外部plugin loadをAndroid初期版へ含めないことを明示している。
- Android固有設計の対象と、planのステップ5に委ねるnative plugin境界を明示している。
- `muon.json`でAndroid targetが受理する値、制約付きで受理する値、拒否する値の方針を示している。
- Android公式資料と現在のMuon実装を判断根拠として参照している。

この文書では上記条件をすべて満たしている。したがって、ステップ4の成果物であるAPI対応方針の定義は完了している。加えて、「現在のAndroid実装状況」に列挙した初期API、非対応APIの非公開化、設定検証、runtime型の実装とテストも完了した。`content://`、`muon.fs.dialogs`、nodejs-mobile sidecar、`arm64-v8a`、NDKプラグインは明示した後続作業であり、今回の完了判定には含めない。
