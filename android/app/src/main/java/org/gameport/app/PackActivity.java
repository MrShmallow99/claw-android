package org.gameport.app;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.view.Gravity;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;
import java.util.Locale;

/**
 * Optional, one-time compression of the imported game files (chosen on the setup
 * screen): builds game.pak in the game folder with the same lossless packer as the
 * bundled APK (runtime/pack/packer.c), verifies every file against the original,
 * and only then removes the loose copies.  On any failure the files are left as
 * they were and the game runs uncompressed.
 *
 * Runs in its own process (":pack" in the manifest), so the game process later
 * starts clean and opens the finished pack like any other.
 */
public class PackActivity extends Activity {
    private TextView status;
    private ProgressBar bar;
    private Button startButton;
    private volatile boolean busy = true;
    private long lastUi;

    private native String nativePack(String gameDir, String saveDir);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        buildUi();
        final File ext = getExternalFilesDir(null);
        new Thread(() -> {
            String err;
            try {
                System.loadLibrary("SDL2");
                System.loadLibrary("main");
                err = nativePack(new File(ext, "game").getAbsolutePath(), new File(ext, "save").getAbsolutePath());
            } catch (Throwable t) {
                err = t.toString();
            }
            final String e = err;
            runOnUiThread(() -> {
                busy = false;
                if (e == null) {
                    startGame();
                } else {
                    status.setText("Compression did not complete: " + e
                            + "\n\nYour game files were left uncompressed and the game works normally.");
                    bar.setVisibility(ProgressBar.GONE);
                    startButton.setVisibility(Button.VISIBLE);
                }
            });
        }, "packer").start();
    }

    /** Called from native code (the packer's thread). */
    @SuppressWarnings("unused")
    private void onProgress(int phase, long done, long total) {
        long now = System.currentTimeMillis();
        if (now - lastUi < 200 && done < total) return;
        lastUi = now;
        final String[] names = {"Scanning the game files", "Compressing", "Verifying every file", "Removing the uncompressed copy"};
        final String text = String.format(Locale.US, "%s...%s", names[Math.max(0, Math.min(3, phase))],
                total > 0 ? String.format(Locale.US, "  %d / %d MB", done >> 20, total >> 20) : "");
        final int pct = total > 0 ? (int) (done * 1000 / total) : 0;
        runOnUiThread(() -> {
            status.setText(text + "\n\nThis runs once. Keep the app open; it can take several minutes.");
            bar.setIndeterminate(total <= 0);
            bar.setProgress(pct);
        });
    }

    private void buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);
        root.setBackgroundColor(Color.rgb(20, 40, 20));
        int pad = (int) (32 * getResources().getDisplayMetrics().density);
        root.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText("Compressing the game files");
        title.setTextSize(24);
        title.setTextColor(Color.rgb(242, 193, 78));
        title.setGravity(Gravity.CENTER);
        root.addView(title);

        status = new TextView(this);
        status.setTextSize(16);
        status.setTextColor(Color.WHITE);
        status.setGravity(Gravity.CENTER);
        status.setPadding(0, pad / 2, 0, pad / 2);
        status.setText("Starting...");
        root.addView(status);

        bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        bar.setIndeterminate(true);
        root.addView(bar, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));

        startButton = new Button(this);
        startButton.setText("Start the game");
        startButton.setVisibility(Button.GONE);
        startButton.setOnClickListener(v -> startGame());
        root.addView(startButton);
        setContentView(root);
    }

    private void startGame() {
        startActivity(new Intent(this, GameActivity.class));
        finish();
        // this process holds the packer's caches: end it (the game runs in the main process)
        android.os.Process.killProcess(android.os.Process.myPid());
    }

    @Override
    public void onBackPressed() {
        if (!busy) super.onBackPressed();
    }
}
