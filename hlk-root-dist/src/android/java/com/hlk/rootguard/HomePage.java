package com.hlk.rootguard;

import android.content.Context;
import android.content.Intent;
import android.graphics.Color;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;

import java.io.File;

/**
 * "主页" -- the KernelSU-style status screen.
 *
 * The hero card mirrors the SukiSU Manager's "工作中" block, but its big
 * title is driven by the actual boot-chain phase instead of a static label:
 *   探针等待中 / 探针已运行 / 正在装载内核模块 / ROOT运行中
 */
final class HomePage {
    private final Context c;
    private final MainActivity act;
    final View view;

    private final TextView big;
    private final TextView sub;
    private final TextView badge;
    private final TextView hint;
    private final TextView vAutostart;
    private final TextView vPid;
    private final TextView vRoot;
    private final TextView vKsu;
    private final TextView vStrikes;
    private final Switch toggle;
    private final EditText delay;

    HomePage(Context c, MainActivity act) {
        this.c = c;
        this.act = act;

        LinearLayout page = new LinearLayout(c);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(Ui.dp(c, 16), Ui.dp(c, 6), Ui.dp(c, 16), Ui.dp(c, 12));

        // ---- hero card ---------------------------------------------------
        FrameLayout hero = new FrameLayout(c);
        hero.setBackground(Ui.hero(c));
        hero.setLayoutParams(new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, Ui.dp(c, 196)));
        hero.addView(new Ui.Decor(c), new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));

        LinearLayout ht = new LinearLayout(c);
        ht.setOrientation(LinearLayout.VERTICAL);
        int pad = Ui.dp(c, 20);
        ht.setPadding(pad, pad, pad, pad);
        badge = Ui.pill(c, "待机", Ui.BADGE, Color.WHITE);
        LinearLayout badgeRow = new LinearLayout(c);
        badgeRow.addView(badge);
        ht.addView(badgeRow);
        ht.addView(Ui.gap(c, 24));
        big = Ui.text(c, "探针等待中", 34, Color.WHITE, true);
        ht.addView(big);
        ht.addView(Ui.gap(c, 6));
        sub = Ui.text(c, "开机链未启动", 12, 0xE6FFFFFF, false);
        ht.addView(sub);
        hero.addView(ht, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM));
        page.addView(hero);

        page.addView(Ui.gap(c, 12));
        hint = Ui.text(c, "", 12, Ui.HINT_TEXT, false);
        hint.setBackground(Ui.round(Ui.HINT, 18, c));
        hint.setPadding(Ui.dp(c, 14), Ui.dp(c, 10), Ui.dp(c, 14), Ui.dp(c, 10));
        page.addView(hint);

        // ---- two small stat cards ---------------------------------------
        page.addView(Ui.gap(c, 12));
        LinearLayout stats = new LinearLayout(c);
        stats.setOrientation(LinearLayout.HORIZONTAL);
        stats.addView(statCard("2", "内核模块", 1), Ui.lp(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        View mid = Ui.gap(c, 0);
        mid.setLayoutParams(new LinearLayout.LayoutParams(Ui.dp(c, 10),
                ViewGroup.LayoutParams.MATCH_PARENT));
        stats.addView(mid);
        stats.addView(statCard("1", "补丁项目", 2), Ui.lp(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        page.addView(stats);

        // ---- device info card -------------------------------------------
        page.addView(Ui.gap(c, 12));
        LinearLayout info = Ui.card(c, 16);
        info.addView(Ui.row(c, "设备型号", "HLK-AL00"));
        info.addView(Ui.rule(c));
        info.addView(Ui.row(c, "内核版本", "4.14.116"));
        info.addView(Ui.rule(c));
        info.addView(Ui.row(c, "管理器版本", "40994-5"));
        info.addView(Ui.rule(c));
        info.addView(Ui.row(c, "内核完整版本", "v4.2.1-40994-hlk-shim9"));
        info.addView(Ui.rule(c));
        vAutostart = addRow(info, "开机自启", "-");
        info.addView(Ui.rule(c));
        vPid = addRow(info, "探针 PID", "-");
        info.addView(Ui.rule(c));
        vRoot = addRow(info, "ROOT 通道", "-");
        info.addView(Ui.rule(c));
        vKsu = addRow(info, "SukiSU 层", "-");
        info.addView(Ui.rule(c));
        vStrikes = addRow(info, "崩溃计数", "-");
        page.addView(info);

        // ---- controls ----------------------------------------------------
        page.addView(Ui.gap(c, 12));
        LinearLayout ctl = Ui.card(c, 16);
        ctl.addView(Ui.text(c, "运行控制", 15, Ui.TEXT, true));
        ctl.addView(Ui.gap(c, 8));
        toggle = new Switch(c);
        toggle.setText("开机自启 (auto-start on boot)");
        toggle.setTextSize(14);
        toggle.setTextColor(Ui.TEXT);
        toggle.setOnCheckedChangeListener((v, on) -> {
            Prefs.setEnabled(c, on);
            if (on)
                Prefs.sp(c).edit().putInt(Prefs.FAILS, 0).commit();
            refresh();
        });
        ctl.addView(toggle);
        ctl.addView(Ui.gap(c, 8));
        LinearLayout drow = new LinearLayout(c);
        drow.setOrientation(LinearLayout.HORIZONTAL);
        drow.setGravity(Gravity.CENTER_VERTICAL);
        TextView dl = Ui.text(c, "时间门延迟 (秒)", 13, Ui.SUB, false);
        dl.setLayoutParams(Ui.lp(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        drow.addView(dl);
        delay = new EditText(c);
        delay.setInputType(InputType.TYPE_CLASS_NUMBER);
        delay.setText(String.valueOf(Prefs.delayS(c)));
        delay.setTextSize(13);
        delay.setLayoutParams(new LinearLayout.LayoutParams(
                Ui.dp(c, 88), ViewGroup.LayoutParams.WRAP_CONTENT));
        drow.addView(delay);
        ctl.addView(drow);
        ctl.addView(Ui.gap(c, 10));
        LinearLayout brow = new LinearLayout(c);
        brow.setOrientation(LinearLayout.HORIZONTAL);
        android.widget.Button start = Ui.button(c, "启动探针", true);
        start.setLayoutParams(Ui.lp(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        start.setOnClickListener(v -> startProbe());
        brow.addView(start);
        android.widget.Button logs = Ui.button(c, "查看日志", false);
        logs.setOnClickListener(v -> act.goTo(2));
        brow.addView(logs);
        ctl.addView(brow);
        page.addView(ctl);

        page.addView(Ui.gap(c, 10));
        page.addView(Ui.text(c,
                "临时 root：不改系统分区，重启后失效并由开机链重建。\n"
                        + "硬关闭：touch /sdcard/HLK_ROOT_OFF",
                11, Ui.SUB, false));

        ScrollView sc = new ScrollView(c);
        sc.setFillViewport(true);
        sc.addView(page);
        view = sc;
        refresh();
    }

    private TextView addRow(LinearLayout parent, String key, String value) {
        LinearLayout l = new LinearLayout(c);
        l.setOrientation(LinearLayout.HORIZONTAL);
        l.setPadding(0, Ui.dp(c, 9), 0, Ui.dp(c, 9));
        TextView k = Ui.text(c, key, 13, Ui.SUB, false);
        k.setLayoutParams(Ui.lp(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        l.addView(k);
        TextView v = Ui.text(c, value, 13, Ui.TEXT, true);
        l.addView(v);
        parent.addView(l);
        return v;
    }

    private LinearLayout statCard(String number, String label, final int target) {
        LinearLayout l = Ui.card(c, 14);
        l.addView(Ui.text(c, number, 28, Ui.PRIMARY, true));
        l.addView(Ui.text(c, label, 12, Ui.SUB, false));
        l.setOnClickListener(v -> act.goTo(target));
        return l;
    }

    private void startProbe() {
        int d;
        try {
            d = Math.max(0, Math.min(3600,
                    Integer.parseInt(delay.getText().toString().trim())));
        } catch (Exception e) {
            d = Prefs.DEFAULT_DELAY_S;
        }
        Prefs.sp(c).edit()
                .putInt(Prefs.DELAY_S, d)
                .putBoolean(Prefs.ENABLED, true)
                .putInt(Prefs.FAILS, 0)
                .commit();
        if (!toggle.isChecked())
            toggle.setChecked(true);
        GuardService.arm(c);
        c.startForegroundService(new Intent(c, GuardService.class)
                .setAction(GuardService.ACTION_START));
        refresh();
    }

    /** Called once per second by MainActivity. */
    void refresh() {
        String st = GuardService.state();
        String ksu = KsuBootstrap.status();
        boolean root = GuardService.root();
        boolean enabled = Prefs.enabled(c);
        boolean procUp = new File("/proc/hlksu_fd9").exists();

        int phase;
        if (ksu.startsWith("ready") || (root && procUp
                && !ksu.equals("loading-module") && !ksu.equals("extracting")))
            phase = 3;
        else if (root || ksu.equals("extracting")
                || ksu.equals("loading-module") || ksu.equals("module-loaded"))
            phase = 2;
        else if (st.equals("starting") || st.equals("running")
                || st.equals("keepalive") || st.equals("root"))
            phase = 1;
        else
            phase = 0;

        String[] titles = {"探针等待中", "探针已运行",
                "正在装载内核模块", "ROOT运行中"};
        String[] subs = {
                "开机链未启动 · 点下方「启动探针」",
                "Mali JIT UAF 利用链运行中 · 约 400 秒时间门",
                "解压 assets → insmod 两个内核模块",
                "uid 0 · capability 完整 · FBE keyring 保留"};
        String[] badges = {"待机", "PHASE I", "LOADING", "越狱模式"};
        String[] hints = {
                "全新安装请先打开本页点「启动探针」；开启开机自启后，EMUI 丢广播时可能要等 15 分钟周期任务。",
                "探针正在运行，期间请勿覆盖安装或强制停止本应用（会触发内核 panic）。",
                "正在部署 hlk_ksu_shim9.ko / hlk_sucompat.ko 与 root 通道…",
                "补丁不会自动应用。全新出厂、Termux 首次启动或升级后，请到「补丁」页点一次。"};

        big.setText(titles[phase]);
        big.setTextSize(phase == 2 ? 27 : 34);
        sub.setText(subs[phase]);
        badge.setText(badges[phase]);
        hint.setText(hints[phase]);
        hint.setTextColor(phase == 3 ? Ui.HINT_TEXT : Ui.HINT_TEXT);

        if (toggle.isChecked() != enabled)
            toggle.setChecked(enabled);
        vAutostart.setText(enabled ? "ON" : "OFF");
        vAutostart.setTextColor(enabled ? Ui.OK : Ui.WARN);
        vPid.setText(String.valueOf(GuardService.pid()));
        vRoot.setText(root ? "yes (uid 0)" : "no");
        vRoot.setTextColor(root ? Ui.OK : Ui.SUB);
        vKsu.setText(ksu);
        vStrikes.setText(Prefs.fails(c) + "/" + Prefs.MAX_FAILS);
    }
}
