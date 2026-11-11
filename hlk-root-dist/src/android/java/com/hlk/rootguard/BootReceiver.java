package com.hlk.rootguard;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.SystemClock;

/**
 * Decides at boot whether the probe should run again.
 *
 * The decision uses one fact only: was an attempt in flight when the phone
 * went down?  If so this boot is a recovery, not a fresh start, and the
 * failure counter grows.  Everything the user can do to stop us - the app
 * switch, the notification action, the DISABLE file - is checked by the
 * service itself as well.
 */
public class BootReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context ctx, Intent intent) {
        String act = intent.getAction();
        if (act == null)
            return;
        if (!Intent.ACTION_BOOT_COMPLETED.equals(act)
                && !Intent.ACTION_LOCKED_BOOT_COMPLETED.equals(act))
            return;

        boolean inFlight = Prefs.sp(ctx).getBoolean(Prefs.ATTEMPT, false);
        int fails = Prefs.fails(ctx);
        String why;

        if (inFlight) {
            fails++;
            why = "previous attempt took the device down (strike " + fails
                    + "/" + Prefs.MAX_FAILS + ")";
        } else {
            fails = 0;
            why = "clean boot";
        }

        boolean enabled = Prefs.enabled(ctx);
        if (fails >= Prefs.MAX_FAILS) {
            enabled = false;
            why += " -> autostart disabled, open the app to re-enable";
        }

        Prefs.sp(ctx).edit()
                .putBoolean(Prefs.ATTEMPT, false)
                .putInt(Prefs.FAILS, fails)
                .putBoolean(Prefs.ENABLED, enabled)
                .putString(Prefs.LAST_STATE, "boot: " + why)
                .commit();

        GuardService.note("boot uptime="
                + (SystemClock.elapsedRealtime() / 1000) + "s: " + why);

        if (!enabled) {
            GuardService.note("autostart off, not starting the probe");
            return;
        }
        // arm exactly this one launch; a later service restart finds it cleared
        GuardService.arm(ctx);
        Intent svc = new Intent(ctx, GuardService.class)
                .setAction(GuardService.ACTION_START);
        ctx.startForegroundService(svc);
    }
}
