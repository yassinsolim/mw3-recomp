package com.mw2.recomp

import android.content.SharedPreferences
import kotlin.math.cos
import kotlin.math.sin
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Invert horizontal and Invert vertical: the sign each combination gives a
 * finger's movement, the phone's turn and a controller's right stick, and
 * what an older version's single switch becomes.
 */
class LookInvertTest {

    private val combinations = listOf(
        LookInvert(horizontal = false, vertical = false),
        LookInvert(horizontal = true, vertical = false),
        LookInvert(horizontal = false, vertical = true),
        LookInvert(horizontal = true, vertical = true),
    )

    private fun sign(flipped: Boolean) = if (flipped) -1f else 1f

    // ---- a finger ----

    @Test
    fun aSwipeIsFlippedOnlyOnTheAxisThatIsInverted() {
        for (invert in combinations) {
            // Right and down, in pixels: the runtime's signs for a finger.
            assertEquals(sign(invert.horizontal) * 12f, invert.right(12f), 0f)
            assertEquals(sign(invert.vertical) * 7f, invert.down(7f), 0f)
            assertEquals(sign(invert.horizontal) * -12f, invert.right(-12f), 0f)
            assertEquals(sign(invert.vertical) * -7f, invert.down(-7f), 0f)
        }
    }

    @Test
    fun nothingInvertedLeavesAMovementAsItWas() {
        assertEquals(3.5f, LookInvert.NONE.right(3.5f), 0f)
        assertEquals(-2f, LookInvert.NONE.down(-2f), 0f)
    }

    @Test
    fun nothingTurnsWhereThereWasNoMovement() {
        for (invert in combinations) {
            assertEquals(0f, Math.abs(invert.right(0f)), 0f)
            assertEquals(0f, Math.abs(invert.down(0f)), 0f)
        }
    }

    // ---- the gyroscope ----

    private class Attitude(val x: Float, val y: Float, val z: Float, val w: Float) {
        operator fun times(o: Attitude) = Attitude(
            w * o.x + x * o.w + y * o.z - z * o.y,
            w * o.y - x * o.z + y * o.w + z * o.x,
            w * o.z + x * o.y - y * o.x + z * o.w,
            w * o.w - x * o.x - y * o.y - z * o.z,
        )
    }

    private fun about(x: Float, y: Float, z: Float, degrees: Float): Attitude {
        val half = Math.toRadians(degrees.toDouble()).toFloat() / 2f
        return Attitude(x * sin(half), y * sin(half), z * sin(half), cos(half))
    }

    // As TouchLookTest's phone, held upright in landscape.
    private val upright = about(0f, 0f, 1f, 90f) * about(1f, 0f, 0f, 90f)

    private fun turn(after: Attitude): FloatArray {
        val gyro = GyroLook()
        val out = FloatArray(2)
        gyro.turn(upright.x, upright.y, upright.z, upright.w, 1_000_000_000L, 0, out)
        assertTrue(gyro.turn(after.x, after.y, after.z, after.w, 1_005_000_000L, 0, out))
        return out
    }

    @Test
    fun theGyroscopeIsFlippedPerAxisAsAFingerIs() {
        // Clockwise from above is to the right (+0.5 degrees sideways, as
        // TouchLookTest has it); the top edge towards the player is looking up (-0.5).
        val clockwise = turn(about(0f, 0f, 1f, -0.5f) * upright)
        val topTowardsPlayer = turn(upright * about(1f, 0f, 0f, 0.5f))
        assertEquals(0.5f, clockwise[0], 1e-3f)
        assertEquals(-0.5f, topTowardsPlayer[1], 1e-3f)
        for (invert in combinations) {
            assertEquals(sign(invert.horizontal) * 0.5f, invert.right(clockwise[0]), 1e-3f)
            assertEquals(sign(invert.vertical) * -0.5f, invert.down(topTowardsPlayer[1]), 1e-3f)
        }
    }

    @Test
    fun aTurnAboutOneAxisDoesNotMoveTheOther() {
        // Sideways only: the vertical switch does not make up and down appear.
        val sideways = turn(about(0f, 0f, 1f, -0.5f) * upright)
        assertEquals(0f, sideways[1], 1e-3f)
        for (invert in combinations) assertEquals(0f, invert.down(sideways[1]), 1e-3f)
        // And the horizontal switch does not make sideways appear from a nod.
        val nod = turn(upright * about(1f, 0f, 0f, 0.5f))
        assertEquals(0f, nod[0], 1e-3f)
        for (invert in combinations) assertEquals(0f, invert.right(nod[0]), 1e-3f)
    }

    // ---- a controller's right stick ----

    @Test
    fun aControllersRightStickIsFlippedLeftAndRightOnlyWhenAsked() {
        for (value in floatArrayOf(-1f, -0.4f, 0f, 0.7f, 1f)) {
            assertEquals(value, LookInvert.stickX(value, invert = false), 0f)
            assertEquals(-value, LookInvert.stickX(value, invert = true), 0f)
        }
    }

    // ---- what an older version saved ----

    private class Store(vararg initial: Pair<String, Boolean>) : SharedPreferences {
        val values = HashMap<String, Any?>().apply { putAll(initial) }

        override fun getAll(): MutableMap<String, *> = values
        override fun getString(key: String, defValue: String?) = values[key] as? String ?: defValue
        override fun getStringSet(key: String, defValues: MutableSet<String>?): MutableSet<String>? = defValues
        override fun getInt(key: String, defValue: Int) = values[key] as? Int ?: defValue
        override fun getLong(key: String, defValue: Long) = values[key] as? Long ?: defValue
        override fun getFloat(key: String, defValue: Float) = values[key] as? Float ?: defValue
        override fun getBoolean(key: String, defValue: Boolean) = values[key] as? Boolean ?: defValue
        override fun contains(key: String) = values.containsKey(key)
        override fun registerOnSharedPreferenceChangeListener(l: SharedPreferences.OnSharedPreferenceChangeListener?) = Unit
        override fun unregisterOnSharedPreferenceChangeListener(l: SharedPreferences.OnSharedPreferenceChangeListener?) = Unit
        override fun edit(): SharedPreferences.Editor = object : SharedPreferences.Editor {
            override fun putString(key: String, value: String?) = also { values[key] = value }
            override fun putStringSet(key: String, value: MutableSet<String>?) = also { values[key] = value }
            override fun putInt(key: String, value: Int) = also { values[key] = value }
            override fun putLong(key: String, value: Long) = also { values[key] = value }
            override fun putFloat(key: String, value: Float) = also { values[key] = value }
            override fun putBoolean(key: String, value: Boolean) = also { values[key] = value }
            override fun remove(key: String) = also { values.remove(key) }
            override fun clear() = also { values.clear() }
            override fun commit() = true
            override fun apply() = Unit
        }
    }

    @Test
    fun theOldSwitchBecomesInvertVertical() {
        assertTrue(LookInvert.verticalIn(Store(LookInvert.KEY_LEGACY to true)))
        assertFalse(LookInvert.verticalIn(Store(LookInvert.KEY_LEGACY to false)))
    }

    @Test
    fun aPlayerWhoNeverSetItStartsOff() {
        assertFalse(LookInvert.verticalIn(Store()))
    }

    @Test
    fun theNewSettingWinsOnceItIsSet() {
        // Turned off after the update, with the old one still in the file.
        assertFalse(LookInvert.verticalIn(Store(LookInvert.KEY_LEGACY to true, LookInvert.KEY_VERTICAL to false)))
        assertTrue(LookInvert.verticalIn(Store(LookInvert.KEY_LEGACY to false, LookInvert.KEY_VERTICAL to true)))
    }

    @Test
    fun theOldSwitchNeverTurnsOnInvertHorizontalOrTheStick() {
        // Prefs reads these two from their own keys, which an older version never wrote.
        val old = Store(LookInvert.KEY_LEGACY to true)
        assertFalse(old.getBoolean(LookInvert.KEY_HORIZONTAL, false))
        assertFalse(old.getBoolean(LookInvert.KEY_PAD_HORIZONTAL, false))
        assertTrue(LookInvert.verticalIn(old))
    }

    @Test
    fun theKeysAreDistinct() {
        val keys = listOf(LookInvert.KEY_LEGACY, LookInvert.KEY_HORIZONTAL, LookInvert.KEY_VERTICAL, LookInvert.KEY_PAD_HORIZONTAL)
        assertEquals(keys.size, keys.toSet().size)
    }
}
