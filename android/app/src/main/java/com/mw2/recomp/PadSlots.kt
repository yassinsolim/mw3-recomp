package com.mw2.recomp

/**
 * Which player each physical controller is. No Android in here, so the rules
 * are tested on their own (PadSlotsTest); GamepadInput feeds it.
 *
 * Slot 0 is the first controller, the one the title always has signed in, and
 * the touch controls are always there. A built-in pad (the Thor's, the
 * Flip's) is that player, however many input devices it shows up as, and so
 * is the first other pad pressed: one person holding one pad must never be the
 * second controller, which the title treats as a player nobody signed in and
 * will not save for. Only a second pad is slot 1, a third slot 2.
 *
 * A slot is given on a pad's first press, never when a device is enumerated:
 * a phone lists devices that have pad bits and are not pads, and one of them
 * listed first used to take slot 0. A Bluetooth pad that sleeps and wakes
 * comes back under a new device id; its descriptor (Android's id for the
 * physical device, the same across reconnects) gets it the slot it had.
 */
class PadSlots {

    /** One input device: Android's id for this connection, its descriptor, its name. */
    data class Device(val id: Int, val descriptor: String, val name: String, val builtIn: Boolean)

    /** A device that went, the slot it had, and whether nothing holds that slot now. */
    data class Gone(val device: Device, val slot: Int, val slotEmpty: Boolean)

    private class Held(val device: Device, val slot: Int)

    private val live = LinkedHashMap<Int, Held>()
    /** The slot each pad's descriptor last had, other than a built-in one's. */
    private val remembered = HashMap<String, Int>()

    fun slotOf(id: Int): Int? = live[id]?.slot

    fun deviceOf(id: Int): Device? = live[id]?.device

    fun isEmpty(): Boolean = live.isEmpty()

    fun occupied(slot: Int): Boolean = live.values.any { it.slot == slot }

    /**
     * The slot for a pad on its press, given one if it has none. The second
     * of the pair says the slot had nobody before (the runtime is told a
     * controller came). Null when four other pads hold the four slots.
     */
    fun take(device: Device): Pair<Int, Boolean>? {
        live[device.id]?.let { return it.slot to false }
        val slot = if (device.builtIn) 0 else {
            fun heldByPad(slot: Int) = live.values.any { !it.device.builtIn && it.slot == slot }
            remembered[device.descriptor]?.takeIf { !heldByPad(it) }
                ?: (0 until SLOTS).firstOrNull { !heldByPad(it) }
                ?: return null
        }
        val wasEmpty = !occupied(slot)
        live[device.id] = Held(device, slot)
        if (!device.builtIn) remembered[device.descriptor] = slot
        return slot to wasEmpty
    }

    /** The device with this id is gone; null when it had no slot. */
    fun remove(id: Int): Gone? {
        val held = live.remove(id) ?: return null
        return Gone(held.device, held.slot, !occupied(held.slot))
    }

    /** Everything whose id is not among the devices there now goes. */
    fun keepOnly(present: Set<Int>): List<Gone> =
        live.keys.filter { it !in present }.mapNotNull { remove(it) }

    companion object {
        const val SLOTS = 4

        /**
         * A controller, not a keyboard, mouse or stylus: a real device with
         * the gamepad or joystick source and an A button or a stick. Android
         * sets the gamepad source on keyboards that merely have gamepad keys.
         */
        fun isPad(virtual: Boolean, sources: Int, hasButtonA: Boolean, hasStick: Boolean): Boolean {
            if (virtual) return false
            val joystick = sources and SOURCE_JOYSTICK == SOURCE_JOYSTICK
            val gamepad = sources and SOURCE_GAMEPAD == SOURCE_GAMEPAD
            return (joystick || gamepad) && (hasButtonA || hasStick)
        }

        // InputDevice's values, here so the tests need no Android.
        const val SOURCE_GAMEPAD = 0x00000401
        const val SOURCE_JOYSTICK = 0x01000010
    }
}
