package dev.muon.runtime;

/** Android component for the independent QuickJS service fixture. */
public final class MuonFixtureQuickJsService extends MuonNativeService {
    @Override protected String serviceId() { return "quickjs"; }
}
