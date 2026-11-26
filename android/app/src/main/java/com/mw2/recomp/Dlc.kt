package com.mw2.recomp

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import java.io.File
import java.io.IOException
import java.io.InputStream

/**
 * A player's own downloadable content for Advanced Warfare (docs/dlc.md):
 * the map packs and the title update, each one file as the console keeps it
 * on its hard disk, copied into the dlc folder beside the game folder, where
 * the install disc's two packages already are and where the game looks for
 * a title's content (runtime/kernel/content.cpp). The files are the
 * player's; the app brings none and fetches none.
 */
object Dlc {
    private const val TITLE_ID = 0x41560914
    private const val CONTENT = 0x00000002
    private const val UPDATE = 0x000B0000
    private const val HEADER = 0x511

    /** What the app installs itself, from the install disc: not a pack the player added. */
    private val installDisc = setOf("41560914051BF3C0", "4156091407406280")

    /** A content package of this game: its name as the console shows it, and whether it is the title update. */
    class Header(val name: String, val update: Boolean)

    /** A package in the dlc folder. */
    class Package(val file: File, val header: Header)

    /** Whether this app's game has downloadable content at all. */
    val offered: Boolean = Title.isAw

    /** The dlc folder: beside the game folder, wherever that is. */
    fun folder(context: Context): File {
        val game = Prefs(context).gameDirectory.ifEmpty { File(context.getExternalFilesDir(null) ?: context.filesDir, "game").absolutePath }
        return File(File(game).absoluteFile.parentFile, "dlc")
    }

    /**
     * Reads a package's header: null for anything that is not a package of
     * this game's downloadable content or its title update. The container
     * begins LIVE or PIRS, with its content type at 0x344, the title at
     * 0x360 and the name at 0x411 (tools/stfs.py).
     */
    fun header(input: InputStream): Header? {
        val head = ByteArray(HEADER)
        var have = 0
        while (have < head.size) {
            val read = input.read(head, have, head.size - have)
            if (read <= 0) return null
            have += read
        }
        val magic = String(head, 0, 4, Charsets.ISO_8859_1)
        if (magic != "LIVE" && magic != "PIRS") return null
        fun word(at: Int) = (0 until 4).fold(0) { value, i -> (value shl 8) or (head[at + i].toInt() and 0xFF) }
        val type = word(0x344)
        if (word(0x360) != TITLE_ID || (type != CONTENT && type != UPDATE)) return null
        val name = String(head, 0x411, 0x100, Charsets.UTF_16BE).substringBefore('\u0000')
        return Header(name, type == UPDATE)
    }

    private fun header(file: File): Header? = try {
        file.inputStream().use { header(it) }
    } catch (_: IOException) {
        null
    }

    /** The player's packages in the dlc folder, the title update first. */
    fun installed(context: Context): List<Package> =
        folder(context).listFiles().orEmpty()
            .filter { it.isFile && it.name !in installDisc && !it.name.endsWith(".part") }
            .mapNotNull { file -> header(file)?.let { Package(file, it) } }
            .sortedWith(compareBy({ !it.header.update }, { it.header.name }))

    /** Whether a title update's package is there: the map packs are for the game it makes. */
    fun hasUpdate(context: Context): Boolean = update(context) != null

    /** The title update's package, the first by name as the runtime takes them (runtime/kernel/content.cpp). */
    fun update(context: Context): File? = installed(context).firstOrNull { it.header.update }?.file

    /** One document the player chose: where it is, its name and its size. */
    class Source(val uri: Uri, val name: String, val size: Long)

    /** Every file under a folder the player chose (the system's picker gives a tree), folders within it too. */
    fun filesUnder(context: Context, tree: Uri): List<Source> {
        val found = ArrayList<Source>()
        val pending = ArrayDeque(listOf(DocumentsContract.getTreeDocumentId(tree)))
        val columns = arrayOf(
            DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE,
        )
        while (pending.isNotEmpty()) {
            val children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, pending.removeFirst())
            context.contentResolver.query(children, columns, null, null, null)?.use { rows ->
                while (rows.moveToNext()) {
                    val id = rows.getString(0)
                    if (rows.getString(2) == DocumentsContract.Document.MIME_TYPE_DIR) pending.add(id)
                    else found.add(Source(DocumentsContract.buildDocumentUriUsingTree(tree, id), rows.getString(1).orEmpty(), rows.getLong(3)))
                }
            }
        }
        return found
    }

    /** A file the player chose by itself. */
    fun single(context: Context, uri: Uri): Source {
        var name = uri.lastPathSegment.orEmpty().substringAfterLast('/')
        var size = 0L
        context.contentResolver.query(
            uri, arrayOf(DocumentsContract.Document.COLUMN_DISPLAY_NAME, DocumentsContract.Document.COLUMN_SIZE), null, null, null,
        )?.use { rows ->
            if (rows.moveToFirst()) {
                name = rows.getString(0) ?: name
                size = rows.getLong(1)
            }
        }
        return Source(uri, name, size)
    }

    /** What [add] did with the files it was given. */
    class Added(val copied: List<String>, val already: Int, val notPackages: Int)

    /**
     * Copies those of [sources] that are this game's packages into the dlc
     * folder, each under the name it has (the console's) and through a
     * temporary one, so a file there is a file copied whole. One of the same
     * name and size is left as it is. [progress] is told the bytes copied of
     * the bytes there are; [cancelled] ends it between blocks. Throws an
     * [IOException] that says what went wrong.
     */
    fun add(context: Context, sources: List<Source>, progress: (Long, Long) -> Unit, cancelled: () -> Boolean): Added {
        val folder = folder(context)
        if (!folder.isDirectory && !folder.mkdirs()) throw IOException("the dlc folder could not be made: $folder")
        val packages = sources.filter { source ->
            try {
                context.contentResolver.openInputStream(source.uri)?.use { header(it) } != null
            } catch (_: Exception) {
                false
            }
        }
        val wanted = packages.filter { File(folder, safeName(it.name)).let { there -> !there.isFile || there.length() != it.size } }
        val total = wanted.sumOf { it.size }
        if (total > folder.usableSpace) {
            throw IOException(context.getString(R.string.dlc_no_room, total / 1e9, folder.usableSpace / 1e9))
        }
        var done = 0L
        val copied = ArrayList<String>()
        val buffer = ByteArray(1 shl 20)
        for (source in wanted) {
            val target = File(folder, safeName(source.name))
            val partial = File(target.path + ".part")
            try {
                val input = context.contentResolver.openInputStream(source.uri) ?: throw IOException("${source.name} could not be opened")
                input.use {
                    partial.outputStream().use { output ->
                        while (true) {
                            if (cancelled()) throw IOException(context.getString(R.string.dlc_cancelled))
                            val read = input.read(buffer)
                            if (read <= 0) break
                            output.write(buffer, 0, read)
                            done += read
                            progress(done, total)
                        }
                    }
                }
                if (!partial.renameTo(target)) throw IOException("${source.name} could not be kept")
                copied.add(header(target)?.name ?: source.name)
            } finally {
                partial.delete()
            }
        }
        TitleUpdate.refresh(context)
        return Added(copied, packages.size - wanted.size, sources.size - packages.size)
    }

    /** A document's name as a file's: the console's names are letters and digits, and nothing else is let through as a path. */
    private fun safeName(name: String): String = name.substringAfterLast('/').substringAfterLast('\\').ifEmpty { "package" }

    /** Removes one of the player's packages. */
    fun remove(context: Context, item: Package) {
        item.file.delete()
        TitleUpdate.refresh(context)
    }
}

/**
 * Advanced Warfare's multiplayer as its title update makes it: another
 * executable than the disc's, so another library of the app and another of
 * the game code (awmp17; docs/dlc.md). The map packs and Exo Zombies are for
 * it, and it plays only with others who have it, as an updated console does.
 * It is what the multiplayer starts as while the update's package is in the
 * dlc folder and its game code is on the device; otherwise the disc's.
 */
object TitleUpdate {
    /** The library's name, "" in an app whose game has no such build. */
    const val LIBRARY: String = BuildConfig.LIBRARY_MP_UPDATE

    /**
     * Exo Zombies is the updated multiplayer in another mode, which the
     * updated campaign's menu names in the launch data it starts it with:
     * 0x3F8 bytes, the first word 11 and the mode, 5, at 0x3D8
     * (docs/AW-BRINGUP.md). The campaign here is the disc's and has no such
     * entry, so the launcher has it.
     */
    val zombiesLaunchData: String = "0000000b" + "00".repeat(0x3D4) + "00000005" + "00".repeat(0x1C)

    /** Whether the multiplayer is the update's in this process's runs. */
    @Volatile
    var inUse: Boolean = false
        private set

    /** Whether the update's package is there and only its game code is missing: what the launcher says. */
    @Volatile
    var codeMissing: Boolean = false
        private set

    /** Looks again: as each process starts, and when the player adds or removes a package. */
    fun refresh(context: Context) {
        if (LIBRARY.isEmpty()) return
        val update = Dlc.hasUpdate(context)
        val code = GameLibrary.updateReady(context)
        inUse = update && code
        codeMissing = update && !code
    }
}
