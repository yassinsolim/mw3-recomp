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

    /** A flick of the look area, in pixels; the runtime turns it into a stick. */
    external fun nativeLookDelta(dx: Float, dy: Float)
    external fun nativeLookEnd()
    external fun nativeLookSettings(
        sensitivityX: Float,
        sensitivityY: Float,
        smoothing: Float,
        invertY: Boolean,
        saturation: Float,
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

    /** Invite every network player heard to this player's lobby, if any. */
    external fun nativeInviteFriends()

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

    // ---- what the overlay shows --------------------------------------------

    /** Fills [out] with { frames per second, texture MB, width, height, frame time ms, presented frames }. */
    external fun nativeStats(out: FloatArray)

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
