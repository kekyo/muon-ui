# Accessing external networks

As explained earlier, muon's various features become available through a whitelist model.
Network access, which is one of the most important parts of CEF, is also filtered according to a whitelist.

Add an allow list to `muon.json` as follows to access external networks:

```json
{
  "network": {
    "allow": [
      "asset://main/**"
    ]
  },
}
```

Adding URLs to `network.allow` allows access to those URLs.
When omitted, the default is `["asset://**", "data:image/**"]`, which allows access to all local assets and inline image data.
Specifying `network.allow` replaces this default list rather than extending it.

For example, if you intentionally set an empty list (`network.allow: []`), all network access, including local assets, is disabled and nothing can be displayed.
However, if you try this and launch the Vite server and muon with `npm run dev`, the app will still display correctly.
This is because the Vite server URL is temporarily added to the `network.allow` list when `npm run dev` is used.
Be careful: building with an empty list generates an invalid muon app.

- Note: Inline data URLs such as `data:...` are also subject to `network.allow`.
  The default allows only image data through `data:image/**`. If you explicitly configure `network.allow` and still need inline images, include this pattern in the configured list.

If your page references an external server, for example when only image data in an `<img>` tag references external `https://img.example.com/images/...`, add valid URLs like this:

```json
{
  "network": {
    "allow": [
      "asset://main/**",
      "https://img.example.com/images/**"
    ]
  },
}
```

This allows only local assets and that external server to be referenced.

- This applies not only to images, but to all network access, including CSS files, `iframe` tags, `fetch` API access, and WebSocket connections.
  You must add every required URL.
- These URLs can use a pseudo-glob format. `*` does not cross `:`, `/`, `?`, or `#` separators, while `**` matches all following characters.

## Accessing the local network

Chromium requests Local Network Access permission for requests to loopback or local network endpoints.
Allow the destination URL with `network.allow`, and specify the requesting origin with `network.localAccess`.

```json
{
  "network": {
    "allow": [
      "asset://main/**",
      "http://localhost:5171/**"
    ],
    "localAccess": {
      "loopbackOrigins": [
        { "scheme": "asset", "domain": "main" }
      ],
      "localNetworkOrigins": []
    }
  }
}
```

Use `loopbackOrigins` for localhost and loopback addresses, and `localNetworkOrigins` for hosts on the LAN.
Both lists use exact requesting-origin matching and do not accept wildcards.
Permission requests from unconfigured origins are explicitly denied.

`network.localAccess.allowInsecureLocalhost` is independent from these permissions and from `network.allow`.
It only changes invalid HTTPS certificate handling for localhost.
