package com.mw2.recomp

import java.io.ByteArrayOutputStream
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * Tombstone.read against a tombstone written here field by field, with the
 * numbers of AOSP's debuggerd/proto/tombstone.proto: the app reads the
 * system's record without a protobuf library, so the test writes one without
 * it too.
 */
class TombstoneTest {

    private class Message {
        val bytes = ByteArrayOutputStream()

        private fun raw(value: Long) {
            var left = value
            while ((left and 0x7fL.inv()) != 0L) {
                bytes.write(((left and 0x7f) or 0x80).toInt())
                left = left ushr 7
            }
            bytes.write(left.toInt())
        }

        fun varint(field: Int, value: Long) = apply { raw((field shl 3).toLong()); raw(value) }
        fun text(field: Int, value: String) = data(field, value.toByteArray())
        fun message(field: Int, value: Message) = data(field, value.bytes.toByteArray())
        fun fixed64(field: Int) = apply { raw(((field shl 3) or 1).toLong()); bytes.write(ByteArray(8)) }

        private fun data(field: Int, value: ByteArray) = apply {
            raw(((field shl 3) or 2).toLong())
            raw(value.size.toLong())
            bytes.write(value)
        }
    }

    private fun frame(pc: Long, function: String, offset: Long, file: String) = Message()
        .varint(1, pc).varint(2, 0x7000000000L + pc).text(4, function).varint(5, offset)
        .text(6, file).text(8, "89f7d454")

    private fun thread(id: Long, name: String, vararg frames: Message) = Message().apply {
        varint(1, id)
        text(2, name)
        // A register, which is skipped.
        message(3, Message().text(1, "x0").varint(2, 1))
        frames.forEach { message(4, it) }
    }

    private fun tombstone(): ByteArray = Message()
        .varint(1, 1).text(2, "a/fingerprint").varint(5, 9133).varint(6, 9185)
        .text(9, "com.mw2.recomp.dev:game")
        .message(
            10,
            Message().varint(1, 11).text(2, "SIGSEGV").varint(3, 1).text(4, "SEGV_MAPERR")
                .varint(8, 1).varint(9, 0),
        )
        .message(16, Message().varint(1, 9133).message(2, thread(9133, "main", frame(0x10, "other", 4, "/system/lib64/libc.so"))))
        .message(
            16,
            Message().varint(1, 9185).message(
                2,
                thread(
                    9185, "vulkan presente",
                    frame(0xdcd38, "crash::Step(char const*, char const*)", 120, "/data/app/lib/arm64/libmw2mp.so"),
                    frame(0x114a98, "", 0, "/data/app/lib/arm64/libmw2mp.so"),
                ),
            ),
        )
        // A field of another wire type and a mapping, both skipped.
        .fixed64(30)
        .message(17, Message().varint(1, 0x1000).text(7, "/system/lib64/libc.so"))
        .bytes.toByteArray()

    @Test
    fun theCrashingThreadsBacktrace() {
        assertEquals(
            "    signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault address 0x0\n" +
                "    thread 9185 (vulkan presente):\n" +
                "      #00 pc 00000000000dcd38  /data/app/lib/arm64/libmw2mp.so" +
                " (crash::Step(char const*, char const*)+120) (BuildId: 89f7d454)\n" +
                "      #01 pc 0000000000114a98  /data/app/lib/arm64/libmw2mp.so (BuildId: 89f7d454)\n",
            Tombstone.read(tombstone()),
        )
    }

    @Test
    fun anAbortSaysItsMessage() {
        val text = Tombstone.read(
            Message().varint(6, 7)
                .message(10, Message().varint(1, 6).text(2, "SIGABRT").varint(3, -6).text(4, "SI_TKILL"))
                .text(14, "terminating due to uncaught exception")
                .message(16, Message().varint(1, 7).message(2, thread(7, "mw2")))
                .bytes.toByteArray(),
        )
        assertEquals(
            "    signal 6 (SIGABRT), code -6 (SI_TKILL)\n" +
                "    abort message: terminating due to uncaught exception\n" +
                "    thread 7 (mw2):\n",
            text,
        )
    }

    @Test
    fun whatIsNotATombstoneIsRefused() {
        // Text, as an older system's trace is: a length that runs off the end.
        assertNull(Tombstone.read("*** *** *** *** *** *** *** ***\npid: 1".toByteArray()))
        // Cut short in the middle of a field.
        assertNull(Tombstone.read(tombstone().copyOf(40)))
    }
}
