package com.hlk.rootguard;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/**
 * adb control surface, so the switch can be flipped without touching the
 * phone screen (and so a broken UI can never lock us out):
 *
 *   adb shell am broadcast -n com.hlk.rootguard/.CtlReceiver \
 *       -a com.hlk.rootguard.CTL --es cmd disable
 *   ... --es cmd enable
 *   ... --es cmd selftest
 *   ... --es cmd run [--ei delay 0]
 *   ... --es cmd ksu          # replay SukiSU deploy, no probe re-arm
 *   ... --es cmd patch        # apply the Termux patch on demand (= 补丁 page)
 *   ... --es cmd patchcheck   # read-only Termux patch status
 *   ... --es cmd gate --es mode signal|time|either
 *                            # signal ~6.5 min (default); time = v16 25 min
 *
 * The component has to be named explicitly: an implicit broadcast is dropped
 * with "Background execution not allowed" once the app is in the background.
 *
 * This receiver is exported and unguarded on purpose: anyone with adb access
 * already owns the phone. Do not install this APK as a system-wide helper.
 */
public class CtlReceiver extends BroadcastReceiver {
    public static final String ACTION = "com.hlk.rootguard.CTL";

    @Override
    public void onReceive(Context ctx, Intent in) {
        String cmd = in.getStringExtra("cmd");
        if (cmd == null)
            cmd = "";
        GuardService.note("ctl: " + cmd);
        switch (cmd) {
            case "disable":
                Prefs.setEnabled(ctx, false);
                break;
            case "enable":
                Prefs.sp(ctx).edit()
                        .putBoolean(Prefs.ENABLED, true)
                        .putInt(Prefs.FAILS, 0)
                        .putBoolean("selftest", false)
                        .commit();
                break;
            case "ksu":
                // Replay the SukiSU deployment on a live root channel.
                // Does NOT arm or restart the probe (that would panic).
                KsuBootstrap.forceRun(ctx);
                break;
            case "patch":
            case "patchcheck": {
                // Same entry point as the "补丁" page, reachable from adb for
                // unattended verification.  goAsync keeps the receiver alive
                // while the root channel answers (up to 30 s).
                final PendingResult pr = goAsync();
                final boolean apply = cmd.equals("patch");
                final String tag = cmd;   // effectively-final copy for lambda
                new Thread(() -> {
                    try {
                        String res = apply
                                ? KsuBootstrap.applyTermuxPatch(ctx)
                                : KsuBootstrap.checkTermuxPatch(ctx);
                        GuardService.note("ctl: " + tag + " -> "
                                + (res == null ? "(null)"
                                        : res.replace('\n', ' ').trim()));
                    } catch (Throwable t) {
                        GuardService.note("ctl: " + tag + " error: " + t);
                    } finally {
                        pr.finish();
                    }
                }, "ctl-patch").start();
                break;
            }
            case "gate": {
                String m = in.getStringExtra("mode");
                if (m == null) m = "signal";
                if (!m.equals("signal") && !m.equals("time") &&
                        !m.equals("either")) {
                    GuardService.note("ctl: bad gate mode '" + m + "'");
                    break;
                }
                Prefs.sp(ctx).edit().putString(Prefs.GATE_MODE, m).commit();
                GuardService.note("ctl: gate mode = " + m +
                        " (takes effect on the next probe start)");
                break;
            }
            case "selftest":
                Prefs.sp(ctx).edit()
                        .putBoolean(Prefs.ENABLED, true)
                        .putBoolean("selftest", true)
                        .putInt(Prefs.DELAY_S, 0)
                        .putInt(Prefs.FAILS, 0)
                        .commit();
                GuardService.arm(ctx);
                ctx.startForegroundService(new Intent(ctx, GuardService.class)
                        .setAction(GuardService.ACTION_START));
                break;
            case "run": {
                // --ei delay N overrides the settle delay (0 = fire now);
                // without it the saved value stays.
                int d = in.getIntExtra("delay", -1);
                android.content.SharedPreferences.Editor e = Prefs.sp(ctx).edit()
                        .putBoolean(Prefs.ENABLED, true)
                        .putBoolean("selftest", false)
                        .putInt(Prefs.FAILS, 0);
                if (d >= 0)
                    e.putInt(Prefs.DELAY_S, Math.min(3600, d));
                e.commit();
                GuardService.arm(ctx);
                ctx.startForegroundService(new Intent(ctx, GuardService.class)
                        .setAction(GuardService.ACTION_START));
                break;
            }
            default:
                GuardService.note("ctl: unknown cmd '" + cmd + "'");
        }
    }
}
