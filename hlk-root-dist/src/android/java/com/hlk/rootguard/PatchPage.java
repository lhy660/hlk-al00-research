package com.hlk.rootguard;

import android.content.Context;
import android.graphics.Color;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * "补丁" -- on-demand patch centre.
 *
 * There is exactly ONE item today: "Termux 补丁".  Nothing here runs in the
 * background; the old 5 s / 60 s watchdogs were removed.  The user taps
 * "应用补丁" and KsuBootstrap.applyTermuxPatch() runs the idempotent setup
 * script once through the root channel.
 */
final class PatchPage {
    private static final String P_LAST = "patch_termux_last";
    private static final String P_RESULT = "patch_termux_result";

    private final Context c;
    final View view;
    private final Handler ui = new Handler(Looper.getMainLooper());

    private final TextView chip;
    private final TextView status;
    private final TextView output;
    private final TextView last;
    private final android.widget.Button applyBtn;
    private final android.widget.Button checkBtn;
    private volatile boolean busy;

    PatchPage(Context c) {
        this.c = c;

        LinearLayout page = new LinearLayout(c);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(Ui.dp(c, 16), Ui.dp(c, 6), Ui.dp(c, 16), Ui.dp(c, 12));

        page.addView(Ui.text(c, "补丁", 24, Ui.TEXT, true));
        page.addView(Ui.text(c, "按需执行 · 无常驻后台线程 · 开机链不会自动应用",
                12, Ui.SUB, false));
        page.addView(Ui.gap(c, 14));

        // ---- the one patch item -----------------------------------------
        LinearLayout card = Ui.card(c, 18);
        LinearLayout head = new LinearLayout(c);
        head.setOrientation(LinearLayout.HORIZONTAL);
        head.setGravity(android.view.Gravity.CENTER_VERTICAL);
        TextView num = Ui.pill(c, "01", Ui.PRIMARY_SOFT, Ui.PRIMARY);
        head.addView(num);
        TextView title = Ui.text(c, "Termux 补丁", 17, Ui.TEXT, true);
        LinearLayout.LayoutParams tp = new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        tp.setMarginStart(Ui.dp(c, 10));
        title.setLayoutParams(tp);
        head.addView(title);
        chip = Ui.pill(c, "检查中", Ui.PRIMARY_SOFT, Ui.PRIMARY);
        head.addView(chip);
        card.addView(head);

        card.addView(Ui.gap(c, 10));
        card.addView(Ui.text(c,
                "解决 Termux / tmoe 容器环境下的 su、sudo、tsu 与挂载问题：",
                12, Ui.SUB, false));
        card.addView(Ui.gap(c, 6));
        String[] items = {
                "① 用真实前端替换 su / tsu / sudo，绕开 sucompat 概率性重定向",
                "② su -c 多参数逐个重加引号（修复 MOUNT_TMP 写不进配置）",
                "③ 给 su -c 补 /system/bin 等系统 PATH（修复 mount 找不到）",
                "④ tmoe 配置目录 0700 → 0755（修复菜单显示假默认值）",
                "⑤ 补 tsudo 链接；容器 profile.d 绝对软链改相对",
                "⑥ 重建 shim 的 root 授权表（模块内存，重启即清空）"};
        for (String s : items)
            card.addView(Ui.text(c, s, 11, Ui.TEXT, false));

        card.addView(Ui.gap(c, 12));
        card.addView(Ui.rule(c));
        card.addView(Ui.gap(c, 8));
        card.addView(Ui.text(c, "当前状态", 12, Ui.SUB, true));
        status = Ui.mono(c, "检查中…", 11, Ui.TEXT);
        card.addView(status);

        card.addView(Ui.gap(c, 12));
        LinearLayout brow = new LinearLayout(c);
        brow.setOrientation(LinearLayout.HORIZONTAL);
        applyBtn = Ui.button(c, "应用补丁", true);
        applyBtn.setLayoutParams(Ui.lp(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        applyBtn.setOnClickListener(v -> apply());
        brow.addView(applyBtn);
        checkBtn = Ui.button(c, "刷新状态", false);
        checkBtn.setOnClickListener(v -> runCheck());
        brow.addView(checkBtn);
        card.addView(brow);

        card.addView(Ui.gap(c, 10));
        card.addView(Ui.text(c, "执行输出", 12, Ui.SUB, true));
        ScrollView osc = new ScrollView(c);
        osc.setBackground(Ui.round(Ui.BG, 12, c));
        osc.setPadding(Ui.dp(c, 8), Ui.dp(c, 8), Ui.dp(c, 8), Ui.dp(c, 8));
        output = Ui.mono(c, "（尚未执行）", 10, Ui.TEXT);
        osc.addView(output);
        card.addView(osc, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, Ui.dp(c, 110)));

        // "什么时候需要点一次" lives INSIDE the same rounded card now, under
        // a small pill label: one continuous surface instead of two cards.
        card.addView(Ui.gap(c, 14));
        card.addView(Ui.rule(c));
        card.addView(Ui.gap(c, 10));
        card.addView(Ui.pill(c, "什么时候需要点一次",
                Ui.PRIMARY_SOFT, Ui.PRIMARY));
        card.addView(Ui.gap(c, 8));
        String[] tips = {
                "· 恢复出厂 / 全新安装 Termux 之后（uid 会重新分配）",
                "· 第一次打开 Termux、让它解压 bootstrap 之后",
                "· Termux 执行 pkg upgrade 还原了原版 su 之后",
                "· tmoe 重新部署容器之后（profile.d 软链会重建）",
                "· 每次手机重启之后（授权表只存在于模块内存）",
                "· 不需要联网、不需要电脑；脚本幂等，可重复点"};
        for (String s : tips)
            card.addView(Ui.text(c, s, 12, Ui.SUB, false));
        page.addView(card);

        page.addView(Ui.gap(c, 12));
        last = Ui.text(c, "", 11, Ui.SUB, false);
        page.addView(last);

        ScrollView sc = new ScrollView(c);
        sc.setFillViewport(true);
        sc.addView(page);
        view = sc;
        refresh();
        runCheck();
    }

    /** Called when the user switches to this tab. */
    void onShown() {
        if (!busy)
            runCheck();
    }

    /** Lightweight per-second refresh: never touches the root channel. */
    void refresh() {
        String t = Prefs.sp(c).getString(P_LAST, null);
        String r = Prefs.sp(c).getString(P_RESULT, null);
        last.setText(t == null
                ? "上次应用：从未（本设备）"
                : "上次应用：" + t + "  ·  " + (r == null ? "-" : r));
    }

    private void setBusy(boolean b, String label) {
        busy = b;
        applyBtn.setEnabled(!b);
        checkBtn.setEnabled(!b);
        applyBtn.setText(b ? label : "应用补丁");
    }

    private void chip(String s, int bg, int fg) {
        chip.setText(s);
        chip.setBackground(Ui.round(bg, 20, c));
        chip.setTextColor(fg);
    }

    private void runCheck() {
        if (busy)
            return;
        setBusy(true, "检查中…");
        status.setText("检查中…");
        new Thread(() -> {
            final String res = KsuBootstrap.checkTermuxPatch(c);
            ui.post(() -> {
                status.setText(res);
                if (res.contains("PATCH_CHECK_OK"))
                    chip("已应用", Ui.PRIMARY_SOFT, Ui.OK);
                else if (res.contains("PATCH_CHECK_NEED"))
                    chip("需要应用", Color.parseColor("#FBEBD9"), Ui.WARN);
                else
                    chip("未就绪", Ui.PRIMARY_SOFT, Ui.SUB);
                setBusy(false, null);
            });
        }, "patch-check").start();
    }

    private void apply() {
        if (busy)
            return;
        setBusy(true, "执行中…");
        output.setText("执行中，请稍候…（最多 30 秒）");
        new Thread(() -> {
            final String res = KsuBootstrap.applyTermuxPatch(c);
            final boolean ok = res.contains("TERMUX-SETUP-OK");
            String stamp = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss",
                    Locale.getDefault()).format(new Date());
            Prefs.sp(c).edit()
                    .putString(P_LAST, stamp)
                    .putString(P_RESULT, ok ? "成功" : "未完成")
                    .commit();
            ui.post(() -> {
                output.setText(res);
                refresh();
                setBusy(false, null);
                chip(ok ? "已应用" : "检查输出",
                        ok ? Ui.PRIMARY_SOFT : Color.parseColor("#FBEBD9"),
                        ok ? Ui.OK : Ui.WARN);
                // refresh the read-only status too, but do not re-lock the UI
                runCheck();
            });
        }, "patch-apply").start();
    }
}
