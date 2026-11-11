package com.hlk.rootguard;

import android.app.job.JobParameters;
import android.app.job.JobService;
import android.content.Intent;
import android.os.SystemClock;

import java.io.BufferedReader;
import java.io.FileReader;

/**
 * Boot trigger that does not depend on BOOT_COMPLETED delivery.
 *
 * EMUI/iAware silently drops the boot broadcast for this app (verified
 * 2026-11-04: the receiver never runs even with the user's auto-start
 * toggle on).  A *persisted* JobScheduler job is a different animal: the
 * system re-registers it at boot inside system_server and runs it without
 * asking our app, so the block on manifest receivers does not apply.
 *
 * MainActivity schedules it as a 15-minute periodic job after every launch
 * (persisted jobs are cancelled by an app update, so each install needs one
 * open-to-reschedule - the UI flow does that anyway).
 *
 * Safety: the job can fire every 15 minutes all day, so "arm + start" is
 * gated four ways - the user switch, the MAX_FAILS brake, no attempt in
 * flight, and a once-per-boot guard keyed on the kernel's boot_id.
 */
public class BootJobService extends JobService {
    private static final String FIRED_BOOT_ID = "fired_boot_id";

    @Override
    public boolean onStartJob(JobParameters params) {
        try {
            decide();
        } catch (Throwable t) {
            GuardService.note("boot job error: " + t);
        }
        jobFinished(params, false);
        return false;
    }

    @Override
    public boolean onStopJob(JobParameters params) {
        return false;
    }

    private void decide() {
        if (!Prefs.enabled(this)) {
            GuardService.note("boot job: switch off");
            return;
        }
        if (Prefs.fails(this) >= Prefs.MAX_FAILS) {
            GuardService.note("boot job: brake on ("
                    + Prefs.fails(this) + "/" + Prefs.MAX_FAILS + ")");
            return;
        }
        if (Prefs.sp(this).getBoolean(Prefs.ATTEMPT, false)) {
            GuardService.note("boot job: attempt in flight");
            return;
        }
        String bootId = bootId();
        String fired = Prefs.sp(this).getString(FIRED_BOOT_ID, "");
        if (bootId != null && bootId.equals(fired)) {
            GuardService.note("boot job: already fired this boot");
            return;
        }

        boolean wasInFlight = Prefs.sp(this).getBoolean(Prefs.ATTEMPT, false);
        int fails = Prefs.fails(this) + (wasInFlight ? 1 : 0);
        String why = "boot job: clean start";
        if (wasInFlight) {
            why = "boot job: previous attempt took the device down (strike "
                    + fails + "/" + Prefs.MAX_FAILS + ")";
        }
        if (fails >= Prefs.MAX_FAILS) {
            Prefs.sp(this).edit()
                    .putBoolean(Prefs.ENABLED, false)
                    .putInt(Prefs.FAILS, fails)
                    .putString(Prefs.LAST_STATE, why + " -> disabled")
                    .commit();
            GuardService.note(why + " -> autostart disabled");
            return;
        }
        Prefs.sp(this).edit()
                .putBoolean(Prefs.ATTEMPT, false)
                .putInt(Prefs.FAILS, fails)
                .putString(Prefs.LAST_STATE, why)
                .commit();
        if (bootId != null) {
            Prefs.sp(this).edit().putString(FIRED_BOOT_ID, bootId).commit();
        }
        GuardService.note(why + ", arming");
        GuardService.arm(this);
        Intent svc = new Intent(this, GuardService.class)
                .setAction(GuardService.ACTION_START);
        startForegroundService(svc);
    }

    /** Kernel boot id - changes on every boot, stable within one.  Returns
     *  null when unreadable, which disables the once-per-boot guard. */
    private static String bootId() {
        try (BufferedReader r =
                     new BufferedReader(new FileReader(
                             "/proc/sys/kernel/random/boot_id"), 64)) {
            String s = r.readLine();
            return s == null ? null : s.trim();
        } catch (Exception e) {
            return null;
        }
    }
}
