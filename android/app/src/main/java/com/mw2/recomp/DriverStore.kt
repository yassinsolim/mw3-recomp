package com.mw2.recomp

import android.content.Context
import android.net.Uri
import android.os.Build
import org.json.JSONObject
import java.io.File
import java.io.InputStream
import java.security.MessageDigest
import java.util.zip.ZipInputStream

/**
 * The drivers the player has imported.
 *
 * A driver package is a plain zip: a meta.json naming the library inside it,
 * and the library itself. It is unpacked into filesDir/drivers/<name>/,
 * which is the layout the loader expects (runtime/android/driver.cpp) and
 * the one every other app that does this uses, so a package downloaded for
 * one works here.
 */
class DriverStore(private val context: Context) {

    data class Driver(
        val directory: File,
        val name: String,
        val description: String,
        val author: String,
        val version: String,
        val library: String,
        val vendor: String,
        val minimumApi: Int,
    )

    // Canonical, as import writes the folders it keeps in the settings: the
    // files folder is /data/user/0/... by one name and /data/data/... by the
    // other, and a driver listed under one was not found by the other.
    private val root: File get() = File(context.filesDir, "drivers").apply { mkdirs() }.canonicalFile

    fun list(): List<Driver> {
        val folders = root.listFiles() ?: return emptyList()
        return folders.filter { it.isDirectory }
            .mapNotNull { read(it) }
            .sortedBy { it.name.lowercase() }
    }

    /** The driver the build carries, once it is unpacked (installBundledDefault); null in a build or on a chip without one. */
    fun bundled(prefs: Prefs): Driver? =
        prefs.bundledDriverDirectory.takeIf { it.isNotEmpty() }?.let { read(File(it)) }

    private fun read(directory: File): Driver? {
        val meta = File(directory, "meta.json")
        if (!meta.exists()) return null
        return try {
            val json = JSONObject(meta.readText())
            val library = json.optString("libraryName")
            // A name, never a path: the runtime opens the folder's file of that name.
            if (library.isEmpty() || library.contains('/') || !File(directory, library).isFile) return null
            Driver(
                directory = directory,
                name = json.optString("name", directory.name),
                description = json.optString("description", ""),
                author = json.optString("author", ""),
                version = json.optString("driverVersion", json.optString("packageVersion", "")),
                library = library,
                vendor = json.optString("vendor", ""),
                minimumApi = json.optInt("minApi", 0),
            )
        } catch (_: Throwable) {
            null
        }
    }

    /** What went wrong, or the driver that is now in place. */
    sealed class Outcome {
        data class Imported(val driver: Driver) : Outcome()
        data class Failed(val reason: String) : Outcome()
    }

    /**
     * Unpacks a zip the player picked.
     */
    fun import(uri: Uri, suggestedName: String): Outcome {
        val stream = try {
            context.contentResolver.openInputStream(uri)
        } catch (error: Throwable) {
            return Outcome.Failed(error.message ?: context.getString(R.string.driver_bad_zip))
        } ?: return Outcome.Failed(context.getString(R.string.driver_bad_zip))
        return import(stream, suggestedName)
    }

    /**
     * Unpacks a driver zip from any stream: the picked file above, or the
     * package a build carries in its assets. Everything is written into a
     * folder of its own and only moved into place once meta.json has been
     * read and makes sense, so a half-unpacked archive never becomes a
     * driver the runtime then fails to load. The stream is closed here.
     * [catalogId]: the catalog's name for the package (DriverCatalog.Entry),
     * whose SHA-256 the caller has checked; it is then unpacked into a
     * folder of that name and remembered as that entry ([idOf]).
     */
    fun import(stream: InputStream, suggestedName: String, catalogId: String? = null): Outcome {
        val staging = File(context.cacheDir, "driver-import-${System.currentTimeMillis()}")
        staging.mkdirs()
        try {
            stream.use { unzip(it, staging) }

            // Some archives hold one folder with everything inside it.
            var content = staging
            if (!File(content, "meta.json").exists()) {
                val only = content.listFiles()?.singleOrNull { it.isDirectory }
                if (only != null && File(only, "meta.json").exists()) content = only
            }
            val metaFile = File(content, "meta.json")
            if (!metaFile.exists()) return Outcome.Failed(context.getString(R.string.driver_bad_zip))

            val json = JSONObject(metaFile.readText())
            val library = json.optString("libraryName")
            if (library.isEmpty() || library.contains('/') || !File(content, library).isFile) {
                return Outcome.Failed(context.getString(R.string.driver_bad_zip))
            }
            if (!looksLikeArm64(File(content, library))) {
                return Outcome.Failed(context.getString(R.string.driver_not_arm64))
            }

            val minApi = json.optInt("minApi", 0)
            if (minApi > 0 && android.os.Build.VERSION.SDK_INT < minApi) {
                return Outcome.Failed(context.getString(R.string.driver_needs_api, minApi, android.os.Build.VERSION.SDK_INT))
            }

            val name = json.optString("name", suggestedName).ifEmpty { suggestedName }
            val driverFolderName = catalogId ?: safeName(name)
            val target = File(root, driverFolderName).canonicalFile
            val rootCanonical = root.canonicalFile
            if (!target.path.startsWith(rootCanonical.path + File.separator)) {
                return Outcome.Failed(context.getString(R.string.driver_bad_zip))
            }

            val tempTarget = File(root, ".import-${System.currentTimeMillis()}").canonicalFile
            tempTarget.deleteRecursively()
            if (!content.renameTo(tempTarget)) {
                content.copyRecursively(tempTarget, overwrite = true)
            }
            val driver = read(tempTarget)
            if (driver == null) {
                tempTarget.deleteRecursively()
                return Outcome.Failed(context.getString(R.string.driver_bad_zip))
            }
            target.deleteRecursively()
            if (!tempTarget.renameTo(target)) {
                tempTarget.copyRecursively(target, overwrite = true)
                tempTarget.deleteRecursively()
            }
            val finalDriver = read(target) ?: return Outcome.Failed(context.getString(R.string.driver_bad_zip))
            if (catalogId != null) File(target, CATALOG_MARK).writeText(catalogId) else File(target, CATALOG_MARK).delete()
            return Outcome.Imported(finalDriver)
        } catch (error: Throwable) {
            return Outcome.Failed(error.message ?: context.getString(R.string.driver_bad_zip))
        } finally {
            staging.deleteRecursively()
        }
    }

    fun remove(driver: Driver) {
        val canonicalDir = driver.directory.canonicalFile
        val rootCanonical = root.canonicalFile
        if (canonicalDir.path.startsWith(rootCanonical.path + File.separator)) {
            canonicalDir.deleteRecursively()
        }
    }

    /**
     * The name a driver's results are kept under (Prefs.driverVerdicts): the
     * catalog's for one fetched from it or carried by the app, "dir:<folder>"
     * for one the player imported.
     */
    fun idOf(driver: Driver, prefs: Prefs, catalog: DriverCatalog?): String {
        try {
            val marked = File(driver.directory, CATALOG_MARK).readText().trim()
            if (marked.isNotEmpty()) return marked
        } catch (_: java.io.IOException) {
            // Not one of the catalog's by its folder; perhaps the carried one.
        }
        if (driver.directory.absolutePath == prefs.bundledDriverDirectory) {
            catalog?.bySha(prefs.bundledDriverSha)?.let { return it.id }
        }
        return "dir:${driver.directory.name}"
    }

    /** The unpacked driver that is the catalog's [entry], or null when it is not on this phone. */
    fun ofCatalog(entry: DriverCatalog.Entry, prefs: Prefs): Driver? {
        if (prefs.bundledDriverSha == entry.sha256) bundled(prefs)?.let { return it }
        val folder = File(root, entry.id)
        val marked = try {
            File(folder, CATALOG_MARK).readText().trim()
        } catch (_: java.io.IOException) {
            ""
        }
        return if (marked == entry.id) read(folder) else null
    }

    /**
     * Makes the driver the build carries the default on Qualcomm phones,
     * where the stock driver renders this game slowly and with corrupted
     * textures and Turnip does not. Three packages, all put in place at build
     * time by android/fetch_deps.sh and never committed: from Android 11 on
     * the community Turnip v27.0.0-R2 (assets/drivers/turnip_v27_r2.zip), on
     * an Adreno 8xx The412Banner's A8xx Turnip of 21 November 2026
     * (turnip_a8xx.zip), and on Android 10 the project's own build of Mesa
     * (turnip_api29.zip, android/build_turnip.sh): every community Turnip
     * asks for Android 11's libc and does not load on Android 10
     * (bundledAssetFor).
     *
     * It only ever fills a gap. Once the player has picked anything, the
     * system's driver included (Prefs.driverChoiceMade), it is never
     * selected for them again; a newer bundled package after an app update
     * is still unpacked over the old one so the list does not go stale, and
     * a player who picked something before this phone was ever given the
     * package finds it in the list and in the black screen's message.
     * Called from the launcher before the run starts, so the :game process
     * finds the choice already written. A failure here leaves the system's
     * driver in use, which is what the app did before.
     */
    fun installBundledDefault(prefs: Prefs) {
        try {
            installBundled(prefs)
        } catch (error: Throwable) {
            Trail.note(context, "driver: bundled default failed: ${error.message}")
        }
    }

    private fun installBundled(prefs: Prefs) {
        // A build without the asset (the download was skipped) has nothing
        // to offer, and everything below is then left exactly as it was.
        val asset = bundledAsset()
        val sha = bundledSha(asset) ?: return

        // An install that predates this has a driver the player picked
        // (the default records its hash the moment it is installed, so a
        // non-empty folder with no hash cannot be ours): that is a choice.
        if (!prefs.driverChoiceMade && prefs.driverDirectory.isNotEmpty() && prefs.bundledDriverSha.isEmpty()) {
            prefs.driverChoiceMade = true
        }
        if (!QualcommSoc.detect { DeviceInfo.gpu(context) }) return

        val previousDirectory = prefs.bundledDriverDirectory
        val previous = if (previousDirectory.isNotEmpty()) read(File(previousDirectory)) else null
        if (prefs.bundledDriverSha == sha && previous != null) {
            val path = previous.directory.absolutePath
            if (!prefs.driverChoiceMade && prefs.driverDirectory != path) select(prefs, previous)
            return
        }
        // The player removed it and nothing says it should come back. One
        // who chose before ever having it -- an Android 10 or 11 Snapdragon,
        // which the app took for another chip until QualcommSoc -- gets it
        // unpacked, and not selected.
        if (prefs.driverChoiceMade && previous == null && prefs.bundledDriverSha.isNotEmpty()) return

        val outcome = context.assets.open(asset).use { import(it, "Turnip") }
        val driver = (outcome as? Outcome.Imported)?.driver
        if (driver == null) {
            Trail.note(context, "driver: bundled package rejected: ${(outcome as? Outcome.Failed)?.reason}")
            return
        }

        val wasSelected = previous != null && prefs.driverDirectory == previous.directory.absolutePath
        prefs.bundledDriverSha = sha
        prefs.bundledDriverDirectory = driver.directory.absolutePath
        // A package that renamed itself lands in a new folder; the old one
        // is only ours to delete because we made it.
        if (previous != null && previous.directory.canonicalPath != driver.directory.canonicalPath) remove(previous)
        if (!prefs.driverChoiceMade || wasSelected) select(prefs, driver)
        Trail.note(context, "driver: bundled ${driver.name} (${driver.version}) installed")
    }

    /** Selecting is not choosing: the choice flag is left alone. */
    private fun select(prefs: Prefs, driver: Driver) {
        prefs.driverDirectory = driver.directory.absolutePath
        prefs.driverLibrary = driver.library
    }

    private fun bundledAsset(): String =
        bundledAssetFor(Build.VERSION.SDK_INT) { DeviceInfo.gpu(context) }

    /** The asset's SHA-256, or null when this build does not carry one. */
    private fun bundledSha(asset: String): String? {
        val stream = try {
            context.assets.open(asset)
        } catch (_: java.io.IOException) {
            return null
        }
        return sha256(stream)
    }

    private fun safeName(name: String): String {
        val cleaned = name
            .map { if (it.isLetterOrDigit() || it == '-' || it == '_') it else '_' }
            .joinToString("")
        val result = cleaned.trim('_', '.').ifEmpty { "driver" }.take(48)
        return if (result == "." || result == ".." || result.isEmpty()) "driver" else result
    }

    /**
     * The first bytes of an ELF say what machine it is for. A driver built
     * for the wrong one would fail to load with a message nobody can act
     * on; this way the app can say so plainly.
     */
    private fun looksLikeArm64(library: File): Boolean = try {
        library.inputStream().use { stream ->
            val header = ByteArray(20)
            if (stream.read(header) < 20) false
            else {
                val elf = header[0] == 0x7F.toByte() && header[1] == 'E'.code.toByte() &&
                    header[2] == 'L'.code.toByte() && header[3] == 'F'.code.toByte()
                val sixtyFour = header[4].toInt() == 2
                // e_machine, little endian, at offset 18: 0xB7 is AArch64.
                val machine = (header[18].toInt() and 0xFF) or ((header[19].toInt() and 0xFF) shl 8)
                elf && sixtyFour && machine == 0xB7
            }
        }
    } catch (_: Throwable) {
        false
    }

    /**
     * Zip extraction with the traversal check every unzip needs: an entry
     * named ../../something would otherwise write wherever it liked.
     */
    private fun unzip(stream: InputStream, into: File) {
        ZipInputStream(stream.buffered()).use { zip ->
            var total = 0L
            while (true) {
                val entry = zip.nextEntry ?: break
                val destination = File(into, entry.name).canonicalFile
                if (!destination.path.startsWith(into.canonicalFile.path + File.separator)) {
                    zip.closeEntry()
                    continue
                }
                if (entry.isDirectory) {
                    destination.mkdirs()
                } else {
                    destination.parentFile?.mkdirs()
                    destination.outputStream().use { out ->
                        val buffer = ByteArray(64 * 1024)
                        while (true) {
                            val read = zip.read(buffer)
                            if (read <= 0) break
                            total += read
                            // A driver is a few tens of megabytes; anything
                            // beyond this is not one.
                            if (total > 512L * 1024 * 1024) throw IllegalStateException("too large")
                            out.write(buffer, 0, read)
                        }
                    }
                }
                zip.closeEntry()
            }
        }
    }

    companion object {
        const val BUNDLED_ASSET = "drivers/turnip_v27_r2.zip"
        const val BUNDLED_ASSET_A8XX = "drivers/turnip_a8xx.zip"
        const val BUNDLED_ASSET_API29 = "drivers/turnip_api29.zip"

        /** In a driver's folder: the catalog's name for it, written only for a package whose SHA-256 was the catalog's. */
        private const val CATALOG_MARK = ".catalog"

        /** The SHA-256 of everything [stream] holds, in hex; the stream is closed here. */
        fun sha256(stream: InputStream): String {
            val digest = MessageDigest.getInstance("SHA-256")
            stream.use {
                val buffer = ByteArray(64 * 1024)
                while (true) {
                    val count = it.read(buffer)
                    if (count < 0) break
                    digest.update(buffer, 0, count)
                }
            }
            return digest.digest().joinToString("") { "%02x".format(it) }
        }

        /**
         * Which package this phone is given. The community builds need
         * bionic's LIBC_R (API 30), the project's own only LIBC_P, so below
         * API 30 only the latter can load. An Adreno 8xx (no phone with one
         * runs Android 10) gets The412Banner's A8xx build: a player on a
         * Snapdragon 8 Elite plays with it, and the 8 Elite's own driver
         * V@0800.71 crashes in its shader compiler on one of Advanced
         * Warfare's pipelines (AW 0.1.5), while the project's unpatched Mesa
         * "doesn't work properly" there for the same player (AW 0.1.6): Mesa
         * main does not yet read the 8 Elite's SnapAlloc window buffers' layout
         * (MR 44464) nor invalidate bindless descriptors the a8xx way, both
         * of which the A8xx build carries. [gpu] is
         * DeviceInfo.gpu's text ("Adreno (TM) 830 (Qualcomm), OpenGL ES ..."),
         * asked only from API 30 on.
         */
        fun bundledAssetFor(sdk: Int, gpu: () -> String): String = when {
            sdk < 30 -> BUNDLED_ASSET_API29
            adreno8xx(gpu()) -> BUNDLED_ASSET_A8XX
            else -> BUNDLED_ASSET
        }

        /** An Adreno 810 ... 840 by its OpenGL ES renderer's name; "Adreno 8c Gen 3" is a 6xx and does not count. */
        fun adreno8xx(gpu: String): Boolean = ADRENO_8XX.containsMatchIn(gpu)

        private val ADRENO_8XX = Regex("""Adreno \(TM\) 8\d\d\b""")
    }
}
