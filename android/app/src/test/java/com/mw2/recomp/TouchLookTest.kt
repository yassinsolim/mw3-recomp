package com.mw2.recomp

import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.sin
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Looking by touch and by the gyroscope, as arithmetic: a centimetre of
 * glass turns the view the same on screens of different densities, and the
 * phone's turning comes out as the view's with the right signs.
 */
class TouchLookTest {

    @Test
    fun aCentimetreTurnsTheSameOnEveryScreen() {
        // The AYN Thor's density and a denser phone's: the same finger
        // travel is a different number of pixels and the same angle.
        for (density in intArrayOf(369, 420, 480)) {
            val pixelsInCentimetre = density / 2.54f
            val turned = TouchLook.degreesPerPixel(13f, density) * pixelsInCentimetre
            assertEquals(13f, turned, 1e-3f)
        }
    }

    @Test
    fun theDefaultIsHalfATurnAcrossAboutOneScreen() {
        // A 1920 pixel wide screen drawn at 369 pixels an inch is 13.2 cm wide.
        val screens = TouchLook.halfTurnInScreens(TouchLook.DEFAULT_DEGREES_PER_CM, 1920, 369)
        assertEquals(1.05f, screens, 0.01f)
    }

    @Test
    fun aTapIsShortAndStill() {
        assertTrue(TouchLook.isTap(1000, 1120, 3f, 16f))
        assertFalse(TouchLook.isTap(1000, 1400, 3f, 16f))
        assertFalse(TouchLook.isTap(1000, 1120, 40f, 16f))
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

    /** The turn between two attitudes five milliseconds apart, or null when there is none. */
    private fun turnBetween(before: Attitude, after: Attitude, rotation: Int): FloatArray? {
        val gyro = GyroLook()
        val out = FloatArray(2)
        assertFalse(gyro.turn(before.x, before.y, before.z, before.w, 1_000_000_000L, rotation, out))
        return if (gyro.turn(after.x, after.y, after.z, after.w, 1_005_000_000L, rotation, out)) out else null
    }

    // A phone held upright in landscape, its screen facing south: the
    // natural top edge (the phone's Y) points west, the screen's right (the
    // phone's -Y... as the screen is turned a quarter) east.
    private val upright = about(0f, 0f, 1f, 90f) * about(1f, 0f, 0f, 90f)

    @Test
    fun turningClockwiseSeenFromAboveLooksRight() {
        // Half a degree about the world's vertical, clockwise from above,
        // whatever way the phone is held.
        for (held in listOf(upright, about(1f, 0f, 0f, 30f), Attitude(0f, 0f, 0f, 1f))) {
            val turned = turnBetween(held, about(0f, 0f, 1f, -0.5f) * held, 0)!!
            assertEquals(0.5f, turned[0], 1e-3f)
        }
        val left = turnBetween(upright, about(0f, 0f, 1f, 0.5f) * upright, 0)!!
        assertEquals(-0.5f, left[0], 1e-3f)
    }

    @Test
    fun tippingTheTopEdgeTowardsThePlayerLooksUp() {
        // About the screen's left-to-right axis, which is the phone's X in
        // its natural position and its Y with the screen a quarter turn on.
        val natural = turnBetween(upright, upright * about(1f, 0f, 0f, 0.5f), 0)!!
        assertEquals(-0.5f, natural[1], 1e-3f)
        assertEquals(0f, natural[0], 1e-3f)
        val quarter = turnBetween(upright, upright * about(0f, 1f, 0f, 0.5f), 1)!!
        assertEquals(-0.5f, quarter[1], 1e-3f)
        val threeQuarters = turnBetween(upright, upright * about(0f, 1f, 0f, 0.5f), 3)!!
        assertEquals(0.5f, threeQuarters[1], 1e-3f)
    }

    @Test
    fun theNegativeOfAnAttitudeIsTheSameAttitude() {
        val after = about(0f, 0f, 1f, -0.5f) * upright
        val turned = turnBetween(upright, Attitude(-after.x, -after.y, -after.z, -after.w), 0)!!
        assertEquals(0.5f, turned[0], 1e-3f)
    }

    @Test
    fun aPhoneLyingStillDoesNotCreep() {
        // A hundredth of a degree in five milliseconds is two degrees a
        // second and is followed whole; a thousandth is a fifth of a degree
        // a second, sensor noise, and all but a fifth of it is dropped.
        val followed = turnBetween(upright, about(0f, 0f, 1f, -0.01f) * upright, 0)!!
        assertEquals(0.01f, followed[0], 1e-4f)
        val noise = turnBetween(upright, about(0f, 0f, 1f, -0.001f) * upright, 0)
        assertTrue(noise == null || abs(noise[0]) < 0.0003f)
    }

    @Test
    fun aJumpIsNotATurn() {
        // The sensor starting again somewhere else, or after a pause.
        assertEquals(null, turnBetween(upright, about(0f, 0f, 1f, 90f) * upright, 0))
        val gyro = GyroLook()
        val out = FloatArray(2)
        gyro.turn(upright.x, upright.y, upright.z, upright.w, 1_000_000_000L, 0, out)
        val later = about(0f, 0f, 1f, -0.5f) * upright
        assertFalse(gyro.turn(later.x, later.y, later.z, later.w, 2_000_000_000L, 0, out))
    }
}
