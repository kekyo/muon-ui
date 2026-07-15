# Local asset permissions (Advanced topics)

The `asset://` URL scheme is treated as an independent trusted origin similar to `https://`, except that no certificate exists.
This can affect CORS and CSRF, so consider it when designing strict boundaries.

Local assets must be placed under some subdirectory.
By default, assets are placed under a `main/` subdirectory, such as `assets/main/`.

The name `main` corresponds to the domain-name part of the URL, as in `asset://main/`.
If you split directories such as `assets/sub/`, you can make them behave as different origins.
(Note: The word `main` itself has no special meaning.)

By combining this feature with the CORS/CSRF controls described above, `plugin.pages`, and `browser.allowUnsafeJavaScriptParentAccess`, you can finely tune page permissions on muon.
The following is the default definition:

```json
{
  "plugin": {
    // Use validate mode by default.
    "mode": "validate",
    // Make muon plugin functions available only to pages under `asset://main/`.
    // For example, pages under `asset://sub/` cannot reference the muon plugin bridge.
    "pages": ["asset://main/**"]
  }
}
```
