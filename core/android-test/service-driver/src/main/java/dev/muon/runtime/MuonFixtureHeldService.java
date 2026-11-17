package dev.muon.runtime;

/** Foreground component whose native initialization is held by a test barrier. */
public final class MuonFixtureHeldService extends MuonNativeService {
    @Override protected String serviceId() { return "held"; }
}
