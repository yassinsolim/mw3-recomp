package com.mw2.recomp

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import java.io.File
import java.io.IOException
import java.io.InputStream

/**
 * A player's own downloadable content (docs/dlc.md): the map packs, and for
 * Advanced Warfare and Black Ops III the title update they need, each one
 * file as the console keeps it on its hard disk, copied into the dlc folder
 * beside the game folder, where the game looks for a title's content
 * (runtime/kernel/content.cpp) and where Advanced Warfare's install disc's
 * two packages already are. The files are the player's; the app brings none
 * and fetches none.
 */
object Dlc {
    private val TITLE_ID = Title.titleId.toInt()
    private const val CONTENT = 0x00000002
    private const val UPDATE = 0x000B0000
    private const val HEADER = 0x511

    /** The game's folder on the console's hard disk, below Content\0000000000000000. */
    val consoleFolder: String = "%08X".format(Title.titleId)

    /** What the app installs itself, from Advanced Warfare's install disc: not a pack the player added. */
    private val installDisc = if (Title.isAw) setOf("41560914051BF3C0", "4156091407406280") else emptySet()

    /**
     * Whether this game is played as its title update makes it, once the
     * player has the update's package: Advanced Warfare, and Black Ops III,
     * which is nothing else (TitleUpdate.required). The others play the
     * disc's executables, which read the packs themselves.
     */
    val takesUpdate: Boolean = Title.isAw || Title.isBo3

    /**
     * How many Compatibility Packs the updated game asks for: Advanced
     * Warfare's title update 17 is the game's last and wants all six, Black
     * Ops III's title update 5 the first two (with the first alone its
     * multiplayer asks to download required content; a third it looks for
     * and does without). Each is a package with the marker
     * `dlccompat<n>.id` at its root, which is what the title tells it by
     * (docs/dlc.md); the name the console shows is another in another
     * language.
     */
    private val COMPATIBILITY_PACKS = if (Title.isBo3) 2 else 6

    /** A content package of this game: its name as the console shows it, and whether it is the title update. */
    class Header(val name: String, val update: Boolean)

    /** A package in the dlc folder. */
    class Package(val file: File, val header: Header)

    /** Every game's app takes its player's packs. */
    const val offered: Boolean = true

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
        if (word(0x360) != TITLE_ID || (type != CONTENT && !(type == UPDATE && takesUpdate))) return null
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

    /**
     * The Compatibility Packs that are not among [packages], by number. With
     * one missing the updated game shows only its notice that required
     * content was added to the console's downloads, and offers the main
     * menu: a download that never comes here, so the app says which to add.
     * Advanced Warfare's alone: the other games have no such packs, and
     * Modern Warfare 2's Add DLC told a player with one map pack that six
     * packs of another game were missing.
     */
    fun missingCompatibilityPacks(packages: List<Package>): List<Int> {
        if (!takesUpdate) return emptyList()
        val markers = packages.filterNot { it.header.update }.flatMapTo(HashSet<String>()) { StfsPackage.rootFiles(it.file) }
        return (1..COMPATIBILITY_PACKS).filter { "dlccompat$it.id" !in markers }
    }

    /** [missing] as the player reads it: each pack by the name the console lists it under. */
    fun compatibilityPackNames(context: Context, missing: List<Int>): String =
        missing.joinToString(", ") { context.getString(R.string.dlc_compatibility_pack, it) }

    /** What is in the dlc folder, for the log a player shares: in English, as the log is. */
    fun describe(context: Context): String {
        val packages = installed(context)
        if (packages.isEmpty()) return "none"
        val count = "${packages.count { !it.header.update }} packages"
        // Only Advanced Warfare's packs are for an updated game.
        if (!takesUpdate) return count
        val missing = missingCompatibilityPacks(packages)
        return "$count and " +
            (if (packages.any { it.header.update }) "the title update" else "no title update") +
            if (missing.isEmpty()) "" else "; not here: " + missing.joinToString(", ") { "Compatibility Pack $it" }
    }

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

    /** A Special Ops mission or Survival map in one of the player's packages: what `map` loads, and its name. */
    class Level(val level: String, val name: String)

    /** The names the game gave its packs' multiplayer maps, which their Survival maps are played on (runtime/mw3_dlc_maps.cpp). */
    private val mapNames = mapOf(
        "mp_italy" to "Piazza", "mp_park" to "Liberation", "mp_overwatch" to "Overwatch", "mp_morningwood" to "Black Box",
        "mp_meteora" to "Sanctuary", "mp_cement" to "Foundation", "mp_qadeem" to "Oasis", "mp_terminal_cls" to "Terminal",
        "mp_shipbreaker" to "Decommission", "mp_roughneck" to "Offshore", "mp_moab" to "Gulch", "mp_boardwalk" to "Boardwalk",
        "mp_nola" to "Parish",
    )

    /** And its packs' Special Ops missions: a Collection holds several, so its package's name is none of theirs. */
    private val missionNames = mapOf(
        "so_escape_rescue_2_so" to "Black Ice", "so_infiltrate_intro" to "Negotiator", "so_coop_vlad_ep01" to "Kill Switch",
        "so_escape_hamburg" to "Iron Clad", "so_demolition_slava_hunt" to "Arctic Recon",
        "so_rooftop_dubai_specops" to "Vertigo", "so_spotlight_castle" to "Light 'Em Up",
        "so_support_dam_specops" to "Special Delivery",
    )

    /**
     * Modern Warfare 3's Special Ops levels in the dlc folder's packages:
     * each so_<level>.ff at a package's root, once (a Collection and a
     * single drop may both hold it). A Survival map is named after its
     * map; a level neither table knows, by its file.
     */
    fun levels(context: Context): List<Level> {
        if (!Title.isMw3) return emptyList()
        val found = LinkedHashMap<String, Level>()
        for (item in installed(context)) {
            val files = try {
                rootFiles(item.file)
            } catch (_: IOException) {
                emptyList()
            }
            for (file in files) {
                val level = file.lowercase().removeSuffix(".ff")
                if (!level.startsWith("so_") || level == file.lowercase()) continue
                val map = level.removePrefix("so_survival_")
                val name = if (map != level) "Survival: " + (mapNames[map] ?: map) else "Mission: " + (missionNames[level] ?: level)
                found.getOrPut(level) { Level(level, name) }
            }
        }
        return found.values.sortedBy { it.name }
    }

    /**
     * The names of the files at a package's root. The file table is a chain
     * of 4 KB blocks of 64-byte entries (the name, its length in the low six
     * bits of the byte at 0x28, the folder it is in at 0x32); a block's
     * place in the file and the block after it are as tools/stfs.py has
     * them, for the read-only layout a downloaded package has.
     */
    private fun rootFiles(file: File): List<String> {
        val names = ArrayList<String>()
        java.io.RandomAccessFile(file, "r").use { input ->
            fun offset(block: Int): Long {
                var tables = 0
                if (block >= 0xAA) tables += block / 0xAA + 1
                if (block >= 0x70E4) tables += block / 0x70E4 + 1
                return 0xC000L + (block + tables).toLong() * 0x1000
            }
            val head = ByteArray(5)
            input.seek(0x37C)
            input.readFully(head)
            fun byte(bytes: ByteArray, at: Int) = bytes[at].toInt() and 0xFF
            val blocks = byte(head, 0) or (byte(head, 1) shl 8)
            var block = byte(head, 2) or (byte(head, 3) shl 8) or (byte(head, 4) shl 16)
            val table = ByteArray(0x1000)
            val next = ByteArray(3)
            for (i in 0 until blocks) {
                input.seek(offset(block))
                input.readFully(table)
                for (entry in 0 until 0x1000 step 0x40) {
                    val flags = byte(table, entry + 0x28)
                    val length = flags and 0x3F
                    if (length == 0) return names
                    val atRoot = byte(table, entry + 0x32) == 0xFF && byte(table, entry + 0x33) == 0xFF
                    if (atRoot && flags and 0x80 == 0) names.add(String(table, entry, length, Charsets.ISO_8859_1))
                }
                input.seek(offset(block - block % 0xAA) - 0x1000 + (block % 0xAA) * 0x18 + 0x15)
                input.readFully(next)
                block = (byte(next, 0) shl 16) or (byte(next, 1) shl 8) or byte(next, 2)
            }
        }
        return names
    }
}

/**
 * The game as the player's title update makes it.
 *
 * Advanced Warfare's multiplayer: another executable than the disc's, so
 * another library of the app and another of the game code (awmp17;
 * docs/dlc.md). The map packs and Exo Zombies are for it, and it plays only
 * with others who have it, as an updated console does. It is what the
 * multiplayer starts as while the update's package and every Compatibility
 * Pack are in the dlc folder and its game code is on the device; otherwise
 * the disc's.
 *
 * Black Ops III is nothing else ([required]): the disc alone has neither a
 * multiplayer map nor the zombies' (docs/BO3-BRINGUP.md), so both of its
 * titles are the update's, built from the disc and the update's package, and
 * there is no game until that package and Compatibility Packs 1 and 2 are here.
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

    /** Whether the game is played only as its update makes it. */
    val required: Boolean = Title.isBo3

    /** Whether the update's package, every Compatibility Pack it asks for and its game code are all here. */
    @Volatile
    var ready: Boolean = false
        private set

    /** Whether the multiplayer is the update's in this process's runs, where the disc's is the other choice. */
    val inUse: Boolean
        get() = LIBRARY.isNotEmpty() && ready

    /** Whether a game that is [required] to have its update has not got the package yet. */
    @Volatile
    var updateMissing: Boolean = false
        private set

    /** Whether the update's package is there and its game code is missing: the app builds it then. */
    @Volatile
    var codeMissing: Boolean = false
        private set

    /**
     * The Compatibility Packs still to add (Dlc.missingCompatibilityPacks),
     * counted while the update's package is there or the game is nothing
     * without it: the multiplayer stays the disc's until there are none, or
     * the game does not start, and the launcher and Add DLC say which.
     */
    @Volatile
    var missingPacks: List<Int> = emptyList()
        private set

    /** Looks again: as each process starts, and when the player adds or removes a package. */
    fun refresh(context: Context) {
        if (LIBRARY.isEmpty() && !required) return
        val packages = Dlc.installed(context)
        val update = packages.any { it.header.update }
        val code = if (required) Title.carried.all { GameLibrary.current(context, it.library) } else GameLibrary.updateReady(context)
        missingPacks = if (update || required) Dlc.missingCompatibilityPacks(packages) else emptyList()
        ready = update && code && missingPacks.isEmpty()
        updateMissing = required && !update
        codeMissing = update && !code
    }
}
