package com.mw2.recomp

/**
 * Everything the app says to the runtime and everything the runtime says
 * back. The other side is runtime/android/jni.cpp; the names here and the
 * names there are the same word for word, so changing one means changing
 * both.
 */
object NativeBridge {

    /** What the runtime calls on its own threads, never on the main one. */
    @Volatile
    var listener: NativeListener? = null

    private var loadedTitle: Title? = null
    private var loadError: String? = null

    /**
     * Loads a title's library. Called once, late, and allowed to fail: a
     * device whose Vulkan is too old, or an apk built for another ABI, still
     * reaches the launcher, which then says what is wrong instead of
     * disappearing. One title to a process: both libraries export the names
     * below, and the first loaded is the one they are bound to, so asking for
     * the other afterwards is refused.
     */
    @Synchronized
    fun load(title: Title): Boolean {
        loadedTitle?.let { return it == title }
        return try {
            System.loadLibrary(title.runLibrary)
            loadedTitle = title
            true
        } catch (error: Throwable) {
            loadError = error.message ?: error.toString()
            false
        }
    }

    fun isLoaded(): Boolean = loadedTitle != null

    /** The title this process has loaded, if any. */
    fun title(): Title? = loadedTitle
    fun loadError(): String? = loadError

    /**
     * A library that loaded but would not start. An UnsatisfiedLinkError on
     * the first call means the .so and this file have drifted apart, which
     * the launcher should say rather than die of.
     */
    fun noteInitFailure(reason: String) {
        loadError = reason
    }

    /**
     * The runtime for a screen of the launcher's process: the app's first
     * title's library loaded and initialised, which the installer and the
     * log need. False when it would not load or start ([loadError] says
     * why). The launcher does this when it opens, and the guided setup when
     * the system has made it again without the launcher under it
     * (SetupActivity). The breadcrumbs are what identify a native crash
     * inside the load itself, which no handler can catch.
     */
    fun launcher(context: android.content.Context): Boolean {
        Trail.note(context, "launcher: loading the runtime library")
        if (!load(Title.carried.first())) return false
        Trail.note(context, "launcher: library loaded, initialising")
        return try {
            nativeInit(
                context.filesDir.absolutePath,
                (context.getExternalFilesDir(null) ?: context.filesDir).absolutePath,
                context.cacheDir.absolutePath,
                context.applicationInfo.nativeLibraryDir,
                null,
            )
            // Here too, where the phone's GPU is asked its name: a run
            // that dies before it has a device finds it written down.
            DeviceInfo.log(context)
            true
        } catch (error: Throwable) {
            noteInitFailure(error.message ?: error.toString())
            false
        }
    }

    // ---- start-up ---------------------------------------------------------

    external fun nativeInit(
        filesDir: String,
        externalDir: String,
        cacheDir: String,
        nativeLibDir: String,
        listener: NativeListener?,
    ): Boolean

    /** One of the runtime's MW2_* switches (docs/switches.md). */
    external fun nativeSetOption(key: String, value: String)

    /**
     * The same for a run that is under way: the switch has that value from
     * the next frame on. False for a switch the runtime reads once, at the
     * start of a run, or one this title has not got.
     */
    external fun nativeApplyOption(key: String, value: String): Boolean

    /**
     * A line of the app's own in this process's log (game.log, launcher.log),
     * as an error when [failure]: the device, and why a run was not started.
     */
    external fun nativeLog(failure: Boolean, text: String)

    /**
     * Loads the recompiled game code from the library at [path], in a build
     * that is published without it (GameLibrary.kt). "" when it is ready, or
     * what is wrong with it. Before nativeStart.
     */
    external fun nativeLoadGame(path: String): String

    /** [homeDir] is where the run keeps saves/ (Title.home). */
    external fun nativeStart(gameDir: String, homeDir: String): Boolean
    external fun nativeRequestStop()
    external fun nativeIsRunning(): Boolean
    external fun nativeStatus(): String
    external fun nativeLogTail(): String

    // ---- the surface ------------------------------------------------------

    external fun nativeSurfaceChanged(surface: Any?, width: Int, height: Int, refreshHz: Float)
    external fun nativeSurfaceDestroyed()
    external fun nativeSetPaused(paused: Boolean)
    external fun nativeTrimMemory(level: Int)

    /** Give back what a trim took away, once the run is in front again. */
    external fun nativeRestoreMemory()
    external fun nativeSetRefreshRate(hz: Float)

    // ---- input ------------------------------------------------------------

    external fun nativeTouchState(
        buttons: Int,
        leftTrigger: Int,
        rightTrigger: Int,
        leftX: Float,
        leftY: Float,
        rightX: Float,
        rightY: Float,
    )

    /**
     * A finger's movement on the look area, in pixels, which the runtime
     * turns the view by (runtime/android/input.cpp); [eventNanos] is when
     * the screen reported it, in the system's uptime.
     */
    external fun nativeLookDelta(dx: Float, dy: Float, eventNanos: Long)

    /** The gyroscope's turn of the view, in degrees to the right and downwards. */
    external fun nativeGyroDelta(right: Float, down: Float, eventNanos: Long)
    external fun nativeLookEnd()

    /**
     * Degrees a pixel sideways and up and down, the share of them while
     * aiming, and the gyroscope's setting (Prefs.GYRO_*). Taken at once.
     */
    external fun nativeLookSettings(
        degreesX: Float,
        degreesY: Float,
        aimingFactor: Float,
        gyroMode: Int,
    )

    external fun nativePadState(
        user: Int,
        buttons: Int,
        leftTrigger: Int,
        rightTrigger: Int,
        leftX: Float,
        leftY: Float,
        rightX: Float,
        rightY: Float,
    )

    external fun nativePadConnected(user: Int, connected: Boolean)
    external fun nativePadEnabled(enabled: Boolean)

    /** Players who can be invited, a line each: the player's id in hexadecimal, a tab, the name. */
    external fun nativeInvitable(): String

    /** Invite that one to this player's lobby. False with no lobby to invite to, or the player gone. */
    external fun nativeInvitePlayer(id: Long): Boolean

    /** Other copies heard on the network right now (0 when offline/alone). */
    external fun nativeLanPeers(): Int

    /** Joinable peers, a line each: the peer's id in hexadecimal, a tab, its name. */
    external fun nativeLanJoinable(): String

    /** Join a heard peer now, by its id. False when no such peer is heard. */
    external fun nativeLanJoinPeer(id: Long): Boolean

    // ---- the graphics driver ----------------------------------------------

    /** An unpacked driver folder and the library inside it, or "" for the system's. */
    external fun nativeSelectDriver(directory: String, library: String)
    external fun nativeDriverDescription(): String

    /**
     * The driver self-test, on the driver selected, in the process started
     * for it (DriverSelfTest.kt): "" when the test frame came out right, or
     * what went wrong. Every step is written to the file at [report].
     */
    external fun nativeDriverSelfTest(report: String): String

    /**
     * The runtime's last look at a frame of the run (FrameWatch.kt), into
     * [out]: how many frames have been looked at, 1 for one colour, the
     * samples unlike all their neighbours per thousand, the brightness.
     */
    external fun nativePictureProbe(out: IntArray)

    // ---- installing --------------------------------------------------------

    external fun nativeGameInstalled(gameDir: String): Boolean

    /**
     * Blocking. Returns null when it worked, or a sentence to show when it did
     * not. [source] is a path, or "fd:<number>" for an open document
     * (DiscImage.kt); with [link], only the executables are copied, for a
     * game played from the image.
     */
    external fun nativeInstall(source: String, gameDir: String, link: Boolean): String?

    /**
     * The bytes of the game folder's copies of the image's files, which a
     * game played from the image does not need; deleted with [remove].
     */
    external fun nativeCopiedFiles(source: String, gameDir: String, remove: Boolean): Long

    /** Fills [progress] with { bytes done, bytes total } and returns the file being copied. */
    external fun nativeInstallProgress(progress: LongArray): String

    external fun nativeCancelInstall()

    /** Blocking. Applies the downloaded title update 6 (TU6 builds only).
     * Returns null when it worked, or a sentence to show when it did not. */
    external fun nativeApplyTu6(packagePath: String, gameDir: String): String?

    /** Blocking. Writes the multiplayer's executable as the title update in
     * [packagePath] makes it of [discExecutable], to [target], for the build
     * of its game code here (GameBuild.kt; Advanced Warfare only). Returns
     * null when it worked, or a sentence to show when it did not. */
    external fun nativeMakeUpdatedExecutable(packagePath: String, discExecutable: String, target: String): String?

    // ---- what the overlay shows --------------------------------------------

    /**
     * Fills [out] with { frames per second, texture MB, width, height, frame
     * time ms, presented frames, shaders queued, compiled, the last one's ms,
     * the rate the run is held to, the graphics chip's ms a frame (negative
     * when it does not say), the scene's share of its width in percent,
     * 1 when the rate follows the device, the rate the display is to be
     * asked for a mode of, what the last two seconds of play waited for (0
     * nothing, 1 the graphics chip, 2 the processor, 3 not known) and the
     * seconds since they ended (negative before any), and of the play so
     * far its seconds, the chip's ms a frame and the share of it that came
     * under nine tenths of the rate, and of the compile before the first
     * frame (Compile shaders at boot) its pipelines, how many it has been
     * through and 1 when what it builds is kept for the next run }.
     */
    external fun nativeStats(out: FloatArray)

    /** Why the renderer cannot show the game, or "" (runtime/android/jni.cpp). */
    external fun nativeRendererFailure(): String

    /** The run's side of the performance report: driver, device, what it is held to, how it went. */
    external fun nativePerformance(): String

    /**
     * The run's play so far as a performance report has it (PerfReports.kt):
     * a line each of name=value -- its seconds, frames a second and 1% low,
     * what the frames waited for, the map, the graphics chip and its driver.
     */
    external fun nativeSession(): String

    /** Returns formatted "Device|RendererMode|TextureCompression" */
    external fun nativeRendererInfo(): String

    /** Feeds a console command into IW4 Cbuf_AddText queue (god, give all, map, etc.) */
    external fun nativeRunConsoleCommand(command: String)

    // ---- the button bits the guest expects ----------------------------------
    // XINPUT_GAMEPAD_*, as runtime/kernel/input.cpp reads them.
    const val UP = 0x0001
    const val DOWN = 0x0002
    const val LEFT = 0x0004
    const val RIGHT = 0x0008
    const val START = 0x0010
    const val BACK = 0x0020
    const val L3 = 0x0040
    const val R3 = 0x0080
    const val LB = 0x0100
    const val RB = 0x0200
    const val A = 0x1000
    const val B = 0x2000
    const val X = 0x4000
    const val Y = 0x8000

    /**
     * Not a pad bit: the on-screen push-to-talk button, sent with the pad's
     * in nativeTouchState and taken out again by the runtime (jni.cpp).
     */
    const val TALK = 0x10000
}

/**
 * Implemented by the game activity. Both methods arrive on runtime threads.
 * The names and signatures are looked up from C++ by reflection, so the
 * shrinker is told to leave them alone (proguard-rules.pro).
 */
interface NativeListener {
    /** Motor strengths, 0..65535, only when they change. */
    fun onRumble(user: Int, lowFrequency: Int, highFrequency: Int)

    /** A sentence for the player: what the run is doing, or why it stopped. */
    fun onStatus(text: String)

    /**
     * The title asked for its other executable, by its name on the disc, with
     * the launch data it left as hexadecimal. The process ends when this
     * returns, so whatever starts the next run has to be under way by then.
     */
    fun onNextTitle(executable: String, launchData: String)
}
