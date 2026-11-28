package com.mw2.recomp

import android.app.ActivityManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.IBinder
import android.os.Process
import android.os.SystemClock
import java.io.File
import kotlin.concurrent.thread

/**
 * Whether a driver can run the renderer, found out before the game is
 * started on it (runtime/gpu/vulkan/selftest.cpp): the device made as a run
 * makes it, one frame drawn off screen with shaders built as the renderer's
 * are, and its pixels compared with what they must be.
 *
 * In a process of its own (":drivertest", [DriverTestService]): a driver is
 * opened once in a process's life, and one that crashes or hangs takes that
 * process and not the launcher. The process writes each step to a file as
 * it starts it, so one that died has said how far it got.
 */
object DriverSelfTest {

    /** What a test said. [device] and the rest are the Vulkan driver's own account of itself, "" where it never got that far. */
    data class Result(
        val passed: Boolean,
        /** In English, as the log is: why it failed, or "". */
        val reason: String,
        val device: String = "",
        val vendorId: String = "",
        val deviceId: String = "",
        val driverName: String = "",
        val driverInfo: String = "",
        val api: String = "",
        val opened: String = "",
    ) {
        /** One line for the log and a shared report. */
        fun line(): String = buildString {
            append(if (passed) "passed" else "FAILED: $reason")
            if (device.isNotEmpty()) append("; $device (vendor $vendorId device $deviceId), $driverName $driverInfo, Vulkan $api")
        }
    }

    /**
     * The verdict in a report file. A file without a result line is a
     * process that died or hung ([ended]: it is known to be gone): the last
     * step it began is where.
     */
    fun parse(report: String, ended: Boolean): Result {
        val values = HashMap<String, String>()
        var stage = ""
        for (line in report.lineSequence()) {
            val at = line.indexOf('=')
            if (at <= 0) continue
            val key = line.substring(0, at)
            val value = line.substring(at + 1)
            if (key == "stage") stage = value else values[key] = value
        }
        val result = values["result"]
        val reason = when {
            result == "pass" -> ""
            result != null -> result.removePrefix("fail:").ifEmpty { "unknown" }
            stage.isEmpty() -> if (ended) "the test's process ended before it began" else "the test did not start"
            else -> "the driver ${if (ended) "crashed" else "hung"} at ${stageName(stage)}"
        }
        return Result(
            passed = result == "pass",
            reason = reason,
            device = values["device"].orEmpty(),
            vendorId = values["vendor"].orEmpty(),
            deviceId = values["deviceId"].orEmpty(),
            driverName = values["driverName"].orEmpty(),
            driverInfo = values["driverInfo"].orEmpty(),
            api = values["api"].orEmpty(),
            opened = values["driver"].orEmpty(),
        )
    }

    private fun stageName(stage: String): String = when (stage) {
        "open" -> "loading"
        "instance" -> "vkCreateInstance"
        "device" -> "making the device"
        "shaders" -> "the shaders"
        "pipelines" -> "compiling the pipelines"
        "draw" -> "drawing the test frame"
        "compare" -> "reading the frame back"
        else -> stage
    }

    private const val TIMEOUT_MS = 25_000L
    private val lock = Any()

    /** How the reason of a test that never ran begins: it says nothing of the driver. */
    const val NOT_STARTED = "the test could not be started"

    private fun reportFile(context: Context) = File(context.filesDir, "driver-selftest.txt")

    /**
     * Tests [driver], or the phone's own driver for null. Blocks for as long
     * as the test takes, a few seconds, and [TIMEOUT_MS] at the most: not
     * for the main thread. One test at a time.
     */
    fun run(context: Context, driver: DriverStore.Driver?): Result = synchronized(lock) {
        val app = context.applicationContext
        val report = reportFile(app)
        endProcess(app)
        report.delete()
        val intent = Intent(app, DriverTestService::class.java)
            .putExtra(DriverTestService.DIRECTORY, driver?.directory?.absolutePath.orEmpty())
            .putExtra(DriverTestService.LIBRARY, driver?.library.orEmpty())
            .putExtra(DriverTestService.REPORT, report.absolutePath)
        try {
            app.startService(intent)
        } catch (error: Exception) {
            // The app is not in front (Android 8 on): asked again when it is.
            return Result(false, "$NOT_STARTED (${error.message})")
        }
        val began = SystemClock.elapsedRealtime()
        var seen = false
        var ended = false
        while (SystemClock.elapsedRealtime() - began < TIMEOUT_MS) {
            Thread.sleep(100)
            val text = read(report)
            if (text.contains("\nresult=") || text.startsWith("result=")) break
            val alive = processId(app) != null
            seen = seen || alive
            // Gone without a verdict, after it was seen or should long have been.
            if (!alive && (seen || SystemClock.elapsedRealtime() - began > 5_000)) {
                ended = true
                break
            }
        }
        // What the process wrote in its last moment.
        if (ended) Thread.sleep(150)
        val result = parse(read(report), ended)
        endProcess(app)
        result
    }

    private fun read(file: File): String = try {
        file.readText()
    } catch (_: Exception) {
        ""
    }

    private fun processId(context: Context): Int? {
        val name = context.packageName + DriverTestService.PROCESS
        return context.getSystemService(ActivityManager::class.java).runningAppProcesses
            ?.firstOrNull { it.processName == name }?.pid
    }

    /** The test's process, ended: the next test opens its driver in a new one. */
    private fun endProcess(context: Context) {
        try {
            context.stopService(Intent(context, DriverTestService::class.java))
        } catch (_: Exception) {
        }
        processId(context)?.let { Process.killProcess(it) }
    }
}

/**
 * The self-test's process: loads the runtime, selects the driver it was
 * handed, runs the test, and ends itself. It shows nothing and keeps
 * nothing; what it found is in the report file.
 */
class DriverTestService : Service() {

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val directory = intent?.getStringExtra(DIRECTORY).orEmpty()
        val library = intent?.getStringExtra(LIBRARY).orEmpty()
        val report = intent?.getStringExtra(REPORT).orEmpty()
        // One test in a process's life.
        if (report.isEmpty() || started) return START_NOT_STICKY
        started = true
        thread(name = "driver-selftest") {
            try {
                if (!NativeBridge.launcher(this)) {
                    File(report).writeText("result=fail:the runtime would not load (${NativeBridge.loadError()})\n")
                } else {
                    NativeBridge.nativeSelectDriver(directory, library)
                    NativeBridge.nativeDriverSelfTest(report)
                }
            } catch (error: Throwable) {
                try {
                    File(report).appendText("result=fail:${error.message ?: error.toString()}\n")
                } catch (_: Throwable) {
                }
            }
            Process.killProcess(Process.myPid())
        }
        return START_NOT_STICKY
    }

    companion object {
        const val PROCESS = ":drivertest"
        const val DIRECTORY = "directory"
        const val LIBRARY = "library"
        const val REPORT = "report"

        @Volatile
        private var started = false
    }
}
