package com.mw2.recomp

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.security.MessageDigest

/**
 * PackageCheck against packages made here, header and hashes and all: a
 * console's own are the player's and no test's fixture. One case for each
 * thing a copied folder was seen or expected to hold: a whole package, a
 * file that is none, another game's, another kind, a damaged header, a
 * damaged block, a copy that stopped part-way, a title update.
 */
class PackageCheckTest {
    private val block = 0x1000
    private val game = 0x41560914
    private val content = 2
    private val update = 0x000B0000

    private fun put(data: ByteArray, at: Int, value: Int) {
        for (i in 0 until 4) data[at + i] = (value shr (24 - 8 * i)).toByte()
    }

    private fun sha1(data: ByteArray, from: Int, length: Int): ByteArray =
        MessageDigest.getInstance("SHA-1").apply { update(data, from, length) }.digest()

    /** The header's own hash, as the console writes it last. */
    private fun seal(data: ByteArray) = sha1(data, 0x344, PackageCheck.HEADER - 0x344).copyInto(data, 0x32C)

    /** A package of [blocks] data blocks of [title], each block hashed in its group's table. */
    private fun pack(blocks: Int, title: Int = game, type: Int = content, version: Int = 0, name: String = "Compatibility Pack 3"): ByteArray {
        val data = ByteArray(PackageCheck.offset(blocks - 1).toInt() + block)
        "LIVE".toByteArray().copyInto(data)
        put(data, 0x340, 0xAD0E)
        put(data, 0x344, type)
        put(data, 0x358, version)
        put(data, 0x360, title)
        put(data, 0x395, blocks)
        name.toByteArray(Charsets.UTF_16BE).copyInto(data, 0x411)
        for (number in 0 until blocks) {
            val at = PackageCheck.offset(number).toInt()
            for (i in 0 until block) data[at + i] = (number * 7 + i).toByte()
            val table = PackageCheck.offset(number - number % 0xAA).toInt() - block
            sha1(data, at, block).copyInto(data, table + (number % 0xAA) * 0x18)
        }
        seal(data)
        return data
    }

    private fun header(data: ByteArray, size: Long = data.size.toLong(), title: Int = game, takesUpdate: Boolean = true) =
        PackageCheck.header(data.copyOf(minOf(data.size, PackageCheck.HEADER)), size, title, takesUpdate)

    /** Whether [data], handed over in pieces of [piece] bytes as a copy reads them, is the whole package its header says. */
    private fun blocksWhole(data: ByteArray, piece: Int = 70_001): Boolean {
        val blocks = PackageCheck.Blocks(header(data, 0).blocks)
        var at = 0
        while (at < data.size) {
            val length = minOf(piece, data.size - at)
            blocks.feed(data, at, length)
            at += length
        }
        return blocks.whole
    }

    @Test
    fun aWholePackageIsTaken() {
        // Past the first hash table's 170 blocks, so a second table is read.
        val data = pack(400, version = 5)
        val found = header(data)
        assertNull(found.problem)
        assertEquals("Compatibility Pack 3", found.name)
        assertFalse(found.update)
        assertEquals(5, found.version)
        assertEquals(400, found.blocks)
        assertEquals(data.size.toLong(), found.bytes)
        assertTrue(blocksWhole(data))
        assertTrue(blocksWhole(data, piece = block))
        assertTrue(blocksWhole(data, piece = 1 shl 20))
    }

    @Test
    fun aTitleUpdateIsTakenByTheGameThatPlaysIt() {
        val data = pack(3, type = update, name = "Advanced Warfare Title Update")
        assertTrue(header(data).update)
        assertNull(header(data).problem)
        assertEquals(PackageCheck.Problem.OTHER_KIND, header(data, takesUpdate = false).problem)
    }

    @Test
    fun aFileThatIsNoPackage() {
        assertEquals(PackageCheck.Problem.NOT_A_PACKAGE, header(ByteArray(0x20000)).problem)
        assertEquals(PackageCheck.Problem.NOT_A_PACKAGE, header(ByteArray(2)).problem)
        // A save or a profile: signed by the console, with two hash tables a group.
        val save = pack(3)
        "CON ".toByteArray().copyInto(save)
        assertEquals(PackageCheck.Problem.NOT_A_PACKAGE, header(save).problem)
        val layout = pack(3)
        put(layout, 0x340, 0x971A)
        seal(layout)
        assertEquals(PackageCheck.Problem.NOT_A_PACKAGE, header(layout).problem)
    }

    @Test
    fun anotherGamesPackageSaysWhose() {
        val found = header(pack(3, title = 0x415608CB.toInt(), name = "Collection 1"))
        assertEquals(PackageCheck.Problem.OTHER_GAME, found.problem)
        assertEquals(0x415608CB.toInt(), found.titleId)
        assertEquals("Collection 1", found.name)
    }

    @Test
    fun anotherKindOfContent() {
        // A saved game of this game, an avatar item, a theme.
        for (type in listOf(1, 0x9000, 0x30000)) {
            assertEquals(PackageCheck.Problem.OTHER_KIND, header(pack(3, type = type)).problem)
        }
    }

    @Test
    fun aDamagedHeaderIsToldByItsOwnHash() {
        val data = pack(3)
        data[0x420] = (data[0x420] + 1).toByte()
        assertEquals(PackageCheck.Problem.DAMAGED, header(data).problem)
        // Another game's package edited to pass for this one's.
        val edited = pack(3, title = 0x415608CB.toInt())
        put(edited, 0x360, game)
        assertEquals(PackageCheck.Problem.DAMAGED, header(edited).problem)
    }

    @Test
    fun aDamagedBlockIsToldByItsHash() {
        for (number in listOf(0, 169, 170, 399)) {
            val data = pack(400)
            val at = PackageCheck.offset(number).toInt() + 17
            data[at] = (data[at] + 1).toByte()
            assertNull(header(data).problem)
            assertFalse(blocksWhole(data))
        }
        // A hash table that is damaged damages what it vouches for.
        val data = pack(400)
        data[PackageCheck.offset(170).toInt() - block + 3] = 0x55
        assertFalse(blocksWhole(data))
    }

    @Test
    fun aCopyThatStoppedPartWay() {
        val data = pack(400)
        // By its length, where the length is known before it is read,
        assertEquals(PackageCheck.Problem.INCOMPLETE, header(data, size = data.size - 1L).problem)
        assertEquals(PackageCheck.Problem.INCOMPLETE, header(data.copyOf(0x4000)).problem)
        // and by its blocks where it is not (a document that tells no size).
        assertNull(header(data, size = 0).problem)
        assertFalse(blocksWhole(data.copyOf(data.size - block)))
        assertFalse(blocksWhole(data.copyOf(PackageCheck.offset(200).toInt())))
    }
}
