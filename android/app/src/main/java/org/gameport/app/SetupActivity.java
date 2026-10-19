package org.gameport.app;

import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.os.StatFs;
import android.provider.DocumentsContract;
import android.view.Gravity;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Locale;

/**
 * Makes sure the original Windows game files are available in
 * <external files dir>/game, then starts the game.
 *
 * When the APK bundles game.pak (built with -Ppack=...) nothing needs to be
 * copied: the game reads it straight from the APK.  Otherwise the files come
 * from a folder the user picks with the system folder picker.  Optionally they
 * are then compressed once into game.pak on the device (PackActivity).
 * Game-specific strings come from games/<name>/game.ini via BuildConfig.
 */
public class SetupActivity extends Activity {
    private static final int REQ_PICK_FOLDER = 1;

    private TextView status;
    private Button pickButton;
    private CheckBox compressBox;
    private volatile boolean busy;

    private File gameDir() {
        return new File(getExternalFilesDir(null), "game");
    }

    private static File findIgnoreCase(File dir, String name) {
        String[] names = dir.list();
        if (names == null) return null;
        for (String n : names) {
            if (n.equalsIgnoreCase(name)) return new File(dir, n);
        }
        return null;
    }

    /** All [game] required files are present (the native side checks the exe's version). */
    private static boolean hasRequired(File d) {
        for (String name : BuildConfig.REQUIRED) {
            File f = findIgnoreCase(d, name);
            if (f == null || f.length() == 0) return false;
        }
        return true;
    }

    private boolean gameInstalled() {
        // loose files, or the pack the optional on-device compression made
        return hasRequired(gameDir()) || new File(gameDir(), "game.pak").length() > 0;
    }

    private static String requiredList() {
        return String.join(", ", BuildConfig.REQUIRED).toUpperCase(Locale.ROOT);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (hasBundledPack()) {
            // The game reads its files straight from the APK.  Remove the copy that
            // older versions of the app extracted.
            File ext = getExternalFilesDir(null);
            deleteTree(gameDir());
            deleteTree(new File(ext, "game.tmp"));
            new File(ext, "bundle.stamp").delete();
            startGame();
            return;
        }
        new File(gameDir(), "game.pak.tmp").delete();   // an interrupted compression
        if (gameInstalled()) {
            startGame();
            return;
        }
        buildUi();
        {
            status.setText(BuildConfig.GAME_TITLE + " needs the files of your Windows version of the game.\n\n"
                    + "Copy the game folder (the one that contains " + BuildConfig.FILES_HINT
                    + ") to this device, then tap the button below and select it.");
            pickButton.setEnabled(true);
        }
    }

    private void buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);
        root.setBackgroundColor(Color.rgb(20, 40, 20));
        int pad = (int) (32 * getResources().getDisplayMetrics().density);
        root.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText(BuildConfig.GAME_TITLE);
        title.setTextSize(28);
        title.setTextColor(Color.rgb(242, 193, 78));
        title.setGravity(Gravity.CENTER);
        root.addView(title);

        status = new TextView(this);
        status.setTextSize(16);
        status.setTextColor(Color.WHITE);
        status.setGravity(Gravity.CENTER);
        status.setPadding(0, pad / 2, 0, pad / 2);
        root.addView(status);

        pickButton = new Button(this);
        pickButton.setText("Select game folder");
        pickButton.setEnabled(false);
        pickButton.setOnClickListener(v -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            startActivityForResult(i, REQ_PICK_FOLDER);
        });
        root.addView(pickButton);

        // optional one-time compression of the imported files (off by default)
        compressBox = new CheckBox(this);
        compressBox.setText("Compress the game files after importing (lossless; uses about half the storage)");
        compressBox.setTextColor(Color.WHITE);
        compressBox.setTextSize(15);
        compressBox.setChecked(false);
        compressBox.setPadding(0, pad / 3, 0, 0);
        root.addView(compressBox);

        TextView warn = new TextView(this);
        warn.setText("\u26A0 Compression adds extra work on top of the import, which is already demanding: "
                + "the phone will be busy, and may get warm, for several more minutes. "
                + "It runs only once, during this first import. Not recommended for low-end devices.");
        warn.setTextColor(Color.rgb(242, 193, 78));
        warn.setTextSize(13);
        warn.setGravity(Gravity.CENTER);
        warn.setPadding(pad / 2, pad / 6, pad / 2, 0);
        root.addView(warn);
        setContentView(root);
    }

    private void startGame() {
        startActivity(new Intent(this, GameActivity.class));
        finish();
    }

    /** The packer needs room for the pack (about half the files) next to the files. */
    private void startCompression(long copied) {
        long free = new StatFs(getExternalFilesDir(null).getPath()).getAvailableBytes();
        if (free < copied * 6 / 10 + (100L << 20)) {
            status.setText(String.format(Locale.US, "Not enough free storage to compress (%d MB free, about %d MB needed). "
                    + "Starting the game uncompressed...", free >> 20, (copied * 6 / 10 >> 20) + 100));
            status.postDelayed(this::startGame, 4000);
            return;
        }
        startActivity(new Intent(this, PackActivity.class));
        finish();
    }

    private void setStatus(final String s) {
        runOnUiThread(() -> status.setText(s));
    }

    // ---- bundled pack (assets/game.pak) ------------------------------------------------

    private boolean hasBundledPack() {
        try (InputStream in = getAssets().open("game.pak")) {
            return true;
        } catch (IOException e) {
            return false;
        }
    }

    // ---- files picked with the Storage Access Framework ------------------------------------

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQ_PICK_FOLDER || resultCode != RESULT_OK || data == null) return;
        final Uri tree = data.getData();
        if (tree == null) return;
        pickButton.setEnabled(false);
        compressBox.setEnabled(false);
        busy = true;
        // the import takes minutes: keep the screen on, or the phone locks and Android
        // won't let the app move on to the next screen until it's unlocked
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        new Thread(() -> {
            try {
                File tmp = new File(getExternalFilesDir(null), "game.tmp");
                deleteTree(tmp);
                tmp.mkdirs();
                String rootId = DocumentsContract.getTreeDocumentId(tree);
                final long copied = copyDocumentTree(tree, rootId, tmp, 0);
                // accept either the game folder itself or a parent containing it
                File src = tmp;
                if (!hasRequired(tmp)) {
                    File[] subs = tmp.listFiles();
                    if (subs != null) {
                        for (File s : subs) {
                            if (s.isDirectory() && hasRequired(s)) src = s;
                        }
                    }
                }
                deleteTree(gameDir());
                if (!src.renameTo(gameDir())) throw new IOException("rename failed");
                deleteTree(tmp);
                runOnUiThread(() -> {
                    busy = false;
                    getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                    compressBox.setEnabled(true);
                    if (gameInstalled()) {
                        if (compressBox.isChecked()) startCompression(copied);
                        else startGame();
                    } else {
                        status.setText("That folder does not contain the Windows version of "
                                + BuildConfig.GAME_TITLE + " (" + requiredList()
                                + " not found). Please select the game folder.");
                        pickButton.setEnabled(true);
                    }
                });
            } catch (Exception e) {
                setStatus("Copy failed: " + e.getMessage());
                runOnUiThread(() -> {
                    pickButton.setEnabled(true);
                    compressBox.setEnabled(true);
                    getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                });
                busy = false;
            }
        }).start();
    }

    /** Copies the picked folder: lists the tree, then copies the files on several threads
     *  (the storage framework's per-file overhead, not bandwidth, dominates with thousands
     *  of small files).  Returns the bytes copied. */
    private long copyDocumentTree(Uri tree, String rootId, File dst, long ignored) throws IOException {
        final java.util.List<Uri> docs = new java.util.ArrayList<>();
        final java.util.List<File> outs = new java.util.ArrayList<>();
        listTree(tree, rootId, dst, docs, outs);
        final java.util.concurrent.atomic.AtomicLong done = new java.util.concurrent.atomic.AtomicLong();
        final java.util.concurrent.atomic.AtomicLong reported = new java.util.concurrent.atomic.AtomicLong();
        final java.util.concurrent.atomic.AtomicReference<IOException> error = new java.util.concurrent.atomic.AtomicReference<>();
        java.util.concurrent.ExecutorService pool = java.util.concurrent.Executors.newFixedThreadPool(4);
        for (int i = 0; i < docs.size(); i++) {
            final Uri doc = docs.get(i);
            final File out = outs.get(i);
            pool.execute(() -> {
                if (error.get() != null) return;
                byte[] buf = new byte[1 << 16];
                try (InputStream in = getContentResolver().openInputStream(doc);
                     OutputStream o = new FileOutputStream(out)) {
                    int n;
                    while (in != null && (n = in.read(buf)) > 0) {
                        o.write(buf, 0, n);
                        long d = done.addAndGet(n), r = reported.get();
                        if (d - r > (2 << 20) && reported.compareAndSet(r, d))
                            setStatus(String.format(Locale.US, "Copying game files... %d MB", d >> 20));
                    }
                } catch (IOException e) {
                    error.compareAndSet(null, e);
                }
            });
        }
        pool.shutdown();
        try {
            pool.awaitTermination(1, java.util.concurrent.TimeUnit.DAYS);
        } catch (InterruptedException e) {
            throw new IOException("interrupted");
        }
        if (error.get() != null) throw error.get();
        return done.get();
    }

    private void listTree(Uri tree, String docId, File dst, java.util.List<Uri> docs, java.util.List<File> outs) {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId);
        String[] cols = {
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
        };
        dst.mkdirs();
        try (Cursor c = getContentResolver().query(children, cols, null, null, null)) {
            if (c == null) return;
            while (c.moveToNext()) {
                String id = c.getString(0), name = c.getString(1), mime = c.getString(2);
                File out = new File(dst, name);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    // folders the game never opens ([android] copy_skip)
                    if (skipDir(name)) continue;
                    listTree(tree, id, out, docs, outs);
                } else {
                    docs.add(DocumentsContract.buildDocumentUriUsingTree(tree, id));
                    outs.add(out);
                }
            }
        }
        if (docs.size() % 2000 == 0) setStatus("Listing the game folder... " + docs.size() + " files");
    }

    // ---- helpers ------------------------------------------------------------------------------

    private static boolean skipDir(String name) {
        for (String s : BuildConfig.COPY_SKIP) if (s.equalsIgnoreCase(name)) return true;
        return false;
    }

    private static void deleteTree(File f) {
        File[] l = f.listFiles();
        if (l != null) for (File c : l) deleteTree(c);
        f.delete();
    }

    @Override
    public void onBackPressed() {
        if (!busy) super.onBackPressed();
    }
}
