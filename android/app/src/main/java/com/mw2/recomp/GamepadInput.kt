package com.mw2.recomp

import android.content.Context
import android.hardware.input.InputManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import kotlin.math.abs

/**
 * A physical controller, turned into the same state the touch pad produces.
 *
 * Android reports pads through two channels -- keys for the buttons, motion
 * events for the sticks and triggers -- and a device can appear on either
 * before the other. Both are funnelled here so the runtime sees one pad per
 * player, whichever way its parts arrived.
 *
 * The whole thing can be switched off (Prefs.gamepadEnabled). That is not a
 * nicety: plenty of phones enumerate something as a gamepad that is not one,
 * and a stick reading half-left that nobody is touching makes the game
 * unplayable in a way nothing else explains.
 */
class GamepadInput {

    private var enabled = true

    /** Which player each controller is (PadSlots.kt). */
    private val slots = PadSlots()
    private val buttons = IntArray(4)
    private val axes = Array(4) { FloatArray(4) }       // lx, ly, rx, ry
    private val triggers = Array(4) { IntArray(2) }

    private var inputManager: InputManager? = null

    /**
     * Devices coming and going while the game runs. A Bluetooth pad that
     * sleeps comes back under a new id; without this its old id kept slot 0
     * and the pad became the second controller until the app was put away
     * and brought back.
     */
    private val deviceListener = object : InputManager.InputDeviceListener {
        override fun onInputDeviceAdded(deviceId: Int) {
            val device = InputDevice.getDevice(deviceId) ?: return
            if (isPad(device)) log("pad: \"${device.name}\" id $deviceId connected; it gets a slot on its first press")
        }

        override fun onInputDeviceRemoved(deviceId: Int) = drop(deviceId)

        override fun onInputDeviceChanged(deviceId: Int) {
            // A device that is no longer a pad, or that is another device
            // under the same id, starts over.
            val had = slots.deviceOf(deviceId) ?: return
            val device = InputDevice.getDevice(deviceId)
            if (device == null || !isPad(device) || device.descriptor != had.descriptor) drop(deviceId)
        }
    }

    /** Listens for devices from now until [stopListening]. */
    fun listen(context: Context) {
        if (inputManager != null) return
        val manager = context.getSystemService(InputManager::class.java) ?: return
        manager.registerInputDeviceListener(deviceListener, Handler(Looper.getMainLooper()))
        inputManager = manager
    }

    fun stopListening() {
        inputManager?.unregisterInputDeviceListener(deviceListener)
        inputManager = null
    }

    /** A controller's right stick, left and right turned round: the titles' own Invert option is for up and down only. */
    @Volatile
    var invertRightX = false

    fun setEnabled(value: Boolean) {
        if (enabled == value) return
        enabled = value
        NativeBridge.nativePadEnabled(value)
        if (!value) release() else refreshDevices()
    }

    /**
     * Everything let go, or whatever was held stays held forever: the pads
     * are being ignored from now on, or their events go to the app's menu
     * and the game does not see the buttons come up.
     */
    fun release() {
        for (slot in 0 until 4) {
            buttons[slot] = 0
            axes[slot].fill(0f)
            triggers[slot][0] = 0
            triggers[slot][1] = 0
            push(slot)
        }
    }

    fun isEnabled(): Boolean = enabled

    /**
     * Forgets the devices that are not there any more: at start-up, and on
     * coming back to the front, in case a removal was missed meanwhile.
     */
    fun refreshDevices() {
        val present = InputDevice.getDeviceIds().toHashSet()
        for (gone in slots.keepOnly(present)) gone(gone)
    }

    fun hasAnyPad(): Boolean = !slots.isEmpty()

    private fun drop(deviceId: Int) {
        gone(slots.remove(deviceId) ?: return)
    }

    private fun gone(gone: PadSlots.Gone) {
        log("pad: \"${gone.device.name}\" id ${gone.device.id} gone from slot ${gone.slot}")
        if (!gone.slotEmpty) return
        val slot = gone.slot
        buttons[slot] = 0
        axes[slot].fill(0f)
        triggers[slot][0] = 0
        triggers[slot][1] = 0
        push(slot)
        NativeBridge.nativePadConnected(slot, false)
    }

    private fun isPad(device: InputDevice): Boolean = PadSlots.isPad(
        device.isVirtual, device.sources,
        device.hasKeys(KeyEvent.KEYCODE_BUTTON_A)[0],
        device.getMotionRange(MotionEvent.AXIS_X, InputDevice.SOURCE_JOYSTICK) != null,
    )

    /** The slot of the pad that sent an event, given on its first; null for a device that is not a pad. */
    private fun slotFor(deviceId: Int): Int? {
        slots.slotOf(deviceId)?.let { return it }
        val device = InputDevice.getDevice(deviceId) ?: return null
        if (!isPad(device)) return null
        // An id seen for the first time: whatever left meanwhile goes first,
        // so a pad back under a new id finds its old slot free.
        refreshDevices()
        // Before Android 10 there is no telling; such a pad is taken for an external one.
        val builtIn = Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && !device.isExternal
        val entry = PadSlots.Device(deviceId, device.descriptor ?: "id $deviceId", device.name ?: "?", builtIn)
        val (slot, wasEmpty) = slots.take(entry) ?: return null
        log("pad: \"${entry.name}\" id $deviceId vendor ${hex(device.vendorId)}:${hex(device.productId)} " +
            "${if (builtIn) "built-in" else "external"}: slot $slot (player ${slot + 1})")
        if (wasEmpty) NativeBridge.nativePadConnected(slot, true)
        return slot
    }

    private fun hex(value: Int) = "%04x".format(value)

    private fun log(line: String) {
        android.util.Log.i("MW2", line)
        NativeBridge.nativeLog(false, line)
    }

    /** True when the event was a pad's and has been dealt with. */
    fun onKey(event: KeyEvent): Boolean {
        if (!enabled) return false
        val source = event.source
        val fromPad = source and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD ||
            source and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK
        if (!fromPad) return false
        val bit = bitFor(event.keyCode)
        if (bit == 0) return false
        val slot = slotFor(event.deviceId) ?: return false
        when (event.action) {
            KeyEvent.ACTION_DOWN -> buttons[slot] = buttons[slot] or bit
            KeyEvent.ACTION_UP -> buttons[slot] = buttons[slot] and bit.inv()
            else -> return false
        }
        push(slot)
        return true
    }

    fun onMotion(event: MotionEvent): Boolean {
        if (!enabled) return false
        if (event.source and InputDevice.SOURCE_JOYSTICK != InputDevice.SOURCE_JOYSTICK) return false
        if (event.action != MotionEvent.ACTION_MOVE) return false
        val lx = axis(event, MotionEvent.AXIS_X)
        val ly = axis(event, MotionEvent.AXIS_Y)
        val rx = if (has(event, MotionEvent.AXIS_Z)) axis(event, MotionEvent.AXIS_Z)
        else axis(event, MotionEvent.AXIS_RX)
        val ry = if (has(event, MotionEvent.AXIS_RZ)) axis(event, MotionEvent.AXIS_RZ)
        else axis(event, MotionEvent.AXIS_RY)
        val lt = event.getAxisValue(MotionEvent.AXIS_LTRIGGER).takeIf { it > 0f }
            ?: event.getAxisValue(MotionEvent.AXIS_BRAKE)
        val rt = event.getAxisValue(MotionEvent.AXIS_RTRIGGER).takeIf { it > 0f }
            ?: event.getAxisValue(MotionEvent.AXIS_GAS)
        // Some pads report the d-pad as a hat rather than as keys.
        val hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X)
        val hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y)

        // A device with no slot gets one for a press, not for a stick at
        // rest: a pad's first report on connecting is its sticks centred.
        val atRest = lx == 0f && ly == 0f && rx == 0f && ry == 0f && lt <= 0.1f && rt <= 0.1f &&
            abs(hatX) <= 0.5f && abs(hatY) <= 0.5f
        if (atRest && slots.slotOf(event.deviceId) == null) return true
        val slot = slotFor(event.deviceId) ?: return false

        axes[slot][0] = lx
        // The guest's sticks point up; Android's point down.
        axes[slot][1] = -ly
        axes[slot][2] = LookInvert.stickX(rx, invertRightX)
        axes[slot][3] = -ry

        triggers[slot][0] = (lt.coerceIn(0f, 1f) * 255f).toInt()
        triggers[slot][1] = (rt.coerceIn(0f, 1f) * 255f).toInt()

        var bits = buttons[slot] and
            (NativeBridge.UP or NativeBridge.DOWN or NativeBridge.LEFT or NativeBridge.RIGHT).inv()
        if (hatX < -0.5f) bits = bits or NativeBridge.LEFT
        if (hatX > 0.5f) bits = bits or NativeBridge.RIGHT
        if (hatY < -0.5f) bits = bits or NativeBridge.UP
        if (hatY > 0.5f) bits = bits or NativeBridge.DOWN
        buttons[slot] = bits

        push(slot)
        return true
    }

    private fun has(event: MotionEvent, axis: Int): Boolean =
        event.device?.getMotionRange(axis, event.source) != null

    /**
     * One axis, with the device's own flat zone taken out. A worn stick that
     * rests at 0.12 would otherwise walk the player into a wall; the device
     * knows how far it drifts and says so.
     */
    private fun axis(event: MotionEvent, which: Int): Float {
        val range = event.device?.getMotionRange(which, event.source)
        val raw = event.getAxisValue(which)
        val flat = range?.flat ?: 0.05f
        val dead = maxOf(flat, 0.08f)
        if (abs(raw) <= dead) return 0f
        val sign = if (raw < 0) -1f else 1f
        return sign * ((abs(raw) - dead) / (1f - dead)).coerceIn(0f, 1f)
    }

    private fun push(slot: Int) {
        NativeBridge.nativePadState(
            slot, buttons[slot], triggers[slot][0], triggers[slot][1],
            axes[slot][0], axes[slot][1], axes[slot][2], axes[slot][3],
        )
    }

    private fun bitFor(keyCode: Int): Int = when (keyCode) {
        KeyEvent.KEYCODE_BUTTON_A -> NativeBridge.A
        KeyEvent.KEYCODE_BUTTON_B -> NativeBridge.B
        KeyEvent.KEYCODE_BUTTON_X -> NativeBridge.X
        KeyEvent.KEYCODE_BUTTON_Y -> NativeBridge.Y
        KeyEvent.KEYCODE_BUTTON_L1 -> NativeBridge.LB
        KeyEvent.KEYCODE_BUTTON_R1 -> NativeBridge.RB
        KeyEvent.KEYCODE_BUTTON_THUMBL -> NativeBridge.L3
        KeyEvent.KEYCODE_BUTTON_THUMBR -> NativeBridge.R3
        KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_MENU -> NativeBridge.START
        KeyEvent.KEYCODE_BUTTON_SELECT -> NativeBridge.BACK
        KeyEvent.KEYCODE_DPAD_UP -> NativeBridge.UP
        KeyEvent.KEYCODE_DPAD_DOWN -> NativeBridge.DOWN
        KeyEvent.KEYCODE_DPAD_LEFT -> NativeBridge.LEFT
        KeyEvent.KEYCODE_DPAD_RIGHT -> NativeBridge.RIGHT
        else -> 0
    }
}
