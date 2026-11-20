package dev.muon.runtime;

/** Deliberately lacks the permission needed for its camera foreground type. */
public final class MuonFixtureRejectedService extends MuonNativeService {
    @Override protected String serviceId() { return "rejected"; }
}
