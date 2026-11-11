package com.hlk.rootguard;

import android.content.Context;
import android.content.SharedPreferences;

/**
 * Every piece of state that has to survive a reboot lives here.
 *
 * The important one is "attempt": it is set right before the probe is
 * started and cleared once the probe reports root.  If the device comes back
 * up with attempt still set, the previous run ended in a panic/reboot, so the
 * failure counter goes up and after MAX_FAILS the auto-start disables itself.
 * That is the bootloop brake -- without it a crashing probe would keep
 * rebooting the phone forever.
 */
final class Prefs {
    static final String NAME = "guard";

    static final String ENABLED = "enabled";        // user switch
    static final String DELAY_S = "delay_s";        // settle time after boot
    static final String ATTEMPT = "attempt";        // 1 = run in flight
    static final String FAILS = "fails";            // consecutive aborts
    static final String LAST_PID = "last_pid";
    static final String LAST_STATE = "last_state";  // human readable
    /** "signal" (default) | "time" (v16 rollback) | "either" (no floor). */
    static final String GATE_MODE = "gate_mode";
    /**
     * One-shot permission to fire the chain.  Set by an explicit user action
     * (boot receiver, Start button, adb "run") and consumed right before the
     * probe is exec'd.
     *
     * This exists because of a real accident: the service used to return
     * START_STICKY, so Android recreated it after the package was updated,
     * the saved "enabled" pref was still true, the settle delay was 0, and the
     * exploit fired with nobody asking - on a system that was not settled.
     * It panicked.  Without an arming flag every restart is a loaded gun.
     */
    static final String ARMED = "armed";

    static final int DEFAULT_DELAY_S = 300;
    static final int MAX_FAILS = 2;

    private Prefs() {
    }

    static SharedPreferences sp(Context c) {
        return c.getSharedPreferences(NAME, Context.MODE_PRIVATE);
    }

    static boolean enabled(Context c) {
        return sp(c).getBoolean(ENABLED, false);
    }

    static void setEnabled(Context c, boolean on) {
        sp(c).edit().putBoolean(ENABLED, on).commit();
    }

    static int delayS(Context c) {
        return sp(c).getInt(DELAY_S, DEFAULT_DELAY_S);
    }

    static int fails(Context c) {
        return sp(c).getInt(FAILS, 0);
    }
}
