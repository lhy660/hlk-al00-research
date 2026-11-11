package com.hlk.rootguard;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.DashPathEffect;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.util.TypedValue;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * Tiny hand-rolled design system for the KernelSU-like UI.
 *
 * This APK is built with aapt2 + javac + d8 only: there are NO resources,
 * NO XML layouts and NO Material Components dependency.  Every rounded card,
 * badge, nav glyph and the decorative rings on the hero card are drawn in
 * code here.  Colours follow the SukiSU Manager screenshot (soft pink page,
 * dusty-rose hero, white cards, floating capsule navigation).
 */
final class Ui {
    static final int BG = Color.parseColor("#FFF4F6");
    static final int CARD = Color.parseColor("#FFFFFF");
    static final int PRIMARY = Color.parseColor("#9B5563");
    static final int PRIMARY_DARK = Color.parseColor("#7F4452");
    static final int PRIMARY_SOFT = Color.parseColor("#F8E2E7");
    static final int BADGE = Color.parseColor("#D4788A");
    static final int TEXT = Color.parseColor("#2C2326");
    static final int SUB = Color.parseColor("#8A767B");
    static final int LINE = Color.parseColor("#F1E5E8");
    static final int HINT = Color.parseColor("#F3DAF0");
    static final int HINT_TEXT = Color.parseColor("#7B4B78");
    static final int OK = Color.parseColor("#2F8265");
    static final int WARN = Color.parseColor("#B76B2C");
    static final int ERR = Color.parseColor("#B3454F");

    private Ui() {
    }

    static int dp(Context c, float v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP,
                v, c.getResources().getDisplayMetrics());
    }

    static GradientDrawable round(int color, float radiusDp, Context c) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(dp(c, radiusDp));
        return d;
    }

    static GradientDrawable hero(Context c) {
        GradientDrawable d = new GradientDrawable(
                GradientDrawable.Orientation.TL_BR,
                new int[]{Color.parseColor("#A75F6C"),
                        Color.parseColor("#834653")});
        d.setCornerRadius(dp(c, 26));
        return d;
    }

    static TextView text(Context c, String s, float sp, int color, boolean bold) {
        TextView t = new TextView(c);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(color);
        if (bold)
            t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    static TextView mono(Context c, String s, float sp, int color) {
        TextView t = text(c, s, sp, color, false);
        t.setTypeface(Typeface.MONOSPACE);
        t.setTextIsSelectable(true);
        return t;
    }

    /** White rounded vertical card, the workhorse of every page. */
    static LinearLayout card(Context c, int padDp) {
        LinearLayout l = new LinearLayout(c);
        l.setOrientation(LinearLayout.VERTICAL);
        l.setBackground(round(CARD, 18, c));
        int p = dp(c, padDp);
        l.setPadding(p, p, p, p);
        return l;
    }

    static LinearLayout.LayoutParams lp(int width, int height, float weight) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(width, height);
        p.weight = weight;
        return p;
    }

    static View gap(Context c, int dp) {
        View v = new View(c);
        v.setLayoutParams(new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(c, dp)));
        return v;
    }

    static View rule(Context c) {
        View v = new View(c);
        v.setBackgroundColor(LINE);
        v.setLayoutParams(new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, Math.max(1, dp(c, 0.8f))));
        return v;
    }

    /** A left label / right value row, as in the Manager's info card. */
    static LinearLayout row(Context c, String key, String value) {
        LinearLayout l = new LinearLayout(c);
        l.setOrientation(LinearLayout.HORIZONTAL);
        l.setPadding(0, dp(c, 9), 0, dp(c, 9));
        TextView k = text(c, key, 13, SUB, false);
        k.setLayoutParams(lp(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        l.addView(k);
        TextView v = text(c, value, 13, TEXT, true);
        v.setSingleLine(true);
        l.addView(v);
        return l;
    }

    /** Small pill used for the mode badge and the hint banner. */
    static TextView pill(Context c, String s, int bg, int fg) {
        TextView t = text(c, s, 11, fg, true);
        t.setBackground(round(bg, 20, c));
        int h = dp(c, 8), v = dp(c, 4);
        t.setPadding(h, v, h, v);
        return t;
    }

    static android.widget.Button button(Context c, String label, boolean primary) {
        android.widget.Button b = new android.widget.Button(c);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextSize(14);
        b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setTextColor(primary ? Color.WHITE : PRIMARY);
        b.setBackground(round(primary ? PRIMARY : PRIMARY_SOFT, 22, c));
        b.setPadding(dp(c, 14), dp(c, 6), dp(c, 14), dp(c, 6));
        b.setStateListAnimator(null);
        return b;
    }

    /** Decorative concentric rings + a soft dot, drawn behind the hero text. */
    static final class Decor extends View {
        private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);

        Decor(Context c) {
            super(c);
        }

        @Override
        protected void onDraw(Canvas cv) {
            super.onDraw(cv);
            float cx = getWidth() * 0.80f;
            float cy = getHeight() * 0.46f;
            float unit = Math.min(getWidth(), getHeight()) * 0.145f;
            p.setStyle(Paint.Style.STROKE);
            p.setStrokeWidth(dp(getContext(), 1.2f));
            p.setColor(0x30FFFFFF);
            p.setPathEffect(null);
            for (int i = 2; i <= 4; i++)
                cv.drawCircle(cx, cy, unit * i, p);
            p.setColor(0x28FFFFFF);
            p.setPathEffect(new DashPathEffect(
                    new float[]{dp(getContext(), 5), dp(getContext(), 7)}, 0));
            cv.drawCircle(cx, cy, unit * 4.7f, p);
            p.setPathEffect(null);
            p.setStyle(Paint.Style.FILL);
            p.setColor(0x22FFFFFF);
            cv.drawCircle(getWidth() * 0.90f, getHeight() * 0.20f, unit * 0.75f, p);
        }
    }

    /** Vector glyph for the bottom navigation: 0=home, 1=patch, 2=log. */
    static final class Glyph extends View {
        private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final int type;
        private boolean selected;

        Glyph(Context c, int type) {
            super(c);
            this.type = type;
        }

        /** Not named setSelected(): View already owns that public method. */
        void select(boolean s) {
            selected = s;
            invalidate();
        }

        @Override
        protected void onDraw(Canvas cv) {
            super.onDraw(cv);
            float w = getWidth(), h = getHeight();
            float s = Math.min(w, h);
            p.setColor(selected ? Color.WHITE : PRIMARY);
            p.setStrokeWidth(dp(getContext(), 1.8f));
            p.setStyle(Paint.Style.STROKE);
            p.setStrokeCap(Paint.Cap.ROUND);
            p.setStrokeJoin(Paint.Join.ROUND);
            if (type == 0) {
                Path roof = new Path();
                roof.moveTo(w * 0.12f, h * 0.50f);
                roof.lineTo(w * 0.50f, h * 0.18f);
                roof.lineTo(w * 0.88f, h * 0.50f);
                cv.drawPath(roof, p);
                Path body = new Path();
                body.moveTo(w * 0.24f, h * 0.46f);
                body.lineTo(w * 0.24f, h * 0.84f);
                body.lineTo(w * 0.76f, h * 0.84f);
                body.lineTo(w * 0.76f, h * 0.46f);
                cv.drawPath(body, p);
            } else if (type == 1) {
                // band-aid: rounded square rotated 45 degrees + two dots
                cv.save();
                cv.rotate(45, w / 2f, h / 2f);
                float r = s * 0.30f;
                cv.drawRoundRect(w / 2f - r, h / 2f - r * 0.55f,
                        w / 2f + r, h / 2f + r * 0.55f,
                        r * 0.35f, r * 0.35f, p);
                cv.restore();
                p.setStyle(Paint.Style.FILL);
                cv.drawCircle(w / 2f - s * 0.09f, h / 2f, dp(getContext(), 1.4f), p);
                cv.drawCircle(w / 2f + s * 0.09f, h / 2f, dp(getContext(), 1.4f), p);
            } else if (type == 2) {
                for (int i = 0; i < 3; i++) {
                    float y = h * (0.28f + i * 0.22f);
                    cv.drawLine(w * 0.18f, y, w * 0.82f, y, p);
                }
            } else {
                // power: broken ring + stem
                float r = s * 0.30f;
                RectF oval = new RectF(w / 2f - r, h / 2f - r,
                        w / 2f + r, h / 2f + r);
                cv.drawArc(oval, -50, 280, false, p);
                cv.drawLine(w / 2f, h * 0.14f, w / 2f, h * 0.50f, p);
            }
        }
    }
}
