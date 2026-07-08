# 制約

## WaylandとVulkan

現在のCEF (`147.0.14+g76d2442`) は、Linux+Wayland環境において、Vulkanとの併用を完全にサポートしていません。多くの場合、この組み合わせは問題を引き起こすとの事なので、muonではLinux表示バックエンドがWaylandと判定された場合に、自動的にCEFのVulkan関連機能を無効化します。
例えば、 `XDG_SESSION_TYPE=wayland` や `WAYLAND_DISPLAY` 、または `--ozone-platform=wayland` が検出された場合が対象です。
一方で、 `--ozone-platform=x11` のようにX11バックエンドが明示されている場合は、この無効化を行いません。

また、GL/ANGLEバックエンドが明示指定されていないWayland環境では、Vulkan経由のANGLEを避けるため、CEFがANGLEのOpenGLバックエンドを使用するように調整します。

## virtual moduleインポートのフィルタ機能

muonプラグインのvirtual moduleインポートのフィルタ機能は、サプライチェーン攻撃を完全に排除するものではないことに注意してください。
これは、実行時にmuonコードへ要求を発生させる場合に、Viteが生成したランダムなcapability IDを使用して、関数呼び出しをフィルタします。
このIDは推測されにくい値ですが、ロードされたbundleの動的解析や、同じページ上で実行されている悪意あるコードによってIDを特定された場合は、フィルタを突破される可能性があります。

一方で、ページURLのフィルタ（`validate`モードでは自動構成・`simple`モードでは手動構成）は、muonオブジェクトそのものをJavaScriptから参照できなくするため、強固に作用します。
したがって、不要なプラグイン関数を露出させないように常に注意してください。
