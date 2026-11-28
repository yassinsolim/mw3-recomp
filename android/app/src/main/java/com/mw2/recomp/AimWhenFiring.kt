package com.mw2.recomp

/**
 * "Aim when firing (touch)": the on-screen Fire button also aims, so a player
 * with touch controls and two thumbs does not need a third for the left
 * trigger.
 *
 * Pressing Fire holds the left trigger (the sights come up) at once, and the
 * right trigger only after [delayMillis], when they are up: a shot pulled in
 * the first frames of a raise is a hip shot, which is what the player turned
 * this on to avoid. Releasing Fire lets go of both. A tap shorter than the
 * delay still shoots: the right trigger is held for at least
 * [MIN_SHOT_MILLIS], so the game, which reads the pad once a frame, sees it
 * pressed, and the left trigger goes up with it.
 *
 * Fire pressed with the player already aiming (Aim held, or toggled on) is
 * not touched: the sights are up and the shot goes out at once, as without
 * this setting, and nothing is held that the player did not ask for. That is
 * decided once, when Fire goes down.
 *
 * Pure arithmetic over times the caller gives: the view reads it each time it
 * pushes a pad state, and asks [nextChangeAt] when to push another.
 */
class AimWhenFiring(var delayMillis: Long = DEFAULT_DELAY_MILLIS) {
    private var active = false
    private var aimSince = 0L           // when the sights were asked for
    private var shotFrom = 0L           // from when the right trigger is held
    private var releasedAt = NOT_YET    // when Fire came up, if it has

    /**
     * Fire went down at [now]. [aimingAlready]: Aim is held or on. Returns
     * whether this press is managed here; when it is not, the caller treats
     * Fire as a plain trigger.
     */
    fun press(now: Long, aimingAlready: Boolean): Boolean {
        if (active && now >= endOfHold()) active = false
        if (active) {
            // Fire again while the last tap's trigger was still held: the
            // sights are up or coming up, and stay so.
            shotFrom = maxOf(now, aimSince + delayMillis)
            releasedAt = NOT_YET
            return true
        }
        if (aimingAlready) return false
        active = true
        aimSince = now
        shotFrom = now + delayMillis
        releasedAt = NOT_YET
        return true
    }

    /** Fire came up at [now]. */
    fun release(now: Long) {
        if (active && releasedAt == NOT_YET) releasedAt = now
    }

    /** Everything up, at once: the pad is hidden, edited or paused. */
    fun cancel() {
        active = false
        releasedAt = NOT_YET
    }

    /** Whether a press is being managed at [now]: while Fire is down and until a short tap's shot is out. */
    fun managing(now: Long): Boolean = active && now < endOfHold()

    /** The left trigger is held at [now]. */
    fun leftTrigger(now: Long): Boolean = managing(now)

    /** The right trigger is held at [now]. */
    fun rightTrigger(now: Long): Boolean = managing(now) && now >= shotFrom

    /**
     * The next time at or after [now] at which either trigger changes, or
     * [NOT_YET] when none will on its own (Fire is held and the shot is out,
     * or nothing is being managed).
     */
    fun nextChangeAt(now: Long): Long {
        if (!managing(now)) return NOT_YET
        if (now < shotFrom) return shotFrom
        return endOfHold()
    }

    // Both triggers go up when Fire has been up and the shot has had its
    // minimum; held for ever while it is down.
    private fun endOfHold(): Long =
        if (releasedAt == NOT_YET) NOT_YET else maxOf(releasedAt, shotFrom + MIN_SHOT_MILLIS)

    companion object {
        /**
         * Measured in Call of Duty 4's campaign with its M4 and holographic sight (tools/aim_fire.py, docs/android.md): the
         * sight starts to come up some 100 ms after the trigger is pulled and is up at about 240 ms. A shot at 200 ms
         * leaves with the sights nearly there and costs a fifth of a second; the delay is a setting (0 to
         * [MAX_DELAY_MILLIS]) for a weapon, a sniper rifle's scope above all, that takes longer.
         */
        const val DEFAULT_DELAY_MILLIS = 200L
        const val MAX_DELAY_MILLIS = 500L
        const val DELAY_STEP_MILLIS = 50L

        /** Two frames of the slower titles: enough for the game's own poll to see the trigger. */
        const val MIN_SHOT_MILLIS = 70L

        const val NOT_YET = Long.MAX_VALUE
    }
}
