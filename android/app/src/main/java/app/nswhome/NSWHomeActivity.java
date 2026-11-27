package app.nswhome;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.ClipData;
import android.content.ComponentName;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.UriPermission;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.provider.OpenableColumns;
import android.util.Log;
import android.view.DisplayCutout;
import android.view.KeyEvent;
import android.view.WindowInsets;
import android.widget.Toast;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLControllerManager;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;

/**
 * The one activity of NSW-Home. SDL's activity gives it the surface, the input
 * and the life cycle; this class adds what the menu needs from Android itself
 * and the native code (src/port/android) reaches through static methods:
 * the system's folder and file pickers, reading the folders the user granted,
 * finding the installed emulators and starting a game in one of them.
 *
 * Every static method here may be called from a native thread.
 */
public class NSWHomeActivity extends SDLActivity {
    private static final String TAG = "NSW-Home";
    private static final String STORAGE_AUTHORITY = "com.android.externalstorage.documents";
    private static final int PICK_FOLDER = 0, PICK_FILE = 1, PICK_IMAGE = 2, PICK_KEYS = 3;
    private static final int REQUEST_PICK = 0x444B;
    private static final String DEBUG_ACTION = "app.nswhome.DEBUG";

    private static volatile NSWHomeActivity self;
    private static final Object pickLock = new Object();
    private static boolean pickDone = true;
    private static String pickResult = "";
    private static int pickKind;
    private BroadcastReceiver debugReceiver;

    /** Debug builds only: a command from adb for the native side (see main_android.cpp). */
    private static native void nativeDebug(String command, String argument);

    /** A gamepad button that did not come from a device SDL treats as a controller. */
    private static native void nativePadKey(int keyCode, boolean down);

    /**
     * Some handhelds report their built-in buttons through a keyboard-type
     * device, and so does `adb shell input keyevent`. SDL only takes gamepad
     * buttons from devices it has listed as controllers and drops these; the
     * menu takes them here instead.
     */
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        int code = event.getKeyCode();
        if (KeyEvent.isGamepadButton(code) && !SDLControllerManager.isDeviceSDLJoystick(event.getDeviceId())) {
            try {
                if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0) nativePadKey(code, true);
                else if (event.getAction() == KeyEvent.ACTION_UP) nativePadKey(code, false);
            } catch (UnsatisfiedLinkError e) {
                return super.dispatchKeyEvent(event); // the native library did not load
            }
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    /**
     * How far the display's cut-outs reach into the window from the left, top,
     * right and bottom, in pixels. SDL lets a full-screen window run under them.
     */
    public static int[] safeInsets() {
        int[] out = {0, 0, 0, 0};
        NSWHomeActivity a = self;
        if (a == null || Build.VERSION.SDK_INT < 28) return out;
        try {
            WindowInsets insets = a.getWindow().getDecorView().getRootWindowInsets();
            DisplayCutout cut = insets == null ? null : insets.getDisplayCutout();
            if (cut != null) {
                out[0] = cut.getSafeInsetLeft();
                out[1] = cut.getSafeInsetTop();
                out[2] = cut.getSafeInsetRight();
                out[3] = cut.getSafeInsetBottom();
            }
        } catch (Exception e) {
            // no insets yet: the whole window counts as safe
        }
        return out;
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL2", "main"};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        self = this;
        super.onCreate(savedInstanceState);
        // With a gamepad the window is not in touch mode, and Android then
        // frames whichever view has the input focus: the drawing surface, and
        // the invisible field SDL puts behind the system keyboard. Those views
        // are made in code, where the theme's setting does not reach them.
        if (Build.VERSION.SDK_INT >= 26) {
            getWindow().getDecorView().getViewTreeObserver().addOnGlobalFocusChangeListener((before, now) -> {
                if (now != null) now.setDefaultFocusHighlightEnabled(false);
            });
        }
        if (isDebuggable()) {
            debugReceiver = new BroadcastReceiver() {
                @Override
                public void onReceive(Context context, Intent intent) {
                    forwardDebug(intent);
                }
            };
            IntentFilter filter = new IntentFilter(DEBUG_ACTION);
            if (Build.VERSION.SDK_INT >= 33) registerReceiver(debugReceiver, filter, Context.RECEIVER_EXPORTED);
            else registerReceiver(debugReceiver, filter);
        }
    }

    @Override
    protected void onDestroy() {
        if (debugReceiver != null) unregisterReceiver(debugReceiver);
        debugReceiver = null;
        // A picker the native side is waiting for will never answer now; it
        // must stop waiting before SDL waits for the native thread to end.
        finishPick("");
        super.onDestroy();
        // The native side keeps its state in globals that are set up once per
        // process. A new activity in this process would start on the leftovers
        // of the old one, so the process ends with its activity.
        System.exit(0);
    }

    private boolean isDebuggable() {
        return (getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0;
    }

    private void forwardDebug(Intent intent) {
        Bundle extras = intent.getExtras();
        if (extras == null) return;
        for (String key : extras.keySet()) {
            Object value = extras.get(key);
            if (value != null) nativeDebug(key, value.toString());
        }
    }

    /**
     * Debug builds only: the extras the activity was started with, as
     * "key=value" strings ("data", "goto", "nosetup" ...), for automated runs.
     * Always empty in a release build.
     */
    public static String[] debugExtras() {
        NSWHomeActivity a = self;
        if (a == null || !a.isDebuggable()) return new String[0];
        Bundle extras = a.getIntent().getExtras();
        List<String> out = new ArrayList<>();
        if (extras != null)
            for (String key : extras.keySet()) {
                Object value = extras.get(key);
                if (value != null) out.add(key + "=" + value);
            }
        return out.toArray(new String[0]);
    }

    public static String filesDir() {
        NSWHomeActivity a = self;
        return a == null ? "" : a.getFilesDir().getAbsolutePath();
    }

    public static void quit() {
        final NSWHomeActivity a = self;
        if (a != null) a.runOnUiThread(a::finishAndRemoveTask);
    }

    // ---- pickers ------------------------------------------------------------

    /**
     * Opens the system picker. The answer is fetched with pickPoll() once the
     * activity is in front again: the native loop keeps pumping SDL's events
     * meanwhile, which is where SDL parks a paused app.
     */
    public static void pickStart(final int kind) {
        final NSWHomeActivity a = self;
        synchronized (pickLock) {
            pickDone = a == null;
            pickResult = "";
            pickKind = kind;
        }
        if (a == null) return;
        a.runOnUiThread(() -> {
            try {
                Intent intent;
                if (kind == PICK_FOLDER) {
                    intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
                    intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
                } else {
                    intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                    intent.addCategory(Intent.CATEGORY_OPENABLE);
                    // The key file has no type of its own: every file is offered.
                    intent.setType(kind == PICK_IMAGE ? "image/*" : "*/*");
                    if (kind == PICK_IMAGE) intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"image/jpeg", "image/png"});
                }
                a.startActivityForResult(intent, REQUEST_PICK);
            } catch (Exception e) {
                Log.w(TAG, "no picker: " + e);
                finishPick("");
            }
        });
    }

    /** null while the picker is still open; afterwards the chosen path, or "" when nothing was chosen. */
    public static String pickPoll() {
        synchronized (pickLock) {
            return pickDone ? pickResult : null;
        }
    }

    private static void finishPick(String result) {
        synchronized (pickLock) {
            pickResult = result == null ? "" : result;
            pickDone = true;
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_PICK) return;
        Uri uri = resultCode == Activity.RESULT_OK && data != null ? data.getData() : null;
        if (uri == null) {
            finishPick("");
            return;
        }
        String result = "";
        try {
            if (pickKind == PICK_FOLDER) {
                String path = treePath(uri);
                if (path == null) {
                    toast("Elige una carpeta del almacenamiento del dispositivo o de la tarjeta SD.");
                } else {
                    // Kept across restarts: this grant is how the games are listed and read from now on.
                    getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
                    result = path;
                }
            } else {
                // A single file (the key file, a picture) is copied into the
                // app's private cache and used from there; the native side
                // deletes the copy as soon as it has been read.
                result = copyToCache(uri, pickKind == PICK_KEYS ? "prod.keys" : "picked");
            }
        } catch (Exception e) {
            Log.w(TAG, "pick failed: " + e.getClass().getSimpleName());
        }
        finishPick(result);
    }

    private void toast(final String text) {
        runOnUiThread(() -> Toast.makeText(this, text, Toast.LENGTH_LONG).show());
    }

    private String copyToCache(Uri uri, String name) throws Exception {
        File dir = new File(getCacheDir(), "pick");
        if (!dir.isDirectory() && !dir.mkdirs()) return "";
        if (pickKind == PICK_IMAGE || pickKind == PICK_FILE) {
            // Keep the ending: the picture is recognised by its contents, but a name helps in logs.
            try (Cursor c = getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
                if (c != null && c.moveToFirst() && c.getString(0) != null) {
                    String display = c.getString(0);
                    int dot = display.lastIndexOf('.');
                    if (dot > 0 && display.length() - dot <= 6) name += display.substring(dot).replaceAll("[^A-Za-z0-9.]", "");
                }
            }
        }
        File out = new File(dir, name);
        final long limit = 64L << 20; // no picture or key file is anywhere near this
        long total = 0;
        try (InputStream in = getContentResolver().openInputStream(uri); FileOutputStream os = new FileOutputStream(out)) {
            if (in == null) return "";
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) {
                total += n;
                if (total > limit) break;
                os.write(buf, 0, n);
            }
        }
        if (total > limit) {
            out.delete();
            return "";
        }
        return out.getAbsolutePath();
    }

    // ---- granted folders ------------------------------------------------------
    // A folder granted through the picker is a "tree" of the storage provider,
    // whose document ids are "<volume>:<path inside the volume>". That maps both
    // ways to the place the folder has on the device, which is what the native
    // side shows and stores: "primary:Roms/Switch" <-> /storage/emulated/0/Roms/Switch.

    private static final class Tree {
        Uri uri;
        String docId; // "primary:Roms"
        String path;  // "/storage/emulated/0/Roms"
    }

    private static String volumeRoot(String volume) {
        if ("primary".equalsIgnoreCase(volume)) return Environment.getExternalStorageDirectory().getAbsolutePath();
        return "/storage/" + volume;
    }

    /** The device path of a tree the picker returned, or null when it is not on the storage provider. */
    private static String treePath(Uri tree) {
        if (!STORAGE_AUTHORITY.equals(tree.getAuthority())) return null;
        String id = DocumentsContract.getTreeDocumentId(tree);
        int colon = id.indexOf(':');
        if (colon <= 0) return null;
        String rel = id.substring(colon + 1);
        String root = volumeRoot(id.substring(0, colon));
        return rel.isEmpty() ? root : root + "/" + rel;
    }

    private static List<Tree> trees() {
        List<Tree> out = new ArrayList<>();
        NSWHomeActivity a = self;
        if (a == null) return out;
        for (UriPermission p : a.getContentResolver().getPersistedUriPermissions()) {
            if (!p.isReadPermission()) continue;
            String path;
            try {
                path = treePath(p.getUri());
            } catch (Exception e) {
                continue; // a single document, not a tree
            }
            if (path == null) continue;
            Tree t = new Tree();
            t.uri = p.getUri();
            t.docId = DocumentsContract.getTreeDocumentId(p.getUri());
            t.path = path;
            out.add(t);
        }
        return out;
    }

    /** The folders granted so far, as device paths: the native side tests paths against these. */
    public static String[] grantedRoots() {
        List<Tree> list = trees();
        String[] out = new String[list.size()];
        for (int i = 0; i < out.length; i++) out[i] = list.get(i).path;
        return out;
    }

    /** The document behind a device path inside a granted folder, or null. */
    private static Uri documentFor(String path) {
        Tree best = null;
        for (Tree t : trees()) {
            boolean inside = path.equals(t.path) || path.startsWith(t.path + "/");
            if (inside && (best == null || t.path.length() > best.path.length())) best = t;
        }
        if (best == null) return null;
        String rel = path.length() > best.path.length() ? path.substring(best.path.length() + 1) : "";
        String id = best.docId;
        if (!rel.isEmpty()) id += (id.endsWith(":") ? "" : "/") + rel;
        return DocumentsContract.buildDocumentUriUsingTree(best.uri, id);
    }

    /** One folder's entries as name, "d" or "f", size, modification time (ms), repeated; null on failure. */
    public static String[] listDir(String path) {
        NSWHomeActivity a = self;
        Uri doc = a == null ? null : documentFor(path);
        if (doc == null) return null;
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(doc, DocumentsContract.getDocumentId(doc));
        List<String> out = new ArrayList<>();
        try (Cursor c = a.getContentResolver().query(children,
                new String[] {Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE, Document.COLUMN_SIZE, Document.COLUMN_LAST_MODIFIED},
                null, null, null)) {
            if (c == null) return null;
            while (c.moveToNext()) {
                String name = c.getString(0);
                if (name == null || name.isEmpty()) continue;
                out.add(name);
                out.add(Document.MIME_TYPE_DIR.equals(c.getString(1)) ? "d" : "f");
                out.add(Long.toString(c.isNull(2) ? 0 : c.getLong(2)));
                out.add(Long.toString(c.isNull(3) ? 0 : c.getLong(3)));
            }
        } catch (Exception e) {
            return null;
        }
        return out.toArray(new String[0]);
    }

    /** "d" or "f", size, modification time of one path; null when it does not exist. */
    public static String[] statPath(String path) {
        NSWHomeActivity a = self;
        Uri doc = a == null ? null : documentFor(path);
        if (doc == null) return null;
        try (Cursor c = a.getContentResolver().query(doc,
                new String[] {Document.COLUMN_MIME_TYPE, Document.COLUMN_SIZE, Document.COLUMN_LAST_MODIFIED}, null, null, null)) {
            if (c == null || !c.moveToFirst()) return null;
            return new String[] {Document.MIME_TYPE_DIR.equals(c.getString(0)) ? "d" : "f",
                                 Long.toString(c.isNull(1) ? 0 : c.getLong(1)), Long.toString(c.isNull(2) ? 0 : c.getLong(2))};
        } catch (Exception e) {
            return null;
        }
    }

    /** A read-only file descriptor the caller owns, or -1. */
    public static int openRead(String path) {
        NSWHomeActivity a = self;
        Uri doc = a == null ? null : documentFor(path);
        if (doc == null) return -1;
        try {
            ParcelFileDescriptor pfd = a.getContentResolver().openFileDescriptor(doc, "r");
            return pfd == null ? -1 : pfd.detachFd();
        } catch (Exception e) {
            return -1;
        }
    }

    // ---- emulators ------------------------------------------------------------

    /** The name of the app when `pkg` is installed and has the activity `cls`; null otherwise. */
    public static String emulatorLabel(String pkg, String cls) {
        NSWHomeActivity a = self;
        if (a == null) return null;
        try {
            PackageManager pm = a.getPackageManager();
            if (!pm.getActivityInfo(new ComponentName(pkg, cls), 0).exported) return null;
            return pm.getApplicationLabel(pm.getApplicationInfo(pkg, 0)).toString();
        } catch (Exception e) {
            return null; // not installed, or another app that only shares the package name
        }
    }

    /**
     * Starts `game` (a device path inside a granted folder) in an emulator.
     * The game goes as the intent's data with read permission for the receiver;
     * with `extra` set it goes as that string extra instead (Kenji-NX).
     * Returns "" or an error code.
     */
    public static String launch(String pkg, String cls, String action, String extra, String game) {
        final NSWHomeActivity a = self;
        if (a == null) return "NO_ACTIVITY";
        Uri doc = documentFor(game);
        if (doc == null) return "NO_GRANT";
        try {
            final Intent intent = new Intent(action);
            intent.setComponent(new ComponentName(pkg, cls));
            intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK);
            if (extra == null || extra.isEmpty()) {
                intent.setDataAndType(doc, "application/octet-stream");
            } else {
                intent.putExtra(extra, doc.toString());
                // A grant only travels with an intent's data or clip, never with an extra.
                intent.setClipData(ClipData.newRawUri("game", doc));
                a.grantUriPermission(pkg, doc, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            }
            final String[] error = {""};
            final Object done = new Object();
            final boolean[] finished = {false};
            a.runOnUiThread(() -> {
                try {
                    a.startActivity(intent);
                } catch (Exception e) {
                    error[0] = e.getClass().getSimpleName();
                }
                synchronized (done) {
                    finished[0] = true;
                    done.notifyAll();
                }
            });
            synchronized (done) {
                long until = System.currentTimeMillis() + 3000;
                while (!finished[0] && System.currentTimeMillis() < until) done.wait(100);
            }
            if (isDebuggableStatic()) Log.i(TAG, "launch " + intent + " -> " + (error[0].isEmpty() ? "ok" : error[0]));
            return error[0];
        } catch (Exception e) {
            return e.getClass().getSimpleName();
        }
    }

    private static boolean isDebuggableStatic() {
        NSWHomeActivity a = self;
        return a != null && a.isDebuggable();
    }

    // ---- battery --------------------------------------------------------------

    /** percent * 2 + (charging ? 1 : 0), or -1 when there is no battery. */
    public static int battery() {
        NSWHomeActivity a = self;
        if (a == null) return -1;
        try {
            Intent i = a.registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
            if (i == null || !i.getBooleanExtra(BatteryManager.EXTRA_PRESENT, true)) return -1;
            int level = i.getIntExtra(BatteryManager.EXTRA_LEVEL, -1), scale = i.getIntExtra(BatteryManager.EXTRA_SCALE, 100);
            if (level < 0 || scale <= 0) return -1;
            int status = i.getIntExtra(BatteryManager.EXTRA_STATUS, -1);
            boolean charging = status == BatteryManager.BATTERY_STATUS_CHARGING || status == BatteryManager.BATTERY_STATUS_FULL;
            return level * 100 / scale * 2 + (charging ? 1 : 0);
        } catch (Exception e) {
            return -1;
        }
    }
}
