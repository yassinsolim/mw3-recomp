package com.mw2.recomp

import android.content.SharedPreferences

/**
 * Which way each axis of looking is turned round, for a finger on the look
 * area and for the gyroscope: the two switches under Settings > Controls
 * and in the game's menu.
 *
 * Turns are signed as the runtime takes them (NativeBridge.nativeLookDelta,
 * nativeGyroDelta): positive to the right and positive downwards. Inverting
 * an axis flips the sign of that part of the turn and leaves the other
 * alone, so the four combinations are the four sign pairs.
 *
 * A physical controller's right stick is not among them. The title reads
 * it and has its own Invert option for up and down (input_invertPitch,
 * profile_toggleInvertedPitch), which the angles a finger and the gyroscope
 * make never pass through; none has one for left and right, which is what
 * [stickX] is for.
 */
class LookInvert(val horizontal: Boolean, val vertical: Boolean) {

    /** A turn to the right, in whatever unit: pixels of a finger's movement or degrees of the phone's. */
    fun right(amount: Float): Float = if (horizontal) -amount else amount

    /** A turn downwards. */
    fun down(amount: Float): Float = if (vertical) -amount else amount

    companion object {
        val NONE = LookInvert(horizontal = false, vertical = false)

        /** What an older version saved for the one switch it had, which turned up and down round for a finger. */
        const val KEY_LEGACY = "lookInvert"
        const val KEY_HORIZONTAL = "lookInvertX"
        const val KEY_VERTICAL = "lookInvertY"
        const val KEY_PAD_HORIZONTAL = "padInvertX"

        /**
         * Invert vertical as saved. A player who has not touched it since
         * the one switch became two has the old switch's value, so what they
         * set stays as it was; once it is set, it is the new key's own.
         */
        fun verticalIn(store: SharedPreferences): Boolean =
            if (store.contains(KEY_VERTICAL)) store.getBoolean(KEY_VERTICAL, false)
            else store.getBoolean(KEY_LEGACY, false)

        /** A controller's right stick, to the right positive, as the title is to read it. */
        fun stickX(value: Float, invert: Boolean): Float = if (invert) -value else value
    }
}
