package com.mw2.recomp

import java.security.MessageDigest

/**
 * What Add DLC makes of a file before the game is given it (docs/dlc.md).
 *
 * The game's own answer to a package it cannot use is a notice that an
 * installed content pack is corrupt and has to be deleted, with nothing to
 * say which or why, and only Exit below it. So everything a copy can do to a
 * package is looked for here, and said in a sentence that names the file: a
 * file that is no package, one of another game or of another kind, one whose
 * header or blocks are not what the console hashed, one that stops before
 * its end. A package says all of that about itself: its SHA-1 of its header
 * (0x32C, over everything from the content type on), how many blocks it has
 * (0x395), and a SHA-1 of every block in the hash table before the block's
 * group (tools/stfs.py has the layout; only the read-only kind, LIVE and
 * PIRS, is a downloaded package). Its signature is the console maker's and
 * is not checked: no key of theirs is in the app.
 *
 * The runtime checks the header and the length again at every start
 * (runtime/kernel/volume.cpp), and gives each executable the packages it
 * can use (runtime/kernel/content_route.h). The blocks are checked here
 * alone, as the file is copied: it is read once anyway.
 */
object PackageCheck {
    private const val BLOCK = 0x1000
    private const val PER_TABLE = 0xAA
    private const val PER_LEVEL1 = 0x70E4
    private const val FIRST_BLOCK = 0xC000L
    private const val TABLE_ENTRY = 0x18
    private const val DIGEST = 20

    /** Where the header ends and the first hash table begins: what [header] is given. */
    const val HEADER = 0xB000

    private const val CONTENT = 0x00000002
    private const val UPDATE = 0x000B0000

    /** What is wrong with a file, each with its own sentence (strings.xml, dlc_check_*). */
    enum class Problem { NOT_A_PACKAGE, OTHER_GAME, OTHER_KIND, DAMAGED, INCOMPLETE }

    /**
     * A file's own account of itself: [problem] null for a whole package of
     * this game. [name] is what the console shows, [bytes] what the package
     * says it is long and [titleId] whose it is, where it got that far.
     */
    class Header(
        val problem: Problem?, val name: String = "", val update: Boolean = false, val version: Int = 0,
        val blocks: Int = 0, val bytes: Long = 0, val titleId: Int = 0,
    )

    /** Where data block [number] is in the file: the hash tables sit between them. */
    fun offset(number: Int): Long {
        var tables = 0
        if (number >= PER_TABLE) tables += number / PER_TABLE + 1
        if (number >= PER_LEVEL1) tables += number / PER_LEVEL1 + 1
        return FIRST_BLOCK + (number + tables).toLong() * BLOCK
    }

    private fun word(bytes: ByteArray, at: Int) = (0 until 4).fold(0) { value, i -> (value shl 8) or (bytes[at + i].toInt() and 0xFF) }

    /**
     * Reads [head], the file's first [HEADER] bytes or all of a shorter
     * file, of a file [size] bytes long (0 when its length is not known):
     * for the game [titleId], which takes its title update's package or not.
     */
    fun header(head: ByteArray, size: Long, titleId: Int, takesUpdate: Boolean): Header {
        if (head.size < 4) return Header(Problem.NOT_A_PACKAGE)
        val magic = String(head, 0, 4, Charsets.ISO_8859_1)
        if (magic != "LIVE" && magic != "PIRS") return Header(Problem.NOT_A_PACKAGE)
        // It began as a package and stops inside its own header.
        if (head.size < HEADER) return Header(Problem.INCOMPLETE)
        // One hash table per group is the read-only layout; a console-signed
        // package is a save or a profile and no content.
        if (((word(head, 0x340) + 0xFFF) and 0xF000) shr 12 != 0xB) return Header(Problem.NOT_A_PACKAGE)
        val name = String(head, 0x411, 0x100, Charsets.UTF_16BE).substringBefore('\u0000')
        val hash = MessageDigest.getInstance("SHA-1").apply { update(head, 0x344, HEADER - 0x344) }.digest()
        if (!hash.contentEquals(head.copyOfRange(0x32C, 0x32C + DIGEST))) return Header(Problem.DAMAGED, name)
        val type = word(head, 0x344)
        val title = word(head, 0x360)
        if (title != titleId) return Header(Problem.OTHER_GAME, name, titleId = title)
        if (type != CONTENT && !(type == UPDATE && takesUpdate)) return Header(Problem.OTHER_KIND, name, titleId = title)
        val blocks = word(head, 0x395)
        val bytes = if (blocks > 0) offset(blocks - 1) + BLOCK else HEADER.toLong()
        val problem = if (size in 1 until bytes) Problem.INCOMPLETE else null
        return Header(problem, name, type == UPDATE, word(head, 0x358), blocks, bytes, title)
    }

    /**
     * A package's blocks against the hashes it keeps of them, as the file
     * goes by from its first byte: each group's hash table is the block
     * before the group, so one pass is enough. [whole] says, at the end,
     * whether every block was there and was what its hash says.
     */
    class Blocks(private val blocks: Int) {
        private val digest = MessageDigest.getInstance("SHA-1")
        private val block = ByteArray(BLOCK)
        private var filled = 0
        private var at = 0L                 // where the block being filled starts in the file
        private var next = 0                // the data block expected next
        private var table = ByteArray(0)    // the hash table of its group
        private var tableAt = offset(0) - BLOCK
        private var bad = false

        fun feed(bytes: ByteArray, offset: Int, length: Int) {
            var from = offset
            val end = offset + length
            while (from < end) {
                val take = minOf(end - from, BLOCK - filled)
                System.arraycopy(bytes, from, block, filled, take)
                filled += take
                from += take
                if (filled == BLOCK) {
                    took(block)
                    at += BLOCK
                    filled = 0
                }
            }
        }

        /** One block of the file, as it lies at [at]: a group's hash table, a data block, or neither. */
        private fun took(bytes: ByteArray) {
            if (next >= blocks) return
            if (at == tableAt) {
                table = bytes.copyOf()
            } else if (at == offset(next)) {
                val entry = (next % PER_TABLE) * TABLE_ENTRY
                val hash = digest.digest(bytes)
                if (table.size != BLOCK || (0 until DIGEST).any { hash[it] != table[entry + it] }) bad = true
                next++
                if (next % PER_TABLE == 0) tableAt = offset(next) - BLOCK
            }
        }

        /** Whether the file that went by was the whole package, undamaged. */
        val whole: Boolean
            get() = !bad && next == blocks
    }
}
