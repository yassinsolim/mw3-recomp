package com.mw2.recomp

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.io.FileNotFoundException
import java.io.IOException
import java.io.OutputStream
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.channels.FileChannel
import java.security.MessageDigest

/**
 * Why a disc image is refused, kind by kind (DiscRefusal): an image that is
 * gone, one whose permission is, one that fails while it is read, a storage
 * that is full, a file that is no disc image of the game, a disc of a version
 * this app has no game code for, and an executable that has the supported
 * version but not the disc's bytes. The images are made here, a small disc
 * (XDVDFS) with the executables at its root: a console's own are the
 * player's and no test's fixture.
 */
class DiscRefusalTest {
    @get:Rule
    val folder = TemporaryFolder()

    private val sector = 2048

    /** An executable's header as far as its version: the execution info says { media id, version, base version, title id }. */
    private fun xex(version: Int, padding: Int = 0): ByteArray {
        val data = ByteBuffer.allocate(0x60 + padding).order(ByteOrder.BIG_ENDIAN)
        data.putInt(0, 0x58455832) // "XEX2"
        data.putInt(20, 1) // one optional header
        data.putInt(24, 0x00040006) // execution info
        data.putInt(28, 0x40) // where it is
        data.putInt(0x40, 0x2C8C0267) // media id
        data.putInt(0x44, version)
        data.putInt(0x48, version) // base version
        data.putInt(0x4C, 0x41560817) // title id
        return data.array()
    }

    private fun sha256(data: ByteArray): String =
        MessageDigest.getInstance("SHA-256").digest(data).joinToString("") { "%02x".format(it) }

    private fun put(image: ByteArray, at: Int, value: Int) {
        for (i in 0 until 4) image[at + i] = (value shr (8 * i)).toByte()
    }

    private fun put16(image: ByteArray, at: Int, value: Int) {
        image[at] = value.toByte()
        image[at + 1] = (value shr 8).toByte()
    }

    /** A disc image with [files] at its root. */
    private fun disc(vararg files: Pair<String, ByteArray>): File {
        val rootSector = 40
        val firstData = 48
        val image = ByteArray((firstData + files.sumOf { (it.second.size + sector - 1) / sector } + 1) * sector)
        DiscImage.MAGIC.copyInto(image, 32 * sector)
        put(image, 32 * sector + DiscImage.MAGIC.size, rootSector)
        put(image, 32 * sector + DiscImage.MAGIC.size + 4, sector)
        var at = 0
        var next = firstData
        files.forEachIndexed { index, (name, content) ->
            val length = 14 + name.length
            val width = (length + 3) / 4 * 4
            val last = index == files.size - 1
            val entry = rootSector * sector + at
            put16(image, entry, 0)
            put16(image, entry + 2, if (last) 0 else (at + width) / 4)
            put(image, entry + 4, next)
            put(image, entry + 8, content.size)
            image[entry + 12] = 0x80.toByte()
            image[entry + 13] = name.length.toByte()
            name.toByteArray(Charsets.ISO_8859_1).copyInto(image, entry + 14)
            content.copyInto(image, next * sector)
            next += (content.size + sector - 1) / sector
            at += width
        }
        return folder.newFile().also { it.writeBytes(image) }
    }

    private fun channel(file: File): FileChannel = RandomAccessFile(file, "r").channel

    private fun title(content: ByteArray, versions: List<Long>, name: String = "default.xex") =
        GameBuild.FromDisc("tu0", name, listOf(sha256(content)), versions)

    private fun kept() = folder.newFolder()

    /** What taking [titles] from [image] throws; fails when it throws nothing. */
    private fun refusal(
        image: File,
        titles: List<GameBuild.FromDisc>,
        into: File = kept(),
        open: (File) -> OutputStream = { it.outputStream() },
    ): DiscRefusal {
        try {
            channel(image).use { GameBuild.takeAll(it, titles, open) { title -> File(into, title.executable) } }
        } catch (refusal: DiscRefusal) {
            return refusal
        }
        fail("the image was taken")
        throw IllegalStateException()
    }

    // ---- what was taken -------------------------------------------------------

    @Test
    fun aSupportedDiscIsTakenAndNothingIsLeftBeside() {
        val game = xex(10)
        val multiplayer = xex(10, 16)
        val image = disc("default.xex" to game, "default_mp.xex" to multiplayer)
        val into = kept()
        val titles = listOf(title(game, listOf(10)), title(multiplayer, listOf(10), "default_mp.xex"))
        channel(image).use { GameBuild.takeAll(it, titles) { title -> File(into, title.executable) } }
        assertEquals(sha256(game), sha256(File(into, "default.xex").readBytes()))
        assertEquals(sha256(multiplayer), sha256(File(into, "default_mp.xex").readBytes()))
        assertEquals(setOf("default.xex", "default_mp.xex"), into.list().orEmpty().toSet())
    }

    // ---- an image that cannot be read -----------------------------------------

    @Test
    fun anImageThatIsGoneIsMissing() {
        val gone = FileNotFoundException("/storage/emulated/0/Download/mw2.iso: open failed: ENOENT (No such file or directory)")
        assertEquals(DiscRefusal.Reason.MISSING, DiscRefusal.unreadable(gone).reason)
    }

    @Test
    fun aDocumentTheProviderHasNoFileForIsMissing() {
        assertEquals(DiscRefusal.Reason.MISSING, DiscRefusal.unreadable(FileNotFoundException("the provider has no file")).reason)
    }

    @Test
    fun anImageThePermissionIsGoneForSaysSoWhetherTheSystemThrowsOrWords() {
        val revoked = SecurityException("Permission Denial: opening provider com.android.externalstorage.documents")
        assertEquals(DiscRefusal.Reason.PERMISSION, DiscRefusal.unreadable(revoked).reason)
        // A file the app may no longer open is a FileNotFoundException that says so; not mistaken for one that is gone.
        val denied = FileNotFoundException("/storage/0000-0000/mw2.iso: open failed: EACCES (Permission denied)")
        assertEquals(DiscRefusal.Reason.PERMISSION, DiscRefusal.unreadable(denied).reason)
    }

    @Test
    fun aFailureWhileReadingIsNotTakenForAMissingFile() {
        val failing = IOException("pread failed: EIO (I/O error)")
        val refusal = DiscRefusal.unreadable(failing)
        assertEquals(DiscRefusal.Reason.IO, refusal.reason)
        assertTrue(refusal.cause === failing)
    }

    @Test
    fun aCloudDocumentThatCannotBeReadAtPositionsIsToldApart() {
        assertEquals(DiscRefusal.Reason.NOT_SEEKABLE, DiscRefusal.unreadable(IOException("pread failed: ESPIPE (Illegal seek)")).reason)
    }

    @Test
    fun anImageThatFailsWhileItsHeaderIsReadIsUnreadableAndNoDiscImage() {
        val image = disc("default.xex" to xex(10))
        val closed = channel(image).also { it.close() }
        val refusal = try {
            DiscImage(closed)
            fail("the closed image was read")
            return
        } catch (refusal: DiscRefusal) {
            refusal
        }
        assertTrue(refusal is DiscRefusal.Unreadable)
        assertEquals(DiscRefusal.Reason.IO, (refusal as DiscRefusal.Unreadable).reason)
    }

    @Test
    fun anImageThatFailsWhileAnExecutableIsCopiedIsUnreadable() {
        val game = xex(10)
        val image = disc("default.xex" to game)
        val source = channel(image)
        // The image reads as far as its folder; where the executable is, the card fails.
        val failing = object : FileChannel() {
            override fun read(dst: ByteBuffer, position: Long): Int {
                if (position >= 48L * sector) throw IOException("pread failed: EIO (I/O error)")
                return source.read(dst, position)
            }
            override fun read(dst: ByteBuffer) = source.read(dst)
            override fun read(dsts: Array<ByteBuffer>, offset: Int, length: Int) = source.read(dsts, offset, length)
            override fun write(src: ByteBuffer) = throw UnsupportedOperationException()
            override fun write(srcs: Array<ByteBuffer>, offset: Int, length: Int) = throw UnsupportedOperationException()
            override fun position() = source.position()
            override fun position(newPosition: Long): FileChannel {
                source.position(newPosition)
                return this
            }
            override fun size() = source.size()
            override fun truncate(size: Long) = throw UnsupportedOperationException()
            override fun force(metaData: Boolean) = Unit
            override fun transferTo(position: Long, count: Long, target: java.nio.channels.WritableByteChannel) = 0L
            override fun transferFrom(src: java.nio.channels.ReadableByteChannel, position: Long, count: Long) = 0L
            override fun write(src: ByteBuffer, position: Long) = throw UnsupportedOperationException()
            override fun map(mode: MapMode, position: Long, size: Long) = throw UnsupportedOperationException()
            override fun lock(position: Long, size: Long, shared: Boolean) = throw UnsupportedOperationException()
            override fun tryLock(position: Long, size: Long, shared: Boolean) = throw UnsupportedOperationException()
            override fun implCloseChannel() = source.close()
        }
        val into = kept()
        val refusal = try {
            failing.use { GameBuild.takeAll(it, listOf(title(game, listOf(10)))) { title -> File(into, title.executable) } }
            fail("the image was taken")
            return
        } catch (refusal: DiscRefusal) {
            refusal
        }
        assertEquals(DiscRefusal.Reason.IO, (refusal as DiscRefusal.Unreadable).reason)
        assertEquals(emptyList<String>(), into.list().orEmpty().toList())
    }

    // ---- the storage is full --------------------------------------------------

    @Test
    fun aFullStorageSaysHowMuchTheExecutablesNeed() {
        val game = xex(10, 3000)
        val multiplayer = xex(10, 5000)
        val image = disc("default.xex" to game, "default_mp.xex" to multiplayer)
        val titles = listOf(title(game, listOf(10)), title(multiplayer, listOf(10), "default_mp.xex"))
        val into = kept()
        val full = refusal(image, titles, into) { throw IOException("write failed: ENOSPC (No space left on device)") }
        assertTrue(full is DiscRefusal.NoSpace)
        assertEquals((game.size + multiplayer.size).toLong(), (full as DiscRefusal.NoSpace).needed)
        assertEquals(emptyList<String>(), into.list().orEmpty().toList())
    }

    @Test
    fun aWriteThatFailsOtherwiseIsNotCalledAFullStorage() {
        val game = xex(10)
        val image = disc("default.xex" to game)
        try {
            channel(image).use {
                GameBuild.takeAll(it, listOf(title(game, listOf(10))), { throw IOException("Read-only file system") }) { t -> File(kept(), t.executable) }
            }
            fail("the image was taken")
        } catch (error: DiscRefusal) {
            fail("a read-only store was told as $error")
        } catch (error: IOException) {
            assertEquals("Read-only file system", error.message)
        }
        assertFalse(DiscRefusal.isNoSpace(IOException("Read-only file system")))
        assertTrue(DiscRefusal.isNoSpace(IOException("write failed", IOException("ENOSPC (No space left on device)"))))
    }

    // ---- not a disc image of this game ----------------------------------------

    @Test
    fun aFileThatIsNoDiscImageIsNotThisGame() {
        val notes = folder.newFile().also { it.writeBytes("not an Xbox 360 disc, only some text".toByteArray()) }
        assertTrue(refusal(notes, listOf(title(xex(10), listOf(10)))) is DiscRefusal.NotThisGame)
    }

    @Test
    fun aDiscWithoutTheExecutableIsNotThisGame() {
        val image = disc("default.xex" to xex(10))
        val missing = refusal(image, listOf(title(xex(10), listOf(10), "default_mp.xex")))
        assertTrue(missing is DiscRefusal.NotThisGame)
        assertTrue(missing.message!!.contains("default_mp.xex"))
    }

    @Test
    fun anExecutableThatIsNoExecutableIsNotThisGame() {
        val image = disc("default.xex" to "this is not an executable at all".toByteArray())
        assertTrue(refusal(image, listOf(title(xex(10), listOf(10)))) is DiscRefusal.NotThisGame)
    }

    // ---- the version ----------------------------------------------------------

    @Test
    fun aDiscOfAnotherVersionNamesTheVersionFoundAndTheOneSupported() {
        val image = disc("default.xex" to xex(21))
        val refused = refusal(image, listOf(title(xex(10), listOf(10))))
        assertTrue(refused is DiscRefusal.UnsupportedVersion)
        refused as DiscRefusal.UnsupportedVersion
        assertEquals("default.xex", refused.executable)
        assertEquals(21L, refused.found)
        assertEquals(listOf(10L), refused.supported)
        assertTrue(refused.message!!.contains("21") && refused.message!!.contains("10"))
    }

    @Test
    fun aVersionTheKitDoesNotKnowIsStillNamed() {
        val image = disc("default.xex" to xex(21))
        val refused = refusal(image, listOf(title(xex(10), emptyList()))) as DiscRefusal.UnsupportedVersion
        assertEquals(21L, refused.found)
        assertEquals(emptyList<Long>(), refused.supported)
    }

    @Test
    fun anExecutableOfTheSupportedVersionThatIsNotTheDiscsIsAltered() {
        // Version 10 is the supported one, so it is not another edition of the disc: the bytes are not the disc's.
        val image = disc("default.xex" to xex(10, 8))
        val refused = refusal(image, listOf(title(xex(10), listOf(10))))
        assertTrue(refused is DiscRefusal.Altered)
        assertEquals(10L, (refused as DiscRefusal.Altered).version)
    }

    @Test
    fun theVersionIsReadFromTheExecutionInfoWhereverItIs() {
        val file = folder.newFile().also { it.writeBytes(xex(0xA5)) }
        assertEquals(0xA5L, XexHeader.version(file))
        val big = folder.newFile().also { it.writeBytes(xex(-1)) }
        assertEquals(0xFFFFFFFFL, XexHeader.version(big))
    }

    @Test
    fun aFileThatIsNoExecutableHasNoVersion() {
        assertNull(XexHeader.version(folder.newFile().also { it.writeBytes(ByteArray(0)) }))
        assertNull(XexHeader.version(folder.newFile().also { it.writeBytes("XEX2".toByteArray()) }))
        assertNull(XexHeader.version(folder.newFile().also { it.writeBytes(ByteArray(200)) }))
        val noInfo = xex(10).also { it.fill(0, 24, 28) } // the execution info's key is gone
        assertNull(XexHeader.version(folder.newFile().also { it.writeBytes(noInfo) }))
        val pointsOutside = xex(10).also { it[31] = 0x7F } // where it is, past the file's end
        assertNull(XexHeader.version(folder.newFile().also { it.writeBytes(pointsOutside) }))
    }

    // ---- what is said ---------------------------------------------------------

    private fun said(refusal: DiscRefusal) = refusal.message("Modern Warfare 2", "mw2.iso") { "$it B" }

    @Test
    fun eachKindHasItsOwnSentence() {
        val cause = IOException("pread failed: EIO (I/O error)")
        val all = listOf(
            said(DiscRefusal.Unreadable(DiscRefusal.Reason.MISSING, cause)),
            said(DiscRefusal.Unreadable(DiscRefusal.Reason.PERMISSION, cause)),
            said(DiscRefusal.Unreadable(DiscRefusal.Reason.IO, cause)),
            said(DiscRefusal.NoSpace(1, cause)),
            said(DiscRefusal.NotThisGame("no")),
            said(DiscRefusal.UnsupportedVersion("default.xex", 21, listOf(10))),
            said(DiscRefusal.Altered("default.xex", 10)),
        )
        assertEquals(all.size, all.map { it.id }.toSet().size)
        // The disc of another version is not told as any other kind: it names no unreadable file or full storage.
        assertNotEquals(R.string.setup_disc_not_image, said(DiscRefusal.UnsupportedVersion("default.xex", 21, listOf(10))).id)
    }

    @Test
    fun aUnreadableImageAsksToPickItAgainAndSaysWhetherItsPermissionIsLost() {
        val cause = IOException("pread failed: EIO (I/O error)")
        val gone = said(DiscRefusal.Unreadable(DiscRefusal.Reason.MISSING, cause))
        assertEquals(R.string.setup_disc_lost, gone.id)
        assertEquals(listOf<Any>("mw2.iso"), gone.args)
        val denied = said(DiscRefusal.Unreadable(DiscRefusal.Reason.PERMISSION, cause))
        assertEquals(R.string.setup_disc_permission, denied.id)
        assertEquals(listOf<Any>("mw2.iso"), denied.args)
        val failing = said(DiscRefusal.Unreadable(DiscRefusal.Reason.IO, cause))
        assertEquals(R.string.setup_disc_unreadable, failing.id)
        assertEquals(listOf<Any>("mw2.iso", "pread failed: EIO (I/O error)"), failing.args)
    }

    @Test
    fun aFullStorageSaysHowMuchIsNeeded() {
        val full = said(DiscRefusal.NoSpace(123_456_789, IOException("ENOSPC")))
        assertEquals(R.string.setup_disc_no_space, full.id)
        assertEquals(listOf<Any>("123456789 B"), full.args)
    }

    @Test
    fun aDiscOfAnotherVersionSaysTheVersionFoundAndTheOneSupported() {
        val another = said(DiscRefusal.UnsupportedVersion("default.xex", 21, listOf(2, 22)))
        assertEquals(R.string.setup_disc_version, another.id)
        assertEquals(listOf<Any>("Modern Warfare 2", 21L, "2 / 22"), another.args)
        val unknown = said(DiscRefusal.UnsupportedVersion("default.xex", 21, emptyList()))
        assertEquals(R.string.setup_disc_version_found, unknown.id)
        assertEquals(listOf<Any>("Modern Warfare 2", 21L), unknown.args)
    }

    @Test
    fun aFileThatIsNoDiscOfTheGameKeepsTheOldSentence() {
        assertEquals(R.string.setup_disc_not_image, said(DiscRefusal.NotThisGame("no")).id)
        assertEquals(R.string.setup_disc_not_on_device, said(DiscRefusal.Unreadable(DiscRefusal.Reason.NOT_SEEKABLE, IOException("ESPIPE"))).id)
    }

    // ---- the log --------------------------------------------------------------

    @Test
    fun theLogLineNamesTheClassAndMessageOfWhatWasThrown() {
        val cause = FileNotFoundException("/sdcard/mw2.iso: open failed: ENOENT (No such file or directory)")
        val line = DiscRefusal.describe(DiscRefusal.unreadable(cause))
        assertTrue(line, line.startsWith("com.mw2.recomp.DiscRefusal\$Unreadable: "))
        assertTrue(line, line.contains("java.io.FileNotFoundException: /sdcard/mw2.iso: open failed: ENOENT (No such file or directory)"))
    }
}
