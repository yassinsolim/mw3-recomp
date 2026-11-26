package com.mw2.recomp

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.net.ConnectivityManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.StatFs
import android.view.View
import android.widget.Button
import android.widget.ProgressBar
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import java.io.File
import kotlin.concurrent.thread

/**
 * "Set up the game": the one way from a new app to a game, a numbered step
 * to a screen, each with a plain sentence and one large button (Setup.kt
 * says which step, from what is on the device).
 *
 *  1. The player's disc image: found on the device, picked with the system's
 *     picker, or tapped in a folder. It is told by what is in it: another
 *     game's disc, a disc with the multiplayer cut out and a version this
 *     app does not support are each said in a sentence. The game is then
 *     set up to play from the image where it lies, which takes a second
 *     (the runtime's installer, as the install screen runs it). Advanced
 *     Warfare's second disc is asked for in the same step.
 *  2. The build of the game code, both titles in one go, in the service
 *     that keeps it going when the player leaves (GameBuildService): this
 *     screen shows one bar, the time left and Android's question about the
 *     toolchain package.
 *  3. The player's own map packs, which most have none of: skipped with one
 *     button.
 *
 * Then Play. What a player may choose besides, copying the files in, a
 * folder of extracted files, pausing a build, is on the screens the
 * launcher's "Repair / rebuild" leads to (InstallActivity, BuildActivity).
 */
class SetupActivity : AppCompatActivity() {

    private lateinit var prefs: Prefs
    private lateinit var state: GameBuildService.State
    private lateinit var stepLabel: TextView
    private lateinit var heading: TextView
    private lateinit var sentence: TextView
    private lateinit var problem: TextView
    private lateinit var progress: ProgressBar
    private lateinit var progressText: TextView
    private lateinit var note: TextView
    private lateinit var primary: Button
    private lateinit var secondary: Button
    private lateinit var tertiary: Button
    private lateinit var share: Button

    private val handler = Handler(Looper.getMainLooper())

    /** A disc image is being read and set up. */
    @Volatile
    private var checking = false

    /** Why the image last chosen was not taken, in a sentence; null when it was, or none was chosen yet. */
    private var discProblem: String? = null

    /** An image of this game found on the device for the step in hand, which the large button then offers. */
    private var found: File? = null
    private var searching = false

    /** The player has answered the map packs' step, which is put once. */
    private var dlcAnswered = false

    private val document = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
        if (result.resultCode != Activity.RESULT_OK) return@registerForActivityResult
        val data = result.data ?: return@registerForActivityResult
        val chosen = ArrayList<Uri>()
        data.clipData?.let { clip -> (0 until clip.itemCount).mapTo(chosen) { clip.getItemAt(it).uri } }
        if (chosen.isEmpty()) data.data?.let { chosen.add(it) }
        // The permission is kept: the game reads the image on every run.
        chosen.forEach { DiscImage.keep(this, it) }
        take(chosen.map { it.toString() })
    }

    private val browser = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
        val path = result.data?.getStringExtra(FileBrowserActivity.EXTRA_PATH)
        if (result.resultCode == Activity.RESULT_OK && path != null) take(listOf(path))
    }

    private val dlc = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {
        dlcAnswered = true
        show()
    }

    /** The notification is where the player sees the build from elsewhere; it runs without the permission too. */
    private val notifications = registerForActivityResult(ActivityResultContracts.RequestPermission()) { startBuild() }

    private val tick = object : Runnable {
        override fun run() {
            // Android's question about the toolchain package, which a build
            // in the background could not put (ToolchainInstall).
            ToolchainInstall.ask(this@SetupActivity)
            show()
            handler.postDelayed(this, 500)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_setup)
        prefs = Prefs(this)
        state = GameBuildService.State(this)
        dlcAnswered = savedInstanceState?.getBoolean(STATE_DLC_ANSWERED) ?: false
        stepLabel = findViewById(R.id.setup_step)
        heading = findViewById(R.id.setup_heading)
        sentence = findViewById(R.id.setup_sentence)
        problem = findViewById(R.id.setup_problem)
        progress = findViewById(R.id.setup_progress)
        progressText = findViewById(R.id.setup_progress_text)
        note = findViewById(R.id.setup_note)
        primary = findViewById(R.id.setup_primary)
        secondary = findViewById(R.id.setup_secondary)
        tertiary = findViewById(R.id.setup_tertiary)
        share = findViewById(R.id.setup_share)
        share.setOnClickListener { shareLog() }
        // The installer is the runtime's. The launcher has loaded it, but
        // not when the system made this screen again by itself.
        if (!NativeBridge.isLoaded()) NativeBridge.launcher(this)
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putBoolean(STATE_DLC_ANSWERED, dlcAnswered)
    }

    override fun onResume() {
        super.onResume()
        // A build the system ended, or one that waited for the player's
        // answer about the toolchain package, carries on.
        GameBuildService.resumeIfInterrupted(this)
        search()
        handler.post(tick)
    }

    override fun onPause() {
        handler.removeCallbacks(tick)
        super.onPause()
    }

    // ---- step 1: the disc image ----------------------------------------------------

    /**
     * Looks for this game's disc images where players keep them, off the
     * main thread: a player whose image is in Download is then offered it
     * by name and picks nothing. It finds none without the permission to
     * list the storage, and the picker needs none.
     */
    private fun search() {
        val step = Setup.step(this)
        if (searching || (step != Setup.Step.DISC && step != Setup.Step.SECOND_DISC)) return
        searching = true
        thread(name = "disc-search", isDaemon = true) {
            val all = DiscImage.search(this)
            handler.post {
                searching = false
                // For the step the player is at now, since the first disc may have
                // been taken meanwhile: the second is Advanced Warfare's install disc.
                found = (if (Setup.step(this) == Setup.Step.SECOND_DISC) all.installs else all.games).firstOrNull()
            }
        }
    }

    private fun pickFile() {
        document.launch(
            Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*")
                // Advanced Warfare's two discs can be chosen in one go.
                .putExtra(Intent.EXTRA_ALLOW_MULTIPLE, Title.isAw),
        )
    }

    /** Sets the game up from what the player chose: disc images, or the folder they are in. */
    private fun take(choices: List<String>) {
        checking = true
        discProblem = null
        show()
        thread(name = "setup-disc") {
            val failed = choices.firstNotNullOfOrNull { takeChoice(it) }
            handler.post {
                checking = false
                discProblem = failed
                // What was found was for the disc just taken; the next is looked for.
                found = null
                search()
                show()
            }
        }
    }

    /** One thing the player chose; null, or the sentence that says why it was not taken. */
    private fun takeChoice(choice: String): String? {
        if (DiscImage.isDocument(choice) || !File(choice).isDirectory) return takeImage(choice)
        // A folder: the one the images are in, which is what a player who
        // keeps them in a folder of their own means by it.
        val inside = DiscImage.inFolder(this, File(choice))
        val images = inside.games.take(1) + inside.installs.take(1)
        if (images.isEmpty()) return getString(R.string.setup_disc_none_in_folder, Setup.gameName(this))
        return images.firstNotNullOfOrNull { takeImage(it.path) }
    }

    /**
     * Checks that [image] is a disc of this game that this app supports and
     * sets the game up to play from it where it lies; null, or the sentence
     * that says what is wrong with it.
     */
    private fun takeImage(image: String): String? {
        val game = Setup.gameName(this)
        val (kind, title) = DiscImage.inspect(this, image) ?: return getString(
            // A document that is no file on this device (a cloud entry) cannot be read at positions.
            if (DiscImage.isDocument(image) && readable(image)) R.string.setup_disc_not_on_device else R.string.setup_disc_not_image,
            game,
        )
        when (kind) {
            DiscImage.Kind.OTHER -> return when {
                // This game's disc without one of its executables: an image
                // somebody cut the multiplayer out of.
                title == Title.titleId -> getString(R.string.setup_disc_incomplete, game)
                else -> Setup.gameName(this, title)?.let { getString(R.string.setup_disc_other_game, it, game) }
                    ?: getString(R.string.setup_disc_unknown_game, game)
            }
            // The executables are kept for the build, each checked to be
            // one this app has the tables for: another is another region's
            // or a later pressing's.
            DiscImage.Kind.GAME ->
                if (GameBuild.offered(this) && !GameBuild.takeFromImage(this, image)) return getString(R.string.setup_disc_version, game)
            DiscImage.Kind.INSTALL -> Unit
        }
        val source = DiscImage.source(this, image) ?: return getString(R.string.setup_disc_not_on_device, game)
        val target = Setup.gameFolder(this)
        target.mkdirs()
        val error = try {
            if (NativeBridge.isLoaded()) NativeBridge.nativeInstall(source, target.absolutePath, true)
            else NativeBridge.loadError() ?: getString(R.string.unknown_reason)
        } catch (failure: Throwable) {
            failure.message ?: failure.toString()
        }
        // The game's own process opens the image for itself.
        DiscImage.close(source)
        if (error != null) return getString(R.string.setup_disc_failed, error)
        if (kind == DiscImage.Kind.INSTALL) {
            prefs.installDiscImage = image
        } else {
            // The image before the folder: the folder is what says the game is installed (Prefs.discImage).
            prefs.discImage = image
            prefs.chosenImage = image
            prefs.gameDirectory = target.absolutePath
        }
        return takeBeside(image, kind)
    }

    /**
     * Advanced Warfare's other disc usually lies beside the one chosen, and
     * is taken with it; a document has no folder to look in.
     */
    private fun takeBeside(image: String, kind: DiscImage.Kind): String? {
        if (!Title.isAw || DiscImage.isDocument(image)) return null
        val beside = File(image).parentFile?.let { DiscImage.inFolder(this, it) } ?: return null
        val other = when {
            kind == DiscImage.Kind.GAME && Title.installDiscMissing(prefs.gameDirectory, prefs.installDiscImage) -> beside.installs
            kind == DiscImage.Kind.INSTALL && prefs.gameDirectory.isEmpty() -> beside.games
            else -> emptyList()
        }.firstOrNull() ?: return null
        return takeImage(other.path)
    }

    /** Whether the document [image] can be opened at all, whatever kind of file it is. */
    private fun readable(image: String): Boolean = try {
        contentResolver.openFileDescriptor(Uri.parse(image), "r")?.use { true } ?: false
    } catch (_: Throwable) {
        false
    }

    // ---- step 2: the build ---------------------------------------------------------

    private fun build() {
        val asks = Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        if (asks && !state.wanted) notifications.launch(Manifest.permission.POST_NOTIFICATIONS) else startBuild()
    }

    private fun startBuild() {
        GameBuildService.start(this, automatic = false)
        show()
    }

    /** The toolchain package this app builds with is installed: nothing is downloaded and Android asks nothing. */
    private fun haveTools(): Boolean = GameBuild.kit(this)?.let { kit -> Toolchain.find(this)?.let { kit.takes(it) } } == true

    private fun online(): Boolean =
        (getSystemService(CONNECTIVITY_SERVICE) as ConnectivityManager).activeNetwork != null

    /** Why the build stopped, in one sentence a player can act on; what the build itself said goes with the log. */
    private fun buildProblem(): String = getString(
        when {
            StatFs(filesDir.absolutePath).availableBytes < BUILD_BYTES -> R.string.setup_build_no_space
            !haveTools() && !online() -> R.string.setup_build_offline
            !haveTools() -> R.string.setup_build_tools_refused
            else -> R.string.setup_build_failed
        },
    )

    private fun shareLog() {
        val session = try {
            if (NativeBridge.isLoaded()) NativeBridge.nativeLogTail() else ""
        } catch (_: Throwable) {
            ""
        }
        val text = buildString {
            append("===== setup =====\n")
            append("step: ${Setup.step(this@SetupActivity).name}\n")
            discProblem?.let { append("disc: $it\n") }
            state.error.takeIf { it.isNotEmpty() }?.let { append("build: $it\n") }
            append(GameLibrary.describe(this@SetupActivity)).append("\n\n")
            append("===== game-build-report.txt =====\n").append(GameBuild.report(this@SetupActivity).readTextTail(8_000)).append("\n\n")
            append(LogSummary.whole(this@SetupActivity, session))
        }
        LogShare.share(this, text)
    }

    // ---- what the screen says ------------------------------------------------------

    /**
     * Sets a view's text when it is another than it shows. The screen is
     * said again twice a second while a build runs, and a text set again
     * unchanged is laid out again and read out again by a screen reader.
     */
    private fun put(view: TextView, text: CharSequence?) {
        if (view.text.toString() != text?.toString().orEmpty()) view.text = text
    }

    /** The buttons' labels and what they do; a null label hides the button. */
    private fun buttons(first: Pair<Int, () -> Unit>?, second: Pair<Int, () -> Unit>? = null, third: Pair<Int, () -> Unit>? = null) {
        for ((button, what) in listOf(primary to first, secondary to second, tertiary to third)) {
            button.visibility = if (what == null) View.GONE else View.VISIBLE
            if (what == null) continue
            put(button, getString(what.first))
            button.setOnClickListener { what.second() }
        }
    }

    /** The bar and the line under it, or neither; [percent] below zero is a bar that only moves. */
    private fun bar(line: String?, percent: Int = -1) {
        progress.visibility = if (line == null) View.GONE else View.VISIBLE
        progressText.visibility = progress.visibility
        if (line == null) return
        progress.isIndeterminate = percent < 0
        progress.progress = percent
        put(progressText, line)
    }

    private fun say(problemText: String?) {
        problem.visibility = if (problemText == null) View.GONE else View.VISIBLE
        put(problem, problemText)
        // Under Retry: what a bug report needs.
        share.visibility = problem.visibility
    }

    private fun show() {
        val step = Setup.step(this).let { if (it == Setup.Step.DLC && dlcAnswered) Setup.Step.DONE else it }
        val game = Setup.gameName(this)
        put(
            stepLabel,
            if (step == Setup.Step.DONE) getString(R.string.setup_step_done)
            else getString(R.string.setup_step, Setup.number(step), Setup.STEPS),
        )
        note.visibility = View.GONE
        when (step) {
            Setup.Step.DISC, Setup.Step.SECOND_DISC -> showDisc(step, game)
            Setup.Step.BUILD -> showBuild(game)
            Setup.Step.DLC -> {
                put(heading, getString(R.string.setup_dlc_heading))
                put(sentence, getString(R.string.setup_dlc_sentence, Dlc.consoleFolder))
                say(null)
                bar(null)
                buttons(
                    R.string.setup_dlc_skip to { dlcAnswered = true; show() },
                    R.string.setup_dlc_add to { dlc.launch(Intent(this, DlcActivity::class.java)) },
                )
            }
            Setup.Step.DONE -> {
                // A first setup is over: the map packs' step is not put again.
                if (!prefs.setupFinished) prefs.setupFinished = true
                put(heading, getString(R.string.setup_done_heading))
                put(sentence, getString(if (Title.isAw) R.string.setup_done_sentence_two else R.string.setup_done_sentence))
                say(null)
                bar(null)
                buttons(
                    R.string.setup_play to { setResult(RESULT_OK); finish() },
                    R.string.setup_to_main to { finish() },
                )
            }
        }
    }

    private fun showDisc(step: Setup.Step, game: String?) {
        val lost = Setup.lostDisc(this)
        put(
            heading,
            getString(
                when {
                    step == Setup.Step.SECOND_DISC -> R.string.setup_disc_second_heading
                    Title.isAw -> R.string.setup_disc_heading_two
                    else -> R.string.setup_disc_heading
                },
            ),
        )
        put(
            sentence,
            when {
                lost != null -> getString(R.string.setup_disc_lost, DiscImage.label(this, lost).substringAfterLast('/'))
                step == Setup.Step.SECOND_DISC -> getString(R.string.setup_disc_second_install)
                else -> getString(if (Title.isAw) R.string.setup_disc_sentence_two else R.string.setup_disc_sentence, game)
            },
        )
        if (checking) {
            say(null)
            bar(getString(R.string.setup_disc_checking))
            buttons(null)
            return
        }
        say(discProblem)
        val image = found
        if (image != null) {
            bar(null)
            progressText.visibility = View.VISIBLE
            put(progressText, getString(R.string.setup_disc_found, image.name))
            buttons(
                R.string.setup_disc_use_found to { take(listOf(image.path)) },
                R.string.setup_disc_pick_other to { pickFile() },
                R.string.setup_disc_browse to { browser.launch(Intent(this, FileBrowserActivity::class.java)) },
            )
        } else {
            bar(null)
            buttons(
                R.string.setup_disc_pick to { pickFile() },
                R.string.setup_disc_browse to { browser.launch(Intent(this, FileBrowserActivity::class.java)) },
            )
        }
    }

    private fun showBuild(game: String?) {
        val now = GameBuild.progress
        val updating = GameBuild.outdated(this)
        put(heading, getString(if (updating) R.string.setup_build_heading_update else R.string.setup_build_heading))
        val error = state.error
        when {
            // An app made without the build kit: the game is built on a PC.
            !GameBuild.offered(this) -> {
                put(sentence, getString(R.string.game_code_missing))
                say(null)
                bar(null)
                buttons(R.string.setup_guide to { startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(Setup.guide()))) })
            }
            now.running -> {
                put(sentence, getString(R.string.setup_build_running, game))
                say(null)
                bar(Setup.progressLine(this, now), Setup.percent(now))
                note.visibility = View.VISIBLE
                buttons(null)
            }
            ToolchainInstall.confirmation != null -> {
                put(sentence, getString(R.string.setup_build_android_asks))
                say(null)
                bar(getString(R.string.setup_build_waiting_answer))
                // Android's question closes when it sends the player to
                // Settings, and is not there on the way back.
                buttons(R.string.setup_build_ask_again to { ToolchainInstall.askAgain(this) })
            }
            !now.running && error.isNotEmpty() -> {
                put(sentence, getString(R.string.setup_build_running, game))
                say(buildProblem())
                bar(null)
                buttons(R.string.setup_retry to { build() })
            }
            // Paused on the build's own screen (Repair / rebuild).
            state.wanted && state.paused -> {
                put(sentence, getString(R.string.build_paused))
                say(null)
                bar(null)
                buttons(R.string.build_resume to { build() })
            }
            state.wanted -> {
                put(sentence, getString(R.string.setup_build_running, game))
                say(null)
                bar(getString(R.string.setup_build_starting))
                note.visibility = View.VISIBLE
                buttons(null)
            }
            else -> {
                put(
                    sentence,
                    getString(
                        when {
                            updating -> R.string.setup_build_sentence_update
                            haveTools() -> R.string.setup_build_sentence
                            else -> R.string.setup_build_sentence_tools
                        },
                        game,
                    ),
                )
                say(null)
                bar(null)
                buttons(R.string.setup_build_begin to { build() })
            }
        }
    }

    companion object {
        private const val STATE_DLC_ANSWERED = "dlcAnswered"

        /** What the build needs free while it works (docs/ondevice-build.md). */
        private const val BUILD_BYTES = 1L shl 30
    }
}
