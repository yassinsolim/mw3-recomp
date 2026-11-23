package com.mw2.recomp

import android.view.Display
import android.view.Window

/**
 * The display's rate under the FPS limit: asking the system for a mode that
 * shows the limit, and telling when the system has not given it. The system's
 * own display setting outranks an app's request, and an AYN Thor whose
 * dual-screen assistant put it back to 60 Hz after a restart showed 60 under
 * a limit of 120. The launcher asks as the game does, so that what it says
 * about the screen is what the game will find.
 */
object DisplayRate {

    /**
     * Asks for the slowest mode that shows `limit` frames a second evenly (a
     * 120 Hz mode shows 60 as well as a 60 Hz one, and costs more battery),
     * else the slowest one that is fast enough.
     */
    fun request(window: Window, display: Display?, limit: Int) {
        val attributes = window.attributes
        val target = limit.toFloat()
        attributes.preferredRefreshRate = target
        if (display != null) {
            val modes = display.supportedModes.filter { it.refreshRate + 1f >= target }
            val mode = modes
                .filter { kotlin.math.abs(it.refreshRate / target - Math.round(it.refreshRate / target)) < 0.03f }
                .minByOrNull { it.refreshRate }
                ?: modes.minByOrNull { it.refreshRate }
            if (mode != null) attributes.preferredDisplayModeId = mode.modeId
        }
        window.attributes = attributes
    }

    /**
     * The rate the system holds the display at, when that is under the limit
     * and the screen has a mode that is not; null when all is as asked. It
     * means something only once a request has had a moment to be granted.
     */
    fun heldAt(display: Display?, limit: Int): Float? {
        display ?: return null
        val now = display.mode.refreshRate
        if (now >= limit * 0.97f || display.supportedModes.none { it.refreshRate >= limit * 0.97f }) return null
        return now
    }
}
