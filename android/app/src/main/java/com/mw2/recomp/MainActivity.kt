package com.mw2.recomp

import android.app.DownloadManager
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.view.InputDevice
import android.widget.Button
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import kotlin.concurrent.thread
import org.json.JSONArray

/**
 * The launcher: is the game installed, which driver will be used, and the
 * button that starts it. It runs in its own process, separate from the game
 * (see the manifest), so that ending a run -- which means killing the
 * game's process -- leaves this one standing.
 *
 * It is written so that it cannot fail to appear. The native library is not
 * needed to draw this screen and is loaded in a guarded step afterwards: a
 * library that will not load leaves a launcher that says so, with the log
 * and the last crash still reachable, rather than an app that vanishes.
 */
class MainActivity : AppCompatActivity() {

    private lateinit var prefs: Prefs
    private lateinit var status: TextView
    private lateinit var footer: TextView
    private lateinit var play: Button
    private lateinit var groupPlay: android.view.ViewGroup
    private lateinit var groupExtras: android.view.ViewGroup
    private lateinit var secondButton: Button
    private lateinit var botsButton: Button
    private lateinit var zombiesButton: Button
    private lateinit var dlcButton: Button
    private lateinit var packLevelsButton: Button
    private lateinit var repairButton: Button
    private lateinit var modsButton: Button
    private lateinit var tu6Button: Button
    private lateinit var updatesButton: Button
    private lateinit var quitButton: Button
    private lateinit var detailTitle: TextView
    private lateinit var detailText: TextView
    private lateinit var versionTag: TextView
    private lateinit var directLaunchSwitch: androidx.appcompat.widget.SwitchCompat
    /** The disc image an app of one title would install from: a path, or the document the player chose for the build. */
    private var detectedImage: String? = null

    /**
     * This game's disc images on the device and its memory cards
     * (DiscImage.search), from the last search to end; a search is made
     * each time the launcher comes to the front with something to install.
     */
    private var searched: DiscImage.Found? = null
    private var searching = false

    /** When the artwork on screen was taken from the disc's package; 0 while it is the haze. */
    @Volatile
    private var artShown = 0L

    /** The game version this app plays, as the version line names it. */
    private var discVersion = ""

    /** Whether the detail card is the Play entry's, which changes when an install finishes. */
    private var detailIsPlay = false

    /** Whether the guided setup was opened by itself since this launcher started: it is not put in front of a player twice. */
    private var setupOpened = false

    /** The guided setup (SetupActivity): its last screen's Play comes back here, to the one way a run is started. */
    private val setup = registerForActivityResult(androidx.activity.result.contract.ActivityResultContracts.StartActivityForResult()) { result ->
        if (result.resultCode == RESULT_OK) launch(firstTitle)
    }

    /** What the large button starts: the campaign, where the app carries it. */
    private val firstTitle = Title.carried.first()

    /** The multiplayer, in the app that carries the whole game. */
    private val secondTitle = Title.carried.getOrNull(1)

    private var nativeReady = false
    private var hasAutoLaunched = false
    private lateinit var displayNotice: TextView
    private val handler = android.os.Handler(android.os.Looper.getMainLooper())

    // The system grants or refuses the mode asked for a moment after the
    // window has it, and the player mends the setting with the launcher
    // still up: the notice follows the display.
    private val displayListener = object : android.hardware.display.DisplayManager.DisplayListener {
        override fun onDisplayAdded(displayId: Int) = Unit
        override fun onDisplayRemoved(displayId: Int) = Unit
        override fun onDisplayChanged(displayId: Int) {
            if (displayId == shownDisplay()?.displayId) showDisplayHeld()
        }
    }
    private var pendingInstallId: Long = -1
    private var pendingUpdateUrl: String? = null

    private val downloadDone = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val id = intent.getLongExtra(DownloadManager.EXTRA_DOWNLOAD_ID, -1)
            if (id != pendingInstallId) return
            pendingInstallId = -1
            installDownloadedApk(id)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // The launcher's process is often ended under a run for its memory,
        // and the launcher the player quits to is then a new one made from
        // what was saved: it has done its direct launch, or been through an
        // install that started the game, and must not start it again.
        hasAutoLaunched = savedInstanceState?.getBoolean(STATE_AUTO_LAUNCHED) ?: false
        setupOpened = savedInstanceState?.getBoolean(STATE_SETUP_OPENED) ?: false
        setContentView(R.layout.activity_main)
        prefs = Prefs(this)
        // Before anything reads the driver choice (refresh() applies it, and
        // the :game process reads it when the run starts): on a Qualcomm
        // phone whose player has not picked a driver, the one this build
        // carries becomes the default.
        DriverStore(this).installBundledDefault(prefs)

        status = findViewById(R.id.status)
        footer = findViewById(R.id.footer)
        play = findViewById(R.id.play)
        groupPlay = findViewById(R.id.group_play)
        groupExtras = findViewById(R.id.group_extras)
        secondButton = findViewById(R.id.other_app)
        botsButton = findViewById(R.id.entry_bots)
        zombiesButton = findViewById(R.id.entry_zombies)
        dlcButton = findViewById(R.id.entry_dlc)
        packLevelsButton = findViewById(R.id.entry_pack_levels)
        repairButton = findViewById(R.id.entry_repair)
        modsButton = findViewById(R.id.entry_mods)
        tu6Button = findViewById(R.id.entry_tu6)
        updatesButton = findViewById(R.id.entry_updates)
        quitButton = findViewById(R.id.entry_quit)
        detailTitle = findViewById(R.id.detail_title)
        detailText = findViewById(R.id.detail_text)
        versionTag = findViewById(R.id.version_tag)
        directLaunchSwitch = findViewById(R.id.direct_launch_switch)
        displayNotice = findViewById(R.id.display_notice)
        getSystemService(android.hardware.display.DisplayManager::class.java)
            .registerDisplayListener(displayListener, handler)
        try {
            val info = packageManager.getPackageInfo(packageName, 0)
            val name = info.versionName ?: ""
            val code = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) info.longVersionCode.toInt()
            else info.versionCode
            val tu = BuildConfig.MW2_TU.uppercase()
            discVersion = when {
                Title.isMw3 || Title.isCod4 || Title.isAw -> "DISC VERSION"
                isTu6() -> "TITLE UPDATE 6"
                else -> "DISC VERSION 1.0.557"
            }
            versionTag.text = "$name · vcode $code · $tu · $discVersion"
        } catch (_: Throwable) {
        }
        // TU6 apps offer the TU0 archives instead (PC launcher parity).
        if (isTu6()) tu6Button.text = getString(R.string.entry_tu0)
        // MW3 has one game version, and Modern Warfare 2's one app is the
        // disc's version alone: no other to point at.
        if (Title.isMw3 || Title.both) tu6Button.visibility = android.view.View.GONE
        if (BuildConfig.UPDATE_REPO.isEmpty()) updatesButton.visibility = android.view.View.GONE

        directLaunchSwitch.isChecked = prefs.autoLaunch
        directLaunchSwitch.setOnCheckedChangeListener { _, isChecked ->
            prefs.autoLaunch = isChecked
        }

        play.setOnClickListener {
            select(play)
            // Until the game is set up the large button is the guided setup.
            if (Setup.guided && !Setup.complete(this)) {
                openSetup()
                return@setOnClickListener
            }
            showDetail(getString(R.string.play), playDetail())
            detailIsPlay = true
            launch(firstTitle)
        }
        // The app with the whole game: its multiplayer. An app of one title:
        // the other half of the game, when that app is installed, so the
        // campaign jumps straight into multiplayer and back, no home screen
        // in between.
        secondButton.setOnClickListener {
            select(secondButton)
            val second = secondTitle
            if (second != null) {
                showDetail(
                    getString(R.string.title_multiplayer),
                    getString(
                        when {
                            !Title.hasBots -> R.string.multiplayer_detail
                            Title.isMw3 -> R.string.multiplayer_detail_bots
                            else -> R.string.multiplayer_detail_bots_only
                        },
                    ),
                )
                launch(second)
            } else {
                launchOtherApp()
            }
        }
        zombiesButton.setOnClickListener {
            select(zombiesButton)
            showDetail(getString(R.string.entry_zombies), getString(R.string.zombies_detail))
            if (Title.MULTIPLAYER.installed(prefs.gameDirectory)) startGame(Title.MULTIPLAYER, TitleUpdate.zombiesLaunchData)
        }
        dlcButton.setOnClickListener {
            select(dlcButton)
            showDetail(getString(R.string.dlc_title), getString(if (Dlc.takesUpdate) R.string.dlc_detail_update else R.string.dlc_detail))
            startActivity(Intent(this, DlcActivity::class.java))
        }
        packLevelsButton.setOnClickListener {
            select(packLevelsButton)
            showDetail(getString(R.string.entry_pack_levels), getString(R.string.pack_levels_detail))
            val levels = Dlc.levels(this)
            AlertDialog.Builder(this)
                .setTitle(R.string.entry_pack_levels)
                .setItems(levels.map { it.name }.toTypedArray()) { _, which -> startGame(Title.CAMPAIGN, level = levels[which].level) }
                .setNegativeButton(R.string.cancel, null)
                .show()
        }
        botsButton.setOnClickListener {
            select(botsButton)
            showDetail(getString(R.string.bots_title), getString(R.string.bots_detail))
            showBots()
        }
        // What the guided setup does for the player, for one who wants it
        // done another way: another disc image or the files copied in
        // (InstallActivity), the game code built again or paused
        // (BuildActivity).
        repairButton.setOnClickListener {
            select(repairButton)
            showDetail(getString(R.string.entry_repair), getString(R.string.repair_detail))
            val screens = buildList {
                add(R.string.repair_files to InstallActivity::class.java)
                if (GameBuild.offered(this@MainActivity)) add(R.string.repair_build to BuildActivity::class.java)
            }
            AlertDialog.Builder(this)
                .setTitle(R.string.entry_repair)
                .setItems(screens.map { getString(it.first) }.toTypedArray()) { _, which ->
                    startActivity(Intent(this, screens[which].second))
                }
                .setNegativeButton(R.string.cancel, null)
                .show()
        }
        findViewById<Button>(R.id.driver).setOnClickListener {
            select(it as Button)
            showDetail(
                getString(R.string.graphics_driver),
                getString(R.string.driver_hint),
            )
            startActivity(Intent(this, DriverActivity::class.java))
        }
        findViewById<Button>(R.id.settings).setOnClickListener {
            select(it as Button)
            showDetail(getString(R.string.entry_settings), getString(R.string.settings_detail))
            startActivity(Intent(this, SettingsActivity::class.java))
        }
        findViewById<Button>(R.id.log).setOnClickListener {
            select(it as Button)
            showDetail(getString(R.string.view_log), "")
            showLog()
        }
        // The bug report's form, with what it asks of the app filled in.
        findViewById<Button>(R.id.entry_report).setOnClickListener {
            select(it as Button)
            showDetail(getString(R.string.report_problem_title), getString(R.string.report_problem_detail))
            LogShare.report(this) { wholeLog() }
        }
        // Help, news and requests on the project's Discord; bugs go through the form above.
        findViewById<Button>(R.id.entry_discord).setOnClickListener {
            select(it as Button)
            showDetail(getString(R.string.entry_discord), getString(R.string.discord_detail))
            Discord.open(this)
        }
        modsButton.setOnClickListener {
            select(modsButton)
            showDetail(getString(R.string.entry_mods), getString(R.string.mods_detail_phone))
            startActivity(
                Intent(this, SettingsActivity::class.java).apply {
                    putExtra(SettingsActivity.EXTRA_TAB, SettingsActivity.TAB_FILES)
                },
            )
        }
        tu6Button.setOnClickListener {
            select(tu6Button)
            // Mirror of the PC launcher's GET TU6/GET TU0 entry: each build
            // points at the other game version's archives. Same-version rule:
            // TU0 plays only with TU0, TU6 only with TU6.
            if (isTu6()) {
                tu6Button.text = getString(R.string.entry_tu0)
                showDetail(getString(R.string.entry_tu0), getString(R.string.tu0_detail_phone))
            } else {
                showDetail(getString(R.string.entry_tu6), getString(R.string.tu6_detail_phone))
            }
            openReleasesPage()
        }
        updatesButton.setOnClickListener {
            select(updatesButton)
            showDetail(getString(R.string.check_updates), "")
            checkForUpdates(manual = true)
        }
        quitButton.setOnClickListener {
            select(quitButton)
            finishAndRemoveTask()
        }

        // Whatever killed the app last time, shown before anything else is
        // attempted -- it may well be about to happen again.
        val crash = Trail.lastCrash(this)
        if (crash.isNotEmpty()) showLastCrash(crash)

        // The library is loaded after the screen exists, so a failure here
        // is reported rather than fatal.
        nativeReady = NativeBridge.launcher(this)
        Trail.clear(this)
        // Exported: the broadcast comes from the system's download provider,
        // which is another app, and a receiver that is not exported never
        // hears it (the update then sat downloaded and uninstalled). What it
        // acts on is only this activity's own download, by its id.
        ContextCompat.registerReceiver(
            this, downloadDone,
            android.content.IntentFilter(DownloadManager.ACTION_DOWNLOAD_COMPLETE),
            ContextCompat.RECEIVER_EXPORTED,
        )
        // What the multiplayer kept when it was an app of its own, where the
        // system lets this app read it. Off the main thread: it is files.
        val gameDirectory = File(
            prefs.gameDirectory.ifEmpty { File(getExternalFilesDir(null) ?: filesDir, "game").absolutePath },
        )
        thread(name = "old-saves", isDaemon = true) { OldMultiplayerApp.adoptSaves(gameDirectory) }
        checkForUpdates()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putBoolean(STATE_AUTO_LAUNCHED, hasAutoLaunched)
        outState.putBoolean(STATE_SETUP_OPENED, setupOpened)
    }

    override fun onDestroy() {
        handler.removeCallbacksAndMessages(null)
        getSystemService(android.hardware.display.DisplayManager::class.java)
            .unregisterDisplayListener(displayListener)
        try {
            unregisterReceiver(downloadDone)
        } catch (_: Throwable) {
        }
        super.onDestroy()
    }

    private fun shownDisplay(): android.view.Display? =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) display
        else {
            @Suppress("DEPRECATION")
            windowManager.defaultDisplay
        }

    /**
     * The notice that the system holds the screen under the FPS limit, with
     * where to mend it, or none. In the game it is a toast that is gone in a
     * few seconds (GameActivity.sayDisplayHeld), and a player who missed it
     * had 60 frames a second on a 120 Hz screen with nothing to say why.
     */
    private fun showDisplayHeld() {
        val limit = prefs.fpsLimit
        val now = DisplayRate.heldAt(shownDisplay(), limit)
        if (now == null) {
            displayNotice.visibility = android.view.View.GONE
            return
        }
        var text = getString(R.string.display_held_launcher, Math.round(now), limit)
        if (Build.MANUFACTURER.equals("AYN", ignoreCase = true)) {
            text += " " + getString(R.string.display_held_ayn)
        }
        displayNotice.text = text
        displayNotice.visibility = android.view.View.VISIBLE
    }

    override fun onResume() {
        super.onResume()
        // The game code this version of the app expects, built by itself when
        // the code on the device is not it; and a build the system ended
        // carries on (GameUpdate.kt). What it does is shown as it goes.
        GameUpdate.consider(this)
        handler.removeCallbacks(buildTick)
        handler.postDelayed(buildTick, BUILD_TICK_MS)
        showArt()
        searchDiscs()
        refresh()
        // The launcher asks for the mode the game will ask for (the limit may
        // have been changed in Settings since), and says so when the system
        // has not given it after the moment a mode change takes.
        DisplayRate.request(window, shownDisplay(), prefs.fpsLimit)
        handler.postDelayed({ showDisplayHeld() }, DISPLAY_SETTLE_MS)
        // Back from an install the Play card started: it said a disc was
        // found, and the game is there now.
        if (detailIsPlay) detailText.text = playDetail()
        // Back from the install-permission screen with a download waiting.
        pendingUpdateUrl?.let { url ->
            pendingUpdateUrl = null
            if (packageManager.canRequestPackageInstalls()) startUpdateDownload(url)
        }
    }

    override fun onPause() {
        handler.removeCallbacks(buildTick)
        super.onPause()
    }

    /** A build of the game code was under way when the launcher last looked. */
    private var sawBuilding = false

    /**
     * While the launcher is in front: the build's progress on the large
     * button and in the status, the launcher as it is with the new code
     * once the build has ended, and Android's question about the toolchain
     * package, which a build in the background could not put
     * (ToolchainInstall).
     */
    private val buildTick = object : Runnable {
        override fun run() {
            val building = GameBuildService.State(this@MainActivity).wanted
            if (building) {
                ToolchainInstall.ask(this@MainActivity)
                if (Setup.guided) showSetup(Setup.step(this@MainActivity))
            } else if (sawBuilding) {
                refresh()
            }
            sawBuilding = building
            handler.postDelayed(this, BUILD_TICK_MS)
        }
    }

    private fun openSetup() = setup.launch(Intent(this, SetupActivity::class.java))

    /** The large button's two lines: what it does, and under it, smaller, what that is. */
    private fun largeButton(first: String, second: String) {
        play.text = android.text.SpannableString("$first\n$second").apply {
            setSpan(android.text.style.RelativeSizeSpan(0.6f), first.length + 1, length, 0)
        }
    }

    /**
     * The game is not set up, or an update of the app needs it built again:
     * the large button is the guided setup, and says how far it is; the
     * status, in a line, what is in the way.
     */
    private fun showSetup(step: Setup.Step) {
        val state = GameBuildService.State(this)
        val now = GameBuild.progress
        val updating = GameBuild.outdated(this)
        val lost = Setup.lostDisc(this)
        val stepLine = getString(R.string.setup_step, Setup.number(step), Setup.STEPS)
        play.isEnabled = true
        when {
            state.wanted && now.running ->
                largeButton(getString(if (updating) R.string.setup_button_updating else R.string.setup_button_building, Setup.percent(now)), getString(R.string.setup_button_watch))
            updating -> largeButton(getString(R.string.setup_button_update), getString(R.string.setup_button_update_hint))
            // Nothing done yet: a first start.
            step == Setup.Step.DISC && lost == null -> largeButton(getString(R.string.setup_button_start), getString(R.string.setup_button_start_hint))
            else -> largeButton(getString(R.string.setup_button_continue), stepLine)
        }
        status.text = when {
            ToolchainInstall.confirmation != null -> getString(R.string.update_code_confirm)
            state.wanted && now.running -> Setup.progressLine(this, now)
            lost != null -> getString(R.string.status_disc_lost, DiscImage.label(this, lost).substringAfterLast('/'))
            updating && GameUpdate.enabled(this) && !GameUpdate.batteryAllows(this) ->
                getString(R.string.status_update_charger, GameUpdate.batteryLevel(this))
            updating -> getString(R.string.status_update)
            step == Setup.Step.DISC -> getString(R.string.status_setup_disc)
            step == Setup.Step.SECOND_DISC -> getString(R.string.status_setup_second)
            else -> getString(R.string.status_setup_build)
        }
    }

    /**
     * The game's own artwork behind the menu once the game is installed
     * (DiscArt.kt), and the haze again when it is removed. On each return to
     * the launcher, because an install is what brings the picture; decoded
     * only when it is another picture than the one showing.
     */
    private fun showArt() {
        val gameDirectory = File(
            prefs.gameDirectory.ifEmpty { File(getExternalFilesDir(null) ?: filesDir, "game").absolutePath },
        )
        thread(name = "disc-art", isDaemon = true) {
            val file = DiscArt.backdrop(this, gameDirectory)
            val taken = file?.lastModified() ?: 0L
            if (taken == artShown) return@thread
            val picture = file?.let { android.graphics.BitmapFactory.decodeFile(it.absolutePath) }
            runOnUiThread {
                if (isDestroyed) return@runOnUiThread
                artShown = if (picture != null) taken else 0L
                findViewById<BackdropView>(R.id.backdrop).art = picture
                // The card lets the picture through; alone it would hide
                // the half of it the menu does not.
                findViewById<android.view.View>(R.id.detail_card).background.mutate().alpha = if (picture != null) 160 else 255
            }
        }
    }

    private fun refresh() {
        if (!nativeReady) {
            status.text = getString(
                R.string.status_library_missing,
                NativeBridge.loadError() ?: getString(R.string.unknown_reason),
            )
            play.isEnabled = false
            footer.text = getString(
                R.string.footer, device(), getString(R.string.driver_system),
                BuildConfig.BUILD_SHA, BuildConfig.BUILD_STAMP,
            )
            return
        }
        var gameDirectory = prefs.gameDirectory
        if (gameDirectory.isEmpty()) {
            val defaultGameDir = File(getExternalFilesDir(null) ?: filesDir, "game")
            if (firstTitle.installed(defaultGameDir.absolutePath)) {
                gameDirectory = defaultGameDir.absolutePath
                prefs.gameDirectory = gameDirectory
            }
        }
        val installed = firstTitle.installed(gameDirectory)
        refreshSecondEntry()
        refreshBotsEntry()
        // The player's own map packs, and Exo Zombies while the update they
        // need is in use. Compatibility Packs still to add are counted on
        // the entry itself: the multiplayer is the disc's until they are
        // there, and the title would not say why.
        TitleUpdate.refresh(this)
        dlcButton.visibility = if (installed) android.view.View.VISIBLE else android.view.View.GONE
        dlcButton.text = TitleUpdate.missingPacks.size.let { missing ->
            if (missing == 0) getString(R.string.entry_dlc) else getString(R.string.entry_dlc_missing, missing)
        }
        zombiesButton.visibility = if (TitleUpdate.inUse && installed) android.view.View.VISIBLE else android.view.View.GONE
        // Modern Warfare 3's Special Ops levels of the packs, which the
        // disc's menus have no entries for (docs/dlc.md).
        val packLevels = installed && Title.CAMPAIGN.carried && Title.CAMPAIGN.installed(gameDirectory) && Dlc.levels(this).isNotEmpty()
        packLevelsButton.visibility = if (packLevels) android.view.View.VISIBLE else android.view.View.GONE
        sizeLeftColumn()

        val ready = if (Setup.guided) refreshGuided() else refreshOneTitle(installed)
        directLaunchSwitch.visibility = if (ready) android.view.View.VISIBLE else android.view.View.GONE
        if (ready && prefs.autoLaunch && !hasAutoLaunched) {
            hasAutoLaunched = true
            // The title played last, while its files are still there.
            val last = RunRequest.read(this).title
            startGame(if (last.installed(gameDirectory)) last else firstTitle)
            return
        }
        // Direct launch is for the app's start. A launcher that started
        // without the game sends the player through the setup, whose last
        // screen starts the game: coming back here from that run is a Quit,
        // and must not start the game a second time.
        if (!ready) hasAutoLaunched = true

        applyDriverChoice()
        footer.text = getString(
            R.string.footer, device(), currentDriverName(),
            BuildConfig.BUILD_SHA, BuildConfig.BUILD_STAMP,
        )
    }

    /**
     * The launcher of an app with the whole game: Play, a line that says it
     * is ready, and what there is to play and to add; or, until the game is
     * set up, the guided setup on the large button and nothing a game is
     * needed for. A new player is taken into the setup once for each start
     * of the launcher, with nothing to find first. True when the game can
     * be played.
     */
    private fun refreshGuided(): Boolean {
        val step = Setup.step(this)
        val ready = step == Setup.Step.DLC || step == Setup.Step.DONE
        // Hidden, not gone: the large button keeps its size.
        val shown = if (ready) android.view.View.VISIBLE else android.view.View.INVISIBLE
        groupPlay.visibility = shown
        groupExtras.visibility = shown
        if (ready) {
            // Set up before this version, or by the setup just left: its
            // step for the map packs is a first setup's (Prefs.setupFinished).
            if (!prefs.setupFinished) prefs.setupFinished = true
            play.isEnabled = true
            largeButton("▶  " + getString(R.string.play_large), getString(R.string.title_campaign))
            status.text = getString(R.string.status_ready)
            return true
        }
        showSetup(step)
        if (!prefs.setupFinished && !setupOpened) {
            setupOpened = true
            openSetup()
        }
        return false
    }

    /**
     * The left column's heights. Its groups show two to five entries by the
     * game and what the player has added; shared out by fixed weights, two
     * entries stood as far apart as four. Each entry gets one row, a row
     * being about the height of an entry of the right column; of the rows
     * left over the large button takes the first and the rest stand as
     * space under each group.
     */
    private fun sizeLeftColumn() {
        fun rows(group: android.view.ViewGroup) = (0 until group.childCount).count {
            val child = group.getChildAt(it)
            child is Button && child.visibility == android.view.View.VISIBLE
        }
        fun weigh(view: android.view.View, rows: Float) {
            val params = view.layoutParams as android.widget.LinearLayout.LayoutParams
            if (params.weight == rows) return
            params.weight = rows
            view.layoutParams = params
        }
        // A group's heading is in its height: under half a row, on both phones.
        fun group(rows: Int): Float = if (rows == 0) 0f else rows + HEADING_ROWS
        val playRows = rows(groupPlay)
        val extraRows = rows(groupExtras)
        val spare = (ENTRY_ROWS - playRows - extraRows).coerceAtLeast(0).toFloat()
        val gap = (spare - 1f).coerceAtLeast(0f) / 2f
        weigh(play, PLAY_ROWS + spare.coerceAtMost(1f))
        weigh(groupPlay, group(playRows))
        weigh(findViewById(R.id.gap_groups), gap)
        weigh(groupExtras, group(extraRows))
        weigh(findViewById(R.id.gap_switch), gap)
    }

    /**
     * The launcher of an app of one title (the older apps, and title update
     * 6's) and of Black Ops III: Play, or the install screen until the
     * game's files are there. True when the game can be played.
     */
    private fun refreshOneTitle(installed: Boolean): Boolean {
        // A disc to install from is looked for only while the game is not
        // installed. The one the build screen took the executables from
        // comes first: it is known to be this game's.
        detectedImage = if (installed) null else chosenImage() ?: findDisc()?.absolutePath
        val lost = Setup.lostDisc(this)
        // Black Ops III is no game until its title update and Compatibility
        // Packs are in the dlc folder, and its game code is built from that
        // update: the status says which is still to come, the update first,
        // and the large button is Add DLC, which takes the one and builds
        // the other (launch).
        val addDlc = installed && lost == null && dlcComesFirst()
        play.isEnabled = lost == null
        play.text = getString(
            when {
                !installed -> R.string.install_game
                addDlc -> R.string.dlc_title
                else -> R.string.play_large
            },
        )
        status.text = when {
            !installed -> getString(R.string.status_setup_disc)
            lost != null -> getString(R.string.status_disc_lost, DiscImage.label(this, lost).substringAfterLast('/'))
            addDlc && updateNeeded() -> getString(R.string.status_update_needed)
            addDlc -> getString(if (GameBuild.offered(this)) R.string.build_code_missing else R.string.game_code_missing)
            else -> getString(R.string.status_ready_one)
        }
        return installed && lost == null && !addDlc
    }

    /** Its title update or a Compatibility Pack is not in the dlc folder, for a game that is none without them (TitleUpdate.required). */
    private fun updateNeeded(): Boolean = TitleUpdate.required && (TitleUpdate.updateMissing || TitleUpdate.missingPacks.isNotEmpty())

    /** Add DLC is what such a game needs next: the update and the packs, or the game code built from them. */
    private fun dlcComesFirst(): Boolean = TitleUpdate.required && (updateNeeded() || !Setup.codeReady(this))

    /** The install screen of an app of one title, from the disc image found on the device when there is one. */
    private fun install(title: Title) {
        startActivity(
            Intent(this, InstallActivity::class.java).apply {
                putExtra(InstallActivity.EXTRA_TITLE, title.name)
                detectedImage?.let { putExtra(InstallActivity.EXTRA_INITIAL_PATH, it) }
                putExtra(InstallActivity.EXTRA_AUTO_START, detectedImage != null)
            },
        )
    }

    /**
     * Looks for the disc images an app of one title could install from,
     * while its game is not installed, off the main thread: it lists
     * folders down the storage. What it finds is shown when it ends. The
     * apps with the whole game look for theirs in the guided setup
     * (SetupActivity).
     */
    private fun searchDiscs() {
        if (Setup.guided || firstTitle.installed(prefs.gameDirectory) || searching) return
        searching = true
        thread(name = "disc-search", isDaemon = true) {
            val found = DiscImage.search(this)
            runOnUiThread {
                searching = false
                searched = found
                if (!isFinishing && !isDestroyed) refresh()
            }
        }
    }

    /** The image chosen on the build screen, while it can still be read. */
    private fun chosenImage(): String? {
        val image = prefs.chosenImage
        if (image.isEmpty()) return null
        val source = DiscImage.source(this, image) ?: return null
        DiscImage.close(source)
        return image
    }

    /** A disc image where players usually leave one, or null: for an app of one title (the others have the guided setup). */
    private fun findDisc(): File? {
        // MW2's multiplayer needs the FULL disc: mw2_campaign_lean.iso pruned
        // the multiplayer fastfiles to save 2.7 GB, so it can never satisfy
        // a multiplayer install. SP accepts either, preferring lean.
        // (TU6 installs from the same disc; the update applies after.)
        val isMp = Title.MULTIPLAYER.carried
        val candidateIsos = if (Title.isBo3) {
            listOf(
                File("/sdcard/Download/bo3.iso"),
                File("/sdcard/Download/Call of Duty - Black Ops III (USA, Europe) (En,Fr,De,Es,It,Pt,Pl,Ru).iso"),
                File(getExternalFilesDir(null) ?: filesDir, "bo3.iso"),
            )
        } else if (Title.isMw3) {
            listOf(
                File("/sdcard/Download/mw3.iso"),
                File("/sdcard/Download/Call of Duty - Modern Warfare 3.iso"),
                File("/sdcard/Download/Call of Duty - Modern Warfare 3 (USA, Europe).iso"),
                File(getExternalFilesDir(null) ?: filesDir, "mw3.iso"),
            )
        } else if (isMp) {
            listOf(
                File("/sdcard/Download/mw2.iso"),
                File("/sdcard/Download/Call of Duty - Modern Warfare 2 (USA, Europe).iso"),
                File(getExternalFilesDir(null) ?: filesDir, "mw2.iso"),
                // Lean last: detected but installer will report missing MP zones.
                File("/sdcard/Download/mw2_campaign_lean.iso"),
            )
        } else {
            listOf(
                File("/sdcard/Download/mw2_campaign_lean.iso"),
                File("/sdcard/Download/mw2.iso"),
                File("/sdcard/Download/Call of Duty - Modern Warfare 2 (USA, Europe).iso"),
                File(getExternalFilesDir(null) ?: filesDir, "mw2_campaign_lean.iso"),
                File(getExternalFilesDir(null) ?: filesDir, "mw2.iso")
            )
        }
        // Any other image of this game on the device or a memory card,
        // under whatever name and in whatever folder (DiscImage.search).
        val anyImage = searched?.games?.firstOrNull {
            // Lean ISO has no MP zones; don't offer it as fallback for MP.
            !isMp || !it.name.contains("lean", ignoreCase = true)
        }

        return candidateIsos.firstOrNull { it.exists() && it.length() > DiscImage.SMALLEST } ?: anyImage
    }

    /**
     * Starts a title, or what has to come first: the guided setup, or in an
     * app of one title the install, from the disc image found on the phone
     * when there is one.
     */
    private fun launch(title: Title) {
        val installed = title.installed(prefs.gameDirectory) && !Title.installDiscMissing(prefs.gameDirectory, prefs.installDiscImage)
        when {
            installed && dlcComesFirst() -> startActivity(Intent(this, DlcActivity::class.java))
            installed -> startGame(title)
            Setup.guided -> openSetup()
            else -> install(title)
        }
    }

    private fun device(): String {
        val vkVersion = packageManager.systemAvailableFeatures
            .firstOrNull { it.name == "android.hardware.vulkan.version" }?.version ?: 0
        val vkMajor = vkVersion shr 22
        val vkMinor = (vkVersion shr 12) and 0x3ff
        val vkStr = if (vkVersion > 0) "Vulkan $vkMajor.$vkMinor" else "No Vulkan"
        return "${android.os.Build.MANUFACTURER} ${android.os.Build.MODEL} ($vkStr), " +
            "Android ${android.os.Build.VERSION.RELEASE}"
    }

    /** A native call that must not take the launcher with it if it fails. */
    private fun <T> safely(fallback: T, call: () -> T): T = try {
        call()
    } catch (_: Throwable) {
        fallback
    }

    private fun applyDriverChoice() {
        val directory = prefs.driverDirectory
        val library = prefs.driverLibrary
        if (directory.isNotEmpty() && library.isNotEmpty() && File(directory, library).exists()) {
            safely(Unit) { NativeBridge.nativeSelectDriver(directory, library) }
        } else {
            // A driver that was deleted from underneath the setting.
            if (directory.isNotEmpty()) {
                prefs.driverDirectory = ""
                prefs.driverLibrary = ""
            }
            safely(Unit) { NativeBridge.nativeSelectDriver("", "") }
        }
    }

    private fun currentDriverName(): String {
        val directory = prefs.driverDirectory
        if (directory.isEmpty()) return getString(R.string.driver_system)
        return File(directory).name
    }

    private fun startGame(title: Title, launchData: String = "", level: String = "") {
        if (!nativeReady) return
        // Every way into a run comes through here: without the title's game
        // code, or with code this version of the app does not take
        // (GameLibrary.kt), one only ends in "The game could not start",
        // and the entries under Play are not replaced as Play is.
        if (!GameLibrary.ready(this, title) || !GameLibrary.current(this, title)) {
            showDetail(getString(R.string.cannot_start), getString(R.string.cannot_start_build))
            return
        }
        // A second controller means a second player can join (split-screen
        // Spec Ops / multiplayer); the touch controls and the first controller
        // are both player one. Warn once: two viewports cost frames and heat.
        // A level of a map pack is started for one player (Dlc.levels).
        if (level.isEmpty() && !prefs.seenPadWarning && physicalPadCount() >= 2) {
            showSplitscreenWarning(title, launchData)
            return
        }
        GameActivity.start(this, title, launchData, level)
    }

    private fun showDetail(title: String, text: String) {
        detailTitle.text = title
        detailText.text = text
        detailIsPlay = false
    }

    /** What the Play card says: the game that is installed, or what the status says is in the way. */
    private fun playDetail(): String {
        val gameDirectory = prefs.gameDirectory
        return if (firstTitle.installed(gameDirectory)) {
            getString(R.string.play_installed, discVersion.lowercase(), gameDirectory)
        } else {
            status.text.toString()
        }
    }

    /** An app of the multiplayer alone (cod.modern.warfare2.mp, .mp.tu6, com.mw3.recomp.mp). */
    private fun isMp(): Boolean = firstTitle == Title.MULTIPLAYER

    /** cod.modern.warfare2.sp.tu6 / .mp.tu6 run title update 6. */
    private fun isTu6(): Boolean = packageName.endsWith(".tu6")

    /** The other flavor's package, or null when it is not installed. */
    private fun otherApp(): Pair<String, String>? {
        // Same game and version on both ends: TU6 cross-launches the TU6
        // sibling, and MW3's multiplayer test copy the game itself.
        val pkg = if (Title.isMw3) {
            "com.mw3.recomp"
        } else {
            // MW2's own package with the other title's suffix
            // (build.gradle.kts: cod.modern.warfare2.sp / .mp, then .tu6).
            packageName.removeSuffix(".tu6").substringBeforeLast('.') +
                (if (isMp()) ".sp" else ".mp") + if (isTu6()) ".tu6" else ""
        }
        val label = getString(
            R.string.launch_other,
            getString(if (isMp()) R.string.other_name_campaign else R.string.other_name_multiplayer),
        )
        return try {
            packageManager.getLaunchIntentForPackage(pkg) ?: return null
            pkg to label
        } catch (_: Throwable) {
            null
        }
    }

    /**
     * The second entry: the multiplayer in the app that carries both titles,
     * else the game's other app when that is installed.
     */
    private fun refreshSecondEntry() {
        if (secondTitle != null) {
            secondButton.visibility = android.view.View.VISIBLE
            secondButton.text = getString(R.string.title_multiplayer)
            return
        }
        val other = otherApp()
        if (other == null) {
            secondButton.visibility = android.view.View.GONE
        } else {
            secondButton.visibility = android.view.View.VISIBLE
            secondButton.text = other.second
        }
    }

    /** The Bots entry says what is set, so it is read without opening anything. */
    private fun refreshBotsEntry() {
        if (!Title.hasBots) return
        botsButton.visibility = android.view.View.VISIBLE
        val count = prefs.botCount
        botsButton.text = if (count == 0) getString(R.string.entry_bots_off)
        else getString(
            R.string.entry_bots, count,
            resources.getStringArray(R.array.bot_skills)[prefs.botSkill].uppercase(),
        )
    }

    /**
     * The bots of the matches this device hosts, set where a match is started
     * from: the same three settings as the Multiplayer tab of Settings, which
     * has the rest.
     */
    private fun showBots() {
        val view = layoutInflater.inflate(R.layout.dialog_bots, null)
        val countLabel: TextView = view.findViewById(R.id.bot_count_label)
        val count: android.widget.SeekBar = view.findViewById(R.id.bot_count)
        val skillLabel: TextView = view.findViewById(R.id.bot_skill_label)
        val skill: android.widget.SeekBar = view.findViewById(R.id.bot_skill)
        val against: androidx.appcompat.widget.SwitchCompat = view.findViewById(R.id.bots_against_players)
        val skills = resources.getStringArray(R.array.bot_skills)
        fun countText(value: Int) =
            if (value == 0) getString(R.string.bot_count_none) else getString(R.string.bot_count, value)

        count.max = Prefs.MAX_BOTS
        count.progress = prefs.botCount
        countLabel.text = countText(count.progress)
        skill.max = skills.size - 1
        skill.progress = prefs.botSkill
        skillLabel.text = getString(R.string.bot_skill, skills[skill.progress])
        against.isChecked = prefs.botsAgainstPlayers
        val changed = object : android.widget.SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(bar: android.widget.SeekBar, value: Int, fromUser: Boolean) {
                if (bar === count) {
                    countLabel.text = countText(value)
                    if (fromUser) prefs.botCount = value
                } else {
                    skillLabel.text = getString(R.string.bot_skill, skills[value])
                    if (fromUser) prefs.botSkill = value
                }
                refreshBotsEntry()
            }
            override fun onStartTrackingTouch(bar: android.widget.SeekBar) = Unit
            override fun onStopTrackingTouch(bar: android.widget.SeekBar) = Unit
        }
        count.setOnSeekBarChangeListener(changed)
        skill.setOnSeekBarChangeListener(changed)
        against.setOnCheckedChangeListener { _, on -> prefs.botsAgainstPlayers = on }

        AlertDialog.Builder(this)
            .setTitle(R.string.bots_title)
            .setView(view)
            .setPositiveButton(R.string.done, null)
            .setNeutralButton(R.string.bots_more) { _, _ ->
                startActivity(
                    Intent(this, SettingsActivity::class.java).apply {
                        putExtra(SettingsActivity.EXTRA_TAB, SettingsActivity.TAB_MULTIPLAYER)
                    },
                )
            }
            .show()
    }

    private fun launchOtherApp() {
        val other = otherApp()
        if (other == null) {
            showDetail(
                getString(R.string.play),
                getString(
                    R.string.other_missing,
                    getString(
                        if (isMp()) R.string.other_name_campaign
                        else R.string.other_name_multiplayer,
                    ),
                ),
            )
            return
        }
        showDetail(other.second, "")
        try {
            startActivity(packageManager.getLaunchIntentForPackage(other.first))
        } catch (_: Throwable) {
        }
    }

    /**
     * Physical controllers. A handheld's own pad counts too: the Retroid
     * Pocket's is wired over USB inside the case and Android reports it as
     * external, so it cannot be told from a plugged-in one.
     */
    private fun physicalPadCount(): Int {
        var count = 0
        for (id in InputDevice.getDeviceIds()) {
            val device = InputDevice.getDevice(id) ?: continue
            if (device.isVirtual) continue
            val sources = device.sources
            if (sources and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD ||
                sources and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK) {
                count++
            }
        }
        return count
    }

    private fun showSplitscreenWarning(title: Title, launchData: String) {
        AlertDialog.Builder(this)
            .setTitle(R.string.splitscreen_title)
            .setMessage(R.string.splitscreen_warning)
            .setPositiveButton(R.string.play_anyway) { _, _ -> GameActivity.start(this, title, launchData) }
            .setNegativeButton(R.string.cancel, null)
            .setNeutralButton(R.string.dont_show_again) { _, _ ->
                prefs.seenPadWarning = true
                GameActivity.start(this, title, launchData)
            }
            .show()
    }

    private var selectedEntry: Button? = null

    /** The PC launcher's black-bar highlight on the chosen entry. */
    private fun select(entry: Button) {
        selectedEntry?.setBackgroundColor(android.graphics.Color.TRANSPARENT)
        selectedEntry?.setTextColor(0xFFE8EAED.toInt())
        selectedEntry = entry
        entry.setBackgroundResource(R.drawable.bg_entry_selected)
        entry.setTextColor(0xFF101828.toInt())
    }

    /** TU-aware asset match: TU0 never takes a TU6 APK and vice versa.
     * Unmarked (pre-TU) assets match TU0 only, the version they were. */
    private fun assetMatches(asset: String, wantTitle: String, wantTu: String): Boolean {
        if (!asset.contains(wantTitle, ignoreCase = true)) return false
        val hasTu6 = asset.contains("tu6", ignoreCase = true)
        return if (wantTu == "tu6") hasTu6 else !hasTu6
    }

    /** A newer published APK, if there is one: silent when offline. */
    private fun checkForUpdates(manual: Boolean = false) {
        // A title with no published builds has nothing to compare against.
        if (BuildConfig.UPDATE_REPO.isEmpty()) return
        thread(name = "update-check", isDaemon = true) {
            var offered = false
            try {
                val list = BuildConfig.UPDATE_API.ifEmpty {
                    "https://api.github.com/repos/${BuildConfig.UPDATE_REPO}/releases?per_page=10"
                }
                val connection = (URL(list)
                    .openConnection() as HttpURLConnection).apply {
                    connectTimeout = 15000
                    readTimeout = 15000
                    setRequestProperty("Accept", "application/vnd.github+json")
                }
                if (connection.responseCode == 200) {
                    val releases = JSONArray(connection.inputStream.bufferedReader().readText())
                    // The asset's name says which app it is: the one app of
                    // each game is <game>-recomp-<tag>.apk.
                    val wantTitle = when {
                        Title.isMw3 -> "mw3"
                        Title.isCod4 -> "cod4-recomp"
                        Title.isAw -> "aw-recomp"
                        Title.both -> "mw2-recomp"
                        isMp() -> "multiplayer"
                        else -> "campaign"
                    }
                    val wantTu = if (isTu6()) "tu6" else "tu0"
                    for (i in 0 until releases.length()) {
                        val release = releases.getJSONObject(i)
                        if (release.optBoolean("draft")) continue
                        val assets = release.optJSONArray("assets") ?: continue
                        val matches = (0 until assets.length()).any {
                            assetMatches(assets.getJSONObject(it).optString("name"), wantTitle, wantTu)
                        }
                        if (!matches) continue
                        val tag = release.optString("tag_name")
                        // What Android will compare: the number the release's
                        // notes give, or the one its tag stands for.
                        val code = Regex("APK versionCode:\\s*(\\d+)")
                            .find(release.optString("body"))?.groupValues?.get(1)?.toIntOrNull()
                            ?: versionCodeOfTag(tag)
                        val installed = packageManager.getPackageInfo(packageName, 0).let {
                            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) it.longVersionCode.toInt()
                            else it.versionCode
                        }
                        // Newest matching release only: older ones stay buried.
                        if (code > installed && tag.isNotEmpty() && tag != prefs.seenUpdateTag) {
                            val page = release.optString("html_url")
                            val apkUrl = (0 until assets.length())
                                .map { assets.getJSONObject(it) }
                                .firstOrNull {
                                    it.optString("name").endsWith(".apk", ignoreCase = true) &&
                                        assetMatches(it.optString("name"), wantTitle, wantTu)
                                }?.optString("browser_download_url") ?: ""
                            val notes = release.optString("body")
                            offered = true
                            runOnUiThread { showUpdateDialog(tag, page, apkUrl, notes, installed, code) }
                        }
                        break
                    }
                }
            } catch (_: Throwable) {
            }
            if (manual && !offered) {
                runOnUiThread {
                    AlertDialog.Builder(this)
                        .setMessage(R.string.up_to_date)
                        .setPositiveButton(R.string.close, null)
                        .show()
                }
            }
        }
    }

    private fun showUpdateDialog(tag: String, page: String, apkUrl: String, notes: String, installed: Int, code: Int) {
        val body = TextView(this)
        body.setPadding(32, 8, 32, 8)
        body.textSize = 12f
        body.setLineSpacing(0f, 1.15f)
        body.text = android.text.SpannableStringBuilder(getString(R.string.update_message, installed, code))
            .append("\n\n")
            .append(ReleaseNotes.styled(notes, resources.displayMetrics.density))
        val scroll = ScrollView(this)
        scroll.addView(body)
        AlertDialog.Builder(this)
            .setTitle(getString(R.string.update_title, tag))
            .setView(scroll)
            .setPositiveButton(R.string.update_download) { _, _ ->
                if (apkUrl.isNotEmpty()) downloadAndInstall(apkUrl)
                else openPage(page)
            }
            .setNeutralButton(R.string.skip_version) { _, _ -> prefs.seenUpdateTag = tag }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun openPage(page: String) {
        try {
            startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(page)))
        } catch (_: Throwable) {
        }
    }

    /** TU6 lives in separate PC archives; Android stays TU0 (future work, not started). */
    private fun openReleasesPage() {
        if (BuildConfig.UPDATE_REPO.isEmpty()) return
        openPage("https://github.com/${BuildConfig.UPDATE_REPO}/releases")
    }

    private fun downloadAndInstall(apkUrl: String) {
        if (!packageManager.canRequestPackageInstalls()) {
            // System sends the user to allow installs from this app; the
            // download starts when they come back (onResume).
            pendingUpdateUrl = apkUrl
            try {
                startActivity(
                    Intent(
                        android.provider.Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                        Uri.parse("package:$packageName"),
                    )
                )
            } catch (_: Throwable) {
                pendingUpdateUrl = null
            }
            return
        }
        startUpdateDownload(apkUrl)
    }

    private fun startUpdateDownload(apkUrl: String) {
        try {
            val manager = getSystemService(DOWNLOAD_SERVICE) as DownloadManager
            val request = DownloadManager.Request(Uri.parse(apkUrl))
                .setTitle(getString(R.string.app_name))
                .setDescription("Downloading APK")
                .setNotificationVisibility(DownloadManager.Request.VISIBILITY_VISIBLE_NOTIFY_COMPLETED)
                .setDestinationInExternalPublicDir(
                    Environment.DIRECTORY_DOWNLOADS,
                    if (Title.isMw3) "mw3-update.apk" else "mw2-update.apk",
                )
                .setAllowedOverMetered(true)
            pendingInstallId = manager.enqueue(request)
            Toast.makeText(this, R.string.update_downloading, Toast.LENGTH_SHORT).show()
        } catch (_: Throwable) {
            openReleasesPage()
        }
    }

    private fun installDownloadedApk(id: Long) {
        try {
            val manager = getSystemService(DOWNLOAD_SERVICE) as DownloadManager
            val uri = manager.getUriForDownloadedFile(id) ?: throw java.io.IOException("the download failed")
            val intent = Intent(Intent.ACTION_VIEW).apply {
                setDataAndType(uri, "application/vnd.android.package-archive")
                addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_GRANT_READ_URI_PERMISSION)
            }
            startActivity(intent)
        } catch (_: Throwable) {
            AlertDialog.Builder(this)
                .setMessage(R.string.update_failed)
                .setPositiveButton(R.string.open_page) { _, _ -> openReleasesPage() }
                .setNegativeButton(R.string.cancel, null)
                .show()
        }
    }

    private fun showLastCrash(crash: String) {
        AlertDialog.Builder(this)
            .setTitle(R.string.last_crash)
            .setMessage(crash.take(4000))
            .setPositiveButton(R.string.close) { _, _ -> Trail.forgetCrash(this) }
            .setNeutralButton(R.string.share) { _, _ -> LogShare.share(this, crash) }
            .show()
    }

    /**
     * The log screen: a summary of it, with the whole thing one button away
     * (LogSummary says what is kept and what is only counted). Share sends the
     * whole log, because that is what a bug report needs.
     */
    private fun showLog() {
        val session = if (nativeReady) safely("") { NativeBridge.nativeLogTail() } else ""
        val renderer = if (nativeReady) safely("") { NativeBridge.nativeRendererInfo() } else ""
        showLogText(LogSummary.build(this, renderer, session), 11f)
    }

    /** Everything, as it was, for when the summary is not enough. */
    private fun showWholeLog() {
        val session = if (nativeReady) safely("") { NativeBridge.nativeLogTail() } else ""
        showLogText(LogSummary.whole(this, session), 10f)
    }

    private fun showLogText(text: String, textSize: Float) {
        val body = TextView(this)
        body.setPadding(32, 24, 32, 24)
        body.textSize = textSize
        body.setTextIsSelectable(true)
        body.text = text
        val scroll = ScrollView(this)
        scroll.addView(body)
        AlertDialog.Builder(this)
            .setTitle(R.string.view_log)
            .setView(scroll)
            .setPositiveButton(R.string.close, null)
            .setNegativeButton(R.string.log_everything) { _, _ -> showWholeLog() }
            .setNeutralButton(R.string.share) { _, _ -> LogShare.share(this, wholeLog()) }
            .show()
    }

    /** What Share sends, as a file (LogShare.kt): every log and crash file, not the summary. */
    private fun wholeLog(): String {
        val session = if (nativeReady) safely("") { NativeBridge.nativeLogTail() } else ""
        return LogSummary.whole(this, session)
    }

    companion object {
        private const val STATE_AUTO_LAUNCHED = "hasAutoLaunched"
        private const val STATE_SETUP_OPENED = "setupOpened"

        /** A mode change takes under a second on the devices seen; the notice waits this long for it. */
        private const val DISPLAY_SETTLE_MS = 1500L

        /** How often the launcher looks at a build of the game code that is under way. */
        private const val BUILD_TICK_MS = 2000L

        /** The large button's height, in rows of the left column's entries. */
        private const val PLAY_ROWS = 2.4f

        /** The entries the left column is laid out for; more than these and its rows are shorter. */
        private const val ENTRY_ROWS = 6

        /** A group's heading, in rows. */
        private const val HEADING_ROWS = 0.4f
    }
}

/** The end of a file, which is the part of a log anyone wants. */
fun File.readTextTail(bytes: Int): String = try {
    val length = length()
    if (length <= bytes) readText()
    else inputStream().use { stream ->
        stream.skip(length - bytes)
        stream.readBytes().toString(Charsets.UTF_8)
    }
} catch (_: Throwable) {
    ""
}

/**
 * The versionCode a release tag stands for: v1.2.3 is 10203, as the release
 * workflow builds it (.github/workflows/android-release.yml). 0 for a tag
 * that is not a version.
 */
fun versionCodeOfTag(tag: String): Int {
    val parts = Regex("^v?(\\d+)\\.(\\d+)\\.(\\d+)").find(tag)?.groupValues ?: return 0
    return parts[1].toInt() * 10000 + parts[2].toInt() * 100 + parts[3].toInt()
}
