package com.mw2.recomp

import android.content.Context
import org.json.JSONObject
import java.io.File

/**
 * Camouflage packs: what Modern Warfare 3's multiplayer lists below Gold in
 * Create a Class (runtime/camo_packs.h, docs/textures.md). A pack is a folder
 * `camos/<pack-id>/` in the game folder with `pack.json` and its pictures.
 *
 * The packs the APK was built with (the project's own generated one, and any
 * the builder imported from their authors' downloads) are assets under
 * `camos/` and are copied into the game folder; a pack the player put there
 * by hand is left alone.
 */
object CamoPacks {
    private const val ASSETS = "camos"
    private const val PACK_FILE = "pack.json"

    /** What a pack says of itself, for the credits. */
    data class Pack(
        val name: String,
        val author: String,
        val url: String,
        val permission: String,
        val camos: Int,
    )

    private fun folder(gameDirectory: String) = File(gameDirectory, ASSETS)

    /**
     * Copies each bundled pack whose pack.json is not the one in the game
     * folder: a first start, or an APK built with a newer pack. Never throws:
     * a pack that cannot be copied is a camouflage list without it.
     */
    fun installBundled(context: Context, gameDirectory: String) {
        val assets = context.assets
        val bundled = try {
            assets.list(ASSETS) ?: emptyArray()
        } catch (_: Throwable) {
            emptyArray()
        }
        for (id in bundled) {
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

    private fun pack(id: String, json: String): Pack? = try {
        val read = JSONObject(json)
        Pack(
            name = read.optString("name").ifEmpty { id },
            author = read.optString("author"),
            url = read.optString("url"),
            permission = read.optString("permission"),
            camos = read.optJSONArray("camos")?.length() ?: 0,
        )
    } catch (_: Throwable) {
        null
    }

    /**
     * Every pack the game will list, in its order: those in the game folder,
     * and those this app carries and copies there when the multiplayer next
     * starts (installBundled), so an app updated over one that had no packs
     * credits them before its first match.
     */
    fun credits(context: Context, gameDirectory: String): List<Pack> {
        val packs = sortedMapOf<String, Pack>()
        for (id in try { context.assets.list(ASSETS) ?: emptyArray() } catch (_: Throwable) { emptyArray() }) {
            val json = try {
                context.assets.open("$ASSETS/$id/$PACK_FILE").use { it.readBytes().decodeToString() }
            } catch (_: Throwable) {
                continue
            }
            pack(id, json)?.let { packs[id] = it }
        }
        for (folder in folder(gameDirectory).listFiles() ?: emptyArray()) {
            if (!folder.isDirectory) continue
            val json = try {
                File(folder, PACK_FILE).readText()
            } catch (_: Throwable) {
                continue
            }
            pack(folder.name, json)?.let { packs[folder.name] = it }
        }
        return packs.values.toList()
    }
}
