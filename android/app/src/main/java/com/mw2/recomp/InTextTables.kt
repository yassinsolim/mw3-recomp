package com.mw2.recomp

import java.io.File
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import javax.crypto.Cipher
import javax.crypto.spec.IvParameterSpec
import javax.crypto.spec.SecretKeySpec

/**
 * The jump tables XenonAnalyse does not recognise, added to a title's switch
 * tables: tools/intext_switch_tables.py, for a build on the device
 * (GameBuild.kt). Call of Duty 4's compiler also writes a dispatch as
 *
 *     rlwinm r0,rN,2,0,29     the index, times four
 *     lwzx   r0,r12,r0        the case's address, from the table
 *     mtctr  r0
 *     bctr
 *     .long  case, case, ...  the table, in the code, right after the bctr
 *
 * and without an entry for it the recompiler takes the bctr for a call
 * through a pointer. Title update 17 of Advanced Warfare's multiplayer also
 * has a table of halfwords with no guard before it, which the analyser
 * passes over for want of a length. What is appended has to be the script's
 * output to the byte: the file's hash is part of what the game library
 * states (cmake/game_abi.cmake), and the build compares it with the app's.
 */
internal object InTextTables {
    /** Where every title's image is loaded (tools/title.py, BASE). */
    private const val BASE = 0x82000000L
    private const val MARK = "# ---- IN-TEXT JUMPTABLE (tools/intext_switch_tables.py) ----\n"

    private fun decrypt(key: ByteArray, data: ByteArray): ByteArray {
        val cipher = Cipher.getInstance("AES/CBC/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE, SecretKeySpec(key, "AES"), IvParameterSpec(ByteArray(16)))
        return cipher.doFinal(data)
    }

    /**
     * The executable's image as it is in the console's memory: decrypted with
     * the key every retail executable is wrapped in, and its stored blocks
     * laid out with the zeroes between them (tools/xexdump.py, load).
     */
    private fun image(executable: File): ByteArray {
        val file = executable.readBytes()
        val header = ByteBuffer.wrap(file).order(ByteOrder.BIG_ENDIAN)
        if (file.size < 0x18 || String(file, 0, 4, Charsets.ISO_8859_1) != "XEX2") throw IOException("${executable.name} is not an Xbox 360 executable")
        val headerSize = header.getInt(8)
        val security = header.getInt(0x10)
        var format = -1
        for (index in 0 until header.getInt(0x14)) {
            if (header.getInt(0x18 + index * 8) == 0x3FF) format = header.getInt(0x18 + index * 8 + 4)
        }
        if (format < 0) throw IOException("${executable.name} does not say how it is stored")
        val formatSize = header.getInt(format)
        val encryption = header.getShort(format + 4).toInt()
        val compression = header.getShort(format + 6).toInt()
        var body = file.copyOfRange(headerSize, file.size)
        if (encryption == 1) {
            val retail = byteArrayOf(
                0x20, 0xB1.toByte(), 0x85.toByte(), 0xA5.toByte(), 0x9D.toByte(), 0x28, 0xFD.toByte(), 0xC3.toByte(),
                0x40, 0x58, 0x3F, 0xBB.toByte(), 0x08, 0x96.toByte(), 0xBF.toByte(), 0x91.toByte(),
            )
            val session = decrypt(retail, file.copyOfRange(security + 336, security + 352))
            // Whole blocks only: the last one is padded with zeroes.
            body = decrypt(session, body.copyOf((body.size + 15) / 16 * 16))
        }
        if (compression == 0) return body
        if (compression != 1) throw IOException("${executable.name} is compressed in a way this build cannot read")
        val out = java.io.ByteArrayOutputStream(body.size * 2)
        var at = 0
        for (block in 0 until (formatSize - 8) / 8) {
            val data = header.getInt(format + 8 + block * 8)
            val zeroes = header.getInt(format + 12 + block * 8)
            out.write(body, at, data)
            at += data
            out.write(ByteArray(zeroes))
        }
        return out.toByteArray()
    }

    /** Appends an entry for every such table [tables] has not got. Returns how many. */
    fun add(executable: File, tables: File): Int {
        val image = image(executable)
        val big = ByteBuffer.wrap(image).order(ByteOrder.BIG_ENDIAN)
        val little = ByteBuffer.wrap(image).order(ByteOrder.LITTLE_ENDIAN)
        fun word(address: Long): Long = big.getInt((address - BASE).toInt()).toLong() and 0xFFFFFFFFL

        // Every section that holds instructions, from the image's section
        // table: .text, and the ones of libraries that bring their own
        // (Advanced Warfare's Bink has four such tables).
        val pe = little.getInt(0x3C)
        val sections = little.getShort(pe + 6).toInt() and 0xFFFF
        val optional = little.getShort(pe + 20).toInt() and 0xFFFF
        val code = ArrayList<Pair<Long, Long>>()
        // Every function .pdata describes, by its start: its end (tools/title.py, functions).
        val functions = java.util.TreeMap<Long, Long>()
        for (index in 0 until sections) {
            val at = pe + 24 + optional + index * 40
            if (little.getInt(at + 36) and 0x20 != 0) {     // IMAGE_SCN_CNT_CODE
                val start = BASE + (little.getInt(at + 12).toLong() and 0xFFFFFFFFL)
                code.add(start to start + (little.getInt(at + 8).toLong() and 0xFFFFFFFFL))
            }
            if (String(image, at, 8, Charsets.ISO_8859_1).trimEnd('\u0000') == ".pdata") {
                val table = little.getInt(at + 12)
                for (entry in 0 until little.getInt(at + 8) / 8) {
                    val start = big.getInt(table + entry * 8).toLong() and 0xFFFFFFFFL
                    if (start == 0L) break
                    functions[start] = start + ((big.getInt(table + entry * 8 + 4) ushr 8) and 0x3FFFFF) * 4L
                }
            }
        }
        if (code.isEmpty()) throw IOException("${executable.name} has no code section")

        val text = tables.readText(Charsets.ISO_8859_1)
        val known = Regex("^    0x([0-9A-F]+),\r?$", RegexOption.MULTILINE).findAll(text).map { it.groupValues[1].toLong(16) }.toHashSet()
        val bases = Regex("^base = 0x([0-9A-F]+)\r?$", RegexOption.MULTILINE).findAll(text).map { it.groupValues[1].toLong(16) }.toHashSet()
        val added = StringBuilder()
        var count = 0
        fun entry(base: Long, register: Long, labels: List<Long>) {
            added.append("\n").append(MARK).append("[[switch]]\nbase = 0x%08X\nr = %d\ndefault = 0x%08X\nlabels = [\n".format(base, register, labels[0]))
            labels.forEach { added.append("    0x%08X,\n".format(it)) }
            added.append("]\n")
            count++
        }
        for ((start, end) in code) {
            var bctr = start + 12
            while (bctr < end - 8) {
                if (word(bctr) == 0x4E800420L) {
                    val labels = ArrayList<Long>()
                    var at = bctr + 4
                    // A case is an instruction of the same function: within 16 KB of the dispatch.
                    while (at < end) {
                        val label = word(at)
                        if (label < start || label >= end || label % 4 != 0L || Math.abs(label - bctr) >= 0x4000) break
                        labels.add(label)
                        at += 4
                    }
                    if (labels.size >= 2 && !known.containsAll(labels)) {
                        var base = bctr - 12
                        var index = word(base)
                        val load = word(bctr - 8)
                        val move = word(bctr - 4)
                        // addi r12,r12,N between the two: the index is one further back.
                        if ((index and 0xFFFF0000L) == 0x398C0000L) {
                            base -= 4
                            index = word(base)
                        }
                        if ((index and 0xFC1FFFFFL) != 0x5400103AL || (load and 0xFFE007FFL) != 0x7C00002EL || move != 0x7C0903A6L) {
                            throw IOException("an unknown dispatch before the table at %08X".format(bctr + 4))
                        }
                        // The cases follow the table.
                        if (labels.min() < at) throw IOException("the table at %08X runs into its own cases".format(bctr + 4))
                        entry(base, (index shr 21) and 31, labels)
                    }
                }
                bctr += 4
            }
        }
        // A table of halfwords with no guard before it (826B46E8 there: the
        // caller's index is trusted):
        //     lis r12 / slwi r0,rN,1 / addi r12,r12,lo / lhzx r0,r12,r0    the case's distance, from the table
        //     lis r12 / addi r12,r12,lo / nop / add r12,r12,r0 / mtctr r12 / bctr
        // The cases are taken while they are instructions of the function
        // after the dispatch.
        for ((first, end) in code) {
            var bctr = first + 36
            while (bctr < end - 8) {
                val start = bctr - 36
                val high = word(start)
                val index = word(start + 4)
                val low = word(start + 8)
                val high2 = word(start + 16)
                val low2 = word(start + 20)
                if (word(bctr) == 0x4E800420L && word(bctr - 4) == 0x7D8903A6L && word(bctr - 8) == 0x7D8C0214L &&
                    word(bctr - 12) == 0x60000000L && (high and 0xFFFF0000L) == 0x3D800000L &&
                    (index and 0xFC1FFFFFL) == 0x5400083CL && (low and 0xFFFF0000L) == 0x398C0000L &&
                    word(start + 12) == 0x7C0C022EL && (high2 and 0xFFFF0000L) == 0x3D800000L &&
                    (low2 and 0xFFFF0000L) == 0x398C0000L && start !in bases
                ) {
                    fun address(upper: Long, lower: Long) = ((upper and 0xFFFF) shl 16) + (lower and 0xFFFF).toShort()
                    val table = address(high, low)
                    val origin = address(high2, low2)
                    val within = functions.floorEntry(bctr)?.value?.takeIf { bctr < it }
                        ?: throw IOException("the unguarded table's dispatch at %08X is in no function .pdata describes".format(start))
                    val labels = ArrayList<Long>()
                    while (true) {
                        val label = origin + (big.getShort((table + 2 * labels.size - BASE).toInt()).toLong() and 0xFFFF)
                        if (label % 4 != 0L || label <= bctr || label >= within) break
                        labels.add(label)
                    }
                    if (labels.size < 2) throw IOException("the unguarded table at %08X has no cases in its function".format(table))
                    entry(start, (index shr 21) and 31, labels)
                }
                bctr += 4
            }
        }
        if (count > 0) tables.appendText(added.toString(), Charsets.ISO_8859_1)
        return count
    }
}
