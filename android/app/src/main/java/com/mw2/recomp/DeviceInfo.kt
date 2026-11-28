package com.mw2.recomp

import android.app.ActivityManager
import android.app.ApplicationExitInfo
import android.content.Context
import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.GLES20
import android.os.Build
import androidx.annotation.RequiresApi
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * What a report from a phone nobody here has must say without being asked:
 * which app, which phone, which chip and GPU, how much memory, whether the
 * game's code is there, and how the system says the last runs ended.
 *
 * It is the head of a shared log (LogSummary.whole) and the first lines of
 * each process's own log, so a log cut out of a share still names the device.
 */
object DeviceInfo {

    /** How many of the system's records of ended processes a share carries. */
    private const val kExits = 6

    /** Native crashes whose backtrace is printed: the newest say the most. */
    private const val kTombstones = 2

    /** A tombstone carries memory dumps and logs; the backtrace is at its front. */
    private const val kTombstoneBytes = 8 shl 20

    /** One fact a line. Never throws: a fact that cannot be read says so. */
    fun lines(context: Context): List<String> = listOfNotNull(
        "build: ${context.getString(R.string.app_name)} ${BuildConfig.VERSION_NAME}" +
            " (code ${BuildConfig.VERSION_CODE}, ${context.packageName}, built ${BuildConfig.BUILD_STAMP})",
        "phone: ${Build.MANUFACTURER} ${Build.MODEL} (${Build.DEVICE}), Android ${Build.VERSION.RELEASE}" +
            " (API ${Build.VERSION.SDK_INT})",
        "chip: ${chip()}",
        "memory: " + said { memory(context) },
        "gpu: " + said { gpu(context) },
        "game code: " + said { GameLibrary.describe(context) },
        if (Dlc.offered) "map packs: " + said { Dlc.describe(context) } else null,
    )

    private fun said(fact: () -> String): String = try {
        fact()
    } catch (error: Throwable) {
        "could not be read ($error)"
    }

    /** The same lines into this process's log; never the reason a start fails. */
    fun log(context: Context) {
        try {
            lines(context).forEach { NativeBridge.nativeLog(false, it) }
        } catch (_: Throwable) {
            // A library without the call: the share still has the lines.
        }
    }

    private fun chip(): String {
        val named = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
            "${Build.SOC_MANUFACTURER} ${Build.SOC_MODEL}, " else ""
        return "${named}hardware ${Build.HARDWARE}, board ${Build.BOARD}, ${Build.SUPPORTED_ABIS.joinToString("/")}"
    }

    private fun memory(context: Context): String {
        val info = ActivityManager.MemoryInfo()
        context.getSystemService(ActivityManager::class.java).getMemoryInfo(info)
        val mb = 1024L * 1024L
        return "${info.totalMem / mb} MB, ${info.availMem / mb} MB available" +
            " (the system starts killing below ${info.threshold / mb} MB)" +
            if (info.lowMemory) ", and it is short now" else ""
    }

    /**
     * The GPU's name and its driver's version, with the Vulkan the system
     * declares. Asked of OpenGL ES, on a context of one pixel that is gone
     * again at once: its strings name the same chip and the same driver
     * release as the vendor's Vulkan driver, and asking does not open the
     * Vulkan driver -- so a phone whose run dies in that driver has still
     * said what it is. Kept in a file, for the run's process to quote without
     * a context of its own, and asked again after a system update.
     */
    fun gpu(context: Context): String {
        val kept = File(context.filesDir, "gpu.txt")
        val stamp = Build.FINGERPRINT
        try {
            val lines = kept.readLines()
            if (lines.size == 2 && lines[0] == stamp) return lines[1]
        } catch (_: Throwable) {
            // Not asked yet.
        }
        val vulkan = context.packageManager.systemAvailableFeatures
            .firstOrNull { it.name == "android.hardware.vulkan.version" }?.version ?: 0
        val declared = if (vulkan > 0)
            "Vulkan ${vulkan shr 22}.${(vulkan shr 12) and 0x3ff}.${vulkan and 0xfff} declared"
        else "no Vulkan declared"
        val text = "${glStrings() ?: "OpenGL ES would not say"}; $declared"
        try {
            kept.writeText("$stamp\n$text")
        } catch (_: Throwable) {
            // Asked again next time.
        }
        return text
    }

    /** "<renderer> (<vendor>), <version>", or null when no context could be made. */
    private fun glStrings(): String? = try {
        val display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
        val version = IntArray(2)
        if (!EGL14.eglInitialize(display, version, 0, version, 1)) null
        else {
            val configs = arrayOfNulls<EGLConfig>(1)
            val count = IntArray(1)
            EGL14.eglChooseConfig(
                display,
                intArrayOf(
                    EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                    EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT,
                    EGL14.EGL_NONE,
                ),
                0, configs, 0, 1, count, 0,
            )
            val config = configs[0]
            if (count[0] < 1 || config == null) null
            else {
                val glContext = EGL14.eglCreateContext(
                    display, config, EGL14.EGL_NO_CONTEXT,
                    intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE), 0,
                )
                val surface = EGL14.eglCreatePbufferSurface(
                    display, config,
                    intArrayOf(EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE), 0,
                )
                val said = if (EGL14.eglMakeCurrent(display, surface, surface, glContext))
                    "${GLES20.glGetString(GLES20.GL_RENDERER)} (${GLES20.glGetString(GLES20.GL_VENDOR)}), " +
                        GLES20.glGetString(GLES20.GL_VERSION)
                else null
                EGL14.eglMakeCurrent(
                    display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT,
                )
                EGL14.eglDestroySurface(display, surface)
                EGL14.eglDestroyContext(display, glContext)
                said
            }
        }
    } catch (_: Throwable) {
        null
    }

    // ---- how the system says the last runs ended ---------------------------

    /**
     * The system's own record of this app's processes that ended, newest
     * first: killed for memory, a native crash, stopped by the user, or
     * exited by itself, with the memory each held. It is the only account
     * there is of a process that was killed, which writes nothing itself.
     * For a native crash the system keeps a tombstone (Android 12 on), and
     * its signal and the crashing thread's backtrace are printed under the
     * record. Empty before Android 11, which keeps no such record.
     */
    fun exits(context: Context): String {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return ""
        // Whole, or what there was before something failed: a report that
        // cannot be made must not take the Log screen with it.
        val text = StringBuilder()
        try {
            val records = context.getSystemService(ActivityManager::class.java)
                .getHistoricalProcessExitReasons(context.packageName, 0, kExits)
            var tombstones = 0
            for (record in records) {
                text.append(describe(record)).append('\n')
                if (record.reason != ApplicationExitInfo.REASON_CRASH_NATIVE) continue
                if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S || tombstones >= kTombstones) continue
                tombstones++
                val trace = try {
                    record.traceInputStream?.use { Tombstone.read(it.readNBytesCompat(kTombstoneBytes)) }
                } catch (_: Throwable) {
                    null
                }
                // Not every crash leaves one an app may read: the Retroid
                // Pocket Flip 2 kept one for one crash of two.
                text.append(
                    trace ?: "    (this system kept no tombstone to read; the fault's report in game.log has the backtrace)\n",
                )
            }
        } catch (error: Throwable) {
            text.append("(the record could not be read: ").append(error).append(")\n")
        }
        return text.toString()
    }

    /** The newest record of the run's process, in one line, or "". */
    fun lastRunExit(context: Context): String {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return ""
        return try {
            context.getSystemService(ActivityManager::class.java)
                .getHistoricalProcessExitReasons(context.packageName, 0, kExits)
                .firstOrNull { it.processName.endsWith(":game") }?.let(::describe) ?: ""
        } catch (_: Throwable) {
            ""
        }
    }

    /**
     * The run's newest end, where it was a native crash and the system kept
     * its tombstone (Android 12 on): when, and the crashing thread's
     * backtrace as Tombstone.read writes it. Null otherwise, an end that was
     * no crash included.
     */
    fun lastRunCrash(context: Context): Pair<Long, String>? {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) return null
        return try {
            val record = context.getSystemService(ActivityManager::class.java)
                .getHistoricalProcessExitReasons(context.packageName, 0, kExits)
                .firstOrNull { it.processName.endsWith(":game") } ?: return null
            if (record.reason != ApplicationExitInfo.REASON_CRASH_NATIVE) return null
            val trace = record.traceInputStream?.use { Tombstone.read(it.readNBytesCompat(kTombstoneBytes)) } ?: return null
            record.timestamp to trace
        } catch (_: Throwable) {
            null
        }
    }

    @RequiresApi(Build.VERSION_CODES.R)
    private fun describe(record: ApplicationExitInfo): String {
        val status = when (record.reason) {
            ApplicationExitInfo.REASON_SIGNALED, ApplicationExitInfo.REASON_CRASH_NATIVE ->
                "signal ${record.status}"
            ApplicationExitInfo.REASON_EXIT_SELF -> "exit code ${record.status}"
            else -> "status ${record.status}"
        }
        val mb = 1024L
        return SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(Date(record.timestamp)) +
            " ${record.processName} (pid ${record.pid}): ${reason(record.reason)}, $status," +
            " rss ${record.rss / mb} MB, pss ${record.pss / mb} MB, importance ${record.importance}" +
            (record.description?.let { ", \"$it\"" } ?: "")
    }

    @RequiresApi(Build.VERSION_CODES.R)
    private fun reason(reason: Int): String = when (reason) {
        ApplicationExitInfo.REASON_EXIT_SELF -> "exited by itself"
        ApplicationExitInfo.REASON_SIGNALED -> "killed by a signal"
        ApplicationExitInfo.REASON_LOW_MEMORY -> "killed for memory"
        ApplicationExitInfo.REASON_CRASH -> "crashed (Java)"
        ApplicationExitInfo.REASON_CRASH_NATIVE -> "crashed (native)"
        ApplicationExitInfo.REASON_ANR -> "not responding"
        ApplicationExitInfo.REASON_INITIALIZATION_FAILURE -> "failed to initialise"
        ApplicationExitInfo.REASON_PERMISSION_CHANGE -> "a permission changed"
        ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE -> "killed for excessive resource use"
        ApplicationExitInfo.REASON_USER_REQUESTED -> "stopped by the user"
        ApplicationExitInfo.REASON_USER_STOPPED -> "the user was stopped"
        ApplicationExitInfo.REASON_DEPENDENCY_DIED -> "a dependency died"
        ApplicationExitInfo.REASON_OTHER -> "killed by the system (other)"
        else -> "reason $reason"
    }

    /** InputStream.readNBytes is Android 13's; a tombstone is read on 12. */
    private fun java.io.InputStream.readNBytesCompat(limit: Int): ByteArray {
        val out = java.io.ByteArrayOutputStream()
        val buffer = ByteArray(64 * 1024)
        while (out.size() < limit) {
            val got = read(buffer, 0, minOf(buffer.size, limit - out.size()))
            if (got <= 0) break
            out.write(buffer, 0, got)
        }
        return out.toByteArray()
    }
}
