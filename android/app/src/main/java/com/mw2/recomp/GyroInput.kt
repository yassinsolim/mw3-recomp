package com.mw2.recomp

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.hardware.display.DisplayManager
import android.os.Handler
import android.os.HandlerThread
import android.os.Process
import android.view.Display
import kotlin.math.max
import kotlin.math.sqrt

/**
 * The phone's gyroscope turning the view (GyroLook has the arithmetic): off
 * unless the player turns it on, and then added to whatever a finger or a
 * controller's stick does.
 *
 * The game rotation vector is read 200 times a second on a thread of its
 * own. The system delivers a sensor on the main thread unless told
 * otherwise, behind every touch and every frame of the app's own drawing;
 * 200 a second is the most an app is given without a permission, and at the
 * usual "game" rate of 50 a turn would reach the view up to a frame later
 * than it does.
 */
class GyroInput(context: Context) : SensorEventListener {

    private val sensors = context.getSystemService(Context.SENSOR_SERVICE) as SensorManager
    private val sensor: Sensor? = sensors.getDefaultSensor(Sensor.TYPE_GAME_ROTATION_VECTOR)
    private val display: Display? =
        (context.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager).getDisplay(Display.DEFAULT_DISPLAY)

    private val look = GyroLook()
    private val turn = FloatArray(2)
    private var thread: HandlerThread? = null
    private var rotation = 0
    private var rotationReadNanos = 0L

    /** Degrees the view turns for a degree the phone is turned. */
    @Volatile
    var sensitivity = 1f

    /** Whether this phone has the sensor at all. */
    val available: Boolean
        get() = sensor != null

    /** Listens, or stops listening: nothing is read while the setting is off or the game is away. */
    fun listen(on: Boolean) {
        if (on == (thread != null) || sensor == null) return
        if (on) {
            val started = HandlerThread("gyro-look", Process.THREAD_PRIORITY_DISPLAY).apply { start() }
            thread = started
            sensors.registerListener(this, sensor, PERIOD_MICROS, Handler(started.looper))
        } else {
            sensors.unregisterListener(this)
            thread?.quitSafely()
            thread = null
            look.reset()
        }
    }

    override fun onSensorChanged(event: SensorEvent) {
        val values = event.values
        if (values.size < 3) return
        // The screen turns with the phone between the two landscapes; asked
        // twice a second, not at every sample.
        if (event.timestamp - rotationReadNanos > 500_000_000L || rotationReadNanos == 0L) {
            rotation = display?.rotation ?: 0
            rotationReadNanos = event.timestamp
        }
        // Older sensors leave the fourth number out: it is what makes the
        // four a unit.
        val x = values[0]
        val y = values[1]
        val z = values[2]
        val w = if (values.size >= 4) values[3] else sqrt(max(0f, 1f - x * x - y * y - z * z))
        if (look.turn(x, y, z, w, event.timestamp, rotation, turn)) {
            NativeBridge.nativeGyroDelta(turn[0] * sensitivity, turn[1] * sensitivity, System.nanoTime())
        }
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit

    private companion object {
        const val PERIOD_MICROS = 5_000
    }
}
