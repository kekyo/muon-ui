package dev.muon.runtime;

/** Real dataSync foreground service subject to the OS execution deadline. */
public final class MuonFixtureTimedService extends MuonNativeService {
    @Override protected String serviceId() { return "timed"; }
}
