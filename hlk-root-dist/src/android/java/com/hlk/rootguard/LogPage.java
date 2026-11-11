package com.hlk.rootguard;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewParent;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/**
 * "日志" -- live boot-chain log plus the root command runner.
 *
 * Two interaction rules fixed in the v1.4 log-page hotfix (RESEARCH_LOG 47.7):
 *   - The two output areas have INDEPENDENT clear buttons.  The button in the
 *     log card clears GuardService's ring buffer, not the command output.
 *   - The log box is a nested ScrollView.  Android's default nested scrolling
 *     hands the gesture to the outer page scroller first, so the box itself
 *     appeared unscrollable.  Disable nested scrolling on the inner view and
 *     disallow parent interception while the finger is inside it.  TextViews
 *     are also made non-focusable and only re-set when the text really changed:
 *     a focusable TextView re-laid out every second kept calling
 *     bringIntoView() and yanked the whole page away from the user's scroll.
 */
final class LogPage {
    private final Context c;
    final View view;
    private final Handler ui = new Handler(Looper.getMainLooper());

    private final TextView state;
    private final EditText command;
    // Not final: lambdas created inside the constructor capture these, and
    // javac rejects a blank final read before its assignment point.  They are
    // always assigned before any click can happen.
    private TextView cmdOut;
    private final TextView log;
    private ScrollView logScroll;
    /** Last rendered log text; avoids a full re-layout on every 1 s tick. */
    private String lastLog = null;
    /** Placeholder kept on screen after an explicit clear, until new output. */
    private String clearedHint = null;

    LogPage(Context c) {
        this.c = c;

        LinearLayout page = new LinearLayout(c);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(Ui.dp(c, 16), Ui.dp(c, 6), Ui.dp(c, 16), Ui.dp(c, 12));

        page.addView(Ui.text(c, "日志", 24, Ui.TEXT, true));
        page.addView(Ui.text(c, "开机链实时输出 · 可用 adb logcat -s HlkGuard 对照",
                12, Ui.SUB, false));
        page.addView(Ui.gap(c, 14));

        LinearLayout st = Ui.card(c, 14);
        st.addView(Ui.text(c, "当前状态", 12, Ui.SUB, true));
        state = plainMono(c, "", 11);
        st.addView(state);
        page.addView(st);

        // ---- root command runner ----------------------------------------
        page.addView(Ui.gap(c, 12));
        LinearLayout runner = Ui.card(c, 14);
        LinearLayout rh = new LinearLayout(c);
        rh.setOrientation(LinearLayout.HORIZONTAL);
        rh.setGravity(Gravity.CENTER_VERTICAL);
        TextView rt = Ui.text(c, "以 root 身份执行命令", 14, Ui.TEXT, true);
        rt.setLayoutParams(Ui.lp(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        rh.addView(rt);
        android.widget.Button clearCmd = Ui.button(c, "清空输出", false);
        clearCmd.setOnClickListener(v -> cmdOut.setText("（已清空）"));
        rh.addView(clearCmd);
        runner.addView(rh);
        runner.addView(Ui.gap(c, 8));
        LinearLayout row = new LinearLayout(c);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        command = new EditText(c);
        command.setSingleLine(true);
        command.setText("id");
        command.setInputType(InputType.TYPE_CLASS_TEXT);
        command.setTextSize(12);
        command.setLayoutParams(Ui.lp(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        row.addView(command);
        android.widget.Button run = Ui.button(c, "运行", true);
        run.setOnClickListener(v -> runCommand());
        row.addView(run);
        runner.addView(row);
        runner.addView(Ui.gap(c, 8));
        cmdOut = plainMono(c, "（尚未执行）", 10);
        cmdOut.setBackground(Ui.round(Ui.BG, 10, c));
        cmdOut.setPadding(Ui.dp(c, 8), Ui.dp(c, 8), Ui.dp(c, 8), Ui.dp(c, 8));
        runner.addView(cmdOut);
        page.addView(runner);

        // ---- live log ----------------------------------------------------
        page.addView(Ui.gap(c, 12));
        LinearLayout logCard = Ui.card(c, 14);
        LinearLayout lh = new LinearLayout(c);
        lh.setOrientation(LinearLayout.HORIZONTAL);
        lh.setGravity(Gravity.CENTER_VERTICAL);
        TextView lt = Ui.text(c, "实时日志（120 行）", 13, Ui.TEXT, true);
        lt.setLayoutParams(Ui.lp(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        lh.addView(lt);
        android.widget.Button latest = Ui.button(c, "最新", false);
        latest.setOnClickListener(v ->
                logScroll.post(() -> logScroll.fullScroll(View.FOCUS_DOWN)));
        lh.addView(latest);
        android.widget.Button clearLogBtn = Ui.button(c, "清空日志", false);
        clearLogBtn.setOnClickListener(v -> doClearLog());
        lh.addView(clearLogBtn);
        logCard.addView(lh);
        logCard.addView(Ui.gap(c, 8));

        logScroll = new ScrollView(c);
        logScroll.setBackground(Ui.round(Ui.BG, 10, c));
        logScroll.setPadding(Ui.dp(c, 8), Ui.dp(c, 8), Ui.dp(c, 8), Ui.dp(c, 8));
        // Children must own their gesture: with nested scrolling on, the outer
        // page ScrollView consumes pre-scroll first and the log never moves.
        logScroll.setNestedScrollingEnabled(false);
        logScroll.setOnTouchListener((v, ev) -> {
            ViewParent p = v.getParent();
            if (p != null) {
                int a = ev.getActionMasked();
                p.requestDisallowInterceptTouchEvent(
                        a == MotionEvent.ACTION_DOWN || a == MotionEvent.ACTION_MOVE);
            }
            return false; // let the inner ScrollView handle the gesture itself
        });
        log = plainMono(c, "", 10);
        logScroll.addView(log);
        logCard.addView(logScroll, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, Ui.dp(c, 300)));
        page.addView(logCard);

        ScrollView sc = new ScrollView(c);
        sc.setFillViewport(true);
        sc.addView(page);
        view = sc;
        refresh();
    }

    /** Monospace text that must NOT steal focus or request bringIntoView(). */
    private static TextView plainMono(Context c, String s, float sp) {
        TextView t = Ui.mono(c, s, sp, Ui.TEXT);
        t.setTextIsSelectable(false);
        t.setFocusable(false);
        t.setFocusableInTouchMode(false);
        return t;
    }

    private void runCommand() {
        final String cmd = command.getText().toString().trim();
        if (cmd.isEmpty())
            return;
        cmdOut.setText("$ " + cmd + "\n(waiting for the root daemon...)");
        new Thread(() -> {
            final String res = GuardService.runAsRoot(c, cmd, 8000);
            ui.post(() -> cmdOut.setText(res != null
                    ? "$ " + cmd + "\n" + res
                    : "$ " + cmd + "\n(no answer - is root up? check the log)"));
        }, "root-cmd").start();
    }

    /** The log card's own button: clear the ring buffer, not the command box. */
    private void doClearLog() {
        int n = GuardService.clearLog();
        lastLog = null;
        logScroll.scrollTo(0, 0);
        clearedHint = "（已清空 " + n + " 行日志，等待新输出…）";
        log.setText(clearedHint);
    }

    /** Called once per second by MainActivity. */
    void refresh() {
        // GuardService.sState is a raw flag: a SECOND, redundant probe launch
        // can fail with ETXTBSY and write "error" even though the first probe
        // is alive and root is fully up.  Show the effective state instead so
        // the log page never looks broken while root works.
        String raw = GuardService.state();
        String svc = GuardService.root()
                && (raw.equals("error") || raw.startsWith("exited"))
                ? "root active (stale flag: " + raw + ")" : raw;
        state.setText("service : " + svc
                + "\nroot    : " + (GuardService.root() ? "uid 0" : "no")
                + "\nprobe   : pid " + GuardService.pid()
                + "\nsukisu  : " + KsuBootstrap.status()
                + "\nautostart : " + (Prefs.enabled(c) ? "ON" : "OFF"));

        StringBuilder lb = new StringBuilder();
        for (String s : GuardService.logTail(120))
            lb.append(s).append('\n');
        String text = lb.toString();
        if (text.equals(lastLog))
            return;                       // nothing new: do not disturb scrolling

        if (text.isEmpty()) {
            // Just cleared (or not started yet): keep a stable placeholder.
            log.setText(clearedHint != null ? clearedHint : "（暂无日志输出）");
            logScroll.scrollTo(0, 0);
            lastLog = text;
            return;
        }
        clearedHint = null;
        boolean atBottom = logScroll.getScrollY() + logScroll.getHeight()
                >= log.getHeight() - Ui.dp(c, 24);
        lastLog = text;
        log.setText(text);
        if (atBottom)
            logScroll.post(() -> logScroll.fullScroll(View.FOCUS_DOWN));
    }
}
