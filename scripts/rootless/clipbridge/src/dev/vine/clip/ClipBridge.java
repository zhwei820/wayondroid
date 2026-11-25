package dev.vine.clip;

import android.content.ClipData;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.PersistableBundle;
import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;

/**
 * The guest's clipboard on a unix socket, for the engine. Runs as `shell` under app_process (vine-rootless.rc): only a
 * process inside init's session gets ART's linker namespaces, and only a uid that owns the package it names may read
 * the clipboard (com.android.shell also reads it in the background).
 *
 * One client at a time, UTF-8 lines with \\ \n \r \t escaped:
 *   in:  "set <text>"   the guest's clipboard becomes text
 *   out: "clip <text>"  the guest's clipboard, on connect and on every change
 */
public final class ClipBridge {
    private static final String PKG = "com.android.shell";
    private static final int MAX_CHARS = 512 * 1024;

    private final Object clipboard;
    private final Method get, set, addListener;
    private final Object lock = new Object();
    private OutputStream client;
    private String lastSent;

    private ClipBridge() throws Exception {
        IBinder b = (IBinder) Class.forName("android.os.ServiceManager").getMethod("getService", String.class).invoke(null, "clipboard");
        if (b == null) throw new IllegalStateException("no clipboard service");
        clipboard = Class.forName("android.content.IClipboard$Stub").getMethod("asInterface", IBinder.class).invoke(null, b);
        get = method("getPrimaryClip");
        set = method("setPrimaryClip");
        addListener = method("addPrimaryClipChangedListener");
    }

    private Method method(String name) throws NoSuchMethodException {
        for (Method m : clipboard.getClass().getMethods()) if (m.getName().equals(name)) return m;
        throw new NoSuchMethodException(name);
    }

    // The leading object (clip, listener) and then (pkg, attributionTag, userId) as this release declares them
    private static Object[] args(Method m, Object first) {
        Class<?>[] t = m.getParameterTypes();
        Object[] a = new Object[t.length];
        int i = 0;
        if (first != null) a[i++] = first;
        boolean pkgDone = false;
        for (; i < t.length; i++) {
            if (t[i] == String.class) {
                a[i] = pkgDone ? null : PKG;
                pkgDone = true;
            } else if (t[i] == int.class) {
                a[i] = 0;
            }
        }
        return a;
    }

    private String read() {
        try {
            ClipData c = (ClipData) get.invoke(clipboard, args(get, null));
            if (c == null || c.getItemCount() == 0) return null;
            CharSequence text = c.getItemAt(0).getText();
            if (text == null) return null;
            String s = text.toString();
            return s.length() > MAX_CHARS ? null : s;
        } catch (Exception e) {
            System.err.println("vine-clip: read: " + e);
            return null;
        }
    }

    private void write(String text) {
        try {
            ClipData clip = ClipData.newPlainText("vine", text);
            // SystemUI shows no clipboard overlay (share / edit) for a clip of com.android.shell that asks it not to
            PersistableBundle extras = new PersistableBundle();
            extras.putBoolean("com.android.systemui.SUPPRESS_CLIPBOARD_OVERLAY", true);
            clip.getDescription().setExtras(extras);
            set.invoke(clipboard, args(set, clip));
        } catch (Exception e) {
            System.err.println("vine-clip: write: " + e);
        }
    }

    // Changes fire more than once per set: only a new text goes out
    private void publish() {
        String text = read();
        synchronized (lock) {
            if (text == null || text.equals(lastSent) || client == null) return;
            try {
                client.write(("clip " + escape(text) + "\n").getBytes(StandardCharsets.UTF_8));
                client.flush();
                lastSent = text;
            } catch (IOException e) {
                client = null;
            }
        }
    }

    private void listen() throws Exception {
        final String descriptor = "android.content.IOnPrimaryClipChangedListener";
        Binder listener = new Binder() {
            @Override
            protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
                if (code == FIRST_CALL_TRANSACTION) {
                    publish();
                    return true;
                }
                if (code == INTERFACE_TRANSACTION) {
                    reply.writeString(descriptor);
                    return true;
                }
                return false;
            }
        };
        Object proxy = Class.forName(descriptor + "$Stub").getMethod("asInterface", IBinder.class).invoke(null, listener);
        addListener.invoke(clipboard, args(addListener, proxy));
    }

    private void serve(String path) throws IOException {
        new java.io.File(path).delete();
        LocalSocket bound = new LocalSocket();
        bound.bind(new LocalSocketAddress(path, LocalSocketAddress.Namespace.FILESYSTEM));
        LocalServerSocket server = new LocalServerSocket(bound.getFileDescriptor());
        for (;;) {
            LocalSocket s = server.accept();
            synchronized (lock) {
                client = s.getOutputStream();
                lastSent = null;
            }
            publish();
            try (BufferedReader in = new BufferedReader(new InputStreamReader(s.getInputStream(), StandardCharsets.UTF_8))) {
                for (String line; (line = in.readLine()) != null; ) {
                    if (line.startsWith("set ")) {
                        String text = unescape(line.substring(4));
                        // What the host sent is what it has: no echo back
                        synchronized (lock) {
                            lastSent = text;
                        }
                        write(text);
                    }
                }
            } catch (IOException ignored) {
            }
            synchronized (lock) {
                client = null;
            }
            s.close();
        }
    }

    static String escape(String s) {
        StringBuilder b = new StringBuilder(s.length());
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '\n': b.append("\\n"); break;
                case '\r': b.append("\\r"); break;
                case '\t': b.append("\\t"); break;
                case '\\': b.append("\\\\"); break;
                default: b.append(c);
            }
        }
        return b.toString();
    }

    static String unescape(String s) {
        StringBuilder b = new StringBuilder(s.length());
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '\\' && i + 1 < s.length()) {
                char n = s.charAt(++i);
                b.append(n == 'n' ? '\n' : n == 'r' ? '\r' : n == 't' ? '\t' : n);
            } else {
                b.append(c);
            }
        }
        return b.toString();
    }

    public static void main(String[] argv) throws Exception {
        if (argv.length != 1) throw new IllegalArgumentException("usage: ClipBridge <socket path>");
        ClipBridge bridge = new ClipBridge();
        bridge.listen();
        bridge.serve(argv[0]);
    }
}
