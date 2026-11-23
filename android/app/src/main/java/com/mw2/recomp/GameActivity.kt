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

    /** What the other title left for this run (RunRequest), as hexadecimal. */
    private var launchData = ""

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
    private var editPanel: EditPanel? = null

    private val pads = GamepadInput()
    private val handler = Handler(Looper.getMainLooper())
    private var vibrator: Vibrator? = null
    private var lastRumble = 0

    private val statsValues = FloatArray(9)
    private var started = false

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
            if (prefs.showStats && NativeBridge.isLoaded()) {
                NativeBridge.nativeStats(statsValues)
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

                val temp = getDeviceTemp()
                val (appMb, totalMb) = getMemoryStats()
                val (battTemp, isCharging, battPct) = getBatteryStats()
                val cpuStr = if (cpuUsagePercent >= 0f) String.format(Locale.US, "%.0f%%", cpuUsagePercent) else "N/A"

                val warmupQueued = if (statsValues.size >= 7) statsValues[6].toInt() else 0
                val warmupCompleted = if (statsValues.size >= 8) statsValues[7].toInt() else 0
                val warmupLastMs = if (statsValues.size >= 9) statsValues[8].toInt() else 0

                val tempPart = if (temp > 0f) String.format(Locale.US, " · %.0f°C", temp) else ""
                val ramPart = if (appMb > 0) String.format(Locale.US, " · RAM %dM", appMb) else ""
                val ftPart = if (frameTimeMs > 0f) String.format(Locale.US, " · %.1fms", frameTimeMs) else ""
                val warmupPart = if (warmupQueued > 0 && warmupCompleted < warmupQueued) {
                    val pct = (warmupCompleted * 100) / warmupQueued
                    String.format(Locale.US, "  •  ⚡ Shaders %d%% (%d/%d)", pct, warmupCompleted, warmupQueued)
                } else ""

                // Other copies heard on the network (multiplayer only). Zero
                // with a friend hosting beside this phone means discovery is
                // not arriving -- same WiFi, firewall, hotspot; see the guide.
                // The TU badge matters: TU0 plays only with TU0, TU6 with TU6.
                val isMpHud = isMultiplayer
                val tuHud = BuildConfig.MW2_TU.uppercase()
                val lanPeers = if (isMpHud) {
                    try {
                        NativeBridge.nativeLanPeers()
                    } catch (_: Throwable) {
                        0
                    }
                } else 0
                val lanPart = if (isMpHud) {
                    String.format(Locale.US, "  •  LAN: %d · %s", lanPeers, tuHud)
                } else {
                    String.format(Locale.US, "  •  %s", tuHud)
                }

                statsText.text = String.format(
                    Locale.US,
                    "%d FPS%s  •  CPU %s%s%s  •  Tex %dMB%s%s  [▼ HUD]",
                    fps, ftPart, cpuStr, tempPart, ramPart, texMb, warmupPart, lanPart
                )

                if (hudIsExpanded) {
                    val parts = cachedRendererInfo.split("|")
                    val devName = if (parts.isNotEmpty() && parts[0].isNotEmpty()) parts[0] else "Vulkan Device"
                    val renderMode = if (parts.size > 1) parts[1] else "Vulkan 1.1 Legacy RenderPass"
                    val tcMode = if (parts.size > 2) parts[2] else "CPU Decompress Fallback"

                    val battStr = if (battTemp > 0f) {
                        val chg = if (isCharging) " (Chg $battPct%)" else if (battPct >= 0) " ($battPct%)" else ""
                        String.format(Locale.US, "%.1f°C%s", battTemp, chg)
                    } else "N/A"

                    val ramStr = if (totalMb > 0) String.format(Locale.US, "%d MB (App) / %d MB (Total)", appMb, totalMb)
                                 else if (appMb > 0) "$appMb MB" else "N/A"
                    val cpuTempStr = if (temp > 0f) String.format(Locale.US, "%.0f°C", temp) else "N/A"

                    hudExpandedText.text = buildString {
                        append("FPS / FRAME TIME\n")
                        append(String.format(Locale.US, "  ▶ %d FPS   (%.1f ms)\n", fps, frameTimeMs))
                        append(String.format(Locale.US, "  Presented: %,d frames\n\n", framesPresented))

                        append("CPU & SYSTEM\n")
                        append("  Utilization: $cpuStr (${Runtime.getRuntime().availableProcessors()} Cores)\n")
                        append("  SoC/Thermal: $cpuTempStr  ·  Battery: $battStr\n\n")

                        append("GPU & VULKAN\n")
                        append("  Device: $devName\n")
                        append("  Backend: $renderMode\n")
                        append("  Textures: $texMb MB  ·  $tcMode\n")
                        if (warmupQueued > 0) {
                            val pct = (warmupCompleted * 100) / warmupQueued
                            append("  Prewarm: $warmupCompleted / $warmupQueued ($pct%)  ·  Last: ${warmupLastMs}ms\n\n")
                        } else {
                            append("\n")
                        }

                        append("MEMORY & VIEWPORT\n")
                        append("  App / System Total RAM: $ramStr\n")
                        append("  Internal Render: ${resW}×${resH}  •  Refresh: ${refreshHz().roundToInt()} Hz\n")
                        if (isMultiplayer) {
                            append("\nNETWORK\n")
                            append("  Players heard on LAN: $lanPeers\n")
                        }
                    }
                }
            }
            handler.postDelayed(this, 500)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        prefs = Prefs(this)

        Trail.note(this, "game: starting")
        val request = RunRequest.read(this)
        // Launch data is for the run it was left for, not for whichever run
        // comes after it.
        if (request.launchData.isNotEmpty()) RunRequest(request.title, "").write(this)
        launchData = request.launchData
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

        buildUi()
        goFullscreen()
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        // Sustained performance mode locks CPU/GPU governors to steady frequencies
        // to prevent erratic thermal throttling mid-gameplay.
        window.setSustainedPerformanceMode(true)
        if (prefs.fpsLimit in Prefs.FPS_LIMITS) DisplayRate.request(window, shownDisplay(), prefs.fpsLimit)
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
            fail(it)
            return
        }

        applyOptions()
        applyDriver()

        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                if (overlay.editing) endEditing(save = true) else showMenu()
            }
        })

        val gameDirectory = prefs.gameDirectory
        if (gameDirectory.isEmpty() || !NativeBridge.nativeGameInstalled(gameDirectory)) {
            fail(getString(R.string.error_not_installed))
            return
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
        // The run is up, so ask the system to treat its process as one still
        // wanted: stepping out to a message then costs a pause, not the whole
        // load again (GameService.kt).
        GameService.start(this)
        Trail.clear(this)
        pads.setEnabled(prefs.gamepadEnabled)
        pads.refreshDevices()
        startHardwareMonitor()
        handler.post(statsTick)
        // Modern Warfare 3's multiplayer takes the field of view as MW2_FOV
        // (applyOptions): its scripts put cg_fov back at every spawn.
        if (!isMw3Multiplayer) {
            handler.postDelayed({
                if (started) {
                    NativeBridge.nativeRunConsoleCommand("cg_fov ${prefs.fov}")
                }
            }, 2500)
        }
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
                val scale = prefs.resolutionScale
                val wantedHeight = max(240, (surfaceView.height * scale).roundToInt())
                val wantedWidth = if (prefs.fillScreen) max(320, (wantedHeight * 16f / 9f).roundToInt()) else max(320, (surfaceView.width * scale).roundToInt())
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
            text = "MW2 ANDROID · PERFORMANCE"
            setTextColor(Color.argb(255, 255, 122, 26))
            textSize = 11f
            typeface = Typeface.DEFAULT_BOLD
        }
        val hudCollapseBtn = TextView(this).apply {
            text = "  [▲ Close]"
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
            text = " [⚡ CMD]"
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

        setContentView(root)
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
            hint = "e.g. god, give all, cg_fov 85, cg_drawFPS 1..."
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
            "cg_fov 85" to "Ultrawide (85°)",
            "cg_fov 65" to "Default (65°)",
            "cg_drawFPS 1" to "Show FPS",
            "god" to "God Mode",
            "give all" to "All Weapons",
            "ufo" to "UFO / Fly",
            "r_fullbright 1" to "Full Bright",
            "cg_drawGun 0" to "Hide Gun",
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
            .setTitle("IW4 Console Command")
            .setView(container)
            .setPositiveButton("Run") { _, _ ->
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
        // MP refuses to start matches without a link; runtime defaults to up
        // for MP and down for SP (kernel/net.cpp), but be explicit so a
        // misbuilt lib still reports a link in the MP app.
        if (isMw2Multiplayer) {
            NativeBridge.nativeSetOption("MW2_NET_LINK", "1")
        }
        NativeBridge.nativeSetOption("MW2_NO_AUDIO", if (prefs.audioEnabled) "" else "1")
        NativeBridge.nativeSetOption("MW2_NO_MSAA", if (prefs.multisampling) "" else "1")
        if (prefs.renderScale in 1..3) {
            NativeBridge.nativeSetOption("MW2_SCALE", prefs.renderScale.toString())
        }
        // Modern Warfare 2 alone has the hook (runtime/model_detail.cpp).
        if (!Title.isMw3) NativeBridge.nativeSetOption("MW2_LOD_BIAS", prefs.detailBias.toString())
        NativeBridge.nativeSetOption("MW2_FPS_LIMIT", prefs.fpsLimit.toString())
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
        // Modern Warfare 3's multiplayer: every item unlocked, the host's match
        // options, the field of view and voice chat (the other titles read
        // none of them here).
        if (isMw3Multiplayer) {
            NativeBridge.nativeSetOption("MW2_UNLOCK_ALL", if (prefs.unlockAllItems) "1" else "")
            NativeBridge.nativeSetOption("MW2_MATCH_GRAVITY", prefs.matchGravity.toString())
            NativeBridge.nativeSetOption("MW2_MATCH_SPEED", prefs.matchSpeed.toString())
            NativeBridge.nativeSetOption("MW2_MATCH_JUMP", prefs.matchJump.toString())
            NativeBridge.nativeSetOption("MW2_MATCH_SPRINT", if (prefs.matchUnlimitedSprint) "1" else "")
            NativeBridge.nativeSetOption("MW2_MATCH_FALL_DAMAGE", if (prefs.matchFallDamage) "1" else "0")
            NativeBridge.nativeSetOption("MW2_FOV", prefs.fov.coerceAtLeast(65).toString())
            NativeBridge.nativeSetOption("MW2_MAP_VOTE", if (prefs.mapVote) "1" else "")
            NativeBridge.nativeSetOption("MW2_VOICE", if (voiceChat) "1" else "")
            NativeBridge.nativeSetOption("MW2_VOICE_PTT", if (prefs.voicePushToTalk) "1" else "0")
        }
        // Mali drivers have had trouble reloading persisted Vulkan pipeline blobs.
        // Respect the user's setting; the safe default is off on this device class.
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
        NativeBridge.nativeLookSettings(
            prefs.lookSensitivityX, prefs.lookSensitivityY,
            prefs.lookSmoothing, prefs.invertLook, prefs.lookSaturation,
        )
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

    // ---- the pause menu ----------------------------------------------------------

    private fun showMenu() {
        // Built as a list: the invite entry only exists in the multiplayer
        // app, so fixed indices would point at the wrong rows there.
        val labels = mutableListOf(
            getString(R.string.resume) to { _: android.content.DialogInterface -> },
            getString(R.string.edit_controls) to { _: android.content.DialogInterface -> beginEditing() },
        )
        // The Live lobby's invitations: MW2's and Call of Duty 4's, and
        // MW3's when it is signed in, which through a relay it is
        // (runtime/online/service.cpp, online::Live) -- its private match
        // had a lobby nobody could be asked into or find.
        if (isMw2Multiplayer ||
            (isMultiplayer && (prefs.liveSignIn || prefs.networkBackend == "server"))) {
            labels.add(getString(R.string.invite_players) to { _: android.content.DialogInterface -> inviteNetworkPlayers() })
            labels.add(getString(R.string.join_game) to { _: android.content.DialogInterface -> pickNetworkPeer() })
        }
        // Modern Warfare 2's stat cheats write that game's stats file
        // (MpCheatsData's offsets): Call of Duty 4's is laid out differently,
        // so its multiplayer is not offered them.
        if (isMw2Multiplayer && !Title.isCod4) {
            labels.add(getString(R.string.max_rank) to { _: android.content.DialogInterface -> runCheat(R.string.max_rank, Cheats::maxRank) })
            labels.add(getString(R.string.unlock_all) to { _: android.content.DialogInterface -> runCheat(R.string.unlock_all, Cheats::unlockEverything) })
            labels.add(getString(R.string.prestige_up) to { _: android.content.DialogInterface -> runCheat(R.string.prestige_up, Cheats::prestigeUp) })
        }
        if (hasBots) {
            val bots = if (prefs.botCount == 0) getString(R.string.bot_count_none) else getString(R.string.bot_count, prefs.botCount)
            labels.add(bots to { _: android.content.DialogInterface -> pickBotCount() })
        }
        labels.add(getString(if (prefs.touchControls) R.string.hide_touch_controls else R.string.show_touch_controls) to { _: android.content.DialogInterface -> setTouchControls(!prefs.touchControls) })
        labels.add(getString(if (prefs.gamepadEnabled) R.string.disable_gamepad else R.string.enable_gamepad) to { _: android.content.DialogInterface -> setGamepad(!prefs.gamepadEnabled) })
        labels.add(getString(if (prefs.showStats) R.string.hide_stats else R.string.show_stats) to { _: android.content.DialogInterface -> setStats(!prefs.showStats) })
        labels.add(getString(R.string.quit) to { _: android.content.DialogInterface -> confirmQuit() })
        AlertDialog.Builder(this)
            .setTitle(R.string.paused)
            .setItems(labels.map { it.first }.toTypedArray()) { dialog, which ->
                dialog.dismiss()
                labels[which].second(dialog)
            }
            .setOnDismissListener { goFullscreen() }
            .show()
    }

    /**
     * The bots, changed without leaving the game: the host's console takes
     * `bots <n>` (runtime/bots.cpp) and the server adds or drops them on its
     * next frame. Kept as the setting too, so the next start has the same.
     */
    private fun pickBotCount() {
        val counts = Array(Prefs.MAX_BOTS + 1) { if (it == 0) getString(R.string.bot_count_none) else getString(R.string.bot_count, it) }
        AlertDialog.Builder(this)
            .setTitle(R.string.bots_in_game_title)
            .setSingleChoiceItems(counts, prefs.botCount) { dialog, which ->
                prefs.botCount = which
                if (NativeBridge.isLoaded()) {
                    try {
                        NativeBridge.nativeRunConsoleCommand("bots $which")
                    } catch (_: Throwable) {
                    }
                }
                dialog.dismiss()
            }
            .setOnDismissListener { goFullscreen() }
            .show()
    }

    // The title lobby's "Invite friends" for the network: invites every
    // player this copy hears. Nothing happens with nobody heard yet -- the
    // HUD's LAN line says how many that is.
    private fun inviteNetworkPlayers() {
        if (NativeBridge.isLoaded()) {
            try {
                NativeBridge.nativeInviteFriends()
            } catch (_: Throwable) {
            }
        }
    }

    // Join a heard host right now, re-firing the join even if the automatic
    // one raced the menu and was swallowed.
    private fun pickNetworkPeer() {
        // A line a peer: its id in hexadecimal, a tab, its name.
        val peers = try {
            NativeBridge.nativeLanJoinable().split("\n").mapNotNull { line ->
                val id = line.substringBefore('\t').toULongOrNull(16) ?: return@mapNotNull null
                id.toLong() to line.substringAfter('\t', "")
            }
        } catch (_: Throwable) {
            emptyList()
        }
        if (peers.isEmpty()) {
            AlertDialog.Builder(this)
                .setMessage(R.string.no_peers)
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
            .setTitle(R.string.join_game)
            .setItems(labels.toTypedArray()) { dialog, which ->
                dialog.dismiss()
                try {
                    NativeBridge.nativeLanJoinPeer(peers[which].first)
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
        if (NativeBridge.isLoaded()) NativeBridge.nativeRequestStop()
        finishAndRemoveTask()
        // The only way to end a guest that is inside recompiled code. The
        // launcher is another process and carries on.
        handler.postDelayed({ Process.killProcess(Process.myPid()) }, 120)
    }

    private fun fail(message: String) {
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
        AlertDialog.Builder(this)
            .setTitle(R.string.cannot_start)
            .setMessage(message)
            .setCancelable(false)
            .setPositiveButton(R.string.close) { _, _ ->
                finish()
                handler.postDelayed({ Process.killProcess(Process.myPid()) }, 80)
            }
            .show()
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
                    val smallest = if (element.kind == ControlKind.LOOK) 120f else 36f
                    val largest = if (element.kind == ControlKind.LOOK) 900f else 200f
                    element.sizeDp = smallest + (largest - smallest) * (value / 100f)
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
                size.isEnabled = false
                opacity.isEnabled = false
                visible.isEnabled = false
                toggle.isEnabled = false
                return
            }
            title.text = getString(R.string.edit_selected, element.label)
            size.isEnabled = true
            opacity.isEnabled = true
            visible.isEnabled = true
            toggle.isEnabled = element.kind == ControlKind.BUTTON || element.kind == ControlKind.TRIGGER
            val smallest = if (element.kind == ControlKind.LOOK) 120f else 36f
            val largest = if (element.kind == ControlKind.LOOK) 900f else 200f
            size.progress = (((element.sizeDp - smallest) / (largest - smallest)) * 100f)
                .roundToInt().coerceIn(0, 100)
            opacity.progress = (element.alpha * 100f).roundToInt().coerceIn(0, 100)
            visible.isChecked = element.visible
            toggle.isChecked = element.toggle
        }
    }

    // ---- input ------------------------------------------------------------------------

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        // The dialogs and the editor come first; a pad should not fire a gun
        // through a menu.
        if (started && !overlay.editing && pads.onKey(event)) return true
        return super.dispatchKeyEvent(event)
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        if (started && !overlay.editing && pads.onMotion(event)) return true
        return super.onGenericMotionEvent(event)
    }

    // ---- lifecycle ---------------------------------------------------------------------

    override fun onResume() {
        super.onResume()
        goFullscreen()
        if (!started) return
        NativeBridge.nativeSetPaused(false)
        // A trim while the window was away may have cut the texture cache.
        // Without this it stays cut, and every texture is uploaded again on
        // every draw for the rest of the run.
        NativeBridge.nativeRestoreMemory()
        pads.setEnabled(prefs.gamepadEnabled)
        pads.refreshDevices()
        handler.post(statsTick)
    }

    override fun onPause() {
        super.onPause()
        if (!started) return
        // Nothing is presented while the activity is away, the mixer is
        // muted, and the touch pad lets go of whatever was held -- a trigger
        // stuck down through a phone call would empty a magazine into a wall.
        NativeBridge.nativeSetPaused(true)
        overlay.releaseEverything()
        handler.removeCallbacks(statsTick)
        overlay.layout.save(this)
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
            if (text.startsWith("GPU lacks") || text.startsWith("Failed to") || text.startsWith("No Vulkan")) {
                AlertDialog.Builder(this)
                    .setTitle("Renderer Notice")
                    .setMessage("$text\n\nYour device GPU does not provide Vulkan 1.3 or VK_KHR_dynamic_rendering. The game will run in headless mode without video output. Check the log for details.")
                    .setPositiveButton("View Log") { _, _ ->
                        finish()
                    }
                    .setNegativeButton("Continue Headless", null)
                    .show()
            } else if (text.isNotEmpty() && prefs.showStats) {
                statsText.text = text
            }
        }
    }

    companion object {
        /**
         * Started by the launcher once the files are in place; `then` runs
         * once the run has been started. The multiplayer's first start asks
         * for the player's name before it, with the device's own filled in:
         * the other players see it, and few look for it in Settings.
         */
        fun start(activity: Activity, title: Title, then: () -> Unit = {}) {
            val go = {
                RunRequest(title, "").write(activity)
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
