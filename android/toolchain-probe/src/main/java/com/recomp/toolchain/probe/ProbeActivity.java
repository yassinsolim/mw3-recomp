package com.recomp.toolchain.probe;

import android.app.Activity;
import android.content.pm.ApplicationInfo;
import android.os.Bundle;
import android.view.WindowManager;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.PrintStream;
import java.util.Enumeration;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * Runs one shell command as this app and, if asked, loads a library it made.
 *
 *   adb shell am start -n com.recomp.toolchain.probe/.ProbeActivity \
 *       --es run 'sh $WORK/build.sh' --es load libprobe.so
 *
 * The command runs in the app's files folder with, in its environment,
 *   TOOLS  the toolchain package's library folder, where its programs are
 *   DATA   the folder its data.zip was unpacked into (once for each build of it)
 *   WORK   this app's files folder on shared storage, which adb can write:
 *          the command's inputs are pushed there and its results read there
 * and its output goes to $WORK/probe.log, ending in a line "exit <status>".
 * "load" names a file under the app's own files folder; the log then says
 * whether System.load took it.
 */
public class ProbeActivity extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        TextView text = new TextView(this);
        text.setText("Running; see probe.log");
        setContentView(text);
        // A build is minutes long, and a sleeping phone stops it.
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        String run = getIntent().getStringExtra("run");
        String load = getIntent().getStringExtra("load");
        File work = getExternalFilesDir(null);
        new Thread(() -> {
            try (PrintStream log = new PrintStream(new FileOutputStream(new File(work, "probe.log")), true)) {
                try {
                    probe(run, load, work, log);
                } catch (Throwable error) {
                    error.printStackTrace(log);
                    log.println("exit failed");
                }
            } catch (Exception ignored) {
                // No log to say it in.
            }
            runOnUiThread(() -> text.setText("Done; see probe.log"));
        }).start();
    }

    private void probe(String run, String load, File work, PrintStream log) throws Exception {
        ApplicationInfo toolchain = getPackageManager().getApplicationInfo(BuildConfig.TOOLCHAIN_PACKAGE, 0);
        log.println("toolchain " + toolchain.nativeLibraryDir);
        File data = unpack(toolchain, log);
        if (run != null) {
            ProcessBuilder builder = new ProcessBuilder("sh", "-c", run).directory(getFilesDir()).redirectErrorStream(true);
            builder.environment().put("TOOLS", toolchain.nativeLibraryDir);
            builder.environment().put("DATA", data.getPath());
            builder.environment().put("WORK", work.getPath());
            builder.environment().put("TMPDIR", getCacheDir().getPath());
            Process process = builder.start();
            copy(process.getInputStream(), log);
            log.println("exit " + process.waitFor());
        }
        if (load != null) {
            System.load(new File(getFilesDir(), load).getPath());
            log.println("loaded " + load);
        }
    }

    /** The package's data.zip, unpacked beside this app's files unless this build of it already is. */
    private File unpack(ApplicationInfo toolchain, PrintStream log) throws Exception {
        File data = new File(getFilesDir(), "toolchain-data");
        File stamp = new File(data, ".unpacked");
        String build = new File(toolchain.sourceDir).lastModified() + " " + new File(toolchain.sourceDir).length();
        if (stamp.exists() && new String(java.nio.file.Files.readAllBytes(stamp.toPath())).equals(build)) {
            return data;
        }
        long start = System.currentTimeMillis();
        deleteTree(data);
        File archive = new File(getCacheDir(), "data.zip");
        try (ZipFile apk = new ZipFile(toolchain.sourceDir);
             InputStream in = apk.getInputStream(apk.getEntry("assets/toolchain/data.zip"));
             OutputStream out = new FileOutputStream(archive)) {
            copy(in, out);
        }
        int files = 0;
        try (ZipFile zip = new ZipFile(archive)) {
            for (Enumeration<? extends ZipEntry> entries = zip.entries(); entries.hasMoreElements(); ) {
                ZipEntry entry = entries.nextElement();
                File file = new File(data, entry.getName());
                if (!file.getCanonicalPath().startsWith(data.getCanonicalPath() + File.separator)) {
                    throw new SecurityException("data.zip names a file outside its folder: " + entry.getName());
                }
                file.getParentFile().mkdirs();
                try (InputStream in = zip.getInputStream(entry); OutputStream out = new FileOutputStream(file)) {
                    copy(in, out);
                }
                files++;
            }
        }
        archive.delete();
        java.nio.file.Files.write(stamp.toPath(), build.getBytes());
        log.println("unpacked " + files + " files in " + (System.currentTimeMillis() - start) + " ms");
        return data;
    }

    private static void deleteTree(File file) {
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) {
                deleteTree(child);
            }
        }
        file.delete();
    }

    private static void copy(InputStream in, OutputStream out) throws Exception {
        byte[] buffer = new byte[65536];
        for (int count; (count = in.read(buffer)) > 0; ) {
            out.write(buffer, 0, count);
        }
    }
}
