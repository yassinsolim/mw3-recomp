package com.mw2.recomp

import android.annotation.SuppressLint
import android.app.Activity
import android.app.ActivityManager
import android.content.ComponentCallbacks2
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.res.Configuration
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.hardware.display.DisplayManager
import android.os.BatteryManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.view.Gravity
import android.view.Display
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.os.SystemClock
import java.io.File
import java.util.Locale
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.SeekBar
import android.widget.TextView
import androidx.activity.OnBackPressedCallback
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.widget.SwitchCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import kotlin.math.max
import kotlin.math.roundToInt

/**
 * The run itself: a surface for the renderer, the pad drawn over it, and the
 * few decisions only the app can make -- when to pause, when to let go of
 * the surface, and when the whole thing ends.
 *
 * It lives in its own process (":game" in the manifest). Ending a run means
 * killing that process: the guest is inside recompiled code that cannot be
 * unwound and a driver mid-frame does not survive its device disappearing,
 * so there is nothing to tidy up and everything to lose by trying. Killing
 * the process is instant, cannot fail, and cannot crash -- and the launcher,
 * in the other process, is still there afterwards.
 */
class GameActivity : AppCompatActivity(), NativeListener {

    /**
     * The title this run is: the one whose library the process loaded
     * (onCreate, from the launcher's or the other title's request).
     */
    private val title: Title
        get() = NativeBridge.title() ?: Title.carried.first()

    /** Either game's multiplayer. */
    private val isMultiplayer: Boolean
        get() = title == Title.MULTIPLAYER

    /** A multiplayer run of an app whose multiplayer has bots. */
    private val hasBots: Boolean
        get() = isMultiplayer && Title.hasBots

    /**
     * MW2's multiplayer: the link the app forces up, the Live lobby entries
     * and the stat cheats are for that title only.
     */
    private val isMw2Multiplayer: Boolean
        get() = isMultiplayer && !Title.isMw3

    /** Modern Warfare 3's multiplayer, in the one app or the test copy. */
    private val isMw3Multiplayer: Boolean
        get() = isMultiplayer && Title.isMw3

    /**
     * The titles whose field of view is the runtime's own (MW2_FOV): the
     * multiplayers of Modern Warfare 3 and Advanced Warfare, whose scripts
     * put cg_fov back at every spawn. Advanced Warfare's campaign keeps
     * cg_fov: with the runtime's at the app's 85 degrees Induction's drop
     * failed at the squad mate's prompt on both phones, run after run, where
     * it passes with cg_fov (tools/visualqa, aw-campaign).
     */
    private val fovIsRuntimes: Boolean
        get() = isMw3Multiplayer || (Title.isAw && isMultiplayer)

    /** What the other title left for this run (RunRequest), as hexadecimal. */
    private var launchData = ""

    /** The Special Ops level of a map pack this run starts in (RunRequest), empty for the game's menu. */
    private var contentLevel = ""

    /**
     * Voice chat, for Modern Warfare 3's multiplayer: the player turned it
     * on in Settings and the microphone permission asked for there is still
     * held. Without both the runtime is told nothing and opens no microphone.
     */
    private val voiceChat: Boolean
        get() = isMw3Multiplayer && prefs.voiceChat &&
            checkSelfPermission(android.Manifest.permission.RECORD_AUDIO) ==
            android.content.pm.PackageManager.PERMISSION_GRANTED

    private lateinit var prefs: Prefs
    private lateinit var surfaceView: SurfaceView
    private lateinit var overlay: TouchOverlayView
    private lateinit var root: FrameLayout
    private lateinit var hudContainer: FrameLayout
    private lateinit var statsText: TextView
    private lateinit var hudExpandedCard: LinearLayout
    private lateinit var hudExpandedText: TextView
    private var hudIsExpanded = false
    private lateinit var menuButton: Button
    // Over the black screen while Compile shaders at boot runs (BootCompile).
    private lateinit var bootCard: LinearLayout
    private lateinit var bootText: TextView
    private lateinit var bootNote: TextView
    private lateinit var bootBar: ProgressBar
    private var editPanel: EditPanel? = null

    private val pads = GamepadInput()
    private val gyro: GyroInput by lazy { GyroInput(this) }
    // Between onResume and onPause: the gyroscope is read only then.
    private var inFront = false
    private val handler = Handler(Looper.getMainLooper())
    private var vibrator: Vibrator? = null
    private var lastRumble = 0

    private val statsValues = FloatArray(BootCompile.STATS_VALUES)
    private var started = false

    /**
     * The watch for a black screen (statsTick): when the run started or was
     * last come back to, until a frame of the title's has been shown; and
     * whether the player has been told.
     */
    private var watchingSince = 0L
    private var frameSeen = false
    private var failureShown = false

    /**
     * The rate the runtime holds the run to, as the player was last told, and
     * the rate the display was last asked for a mode of (statsTick).
     */
    private var heldTo = 0
    private var displayAsked = 0
    /** The session as a performance report has it, for a player who chose to send them (PerfReports.kt). */
    private var perfSession: PerfSession? = null

    // The system changes the display's mode under a running game: it grants
    // the rate asked for a moment after the surface is made, and takes it
    // back for the battery or the heat. The runtime paces by the display, so
    // it is told (runtime/frame_rate.h).
    private var displayModeLogged = ""
    private val displayListener = object : DisplayManager.DisplayListener {
        override fun onDisplayAdded(displayId: Int) = Unit
        override fun onDisplayRemoved(displayId: Int) = Unit
        override fun onDisplayChanged(displayId: Int) {
            if (displayId != shownDisplay()?.displayId) return
            logDisplayMode()
            NativeBridge.nativeSetRefreshRate(refreshHz())
        }
    }

    private var cachedRendererInfo = ""

    private var cpuUsagePercent = -1f
    private var lastCpuTotal = 0L
    private var lastCpuIdle = 0L
    private var lastProcTime = 0L
    private var lastProcSampleTime = 0L
    @Volatile private var monitoringActive = true
    private var monitorThread: Thread? = null

    private fun startHardwareMonitor() {
        monitoringActive = true
        monitorThread = Thread {
            while (monitoringActive) {
                sampleCpu()
                try {
                    Thread.sleep(1000)
                } catch (_: InterruptedException) {
                    break
                }
            }
        }.apply {
            isDaemon = true
            name = "mw2-hw-monitor"
            start()
        }
    }

    private fun sampleCpu() {
        try {
            val statFile = File("/proc/stat")
            if (statFile.exists()) {
                val line = statFile.bufferedReader().use { it.readLine() }
                if (line != null && line.startsWith("cpu ")) {
                    val parts = line.split("\\s+".toRegex())
                    if (parts.size >= 5) {
                        val user = parts[1].toLong()
                        val nice = parts[2].toLong()
                        val sys = parts[3].toLong()
                        val idle = parts[4].toLong()
                        val iowait = if (parts.size > 5) parts[5].toLong() else 0L
                        val total = user + nice + sys + idle + iowait
                        val idleTotal = idle + iowait
                        if (lastCpuTotal > 0 && total > lastCpuTotal) {
                            val dTotal = total - lastCpuTotal
                            val dIdle = idleTotal - lastCpuIdle
                            cpuUsagePercent = ((dTotal - dIdle).toFloat() / dTotal.toFloat()) * 100f
                        }
                        lastCpuTotal = total
                        lastCpuIdle = idleTotal
                        return
                    }
                }
            }
        } catch (_: Exception) {}

        try {
            val statFile = File("/proc/self/stat")
            if (statFile.exists()) {
                val line = statFile.readText().trim()
                val rParen = line.lastIndexOf(')')
                if (rParen > 0) {
                    val rest = line.substring(rParen + 2).split(" ")
                    val utime = rest[11].toLong()
                    val stime = rest[12].toLong()
                    val procTime = utime + stime
                    val now = SystemClock.elapsedRealtime()
                    if (lastProcSampleTime > 0 && now > lastProcSampleTime) {
                        val dTimeMs = now - lastProcSampleTime
                        val dTicks = procTime - lastProcTime
                        val dProcMs = dTicks * 10L
                        val cores = Runtime.getRuntime().availableProcessors().coerceAtLeast(1)
                        val pct = (dProcMs.toFloat() / (dTimeMs * cores).toFloat()) * 100f
                        cpuUsagePercent = pct.coerceIn(0f, 100f)
                    }
                    lastProcTime = procTime
                    lastProcSampleTime = now
                    return
                }
            }
        } catch (_: Exception) {}

        cpuUsagePercent = -1f
    }

    private fun getDeviceTemp(): Float {
        var fallbackTemp = 0f
        for (i in 0..19) {
            val typeFile = File("/sys/class/thermal/thermal_zone$i/type")
            val tempFile = File("/sys/class/thermal/thermal_zone$i/temp")
            if (tempFile.exists()) {
                try {
                    val raw = tempFile.readText().trim().toLongOrNull() ?: continue
                    val c = if (raw > 1000) raw / 1000f else raw.toFloat()
                    if (c in 20.0f..105.0f) {
                        val typeName = if (typeFile.exists()) typeFile.readText().trim().lowercase() else ""
                        if (typeName.contains("cpu") || typeName.contains("soc") ||
                            typeName.contains("ap") || typeName.contains("tsens") ||
                            typeName.contains("mtktscpu")) {
                            return c
                        }
                        if (fallbackTemp == 0f) fallbackTemp = c
                    }
                } catch (_: Exception) {}
            }
        }
        if (fallbackTemp > 0f) return fallbackTemp

        try {
            val filter = IntentFilter(Intent.ACTION_BATTERY_CHANGED)
            val batteryIntent = registerReceiver(null, filter)
            val temp = batteryIntent?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0) ?: 0
            if (temp > 0) return temp / 10.0f
        } catch (_: Exception) {}
        return 0f
    }

    private fun getMemoryStats(): Pair<Int, Int> {
        return try {
            val actManager = getSystemService(Context.ACTIVITY_SERVICE) as? ActivityManager
            val memInfo = ActivityManager.MemoryInfo()
            actManager?.getMemoryInfo(memInfo)
            val totalMb = (memInfo.totalMem / (1024 * 1024)).toInt()
            val jvm = Runtime.getRuntime().totalMemory() - Runtime.getRuntime().freeMemory()
            val nativeMem = android.os.Debug.getNativeHeapAllocatedSize()
            val appMb = ((jvm + nativeMem) / (1024 * 1024)).toInt()
            Pair(appMb, totalMb)
        } catch (_: Exception) {
            Pair(0, 0)
        }
    }

    private fun getBatteryStats(): Triple<Float, Boolean, Int> {
        return try {
            val filter = IntentFilter(Intent.ACTION_BATTERY_CHANGED)
            val intent = registerReceiver(null, filter)
            val raw = intent?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0) ?: 0
            val tempC = if (raw > 0) raw / 10.0f else 0.0f
            val status = intent?.getIntExtra(BatteryManager.EXTRA_STATUS, -1) ?: -1
            val isCharging = status == BatteryManager.BATTERY_STATUS_CHARGING ||
                             status == BatteryManager.BATTERY_STATUS_FULL
            val level = intent?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
            Triple(tempC, isCharging, level)
        } catch (_: Exception) {
            Triple(0f, false, -1)
        }
    }

    private val statsTick = object : Runnable {
        override fun run() {
            if (NativeBridge.isLoaded()) {
                NativeBridge.nativeStats(statsValues)
                watchForBlackScreen()
                showBootCompile()
                followHeldRate()
                measureBestLooking()
                perfSession?.tick()
            }
            if (prefs.showStats && NativeBridge.isLoaded()) {
                if (cachedRendererInfo.isEmpty()) {
                    try {
                        cachedRendererInfo = NativeBridge.nativeRendererInfo()
                    } catch (_: Throwable) {
                        cachedRendererInfo = "Vulkan | Mali Fallback | Native"
                    }
                }

                val fps = statsValues[0].roundToInt()
                val texMb = statsValues[1].roundToInt()
                val resW = statsValues[2].roundToInt()
                val resH = statsValues[3].roundToInt()
                val frameTimeMs = if (statsValues[4] > 0.01f) statsValues[4] else if (fps > 0) 1000f / fps else 0f
                val framesPresented = statsValues[5].toLong()

                val warmupQueued = if (statsValues.size >= 7) statsValues[6].toInt() else 0
                val warmupCompleted = if (statsValues.size >= 8) statsValues[7].toInt() else 0
                val warmupLastMs = if (statsValues.size >= 9) statsValues[8].toInt() else 0

                // Other copies heard on the network (multiplayer only). Zero
                // with a friend hosting beside this phone means discovery is
                // not arriving -- same WiFi, firewall, hotspot; see the guide.
                val lanPeers = if (isMultiplayer) {
                    try {
                        NativeBridge.nativeLanPeers()
                    } catch (_: Throwable) {
                        0
                    }
                } else 0

                // The counter: small, and only what says how the game runs
                // -- the frames a second, a frame's time and what holds the
                // rate where it is. Shaders still compiling explain a low
                // number while they last. The rest is in the card a tap opens.
                statsText.text = buildString {
                    append(fps).append(" FPS")
                    if (frameTimeMs > 0f) append(String.format(Locale.US, " · %.1f ms", frameTimeMs))
                    val limit = limitText()
                    if (limit.isNotEmpty()) append(" · ").append(limit)
                    if (warmupQueued > 0 && warmupCompleted < warmupQueued) {
                        append(" · ").append(getString(R.string.counter_shaders, (warmupCompleted * 100) / warmupQueued))
                    }
                    if (isMultiplayer) append(" · LAN: ").append(lanPeers)
                    append(" ▾")
                }

                if (hudIsExpanded) {
                    val temp = getDeviceTemp()
                    val (appMb, totalMb) = getMemoryStats()
                    val (battTemp, isCharging, battPct) = getBatteryStats()
                    val notKnown = getString(R.string.hud_not_known)
                    val cpuStr = if (cpuUsagePercent >= 0f) String.format(Locale.US, "%.0f%%", cpuUsagePercent) else notKnown
                    val parts = cachedRendererInfo.split("|")
                    val devName = if (parts.isNotEmpty() && parts[0].isNotEmpty()) parts[0] else "Vulkan Device"
                    val renderMode = if (parts.size > 1) parts[1] else "Vulkan 1.1 Legacy RenderPass"
                    val tcMode = if (parts.size > 2) parts[2] else "CPU Decompress Fallback"

                    val battStr = if (battTemp > 0f) {
                        val chg = if (isCharging) getString(R.string.hud_battery_charging, battPct)
                        else if (battPct >= 0) " ($battPct%)" else ""
                        String.format(Locale.US, "%.1f°C%s", battTemp, chg)
                    } else notKnown

                    val ramStr = if (totalMb > 0) getString(R.string.hud_ram_value, appMb, totalMb)
                                 else if (appMb > 0) "$appMb MB" else notKnown
                    val cpuTempStr = if (temp > 0f) String.format(Locale.US, "%.0f°C", temp) else notKnown

                    hudExpandedText.text = buildString {
                        fun line(text: String) = append("  ").append(text).append('\n')
                        append(getString(R.string.hud_section_fps)).append('\n')
                        append(String.format(Locale.US, "  ▶ %d FPS   (%.1f ms)\n", fps, frameTimeMs))
                        // TU0 plays only with TU0, TU6 with TU6.
                        line(getString(R.string.hud_presented, String.format("%,d", framesPresented), BuildConfig.MW2_TU.uppercase()))
                        append('\n')

                        append(getString(R.string.hud_section_cpu)).append('\n')
                        line(getString(R.string.hud_cpu, cpuStr, Runtime.getRuntime().availableProcessors()))
                        line(getString(R.string.hud_thermal, cpuTempStr, battStr))
                        append('\n')

                        append(getString(R.string.hud_section_gpu)).append('\n')
                        line(getString(R.string.hud_device, devName))
                        line(getString(R.string.hud_backend, renderMode))
                        line(getString(R.string.hud_textures, texMb, tcMode))
                        if (warmupQueued > 0) {
                            val pct = (warmupCompleted * 100) / warmupQueued
                            line(getString(R.string.hud_prewarm, warmupCompleted, warmupQueued, pct, warmupLastMs))
                        }
                        append('\n')

                        append(getString(R.string.hud_section_memory)).append('\n')
                        line(getString(R.string.hud_ram, ramStr))
                        line(getString(R.string.hud_render, resW, resH, refreshHz().roundToInt()))
                        val gpuMs = statsValues[10]
                        append("  ").append(getString(R.string.hud_held, statsValues[9].roundToInt()))
                        if (statsValues[12] > 0f) append(getString(R.string.hud_held_auto))
                        if (gpuMs >= 0f) append("  •  ").append(getString(R.string.hud_gpu, String.format(Locale.US, "%.1f", gpuMs)))
                        append("  •  ").append(getString(R.string.hud_scene, statsValues[11].roundToInt())).append('\n')
                        val limit = limitText()
                        if (limit.isNotEmpty()) line(getString(R.string.hud_limit, limit))
                        if (isMultiplayer) {
                            append('\n').append(getString(R.string.hud_section_network)).append('\n')
                            line(getString(R.string.hud_lan, lanPeers))
                        }
                    }
                }
            }
            handler.postDelayed(this, 500)
        }
    }

    /**
     * The counter's last word: what the frames of the last two seconds of
     * play waited for, as the runtime measured them (adaptive::Late) --
     * nothing, where they came at the rate the run is held to, the graphics
     * chip or the processor. Empty with no play in the last five seconds: a
     * menu, a film and a loading screen say nothing of the device, nor does
     * a chip that keeps no time.
     */
    private fun limitText(): String {
        val age = statsValues[15]
        if (age < 0f || age > 5f) return ""
        return when (statsValues[14].roundToInt()) {
            0 -> getString(R.string.counter_at_limit)
            1 -> getString(R.string.counter_chip)
            2 -> getString(R.string.counter_processor)
            else -> ""
        }
    }

    /** What this run was started with, which Best looking's measure is read against (applyOptions). */
    private var runStartedBest = false
    private var runRenderScale = 1
    private var runStartedAt = 0
    private var measuredAtPlay = 0f

    /**
     * Best looking draws at twice the size only where it was measured to
     * hold (Prefs.settleBestLooking), and this is the measuring: once a
     * minute of play is behind a run started under that preset, and every
     * half minute after, a run at 1x leaves the graphics chip's time a
     * frame for the next start to judge, and a run at 2x that came late or
     * was held to a slower rate says so, which makes the next start 1x.
     */
    private fun measureBestLooking() {
        if (!runStartedBest) return
        val play = statsValues[16]
        if (play < GraphicsPresets.MEASURED_AFTER || play - measuredAtPlay < 30f) return
        measuredAtPlay = play
        val name = title.name
        if (runRenderScale == 1) {
            val chipMs = statsValues[17]
            if (chipMs >= 0f) {
                prefs.noteChipTime(name, chipMs)
                NativeBridge.nativeLog(false, String.format(
                    Locale.US, "graphics: best looking measured the graphics chip at %.1f ms a frame in %.0f s of play at 1x",
                    chipMs, play,
                ))
            }
        } else if (GraphicsPresets.twiceFailed(statsValues[18], statsValues[9].roundToInt(), runStartedAt)) {
            prefs.noteTwiceFailed(name)
            runStartedBest = false
            NativeBridge.nativeLog(false, String.format(
                Locale.US, "graphics: best looking at %dx did not hold (%.0f%% of %.0f s of play came late, held to %d of %d):" +
                    " 1x from the next start",
                runRenderScale, statsValues[18] * 100f, play, statsValues[9].roundToInt(), runStartedAt,
            ))
        }
    }

    /**
     * A run that shows nothing says so. Two ways there: the runtime knows
     * the renderer did not come up (no driver opened, the chip lacks what
     * the renderer needs: nativeRendererFailure), or nothing says anything
     * and no frame of the title's reaches the screen. The first present is
     * the window's own colour, so the title's first is the second counted.
     * A player on a device nobody here has was left looking at a black
     * screen, and had nothing to send.
     */
    private fun watchForBlackScreen() {
        if (failureShown || frameSeen || isFinishing) return
        val failure = NativeBridge.nativeRendererFailure()
        if (failure.isNotEmpty()) {
            showRendererFailure(failure, mayWait = false)
            return
        }
        if (statsValues[5] >= 2f) {
            frameSeen = true
            return
        }
        // Compiling shaders the player asked for and saying how far it is
        // (showBootCompile): not a black screen while it goes on.
        if (BootCompile.from(statsValues)?.running == true) {
            watchingSince = SystemClock.elapsedRealtime()
            return
        }
        // Compiling every shader before the first frame is minutes of a
        // black screen the player asked for.
        val patience = if (prefs.prewarmAllShaders) NO_FRAME_PREWARM_MS else NO_FRAME_MS
        if (SystemClock.elapsedRealtime() - watchingSince >= patience) {
            val line = "no frame of the game's reached the screen in ${patience / 1000} s"
            NativeBridge.nativeLog(true, line)
            showRendererFailure(getString(R.string.no_frame, (patience / 1000).toInt()), mayWait = true)
        }
    }

    /**
     * The card over the black screen while Compile shaders at boot runs: how
     * many of how many, and whether the next start waits as long
     * (BootCompile). Gone with the first frame of the game's.
     */
    private fun showBootCompile() {
        val boot = BootCompile.from(statsValues)
        if (boot == null || !boot.running || statsValues[5] >= 2f || failureShown) {
            bootCard.visibility = View.GONE
            return
        }
        bootText.text = getString(R.string.boot_compile_progress, boot.done, boot.total, boot.percent)
        bootNote.setText(boot.note)
        bootBar.max = boot.total
        bootBar.progress = boot.done
        bootCard.visibility = View.VISIBLE
    }

    /**
     * The message over a black screen: what is wrong, on which chip and
     * driver, what to try -- the other driver, one tap, where there is one --
     * and the log to share. [mayWait]: nothing has said the renderer failed,
     * so the player may also go on waiting.
     */
    private fun showRendererFailure(reason: String, mayWait: Boolean) {
        failureShown = true
        val bundled = prefs.bundledDriverDirectory.takeIf { it.isNotEmpty() }
            ?.let { path -> DriverStore(this).list().firstOrNull { it.directory.absolutePath == path } }
        val onSystem = prefs.driverDirectory.isEmpty()
        // A driver the player imported failed: the one the app carries is
        // the likelier to work than the phone's own, which on the Snapdragons
        // that need an imported one (Adreno 610-619 on Android 10-11) is
        // refused for want of descriptor indexing.
        val onOtherImported = !onSystem && bundled != null && prefs.driverDirectory != bundled.directory.absolutePath
        val driver = try {
            NativeBridge.nativeDriverDescription()
        } catch (_: Throwable) {
            ""
        }
        val advice = when {
            onOtherImported -> getString(R.string.renderer_try_bundled_instead)
            !onSystem -> getString(R.string.renderer_try_system)
            bundled != null -> getString(R.string.renderer_try_bundled)
            else -> getString(R.string.renderer_try_import)
        }
        val message = getString(
            R.string.renderer_failed_message, reason, DeviceInfo.gpu(this),
            driver.ifEmpty { getString(R.string.driver_system) }, advice,
        )
        val builder = AlertDialog.Builder(this)
            .setTitle(if (mayWait) R.string.no_picture_yet else R.string.renderer_failed)
            .setMessage(message)
            .setCancelable(false)
            .setNeutralButton(R.string.share_log, null)
        if (onOtherImported) {
            builder.setPositiveButton(R.string.renderer_use_bundled) { _, _ -> restartWithDriver(bundled) }
        } else if (!onSystem) {
            builder.setPositiveButton(R.string.renderer_use_system) { _, _ -> restartWithDriver(null) }
        } else if (bundled != null) {
            builder.setPositiveButton(R.string.renderer_use_bundled) { _, _ -> restartWithDriver(bundled) }
        }
        if (mayWait) {
            builder.setNegativeButton(R.string.keep_waiting) { _, _ -> goFullscreen() }
        } else {
            builder.setNegativeButton(R.string.quit) { _, _ -> quit() }
        }
        val dialog = builder.show()
        // Sharing leaves the message up: the player comes back to it from
        // the other app with the choice still to make. As the launcher's
        // Log screen shares: a file, or the end of it copied (LogShare).
        dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener {
            LogShare.share(this, LogSummary.whole(this, NativeBridge.nativeLogTail()))
        }
    }

    /**
     * The other driver, chosen as Settings' Graphics driver chooses it
     * ([driver] null for the phone's own), and the run started again: the
     * driver is opened once in a process's life.
     */
    private fun restartWithDriver(driver: DriverStore.Driver?) {
        prefs.driverChoiceMade = true
        prefs.driverDirectory = driver?.directory?.absolutePath ?: ""
        prefs.driverLibrary = driver?.library ?: ""
        prefs.flush()
        NativeBridge.nativeLog(false, "the player switched to ${driver?.name ?: "the system's driver"} and the run starts again")
        HandOffActivity.start(this, RunRequest(title, launchData, contentLevel))
        finish()
        handler.postDelayed({ Process.killProcess(Process.myPid()) }, 80)
    }

    /**
     * The performance report (the menu's Share performance report): the
     * phone, the settings, and from the run what it is held to and how the
     * play so far went -- frames a second, the graphics chip's time, the
     * scene's size, the sound's underruns (nativePerformance). What a player
     * on a phone nobody here has can send instead of "it lags".
     */
    private fun sharePerformance() {
        val (appMb, totalMb) = getMemoryStats()
        val (batteryTemp, charging, battery) = getBatteryStats()
        val shown = shownDisplay()
        val text = buildString {
            append("===== performance report =====\n")
            DeviceInfo.lines(this@GameActivity).forEach { append(it).append('\n') }
            append("title: ").append(title.name).append('\n')
            if (shown != null) {
                append("screen: ${shown.mode.physicalWidth}x${shown.mode.physicalHeight} at ${shown.mode.refreshRate} Hz; modes ")
                append(shown.supportedModes.joinToString { "${it.refreshRate} Hz" }).append('\n')
            }
            append("surface: ${statsValues[2].roundToInt()}x${statsValues[3].roundToInt()}\n")
            append("\n===== settings =====\n")
            prefs.graphicsSummary().forEach { append(it).append('\n') }
            append("\n===== this run =====\n")
            append(NativeBridge.nativePerformance())
            append(String.format(Locale.US, "now: %.0f FPS shown; app memory %d MB of %d MB\n", statsValues[0], appMb, totalMb))
            append(String.format(
                Locale.US, "heat: chip %.0f C, battery %.1f C at %d%%%s\n",
                getDeviceTemp(), batteryTemp, battery, if (charging) " (charging)" else "",
            ))
        }
        // In the run's log too, so that a log shared later carries it.
        text.lineSequence().filter { it.isNotBlank() }.forEach { NativeBridge.nativeLog(false, it) }
        LogShare.share(this, text)
    }

    /**
     * What the runtime has decided, read twice a second (runtime/adaptive.h):
     * the rate a run that follows the device is held to, which the player is
     * told, and the rate the display is asked for a mode of. That is the
     * rate the run is held to or the player chose, which the slowest mode
     * that shows it evenly costs the least for -- or the display's fastest,
     * where the slower mode costs the sound: an AYN Thor's system gives a
     * game half the share of a busy processor in its 60 Hz mode.
     */
    private fun followHeldRate() {
        val target = statsValues[9].roundToInt()
        if (statsValues[12] > 0f && target > 0 && target != heldTo) {
            // The first reading is the limit the run started with.
            if (heldTo != 0) {
                android.widget.Toast.makeText(this, getString(R.string.held_to, target), android.widget.Toast.LENGTH_SHORT).show()
            }
            heldTo = target
        }
        val display = statsValues[13].roundToInt()
        if (display <= 0 || display == displayAsked) return
        // The first reading is the limit the run started with, asked for in onCreate.
        if (displayAsked != 0) {
            DisplayRate.request(window, shownDisplay(), display)
            logDisplayMode()
        }
        displayAsked = display
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        prefs = Prefs(this)

        Trail.note(this, "game: starting")
        TitleUpdate.refresh(this)
        val request = RunRequest.read(this)
        // Launch data, and a level to start in, are for the run they were
        // left for, not for whichever run comes after it.
        if (request.launchData.isNotEmpty() || request.level.isNotEmpty()) RunRequest(request.title, "").write(this)
        launchData = request.launchData
        contentLevel = request.level
        // The last run's process, not gone yet, holds the other title's
        // library and cannot take this one: start again once it has ended.
        if (NativeBridge.title().let { it != null && it != request.title }) {
            HandOffActivity.start(this, request)
            finish()
            Process.killProcess(Process.myPid())
            return
        }
        if (!NativeBridge.load(request.title)) {
            fail(getString(R.string.error_library, NativeBridge.loadError() ?: "?"))
            return
        }
        perfSession = PerfSession(this, request.title, prefs)

        buildUi()
        goFullscreen()
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        // Sustained performance mode locks CPU/GPU governors to steady frequencies
        // to prevent erratic thermal throttling mid-gameplay.
        window.setSustainedPerformanceMode(true)
        DisplayRate.request(window, shownDisplay(), prefs.fpsLimit)
        logDisplayMode()
        getSystemService(DisplayManager::class.java).registerDisplayListener(displayListener, handler)
        handler.postDelayed({ sayDisplayHeld() }, 5000)

        vibrator = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            (getSystemService(Context.VIBRATOR_MANAGER_SERVICE) as? VibratorManager)?.defaultVibrator
        } else {
            @Suppress("DEPRECATION")
            getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator
        }

        NativeBridge.listener = this
        NativeBridge.nativeInit(
            filesDir.absolutePath,
            (getExternalFilesDir(null) ?: filesDir).absolutePath,
            cacheDir.absolutePath,
            applicationInfo.nativeLibraryDir,
            this,
        )
        DeviceInfo.log(this)
        // After nativeInit, so that the reason a library is refused is in the
        // run's log as well as on the screen.
        GameLibrary.load(this, request.title)?.let {
            fail(it, offerBuild = GameBuild.offered(this))
            return
        }

        applyOptions()
        applyDriver()

        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                when {
                    overlay.editing -> endEditing(save = true)
                    help.isOpen -> help.close()
                    menu.isOpen -> menu.back()
                    else -> showMenu()
                }
            }
        })

        val gameDirectory = prefs.gameDirectory
        if (gameDirectory.isEmpty() || !NativeBridge.nativeGameInstalled(gameDirectory)) {
            fail(getString(R.string.error_not_installed))
            return
        }
        // A game played from its disc image: the runtime lays the image under
        // the game folder (kernel/volume.h), and without it has no files.
        val disc = prefs.discImage
        if (disc.isNotEmpty()) {
            val source = DiscImage.source(this, disc)
            if (source == null) {
                fail(getString(R.string.status_disc_missing, DiscImage.label(this, disc)))
                return
            }
            NativeBridge.nativeSetOption("MW2_DISC_IMAGE", source)
        }
        // Advanced Warfare's install disc, when its packages are read out of
        // its image too (kernel/content.cpp): the title does not start
        // without them.
        val installDisc = prefs.installDiscImage
        if (installDisc.isNotEmpty()) {
            val source = DiscImage.source(this, installDisc)
            if (source == null) {
                fail(getString(R.string.status_disc_missing, DiscImage.label(this, installDisc)))
                return
            }
            NativeBridge.nativeSetOption("MW2_INSTALL_DISC_IMAGE", source)
        }
        // The camouflage packs this APK was built with, where the runtime
        // reads them: Modern Warfare 3's multiplayer lists them.
        if (isMw3Multiplayer) CamoPacks.installBundled(this, gameDirectory)
        Trail.note(this, "game: handing over to the runtime")
        val home = title.home(File(gameDirectory))
        home.mkdirs()
        if (!NativeBridge.nativeStart(gameDirectory, home.absolutePath)) {
            fail(NativeBridge.nativeStatus().ifEmpty { getString(R.string.error_start) })
            return
        }
        started = true
        // How to open the menu, change settings and quit, until the player
        // has asked not to be told again.
        help.openUnlessSeen()
        // The run is up, so ask the system to treat its process as one still
        // wanted: stepping out to a message then costs a pause, not the whole
        // load again (GameService.kt).
        GameService.start(this)
        Trail.clear(this)
        pads.setEnabled(prefs.gamepadEnabled)
        pads.listen(this)
        pads.refreshDevices()
        applyLook()
        startHardwareMonitor()
        watchingSince = SystemClock.elapsedRealtime()
        handler.post(statsTick)
        // A title with the runtime's field of view has it from the start
        // (applyOptions).
        if (!fovIsRuntimes) handler.postDelayed({ if (started) applyFov() }, 2500)
    }

    // ---- the screen ---------------------------------------------------------

    @SuppressLint("SetTextI18n")
    private fun buildUi() {
        root = FrameLayout(this)
        root.setBackgroundColor(Color.BLACK)

        surfaceView = SurfaceView(this)
        surfaceView.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) = Unit

            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                // The renderer draws at a fraction of the screen and the
                // display scales it up for free -- the single biggest thing
                // a weak phone can be given. setFixedSize comes back here
                // with the new size, which is when the runtime is told.
                val (wantedWidth, wantedHeight) = surfaceSize()
                if (width != wantedWidth || height != wantedHeight) {
                    holder.setFixedSize(wantedWidth, wantedHeight)
                    return
                }
                NativeBridge.nativeSurfaceChanged(holder.surface, width, height, refreshHz())
            }

            override fun surfaceDestroyed(holder: SurfaceHolder) {
                // Blocking on purpose: the presenter has to let go of this
                // window before Android takes it away, and the runtime waits
                // for the acknowledgement (runtime/android/window.cpp).
                NativeBridge.nativeSurfaceDestroyed()
            }
        })
        root.addView(
            surfaceView,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT,
            )
        )

        overlay = TouchOverlayView(this)
        overlay.layout = ControlLayout.load(this)
        // The Talk button is there when there is something for it to do,
        // whatever a saved layout says.
        overlay.layout.find("talk")?.visible = voiceChat && prefs.voicePushToTalk
        overlay.visibility = if (prefs.touchControls) View.VISIBLE else View.GONE
        overlay.onLayoutEdited = { overlay.layout.save(this) }
        overlay.onSelectionChanged = { editPanel?.show(it) }
        root.addView(
            overlay,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT,
            )
        )

        hudContainer = FrameLayout(this)
        hudContainer.visibility = if (prefs.showStats) View.VISIBLE else View.GONE

        statsText = TextView(this)
        statsText.setTextColor(Color.argb(245, 255, 236, 214))
        statsText.textSize = 10f
        statsText.typeface = Typeface.MONOSPACE
        statsText.setPadding(pad(10), pad(4), pad(10), pad(4))
        val pillBg = GradientDrawable().apply {
            setColor(Color.argb(215, 20, 20, 23))
            cornerRadius = pad(10).toFloat()
            setStroke(pad(1), Color.argb(170, 255, 122, 26))
        }
        statsText.background = pillBg
        statsText.setOnClickListener {
            hudIsExpanded = true
            statsText.visibility = View.GONE
            hudExpandedCard.visibility = View.VISIBLE
        }
        hudContainer.addView(
            statsText,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP or Gravity.START,
            )
        )

        hudExpandedCard = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            val cardBg = GradientDrawable().apply {
                setColor(Color.argb(235, 20, 20, 23))
                cornerRadius = pad(12).toFloat()
                setStroke(pad(1), Color.argb(200, 255, 122, 26))
            }
            background = cardBg
            setPadding(pad(14), pad(10), pad(14), pad(10))
            visibility = View.GONE
            setOnClickListener {
                hudIsExpanded = false
                hudExpandedCard.visibility = View.GONE
                statsText.visibility = View.VISIBLE
            }
        }

        val hudHeader = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        val hudTitle = TextView(this).apply {
            text = getString(R.string.hud_title)
            setTextColor(Color.argb(255, 255, 122, 26))
            textSize = 11f
            typeface = Typeface.DEFAULT_BOLD
        }
        val hudCollapseBtn = TextView(this).apply {
            text = "  " + getString(R.string.hud_close)
            setTextColor(Color.argb(200, 148, 163, 184))
            textSize = 10f
            typeface = Typeface.MONOSPACE
            setOnClickListener {
                hudIsExpanded = false
                hudExpandedCard.visibility = View.GONE
                statsText.visibility = View.VISIBLE
            }
        }
        val hudConsoleBtn = TextView(this).apply {
            text = " " + getString(R.string.hud_console)
            setTextColor(Color.argb(230, 245, 158, 11))
            textSize = 10f
            typeface = Typeface.MONOSPACE
            setOnClickListener { showConsoleDialog() }
        }
        hudHeader.addView(hudTitle, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        hudHeader.addView(hudConsoleBtn, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        hudHeader.addView(hudCollapseBtn, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        hudExpandedCard.addView(hudHeader)

        hudExpandedText = TextView(this).apply {
            setTextColor(Color.argb(240, 226, 232, 240))
            textSize = 9.5f
            typeface = Typeface.MONOSPACE
            setPadding(0, pad(6), 0, 0)
        }
        hudExpandedCard.addView(hudExpandedText)

        hudContainer.addView(
            hudExpandedCard,
            FrameLayout.LayoutParams(
                pad(320), ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP or Gravity.START,
            )
        )

        root.addView(
            hudContainer,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP or Gravity.START,
            ).apply { topMargin = pad(6); marginStart = pad(8) }
        )

        menuButton = smallButton(getString(R.string.menu)) { showMenu() }
        root.addView(
            menuButton,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP or Gravity.END,
            ).apply { topMargin = pad(6); marginEnd = pad(6) }
        )

        bootCard = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
            background = GradientDrawable().apply {
                setColor(Color.argb(235, 20, 20, 23))
                cornerRadius = pad(12).toFloat()
                setStroke(pad(1), Color.argb(200, 255, 122, 26))
            }
            setPadding(pad(18), pad(14), pad(18), pad(14))
            visibility = View.GONE
        }
        bootText = TextView(this).apply {
            setTextColor(Color.argb(255, 255, 236, 214))
            textSize = 15f
            typeface = Typeface.DEFAULT_BOLD
            gravity = Gravity.CENTER
        }
        bootCard.addView(bootText)
        bootBar = ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal)
        bootCard.addView(
            bootBar,
            LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
                .apply { topMargin = pad(10); bottomMargin = pad(10) },
        )
        bootNote = TextView(this).apply {
            setTextColor(Color.argb(230, 226, 232, 240))
            textSize = 12f
            gravity = Gravity.CENTER
        }
        bootCard.addView(bootNote)
        root.addView(
            bootCard,
            FrameLayout.LayoutParams(pad(360), ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER),
        )

        setContentView(root)
    }

    /** The pixels the renderer is given: the view's, times the resolution setting (Prefs.surfaceScale). */
    private fun surfaceSize(): Pair<Int, Int> {
        val scale = prefs.surfaceScale
        val height = max(240, (surfaceView.height * scale).roundToInt())
        val width = if (prefs.fillScreen) max(320, (height * 16f / 9f).roundToInt())
        else max(320, (surfaceView.width * scale).roundToInt())
        return width to height
    }

    /**
     * The resolution, the sharpening or the fill setting is another one: the surface is given
     * its new size, which comes back through surfaceChanged, where the
     * runtime is told and makes its swapchain again. The title's own frame
     * is not touched, so the level goes on.
     */
    private fun resizeSurface() {
        val (width, height) = surfaceSize()
        surfaceView.holder.setFixedSize(width, height)
    }

    private fun smallButton(label: String, onClick: () -> Unit): Button {
        val button = Button(this)
        button.text = label
        button.textSize = 11f
        button.alpha = 0.55f
        button.setPadding(pad(12), pad(4), pad(12), pad(4))
        button.minHeight = pad(34)
        button.minWidth = pad(64)
        button.setOnClickListener { onClick() }
        return button
    }

    private fun showConsoleDialog() {
        val container = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad(16), pad(12), pad(16), pad(8))
        }

        val input = EditText(this).apply {
            hint = getString(R.string.console_hint)
            setTextColor(Color.WHITE)
            setHintTextColor(Color.GRAY)
            setPadding(pad(12), pad(10), pad(12), pad(10))
        }
        container.addView(input)

        val scroll = HorizontalScrollView(this).apply {
            isFillViewport = true
            setPadding(0, pad(8), 0, 0)
        }
        val chipRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
        }

        val presets = listOf(
            "cg_fov 85" to getString(R.string.console_fov_wide),
            "cg_fov 65" to getString(R.string.console_fov_default),
            "cg_drawFPS 1" to getString(R.string.console_show_fps),
            "god" to getString(R.string.console_god),
            "give all" to getString(R.string.console_all_weapons),
            "ufo" to getString(R.string.console_ufo),
            "r_fullbright 1" to getString(R.string.console_full_bright),
            "cg_drawGun 0" to getString(R.string.console_hide_gun),
        )

        for ((cmd, label) in presets) {
            val chip = Button(this).apply {
                text = label
                textSize = 9.5f
                alpha = 0.85f
                setPadding(pad(8), pad(2), pad(8), pad(2))
                minHeight = pad(28)
                minWidth = pad(48)
                setOnClickListener {
                    input.setText(cmd)
                    input.setSelection(cmd.length)
                }
            }
            chipRow.addView(chip, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                marginEnd = pad(6)
            })
        }
        scroll.addView(chipRow)
        container.addView(scroll)

        AlertDialog.Builder(this)
            .setTitle(R.string.console_title)
            .setView(container)
            .setPositiveButton(R.string.console_run) { _, _ ->
                val cmd = input.text.toString().trim()
                if (cmd.isNotEmpty()) {
                    NativeBridge.nativeRunConsoleCommand(cmd)
                }
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun pad(dp: Int): Int = (dp * resources.displayMetrics.density).roundToInt()

    private fun shownDisplay(): Display? =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) display
        else {
            @Suppress("DEPRECATION")
            windowManager.defaultDisplay
        }

    private fun refreshHz(): Float {
        val hz = shownDisplay()?.refreshRate ?: 60f
        return if (hz < 20f) 60f else hz
    }

    // The mode the display is in, beside the ones it has, whenever that is
    // another: a limit of 120 on a screen the system holds at 60 is seen here.
    private fun logDisplayMode() {
        val shown = shownDisplay() ?: return
        val mode = shown.mode
        val line = "display ${shown.displayId}: mode ${mode.modeId}, " +
            "${mode.physicalWidth}x${mode.physicalHeight} at ${mode.refreshRate} Hz; it has " +
            shown.supportedModes.joinToString { "${it.modeId}: ${it.refreshRate} Hz" } +
            "; FPS limit ${prefs.fpsLimit}"
        if (line == displayModeLogged) return
        displayModeLogged = line
        android.util.Log.i("MW2", line)
    }

    // Said once a run, when the mode request above has had its time
    // (DisplayRate.kt); the launcher says the same, and how to mend it.
    private fun sayDisplayHeld() {
        val limit = prefs.fpsLimit
        val now = DisplayRate.heldAt(shownDisplay(), limit) ?: return
        android.util.Log.i("MW2", "display: held at $now Hz by the system under an FPS limit of $limit")
        android.widget.Toast.makeText(
            this, getString(R.string.display_held, now.roundToInt(), limit), android.widget.Toast.LENGTH_LONG,
        ).show()
    }

    private fun goFullscreen() {
        WindowCompat.setDecorFitsSystemWindows(window, false)
        val controller = WindowInsetsControllerCompat(window, window.decorView)
        controller.hide(WindowInsetsCompat.Type.systemBars())
        controller.systemBarsBehavior =
            WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            // The attributes have to be handed back for a change to take.
            val attributes = window.attributes
            if (attributes.layoutInDisplayCutoutMode !=
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
            ) {
                attributes.layoutInDisplayCutoutMode =
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
                window.attributes = attributes
            }
        }
    }

    // ---- what the runtime is told before it starts -----------------------------

    private fun applyOptions() {
        // Everything here is one of the runtime's own switches
        // (docs/switches.md); the app just fills them in.
        NativeBridge.nativeSetOption("MW2_FULLSCREEN", "1")
        // The game in the phone's language where its disc has it (Black Ops III).
        val (language, locale) = GameLanguage.forThisGame() ?: (null to null)
        NativeBridge.nativeSetOption("MW2_LANGUAGE", language?.toString().orEmpty())
        NativeBridge.nativeSetOption("MW2_LOCALE", locale?.toString().orEmpty())
        // MP refuses to start matches without a link; runtime defaults to up
        // for MP and down for SP (kernel/net.cpp), but be explicit so a
        // misbuilt lib still reports a link in the MP app.
        if (isMw2Multiplayer) {
            NativeBridge.nativeSetOption("MW2_NET_LINK", "1")
        }
        NativeBridge.nativeSetOption("MW2_NO_AUDIO", if (prefs.audioEnabled) "" else "1")
        NativeBridge.nativeSetOption("MW2_NO_MSAA", if (prefs.multisampling) "" else "1")
        // Best looking draws at twice the size in a title measured to hold
        // it, which is settled now; what the run starts with is kept for
        // the measuring (measureBestLooking).
        prefs.settleBestLooking(title.name)
        runStartedBest = prefs.graphicsPreset == GraphicsPresets.BEST
        runRenderScale = prefs.renderScale
        runStartedAt = prefs.fpsLimit
        prefs.graphicsSummary().take(3).forEach { NativeBridge.nativeLog(false, "graphics: $it") }
        NativeBridge.nativeSetOption("MW2_SCALE", prefs.renderScale.toString())
        // The 3D scene: left to the runtime it follows the load; whole, it
        // is the console's on every chip; smaller, one fixed share of it.
        when (prefs.scene) {
            GraphicsPresets.SCENE_WHOLE -> {
                NativeBridge.nativeSetOption("MW2_SCENE_DYNAMIC", "0")
                NativeBridge.nativeSetOption("MW2_SCENE", "console")
            }
            GraphicsPresets.SCENE_SMALLER ->
                NativeBridge.nativeSetOption("MW2_SCENE_SHARE", GraphicsPresets.SMALLER_PERCENT.toString())
        }
        // Modern Warfare 2 alone has the hook (runtime/model_detail.cpp).
        if (!Title.isMw3) NativeBridge.nativeSetOption("MW2_LOD_BIAS", prefs.detailBias.toString())
        NativeBridge.nativeSetOption("MW2_FPS_LIMIT", prefs.fpsLimit.toString())
        // A limit nobody chose is where the run starts, and it may be held
        // to a slower one of the rates this screen shows evenly.
        NativeBridge.nativeSetOption("MW2_FPS_AUTO", if (prefs.fpsFollowsDevice) "1" else "")
        NativeBridge.nativeSetOption("MW2_FPS_STEPS", prefs.fpsSteps.joinToString(","))
        NativeBridge.nativeSetOption("MW2_SHARPEN", prefs.sharpening.toString())
        // Unset leaves the sun's shadows to the runtime's choice for the
        // chip and the title.
        if (prefs.sunShadows != 0) {
            NativeBridge.nativeSetOption("MW2_SHADOWS", if (prefs.sunShadows == 1) "1" else "0")
        }
        NativeBridge.nativeSetOption("MW2_STUTTERS", "1")
        // Full synchronous precompile at boot when the player asked for it in
        // Settings; otherwise the tiered instant-boot budget + background warmup.
        NativeBridge.nativeSetOption("MW2_SHADER_PREWARM_ALL", if (prefs.prewarmAllShaders) "1" else "")
        NativeBridge.nativeSetOption("MW2_ASYNC_PIPELINES", if (prefs.asyncPipelines) "1" else "0")
        // The bots of the matches this device hosts (runtime/bots.cpp).
        if (hasBots) {
            NativeBridge.nativeSetOption("MW2_BOTS", if (prefs.botCount > 0) prefs.botCount.toString() else "")
            NativeBridge.nativeSetOption("MW2_BOT_SKILL", prefs.botSkill.toString())
            NativeBridge.nativeSetOption("MW2_BOT_TEAMS", if (prefs.botsAgainstPlayers) "against" else "")
        }
        // Every item unlocked: Modern Warfare 3's multiplayer and Advanced
        // Warfare's, whose check applies under Xbox LIVE (aw_unlock_all.cpp).
        if (isMw3Multiplayer || (isMultiplayer && Title.isAw)) {
            NativeBridge.nativeSetOption("MW2_UNLOCK_ALL", if (prefs.unlockAllItems) "1" else "")
        }
        if (fovIsRuntimes) NativeBridge.nativeSetOption("MW2_FOV", prefs.fov.coerceAtLeast(65).toString())
        // Modern Warfare 3's multiplayer: the host's match options and voice
        // chat (the other titles read none of them here).
        if (isMw3Multiplayer) {
            NativeBridge.nativeSetOption("MW2_MATCH_GRAVITY", prefs.matchGravity.toString())
            NativeBridge.nativeSetOption("MW2_MATCH_SPEED", prefs.matchSpeed.toString())
            NativeBridge.nativeSetOption("MW2_MATCH_JUMP", prefs.matchJump.toString())
            NativeBridge.nativeSetOption("MW2_MATCH_SPRINT", if (prefs.matchUnlimitedSprint) "1" else "")
            NativeBridge.nativeSetOption("MW2_MATCH_FALL_DAMAGE", if (prefs.matchFallDamage) "1" else "0")
            NativeBridge.nativeSetOption("MW2_MAP_VOTE", if (prefs.mapVote) "1" else "")
            NativeBridge.nativeSetOption("MW2_VOICE", if (voiceChat) "1" else "")
            NativeBridge.nativeSetOption("MW2_VOICE_PTT", if (prefs.voicePushToTalk) "1" else "0")
        }
        // Empty leaves it to the runtime: a Mali's cache is kept from driver
        // r38 on (pipeline_cache_rules.h); on keeps it under older ones too.
        NativeBridge.nativeSetOption(
            "MW2_PERSIST_PIPELINE_CACHE",
            if (prefs.persistPipelineCache) "1" else "",
        )
        // Zero means let the runtime choose a budget for the actual device:
        // the option is then not set at all, since an empty one is kept as
        // the player's choice and would stop the device default applying.
        if (prefs.textureBudgetMB > 0) {
            NativeBridge.nativeSetOption("MW2_TEXTURE_BUDGET_MB", prefs.textureBudgetMB.toString())
        }
        // The upload arena is sized by the runtime from the device's memory
        // (runtime/android/perf.cpp). A fixed 144 MB gives each frame 48 MB,
        // and a frame that runs out of its share draws nothing after that.
        // Both caches live in the app's own folder and make the second start
        // of a level far quicker than the first.
        val extDir = getExternalFilesDir(null)
        // A name of its own for each title in the app that carries both.
        val shaderCacheName = "shader_cache${title.cacheSuffix}.bin"
        val shaderCacheFile = if (extDir != null) File(extDir, shaderCacheName) else File(filesDir, shaderCacheName)
        if (!shaderCacheFile.exists()) {
            val internalCache = File(filesDir, shaderCacheName)
            val legacyShaders = File(filesDir, "shaders")
            if (internalCache.exists() && internalCache.length() > 0) {
                internalCache.copyTo(shaderCacheFile, overwrite = true)
            } else if (legacyShaders.exists() && legacyShaders.length() > 0) {
                legacyShaders.copyTo(shaderCacheFile, overwrite = true)
            } else {
                adoptRicherSeed(shaderCacheFile)
            }
        } else {
            // A fuller bundled seed replaces a thinner installed cache, so a
            // recorded playthrough shipped in the APK actually reaches play.
            adoptRicherSeed(shaderCacheFile)
        }
        NativeBridge.nativeSetOption("MW2_PIPELINE_CACHE", "${filesDir.absolutePath}/pipeline${title.cacheSuffix}.cache")
        // What the other title left for this one to read at its start
        // (XamLoaderGetLaunchData), across the restart between the two.
        if (launchData.isNotEmpty()) NativeBridge.nativeSetOption("MW2_LAUNCH_DATA", launchData)
        // Set when empty as well: a process that outlived a run started in a
        // level would start the next one there.
        NativeBridge.nativeSetOption("MW2_CONTENT_LEVEL", contentLevel)
        NativeBridge.nativeSetOption("MW2_SHADER_CACHE", shaderCacheFile.absolutePath)

        // Identity on the network and lobby behaviour. The name is the
        // player's, or the device's own until he gives one (Prefs.kt); the
        // accept flag lets a host's invitation join without asking.
        NativeBridge.nativeSetOption("MW2_NAME", prefs.playerName)
        NativeBridge.nativeSetOption("MW2_LAN_ACCEPT", if (prefs.lanAccept) "1" else "")
        NativeBridge.nativeSetOption("MW2_LAN_IFACE", prefs.lanIface)
        // Which online service carries the title's network (Settings,
        // NETWORK): a relay server, the local network, or none. The campaign
        // apps have none. A public relay asks for no password.
        if (isMultiplayer) {
            val backend = prefs.networkBackend
            NativeBridge.nativeSetOption("MW2_NETWORK", backend)
            NativeBridge.nativeSetOption("MW2_SERVER", if (backend == "server") prefs.relayAddress else "")
            NativeBridge.nativeSetOption("MW2_SERVER_PASSWORD", "")
            if (!isMw2Multiplayer) {
                NativeBridge.nativeSetOption("MW2_LIVE", if (prefs.liveSignIn) "1" else "")
            }
        }
        // A debug build takes more switches from options.txt in the app's
        // external folder, one MW2_NAME=value a line, after everything
        // above: `adb shell "echo MW2_TRACE_NET=1 > /sdcard/Android/data/
        // <package>/files/options.txt"` turns a trace on without a rebuild.
        if (BuildConfig.DEBUG) {
            val options = File(getExternalFilesDir(null) ?: filesDir, "options.txt")
            if (options.isFile) {
                options.readLines().map { it.trim() }.forEach { line ->
                    val at = line.indexOf('=')
                    if (at > 0 && line.startsWith("MW2_")) {
                        NativeBridge.nativeSetOption(line.substring(0, at), line.substring(at + 1))
                    }
                }
            }
        }
        applyLook()
        NativeBridge.nativePadEnabled(prefs.gamepadEnabled)
    }

    private data class SeedCounts(val magic: Int, val version: Int, val shaders: Int, val pipelines: Int)

    private fun readSeedCounts(stream: java.io.InputStream): SeedCounts? = try {
        stream.use {
            val head = ByteArray(16)
            var read = 0
            while (read < 16) {
                val n = it.read(head, read, 16 - read)
                if (n <= 0) break
                read += n
            }
            if (read < 16) null
            else {
                val words = java.nio.ByteBuffer.wrap(head).order(java.nio.ByteOrder.LITTLE_ENDIAN).asIntBuffer()
                SeedCounts(words[0], words[1], words[2], words[3])
            }
        }
    } catch (_: Exception) {
        null
    }

    // The APK ships a recorded seed; a fuller one wins over a thinner
    // installed cache, whichever side it came from. Headers compared, never
    // trusted blindly; a foreign or corrupt file is left alone.
    private fun adoptRicherSeed(target: File) {
        val seed = try {
            readSeedCounts(assets.open("shader_cache.bin"))
        } catch (_: Exception) {
            null
        } ?: return
        val have = if (target.exists() && target.length() >= 16) {
            try {
                readSeedCounts(target.inputStream())
            } catch (_: Exception) {
                null
            }
        } else null
        val take = have == null ||
            (seed.magic == have.magic && seed.version == have.version && seed.pipelines > have.pipelines)
        if (!take) return
        try {
            assets.open("shader_cache.bin").use { input ->
                target.outputStream().use { output -> input.copyTo(output) }
            }
            android.util.Log.i("MW2", "Shader seed: bundled ${seed.pipelines} pipelines replaces ${have?.pipelines ?: 0}")
        } catch (_: Exception) {
        }
    }

    private fun applyDriver() {
        val directory = prefs.driverDirectory
        val library = prefs.driverLibrary
        if (directory.isNotEmpty() && library.isNotEmpty()) {
            NativeBridge.nativeSelectDriver(directory, library)
        } else {
            NativeBridge.nativeSelectDriver("", "")
        }
    }

    // ---- the menu over the game -----------------------------------------------------

    private val menu: GameMenu by lazy { GameMenu(this, root) { goFullscreen() } }

    private val help: GameHelp by lazy { GameHelp(this, root, prefs) { goFullscreen() } }

    private fun onOff(): List<String> = listOf(getString(R.string.state_off), getString(R.string.state_on))

    /**
     * Opened by the Menu button, the back key, or Select and Start together
     * on a pad (dispatchKeyEvent). The game is not paused by it: the title's
     * own pause does that, and a setting is seen as it changes.
     */
    private fun showMenu() {
        // The pads' events are the menu's from here, and the game would
        // never see what is held now come up.
        pads.release()
        overlay.releaseEverything()
        val rows = mutableListOf<GameMenu.Row>(
            GameMenu.Entry(getString(R.string.resume)) { menu.close() },
            GameMenu.Entry(getString(R.string.menu_graphics)) { menu.push(graphicsPage()) },
            GameMenu.Choice(getString(R.string.touch_controls), onOff(), if (prefs.touchControls) 1 else 0) { setTouchControls(it == 1) },
            GameMenu.Entry(getString(R.string.edit_controls)) { menu.close(); beginEditing() },
            GameMenu.Entry(getString(R.string.menu_look)) { menu.push(lookPage()) },
            GameMenu.Choice(getString(R.string.menu_gamepad), onOff(), if (prefs.gamepadEnabled) 1 else 0) { setGamepad(it == 1) },
        )
        // The Live lobby's invitations: MW2's and Call of Duty 4's, and
        // MW3's when it is signed in, which through a relay it is
        // (runtime/online/service.cpp, online::Live) -- its private match
        // had a lobby nobody could be asked into or find.
        if (isMw2Multiplayer ||
            (isMultiplayer && (prefs.liveSignIn || prefs.networkBackend == "server"))) {
            rows.add(GameMenu.Entry(getString(R.string.invite_players)) { menu.close(); inviteNetworkPlayer() })
            rows.add(GameMenu.Entry(getString(R.string.join_game)) { menu.close(); pickNetworkPeer() })
        }
        // Modern Warfare 2's stat cheats write that game's stats file
        // (MpCheatsData's offsets): Call of Duty 4's and Advanced Warfare's
        // are laid out differently, so their multiplayer is not offered them.
        if (isMw2Multiplayer && !Title.isCod4 && !Title.isAw) {
            rows.add(GameMenu.Entry(getString(R.string.max_rank)) { menu.close(); runCheat(R.string.max_rank, Cheats::maxRank) })
            rows.add(GameMenu.Entry(getString(R.string.unlock_all)) { menu.close(); runCheat(R.string.unlock_all, Cheats::unlockEverything) })
            rows.add(GameMenu.Entry(getString(R.string.prestige_up)) { menu.close(); runCheat(R.string.prestige_up, Cheats::prestigeUp) })
        }
        if (hasBots) {
            val counts = List(Prefs.MAX_BOTS + 1) { if (it == 0) getString(R.string.menu_bots_none) else it.toString() }
            rows.add(GameMenu.Choice(getString(R.string.menu_bots), counts, prefs.botCount.coerceIn(0, Prefs.MAX_BOTS)) { setBotCount(it) })
        }
        rows.add(GameMenu.Entry(getString(R.string.share_performance)) { menu.close(); sharePerformance() })
        rows.add(GameMenu.Entry(getString(R.string.quit)) { menu.push(quitPage()) })
        menu.open(GameMenu.Page(getString(R.string.menu), rows, getString(R.string.menu_hint)))
    }

    /**
     * What Settings' Graphics page and its Advanced page have, with the same
     * Prefs behind them: the preset first, and under it each setting by
     * itself. Those the running game cannot take are marked, and wait for
     * the next start: sizes the renderer makes its targets at once, and the
     * driver, which a process opens once.
     */
    private fun graphicsPage(): GameMenu.Page {
        val rows = mutableListOf<GameMenu.Row>()

        val preset = prefs.graphicsPreset
        rows.add(GameMenu.Choice(
            getString(R.string.menu_preset), resources.getStringArray(R.array.preset_names).toList(), preset,
        ) { chooseGraphicsPreset(it) })

        val percents = (35..100 step 5).toList()
        rows.add(GameMenu.Choice(
            getString(R.string.menu_resolution), percents.map { getString(R.string.menu_percent, it) },
            nearest(percents, (prefs.resolutionScale * 100f).roundToInt()),
        ) {
            advanced {
                prefs.resolutionScale = percents[it] / 100f
                resizeSurface()
            }
        })

        // Sharpened, the picture is shown at the screen's full size
        // whatever the row above says (Prefs.surfaceScale), so the surface
        // changes with the first step from off and the last one back.
        val tenths = (0..100 step 10).toList()
        rows.add(GameMenu.Choice(
            getString(R.string.menu_sharpening),
            tenths.map { if (it == 0) getString(R.string.state_off) else getString(R.string.menu_percent, it) },
            nearest(tenths, prefs.sharpening),
        ) {
            prefs.sharpening = tenths[it]
            NativeBridge.nativeApplyOption("MW2_SHARPEN", tenths[it].toString())
            resizeSurface()
        })

        // Auto first: the screen's rate, and under it what the device keeps.
        val limits = prefs.fpsLimits
        rows.add(GameMenu.Choice(
            getString(R.string.menu_fps_limit), listOf(getString(R.string.menu_auto)) + limits.map { it.toString() },
            if (prefs.fpsFollowsDevice) 0 else limits.indexOf(prefs.fpsLimit) + 1,
        ) {
            advanced {
                if (it == 0) prefs.followDeviceFps() else prefs.fpsLimit = limits[it - 1]
                applyFpsLimit()
            }
        })

        rows.add(GameMenu.Choice(
            getString(R.string.menu_sun_shadows), resources.getStringArray(R.array.menu_sun_shadows_choices).toList(),
            prefs.sunShadows,
        ) {
            advanced {
                prefs.sunShadows = it
                applySunShadows()
            }
        })

        val views = (65..110 step 5).toList()
        rows.add(GameMenu.Choice(
            getString(R.string.menu_fov), views.map { getString(R.string.menu_degrees, it) },
            nearest(views, prefs.fov),
        ) {
            prefs.fov = views[it]
            applyFov()
        })

        if (Title.hasDetailBias) {
            rows.add(GameMenu.Choice(
                getString(R.string.menu_detail), resources.getStringArray(R.array.detail_biases).toList(),
                Prefs.DETAIL_BIASES.indexOf(prefs.detailBias).coerceAtLeast(0),
            ) {
                prefs.detailBias = Prefs.DETAIL_BIASES[it]
                NativeBridge.nativeApplyOption("MW2_LOD_BIAS", prefs.detailBias.toString())
            })
        }

        // As Settings' slider: the device's own figure, then 128 MB steps to 1920.
        val budgets = (0..15).map { it * 128 }
        rows.add(GameMenu.Choice(
            getString(R.string.menu_texture_budget),
            budgets.map { if (it == 0) getString(R.string.menu_auto) else getString(R.string.menu_megabytes, it) },
            nearest(budgets, prefs.textureBudgetMB),
        ) {
            prefs.textureBudgetMB = budgets[it]
            NativeBridge.nativeApplyOption("MW2_TEXTURE_BUDGET_MB", budgets[it].toString())
        })

        rows.add(GameMenu.Choice(getString(R.string.menu_stats), onOff(), if (prefs.showStats) 1 else 0) { setStats(it == 1) })
        rows.add(GameMenu.Choice(getString(R.string.menu_fill_screen), onOff(), if (prefs.fillScreen) 1 else 0) {
            prefs.fillScreen = it == 1
            resizeSurface()
        })

        rows.add(GameMenu.Choice(
            getString(R.string.menu_render_scale), (1..3).map { getString(R.string.menu_times, it) },
            prefs.renderScale - 1, afterRestart = true,
        ) { advanced { prefs.renderScale = it + 1 } })
        rows.add(GameMenu.Choice(
            getString(R.string.menu_scene), resources.getStringArray(R.array.menu_scene_choices).toList(),
            prefs.scene, afterRestart = true,
        ) { advanced { prefs.scene = it } })
        driverRow()?.let { rows.add(it) }
        rows.add(GameMenu.Choice(
            getString(R.string.msaa), onOff(), if (prefs.multisampling) 1 else 0, afterRestart = true,
        ) { prefs.multisampling = it == 1 })

        return GameMenu.Page(
            getString(R.string.menu_graphics), rows,
            resources.getStringArray(R.array.preset_lines)[preset] + "\n" + getString(R.string.menu_after_restart),
        )
    }

    /**
     * A preset chosen in the menu: the frame rate, the sun's shadows and the
     * resolution are the running game's at once, the rest at the next
     * start, and the page is shown again with the preset's values in it.
     */
    private fun chooseGraphicsPreset(preset: Int) {
        prefs.choosePreset(preset)
        applyFpsLimit()
        applySunShadows()
        resizeSurface()
        menu.replace(graphicsPage())
    }

    /**
     * A setting of the Advanced page changed by itself, which makes the
     * preset Custom (Prefs): the page is shown again when that is news, for
     * its first row and the line under it.
     */
    private fun advanced(change: () -> Unit) {
        val before = prefs.graphicsPreset
        change()
        if (before != GraphicsPresets.CUSTOM) menu.replace(graphicsPage())
    }

    /**
     * The driver, where the build carries one: the phone's own, the app's,
     * and a third the player imported while that is the one in use (the
     * rest are in Settings). For the next start; null where there is only
     * the phone's.
     */
    private fun driverRow(): GameMenu.Row? {
        val bundled = DriverStore(this).bundled(prefs) ?: return null
        val bundledDirectory = bundled.directory.absolutePath
        val importedDirectory = prefs.driverDirectory
        val importedLibrary = prefs.driverLibrary
        val imported = importedDirectory.isNotEmpty() && importedDirectory != bundledDirectory
        val names = mutableListOf(getString(R.string.menu_driver_phone), getString(R.string.menu_driver_bundled))
        if (imported) names.add(getString(R.string.menu_driver_imported))
        val now = if (importedDirectory.isEmpty()) 0 else if (imported) 2 else 1
        return GameMenu.Choice(getString(R.string.graphics_driver), names, now, afterRestart = true) {
            prefs.driverChoiceMade = true
            prefs.driverDirectory = when (it) { 0 -> ""; 1 -> bundledDirectory; else -> importedDirectory }
            prefs.driverLibrary = when (it) { 0 -> ""; 1 -> bundled.library; else -> importedLibrary }
        }
    }

    /** The place of the value nearest [wanted]: a setting saved between two of the menu's steps. */
    private fun nearest(values: List<Int>, wanted: Int): Int =
        values.indices.minByOrNull { kotlin.math.abs(values[it] - wanted) } ?: 0

    private fun quitPage(): GameMenu.Page = GameMenu.Page(
        getString(R.string.quit),
        listOf(
            GameMenu.Entry(getString(R.string.cancel)) { menu.close() },
            GameMenu.Entry(getString(R.string.menu_quit_now)) { quit() },
        ),
        getString(R.string.quit_explained),
    )

    /**
     * The cap as Prefs has it, given to the running title: the runtime paces
     * by it from the next frame (frame_rate.h), and says which rate the
     * display is to be asked for (followHeldRate). With none chosen it is
     * the runtime's again, from the screen's rate, as a fresh install starts.
     */
    private fun applyFpsLimit() {
        if (prefs.fpsFollowsDevice) NativeBridge.nativeApplyOption("MW2_FPS_AUTO", "1")
        else NativeBridge.nativeApplyOption("MW2_FPS_LIMIT", prefs.fpsLimit.toString())
    }

    /** The sun's shadows as Prefs has them, given to the running title. */
    private fun applySunShadows() {
        NativeBridge.nativeApplyOption("MW2_SHADOWS", when (prefs.sunShadows) { 1 -> "1"; 2 -> "0"; else -> "" })
    }

    /**
     * The field of view as each title takes it: the runtime's own (MW2_FOV)
     * where the title's scripts would put cg_fov back, and cg_fov elsewhere.
     */
    private fun applyFov() {
        if (fovIsRuntimes) NativeBridge.nativeApplyOption("MW2_FOV", prefs.fov.coerceAtLeast(65).toString())
        else NativeBridge.nativeRunConsoleCommand("cg_fov ${prefs.fov}")
    }

    /**
     * How a finger and the gyroscope turn the view, as the runtime takes
     * it: at the start of a run and whenever the menu changes one, so a
     * setting is felt on the next swipe. The sensitivity is the player's
     * degrees a centimetre, made degrees a pixel of this screen.
     */
    private fun applyLook() {
        val sideways = TouchLook.degreesPerPixel(prefs.touchLookSensitivity, resources.displayMetrics.densityDpi)
        NativeBridge.nativeLookSettings(
            sideways, sideways * prefs.touchLookVertical, prefs.touchLookAiming, prefs.gyroMode,
        )
        // Which way each axis turns, for a finger and the gyroscope, and for
        // a controller's right stick (LookInvert).
        val invert = prefs.lookInvert
        overlay.lookInvert = invert
        gyro.invert = invert
        pads.invertRightX = prefs.invertPadHorizontal
        gyro.sensitivity = prefs.gyroSensitivity
        gyro.listen(started && inFront && prefs.gyroMode != Prefs.GYRO_OFF)
    }

    /** The menu's page for looking by touch and by the gyroscope; each row is taken as it is changed. */
    private fun lookPage(): GameMenu.Page {
        val rows = mutableListOf<GameMenu.Row>()

        val speeds = (TouchLook.MIN_DEGREES_PER_CM.toInt()..20).toList() +
            (22..TouchLook.MAX_DEGREES_PER_CM.toInt() step 2).toList()
        rows.add(GameMenu.Choice(
            getString(R.string.menu_look_sensitivity), speeds.map { getString(R.string.menu_degrees_per_cm, it) },
            nearest(speeds, prefs.touchLookSensitivity.roundToInt()),
        ) {
            prefs.touchLookSensitivity = speeds[it].toFloat()
            applyLook()
        })

        val shares = (25..200 step 5).toList()
        rows.add(GameMenu.Choice(
            getString(R.string.menu_look_vertical), shares.map { getString(R.string.menu_percent, it) },
            nearest(shares, (prefs.touchLookVertical * 100f).roundToInt()),
        ) {
            prefs.touchLookVertical = shares[it] / 100f
            applyLook()
        })
        rows.add(GameMenu.Choice(
            getString(R.string.menu_look_aiming), shares.map { getString(R.string.menu_percent, it) },
            nearest(shares, (prefs.touchLookAiming * 100f).roundToInt()),
        ) {
            prefs.touchLookAiming = shares[it] / 100f
            applyLook()
        })
        rows.add(GameMenu.Choice(getString(R.string.invert_horizontal), onOff(), if (prefs.invertLookHorizontal) 1 else 0) {
            prefs.invertLookHorizontal = it == 1
            applyLook()
        })
        rows.add(GameMenu.Choice(getString(R.string.invert_vertical), onOff(), if (prefs.invertLookVertical) 1 else 0) {
            prefs.invertLookVertical = it == 1
            applyLook()
        })
        // A controller's right stick: the title's own Invert option is for up and down.
        rows.add(GameMenu.Choice(getString(R.string.invert_pad_horizontal), onOff(), if (prefs.invertPadHorizontal) 1 else 0) {
            prefs.invertPadHorizontal = it == 1
            applyLook()
        })
        // Read by the on-screen pad as each touch comes up.
        rows.add(GameMenu.Choice(getString(R.string.tap_to_fire), onOff(), if (prefs.tapToFire) 1 else 0) {
            prefs.tapToFire = it == 1
        })
        // Read by the on-screen pad as Fire goes down (AimWhenFiring).
        rows.add(GameMenu.Choice(getString(R.string.aim_when_firing), onOff(), if (prefs.aimWhenFiring) 1 else 0) {
            prefs.aimWhenFiring = it == 1
        })

        if (gyro.available) {
            rows.add(GameMenu.Choice(
                getString(R.string.gyro_aiming), resources.getStringArray(R.array.gyro_choices).toList(), prefs.gyroMode,
            ) {
                prefs.gyroMode = it
                applyLook()
            })
            val quarters = (1..16).toList()
            rows.add(GameMenu.Choice(
                getString(R.string.menu_gyro_sensitivity), quarters.map { getString(R.string.menu_factor, it / 4f) },
                nearest(quarters, (prefs.gyroSensitivity * 4f).roundToInt()),
            ) {
                prefs.gyroSensitivity = quarters[it] / 4f
                applyLook()
            })
        }
        return GameMenu.Page(
            getString(R.string.menu_look), rows,
            getString(R.string.menu_look_note) + "\n\n" + getString(R.string.invert_pad_hint),
        )
    }

    /**
     * The bots, changed without leaving the game: the host's console takes
     * `bots <n>` (runtime/bots.cpp) and the server adds or drops them on its
     * next frame. Kept as the setting too, so the next start has the same.
     */
    private fun setBotCount(count: Int) {
        prefs.botCount = count
        NativeBridge.nativeRunConsoleCommand("bots $count")
    }

    // The title lobby's "Invite friends" for the network: the players this
    // copy hears, and the one tapped is invited. Never all of them: through
    // a server most are strangers.
    private fun inviteNetworkPlayer() {
        pickPlayer(R.string.invite_players, nobodyHeard(R.string.no_players), { NativeBridge.nativeInvitable() }) { id ->
            if (!NativeBridge.nativeInvitePlayer(id)) {
                android.widget.Toast.makeText(this, R.string.invite_needs_lobby, android.widget.Toast.LENGTH_LONG).show()
            }
        }
    }

    // Join a heard host right now, re-firing the join even if the automatic
    // one raced the menu and was swallowed.
    private fun pickNetworkPeer() {
        pickPlayer(R.string.join_game, nobodyHeard(R.string.no_peers), { NativeBridge.nativeLanJoinable() }) { id ->
            NativeBridge.nativeLanJoinPeer(id)
        }
    }

    // What an empty list says. On the local network with nobody heard at
    // all it is nearly always two WiFi networks: a device on a second
    // router or a mesh's own network hears nothing of the first, and
    // nothing else on the screen says so.
    private fun nobodyHeard(otherwise: Int): Int =
        if (prefs.networkBackend == "lan" && NativeBridge.nativeLanPeers() == 0) R.string.nobody_on_lan else otherwise

    // A list of players to tap one of. `lines` gives a line a player: the
    // id in hexadecimal, a tab, the name.
    private fun pickPlayer(title: Int, nobody: Int, lines: () -> String, chosen: (Long) -> Unit) {
        val peers = try {
            lines().split("\n").mapNotNull { line ->
                val id = line.substringBefore('\t').toULongOrNull(16) ?: return@mapNotNull null
                id.toLong() to line.substringAfter('\t', "")
            }
        } catch (_: Throwable) {
            emptyList()
        }
        if (peers.isEmpty()) {
            AlertDialog.Builder(this)
                .setMessage(nobody)
                .setPositiveButton(R.string.close, null)
                .setOnDismissListener { goFullscreen() }
                .show()
            return
        }
        // Two players may have the same name; the end of each one's id then
        // tells the two lines apart.
        val labels = peers.map { (id, name) ->
            if (peers.count { it.second == name } < 2) name
            else getString(R.string.peer_of_many, name, "%04X".format(id and 0xFFFF))
        }
        AlertDialog.Builder(this)
            .setTitle(title)
            .setItems(labels.toTypedArray()) { dialog, which ->
                dialog.dismiss()
                try {
                    chosen(peers[which].first)
                } catch (_: Throwable) {
                }
                goFullscreen()
            }
            .setOnDismissListener { goFullscreen() }
            .show()
    }

    private fun savesDir(): java.io.File {
        val gameDir = prefs.gameDirectory.ifEmpty {
            (getExternalFilesDir(null) ?: filesDir).absolutePath + "/game"
        }
        return java.io.File(title.home(java.io.File(gameDir)), "saves")
    }

    private fun runCheat(label: Int, cheat: (java.io.File) -> Int) {
        Thread {
            val text = try {
                val n = cheat(savesDir())
                getString(R.string.cheats_done, getString(label), n)
            } catch (e: Throwable) {
                getString(R.string.cheats_failed, e.message ?: "")
            }
            handler.post {
                AlertDialog.Builder(this)
                    .setTitle(label)
                    .setMessage(text)
                    .setPositiveButton(R.string.close, null)
                    .setOnDismissListener { goFullscreen() }
                    .show()
            }
        }.start()
    }

    private fun setTouchControls(on: Boolean) {
        prefs.touchControls = on
        overlay.releaseEverything()
        overlay.visibility = if (on) View.VISIBLE else View.GONE
    }

    private fun setGamepad(on: Boolean) {
        prefs.gamepadEnabled = on
        pads.setEnabled(on)
    }

    private fun setStats(on: Boolean) {
        prefs.showStats = on
        hudContainer.visibility = if (on) View.VISIBLE else View.GONE
    }

    private fun confirmQuit() {
        AlertDialog.Builder(this)
            .setTitle(R.string.quit)
            .setMessage(R.string.quit_explained)
            .setNegativeButton(R.string.cancel, null)
            .setPositiveButton(R.string.quit) { _, _ -> quit() }
            .setOnDismissListener { goFullscreen() }
            .show()
    }

    private fun quit() {
        overlay.layout.save(this)
        // The process is killed below: what the menu changed has to be in
        // the file by then.
        prefs.flush()
        if (NativeBridge.isLoaded()) NativeBridge.nativeRequestStop()
        finishAndRemoveTask()
        // The only way to end a guest that is inside recompiled code. The
        // launcher is another process and carries on.
        handler.postDelayed({ Process.killProcess(Process.myPid()) }, 120)
    }

    /**
     * [offerBuild]: the run needs the game code built, and this app can build
     * it: the dialog leads to that screen, which is the launcher's process's.
     */
    private fun fail(message: String, offerBuild: Boolean = false) {
        // In the log as well as on the screen: a tester sends the log, and a
        // run that was never started used to leave it with nothing to say.
        val line = "the run was not started: " + message.replace('\n', ' ')
        Trail.note(this, "game: $line")
        if (NativeBridge.isLoaded()) {
            try {
                NativeBridge.nativeLog(true, line)
            } catch (_: Throwable) {
                // A library without the call: the trail has the line.
            }
        }
        val dialog = AlertDialog.Builder(this)
            .setTitle(R.string.cannot_start)
            .setMessage(message)
            .setCancelable(false)
        val leave = {
            finish()
            handler.postDelayed({ Process.killProcess(Process.myPid()) }, 80)
        }
        if (offerBuild) {
            dialog.setPositiveButton(R.string.setup_build_here) { _, _ ->
                startActivity(Intent(this, BuildActivity::class.java))
                leave()
            }
            dialog.setNegativeButton(R.string.close) { _, _ -> leave() }
        } else {
            dialog.setPositiveButton(R.string.close) { _, _ -> leave() }
        }
        dialog.show()
    }

    // ---- editing the pad -----------------------------------------------------------

    private fun beginEditing() {
        if (!prefs.touchControls) setTouchControls(true)
        overlay.editing = true
        overlay.select(null)
        menuButton.visibility = View.GONE
        if (editPanel == null) editPanel = EditPanel()
        editPanel?.attach()
        NativeBridge.nativeSetPaused(true)
    }

    private fun endEditing(save: Boolean) {
        overlay.editing = false
        overlay.select(null)
        editPanel?.detach()
        menuButton.visibility = View.VISIBLE
        if (save) overlay.layout.save(this)
        NativeBridge.nativeSetPaused(false)
        goFullscreen()
    }

    /**
     * The editor's own strip along the bottom. Everything on it acts on
     * whichever control is selected; dragging and pinching happen on the
     * control itself.
     */
    private inner class EditPanel {
        private val view: View = layoutInflater.inflate(R.layout.edit_panel, root, false)
        private val title: TextView = view.findViewById(R.id.edit_title)
        private val sizeRow: View = view.findViewById(R.id.edit_size_row)
        private val size: SeekBar = view.findViewById(R.id.edit_size)
        private val opacity: SeekBar = view.findViewById(R.id.edit_opacity)
        private val visible: SwitchCompat = view.findViewById(R.id.edit_visible)
        private val toggle: SwitchCompat = view.findViewById(R.id.edit_toggle)
        private val snap: SwitchCompat = view.findViewById(R.id.edit_snap)
        private var current: ControlElement? = null
        private var attached = false

        init {
            size.max = 100
            opacity.max = 100
            size.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(bar: SeekBar, value: Int, fromUser: Boolean) {
                    if (!fromUser) return
                    val element = current ?: return
                    element.sizeDp = ControlLayout.MIN_SIZE_DP +
                        (ControlLayout.MAX_SIZE_DP - ControlLayout.MIN_SIZE_DP) * (value / 100f)
                    overlay.refresh()
                }

                override fun onStartTrackingTouch(bar: SeekBar) = Unit
                override fun onStopTrackingTouch(bar: SeekBar) {
                    overlay.layout.save(this@GameActivity)
                }
            })
            opacity.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(bar: SeekBar, value: Int, fromUser: Boolean) {
                    if (!fromUser) return
                    val element = current ?: return
                    element.alpha = (value / 100f).coerceIn(0.05f, 1f)
                    overlay.refresh()
                }

                override fun onStartTrackingTouch(bar: SeekBar) = Unit
                override fun onStopTrackingTouch(bar: SeekBar) {
                    overlay.layout.save(this@GameActivity)
                }
            })
            visible.setOnCheckedChangeListener { _, checked ->
                val element = current ?: return@setOnCheckedChangeListener
                element.visible = checked
                overlay.refresh()
                overlay.layout.save(this@GameActivity)
            }
            toggle.setOnCheckedChangeListener { _, checked ->
                val element = current ?: return@setOnCheckedChangeListener
                element.toggle = checked
                overlay.layout.save(this@GameActivity)
            }
            snap.setOnCheckedChangeListener { _, checked -> overlay.snapToGrid = checked }
            view.findViewById<Button>(R.id.edit_reset_one).setOnClickListener {
                val element = current ?: return@setOnClickListener
                overlay.layout.reset(element.id)
                overlay.select(overlay.layout.find(element.id))
                overlay.refresh()
                overlay.layout.save(this@GameActivity)
            }
            view.findViewById<Button>(R.id.edit_reset_all).setOnClickListener {
                AlertDialog.Builder(this@GameActivity)
                    .setTitle(R.string.reset_all)
                    .setMessage(R.string.reset_all_explained)
                    .setNegativeButton(R.string.cancel, null)
                    .setPositiveButton(R.string.reset) { _, _ ->
                        overlay.layout.resetAll()
                        overlay.select(null)
                        overlay.refresh()
                        overlay.layout.save(this@GameActivity)
                    }
                    .show()
            }
            view.findViewById<Button>(R.id.edit_done).setOnClickListener { endEditing(save = true) }
        }

        fun attach() {
            if (attached) return
            root.addView(
                view,
                FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                    Gravity.BOTTOM,
                )
            )
            attached = true
            show(null)
        }

        fun detach() {
            if (!attached) return
            root.removeView(view)
            attached = false
        }

        fun show(element: ControlElement?) {
            current = element
            if (element == null) {
                title.setText(R.string.edit_pick_one)
                sizeRow.visibility = View.VISIBLE
                size.isEnabled = false
                opacity.isEnabled = false
                visible.isEnabled = false
                toggle.isEnabled = false
                return
            }
            // The look area has no size or opacity: it can be moved to the
            // other half, or switched off.
            val drawn = element.adjustable
            title.text = getString(if (drawn) R.string.edit_selected else R.string.edit_selected_fixed, element.label)
            sizeRow.visibility = if (drawn) View.VISIBLE else View.GONE
            size.isEnabled = drawn
            opacity.isEnabled = drawn
            visible.isEnabled = true
            toggle.isEnabled = element.kind == ControlKind.BUTTON || element.kind == ControlKind.TRIGGER
            size.progress = (
                (element.sizeDp - ControlLayout.MIN_SIZE_DP) /
                    (ControlLayout.MAX_SIZE_DP - ControlLayout.MIN_SIZE_DP) * 100f
                ).roundToInt().coerceIn(0, 100)
            opacity.progress = (element.alpha * 100f).roundToInt().coerceIn(0, 100)
            visible.isChecked = element.visible
            toggle.isChecked = element.toggle
        }
    }

    // ---- input ------------------------------------------------------------------------

    // Select and Start as a pad holds them, for the two together.
    private var menuButtonsHeld = 0

    /**
     * Select and Start held together open the menu: the way in for a player
     * with a pad and the on-screen controls off, who has no Menu button to
     * reach and whose B is the game's. It is counted whether or not pads are
     * listened to, so a pad switched off in the menu can be switched on again
     * from it. The second of the two is not passed on; the first already was,
     * and showMenu lets it go.
     */
    private fun opensMenu(event: KeyEvent): Boolean {
        val bit = when (event.keyCode) {
            KeyEvent.KEYCODE_BUTTON_SELECT -> 1
            KeyEvent.KEYCODE_BUTTON_START -> 2
            else -> return false
        }
        if (event.action != KeyEvent.ACTION_DOWN) {
            menuButtonsHeld = menuButtonsHeld and bit.inv()
            return false
        }
        menuButtonsHeld = menuButtonsHeld or bit
        if (menuButtonsHeld != 3) return false
        // Their coming up is the menu's to see, not this count's.
        menuButtonsHeld = 0
        return true
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        // The menu, the dialogs and the editor come first; a pad should not
        // fire a gun through a menu.
        if (started && !overlay.editing) {
            // The help of a first run is closed by the pad, and passes nothing on.
            if (help.isOpen && help.onKey(event)) return true
            if (menu.isOpen) return menu.onKey(event) || super.dispatchKeyEvent(event)
            if (opensMenu(event)) {
                showMenu()
                return true
            }
            if (pads.onKey(event)) return true
        }
        return super.dispatchKeyEvent(event)
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        if (started && !overlay.editing) {
            if (menu.isOpen) return menu.onMotion(event) || super.onGenericMotionEvent(event)
            if (pads.onMotion(event)) return true
        }
        return super.onGenericMotionEvent(event)
    }

    // ---- lifecycle ---------------------------------------------------------------------

    override fun onResume() {
        super.onResume()
        inFront = true
        goFullscreen()
        if (!started) return
        NativeBridge.nativeSetPaused(false)
        // A trim while the window was away may have cut the texture cache.
        // Without this it stays cut, and every texture is uploaded again on
        // every draw for the rest of the run.
        NativeBridge.nativeRestoreMemory()
        pads.setEnabled(prefs.gamepadEnabled)
        pads.refreshDevices()
        applyLook()
        // Time away is not time without a frame.
        watchingSince = SystemClock.elapsedRealtime()
        // onCreate posted it too, before the first onResume: one chain.
        handler.removeCallbacks(statsTick)
        handler.post(statsTick)
    }

    override fun onPause() {
        super.onPause()
        inFront = false
        if (!started) return
        // Nothing is presented while the activity is away, the mixer is
        // muted, and the touch pad lets go of whatever was held -- a trigger
        // stuck down through a phone call would empty a magazine into a wall.
        NativeBridge.nativeSetPaused(true)
        overlay.releaseEverything()
        gyro.listen(false)
        handler.removeCallbacks(statsTick)
        overlay.layout.save(this)
        // A game left in the background may be killed without another word.
        prefs.flush()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) goFullscreen()
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        goFullscreen()
    }

    override fun onTrimMemory(level: Int) {
        super.onTrimMemory(level)
        // The texture cache is the only thing here big enough to be worth
        // giving back, and it can rebuild what it drops
        // (runtime/android/perf.cpp).
        // UI_HIDDEN (20) says the window went away, not that memory is
        // short, and it arrives on every switch away. It is passed on so the
        // runtime can say so in the log, but the levels that mean pressure
        // while running are RUNNING_LOW and RUNNING_CRITICAL.
        if (NativeBridge.isLoaded() && level >= ComponentCallbacks2.TRIM_MEMORY_RUNNING_LOW) {
            NativeBridge.nativeTrimMemory(level)
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        monitoringActive = false
        monitorThread?.interrupt()
        monitorThread = null
        handler.removeCallbacksAndMessages(null)
        getSystemService(DisplayManager::class.java).unregisterDisplayListener(displayListener)
        pads.stopListening()
        NativeBridge.listener = null
        if (started && !isChangingConfigurations) {
            // The activity is going for good: the run goes with it.
            GameService.stop(this)
            NativeBridge.nativeRequestStop()
            Process.killProcess(Process.myPid())
        }
    }

    // ---- what the runtime tells the app ---------------------------------------------------

    override fun onRumble(user: Int, lowFrequency: Int, highFrequency: Int) {
        if (!prefs.vibration || user != 0) return
        val strength = max(lowFrequency, highFrequency)
        // Only on a change, and the runtime only calls on a change, so this
        // is not a vibration started sixty times a second.
        if (strength == lastRumble) return
        lastRumble = strength
        val device = vibrator ?: return
        handler.post {
            try {
                if (strength <= 0) {
                    device.cancel()
                } else {
                    val amplitude = (strength * 255 / 65535).coerceIn(1, 255)
                    device.vibrate(VibrationEffect.createOneShot(160, amplitude))
                }
            } catch (_: Throwable) {
                // A phone that refuses to vibrate is not a reason to stop
                // playing.
            }
        }
    }

    /**
     * The title's own way between the campaign and the multiplayer
     * (XamLoaderLaunchTitle). The other executable is another library, and a
     * process holds one, so the launcher's process is asked to start the run
     * again once this one has ended, which it does as soon as this returns
     * (runtime/main.cpp, crash::RequestExit). An app that does not carry the
     * title asked for does nothing here, and the run simply ends.
     */
    override fun onNextTitle(executable: String, launchData: String) {
        val next = Title.forExecutable(executable) ?: return
        HandOffActivity.start(this, RunRequest(next, launchData))
    }

    override fun onStatus(text: String) {
        handler.post {
            if (isFinishing) return@post
            // A renderer that did not come up is not said here: the runtime
            // keeps why, and the watch for a black screen shows it (statsTick).
            if (text.isNotEmpty() && prefs.showStats) statsText.text = text
        }
    }

    companion object {
        /**
         * How long a run may show nothing before the player is told: every
         * title's first picture is up within ten seconds on the slower of the
         * two devices measured. And with every shader compiled before the
         * first frame, which a cold cache makes minutes.
         */
        private const val NO_FRAME_MS = 30_000L
        private const val NO_FRAME_PREWARM_MS = 600_000L

        /**
         * Started by the launcher once the files are in place; `then` runs
         * once the run has been started. The multiplayer's first start asks
         * for the player's name before it, with the device's own filled in:
         * the other players see it, and few look for it in Settings.
         */
        fun start(activity: Activity, title: Title, launchData: String = "", level: String = "", then: () -> Unit = {}) {
            val go = {
                RunRequest(title, launchData, level).write(activity)
                activity.startActivity(Intent(activity, GameActivity::class.java))
                then()
            }
            val prefs = Prefs(activity)
            if (title != Title.MULTIPLAYER || prefs.playerNameAsked || prefs.networkMode == Prefs.NETWORK_OFF) {
                go()
                return
            }
            val name = EditText(activity).apply {
                setText(prefs.playerName)
                hint = prefs.defaultPlayerName
                isSingleLine = true
                inputType = android.text.InputType.TYPE_CLASS_TEXT
                filters = arrayOf(android.text.InputFilter.LengthFilter(Prefs.MAX_PLAYER_NAME))
                setSelectAllOnFocus(true)
            }
            val margin = (20 * activity.resources.displayMetrics.density).toInt()
            val frame = FrameLayout(activity).apply {
                setPadding(margin, margin / 4, margin, 0)
                addView(name)
            }
            AlertDialog.Builder(activity)
                .setTitle(R.string.player_name)
                .setMessage(R.string.player_name_ask)
                .setView(frame)
                .setPositiveButton(R.string.play) { _, _ ->
                    prefs.playerName = name.text.toString()
                    prefs.playerNameAsked = true
                    go()
                }
                .setNegativeButton(R.string.cancel, null)
                .show()
        }
    }
}
