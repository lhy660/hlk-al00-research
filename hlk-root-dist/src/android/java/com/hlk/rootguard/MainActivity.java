package com.hlk.rootguard;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

/**
 * KernelSU-style shell: a floating capsule bottom navigation with three tabs
 * -- 主页 / 补丁 / 日志 -- over a soft pink page.
 *
 * Everything is built in code (no resources / XML layouts / Material lib),
 * so aapt2 + javac + d8 is still the whole build.  The former single screen
 * with its switches and log tail is now split across HomePage / PatchPage /
 * LogPage; the two resident watchdogs were removed in the same redesign.
 */
public class MainActivity extends Activity {
    private static final String[] TABS = {"主页", "补丁", "日志"};

    private HomePage home;
    private PatchPage patch;
    private LogPage logs;
    private FrameLayout pages;
    private LinearLayout[] tabs;
    private Ui.Glyph[] glyphs;
    private TextView[] labels;
    private int current = -1;

    private final Handler ui = new Handler(Looper.getMainLooper());
    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            refreshCurrent();
            ui.postDelayed(this, 1000);
        }
    };

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);

        // NoActionBar theme + a matching light status bar: the old dark
        // system title bar repeated "HLK Root" above our own page title.
        getWindow().setStatusBarColor(Ui.BG);
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR);

        // (Re)schedule the persisted boot job: an app update cancels persisted
        // jobs, so every launch refreshes it.  EMUI drops BOOT_COMPLETED for
        // this package, so this job is the real boot trigger.
        try {
            android.app.job.JobInfo j =
                    new android.app.job.JobInfo.Builder(1001,
                            new android.content.ComponentName(this,
                                    BootJobService.class))
                            .setPersisted(true)
                            .setPeriodic(15 * 60 * 1000L)
                            .build();
            ((android.app.job.JobScheduler) getSystemService(JOB_SCHEDULER_SERVICE))
                    .schedule(j);
        } catch (Throwable ignored) {
        }

        // One-shot Manager link whenever the app is opened (no polling thread).
        KsuBootstrap.ensureUid(this);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Ui.BG);

        // ---- top bar -----------------------------------------------------
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.setPadding(Ui.dp(this, 20), Ui.dp(this, 18),
                Ui.dp(this, 16), Ui.dp(this, 8));
        LinearLayout titles = new LinearLayout(this);
        titles.setOrientation(LinearLayout.VERTICAL);
        titles.addView(Ui.text(this, "HLK Root", 26, Ui.TEXT, true));
        titles.addView(Ui.text(this, "HLK-AL00 · 临时 root", 12, Ui.SUB, false));
        titles.setLayoutParams(Ui.lp(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        bar.addView(titles);

        FrameLayout power = new FrameLayout(this);
        power.setBackground(Ui.round(Ui.CARD, 18, this));
        int ps = Ui.dp(this, 38);
        power.setLayoutParams(new LinearLayout.LayoutParams(ps, ps));
        Ui.Glyph pg = new Ui.Glyph(this, 3);
        // selected=false draws PRIMARY on the white circle (selected would
        // draw white-on-white and make the button look blank).
        power.addView(pg, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        power.setOnClickListener(v -> disable());
        bar.addView(power);
        root.addView(bar);

        // ---- three pages -------------------------------------------------
        home = new HomePage(this, this);
        patch = new PatchPage(this);
        logs = new LogPage(this);
        pages = new FrameLayout(this);
        pages.setLayoutParams(new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        pages.addView(home.view, match());
        pages.addView(patch.view, match());
        pages.addView(logs.view, match());
        root.addView(pages);

        // ---- floating capsule navigation --------------------------------
        LinearLayout navWrap = new LinearLayout(this);
        navWrap.setOrientation(LinearLayout.HORIZONTAL);
        navWrap.setPadding(Ui.dp(this, 16), Ui.dp(this, 6),
                Ui.dp(this, 16), Ui.dp(this, 14));
        LinearLayout nav = new LinearLayout(this);
        nav.setOrientation(LinearLayout.HORIZONTAL);
        nav.setBackground(Ui.round(Ui.PRIMARY_SOFT, 28, this));
        nav.setPadding(Ui.dp(this, 6), Ui.dp(this, 6),
                Ui.dp(this, 6), Ui.dp(this, 6));
        nav.setElevation(Ui.dp(this, 6));
        nav.setLayoutParams(new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        tabs = new LinearLayout[TABS.length];
        glyphs = new Ui.Glyph[TABS.length];
        labels = new TextView[TABS.length];
        for (int i = 0; i < TABS.length; i++) {
            LinearLayout t = new LinearLayout(this);
            t.setOrientation(LinearLayout.VERTICAL);
            t.setGravity(Gravity.CENTER);
            t.setPadding(Ui.dp(this, 8), Ui.dp(this, 7),
                    Ui.dp(this, 8), Ui.dp(this, 7));
            t.setLayoutParams(Ui.lp(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
            Ui.Glyph g = new Ui.Glyph(this, i);
            g.setLayoutParams(new LinearLayout.LayoutParams(
                    Ui.dp(this, 19), Ui.dp(this, 19)));
            t.addView(g);
            TextView tv = Ui.text(this, TABS[i], 11, Ui.PRIMARY, true);
            t.addView(tv);
            final int k = i;
            t.setOnClickListener(v -> goTo(k));
            tabs[i] = t;
            glyphs[i] = g;
            labels[i] = tv;
            nav.addView(t);
        }
        navWrap.addView(nav);
        root.addView(navWrap);

        setContentView(root);
        // Optional deep link for verification / shortcuts:
        //   am start -n com.hlk.rootguard/.MainActivity --ei page 1
        int p = 0;
        try {
            p = getIntent().getIntExtra("page", 0);
        } catch (Throwable ignored) {
        }
        goTo(p < 0 || p >= TABS.length ? 0 : p);
    }

    private FrameLayout.LayoutParams match() {
        return new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT);
    }

    private void disable() {
        Prefs.setEnabled(this, false);
        startService(new Intent(this, GuardService.class)
                .setAction(GuardService.ACTION_DISABLE));
        Toast.makeText(this, "已关闭开机自启（探针不会停止）",
                Toast.LENGTH_SHORT).show();
        refreshCurrent();
    }

    /** Package-private: HomePage's stat cards and buttons switch tabs. */
    void goTo(int i) {
        if (i == current)
            return;
        current = i;
        for (int k = 0; k < tabs.length; k++) {
            boolean on = k == i;
            pages.getChildAt(k).setVisibility(on ? View.VISIBLE : View.GONE);
            tabs[k].setBackground(on
                    ? Ui.round(Ui.PRIMARY, 22, this) : null);
            labels[k].setTextColor(on ? Color.WHITE : Ui.PRIMARY);
            glyphs[k].select(on);
        }
        refreshCurrent();
        if (i == 1)
            patch.onShown();
    }

    private void refreshCurrent() {
        if (current == 0)
            home.refresh();
        else if (current == 1)
            patch.refresh();
        else if (current == 2)
            logs.refresh();
    }

    @Override
    protected void onResume() {
        super.onResume();
        ui.post(tick);
    }

    @Override
    protected void onPause() {
        ui.removeCallbacks(tick);
        super.onPause();
    }
}
