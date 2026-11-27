package com.mw2.recomp

import android.app.Activity
import android.app.ActivityManager
import android.app.job.JobInfo
import android.app.job.JobParameters
import android.app.job.JobScheduler
import android.app.job.JobService
import android.content.ComponentName
import android.content.Context
import android.os.Build
import android.os.PowerManager
import android.text.util.Linkify
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AlertDialog
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.security.SecureRandom
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone
import kotlin.concurrent.thread
import kotlin.math.roundToInt

/**
 * A performance report: what a player who said yes sends after a session, so
 * that the settings can be tuned for each chip (docs/perf-reports.md,
 * PRIVACY.md). This object is the report alone, with nothing of Android in
 * it (PerfReportTest): the list of its fields, which is all a report ever
 * holds, and how each is made fit to send.
 */
object PerfReport {
    /** Every field of a report, in the order it is written. The server refuses any other. */
    val FIELDS = listOf(
        "v", "install", "model", "chip", "gpu", "driver", "driver_version", "android", "app", "game",
        "game_version", "mode", "map", "minutes", "fps", "low1", "limit", "preset", "scene_scale", "shadows",
        "fps_limit", "multiplier", "thermal",
    )

    private val WHOLE = setOf("v", "minutes", "scene_scale", "fps_limit", "multiplier", "thermal")
    private val NUMBERS = setOf("fps", "low1")
    private const val VERSION = "1"

    /** A session shorter than this says little of the phone, and is not sent. */
    const val LEAST_SECONDS = 60

    private const val NAME_LONGEST = 64
    private const val NAME_PUNCTUATION = " ._+-(),:@#"

    /**
     * A name fit to send: letters, digits and a little punctuation, never a
     * slash, so nothing shaped like a path leaves the phone whatever a driver
     * or a phone maker put in its name.
     */
    fun name(text: String): String =
        text.map { if (it in 'a'..'z' || it in 'A'..'Z' || it in '0'..'9' || it in NAME_PUNCTUATION) it else ' ' }
            .joinToString("").replace(Regex(" +"), " ").trim().take(NAME_LONGEST).trim()

    /** A map's name as the game has it ("mp_dome"), or nothing where what was read is not one. */
    fun map(text: String): String = if (Regex("[a-z0-9_]{1,40}").matches(text)) text else ""

    /** The lines of name=value the run kept (NativeBridge.nativeSession, PerfSession) as a map. */
    fun lines(text: String): Map<String, String> =
        text.lineSequence().mapNotNull { line ->
            val at = line.indexOf('=')
            if (at <= 0) null else line.substring(0, at) to line.substring(at + 1)
        }.toMap()

    /**
     * The report as it is sent, from what the run kept and what the app
     * knows of the phone: the listed fields and no other, whatever else
     * [kept] holds. Null for a session too short to say anything, or one
     * whose numbers are not there.
     */
    fun build(install: String, phone: Map<String, String>, kept: Map<String, String>): String? {
        val seconds = kept["seconds"]?.toDoubleOrNull() ?: return null
        val fps = kept["fps"]?.toDoubleOrNull() ?: return null
        val low = kept["low1"]?.toDoubleOrNull() ?: return null
        if (seconds < LEAST_SECONDS || fps < 1.0) return null
        val whole = { name: String, least: Int, most: Int, otherwise: Int ->
            (kept[name]?.toIntOrNull() ?: otherwise).coerceIn(least, most).toString()
        }
        val one = { name: String, choices: List<String> -> kept[name].takeIf { it in choices } ?: choices.last() }
        val values = mapOf(
            "v" to VERSION,
            "install" to install,
            "model" to name(phone["model"].orEmpty()),
            "chip" to name(phone["chip"].orEmpty()),
            "gpu" to name(kept["gpu"].orEmpty()),
            "driver" to name(kept["driver"].orEmpty()),
            "driver_version" to name(kept["driver_version"].orEmpty()),
            "android" to name(phone["android"].orEmpty()),
            "app" to name(phone["app"].orEmpty()),
            "game" to name(kept["game"].orEmpty()),
            "game_version" to name(kept["game_version"].orEmpty()),
            "mode" to one("mode", listOf("multiplayer", "campaign")),
            "map" to map(kept["map"].orEmpty()),
            "minutes" to (seconds / 60).roundToInt().coerceIn(1, 1440).toString(),
            "fps" to String.format(Locale.US, "%.1f", fps.coerceIn(1.0, 1000.0)),
            "low1" to String.format(Locale.US, "%.1f", low.coerceIn(0.1, 1000.0)),
            "limit" to one("limit", listOf("gpu", "cpu", "unknown")),
            "preset" to name(kept["preset"].orEmpty()),
            "scene_scale" to whole("scene_scale", 10, 100, 100),
            "shadows" to one("shadows", listOf("on", "off", "auto")),
            "fps_limit" to whole("fps_limit", 0, 1000, 0),
            "multiplier" to whole("multiplier", 1, 4, 1),
            "thermal" to whole("thermal", -1, 6, -1),
        )
        // No value has a quote or a backslash in it (name), so none needs escaping.
        return FIELDS.joinToString(",", "{", "}") { field ->
            val value = values.getValue(field)
            if (field in WHOLE || field in NUMBERS) "\"$field\":$value" else "\"$field\":\"$value\""
        }
    }

    /** A report's fields and values, in its order. */
    fun fields(report: String): List<Pair<String, String>> =
        Regex("\"([a-z0-9_]+)\":(?:\"([^\"]*)\"|(-?[0-9.]+))").findAll(report)
            .map { it.groupValues[1] to it.groupValues[2].ifEmpty { it.groupValues[3] } }.toList()

    private val LABELS = mapOf(
        "install" to "Random number (changes every month)", "model" to "Phone", "chip" to "Chip",
        "gpu" to "Graphics chip", "driver" to "Graphics driver", "driver_version" to "Driver version",
        "android" to "Android", "app" to "App version", "game" to "Game", "game_version" to "Game version",
        "mode" to "Mode", "map" to "Map", "minutes" to "Minutes played", "fps" to "Average FPS",
        "low1" to "1% low FPS", "limit" to "Held back by", "preset" to "Graphics preset",
        "scene_scale" to "Scene scale (%)", "shadows" to "Shadows", "fps_limit" to "FPS limit (0 is automatic)",
        "multiplier" to "Resolution multiplier", "thermal" to "Hottest (0 cool to 6, -1 not known)",
    )

    /** The report for a player to read: a line for each thing it holds, and nothing left out but its format's version. */
    fun readable(report: String): String =
        fields(report).filter { it.first != "v" }.joinToString("\n") { (field, value) ->
            "${LABELS[field] ?: field}: ${value.ifEmpty { "-" }}"
        }

    /** An example for the question's "Show me an example": a session on a phone that is nobody's. */
    val EXAMPLE: String = build(
        "3f9a1c0e5b7d42a68c1e9f0b2d4a6c8e",
        mapOf(
            "model" to "AYN Thor", "chip" to "QTI SM8550, board kalama", "android" to "13",
            "app" to "0.1.7",
        ),
        mapOf(
            "seconds" to "1380", "fps" to "58.6", "low1" to "41.2", "limit" to "gpu", "map" to "mp_dome",
            "gpu" to "Turnip Adreno (TM) 740", "driver" to "turnip Mesa driver", "driver_version" to "Mesa 25.3.0",
            "game" to "mw3", "game_version" to "disc", "mode" to "multiplayer", "preset" to "automatic",
            "scene_scale" to "70", "shadows" to "auto", "fps_limit" to "60", "multiplier" to "1", "thermal" to "2",
        ),
    ).orEmpty()

    /**
     * The install's random number for [month] ("2026-11"): the one [kept]
     * for the same month, or a new one from [fresh] -- so reports of one
     * month can be told to be one phone's, and nothing links two months.
     */
    fun install(kept: String?, keptMonth: String?, month: String, fresh: () -> String): String =
        if (keptMonth == month && kept != null && Regex("[0-9a-f]{32}").matches(kept)) kept else fresh()
}

/**
 * The reports on this phone: the player's choice, the session the run keeps
 * as it goes, the queue of those not sent yet and the list of those sent
 * (docs/perf-reports.md).
 *
 *  - Nothing is kept or sent unless the player said yes. Until they are
 *    asked, the run leaves one mark, that a session was played, which is
 *    what the question waits for; and the question is asked only once the
 *    server that takes reports has answered, so nothing waits on the phone
 *    for a server that is not there.
 *  - The run is another process (:game) and is killed when the player ends
 *    it, so it writes what it has every ten seconds to a file, and the
 *    launcher makes the report of it when it is next opened.
 *  - Sent over HTTPS only. A report that could not be sent stays queued for
 *    thirty days and is tried again when the launcher opens and when the
 *    phone has a network (PerfSendJob).
 */
object PerfReports {
    private const val STORE = "perf"
    private const val KEY_CHOICE = "choice"
    private const val KEY_HEALTH_AT = "healthAt"
    private const val KEY_HEALTH = "health"
    private const val KEY_INSTALL = "install"
    private const val KEY_INSTALL_MONTH = "installMonth"
    private const val UNASKED = 0
    private const val YES = 1
    private const val NO = 2

    private const val DAY_MS = 24L * 60 * 60 * 1000
    private const val QUEUE_DAYS = 30
    private const val QUEUE_MOST = 20
    private const val SENT_MOST = 50
    private const val SEND_JOB = 0x5046

    /** How long a run the player has just ended may take to be gone (GameActivity.quit kills it). */
    private const val RUN_ENDS_MS = 3000L

    /** One sender at a time in this process: the launcher's and the job's. */
    private val lock = Any()

    /** The launcher the question is in front of, so that coming back to it does not ask on top of itself. */
    private var asking = java.lang.ref.WeakReference<Activity>(null)

    /** The game as the server names it; null for a game that is not published, whose sessions are nobody's to tune. */
    val game: String? = when {
        Title.isBo3 -> null
        Title.isMw3 -> "mw3"
        Title.isCod4 -> "cod4"
        Title.isAw -> "aw"
        else -> "mw2"
    }

    /** What a report holds and what is done with it, as the public repository says it. */
    val privacyPage: String = "https://github.com/yassinsolim/${game ?: "mw3"}-recomp/blob/main/PRIVACY.md"

    private fun store(context: Context) = context.getSharedPreferences(STORE, Context.MODE_PRIVATE)
    private fun folder(context: Context) = File(context.filesDir, "perf").apply { mkdirs() }
    private fun current(context: Context) = File(folder(context), "current.txt")
    private fun played(context: Context) = File(folder(context), "played")
    private fun queue(context: Context) = File(folder(context), "queue.txt")
    private fun sent(context: Context) = File(folder(context), "sent.txt")

    /** Whether the player said yes. */
    fun chosen(context: Context): Boolean = game != null && store(context).getInt(KEY_CHOICE, UNASKED) == YES

    /** Whether the player has not answered yet: the only time a run leaves its mark that a session was played. */
    fun unasked(context: Context): Boolean = game != null && store(context).getInt(KEY_CHOICE, UNASKED) == UNASKED

    /** The player's answer, from the question or from Settings. Saying no forgets what waited to be sent. */
    fun choose(context: Context, yes: Boolean) {
        store(context).edit().putInt(KEY_CHOICE, if (yes) YES else NO).apply()
        if (!yes) synchronized(lock) {
            current(context).delete()
            queue(context).delete()
        }
    }

    // ---- the run's side (the :game process) --------------------------------

    /** A session was played: all the run leaves behind for a player who has not been asked. */
    fun notePlayed(context: Context) {
        try {
            played(context).createNewFile()
        } catch (_: Throwable) {
            // Asked after another session.
        }
    }

    /** The session so far, whole or not at all: through a temporary name. */
    fun keep(context: Context, lines: Map<String, String>) {
        try {
            val partial = File(folder(context), "current.part")
            partial.writeText(lines.entries.joinToString("\n") { "${it.key}=${it.value}" })
            partial.renameTo(current(context))
        } catch (_: Throwable) {
            // A session that cannot be kept is not reported.
        }
    }

    // ---- the launcher's side ------------------------------------------------

    private fun gameRuns(context: Context): Boolean = try {
        context.getSystemService(ActivityManager::class.java).runningAppProcesses
            .orEmpty().any { it.processName == "${context.packageName}:game" }
    } catch (_: Throwable) {
        false
    }

    private fun phone(): Map<String, String> = mapOf(
        "model" to "${Build.MANUFACTURER} ${Build.MODEL}",
        "chip" to (if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) "${Build.SOC_MANUFACTURER} ${Build.SOC_MODEL}, " else "") +
            "board ${Build.BOARD}",
        "android" to Build.VERSION.RELEASE,
        "app" to BuildConfig.VERSION_NAME,
    )

    private fun month(now: Long): String =
        SimpleDateFormat("yyyy-MM", Locale.US).apply { timeZone = TimeZone.getTimeZone("UTC") }.format(Date(now))

    private fun install(context: Context, now: Long): String {
        val store = store(context)
        val month = month(now)
        return PerfReport.install(store.getString(KEY_INSTALL, null), store.getString(KEY_INSTALL_MONTH, null), month) {
            val bytes = ByteArray(16).also { SecureRandom().nextBytes(it) }
            bytes.joinToString("") { String.format(Locale.US, "%02x", it.toInt() and 0xff) }
                .also { store.edit().putString(KEY_INSTALL, it).putString(KEY_INSTALL_MONTH, month).apply() }
        }
    }

    /** The session a run that has ended kept becomes a report in the queue. */
    private fun finishSession(context: Context, now: Long) = synchronized(lock) {
        val file = current(context)
        if (!file.isFile || gameRuns(context)) return@synchronized
        val kept = try {
            PerfReport.lines(file.readText())
        } catch (_: Throwable) {
            emptyMap()
        }
        file.delete()
        if (!chosen(context)) return@synchronized
        val report = PerfReport.build(install(context, now), phone(), kept) ?: return@synchronized
        val waiting = (queued(context) + "${now / DAY_MS}\t$report").takeLast(QUEUE_MOST)
        queue(context).writeText(waiting.joinToString("\n"))
    }

    private fun queued(context: Context): List<String> = try {
        queue(context).readLines().filter { it.isNotBlank() }
    } catch (_: Throwable) {
        emptyList()
    }

    /** The reports sent from this phone, the newest first: the day each went and what it held. */
    fun sentReports(context: Context): List<Pair<String, String>> = try {
        sent(context).readLines().filter { '\t' in it }.map { it.substringBefore('\t') to it.substringAfter('\t') }.reversed()
    } catch (_: Throwable) {
        emptyList()
    }

    /**
     * A connection to the server's [path], or null where it is not HTTPS: a
     * report never goes in the clear. A debug build may be pointed at a
     * server on the PC beside it (MW2_PERF_URL, adb reverse), as the update
     * check may (docs/perf-reports.md).
     */
    private fun connection(path: String): HttpURLConnection? {
        val url = URL(BuildConfig.PERF_URL + path)
        val beside = BuildConfig.DEBUG && url.host == "127.0.0.1"
        if (url.protocol != "https" && !beside) return null
        return (url.openConnection() as HttpURLConnection).apply {
            connectTimeout = 15000
            readTimeout = 15000
            instanceFollowRedirects = false
        }
    }

    /**
     * Whether the server that takes reports answers, asked once a day at the
     * most. Blocks: not for the main thread.
     */
    fun serverAnswers(context: Context, now: Long = System.currentTimeMillis(), fresh: Boolean = false): Boolean {
        val store = store(context)
        val asked = store.getLong(KEY_HEALTH_AT, 0)
        // The switch in Settings asks anew: a server that was away this morning may be back.
        if (!fresh && now - asked in 0 until DAY_MS) return store.getBoolean(KEY_HEALTH, false)
        val answers = try {
            connection("/health")?.let { it.responseCode == 200 } ?: false
        } catch (_: Throwable) {
            false
        }
        store.edit().putLong(KEY_HEALTH_AT, now).putBoolean(KEY_HEALTH, answers).apply()
        return answers
    }

    /**
     * Sends what waits, the oldest first. True when nothing is left to try:
     * a report the server took is listed as sent, one it refused as not a
     * report is dropped, and at the first it could not take now the rest
     * wait for the next time.
     */
    fun send(context: Context, now: Long = System.currentTimeMillis()): Boolean = synchronized(lock) {
        if (!chosen(context)) return@synchronized true
        val waiting = queued(context).filter {
            (it.substringBefore('\t').toLongOrNull() ?: 0) > now / DAY_MS - QUEUE_DAYS
        }.toMutableList()
        val day = SimpleDateFormat("yyyy-MM-dd", Locale.US).format(Date(now))
        while (waiting.isNotEmpty()) {
            val report = waiting.first().substringAfter('\t')
            val status = try {
                connection("")?.run {
                    requestMethod = "POST"
                    doOutput = true
                    setRequestProperty("Content-Type", "application/json")
                    outputStream.use { it.write(report.toByteArray()) }
                    responseCode
                } ?: 0
            } catch (_: Throwable) {
                0
            }
            if (status == 200) {
                val list = (try { sent(context).readLines() } catch (_: Throwable) { emptyList() }) + "$day\t$report"
                sent(context).writeText(list.takeLast(SENT_MOST).joinToString("\n"))
            } else if (status !in 400..499 || status == 429) {
                break
            }
            waiting.removeAt(0)
            queue(context).writeText(waiting.joinToString("\n"))
        }
        waiting.isEmpty()
    }

    /** Has the queue tried again once the phone has a network. */
    private fun sendLater(context: Context) {
        val job = JobInfo.Builder(SEND_JOB, ComponentName(context, PerfSendJob::class.java))
            .setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY).build()
        (context.getSystemService(Context.JOB_SCHEDULER_SERVICE) as JobScheduler).schedule(job)
    }

    /**
     * The launcher was opened: the last run's session is queued and the
     * queue sent, for a player who said yes; and one who has played a
     * session and has not been asked is asked, once the server answers.
     */
    fun onLauncher(activity: Activity) {
        if (game == null) return
        val context = activity.applicationContext
        val choice = store(context).getInt(KEY_CHOICE, UNASKED)
        if (choice == NO || (choice == UNASKED && !played(context).isFile)) return
        thread(name = "perf-reports", isDaemon = true) {
            if (choice == YES) {
                // The launcher comes back before the run it was left for is gone.
                if (current(context).isFile && gameRuns(context)) Thread.sleep(RUN_ENDS_MS)
                finishSession(context, System.currentTimeMillis())
                if (!send(context)) sendLater(context)
            } else if (serverAnswers(context)) {
                activity.runOnUiThread {
                    if (!activity.isFinishing && !activity.isDestroyed && asking.get() !== activity &&
                        store(context).getInt(KEY_CHOICE, UNASKED) == UNASKED
                    ) {
                        asking = java.lang.ref.WeakReference(activity)
                        ask(activity)
                    }
                }
            }
        }
    }

    // ---- what the player sees ----------------------------------------------

    private fun text(activity: Activity, body: String): ScrollView {
        val view = TextView(activity)
        val pad = (16 * activity.resources.displayMetrics.density).toInt()
        view.setPadding(pad + pad / 2, pad / 2, pad + pad / 2, 0)
        view.textSize = 13f
        view.text = body
        Linkify.addLinks(view, Linkify.WEB_URLS)
        return ScrollView(activity).apply { addView(view) }
    }

    /**
     * The question, asked once: what is sent, in full, with Yes, No and an
     * example. Leaving it without an answer is a no, which Settings changes.
     */
    fun ask(activity: Activity) {
        val context = activity.applicationContext
        AlertDialog.Builder(activity)
            .setTitle(R.string.perf_ask_title)
            .setView(text(activity, activity.getString(R.string.perf_ask_body, privacyPage)))
            .setPositiveButton(R.string.perf_yes) { _, _ -> choose(context, true) }
            .setNegativeButton(R.string.perf_no) { _, _ -> choose(context, false) }
            .setNeutralButton(R.string.perf_example) { _, _ -> showExample(activity) { ask(activity) } }
            .setOnCancelListener { choose(context, false) }
            .show()
    }

    /** What a report looks like, then [back] to where it was asked for. */
    fun showExample(activity: Activity, back: () -> Unit = {}) {
        AlertDialog.Builder(activity)
            .setTitle(R.string.perf_example_title)
            .setView(text(activity, PerfReport.readable(PerfReport.EXAMPLE) + "\n\n" + activity.getString(R.string.perf_example_sent_as) + "\n" + PerfReport.EXAMPLE))
            .setPositiveButton(R.string.perf_back) { _, _ -> back() }
            .setOnCancelListener { back() }
            .show()
    }

    /** Every report sent from this phone, as it was sent. */
    fun showSent(activity: Activity) {
        val reports = sentReports(activity)
        val waiting = queued(activity).size
        val body = buildString {
            if (waiting > 0) append(activity.resources.getQuantityString(R.plurals.perf_waiting, waiting, waiting)).append("\n\n")
            if (reports.isEmpty()) append(activity.getString(R.string.perf_sent_none))
            reports.forEach { (day, report) -> append(day).append('\n').append(PerfReport.readable(report)).append("\n\n") }
        }
        AlertDialog.Builder(activity)
            .setTitle(R.string.perf_sent)
            .setView(text(activity, body.trim()))
            .setPositiveButton(R.string.close, null)
            .show()
    }
}

/**
 * The run's side: every ten seconds of a run, what the session has come to
 * so far (NativeBridge.nativeSession) with the settings in use and how hot
 * the phone has been, kept for the launcher to make a report of
 * (PerfReports) -- for a player who said yes. For one who has not been
 * asked, a mark after a minute that a session was played, and nothing else.
 */
class PerfSession(private val context: Context, private val title: Title, private val prefs: Prefs) {
    private val game = PerfReports.game
    private val reports = PerfReports.chosen(context)
    private val unasked = PerfReports.unasked(context)
    private val power = context.getSystemService(Context.POWER_SERVICE) as PowerManager
    private var ticks = 0
    private var hottest = -1

    /** Twice a second while the run is in front (GameActivity's statsTick). */
    fun tick() {
        if (game == null || ++ticks % EVERY_TICKS != 0) return
        if (!reports) {
            if (unasked && ticks == MARK_TICKS) PerfReports.notePlayed(context)
            return
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) hottest = maxOf(hottest, power.currentThermalStatus)
        val session = try {
            PerfReport.lines(NativeBridge.nativeSession())
        } catch (_: Throwable) {
            // A library without the call: no report of this run.
            return
        }
        PerfReports.keep(
            context,
            session + mapOf(
                "game" to game,
                "game_version" to if (title == Title.MULTIPLAYER && TitleUpdate.inUse) "title update" else "disc",
                "mode" to if (title == Title.MULTIPLAYER) "multiplayer" else "campaign",
                // Settings, Graphics: "automatic", "battery saver", "balanced", "best looking" or "custom".
                "preset" to prefs.graphicsPresetName,
                "scene_scale" to (prefs.resolutionScale * 100).roundToInt().toString(),
                "shadows" to arrayOf("auto", "on", "off")[prefs.sunShadows],
                "fps_limit" to (if (prefs.fpsFollowsDevice) 0 else prefs.fpsLimit).toString(),
                "multiplier" to prefs.renderScale.toString(),
                "thermal" to hottest.toString(),
            ),
        )
    }

    private companion object {
        const val EVERY_TICKS = 20
        const val MARK_TICKS = 120
    }
}

/** The phone has a network again, with reports waiting (PerfReports.sendLater). */
class PerfSendJob : JobService() {
    override fun onStartJob(parameters: JobParameters): Boolean {
        thread(name = "perf-send", isDaemon = true) {
            val done = PerfReports.send(applicationContext)
            jobFinished(parameters, !done)
        }
        return true
    }

    override fun onStopJob(parameters: JobParameters): Boolean = true
}
