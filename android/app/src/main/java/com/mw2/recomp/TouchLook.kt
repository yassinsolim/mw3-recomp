package com.mw2.recomp

import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.sqrt

/**
 * The arithmetic of looking by touch and by the gyroscope, apart from the
 * views and sensors that feed it so that it can be tested on a PC.
 *
 * A finger on the look area turns the view by an angle: so many degrees for
 * a centimetre of glass. A centimetre, not a pixel or a share of the screen,
 * because a thumb is the same size on every phone: the setting then feels
 * the same on a 5.5 inch screen and a 6.8 inch one, whatever their
 * resolution.
 *
 * The centimetre is the system's, 63 dp, as the buttons' sizes are: the
 * density a phone draws at is chosen to be near its panel's and is the one
 * figure every phone gets right. The panel's own (DisplayMetrics.xdpi) is
 * not: the AYN Thor and the Retroid Pocket Flip 2 both report 318.976 for
 * panels of 367 and 401 pixels an inch, a wrong number that looks like a
 * right one. Their densities are 369 and 360, so the system's centimetre is
 * a true one on the Thor and 1.11 cm of the Flip's glass.
 */
object TouchLook {

    /**
     * 13 degrees a centimetre: half a turn for a swipe of 13.8 cm, which is
     * the width of the picture on the two phones this was measured on (13.3
     * cm on the AYN Thor, 12.2 cm on the Retroid Pocket Flip 2) and within a
     * centimetre and a half of a 6.7 inch phone's. The look area is half the
     * screen, so one swipe across it is a quarter turn, the corner of a
     * corridor.
     */
    const val DEFAULT_DEGREES_PER_CM = 13f
    const val MIN_DEGREES_PER_CM = 3f
    const val MAX_DEGREES_PER_CM = 40f

    private const val CM_PER_INCH = 2.54f

    /** The degrees a pixel of movement turns the view, for a setting in degrees a centimetre, on a screen drawn at [densityDpi]. */
    fun degreesPerPixel(degreesPerCm: Float, densityDpi: Int): Float =
        degreesPerCm * CM_PER_INCH / densityDpi

    /** How far across the screen a swipe turns the view half way round, in screen widths, for the settings' label. */
    fun halfTurnInScreens(degreesPerCm: Float, widthPixels: Int, densityDpi: Int): Float =
        180f / (degreesPerPixel(degreesPerCm, densityDpi) * widthPixels)

    /**
     * Whether a touch that has come up was a tap: short, and still where it
     * went down. [slop] is the distance a resting finger wanders, in pixels.
     */
    fun isTap(downMillis: Long, upMillis: Long, travelled: Float, slop: Float): Boolean =
        upMillis - downMillis <= TAP_MILLIS && travelled <= slop

    private const val TAP_MILLIS = 200L

    /** How long a tap holds the trigger: long enough for a title drawing 30 frames a second to see it. */
    const val TAP_FIRE_MILLIS = 80L
}

/**
 * The phone's turning, as a turn of the view.
 *
 * It is fed the game rotation vector (the gyroscope and the accelerometer
 * fused by the phone, without the compass, so a magnet nearby does not turn
 * the view): the phone's attitude as a quaternion that takes the phone's
 * axes to the world's, whose Z is up. Two attitudes one after the other give
 * the turn between them. Sideways is the part of it about the world's
 * vertical, whichever way the phone is held: flat on a table or upright,
 * turning it as a compass needle turns looks left and right. Up and down is
 * the part about the screen's own left-to-right axis, tipping its top edge
 * towards or away from the player.
 */
class GyroLook {

    private val last = FloatArray(4)
    private var lastNanos = 0L
    private var have = false

    /** Forgets the last attitude: the sensor was stopped, and the next one is not a turn from it. */
    fun reset() {
        have = false
    }

    /**
     * Takes an attitude ([x], [y], [z], [w], as the sensor reports it) at
     * [nanos], with the screen turned from the phone's natural position by
     * [rotation] quarter turns (Surface.ROTATION_*), and puts the view's
     * turn in degrees into [out]: to the right, then downwards. False when
     * there is no turn to make: the first attitude, or a jump no hand made.
     */
    fun turn(x: Float, y: Float, z: Float, w: Float, nanos: Long, rotation: Int, out: FloatArray): Boolean {
        val length = sqrt(x * x + y * y + z * z + w * w)
        if (length < 0.5f) return false
        val qx = x / length
        val qy = y / length
        val qz = z / length
        val qw = w / length
        val px = last[0]
        val py = last[1]
        val pz = last[2]
        val pw = last[3]
        val seconds = (nanos - lastNanos) / 1e9f
        val had = have
        last[0] = qx
        last[1] = qy
        last[2] = qz
        last[3] = qw
        lastNanos = nanos
        have = true
        if (!had || seconds <= 0f || seconds > 0.25f) return false

        // now * inverse(before): the turn as the world sees it. Only its
        // part about the vertical is wanted.
        var worldZ = qw * -pz + qx * -py - qy * -px + qz * pw
        var worldW = qw * pw + qx * px + qy * py + qz * pz
        // inverse(before) * now: the same turn as the phone sees it.
        var localX = pw * qx - px * qw - py * qz + pz * qy
        var localY = pw * qy + px * qz - py * qw - pz * qx
        // A quaternion and its negative are one attitude.
        if (worldW < 0f) {
            worldZ = -worldZ
            worldW = -worldW
            localX = -localX
            localY = -localY
        }
        // The screen's left-to-right axis among the phone's own.
        val aboutScreenRight = when (rotation) {
            1 -> localY
            2 -> -localX
            3 -> -localY
            else -> localX
        }
        // For the small turn between two samples the vector part is half
        // the angle, in radians. Anticlockwise from above is to the left,
        // and the top edge coming towards the player is looking up.
        var right = -2f * worldZ * DEGREES
        var down = -2f * aboutScreenRight * DEGREES
        val turned = hypot(right, down)
        // More than this between two samples is the sensor starting again.
        if (turned > 30f) return false
        // A phone lying still is not perfectly still to its gyroscope. Below
        // a degree a second the turn is scaled down towards nothing, so the
        // view does not creep, and a slow deliberate turn is still followed.
        val rate = turned / seconds
        if (rate < STILL_DEGREES_PER_SECOND) {
            val keep = rate / STILL_DEGREES_PER_SECOND
            right *= keep
            down *= keep
        }
        if (abs(right) < 1e-6f && abs(down) < 1e-6f) return false
        out[0] = right
        out[1] = down
        return true
    }

    private companion object {
        const val DEGREES = 57.29578f
        const val STILL_DEGREES_PER_SECOND = 1f
    }
}
