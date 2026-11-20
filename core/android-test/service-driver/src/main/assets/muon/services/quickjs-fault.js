// Intentionally bypass the SDK to exercise engine failure containment: an
// interrupted private async task must also fail its never-settled public call.
globalThis.__muonServiceInterface = {
  schemaVersion: 1,
  methods: { spin: { parameters: [], returns: 'void', errors: [] } },
  events: {},
};
globalThis.__muonServiceRuntime = {
  start() {},
  invoke() {
    void (async () => {
      for (;;) {}
    })();
    return new Promise(() => {});
  },
  cancel() {},
  stop() {},
};
