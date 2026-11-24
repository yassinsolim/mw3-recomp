package com.mw2.recomp

import android.content.Context
import android.content.pm.PackageManager
import java.io.File
import java.io.IOException
import java.security.MessageDigest
import java.util.zip.ZipFile
import org.json.JSONObject

/**
 * The compiler and the recompiler, for building the game code on this device
 * (GameBuild.kt, docs/ondevice-build.md). They are a package of their own,
 * com.recomp.toolchain, published beside the app: Android runs a program only from a
 * package's library folder, never from a file an app wrote, so the programs
 * are that package's "libraries" (libllvm.so is clang and the linker in one,
 * libXenonRecomp.so, libXenonAnalyse.so), and this app starts them from there.
 * What they read (the C and C++ headers and libraries, the recompiler's own
 * headers) is one archive among that package's assets, unpacked here once
 * into this app's storage.
 *
 * `assets/toolchain/toolchain.json` says what the package holds and how its
 * compiler and linker are called; the package's source is android/toolchain.
 */
class Toolchain private constructor(
    private val manifest: JSONObject,
    private val libraries: File,
    private val apk: File,
    /** Where this app unpacks the package's archive. */
    val data: File,
) {
    val version: String = manifest.optString("version")

    /** The recompiler the package was built from: part of what the game library states (cmake/game_abi.cmake). */
    val recompilerCommit: String = manifest.getJSONObject("recompiler").getString("commit")
    val recompilerPatch: String = manifest.getJSONObject("recompiler").getString("patchSha256")

    val recompiler: File = tool("recomp")
    val analyser: File = tool("analyse")

    /** SIMDe's folder, which the recompiled code includes from. */
    val simde: File = File(data, manifest.getJSONObject("paths").getString("simde"))

    /** The recompiler's register context header, which it copies into every tree. */
    val context: File = File(data, manifest.getJSONObject("paths").getString("context"))

    private fun tool(name: String): File = File(libraries, manifest.getJSONObject("tools").getString(name))

    /** One of the manifest's argument lists, with the places it leaves for paths filled in. */
    private fun arguments(key: String): List<String> {
        val list = manifest.getJSONArray(key)
        return (0 until list.length()).map {
            list.getString(it)
                .replace("{llvm}", tool("llvm").path)
                .replace("{recomp}", recompiler.path)
                .replace("{analyse}", analyser.path)
                .replace("{data}", data.path)
        }
    }

    /** The compiler's command line up to the options that are the build's own. */
    val compile: List<String> = arguments("compile")

    /** The linker's command line before the objects, and after them. */
    val linkBefore: List<String> = arguments("linkPrefix")
    val linkAfter: List<String> = arguments("linkSuffix")

    /** Whether the archive unpacked in [data] is this package's. */
    private val stamp = File(data, ".unpacked")
    private val archiveHash: String = manifest.getJSONObject("data").getString("sha256")
    val unpacked: Boolean
        get() = try {
            stamp.readText() == archiveHash
        } catch (_: IOException) {
            false
        }

    /**
     * Unpacks the package's archive into [data], unless it is there already.
     * [progress] hears the files done and their number. Throws an
     * [IOException] that says what went wrong.
     */
    fun unpack(progress: (Int, Int) -> Unit) {
        if (unpacked) return
        data.deleteRecursively()
        data.mkdirs()
        val archive = File(data.parentFile, "${data.name}.zip")
        try {
            // The archive is an entry of the package's APK: copied out first,
            // because a zip inside a zip cannot be read in place, and checked
            // as it is copied.
            val digest = MessageDigest.getInstance("SHA-256")
            ZipFile(apk).use { zip ->
                val entry = zip.getEntry("assets/" + manifest.getJSONObject("data").getString("asset"))
                    ?: throw IOException("the toolchain package has no data archive")
                zip.getInputStream(entry).use { input ->
                    archive.outputStream().use { output ->
                        val buffer = ByteArray(1 shl 16)
                        while (true) {
                            val read = input.read(buffer)
                            if (read <= 0) break
                            digest.update(buffer, 0, read)
                            output.write(buffer, 0, read)
                        }
                    }
                }
            }
            if (digest.digest().joinToString("") { "%02x".format(it) } != archiveHash) {
                throw IOException("the toolchain package's data archive is damaged: install the package again")
            }
            val root = data.canonicalPath + File.separator
            ZipFile(archive).use { zip ->
                val total = zip.size()
                var done = 0
                for (entry in zip.entries()) {
                    val target = File(data, entry.name)
                    // Nothing of it may land outside the folder.
                    if (!target.canonicalPath.startsWith(root)) throw IOException("the toolchain package's data archive is not safe to unpack")
                    if (entry.isDirectory) {
                        target.mkdirs()
                    } else {
                        target.parentFile?.mkdirs()
                        zip.getInputStream(entry).use { input -> target.outputStream().use { input.copyTo(it, 1 shl 16) } }
                    }
                    progress(++done, total)
                }
            }
            stamp.writeText(archiveHash)
        } finally {
            archive.delete()
        }
    }

    companion object {
        const val PACKAGE = "com.recomp.toolchain"

        /** The manifest this app knows how to read. */
        private const val SCHEMA = 1

        /** The toolchain package on this device, or null when it is not installed or is one this app cannot use. */
        fun find(context: Context): Toolchain? {
            val info = try {
                context.packageManager.getApplicationInfo(PACKAGE, 0)
            } catch (_: PackageManager.NameNotFoundException) {
                return null
            }
            return try {
                val apk = File(info.sourceDir)
                val manifest = ZipFile(apk).use { zip ->
                    val entry = zip.getEntry("assets/toolchain/toolchain.json") ?: return null
                    JSONObject(zip.getInputStream(entry).bufferedReader().readText())
                }
                if (manifest.getInt("schema") != SCHEMA) return null
                Toolchain(manifest, File(info.nativeLibraryDir), apk, File(context.filesDir, "toolchain-data"))
            } catch (_: Exception) {
                // An unreadable package is, to the player, one to install again.
                null
            }
        }
    }
}
