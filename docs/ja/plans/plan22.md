# GitHub Actions のサービス・配布物テストと依存パッケージ取得の修正計画

## 対象と優先順位

対象は `0.36.0`（`99d8f35e`）の CI である。develop の #89 では UI テスト6件と Linux のサービスホストテスト1件が失敗した。同じコミットの main の #88 では UI の6件が失敗し、Linux テストを通過した後に Windows 実機テストが失敗した。まずこの3種類の失敗を個別に直し、次に Linux 配布物の検査と Ubuntu パッケージの取得を直す。各段階で修正前の失敗と修正後の結果を記録し、その段階の変更をコミットする。[#89 のログ](https://github.com/kekyo/muon-ui/actions/runs/45413240665/job/138512621385)・[#88 のログ](https://github.com/kekyo/muon-ui/actions/runs/45413178582/job/138512188549)

| 過去の実行 | 今回の扱い |
| --- | --- |
| #78〜#83、#85、#87、#88 | UI テストでサービスホストが見つからない。#87 は中断前にこの失敗を記録した。#81 では Linux の接続エラーも再現しており、どちらも glibc 2.34 対応より前から発生している。段階1・2で扱う。[#78](https://github.com/kekyo/muon-ui/actions/runs/44538268925)・[#81](https://github.com/kekyo/muon-ui/actions/runs/44817953813/job/136571080479#step:13:7146)・[#87](https://github.com/kekyo/muon-ui/actions/runs/45403174673) |
| #76、#78〜#80、#82、#83、#85、#88 | Windows 実機の接続先 `win11` が未設定のときに失敗する。段階3で扱う。[#76](https://github.com/kekyo/muon-ui/actions/runs/43995368222)・[#88 の失敗箇所](https://github.com/kekyo/muon-ui/actions/runs/45413178582/job/138512188549#step:13:7252) |
| #73 | Linux 配布物のリンク検査で `libcef.so` が見つからない。現行の配布物でも同じ結果を確認した。段階4で扱う。[#73 の失敗箇所](https://github.com/kekyo/muon-ui/actions/runs/39940065829/job/120369020063#step:14:6042) |
| #74、#75、#77、#84 | Ubuntu の依存パッケージの取得中にジョブの上限90分に達した。段階5で扱う。[#77](https://github.com/kekyo/muon-ui/actions/runs/44410255828)・[#84](https://github.com/kekyo/muon-ui/actions/runs/45329781330) |

#72 で失敗した GTK のモーダルダイアログのテストは #89 で成功した。最終段階で既存の動画テストを再実行する。#78 で不足していたページ用 Node.js プラグイン `node.so` はその後削除されており、現行の Node.js サービスを段階1・2で検証する。#71・#86・#87 のキャンセルは後続の push を優先するワークフロー設定どおりの動作である。[#72 の失敗箇所](https://github.com/kekyo/muon-ui/actions/runs/39901832110/job/120245222690#step:13:5904)・[プラグインを削除した変更](https://github.com/kekyo/muon-ui/commit/e6d5c458)・[CI の同時実行設定](../../../.github/workflows/ci.yml#L11-L13)

## 段階1　UI テストが使う Linux サービスホストを先にビルドする

サービスホストは UI とは別のプロセスでサービスを実行する。UI の5つのテストファイルが `core/service-host/.build` の生成物を使い、そのうち3つが各自の `beforeAll` でホストをビルドしている。残る2つは並列実行中にコピーを始めるため、ビルドが間に合わない。ブラウザーエンジンの CEF（Chromium Embedded Framework）を検証するワークスペースは UI テストの後でホストをビルドする。[コピーする箇所](../../../ui/cef-test/dev-service-updates.test.ts#L284-L288)・[セッションを検証するテスト](../../../ui/cef-test/development-service-session.test.ts#L82-L86)・[既存のビルド処理](../../../ui/cef-test/dev-services.test.ts#L38-L47)

1. 一時チェックアウトで CI と同じ SDK・Node.js ブリッジ・ネイティブ依存を準備し、サービスホストの `.build` がない状態で次のコマンドを実行する。調査時は6件すべてが `ENOENT` で失敗した。`make -C core/service-host .build/muon-service-host` を実行して再試験すると6件すべてが成功した。

   ```sh
   npm exec --workspace muon-ui -- vitest run cef-test/dev-service-updates.test.ts cef-test/development-service-session.test.ts
   ```

2. この2ファイルと `dev-services.test.ts`、`development-host.test.ts`、`service-package.test.ts` を同じ Vitest プロジェクトにまとめる。対象のテストを選んだときに `globalSetup` で既存の `driver-build.lock` を取り、ホストの Make ターゲットを1回実行する。このターゲットは必要な `libcardio.so` も用意する。3ファイルの `beforeAll` にある重複したビルド処理を取り除く。[Make ターゲット](../../../core/service-host/Makefile#L73-L80)・[CEF テストでの設定例](../../../core/cef-test/vite.config.ts#L53-L75)・[Vitest の globalSetup](https://vitest.dev/config/globalsetup)

- [ ] ホスト未生成の状態から2ファイルをそれぞれ単独で実行しても、5ファイルを並列で実行しても、サービスの起動・更新・終了を確認できる。変更前後の `vitest list --filesOnly` を照合し、各ファイルが1回ずつ選ばれることを確認する。ホストを使わない UI テストを単独で選んだときは、ホスト未生成のままテストが終了する。

## 段階2　サービスの停止が完了してからポートを検査する

最後の参照に対する `release()` は停止を要求する。停止完了は `watchState()` の `stopped` 通知で観測できる。現在のドライバーは参照の解放と既存 WebSocket の切断を待った直後に HTTP 接続を試す。#81 と #89 では期待した `ECONNREFUSED`（接続拒否）に対して `ECONNRESET`（接続のリセット）を受けた。ログで確認できるのは接続結果までなので、その瞬間のサービスの状態は再現試験で確認する。[失敗した判定](../../../core/service-host/server-driver.ts#L59-L67)・[参照を解放する API の仕様](../../../core/common/renderer/services.ts#L68-L76)・[#89 のエラー詳細](https://github.com/kekyo/muon-ui/actions/runs/45413240665/job/138512621385#step:13:7153)

1. テスト用サービスの停止処理で既存 WebSocket を先に閉じ、テスト側が合図するまで待ち受けポートを開いておく。サービスが `stopping` の間に新規接続を受け付けると、現行ドライバーの接続拒否の判定が失敗する。この順序を既存の Node.js テストで使っている通知と応答で再現する。失敗時はサービス ID、その起動を識別する世代番号、状態、接続結果を記録する。[停止を待たせる既存テスト](../../../core/service-host/node-driver.ts#L177-L197)
2. ドライバーは最後の参照を解放する前に対象の世代を監視する。その世代の `stopped` と WebSocket の切断を両方確認してから、新規接続が `ECONNREFUSED` になることを検証する。監視はテストが失敗した場合も終了する。`stopped` の通知後も結果が異なる場合は、ホストがリスナーを閉じる順序と停止完了を通知する順序を調べ、実装側で再現する箇所を修正する。[停止完了の通知](../../../core/common/src/services/muon_service_controller.cpp#L54-L61)

- [ ] native・QuickJS・Node.js の各サービスで、旧世代のポートへの新規接続が拒否され、別のサービスは応答を続ける。同じサービスを再取得すると新しい世代が応答する。Linux の `linux-service-host/service-node.test.ts` で確認し、共通ドライバーを使う Windows 版も段階3で実行する。

## 段階3　Windows のコンパイルと実機での試験を分ける

Windows 実機には `agent-rover` を使って接続する。接続を確立できないときは理由を表示して実機テストをスキップするのがプロジェクトの方針である。#88 で停止した `service-host-windows.test.ts` は未接続を例外に変える。後続の `services-windows.test.ts` にも同じ分岐がある。実機がない CI でもサービスホストの Windows ビルドを実行すれば、コンパイルエラーを検出できる。[ビルドガイド](../muon-ui-self-build-advanced-topic.md#L78-L87)・[サービスホストのテスト](../../../core/cef-test/service-host-windows.test.ts#L29-L39)・[後続の実機テスト](../../../core/cef-test/services-windows.test.ts#L55-L75)

1. `windows-e2e-unconfigured.test.ts` の既存ケースに両ファイルを追加する。未登録の接続先を指定し、実機テストが準備を始めるか例外を返す現象を再現する。[未設定環境の回帰テスト](../../../core/cef-test/windows-e2e-unconfigured.test.ts#L41-L87)
2. 通常のテスト経路に、SDK とネイティブ依存を準備してから `make -C core/service-host windows-i686 windows-amd64` を実行する工程を置く。どちらのコンパイル失敗も CI の失敗として報告する。実機用の2ファイルは既存の接続ヘルパーの結果で実行かスキップかを決める。未設定の場合はローカルでの実機用資材の作成に入る前に理由を記録する。[既存の接続判定](../../../scripts/windows-e2e-environment.ts#L50-L72)・[他の Windows テストでの使い方](../../../core/cef-test/server-windows.test.ts#L23-L30)
3. 接続できた場合は利用確認用のセッションをいったん解放し、ローカルの準備を終えてから実機を確保する。使用中の実機は既存の再試行処理で待つ。転送・起動・検証の失敗をテスト結果に反映し、終了時には実機を解放する。

- [ ] 実機の接続先が未設定の CI で両アーキテクチャのコンパイルが成功し、対象の実機テストだけが理由付きでスキップされる。`win11` を設定した環境では `test:windows-services` と `test:windows-services-ui` を実行し、i686・amd64 の両方でホストと UI からサービスを利用できることを確認する。

## 段階4　CEF を配置した Linux 配布物で GTK のリンクを確認する

npm パッケージには `muon-core` などを収録し、ブラウザーエンジンの `libcef.so` は利用時に配置する。現在の CI は npm パッケージ用の `ui/dist/runtime/linux-amd64` をそのまま `linux-gtk-runtime.test.ts` に渡す。現行の `muon-core` に対しても `LD_LIBRARY_PATH` と `LD_PRELOAD` を外して `ldd` を実行すると、#73 と同じ `libcef.so => not found` になる。[CI の呼び出し](../../../.github/workflows/ci.yml#L173-L185)・[npm パッケージの収録条件](../../../ui/scripts/verify-package-runtime.mjs#L170-L178)・[GTK のリンク検査](../../../core/cef-test/linux-gtk-runtime.test.ts#L22-L49)

1. 現行の CI の入力で `linux-gtk-runtime.test.ts` を実行し、リンク検査の失敗を記録する。同じコミットで `npm pack` したアーカイブを一時プロジェクトにインストールする。
2. インストールした CLI の `muon prepare --target linux-amd64 --muon-path <パッケージ内のランタイム> --stage-dir <一時ディレクトリ> --json` を使う。結果の `stagePath` を `MUON_TEST_LINUX_RUNTIME` に渡す。パッケージの CEF 別配布の検査と、準備後の実行環境に必要なライブラリがすべて解決するかの検査をそれぞれ実行する。[prepare コマンド](../../../ui/common/cli.ts#L798-L813)・[独立した npm プロジェクトでの起動テスト](../../../core/cef-test/packaged-linux-startup.test.ts#L514-L546)

- [ ] 準備したディレクトリで `muon-core` と `libmuon-ui.so` のリンク検査が成功する。CEF の解決先は準備したランタイム、GTK・GDK・GLib などの解決先はシステムのライブラリである。既存のテストでファイル選択・画像デコード・入力メソッドと、インストールしたパッケージからの起動を確認する。パッケージ本体には引き続き CEF の別配布と Linux の glibc 下限の検査を適用する。

## 段階5　Ubuntu パッケージの取得が停滞したときに回復する

`Install build dependencies` の所要時間は #85 が3分3秒、#89 が1分29秒だった。#84 は取得中にジョブの90分を使い切った。APT の索引更新とダウンロードに時間枠を設け、ミラーから取得できない場合に別の Ubuntu 公式ミラーで1回再試行する。初案は索引更新とダウンロードを合わせて1回5分とし、実装時にキャッシュのないランナーで所要時間を確認する。[#85 の実行](https://github.com/kekyo/muon-ui/actions/runs/45370271494/job/138362459608)・[#84 の実行](https://github.com/kekyo/muon-ui/actions/runs/45329781330/job/138227934258)・[Ubuntu の apt-get](https://manpages.ubuntu.com/manpages/noble/man8/apt-get.8.html)

1. 使い捨ての Ubuntu 24.04 環境で最初の取得先を応答待ちにし、現行の手順では次の取得先に進めないことを確認する。試験用の待ち時間は短く設定し、実際のミラー障害を待たずに再現する。
2. `apt-get --download-only` で必要なパッケージをそろえてからインストールする。再試行は取得失敗と取得期限の超過に限る。Ubuntu の取得先を切り替える際は対象リリース、更新・セキュリティ用の配布元、署名検証、amd64・i386 の指定を引き継ぐ。ログには取得先、経過時間、失敗理由を残す。

- [ ] 通常の取得と初回の停滞からの回復を試し、どちらも Wine 32 ビット版を含む既存の依存をインストールしてビルドへ進める。両方の取得先が失敗した場合は取得ステップで終了する。取得用のタイマーは `dpkg` によるインストールの前に解除する。

## 段階6　Android の検証が終わるまでビルドジョブを実行する

#90 では依存の取得、全体テスト、Linux 配布物の検査が成功した。Android 試験の開始までに76分19秒かかり、2回目の npm 利用者向け APK を検証中にジョブ全体の90分上限に達した。直近で完走した #70 では Android の工程に19分41秒かかった。この2つの実測を合わせると96分になる。[#90 の打ち切り箇所](https://github.com/kekyo/muon-ui/actions/runs/45505770586/job/138840485194#step:17:1)・[#70 の Android 試験](https://github.com/kekyo/muon-ui/actions/runs/39625481843/job/119309542704#step:17:1)

1. `Build and test` ジョブの上限を120分に延ばす。APT ミラーからの取得は段階5のとおり1回5分とする。
2. 修正コミットで CI を実行し、2回の APK 検証と独立 SDK の試験を含む Android の工程が完了したことをジョブログで確認する。

- [ ] ビルドジョブが上限内にすべての工程を終え、CodeQL を含む4ジョブが成功する。

## 全体の完了条件

- [ ] 修正コミットのクリーンなチェックアウトでルートの `npm test` を全件実行する。3分を超える試験はログに保存し、タイマーで終了を確認する。修正前後のテスト結果を見比べ、意図した実機スキップと各段階の追加テストを説明できるようにする。
- [ ] 同じコミットの GitHub Actions で `Build and test` と CodeQL の3ジョブが成功する。#89 で未実行だった独立 SDK、Linux 配布物・AppImage、Android エミュレーターと npm 利用者向けのテストまで到達し、各ステップの終了結果を確認する。[CI の後半のステップ](../../../.github/workflows/ci.yml#L167-L221)
- [ ] #72 の GTK モーダルテストを既存の X11 環境で実行し、ダイアログ表示中に親画面を操作できないことと、他のウィンドウが手前に来たときの表示を動画で確認する。再現した場合は失敗フレームとウィンドウの状態を対応付け、原因を特定して個別に修正する。[GTK の動画テスト](../../../core/cef-test/plugin-e2e/native-dialog.ts#L1206-L1425)
- [ ] Windows 実機での2種類のサービス試験、全体テストの結果、CI の実行 URL を修正コミットに対応付けて記録する。途中で別の失敗が出た場合はログと再現条件から該当する段階を見直す。
