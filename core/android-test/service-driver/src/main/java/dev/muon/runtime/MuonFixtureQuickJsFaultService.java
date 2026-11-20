package dev.muon.runtime;

/** Android component for a heap whose private async task never returns. */
public final class MuonFixtureQuickJsFaultService extends MuonNativeService {
    @Override protected String serviceId() { return "quickjs-fault"; }
}
