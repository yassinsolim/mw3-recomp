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
 * through a pointer. What is appended has to be the script's output to the
 * byte: the file's hash is part of what the game library states
 * (cmake/game_abi.cmake), and the build compares it with the app's.
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

        // The code section, from the image's section table.
        val pe = little.getInt(0x3C)
        val sections = little.getShort(pe + 6).toInt() and 0xFFFF
        val optional = little.getShort(pe + 20).toInt() and 0xFFFF
        var start = 0L
        var size = 0L
        for (index in 0 until sections) {
            val at = pe + 24 + optional + index * 40
            if (String(image, at, 8, Charsets.ISO_8859_1).trimEnd('\u0000') == ".text") {
                size = little.getInt(at + 8).toLong() and 0xFFFFFFFFL
                start = BASE + (little.getInt(at + 12).toLong() and 0xFFFFFFFFL)
            }
        }
        if (size == 0L) throw IOException("${executable.name} has no code section")
        val end = start + size

        val text = tables.readText(Charsets.ISO_8859_1)
        val known = Regex("^    0x([0-9A-F]+),\r?$", RegexOption.MULTILINE).findAll(text).map { it.groupValues[1].toLong(16) }.toHashSet()
        val added = StringBuilder()
        var count = 0
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
                    val index = word(bctr - 12)
                    val load = word(bctr - 8)
                    val move = word(bctr - 4)
                    if ((index and 0xFC1FFFFFL) != 0x5400103AL || (load and 0xFFE007FFL) != 0x7C00002EL || move != 0x7C0903A6L) {
                        throw IOException("an unknown dispatch before the table at %08X".format(bctr + 4))
                    }
                    // The cases follow the table.
                    if (labels.min() < at) throw IOException("the table at %08X runs into its own cases".format(bctr + 4))
                    added.append("\n").append(MARK).append("[[switch]]\nbase = 0x%08X\nr = %d\ndefault = 0x%08X\nlabels = [\n".format(bctr - 12, (index shr 21) and 31, labels[0]))
                    labels.forEach { added.append("    0x%08X,\n".format(it)) }
                    added.append("]\n")
                    count++
                }
            }
            bctr += 4
        }
        if (count > 0) tables.appendText(added.toString(), Charsets.ISO_8859_1)
        return count
    }
}
