package com.mw2.recomp

import java.util.Locale

/**
 * The part of a tombstone a bug report needs: the signal, the abort message
 * and the crashing thread's backtrace.
 *
 * ApplicationExitInfo hands a native crash's tombstone over as a protocol
 * buffer (AOSP's debuggerd/proto/tombstone.proto), not as the text logcat
 * shows, and the app carries no protobuf library; the wire format is two
 * kinds of field here (a varint, or a length and that many bytes), so the
 * few fields wanted are read by number and the rest skipped.
 */
object Tombstone {

    private const val kFrames = 32

    /** A message's fields in order: number, then a varint's value or a byte range. */
    private class Fields(private val data: ByteArray, private var at: Int, private val end: Int) {
        var number = 0
        var value = 0L
        var from = 0
        var to = 0

        private fun varint(): Long {
            var result = 0L
            var shift = 0
            while (at < end) {
                val byte = data[at++].toInt()
                result = result or ((byte and 0x7f).toLong() shl shift)
                if ((byte and 0x80) == 0) return result
                shift += 7
                if (shift > 63) break
            }
            throw IllegalStateException("a varint runs off the message")
        }

        fun next(): Boolean {
            if (at >= end) return false
            val key = varint()
            number = (key ushr 3).toInt()
            when ((key and 7).toInt()) {
                0 -> value = varint()
                1 -> at += 8
                5 -> at += 4
                2 -> {
                    val length = varint().toInt()
                    if (length < 0 || at + length > end) throw IllegalStateException("a field runs off the message")
                    from = at
                    to = at + length
                    at = to
                }
                else -> throw IllegalStateException("a wire type no tombstone has")
            }
            return true
        }

        fun text(): String = String(data, from, to - from, Charsets.UTF_8)
        fun inside(): Fields = Fields(data, from, to)
    }

    /** Indented lines, or null for bytes that are not a tombstone. */
    fun read(data: ByteArray): String? = try {
        var tid = 0L
        var signal = ""
        var abort = ""
        val threads = HashMap<Long, String>()
        val top = Fields(data, 0, data.size)
        while (top.next()) {
            when (top.number) {
                6 -> tid = top.value
                10 -> signal = signal(top.inside())
                14 -> abort = top.text()
                16 -> {
                    // map<uint32, Thread>: an entry is { 1: the id, 2: the thread }.
                    val entry = top.inside()
                    var id = 0L
                    var thread = ""
                    while (entry.next()) {
                        if (entry.number == 1) id = entry.value
                        if (entry.number == 2) thread = thread(entry.inside())
                    }
                    threads[id] = thread
                }
            }
        }
        buildString {
            append("    ").append(signal.ifEmpty { "no signal recorded" }).append('\n')
            if (abort.isNotEmpty()) append("    abort message: ").append(abort.take(600)).append('\n')
            append(threads[tid] ?: "    (the crashing thread $tid is not in the tombstone)\n")
        }
    } catch (_: Throwable) {
        null
    }

    private fun signal(fields: Fields): String {
        var number = 0L
        var name = ""
        var code = 0L
        var codeName = ""
        var hasAddress = false
        var address = 0L
        while (fields.next()) {
            when (fields.number) {
                1 -> number = fields.value
                2 -> name = fields.text()
                3 -> code = fields.value
                4 -> codeName = fields.text()
                8 -> hasAddress = fields.value != 0L
                9 -> address = fields.value
            }
        }
        return "signal $number ($name), code $code ($codeName)" +
            if (hasAddress) ", fault address 0x${java.lang.Long.toHexString(address)}" else ""
    }

    private fun thread(fields: Fields): String {
        var id = 0L
        var name = ""
        val frames = ArrayList<String>()
        while (fields.next()) {
            when (fields.number) {
                1 -> id = fields.value
                2 -> name = fields.text()
                4 -> if (frames.size < kFrames) frames.add(frame(fields.inside(), frames.size))
            }
        }
        return buildString {
            append("    thread ").append(id).append(" (").append(name).append("):\n")
            frames.forEach { append(it).append('\n') }
        }
    }

    private fun frame(fields: Fields, index: Int): String {
        var pc = 0L
        var function = ""
        var offset = 0L
        var file = ""
        var buildId = ""
        while (fields.next()) {
            when (fields.number) {
                1 -> pc = fields.value
                4 -> function = fields.text()
                5 -> offset = fields.value
                6 -> file = fields.text()
                8 -> buildId = fields.text()
            }
        }
        return "      #%02d pc %016x  %s".format(Locale.US, index, pc, file.ifEmpty { "<unknown>" }) +
            (if (function.isNotEmpty()) " ($function+$offset)" else "") +
            (if (buildId.isNotEmpty()) " (BuildId: $buildId)" else "")
    }
}
