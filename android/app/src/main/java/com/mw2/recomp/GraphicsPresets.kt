package com.mw2.recomp

/**
 * The graphics presets of Settings and the game's menu: each is a value for
 * every setting of the Advanced page and nothing else, so a preset is read
 * off that page and Custom is whatever the page holds. The settings are the
 * runtime's own switches (Prefs, GameActivity.applyOptions); a preset adds
 * none.
 *
 * No Android in it, so the table is checked without a phone
 * (GraphicsPresetsTest).
 */
object GraphicsPresets {
    /** The run follows what the device sustains, as a fresh install does (runtime/adaptive.h). */
    const val AUTOMATIC = 0
    const val BATTERY = 1
    const val BALANCED = 2
    const val BEST = 3
    /** The Advanced page as the player left it. */
    const val CUSTOM = 4

    /** The 3D scene follows the load: drawn smaller while the graphics chip is late (MW2_SCENE_DYNAMIC). */
    const val SCENE_FOLLOWS = 0
    /** Always whole, and the console's size on every chip (MW2_SCENE_DYNAMIC=0, MW2_SCENE=console). */
    const val SCENE_WHOLE = 1
    /** Always [SMALLER_PERCENT] of its width (MW2_SCENE_SHARE). */
    const val SCENE_SMALLER = 2
    /** One of the runtime's own steps (renderer_scene.cpp): half the scene's pixels. */
    const val SMALLER_PERCENT = 70

    /** Prefs.sunShadows: left to the runtime, on, off. */
    const val SHADOWS_AUTO = 0
    const val SHADOWS_ON = 1
    const val SHADOWS_OFF = 2

    /**
     * The Advanced page's settings but the driver, which no preset chooses:
     * the screen's share the picture is made at in percent, the frame-rate
     * limit (0: the runtime's, from the screen's rate), the sun's shadows,
     * the render multiplier and the 3D scene.
     */
    data class Values(
        val resolution: Int,
        val fpsLimit: Int,
        val sunShadows: Int,
        val renderScale: Int,
        val scene: Int,
    )

    /**
     * What a preset sets; null for Custom, which sets nothing. `twice`: play
     * on this device has shown the graphics chip has the time to draw at
     * twice the size ([twiceHolds]), which only Best looking asks.
     */
    fun values(preset: Int, twice: Boolean = false): Values? = when (preset) {
        AUTOMATIC -> Values(70, 0, SHADOWS_AUTO, 1, SCENE_FOLLOWS)
        // Half the frames and half the scene's pixels are what the chip is
        // spared; the picture keeps the screen's share the others have, since
        // the HUD and the menus are drawn at the frame's size either way.
        BATTERY -> Values(70, 30, SHADOWS_OFF, 1, SCENE_SMALLER)
        BALANCED -> Values(70, 60, SHADOWS_AUTO, 1, SCENE_FOLLOWS)
        BEST -> Values(100, 0, SHADOWS_ON, if (twice) 2 else 1, SCENE_WHOLE)
        else -> null
    }

    /**
     * The preset these settings are, for an install older than the presets:
     * Automatic for one nobody changed, Custom for any the table has not got.
     */
    fun matching(values: Values): Int =
        listOf(AUTOMATIC, BATTERY, BALANCED, BEST).firstOrNull { values(it) == values } ?: CUSTOM

    /** The preset in English, for the log and the reports. */
    fun name(preset: Int): String = when (preset) {
        AUTOMATIC -> "automatic"
        BATTERY -> "battery saver"
        BALANCED -> "balanced"
        BEST -> "best looking"
        else -> "custom"
    }

    /**
     * What twice the size costs the graphics chip against the console's:
     * Advanced Warfare on an AYN Thor took 3.6 ms a frame at 1x and 7.6 at
     * 2x in a match, 4.8 and 10.8 in the lobby (docs/switches.md, MW2_SCALE).
     */
    const val TWICE_COSTS = 2.25f

    /** Seconds of play a measure is taken from: the first minute is a level's lightest view and its compiling. */
    const val MEASURED_AFTER = 60f

    /**
     * Whether twice the size would still leave the chip the room the
     * runtime wants before it calls a frame late (Governor::kChipLate, 85%
     * of the period): `chipMs` is the chip's time a frame at 1x in a minute
     * and more of play under Best looking, negative where none was measured
     * or the chip keeps no time, and `rate` the frames a second the screen
     * shows at its fastest.
     */
    fun twiceHolds(chipMs: Float, rate: Int): Boolean =
        chipMs >= 0f && rate > 0 && chipMs * TWICE_COSTS <= 0.85f * 1000f / rate

    /**
     * Whether a run at twice the size did not hold: more than a tenth of
     * its play came under nine tenths of the rate, or the runtime held it
     * to a slower rate than it started at.
     */
    fun twiceFailed(missedShare: Float, heldTo: Int, startedAt: Int): Boolean =
        missedShare > 0.10f || (heldTo in 1 until startedAt)
}
