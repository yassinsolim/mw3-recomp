package com.mw2.recomp

import android.content.Context
import org.json.JSONObject
import java.io.File

/**
 * Camouflage packs: what Modern Warfare 3's multiplayer lists below Gold in
 * Create a Class (runtime/camo_packs.h, docs/textures.md). A pack is a folder
 * `camos/<pack-id>/` in the game folder with `pack.json` and its pictures.
 *
 * The packs the APK was built with (the project's own generated one) are
 * assets under `camos/` and are copied into the game folder; a pack the
 * player added -- from a file here (CamoImport), fetched (CamoPacksActivity)
 * or put there by hand -- is left alone. The player's choices about them are
 * `camos/options.txt`, which the game reads.
 */
object CamoPacks {
    private const val ASSETS = "camos"
    private const val PACK_FILE = "pack.json"
    private const val OPTIONS_FILE = "options.txt"

    /** The packs the project knows (setup/camo-packs.json), as an asset; empty in an app built without it. */
    const val KNOWN_ASSET = "camo-packs.json"

    /** What a pack says of itself. */
    data class Pack(
        val id: String,
        val name: String,
        val author: String,
        val url: String,
        val camos: List<String>,
        /** The previews of its camouflages that are files; none for a bundled pack not copied yet. */
        val previews: List<File>,
        /** Whether a camouflage of it was made for Gold, which the Animate option moves. */
        val hasGold: Boolean,
        /** Carried by this app: it comes back when removed, so it is switched off instead. */
        val bundled: Boolean,
    )

    /** The player's choices (camo_packs.h): packs switched off, packs that scroll, and Gold's. */
    data class Options(
        val off: MutableSet<String> = sortedSetOf(),
        val animate: MutableSet<String> = sortedSetOf(),
        var animateGold: Boolean = false,
    )

    fun folder(gameDirectory: String) = File(gameDirectory, ASSETS)

    private fun bundledIds(context: Context): Array<String> = try {
        context.assets.list(ASSETS) ?: emptyArray()
    } catch (_: Throwable) {
        emptyArray()
    }

    /**
     * Copies each bundled pack whose pack.json is not the one in the game
     * folder: a first start, or an APK built with a newer pack. Never throws:
     * a pack that cannot be copied is a camouflage list without it.
     */
    fun installBundled(context: Context, gameDirectory: String) {
        val assets = context.assets
        for (id in bundledIds(context)) {
            try {
                val wanted = assets.open("$ASSETS/$id/$PACK_FILE").use { it.readBytes() }
                val target = File(folder(gameDirectory), id)
                val have = File(target, PACK_FILE)
                if (have.isFile && have.readBytes().contentEquals(wanted)) continue
                target.deleteRecursively()
                copy(context, "$ASSETS/$id", target)
            } catch (_: Throwable) {
                // Not a pack, or no room for it.
            }
        }
    }

    /** An asset folder, whole. pack.json goes last, so a copy cut short is taken up again. */
    private fun copy(context: Context, asset: String, target: File) {
        val children = context.assets.list(asset) ?: emptyArray()
        target.mkdirs()
        for (child in children.sortedBy { it == PACK_FILE }) {
            val from = "$asset/$child"
            val to = File(target, child)
            if ((context.assets.list(from) ?: emptyArray()).isNotEmpty()) {
                copy(context, from, to)
            } else {
                context.assets.open(from).use { input ->
                    to.outputStream().use { output -> input.copyTo(output) }
                }
            }
        }
    }

    private fun pack(id: String, json: String, folder: File?, bundled: Boolean): Pack? = try {
        val read = JSONObject(json)
        val camos = read.optJSONArray("camos")
        val entries = (0 until (camos?.length() ?: 0)).mapNotNull { camos?.optJSONObject(it) }
        Pack(
            id = id,
            name = read.optString("name").ifEmpty { id },
            author = read.optString("author"),
            url = read.optString("url"),
            camos = entries.map { it.optString("name").ifEmpty { it.optString("id") } },
            previews = if (folder == null) emptyList() else entries.mapNotNull { entry ->
                listOf("swatch", "image", "specular", "colour").map { entry.optString(it) }
                    .firstOrNull { it.isNotEmpty() }?.let { File(folder, it) }?.takeIf { it.isFile }
            },
            hasGold = entries.any { it.optString("target") == "gold" },
            bundled = bundled,
        )
    } catch (_: Throwable) {
        null
    }

    /**
     * Every pack the game will list, in its order: those in the game folder,
     * and those this app carries and copies there when the multiplayer next
     * starts (installBundled), so an app updated over one that had no packs
     * shows them before its first match.
     */
    fun list(context: Context, gameDirectory: String): List<Pack> {
        val packs = sortedMapOf<String, Pack>()
        val bundled = bundledIds(context)
        for (id in bundled) {
            val json = try {
                context.assets.open("$ASSETS/$id/$PACK_FILE").use { it.readBytes().decodeToString() }
            } catch (_: Throwable) {
                continue
            }
            pack(id, json, null, true)?.let { packs[id] = it }
        }
        for (folder in folder(gameDirectory).listFiles() ?: emptyArray()) {
            if (!folder.isDirectory) continue
            val json = try {
                File(folder, PACK_FILE).readText()
            } catch (_: Throwable) {
                continue
            }
            pack(folder.name, json, folder, folder.name in bundled)?.let { packs[folder.name] = it }
        }
        return packs.values.toList()
    }

    fun remove(gameDirectory: String, id: String) {
        File(folder(gameDirectory), id).deleteRecursively()
    }

    fun options(gameDirectory: String): Options {
        val options = Options()
        val lines = try {
            File(folder(gameDirectory), OPTIONS_FILE).readLines()
        } catch (_: Throwable) {
            emptyList()
        }
        for (line in lines) {
            val value = line.substringAfter('=', "").trim()
            when (line.substringBefore('=')) {
                "off" -> options.off += value
                "animate" -> options.animate += value
                "animate-gold" -> options.animateGold = value == "1"
            }
        }
        return options
    }

    fun save(gameDirectory: String, options: Options) {
        try {
            folder(gameDirectory).mkdirs()
            File(folder(gameDirectory), OPTIONS_FILE).writeText(buildString {
                for (id in options.off) append("off=$id\n")
                for (id in options.animate) append("animate=$id\n")
                if (options.animateGold) append("animate-gold=1\n")
            })
        } catch (_: Throwable) {
            // No game folder to write to yet: the choices are made again once there is.
        }
    }
}
