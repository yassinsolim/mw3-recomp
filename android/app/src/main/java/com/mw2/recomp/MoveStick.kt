package com.mw2.recomp

import kotlin.math.hypot

/**
 * The rules of the on-screen movement stick that are not drawing: where a
 * finger's push is, which touches a floating stick takes, and when pushing
 * it hard enough for long enough means "sprint". Plain arithmetic, so that
 * [MoveStickTest] can hold them without a phone.
 */
object MoveStick {
    /**
     * A finger at ([fingerX], [fingerY]) on a stick whose centre is at
     * ([originX], [originY]) and whose edge is [radius] away: how far it is
     * pushed, on the guest's axes (x to the right, y up, which is the
     * opposite of the screen's) and no further than the edge, in [out].
     * The centre of a fixed stick is where its drawing is; the centre of a
     * floating one is where the finger first came down.
     */
    fun deflection(fingerX: Float, fingerY: Float, originX: Float, originY: Float, radius: Float, out: FloatArray) {
        var dx = (fingerX - originX) / radius
        var dy = -(fingerY - originY) / radius
        val length = hypot(dx, dy)
        if (length > 1f) {
            dx /= length
            dy /= length
        }
        out[0] = dx
        out[1] = dy
    }

    /**
     * Whether a touch at [x] is on the movement stick's side of a screen
     * [width] wide, which is the half its resting place is in: the same rule
     * that makes the other half the look area. A floating stick takes the
     * touches on that side that no button has.
     */
    fun onMovementSide(x: Float, width: Float, restingXFrac: Float): Boolean =
        if (restingXFrac >= 0.5f) x >= width * 0.5f else x < width * 0.5f

    /** How far a push is, 0 at the centre and 1 at the edge. */
    fun magnitude(x: Float, y: Float): Float = hypot(x, y).coerceAtMost(1f)
}

/**
 * Auto-sprint: the movement stick held at its edge for [delayMillis] presses
 * the game's sprint (the left stick's click) for the player; pulled back
 * below [RELEASE_AT] it is let go, and so it is when the finger comes up.
 * The edge to start at is [PUSH_AT] and the way back is lower, so a thumb
 * that wobbles at the edge neither starts nor stops it by turns.
 *
 * The caller gives it the push ([MoveStick.magnitude]) and the time whenever
 * it sends the pad's state, and asks [nextChangeAt] when to send another: a
 * thumb held still at the edge sends nothing by itself.
 */
class AutoSprint(var delayMillis: Long = DEFAULT_DELAY_MILLIS) {
    var sprinting = false
        private set
    private var pushedSince = NOT_YET

    /** Sprint is pressed at [now], with the stick [magnitude] pushed. */
    fun update(magnitude: Float, now: Long): Boolean {
        if (sprinting) {
            if (magnitude < RELEASE_AT) cancel()
        } else if (magnitude >= PUSH_AT) {
            if (pushedSince == NOT_YET) pushedSince = now
            if (now - pushedSince >= delayMillis) sprinting = true
        } else {
            pushedSince = NOT_YET
        }
        return sprinting
    }

    /** When sprint will be pressed if the stick stays where it is, or [NOT_YET]. */
    fun nextChangeAt(now: Long): Long =
        if (!sprinting && pushedSince != NOT_YET) maxOf(now, pushedSince + delayMillis) else NOT_YET

    /** Let go, whatever the stick does: it is hidden, edited, paused or the setting is off. */
    fun cancel() {
        sprinting = false
        pushedSince = NOT_YET
    }

    companion object {
        /** A third of a second at the edge: a flick across the pad on the way somewhere does not sprint. */
        const val DEFAULT_DELAY_MILLIS = 300L

        /** Nine tenths of the way to the edge counts as the edge: a thumb does not stay exactly on it. */
        const val PUSH_AT = 0.9f

        /** Back to seven tenths and sprint is let go. */
        const val RELEASE_AT = 0.7f

        const val NOT_YET = Long.MAX_VALUE
    }
}
