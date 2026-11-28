package com.mw2.recomp

import android.content.Context
import android.net.ConnectivityManager
import android.os.Build
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL

/**
 * Files fetched for the driver screen: the catalog, a driver the catalog
 * names, a zip from a link the player gave. HTTPS only, redirects included
 * (a debug build may be pointed at a server on the PC beside it, as the
 * update check may), and never more than [limit] bytes.
 */
object DriverFetch {
    const val DRIVER_LIMIT = 96L * 1024 * 1024
    private const val CATALOG_LIMIT = 1L * 1024 * 1024

    private fun open(address: String): HttpURLConnection {
        val url = URL(address)
        val beside = BuildConfig.DEBUG && url.host == "127.0.0.1"
        if (url.protocol != "https" && !beside) throw IOException("not an https address")
        val connection = url.openConnection() as HttpURLConnection
        connection.connectTimeout = 20_000
        connection.readTimeout = 30_000
        connection.setRequestProperty("User-Agent", "recomp-android")
        val code = connection.responseCode
        // A redirect is followed only to another HTTPS address (HttpURLConnection never changes protocol).
        if (connection.url.protocol != "https" && !beside) {
            connection.disconnect()
            throw IOException("redirected away from https")
        }
        if (code != HttpURLConnection.HTTP_OK) {
            connection.disconnect()
            throw IOException("the server answered $code")
        }
        return connection
    }

    /** The file at [address] into [target]; returns its SHA-256. [progress] hears the bytes done and their number (-1 unknown). */
    fun download(address: String, target: File, limit: Long = DRIVER_LIMIT, progress: (Long, Long) -> Unit = { _, _ -> }): String {
        val connection = open(address)
        val partial = File(target.path + ".part")
        try {
            val total = connection.contentLengthLong
            if (total > limit) throw IOException("the file is larger than a driver is")
            val digest = java.security.MessageDigest.getInstance("SHA-256")
            connection.inputStream.use { input ->
                partial.outputStream().use { output ->
                    val buffer = ByteArray(1 shl 16)
                    var done = 0L
                    while (true) {
                        val read = input.read(buffer)
                        if (read <= 0) break
                        done += read
                        if (done > limit) throw IOException("the file is larger than a driver is")
                        digest.update(buffer, 0, read)
                        output.write(buffer, 0, read)
                        progress(done, total)
                    }
                }
            }
            target.delete()
            if (!partial.renameTo(target)) throw IOException("the download could not be kept")
            return digest.digest().joinToString("") { "%02x".format(it) }
        } finally {
            connection.disconnect()
            partial.delete()
        }
    }

    fun catalogText(address: String): String {
        val connection = open(address)
        try {
            val bytes = connection.inputStream.use { input ->
                val out = java.io.ByteArrayOutputStream()
                val buffer = ByteArray(1 shl 14)
                while (true) {
                    val read = input.read(buffer)
                    if (read <= 0) break
                    out.write(buffer, 0, read)
                    if (out.size() > CATALOG_LIMIT) throw IOException("too large for a catalog")
                }
                out.toByteArray()
            }
            return String(bytes, Charsets.UTF_8)
        } finally {
            connection.disconnect()
        }
    }

    /** The first https address in what another app shared: a link, or a message with one in it. */
    fun linkIn(text: String): String? =
        Regex("""https://[^\s<>"']+""").find(text)?.value?.trimEnd('.', ',', ')', ']')
}

/**
 * The driver the app chooses by itself: on the first start, and again when
 * the app, the driver catalog or the system has changed.
 *
 * The phone's chip is read from its OpenGL ES driver (DeviceInfo.gpu), the
 * catalog ranks what exists for it (DriverCatalog.ranked), and the best is
 * made ready -- the app's own copy, or a download whose SHA-256 is the
 * catalog's -- and then proven: the self-test (DriverSelfTest) has to draw
 * its frame right on this phone before the driver is selected. One that
 * fails is written down and the next is tried, down to the phone's own.
 *
 * A player who has chosen a driver keeps it (Prefs.driverChoiceMade): it is
 * tested once, and they are told if it fails.
 */
object DriverAuto {

    /** What a pass over the choices came to. */
    class Outcome(
        /** For the launcher to show, in the player's language, or null. */
        val message: String?,
        /** A better driver that was not fetched because the phone is on mobile data: ask, and run again with leave. */
        val wantsDownload: DriverCatalog.Entry?,
    )

    private const val CATALOG_ASSET = "drivers/catalog.json"
    private const val CATALOG_FILE = "driver-catalog.json"
    private const val CATALOG_EVERY_MS = 24L * 60 * 60 * 1000
    private val lock = Any()
    private const val NOT_STARTED = DriverSelfTest.NOT_STARTED

    /** The catalog this build carries, or a newer one fetched since. Never null: an app without one has an empty catalog. */
    fun catalog(context: Context): DriverCatalog {
        val carried = try {
            context.assets.open(CATALOG_ASSET).use { DriverCatalog.parse(it.readBytes().toString(Charsets.UTF_8)) }
        } catch (_: IOException) {
            null
        }
        val fetched = try {
            DriverCatalog.parse(File(context.filesDir, CATALOG_FILE).readText())
        } catch (_: IOException) {
            null
        }
        return listOfNotNull(carried, fetched).maxByOrNull { it.version } ?: DriverCatalog(0, emptyList(), emptyList())
    }

    /**
     * Asks the project's release for a newer catalog, once a day at the
     * most. Blocks; a failure leaves what there is.
     */
    fun refreshCatalog(context: Context, prefs: Prefs, now: Long = System.currentTimeMillis(), force: Boolean = false) {
        if (BuildConfig.DRIVER_CATALOG_URL.isEmpty()) return
        if (!force && now - prefs.driverCatalogAt in 0 until CATALOG_EVERY_MS) return
        prefs.driverCatalogAt = now
        try {
            val text = DriverFetch.catalogText(BuildConfig.DRIVER_CATALOG_URL)
            val fetched = DriverCatalog.parse(text) ?: throw IOException("not a catalog this app reads")
            if (fetched.version > catalog(context).version) {
                File(context.filesDir, CATALOG_FILE).writeText(text)
                log(context, "catalog ${fetched.version} fetched (${fetched.drivers.size} drivers)")
            }
        } catch (error: Exception) {
            log(context, "the catalog could not be fetched: ${error.message}")
        }
    }

    fun metered(context: Context): Boolean =
        (context.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager).isActiveNetworkMetered

    /** The line DeviceInfo.gpu writes, and the chip it names. */
    fun gpuText(context: Context): String = DeviceInfo.gpu(context)

    private fun key(catalog: DriverCatalog, gpuText: String): String =
        "${BuildConfig.VERSION_CODE}/${BuildConfig.BUILD_SHA}/${catalog.version}/${Build.FINGERPRINT}/${gpuText.hashCode()}"

    /** A `driver:` line of the launcher's log, and of the trail a crash leaves. */
    fun log(context: Context, text: String) {
        Trail.note(context, "driver: $text")
        if (NativeBridge.isLoaded()) {
            try {
                NativeBridge.nativeLog(false, "driver: $text")
            } catch (_: Throwable) {
                // The library is loaded but not started: the trail has it.
            }
        }
    }

    /** What has failed on this phone, by the names Prefs.driverVerdicts keeps. */
    fun failed(prefs: Prefs): Set<String> =
        prefs.driverVerdicts.filter { it.value.startsWith("fail") && !it.key.startsWith("strikes:") }.keys

    /**
     * Makes the catalog's [entry] ready on this phone: unpacked already, the
     * app's own copy, or downloaded, and in every case only a package whose
     * SHA-256 is the entry's. Throws an [IOException] that says what went
     * wrong, in English as the log is.
     */
    fun install(context: Context, prefs: Prefs, entry: DriverCatalog.Entry, progress: (Long, Long) -> Unit = { _, _ -> }): DriverStore.Driver {
        val store = DriverStore(context)
        store.ofCatalog(entry, prefs)?.let { return it }
        if (Build.VERSION.SDK_INT < entry.minApi) throw IOException("it needs Android API ${entry.minApi}")
        val file = File(context.cacheDir, "driver-${entry.id}.zip")
        try {
            var have = false
            if (entry.asset.isNotEmpty()) {
                // The build's own copy, where it carries this very package.
                try {
                    context.assets.open(entry.asset).use { input -> file.outputStream().use { input.copyTo(it, 1 shl 16) } }
                    have = file.inputStream().let(DriverStore::sha256) == entry.sha256
                } catch (_: IOException) {
                    have = false
                }
            }
            if (!have) {
                if (entry.url.isEmpty() || !DriverCatalog.allowedUrl(entry.url)) throw IOException("this build does not carry it and it is published nowhere")
                val sha = DriverFetch.download(entry.url, file, progress = progress)
                if (sha != entry.sha256) throw HashMismatch("the download's SHA-256 is $sha, the catalog's ${entry.sha256}")
            }
            return when (val outcome = store.import(file.inputStream(), entry.name, catalogId = entry.id)) {
                is DriverStore.Outcome.Imported -> outcome.driver
                is DriverStore.Outcome.Failed -> throw IOException(outcome.reason)
            }
        } finally {
            file.delete()
        }
    }

    class HashMismatch(message: String) : IOException(message)

    /** Runs the self-test on [driver] (null: the phone's own) and writes down what it said under [id]. Blocks. */
    fun test(context: Context, prefs: Prefs, driver: DriverStore.Driver?, id: String): DriverSelfTest.Result {
        val result = DriverSelfTest.run(context, driver)
        val name = driver?.name ?: "the phone's own driver"
        log(context, "self-test of $name [$id]: ${result.line()}")
        // A test that could not be run says nothing of the driver.
        if (result.passed || !result.reason.startsWith(NOT_STARTED)) {
            val before = prefs.driverVerdicts[id]
            // A run that showed a picture is not undone by a later pass.
            if (!(result.passed && before == "good")) prefs.noteDriverVerdict(id, if (result.passed) "ok" else "fail: ${result.reason}")
        }
        val checks = prefs.driverChecks.lineSequence().filter { it.isNotEmpty() && !it.startsWith("$id: ") }.toList()
        prefs.driverChecks = (checks + "$id: ${result.line()}").takeLast(8).joinToString("\n")
        return result
    }

    /** Selecting is not choosing: Prefs.driverChoiceMade is left alone. */
    fun select(prefs: Prefs, driver: DriverStore.Driver?, id: String) {
        prefs.driverDirectory = driver?.directory?.absolutePath.orEmpty()
        prefs.driverLibrary = driver?.library.orEmpty()
        prefs.driverPicked = id
        prefs.flush()
    }

    /** The driver the settings name now and the name its results are kept under. */
    fun current(context: Context, prefs: Prefs, catalog: DriverCatalog = catalog(context)): Pair<DriverStore.Driver?, String> {
        val directory = prefs.driverDirectory
        if (directory.isEmpty()) return null to DriverCatalog.SYSTEM
        val store = DriverStore(context)
        val driver = store.list().firstOrNull { it.directory.absolutePath == directory } ?: return null to DriverCatalog.SYSTEM
        return driver to store.idOf(driver, prefs, catalog)
    }

    /**
     * The whole pass: see the class comment. Blocks for the downloads and
     * the tests it makes; not for the main thread. [allowMetered]: the
     * player said a download over mobile data is fine. [force]: again even
     * though nothing has changed (a driver was struck out).
     */
    fun run(context: Context, prefs: Prefs, allowMetered: Boolean = false, force: Boolean = false): Outcome = synchronized(lock) {
        val app = context.applicationContext
        refreshCatalog(app, prefs)
        val catalog = catalog(app)
        val text = gpuText(app)
        val gpu = Gpu.parse(text)
        val key = key(catalog, text)
        val (selected, selectedId) = current(app, prefs, catalog)
        if (!force && prefs.driverAutoKey == key && (prefs.driverChoiceMade || prefs.driverPicked == selectedId)) return Outcome(null, null)
        // Something changed: what failed before may not fail now, and what drew is watched once more.
        // Not when a run struck a driver out and asked for this pass (fallBack): that is to be remembered.
        if (prefs.driverVerdictsKey != key) {
            prefs.driverVerdicts = emptyMap()
            prefs.driverVerdictsKey = key
        }
        log(app, "choosing for ${gpu.vendor} ${gpu.model} (${gpu.name.ifEmpty { "unnamed" }}), Android API ${Build.VERSION.SDK_INT}," +
            " ${DriverCatalog.game}, catalog ${catalog.version}" + if (prefs.driverChoiceMade) "; the player's choice stays" else "")

        if (prefs.driverChoiceMade) {
            val result = test(app, prefs, selected, selectedId)
            // Not started (the app went out of sight): asked again at the next start.
            if (result.reason.startsWith(NOT_STARTED)) return Outcome(null, null)
            prefs.driverAutoKey = key
            val name = selected?.name ?: app.getString(R.string.driver_system)
            return Outcome(if (result.passed) null else app.getString(R.string.driver_choice_failed, name, result.reason), null)
        }

        val ranked = catalog.ranked(gpu, Build.VERSION.SDK_INT, DriverCatalog.game, text, failed(prefs))
        log(app, "ranked: " + ranked.joinToString(", ") { "${it.id} (${it.score}${if (it.knownGood) ", known good" else ""})" }.ifEmpty { "nothing" })
        var wants: DriverCatalog.Entry? = null
        var unfinished = false
        var firstFailure: String? = null
        for (choice in ranked) {
            val entry = choice.entry
            val driver = if (entry == null) null else try {
                val unpacked = DriverStore(app).ofCatalog(entry, prefs)
                val carried = entry.asset.isNotEmpty() && carries(app, entry)
                if (unpacked == null && !carried && metered(app) && !allowMetered) {
                    // Not fetched without leave: the next choice serves until the player answers.
                    if (wants == null) wants = entry
                    log(app, "${entry.id} is a download and the phone is on a metered network: the player is asked")
                    continue
                }
                install(app, prefs, entry)
            } catch (error: IOException) {
                log(app, "${entry.id} could not be made ready: ${error.message}")
                // A file that is not the catalog's is never tried again; no network is.
                if (error is HashMismatch) prefs.noteDriverVerdict(entry.id, "fail: ${error.message}") else unfinished = true
                continue
            }
            val result = test(app, prefs, driver, choice.id)
            if (!result.passed) {
                if (result.reason.startsWith(NOT_STARTED)) return Outcome(null, null)
                if (firstFailure == null) firstFailure = "${entry?.name ?: app.getString(R.string.driver_system)}: ${result.reason}"
                continue
            }
            val changed = prefs.driverPicked.isNotEmpty() && prefs.driverPicked != choice.id
            val before = prefs.driverPicked
            select(prefs, driver, choice.id)
            log(app, "selected ${choice.id}" + if (changed) " in place of $before" else "")
            if (wants == null && !unfinished) prefs.driverAutoKey = key
            val name = entry?.name ?: app.getString(R.string.driver_system)
            val message = when {
                firstFailure != null -> app.getString(R.string.driver_auto_switched, firstFailure.substringBefore(": "), firstFailure.substringAfter(": "), name)
                changed || force -> app.getString(R.string.driver_test_passed, name)
                else -> null
            }
            return Outcome(message, wants)
        }
        // Nothing passed: the phone's own stays in use, which is what a run opens when all else fails.
        select(prefs, null, DriverCatalog.SYSTEM)
        if (wants == null && !unfinished) prefs.driverAutoKey = key
        log(app, "no driver passed the self-test")
        Outcome(app.getString(R.string.driver_auto_none, firstFailure ?: "-"), wants)
    }

    private fun carries(context: Context, entry: DriverCatalog.Entry): Boolean = try {
        context.assets.open(entry.asset).let(DriverStore::sha256) == entry.sha256
    } catch (_: IOException) {
        false
    }

    /**
     * The driver that would come after [failedId]: the best ranked one that
     * has not failed here and is unpacked on the phone already (the phone's
     * own always is), with the name its results are kept under. Null where
     * nothing is left.
     */
    fun nextReady(context: Context, prefs: Prefs, failedId: String): Pair<DriverStore.Driver?, String>? {
        val catalog = catalog(context)
        val store = DriverStore(context)
        val text = gpuText(context)
        val ranked = catalog.ranked(Gpu.parse(text), Build.VERSION.SDK_INT, DriverCatalog.game, text, failed(prefs) + failedId)
        val ready = DriverFallback.next(ranked, failedId) { choice ->
            choice.entry == null || store.ofCatalog(choice.entry, prefs) != null
        } ?: return null
        return ready.entry?.let { store.ofCatalog(it, prefs) } to ready.id
    }

    /**
     * A run could not show the game on the driver [failedId] ([why], in
     * English): it is written down, and the next driver that is ready on
     * the phone is selected. Returns that driver's name for the player, or
     * null where nothing is left, or where the driver was the player's own
     * choice and they have not asked for another ([asked]). Does not
     * download or test: this is the run's process, on its way out.
     */
    fun fallBack(context: Context, prefs: Prefs, failedId: String, why: String, asked: Boolean = false): String? {
        prefs.noteDriverVerdict(failedId, "fail: $why")
        log(context, "$failedId failed in a run: $why")
        if (prefs.driverChoiceMade && !asked) return null
        val (driver, id) = nextReady(context, prefs, failedId) ?: return null
        select(prefs, driver, id)
        // The launcher's next pass tests it, and fetches a better one if there is one.
        prefs.driverAutoKey = ""
        log(context, "selected $id in its place")
        return driver?.name ?: context.getString(R.string.driver_system)
    }

    /**
     * After a run, in the launcher: one that ended in a crash inside the
     * graphics driver counts against the driver it ran on, and at
     * [DriverFallback.STRIKES] the driver is struck out as one that failed
     * its self-test is. Returns what to tell the player, or null.
     */
    fun afterRun(context: Context, prefs: Prefs): String? {
        val id = prefs.driverTrial
        if (id.isEmpty()) return null
        val crash = DeviceInfo.lastRunCrash(context) ?: return null
        if (crash.first <= prefs.driverCrashSeen) return null
        prefs.driverCrashSeen = crash.first
        val (driver, currentId) = current(context, prefs)
        if (currentId != id || !DriverFallback.crashInDriver(crash.second, driver?.library.orEmpty())) return null
        val strikes = (prefs.driverVerdicts["strikes:$id"]?.toIntOrNull() ?: 0) + 1
        log(context, "the last run crashed inside the graphics driver ($id, $strikes of ${DriverFallback.STRIKES})")
        if (strikes < DriverFallback.STRIKES) {
            prefs.noteDriverVerdict("strikes:$id", strikes.toString())
            return null
        }
        prefs.noteDriverVerdict("strikes:$id", "0")
        val why = "the game crashed inside it $strikes times"
        val failedName = driver?.name ?: context.getString(R.string.driver_system)
        val next = fallBack(context, prefs, id, why)
        return if (next != null) context.getString(R.string.driver_auto_switched, failedName, why, next)
        else context.getString(R.string.driver_choice_failed, failedName, why)
    }
}
