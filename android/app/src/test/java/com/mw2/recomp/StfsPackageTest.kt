package com.mw2.recomp

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import java.io.File

/**
 * StfsPackage.file against a package laid out here block by block: a disc's
 * own `nxeart` is the game's and cannot be a test's fixture. The file in it
 * is longer than one hash table covers and its blocks are out of order, so
 * both the tables between the blocks and the chain through them are read.
 */
class StfsPackageTest {
    private val block = 0x1000
    private val perTable = 0xAA

    /** Where data block [number] is: after the header, and after each group's hash table. */
    private fun offset(number: Int) = 0xC000 + (number + if (number >= perTable) number / perTable + 1 else 0) * block

    /** A package of [blocks] data blocks: block 0 the file table, naming one file that is [chain]. */
    private fun pack(name: String, contents: ByteArray, chain: List<Int>, blocks: Int): ByteArray {
        val data = ByteArray(offset(blocks))
        "PIRS".toByteArray().copyInto(data)
        data[0x342] = 0xAD.toByte()                 // header size 0xAD00: one hash table per group
        data[0x37C] = 1                             // the file table: one block, block 0
        val entry = offset(0)
        name.toByteArray().copyInto(data, entry)
        data[entry + 0x28] = name.length.toByte()
        data[entry + 0x29] = chain.size.toByte()
        data[entry + 0x2A] = (chain.size shr 8).toByte()
        data[entry + 0x2F] = chain[0].toByte()
        data[entry + 0x30] = (chain[0] shr 8).toByte()
        data[entry + 0x32] = 0xFF.toByte()          // parent -1: at the root
        data[entry + 0x33] = 0xFF.toByte()
        for (i in 0 until 4) data[entry + 0x34 + i] = (contents.size shr (24 - 8 * i)).toByte()
        chain.forEachIndexed { i, number ->
            contents.copyInto(data, offset(number), i * block, minOf(contents.size, (i + 1) * block))
            val next = chain.getOrElse(i + 1) { 0xFFFFFF }
            val record = offset(number - number % perTable) - block + (number % perTable) * 0x18
            for (b in 0 until 3) data[record + 0x15 + b] = (next shr (16 - 8 * b)).toByte()
        }
        return data
    }

    private val contents = ByteArray(200 * block - 123) { (it * 31 + it / block).toByte() }

    /** Blocks 1 to 200, the last two the wrong way round. */
    private val chain = (1..198).toList() + listOf(200, 199)

    @Test
    fun readsAFileAcrossHashTablesAndOutOfOrder() {
        assertArrayEquals(contents, StfsPackage.file(pack("nxebg.jpg", contents, chain, 201), "nxebg.jpg"))
    }

    @Test
    fun aFileThatIsNotThereIsNull() {
        assertNull(StfsPackage.file(pack("nxeslot.jpg", contents, chain, 201), "nxebg.jpg"))
    }

    @Test
    fun aPackageCutShortIsNull() {
        val whole = pack("nxebg.jpg", contents, chain, 201)
        assertNull(StfsPackage.file(whole.copyOf(whole.size - 3 * block), "nxebg.jpg"))
        assertNull(StfsPackage.file(whole.copyOf(0x400), "nxebg.jpg"))
    }

    @Test
    fun anythingElseIsNull() {
        val other = pack("nxebg.jpg", contents, chain, 201)
        "CON ".toByteArray().copyInto(other)
        assertNull(StfsPackage.file(other, "nxebg.jpg"))
        assertNull(StfsPackage.file(ByteArray(0x20000), "nxebg.jpg"))
    }

    /**
     * A map pack is told by a marker at its root and is read from its file:
     * a folder, and a file of the same kind of name inside it, are not names
     * at the root.
     */
    @Test
    fun namesTheFilesAtTheRootOfAPackageInAFile() {
        val data = pack("dlccompat3.id", contents, chain, 201)
        val folder = offset(0) + 0x40
        "english".toByteArray().copyInto(data, folder)
        data[folder + 0x28] = (0x80 or 7).toByte()
        data[folder + 0x32] = 0xFF.toByte()
        data[folder + 0x33] = 0xFF.toByte()
        val inside = folder + 0x40
        "dlccompat4.id".toByteArray().copyInto(data, inside)
        data[inside + 0x28] = 13
        data[inside + 0x33] = 1                     // parent: the folder, entry 1
        val file = File.createTempFile("package", null)
        try {
            file.writeBytes(data)
            assertEquals(listOf("dlccompat3.id"), StfsPackage.rootFiles(file))
            file.writeBytes(data.copyOf(0x400))
            assertEquals(emptyList<String>(), StfsPackage.rootFiles(file))
        } finally {
            file.delete()
        }
    }
}
