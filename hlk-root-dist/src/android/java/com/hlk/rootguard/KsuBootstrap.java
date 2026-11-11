package com.hlk.rootguard;

import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Second stage of the boot chain: turn the raw uid-0 channel into a working
 * SukiSU Ultra shim with NO PC and NO manual insmod.
 *
 * Triggered exactly once, from GuardService.pumpOutput() when the probe
 * reports PHASE_I OK.  Everything here runs on its own thread: the stdout
 * pump must never be blocked, or the probe's stdout buffer fills up and the
 * exploit stalls.
 *
 * Hard rule learned on this device: u:r:kernel:s0 cannot reach
 * servicemanager ("Can't find service: package"), so pm/am/dumpsys only
 * work after setcon to u:r:shell:s0.
 *
 * 2026-11-10: the official Manager APK is NO LONGER bundled here (it used
 * to be a 17 MB asset installed by a detached `pm install`).  Root does
 * not depend on it.  linkManagerOnce() links a separately installed Manager
 * to the shim exactly once per boot; opening the app re-checks it.
 *
 * 2026-11-10 (Patches page): the Termux patch is now ON DEMAND ONLY.  The
 * 5 s / 60 s watchdogs for Termux and Manager were removed, so no thread
 * stays resident for monitoring.  applyTermuxPatch() / checkTermuxPatch()
 * are called solely from the "补丁" page (or CtlReceiver --es cmd patch).
 * The boot chain deliberately does NOT touch Termux at all.
 *
 * A hard off-switch (/sdcard/HLK_KSU_OFF) disables this stage completely.
 */
final class KsuBootstrap {
    static final String MGR = "com.sukisu.ultra";
    private static final String PROC = "/proc/hlksu_fd9";
    private static final String OFF_FILE = "/sdcard/HLK_KSU_OFF";
    private static final String[] ASSETS = {
            "hlk_ksu_shim9.ko", "hlk_su", "ksu_bootstrap.sh",
            "hlk_sucompat.ko", "su.sh",
            "termux_su", "termux_tsu", "grant_su",
            "hlk_termux_setup.sh"
    };

    private static final String TERMUX = "com.termux";
    private static final AtomicBoolean sStarted = new AtomicBoolean(false);
    private static volatile String sStatus = "idle";

    static String status() {
        return sStatus;
    }

    static void onRoot(Context c) {
        if (!sStarted.compareAndSet(false, true))
            return;
        final Context app = c.getApplicationContext();
        new Thread(() -> run(app), "ksu-bootstrap").start();
    }

    /**
     * Manual re-run from CtlReceiver ("--es cmd ksu").  Never re-arms or
     * restarts the probe: it only replays the SukiSU deployment against the
     * already-live root channel.  Safe to use after a manual Manager install
     * or to verify the bootstrap without rebooting the phone.
     */
    static void forceRun(Context c) {
        sStarted.set(false);
        onRoot(c);
    }

    private static void note(String s) {
        GuardService.note("ksu: " + s);
    }

    private static void sleep(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException ignored) {
        }
    }

    private static String oneLine(String s) {
        return s == null ? "(no answer)" : s.replace('\n', ' ').trim();
    }

    /** True once the shim created its /proc node: the module is really loaded. */
    private static boolean procUp(String path) {
        try {
            return new File(path).exists();
        } catch (Throwable t) {
            return false;
        }
    }

    /**
     * Patch readiness must NOT depend on GuardService.sRoot: CtlReceiver may
     * run in a brand-new process where that static flag is still false even
     * though the shim and the root daemon are live.  The procfs node is a
     * process-independent signal that the chain finished and insmod worked.
     */
    private static boolean channelReady() {
        return GuardService.root() || procUp(PROC);
    }

    /** Read the shim state without the root channel (procfs is world-readable). */
    private static String readProc(String path) {
        java.io.FileInputStream in = null;
        try {
            in = new java.io.FileInputStream(path);
            byte[] b = new byte[1024];
            int n = in.read(b);
            return n > 0 ? new String(b, 0, n) : "";
        } catch (Throwable t) {
            return null;
        } finally {
            if (in != null) {
                try {
                    in.close();
                } catch (Throwable ignored) {
                }
            }
        }
    }

    /**
     * Builds <= 1.3 unpacked a 17 MB Manager APK plus its detached installer
     * into files/ksu/.  They are no longer shipped, so drop the leftovers on
     * the first boot with the slim APK -- otherwise the app would still hold
     * 17 MB of dead data after the APK itself shrank.
     */
    private static void pruneStaleAssets(File dir) {
        String[] stale = {"ksu_manager.apk", "ksu_install_mgr.sh"};
        for (String n : stale) {
            File f = new File(dir, n);
            if (f.exists() && f.delete())
                note("removed stale asset " + n);
        }
    }

    /** Unpack the SukiSU payload from assets into our own files dir. */
    private static File extract(Context c) {
        File dir = new File(c.getFilesDir(), "ksu");
        //noinspection ResultOfMethodCallIgnored
        dir.mkdirs();
        pruneStaleAssets(dir);
        int ok = 0;
        for (String n : ASSETS) {
            File out = new File(dir, n);
            try (InputStream in = c.getAssets().open("ksu/" + n);
                 OutputStream os = new FileOutputStream(out)) {
                byte[] b = new byte[1 << 16];
                int r;
                while ((r = in.read(b)) > 0)
                    os.write(b, 0, r);
                //noinspection ResultOfMethodCallIgnored
                out.setExecutable(true, false);
                ok++;
            } catch (Exception e) {
                note("asset " + n + " missing: " + e.getMessage());
            }
        }
        note("assets " + ok + "/" + ASSETS.length + " in " + dir);
        return ok == ASSETS.length ? dir : null;
    }

    /** Java-side PackageManager lookup: no servicemanager access needed. */
    private static int packageUid(Context c, String pkg) {
        try {
            ApplicationInfo ai = c.getPackageManager().getApplicationInfo(pkg, 0);
            return ai.uid;
        } catch (PackageManager.NameNotFoundException e) {
            return -1;
        }
    }

    private static int managerUid(Context c) {
        return packageUid(c, MGR);
    }

    // ------------------------------------------------------------------
    // On-demand patches (Patches page).  NO resident threads any more.
    // ------------------------------------------------------------------

    /**
     * Apply the Termux patch exactly once.  Called ONLY when the user taps
     * "应用补丁" on the Patches page (or via CtlReceiver --es cmd patch).
     * Re-extracts assets first so a newly installed APK always deploys its
     * own front-ends, resolves Termux's uid at runtime (a factory reset
     * changes it), then runs hlk_termux_setup.sh through the root channel.
     */
    static String applyTermuxPatch(Context c) {
        if (!channelReady())
            return "ERROR: root 通道未就绪。\n"
                    + "请等开机链完成（主页显示「ROOT运行中」）后再应用补丁。";
        final Context app = c.getApplicationContext();
        File dir = extract(app);
        if (dir == null)
            return "ERROR: SukiSU assets are incomplete.";
        int uid = packageUid(app, TERMUX);
        if (uid <= 0)
            return "ERROR: Termux (com.termux) 尚未安装。\n"
                    + "请先安装并打开一次 Termux，再回到本页应用补丁。";
        String p = dir.getAbsolutePath();
        String out = GuardService.runAsRoot(app,
                "sh " + p + "/hlk_termux_setup.sh " + uid + " " + p, 30000);
        if (out == null)
            return "ERROR: root 通道没有响应。\n"
                    + "请等开机链完成（主页显示「ROOT运行中」）后再试。";
        if (out.contains("TERMUX-SETUP-OK")) {
            sStatus = "ready (patch applied)";
            note("Termux patch applied on demand (uid=" + uid + ")");
        }
        return out.trim();
    }

    /** Read-only status for the Patches page; never modifies the device. */
    static String checkTermuxPatch(Context c) {
        // Fast fail: without root the channel call would block for its whole
        // timeout and leave the Patches page stuck on "检查中…".
        if (!channelReady())
            return "root 通道未就绪\n"
                    + "请等开机链完成（主页显示「ROOT运行中」）后再查看。\n"
                    + "PATCH_CHECK_NEED";
        final Context app = c.getApplicationContext();
        File dir = extract(app);
        if (dir == null)
            return "ERROR: SukiSU assets are incomplete.";
        String p = dir.getAbsolutePath();
        String out = GuardService.runAsRoot(app,
                "sh " + p + "/hlk_termux_setup.sh check " + p, 8000);
        return out == null
                ? "root 通道没有响应，请稍后重试。\nPATCH_CHECK_NEED"
                : out.trim();
    }

    /**
     * One-shot Manager link.  Replaces the old 10 s / 60 s polling thread:
     * checked once when the chain finishes and once whenever MainActivity is
     * opened (ensureUid).  No force-stop: the Manager picks up the driver fd
     * on its next launch.
     */
    private static void linkManagerOnce(Context c) {
        final Context app = c.getApplicationContext();
        new Thread(() -> {
            try {
                int uid = managerUid(app);
                if (uid > 0) {
                    writeUid(app, uid);
                    note("Manager linked (uid=" + uid + ")");
                }
            } catch (Throwable ignored) {
            }
        }, "ksu-manager-once").start();
    }

    /** Tell the shim which uid owns the Manager process.  Idempotent. */
    private static void writeUid(Context c, int uid) {
        String cmd = "echo u " + uid + " > " + PROC + "; head -4 " + PROC;
        note("mgr_uid=" + uid + " -> "
                + oneLine(GuardService.runAsRoot(c, cmd, 10000)));
        // rootout may be unreadable (see procUp); confirm through procfs.
        String want = "mgr_uid=" + uid;
        for (int i = 0; i < 10; i++) {
            String st = readProc(PROC);
            if (st == null || st.contains(want))
                return;
            sleep(500);
        }
        note("WARN: shim still reports a different mgr_uid");
    }

    private static void run(Context c) {
        try {
            if (new File(OFF_FILE).exists()) {
                sStatus = "off";
                note("disabled by " + OFF_FILE);
                return;
            }

            sStatus = "extracting";
            File dir = extract(c);
            if (dir == null) {
                sStatus = "assets-missing";
                return;
            }

            // rootd may still be finishing stage_root_channel()
            File rootup = new File(GuardService.inbox(c), "rootup");
            String id = null;
            for (int i = 0; i < 20; i++) {
                id = GuardService.runAsRoot(c, "id", 5000);
                if (id != null && id.contains("uid=0"))
                    break;
                // the daemon drops this marker into every inbox it serves, so
                // its presence means the channel is up even if we cannot read
                // the answer back
                if (rootup.exists() && i >= 3)
                    break;
                sleep(2000);
            }
            if ((id == null || !id.contains("uid=0")) && !rootup.exists()) {
                sStatus = "no-root-channel";
                note("root channel never answered, giving up");
                return;
            }

            // Pure kernel-domain work: stage the .ko and insmod it.
            sStatus = "loading-module";
            String out = GuardService.runAsRoot(c,
                    "sh " + dir.getAbsolutePath() + "/ksu_bootstrap.sh "
                            + dir.getAbsolutePath(), 30000);
            note("bootstrap: " + oneLine(out));
            // A null answer does not mean failure: rootd recreates rootout with
            // its own umask, so an app-side read can miss it even though the
            // command ran.  The shim's proc node is the authoritative signal.
            if (!procUp(PROC) && (out == null || out.contains("FATAL")
                    || out.contains("CP-KO-FAIL"))) {
                sStatus = "bootstrap-failed";
                return;
            }
            sStatus = "module-loaded";

            // Termux is intentionally NOT touched here.  Since the Patches
            // page redesign the user applies that patch on demand; nothing
            // in this app keeps a resident polling thread alive.
            linkManagerOnce(c);
            sStatus = "ready";
            note("SukiSU layer ready (Termux patch is manual: open 补丁)");
        } catch (Throwable t) {
            sStatus = "error";
            note("bootstrap threw: " + t);
        }
    }

    /**
     * Called from MainActivity: if the Manager was installed later by hand,
     * make sure the shim still learns its uid.  Never force-stops anything.
     */
    static void ensureUid(Context c) {
        final Context app = c.getApplicationContext();
        new Thread(() -> {
            // Skip before root is up: otherwise writeUid() floods the log
            // with "(no answer)" lines on every app launch.
            if (!channelReady())
                return;
            int uid = managerUid(app);
            if (uid > 0)
                writeUid(app, uid);
        }, "ksu-uid").start();
    }
}
