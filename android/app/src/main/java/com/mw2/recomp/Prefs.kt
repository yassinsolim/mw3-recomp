package com.mw2.recomp

import android.content.Context
import android.content.SharedPreferences

/**
 * Everything the player can change, in one place, with the defaults a weak
 * phone wants. Most of these end up as one of the runtime's MW2_* switches
 * (docs/switches.md) the moment a run starts.
 */
class Prefs(context: Context) {

    private val app: Context = context.applicationContext

    /**
     * One file for the launcher's process and the game's (":game"), which
     * both write it: Settings in one, the in-game menu in the other. A
     * process reads a preference file once and from then on writes its own
     * copy over the file whole, so each undid what the other had saved: the
     * on-screen controls hidden in the game were back at the next start,
     * because the launcher, still holding the copy from before the run, wrote
     * its own after it. MODE_MULTI_PROCESS has the file checked each time it
     * is asked for and read again when another process has written it, so it
     * is asked for at every use instead of once. Deprecated in favour of a
     * content provider, which two processes sharing a dozen settings do not
     * need; the check is one stat of the file.
     */
    private val store: SharedPreferences
        @Suppress("DEPRECATION")
        get() = app.getSharedPreferences("mw2", Context.MODE_PRIVATE or Context.MODE_MULTI_PROCESS)

    /**
     * Waits until everything set so far is in the file. The game's process is
     * killed to end a run (GameActivity.quit), and a write still queued then
     * is a choice lost.
     */
    fun flush() {
        store.edit().commit()
    }

    /**
     * The fastest cap this screen can show: the highest of FPS_LIMITS at or
     * under the display's fastest mode, so a 120 Hz panel (AYN Thor) starts
     * at 120 and a 60 Hz one (Retroid Pocket Flip 2) at 60. Both games hold
     * near those since the texel forms (docs/ANDROID-PROGRESS.md), so the
     * old 30 for Modern Warfare 3 on an Adreno 6xx is gone: a fresh install
     * there was left at half the rate it can now draw.
     */
    private val displayLimit: Int by lazy {
        val display = app.getSystemService(android.hardware.display.DisplayManager::class.java)
            ?.getDisplay(android.view.Display.DEFAULT_DISPLAY)
        val fastest = display?.supportedModes?.maxOfOrNull { it.refreshRate } ?: 60f
        // 3% under, as the runtime's pacing matches a mode: 118.4 Hz is 120.
        FPS_LIMITS.filter { it <= fastest * 1.03f }.maxOrNull() ?: 60
    }

    // ---- graphics ---------------------------------------------------------

    /**
     * The fraction of the screen the game actually renders at. The surface is
     * told to be that many pixels (SurfaceHolder.setFixedSize) and the
     * compositor scales it up, which costs nothing: the phone's display
     * pipeline does that anyway. At 0.7 a 1080p phone renders 756p, close to
     * the console's own, and it is the single biggest thing a weak device can
     * be given.
     */
    var resolutionScale: Float
        get() = store.getFloat(KEY_RESOLUTION, 0f).let { if (it <= 0f) 0.7f else it }
        set(value) = custom().putFloat(KEY_RESOLUTION, value.coerceIn(0.35f, 1.0f)).apply()

    /**
     * Internal Vulkan render scale multiplier (MW2_SCALE):
     * 1 = 1x native base dimensions, 2 = 2x multiplier, 3 = 3x multiplier.
     */
    var renderScale: Int
        get() = store.getInt(KEY_RENDER_SCALE, 1).coerceIn(1, 3)
        set(value) = custom().putInt(KEY_RENDER_SCALE, value.coerceIn(1, 3)).apply()

    /**
     * The 3D scene the title draws under its frame, which the HUD and the
     * menus are no part of: GraphicsPresets.SCENE_FOLLOWS, the default, has
     * it drawn smaller while the graphics chip is late (MW2_SCENE_DYNAMIC,
     * renderer_scene.cpp); SCENE_WHOLE never, and at the console's size on a
     * chip that is given a smaller one by its name (MW2_SCENE); SCENE_SMALLER
     * always at GraphicsPresets.SMALLER_PERCENT of its width
     * (MW2_SCENE_SHARE). Read at the start of a run.
     */
    var scene: Int
        get() = store.getInt(KEY_SCENE, GraphicsPresets.SCENE_FOLLOWS)
            .coerceIn(GraphicsPresets.SCENE_FOLLOWS, GraphicsPresets.SCENE_SMALLER)
        set(value) = custom().putInt(KEY_SCENE, value).apply()

    /**
     * How strongly the frame is sharpened on its way to the screen, in
     * percent of the filter's most; 0, the default, not at all (MW2_SHARPEN,
     * runtime/gpu/vulkan/sharpen.h). It costs a third to half a millisecond
     * of GPU a frame, which a chip at its limit shows as frames.
     */
    var sharpening: Int
        get() = store.getInt(KEY_SHARPENING, 0).coerceIn(0, 100)
        set(value) = store.edit().putInt(KEY_SHARPENING, value.coerceIn(0, 100)).apply()

    /**
     * The fraction of the screen the surface is made at: [resolutionScale],
     * or all of it under sharpening, whose filter works at the size the
     * picture is seen at -- sharpened first and then enlarged by the
     * compositor, its edges would be softened again.
     */
    val surfaceScale: Float
        get() = if (sharpening > 0) 1f else resolutionScale

    /** Multisampling. Off by default: it is pure cost on a tiled GPU. */
    var multisampling: Boolean
        get() = store.getBoolean(KEY_MSAA, false)
        set(value) = store.edit().putBoolean(KEY_MSAA, value).apply()

    /**
     * Modern Warfare 2, detail at distance: how many times nearer than on
     * the console a model changes to its coarser meshes, one of
     * DETAIL_BIASES (MW2_LOD_BIAS, runtime/model_detail.cpp). The console's
     * until the player chooses.
     */
    var detailBias: Int
        get() = store.getInt(KEY_DETAIL_BIAS, 1).let { if (it in DETAIL_BIASES) it else 1 }
        set(value) = store.edit().putInt(KEY_DETAIL_BIAS, if (value in DETAIL_BIASES) value else 1).apply()

    /**
     * How much of the phone's memory the texture cache may hold. Zero, the
     * default, means "let the runtime decide from the device"
     * (runtime/android/perf.cpp): a fixed 128 MB is less than one MW3
     * Survival map binds, and what does not fit is drawn black.
     */
    var textureBudgetMB: Int
        get() = store.getInt(KEY_TEXTURE_MB, 0)
        set(value) = store.edit().putInt(KEY_TEXTURE_MB, value).apply()

    var showStats: Boolean
        get() = store.getBoolean(KEY_SHOW_STATS, true)
        set(value) = store.edit().putBoolean(KEY_SHOW_STATS, value).apply()

    /**
     * Stretch the picture to fill the whole screen (no black bars).
     * The surface is made 16:9 and the display scales it up.
     */
    var fillScreen: Boolean
        get() = store.getBoolean(KEY_FILL_SCREEN, false)
        set(value) = store.edit().putBoolean(KEY_FILL_SCREEN, value).apply()

    /**
     * Once per install: a saved cap goes, so the screen's own rate takes
     * over. Before the cap followed the screen, the settings screen and the
     * old 60 FPS lock saved the old defaults (30 for Modern Warfare 3 on an
     * Adreno 6xx, 60 elsewhere) as readily as a player's choice, and those
     * kept a 120 Hz Thor at 60 and a Flip 2 at 30. A saved cap cannot be told
     * from a chosen one, so every one is dropped once; a player who wants a
     * lower cap sets it again, and it is kept from then on.
     */
    private fun forgetOldDefault() {
        if (store.getBoolean(KEY_CAP_FOLLOWS_SCREEN, false)) return
        store.edit().putBoolean(KEY_CAP_FOLLOWS_SCREEN, true).remove(KEY_FPS_LIMIT).remove(KEY_60HZ).apply()
        android.util.Log.i("mw2", "prefs: the saved FPS cap was dropped once; it now starts at the screen's $displayLimit")
    }

    /**
     * The frame-rate limits this screen is offered, slowest first: those of
     * FPS_LIMITS it has a mode for, and the one saved where that is none of
     * them (45, which an older version offered, or a limit kept from
     * another screen): the player's choice is shown as it is and stays.
     */
    val fpsLimits: List<Int>
        get() = (FPS_LIMITS.filter { it <= displayLimit } + listOf(savedLimit).filter { it > 0 }).distinct().sorted()

    /** The limit the player chose, or 0 for none. */
    private val savedLimit: Int
        get() {
            forgetOldDefault()
            return store.getInt(KEY_FPS_LIMIT, 0).let { if (it >= FPS_LIMITS.first()) it else 0 }
        }

    /**
     * Frame-rate cap (MW2_FPS_LIMIT), one of [fpsLimits]. Until the player
     * chooses, the screen's own fastest rate (displayLimit), and the run may
     * go under it by itself ([fpsFollowsDevice]).
     */
    var fpsLimit: Int
        get() = savedLimit.let { if (it > 0) it else displayLimit }
        set(value) {
            custom().putInt(KEY_FPS_LIMIT, value.coerceAtLeast(FPS_LIMITS.first()))
                .putBoolean(KEY_CAP_FOLLOWS_SCREEN, true).apply()
        }

    /**
     * Whether the cap is the runtime's to lower (MW2_FPS_AUTO): the player
     * has chosen none. The run then starts at the screen's fastest rate and
     * is held to a slower one of [fpsSteps] when it has not kept up for ten
     * seconds (runtime/adaptive.h): a phone that draws 62 frames a second
     * under a cap of 120 shows them at uneven steps, and 60 evenly.
     */
    val fpsFollowsDevice: Boolean
        get() = savedLimit == 0

    /** The cap is the runtime's again: the player chose Auto. */
    fun followDeviceFps() {
        custom().remove(KEY_FPS_LIMIT).putBoolean(KEY_CAP_FOLLOWS_SCREEN, true).apply()
    }

    /**
     * The rates a run that follows the device may be held to, fastest first
     * (MW2_FPS_STEPS): the cap it starts at, and under it every rate the
     * screen has a mode for and the half of each, down to 30. Each is one
     * the screen shows evenly, a frame for one of its blanks or for two.
     */
    val fpsSteps: List<Int> by lazy {
        val display = app.getSystemService(android.hardware.display.DisplayManager::class.java)
            ?.getDisplay(android.view.Display.DEFAULT_DISPLAY)
        val modes = display?.supportedModes?.map { Math.round(it.refreshRate) }.orEmpty()
        (modes + modes.map { it / 2 } + displayLimit)
            .filter { it in 30..displayLimit }.distinct().sortedDescending()
    }

    /**
     * The sun's shadows (MW2_SHADOWS): 0 leaves the choice to the runtime,
     * which measures whether the frames have room for them
     * (runtime/adaptive.h), 1 is on and 2 off. The runtime's choice takes
     * them away on an AYN Thor at its 120, and the player must be able to
     * have them back.
     */
    var sunShadows: Int
        get() = store.getInt(KEY_SUN_SHADOWS, 0).coerceIn(0, 2)
        set(value) = custom().putInt(KEY_SUN_SHADOWS, value.coerceIn(0, 2)).apply()

    // ---- the presets --------------------------------------------------------

    /**
     * The preset the graphics settings are (GraphicsPresets): the one the
     * player chose, Custom once a setting of the Advanced page was changed
     * by itself, and in an install older than the presets the one its
     * settings are, which is Automatic where nobody changed them.
     */
    val graphicsPreset: Int
        get() {
            val saved = store.getInt(KEY_PRESET, -1)
            if (saved in GraphicsPresets.AUTOMATIC..GraphicsPresets.CUSTOM) return saved
            return GraphicsPresets.matching(advancedValues())
        }

    /** The preset in English, for the log and the reports. */
    val graphicsPresetName: String
        get() = GraphicsPresets.name(graphicsPreset)

    /**
     * The player chose a preset: every setting of the Advanced page but the
     * driver has its value from then on, in one write, so the page and the
     * game's menu show what the preset is. What the player had set by hand
     * is kept when Custom is left and put back when Custom is chosen again:
     * trying a preset costs nobody their own settings.
     */
    fun choosePreset(preset: Int) {
        val was = graphicsPreset
        if (was == preset && preset == GraphicsPresets.CUSTOM) return
        val edit = store.edit().putInt(KEY_PRESET, preset)
        if (was == GraphicsPresets.CUSTOM) {
            val own = advancedValues()
            edit.putString(
                KEY_CUSTOM,
                listOf(own.resolution, own.fpsLimit, own.sunShadows, own.renderScale, own.scene).joinToString(","),
            )
        }
        (if (preset == GraphicsPresets.CUSTOM) keptCustom() else GraphicsPresets.values(preset))?.let { put(edit, it) }
        edit.apply()
    }

    /** The Advanced page's settings as they are now. */
    private fun advancedValues() = GraphicsPresets.Values(
        Math.round(resolutionScale * 100f), if (fpsFollowsDevice) 0 else fpsLimit, sunShadows, renderScale, scene,
    )

    /** What the player had set by hand when Custom was last left; null when it never was. */
    private fun keptCustom(): GraphicsPresets.Values? {
        val kept = store.getString(KEY_CUSTOM, "").orEmpty().split(',').mapNotNull { it.toIntOrNull() }
        return if (kept.size == 5) GraphicsPresets.Values(kept[0], kept[1], kept[2], kept[3], kept[4]) else null
    }

    private fun put(edit: SharedPreferences.Editor, values: GraphicsPresets.Values) {
        edit.putFloat(KEY_RESOLUTION, values.resolution / 100f)
            .putInt(KEY_SUN_SHADOWS, values.sunShadows)
            .putInt(KEY_RENDER_SCALE, values.renderScale)
            .putInt(KEY_SCENE, values.scene)
            .putBoolean(KEY_CAP_FOLLOWS_SCREEN, true)
        if (values.fpsLimit == 0) edit.remove(KEY_FPS_LIMIT) else edit.putInt(KEY_FPS_LIMIT, values.fpsLimit)
    }

    /** An edit that also makes the preset Custom: a setting of the Advanced page is being changed by itself. */
    private fun custom(): SharedPreferences.Editor = store.edit().putInt(KEY_PRESET, GraphicsPresets.CUSTOM)

    /**
     * Best looking, as a title starts: the render multiplier is 2x where a
     * minute and more of this title's play at 1x has shown the graphics chip
     * has the time for it (GraphicsPresets.twiceHolds), and 1x until then
     * and once a run at 2x did not hold. The campaign and the multiplayer
     * are measured apart, by the title's name. Written before it returns:
     * the run reads it next.
     */
    fun settleBestLooking(title: String) {
        if (graphicsPreset != GraphicsPresets.BEST) return
        val twice = !store.getBoolean(KEY_TWICE_FAILED + title, false) &&
            GraphicsPresets.twiceHolds(store.getFloat(KEY_CHIP_MS + title, -1f), displayLimit)
        store.edit().putInt(KEY_RENDER_SCALE, if (twice) 2 else 1).commit()
    }

    /** The graphics chip's time a frame in this title's play under Best looking at 1x, in milliseconds. */
    fun noteChipTime(title: String, milliseconds: Float) {
        store.edit().putFloat(KEY_CHIP_MS + title, milliseconds).apply()
    }

    /** A run of this title at 2x under Best looking did not hold: it is 1x from the next start. */
    fun noteTwiceFailed(title: String) {
        store.edit().putBoolean(KEY_TWICE_FAILED + title, true).apply()
    }

    /**
     * The Advanced page's Reset to automatic: its settings as a fresh
     * install has them, the driver the app would choose by itself included,
     * and what Best looking has measured forgotten.
     */
    fun resetGraphics() {
        val edit = store.edit().putInt(KEY_PRESET, GraphicsPresets.AUTOMATIC).remove(KEY_CUSTOM)
        GraphicsPresets.values(GraphicsPresets.AUTOMATIC)?.let { put(edit, it) }
        for (title in Title.entries) edit.remove(KEY_CHIP_MS + title.name).remove(KEY_TWICE_FAILED + title.name)
        edit.putBoolean(KEY_DRIVER_CHOICE_MADE, false).putString(KEY_DRIVER_DIR, "").putString(KEY_DRIVER_LIB, "")
        // And the app chooses again, with what failed before forgotten (DriverAuto.run).
        edit.remove(KEY_DRIVER_AUTO_KEY).remove(KEY_DRIVER_PICKED).remove(KEY_DRIVER_VERDICTS)
        edit.commit()
        // The driver the build carries is the default on the chips it is
        // for, and the phone's own elsewhere.
        DriverStore(app).installBundledDefault(this)
    }

    /**
     * Field of View (cg_fov). Modern phones are 20:9 ultrawide,
     * so 85 FOV gives a natural, wide cinematic view without gun distortion,
     * compared to the 65 FOV console original.
     */
    var fov: Int
        get() = store.getInt(KEY_FOV, 85).coerceIn(60, 110)
        set(value) = store.edit().putInt(KEY_FOV, value.coerceIn(60, 110)).apply()

    /**
     * The settings a performance report names, a line each, in English as
     * the log is: what the player chose, or that the choice was left to the
     * app (GameActivity.sharePerformance).
     */
    fun graphicsSummary(): List<String> = listOf(
        "preset $graphicsPresetName",
        "resolution ${(resolutionScale * 100).toInt()}%, internal multiplier ${renderScale}x, 3D scene " +
            arrayOf("follows the load", "whole (chosen)", "at ${GraphicsPresets.SMALLER_PERCENT}% (chosen)")[scene] +
            ", sharpening $sharpening%" +
            ", multisampling ${if (multisampling) "on" else "off"}, fill ${if (fillScreen) "on" else "off"}",
        "FPS limit " + (if (fpsFollowsDevice) "auto (starts at $fpsLimit, may go to one of ${fpsSteps.joinToString("/")})"
        else "$fpsLimit (chosen)") +
            ", sun shadows " + arrayOf("auto (measured)", "on (chosen)", "off (chosen)")[sunShadows] +
            ", field of view $fov",
        "texture memory " + (if (textureBudgetMB > 0) "$textureBudgetMB MB (chosen)" else "auto") +
            ", shaders " + (if (prewarmAllShaders) "all compiled at the start" else "compiled as play goes") +
            (if (asyncPipelines) ", in the background" else ", at the draw") +
            (if (Title.hasDetailBias) ", detail at distance $detailBias" else ""),
        "driver " + (if (driverDirectory.isEmpty()) "the system's" else java.io.File(driverDirectory).name) +
            (if (driverChoiceMade) " (chosen)" else " (the app's choice" + driverPicked.let { if (it.isEmpty()) ")" else ": $it)" }),
    ) + driverResults()

    /**
     * What this phone found out about its drivers, for a shared report and
     * the log: the self-test's last answers and what runs said. Reports of
     * these are what the catalog's known-good and known-broken lists are
     * made from (docs/android.md, "The driver catalog").
     */
    fun driverResults(): List<String> =
        driverChecks.lineSequence().filter { it.isNotEmpty() }.map { "driver self-test $it" }.toList() +
            driverVerdicts.filter { !it.key.startsWith("strikes:") }.map { "driver result ${it.key}: ${it.value}" }

    // ---- sound ------------------------------------------------------------

    var audioEnabled: Boolean
        get() = store.getBoolean(KEY_AUDIO, true)
        set(value) = store.edit().putBoolean(KEY_AUDIO, value).apply()

    // ---- controls ---------------------------------------------------------

    /** The on-screen pad. Off is for playing with a physical controller. */
    var touchControls: Boolean
        get() = store.getBoolean(KEY_TOUCH, true)
        set(value) = store.edit().putBoolean(KEY_TOUCH, value).apply()

    /**
     * Whether a physical controller is listened to at all. Off is not a
     * nicety: a phone that reports an accessory, or its own sensors, as a
     * gamepad pushes a stick nobody is touching, and the character walks into
     * a wall for the whole match with nothing on screen to explain it.
     */
    var gamepadEnabled: Boolean
        get() = store.getBoolean(KEY_GAMEPAD, true)
        set(value) = store.edit().putBoolean(KEY_GAMEPAD, value).apply()

    var vibration: Boolean
        get() = store.getBoolean(KEY_VIBRATION, true)
        set(value) = store.edit().putBoolean(KEY_VIBRATION, value).apply()

    /**
     * How far a finger on the look area turns the view: degrees for a
     * centimetre of glass, sideways (TouchLook.kt). The old settings (a
     * multiple of a stick's speed, sideways and up and down, and a
     * smoothing) meant something else and are not read.
     */
    var touchLookSensitivity: Float
        get() = store.getFloat(KEY_TOUCH_LOOK, TouchLook.DEFAULT_DEGREES_PER_CM)
        set(value) = store.edit().putFloat(
            KEY_TOUCH_LOOK, value.coerceIn(TouchLook.MIN_DEGREES_PER_CM, TouchLook.MAX_DEGREES_PER_CM),
        ).apply()

    /** Up and down, as a share of sideways: 1 turns the view the same both ways. */
    var touchLookVertical: Float
        get() = store.getFloat(KEY_TOUCH_LOOK_VERTICAL, 1.0f)
        set(value) = store.edit().putFloat(KEY_TOUCH_LOOK_VERTICAL, value.coerceIn(0.25f, 2.0f)).apply()

    /**
     * The share of the sensitivity a finger has while the Aim button (the
     * left trigger) is held. The sight's own zoom slows the turn besides,
     * as it does the stick's.
     */
    var touchLookAiming: Float
        get() = store.getFloat(KEY_TOUCH_LOOK_AIMING, 1.0f)
        set(value) = store.edit().putFloat(KEY_TOUCH_LOOK_AIMING, value.coerceIn(0.25f, 2.0f)).apply()

    /**
     * Looking left and right turned round, for a finger on the look area and
     * for the gyroscope (LookInvert). Up and down is the next one; a
     * controller's stick is [invertPadHorizontal] and the title's own option.
     */
    var invertLookHorizontal: Boolean
        get() = store.getBoolean(LookInvert.KEY_HORIZONTAL, false)
        set(value) = store.edit().putBoolean(LookInvert.KEY_HORIZONTAL, value).apply()

    /** Up and down, the same two. It starts from what the one switch of older versions was set to. */
    var invertLookVertical: Boolean
        get() = LookInvert.verticalIn(store)
        set(value) = store.edit().putBoolean(LookInvert.KEY_VERTICAL, value).apply()

    val lookInvert: LookInvert
        get() = store.let { LookInvert(it.getBoolean(LookInvert.KEY_HORIZONTAL, false), LookInvert.verticalIn(it)) }

    /**
     * A physical controller's right stick, left and right turned round. The
     * titles have an Invert option of their own for up and down and none for
     * left and right, so only this one is here.
     */
    var invertPadHorizontal: Boolean
        get() = store.getBoolean(LookInvert.KEY_PAD_HORIZONTAL, false)
        set(value) = store.edit().putBoolean(LookInvert.KEY_PAD_HORIZONTAL, value).apply()

    /** A tap on the look area fires a shot. */
    var tapToFire: Boolean
        get() = store.getBoolean(KEY_TAP_TO_FIRE, false)
        set(value) = store.edit().putBoolean(KEY_TAP_TO_FIRE, value).apply()

    /** The on-screen Fire button also aims: the left trigger at once, the right one when the sights are up (AimWhenFiring). */
    var aimWhenFiring: Boolean
        get() = store.getBoolean(KEY_AIM_WHEN_FIRING, false)
        set(value) = store.edit().putBoolean(KEY_AIM_WHEN_FIRING, value).apply()

    /** Milliseconds from Fire down to the right trigger when [aimWhenFiring] holds the left one first. */
    var aimWhenFiringDelay: Int
        get() = store.getInt(KEY_AIM_WHEN_FIRING_DELAY, AimWhenFiring.DEFAULT_DELAY_MILLIS.toInt())
            .coerceIn(0, AimWhenFiring.MAX_DELAY_MILLIS.toInt())
        set(value) = store.edit().putInt(KEY_AIM_WHEN_FIRING_DELAY, value.coerceIn(0, AimWhenFiring.MAX_DELAY_MILLIS.toInt())).apply()

    /** The movement stick appears where the left thumb first touches the left side of the screen (MoveStick). */
    var floatingStick: Boolean
        get() = store.getBoolean(KEY_FLOATING_STICK, false)
        set(value) = store.edit().putBoolean(KEY_FLOATING_STICK, value).apply()

    /** The movement stick pushed to its edge for a moment presses the game's sprint, the left stick's click (AutoSprint). */
    var autoSprint: Boolean
        get() = store.getBoolean(KEY_AUTO_SPRINT, false)
        set(value) = store.edit().putBoolean(KEY_AUTO_SPRINT, value).apply()

    /** The gyroscope turning the view: [GYRO_OFF], [GYRO_ALWAYS] or [GYRO_AIMING]. */
    var gyroMode: Int
        get() = store.getInt(KEY_GYRO_MODE, GYRO_OFF).coerceIn(GYRO_OFF, GYRO_AIMING)
        set(value) = store.edit().putInt(KEY_GYRO_MODE, value.coerceIn(GYRO_OFF, GYRO_AIMING)).apply()

    /** Degrees the view turns for a degree the phone is turned. */
    var gyroSensitivity: Float
        get() = store.getFloat(KEY_GYRO_SENSITIVITY, 1.0f)
        set(value) = store.edit().putFloat(KEY_GYRO_SENSITIVITY, value.coerceIn(0.25f, 4.0f)).apply()

    // ---- the graphics driver ------------------------------------------------

    /** The folder of the driver the player picked, or "" for the system's. */
    var driverDirectory: String
        get() = store.getString(KEY_DRIVER_DIR, "") ?: ""
        set(value) = store.edit().putString(KEY_DRIVER_DIR, value).apply()

    var driverLibrary: String
        get() = store.getString(KEY_DRIVER_LIB, "") ?: ""
        set(value) = store.edit().putString(KEY_DRIVER_LIB, value).apply()

    /**
     * Set once the player has picked a driver themselves, the system's
     * included. Until then the build's bundled driver (DriverStore) may be
     * chosen for them; afterwards nothing but the player changes it.
     */
    var driverChoiceMade: Boolean
        get() = store.getBoolean(KEY_DRIVER_CHOICE_MADE, false)
        set(value) = store.edit().putBoolean(KEY_DRIVER_CHOICE_MADE, value).apply()

    /**
     * SHA-256 of the bundled driver package that was last unpacked, so a
     * newer one in an app update is noticed. Empty until the default has
     * been installed, which is also how an install that predates it, with a
     * driver the player picked, is told apart from one holding our default.
     */
    var bundledDriverSha: String
        get() = store.getString(KEY_BUNDLED_DRIVER_SHA, "") ?: ""
        set(value) = store.edit().putString(KEY_BUNDLED_DRIVER_SHA, value).apply()

    /** Where the bundled driver was unpacked, whether or not it is selected. */
    var bundledDriverDirectory: String
        get() = store.getString(KEY_BUNDLED_DRIVER_DIR, "") ?: ""
        set(value) = store.edit().putString(KEY_BUNDLED_DRIVER_DIR, value).apply()

    // ---- the driver the app chooses by itself (DriverAuto.kt) ---------------

    /**
     * What the last choice was made for: the app's build, the catalog's
     * version, the system's build and the chip. A choice is made again when
     * any of them changes, and not at every start.
     */
    var driverAutoKey: String
        get() = store.getString(KEY_DRIVER_AUTO_KEY, "") ?: ""
        set(value) = store.edit().putString(KEY_DRIVER_AUTO_KEY, value).apply()

    /** The catalog's name for the driver the app selected ("system" for the phone's own), or "" before it has. */
    var driverPicked: String
        get() = store.getString(KEY_DRIVER_PICKED, "") ?: ""
        set(value) = store.edit().putString(KEY_DRIVER_PICKED, value).apply()

    /**
     * What this phone has found out about each driver, by the catalog's name
     * for it or "dir:<folder>" for one the player imported: "ok" (the
     * self-test passed), "good" (a run showed a picture), or "fail: <why>".
     * One that failed is not chosen again until the app, the catalog or the
     * system changes (DriverAuto.key).
     */
    var driverVerdicts: Map<String, String>
        get() = (store.getString(KEY_DRIVER_VERDICTS, "") ?: "").lineSequence().mapNotNull { line ->
            val at = line.indexOf('\t')
            if (at <= 0) null else line.substring(0, at) to line.substring(at + 1)
        }.toMap()
        set(value) = store.edit().putString(
            KEY_DRIVER_VERDICTS,
            value.entries.joinToString("\n") { "${it.key}\t${it.value.replace('\n', ' ').replace('\t', ' ')}" },
        ).commit().let { }

    fun noteDriverVerdict(id: String, verdict: String) {
        driverVerdicts = driverVerdicts + (id to verdict)
    }

    /** What the launcher has to tell the player about the driver, once: a switch the app made, a driver that failed. */
    var driverNotice: String
        get() = store.getString(KEY_DRIVER_NOTICE, "") ?: ""
        set(value) = store.edit().putString(KEY_DRIVER_NOTICE, value).commit().let { }

    /**
     * The driver the last run started on, by the name its results are kept
     * under (GameActivity): a run that crashed inside its driver counts
     * against that one (DriverAuto.afterRun).
     */
    var driverTrial: String
        get() = store.getString(KEY_DRIVER_TRIAL, "") ?: ""
        set(value) = store.edit().putString(KEY_DRIVER_TRIAL, value).commit().let { }

    /** When the newest crash DriverAuto.afterRun has looked at happened. */
    var driverCrashSeen: Long
        get() = store.getLong(KEY_DRIVER_CRASH_SEEN, 0)
        set(value) = store.edit().putLong(KEY_DRIVER_CRASH_SEEN, value).commit().let { }

    /** When the catalog was last asked for online. */
    var driverCatalogAt: Long
        get() = store.getLong(KEY_DRIVER_CATALOG_AT, 0)
        set(value) = store.edit().putLong(KEY_DRIVER_CATALOG_AT, value).apply()

    /** The self-test's last answers, a line a driver, in English as the log is: for a shared performance report. */
    var driverChecks: String
        get() = store.getString(KEY_DRIVER_CHECKS, "") ?: ""
        set(value) = store.edit().putString(KEY_DRIVER_CHECKS, value).apply()

    // ---- where the game lives ------------------------------------------------

    /**
     * Set once the files are in place; empty means "not installed yet".
     * Written before the setter returns: the installer starts the game right
     * after, in a process of its own that reads the file, and one written
     * later was read without the folder ("The game files are not installed",
     * seen on the Retroid Pocket Flip 2 after Install & Play).
     */
    var gameDirectory: String
        get() = store.getString(KEY_GAME_DIR, "") ?: ""
        set(value) {
            store.edit().putString(KEY_GAME_DIR, value).commit()
        }

    /**
     * The disc image the game is played from (DiscImage.kt): its path, or the
     * document's address for one chosen with the system's picker. Empty when
     * the game's files were copied into the game folder. Written before the
     * setter returns, as the game folder is, and before it.
     */
    var discImage: String
        get() = store.getString(KEY_DISC_IMAGE, "") ?: ""
        set(value) {
            store.edit().putString(KEY_DISC_IMAGE, value).commit()
        }

    /**
     * Advanced Warfare's second disc, the install disc, when its two content
     * packages are read out of its image where it lies (kernel/content.cpp),
     * named as [discImage] names one. Empty when they were copied beside the
     * game folder, and in every other game.
     */
    var installDiscImage: String
        get() = store.getString(KEY_INSTALL_DISC_IMAGE, "") ?: ""
        set(value) {
            store.edit().putString(KEY_INSTALL_DISC_IMAGE, value).commit()
        }

    /**
     * Whether the player chose, on the install screen, to copy the game
     * files into the app rather than play from the disc image: the choice
     * is theirs and is kept, also across a source that allows only one of
     * the two (a folder of extracted files can only be copied).
     */
    var installByCopy: Boolean
        get() = store.getBoolean(KEY_INSTALL_BY_COPY, false)
        set(value) = store.edit().putBoolean(KEY_INSTALL_BY_COPY, value).apply()

    /**
     * The disc image the player chose on the build screen, as [discImage]
     * names one: Install & Play offers it, so an image is chosen once.
     */
    var chosenImage: String
        get() = store.getString(KEY_CHOSEN_IMAGE, "") ?: ""
        set(value) = store.edit().putString(KEY_CHOSEN_IMAGE, value).apply()

    /**
     * How far the app may use the battery to bring the game code up to date
     * by itself after an update of the app (GameUpdate.kt): the charge in a
     * hundred below which it waits for the charger. [UPDATE_CODE_CHARGING]
     * is the charger only, 0 any charge, [UPDATE_CODE_NEVER] not by itself
     * at all.
     */
    var updateCodeBattery: Int
        get() = store.getInt(KEY_UPDATE_CODE_BATTERY, 30)
        set(value) = store.edit().putInt(KEY_UPDATE_CODE_BATTERY, value).apply()

    /** The folder the file browser last showed, so it opens where it left off. */
    var lastBrowsed: String
        get() = store.getString(KEY_LAST_BROWSED, "") ?: ""
        set(value) = store.edit().putString(KEY_LAST_BROWSED, value).apply()

    /** The update this phone was told about and skipped, by tag. */
    var seenUpdateTag: String
        get() = store.getString(KEY_UPDATE_TAG, "") ?: ""
        set(value) = store.edit().putString(KEY_UPDATE_TAG, value).apply()

    /** Shown once, the first time the player opens the app. */
    var seenWelcome: Boolean
        get() = store.getBoolean(KEY_WELCOME, false)
        set(value) = store.edit().putBoolean(KEY_WELCOME, value).apply()

    var autoLaunch: Boolean
        get() = store.getBoolean(KEY_AUTO_LAUNCH, true)
        set(value) = store.edit().putBoolean(KEY_AUTO_LAUNCH, value).apply()

    /**
     * The split-screen heat warning, shown once when starting the game with a
     * physical controller attached (player 2). Extra viewports cost frames
     * and the phone gets hot; nobody needs telling twice.
     */
    var seenPadWarning: Boolean
        get() = store.getBoolean(KEY_PAD_WARNING, false)
        set(value) = store.edit().putBoolean(KEY_PAD_WARNING, value).apply()

    /**
     * The player asked not to be shown the help over the game again
     * (GameHelp.kt): how to open the menu, change settings and quit. Until
     * then it is shown at the start of each run: a player who closed it in
     * a hurry still finds the menu the next time.
     */
    var seenGameHelp: Boolean
        get() = store.getBoolean(KEY_GAME_HELP, false)
        set(value) = store.edit().putBoolean(KEY_GAME_HELP, value).apply()

    /**
     * The guided setup has been through to its end on this device, or the
     * launcher found the game set up already (an update over an older
     * version): its optional DLC step is for a first setup only, and is not
     * put again when a later update has the game built anew (Setup.kt).
     */
    var setupFinished: Boolean
        get() = store.getBoolean(KEY_SETUP_FINISHED, false)
        set(value) = store.edit().putBoolean(KEY_SETUP_FINISHED, value).apply()

    /**
     * Compile every recorded pipeline synchronously at boot instead of the
     * small instant-boot budget (MW2_SHADER_PREWARM_ALL). Boot takes minutes
     * on a cold cache but there is no warmup debt left afterwards.
     */
    var prewarmAllShaders: Boolean
        get() = store.getBoolean(KEY_PREWARM_ALL, false)
        set(value) = store.edit().putBoolean(KEY_PREWARM_ALL, value).apply()

    /**
     * Compile a pipeline the game has not drawn with before on a background
     * thread, skipping its draws until it is ready (MW2_ASYNC_PIPELINES).
     * On by default: on the AYN Thor a first play of Survival lost 146 frames
     * to compiling at the draw, 25 with this, and the map was whole within
     * the intro and the first seconds of play. Off compiles at the draw.
     */
    var asyncPipelines: Boolean
        get() = store.getBoolean(KEY_ASYNC_PIPELINES, true)
        set(value) = store.edit().putBoolean(KEY_ASYNC_PIPELINES, value).apply()

    /**
     * Modern Warfare 3's and Advanced Warfare's multiplayer: every item reads
     * as unlocked (MW2_UNLOCK_ALL, runtime/mw3_unlock_all.cpp and
     * aw_unlock_all.cpp). The stats are not changed.
     */
    var unlockAllItems: Boolean
        get() = store.getBoolean(KEY_UNLOCK_ALL_ITEMS, true)
        set(value) = store.edit().putBoolean(KEY_UNLOCK_ALL_ITEMS, value).apply()

    /**
     * The multiplayer: how many bots join the matches this
     * device hosts (MW2_BOTS, runtime/bots.cpp), 0 for none; how well they
     * shoot, 0 Recruit to 3 Veteran (MW2_BOT_SKILL); and whether they all
     * take the side the players are not on (MW2_BOT_TEAMS).
     */
    var botCount: Int
        get() = store.getInt(KEY_BOT_COUNT, 0).coerceIn(0, MAX_BOTS)
        set(value) = store.edit().putInt(KEY_BOT_COUNT, value.coerceIn(0, MAX_BOTS)).apply()

    var botSkill: Int
        get() = store.getInt(KEY_BOT_SKILL, 1).coerceIn(0, 3)
        set(value) = store.edit().putInt(KEY_BOT_SKILL, value.coerceIn(0, 3)).apply()

    var botsAgainstPlayers: Boolean
        get() = store.getBoolean(KEY_BOTS_AGAINST_PLAYERS, false)
        set(value) = store.edit().putBoolean(KEY_BOTS_AGAINST_PLAYERS, value).apply()

    /**
     * Modern Warfare 3's multiplayer: what the matches this device hosts are
     * played with (MW2_MATCH_*, runtime/match_options.cpp) -- gravity (800 is
     * the game's), movement speed in percent, jump height (39 is the game's),
     * whether sprinting never runs out, and whether a fall hurts.
     */
    var matchGravity: Int
        get() = store.getInt(KEY_MATCH_GRAVITY, 800).let { if (it in MATCH_GRAVITIES) it else 800 }
        set(value) = store.edit().putInt(KEY_MATCH_GRAVITY, value).apply()

    var matchSpeed: Int
        get() = store.getInt(KEY_MATCH_SPEED, 100).let { if (it in MATCH_SPEEDS) it else 100 }
        set(value) = store.edit().putInt(KEY_MATCH_SPEED, value).apply()

    var matchJump: Int
        get() = store.getInt(KEY_MATCH_JUMP, 39).let { if (it in MATCH_JUMPS) it else 39 }
        set(value) = store.edit().putInt(KEY_MATCH_JUMP, value).apply()

    var matchUnlimitedSprint: Boolean
        get() = store.getBoolean(KEY_MATCH_SPRINT, false)
        set(value) = store.edit().putBoolean(KEY_MATCH_SPRINT, value).apply()

    var matchFallDamage: Boolean
        get() = store.getBoolean(KEY_MATCH_FALL_DAMAGE, true)
        set(value) = store.edit().putBoolean(KEY_MATCH_FALL_DAMAGE, value).apply()

    /**
     * Modern Warfare 3's multiplayer: the matches this device hosts end in a
     * vote for the next map, which then loads with everyone in it
     * (MW2_MAP_VOTE, runtime/map_vote.cpp).
     */
    var mapVote: Boolean
        get() = store.getBoolean(KEY_MAP_VOTE, false)
        set(value) = store.edit().putBoolean(KEY_MAP_VOTE, value).apply()

    /**
     * Keep the driver's pipeline blob on disk between runs under any Mali
     * driver (MW2_PERSIST_PIPELINE_CACHE=1). Off leaves it to the runtime,
     * which keeps it from driver r38 on and not under older ones
     * (runtime/gpu/vulkan/pipeline_cache_rules.h).
     */
    var persistPipelineCache: Boolean
        get() = store.getBoolean(KEY_PERSIST_PIPELINE, false)
        set(value) = store.edit().putBoolean(KEY_PERSIST_PIPELINE, value).apply()

    /**
     * The name other network players see (MW2_NAME). Until the player types
     * one it is the device's own (defaultPlayerName): the profile's name is
     * "Player 1" on every fresh install, and a lobby of them told nobody who
     * was who.
     */
    var playerName: String
        get() = store.getString(KEY_PLAYER_NAME, "").orEmpty().trim().ifEmpty { defaultPlayerName }
        set(value) = store.edit().putString(KEY_PLAYER_NAME, value.trim().take(MAX_PLAYER_NAME)).apply()

    /**
     * The device's model and four characters of the number Android keeps for
     * this app on this device (it outlives an update, and differs between two
     * devices of one model): "AYN Thor 3F2A". The model gives up its leading
     * words until it fits the fifteen characters a name has, so a "Retroid
     * Pocket Flip2" is "Flip2 …". A second copy on one device (-PmpInstance)
     * is another player, with other characters.
     */
    val defaultPlayerName: String by lazy {
        val number = android.provider.Settings.Secure.getString(
            app.contentResolver, android.provider.Settings.Secure.ANDROID_ID,
        ).orEmpty()
        val tail = "%04X".format("$number/${BuildConfig.MP_INSTANCE}".hashCode() and 0xFFFF)
        val room = MAX_PLAYER_NAME - tail.length - 1
        var model = android.os.Build.MODEL.orEmpty().trim()
        while (model.length > room && ' ' in model) model = model.substringAfter(' ').trim()
        "${model.take(room).trim()} $tail".trim()
    }

    /**
     * Whether the player has been asked for that name, which the first start
     * of the multiplayer does (GameActivity.start). One who typed a name in
     * Settings before there was a question has answered it.
     */
    var playerNameAsked: Boolean
        get() = store.getBoolean(KEY_PLAYER_NAME_ASKED, false) ||
            store.getString(KEY_PLAYER_NAME, "").orEmpty().isNotBlank()
        set(value) = store.edit().putBoolean(KEY_PLAYER_NAME_ASKED, value).apply()

    /** Accept LAN lobby invitations automatically (MW2_LAN_ACCEPT). */
    var lanAccept: Boolean
        get() = store.getBoolean(KEY_LAN_ACCEPT, false)
        set(value) = store.edit().putBoolean(KEY_LAN_ACCEPT, value).apply()

    /**
     * Announced network adapter (MW2_LAN_IFACE): name or address piece.
     * Empty means the first connection up. For VPN + WiFi at once.
     */
    var lanIface: String
        get() = store.getString(KEY_LAN_IFACE, "") ?: ""
        set(value) = store.edit().putString(KEY_LAN_IFACE, value).apply()

    /**
     * How the multiplayer reaches other players: NETWORK_ONLINE, through a
     * relay server; NETWORK_LAN, the local network only; NETWORK_OFF, no
     * network at all.
     */
    var networkMode: String
        get() = (store.getString(KEY_NETWORK_MODE, NETWORK_ONLINE) ?: NETWORK_ONLINE)
            .let { if (it in NETWORK_MODES) it else NETWORK_ONLINE }
        set(value) = store.edit().putString(KEY_NETWORK_MODE, value).apply()

    /** The relay server, host or host:port (MW2_SERVER). */
    var relayAddress: String
        get() = store.getString(KEY_RELAY_ADDRESS, DEFAULT_RELAY_ADDRESS) ?: DEFAULT_RELAY_ADDRESS
        set(value) = store.edit().putString(KEY_RELAY_ADDRESS, value.trim()).apply()

    /**
     * Modern Warfare 3's multiplayer, experimental: signed in to Xbox LIVE
     * (MW2_LIVE). Its Xbox LIVE menu also wants the publisher's servers, so
     * this is for testing; players meet in System Link without it.
     */
    var liveSignIn: Boolean
        get() = store.getBoolean(KEY_LIVE_SIGN_IN, false)
        set(value) = store.edit().putBoolean(KEY_LIVE_SIGN_IN, value).apply()

    /**
     * Modern Warfare 3's multiplayer: voice chat (MW2_VOICE). Off until the
     * player turns it on, which is also when the app asks for the
     * microphone: speech goes to the other players through the relay.
     */
    var voiceChat: Boolean
        get() = store.getBoolean(KEY_VOICE_CHAT, false)
        set(value) = store.edit().putBoolean(KEY_VOICE_CHAT, value).apply()

    /**
     * Send only while the Talk button is held (MW2_VOICE_PTT). Off is an
     * open microphone, as the console had: for playing on a controller with
     * the touch controls hidden.
     */
    var voicePushToTalk: Boolean
        get() = store.getBoolean(KEY_VOICE_PTT, true)
        set(value) = store.edit().putBoolean(KEY_VOICE_PTT, value).apply()

    /**
     * What the runtime is given as MW2_NETWORK: the relay when Online is
     * chosen and there is an address for one, else the local network, which
     * needs nothing set up.
     */
    val networkBackend: String
        get() = when (networkMode) {
            NETWORK_OFF -> "none"
            NETWORK_LAN -> "lan"
            else -> if (relayAddress.isBlank()) "lan" else "server"
        }

    companion object {
        /**
         * The public relay every copy starts with, host or host:port (3074
         * without one): the one place it is written. The owner's homelab
         * runs it (docs/relay.md); a player may point Online at another.
         */
        const val DEFAULT_RELAY_ADDRESS = "mw3.yassin.app:3074"
        const val NETWORK_ONLINE = "online"
        const val NETWORK_LAN = "lan"
        const val NETWORK_OFF = "off"
        val NETWORK_MODES = listOf(NETWORK_ONLINE, NETWORK_LAN, NETWORK_OFF)
        /** A gamertag's longest, which is what the title shows of a name. */
        const val MAX_PLAYER_NAME = 15
        private const val KEY_NETWORK_MODE = "networkMode"
        private const val KEY_RELAY_ADDRESS = "relayAddress"
        private const val KEY_LIVE_SIGN_IN = "liveSignIn"
        private const val KEY_FPS_LIMIT = "fpsLimit"
        private const val KEY_SUN_SHADOWS = "sunShadows"
        /** The frame-rate limits a player is offered, where the screen has a mode that fast (fpsLimits). */
        val FPS_LIMITS = listOf(30, 60, 90, 120)
        private const val KEY_RESOLUTION = "resolutionScale"
        private const val KEY_RENDER_SCALE = "renderScale"
        private const val KEY_SCENE = "scene"
        private const val KEY_PRESET = "graphicsPreset"
        /** The Advanced page's five settings as the player last set them by hand (choosePreset). */
        private const val KEY_CUSTOM = "graphicsCustom"
        /** With a title's name after it: what Best looking has measured of that title (settleBestLooking). */
        private const val KEY_CHIP_MS = "bestLookingChipMs."
        private const val KEY_TWICE_FAILED = "bestLookingTwiceFailed."
        private const val KEY_SHARPENING = "sharpening"
        private const val KEY_MSAA = "multisampling"
        private const val KEY_DETAIL_BIAS = "detailBias"
        /** As on the console, reduced, low: what the slider stops at. */
        val DETAIL_BIASES = intArrayOf(1, 2, 4)
        private const val KEY_TEXTURE_MB = "textureBudgetMB"
        private const val KEY_SHOW_STATS = "showStats"
        private const val KEY_FILL_SCREEN = "fillScreen"
        private const val KEY_60HZ = "prefer60Hz"
        private const val KEY_CAP_FOLLOWS_SCREEN = "fpsCapFollowsScreen"
        private const val KEY_FOV = "fov"
        private const val KEY_AUDIO = "audio"
        private const val KEY_TOUCH = "touchControls"
        private const val KEY_GAMEPAD = "gamepad"
        private const val KEY_VIBRATION = "vibration"
        private const val KEY_TOUCH_LOOK = "touchLookDegreesPerCm"
        private const val KEY_TOUCH_LOOK_VERTICAL = "touchLookVertical"
        private const val KEY_TOUCH_LOOK_AIMING = "touchLookAiming"
        private const val KEY_TAP_TO_FIRE = "tapToFire"
        private const val KEY_AIM_WHEN_FIRING = "aimWhenFiring"
        private const val KEY_AIM_WHEN_FIRING_DELAY = "aimWhenFiringDelay"
        private const val KEY_FLOATING_STICK = "floatingStick"
        private const val KEY_AUTO_SPRINT = "autoSprint"
        private const val KEY_GYRO_MODE = "gyroMode"
        private const val KEY_GYRO_SENSITIVITY = "gyroSensitivity"
        /** The gyroscope's settings, in the order the runtime numbers them (android/input.cpp). */
        const val GYRO_OFF = 0
        const val GYRO_ALWAYS = 1
        const val GYRO_AIMING = 2
        private const val KEY_DRIVER_DIR = "driverDirectory"
        private const val KEY_DRIVER_LIB = "driverLibrary"
        private const val KEY_DRIVER_CHOICE_MADE = "driverChoiceMade"
        private const val KEY_BUNDLED_DRIVER_SHA = "bundledDriverSha"
        private const val KEY_BUNDLED_DRIVER_DIR = "bundledDriverDirectory"
        private const val KEY_DRIVER_AUTO_KEY = "driverAutoKey"
        private const val KEY_DRIVER_PICKED = "driverPicked"
        private const val KEY_DRIVER_VERDICTS = "driverVerdicts"
        private const val KEY_DRIVER_NOTICE = "driverNotice"
        private const val KEY_DRIVER_TRIAL = "driverTrial"
        private const val KEY_DRIVER_CRASH_SEEN = "driverCrashSeen"
        private const val KEY_DRIVER_CATALOG_AT = "driverCatalogAt"
        private const val KEY_DRIVER_CHECKS = "driverChecks"
        private const val KEY_GAME_DIR = "gameDirectory"
        private const val KEY_DISC_IMAGE = "discImage"
        private const val KEY_INSTALL_DISC_IMAGE = "installDiscImage"
        private const val KEY_INSTALL_BY_COPY = "installByCopy"
        private const val KEY_CHOSEN_IMAGE = "chosenImage"
        private const val KEY_UPDATE_CODE_BATTERY = "updateCodeBattery"

        /** [updateCodeBattery]: above any charge a battery has, so only the charger allows it. */
        const val UPDATE_CODE_CHARGING = 101

        /** [updateCodeBattery]: the player builds the game code themselves. */
        const val UPDATE_CODE_NEVER = 1000
        private const val KEY_LAST_BROWSED = "lastBrowsed"
        private const val KEY_WELCOME = "seenWelcome"
        private const val KEY_AUTO_LAUNCH = "autoLaunch"
        private const val KEY_UPDATE_TAG = "seenUpdateTag"
        private const val KEY_PAD_WARNING = "seenPadWarning"
        private const val KEY_GAME_HELP = "seenGameHelp"
        private const val KEY_SETUP_FINISHED = "setupFinished"
        private const val KEY_PREWARM_ALL = "prewarmAllShaders"
        private const val KEY_ASYNC_PIPELINES = "asyncPipelines"
        private const val KEY_UNLOCK_ALL_ITEMS = "unlockAllItems"
        private const val KEY_BOT_COUNT = "botCount"
        private const val KEY_BOT_SKILL = "botSkill"
        private const val KEY_BOTS_AGAINST_PLAYERS = "botsAgainstPlayers"
        private const val KEY_MAP_VOTE = "mapVote"
        /**
         * A server has 18 slots and the host keeps one. Advanced Warfare's
         * has 12 on this console.
         */
        val MAX_BOTS: Int = if (Title.isAw) 11 else 17
        private const val KEY_MATCH_GRAVITY = "matchGravity"
        private const val KEY_MATCH_SPEED = "matchSpeed"
        private const val KEY_MATCH_JUMP = "matchJump"
        private const val KEY_MATCH_SPRINT = "matchUnlimitedSprint"
        private const val KEY_MATCH_FALL_DAMAGE = "matchFallDamage"
        /** What the match option sliders stop at; the game's own value is in each. */
        val MATCH_GRAVITIES = intArrayOf(100, 200, 400, 600, 800, 1200, 1600)
        val MATCH_SPEEDS = intArrayOf(50, 75, 100, 125, 150, 200, 300)
        val MATCH_JUMPS = intArrayOf(20, 39, 60, 80, 120, 200, 400)
        private const val KEY_PERSIST_PIPELINE = "persistPipelineCache"
        private const val KEY_PLAYER_NAME = "playerName"
        private const val KEY_PLAYER_NAME_ASKED = "playerNameAsked"
        private const val KEY_LAN_ACCEPT = "lanAccept"
        private const val KEY_LAN_IFACE = "lanIface"
        private const val KEY_VOICE_CHAT = "voiceChat"
        private const val KEY_VOICE_PTT = "voicePushToTalk"
    }
}
