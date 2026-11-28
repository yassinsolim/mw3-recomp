package com.mw2.recomp

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.SeekBar
import android.widget.TextView
import androidx.activity.OnBackPressedCallback
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.widget.SwitchCompat
import androidx.core.widget.doAfterTextChanged
import java.io.File
import kotlin.math.roundToInt

/**
 * Everything the player can change outside a run. Each setting is written
 * the moment it is touched: there is no "apply", because there is nothing
 * to apply to -- the values are read when a run starts.
 */
class SettingsActivity : AppCompatActivity() {

    companion object {
        const val EXTRA_TAB = "mw2.tab"
        const val TAB_GRAPHICS = 0
        const val TAB_CONTROLS = 1
        const val TAB_NETWORK = 2
        const val TAB_FILES = 3
        /** The multiplayer's own: bots and, in Modern Warfare 3, unlocks and match options. */
        const val TAB_MULTIPLAYER = 4
    }

    private lateinit var prefs: Prefs
    private val tabButtons = mutableListOf<Button>()
    private val tabPages = mutableListOf<android.view.View>()

    /** The presets' buttons, in GraphicsPresets' order. */
    private val presetButtons = intArrayOf(
        R.id.preset_automatic, R.id.preset_battery, R.id.preset_balanced, R.id.preset_best, R.id.preset_custom,
    )

    /** The limits the frame-rate slider stops at after Auto (Prefs.fpsLimits), as last shown. */
    private var fpsStops: List<Int> = emptyList()

    /** The page is being made to show what Prefs holds: a button checked meanwhile is not the player's choice. */
    private var showing = false

    /** The back key on the Advanced page goes back to Graphics, not out of Settings. */
    private val backFromAdvanced = object : OnBackPressedCallback(false) {
        override fun handleOnBackPressed() = showAdvanced(false)
    }

    private val modBrowse = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult(),
    ) { result ->
        if (result.resultCode != Activity.RESULT_OK) return@registerForActivityResult
        val path = result.data?.getStringExtra(FileBrowserActivity.EXTRA_PATH)
            ?: return@registerForActivityResult
        installModFolder(File(path))
    }

    /** The disc image the executables are read from when the game files are not installed (BuildOnline.kt). */
    private val discImage = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult(),
    ) { result ->
        if (result.resultCode != Activity.RESULT_OK) return@registerForActivityResult
        val image = result.data?.data ?: return@registerForActivityResult
        prepareBuildOnline { BuildOnline.fromDiscImage(this, image) }
    }

    /**
     * The microphone, asked for when the player turns voice chat on and at
     * no other time. Refused, the switch goes back off: voice chat without a
     * microphone would be a setting that says one thing and does another.
     */
    private val microphonePermission = registerForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { granted ->
        if (!granted) {
            findViewById<SwitchCompat>(R.id.voice_chat).isChecked = false
            android.widget.Toast.makeText(this, R.string.voice_denied, android.widget.Toast.LENGTH_LONG).show()
        }
    }

    private fun hasMicrophone(): Boolean =
        checkSelfPermission(android.Manifest.permission.RECORD_AUDIO) ==
            android.content.pm.PackageManager.PERMISSION_GRANTED

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_settings)
        prefs = Prefs(this)
        colorValues(
            R.id.resolution_label, R.id.render_scale_label, R.id.detail_label, R.id.fps_limit_label,
            R.id.sun_shadows_label, R.id.scene_label, R.id.driver_label, R.id.fov_label, R.id.texture_label,
            R.id.touch_look_label, R.id.touch_look_vertical_label, R.id.touch_look_aiming_label, R.id.gyro_label,
            R.id.gyro_sensitivity_label, R.id.bot_count_label, R.id.bot_skill_label,
            R.id.match_gravity_label, R.id.match_speed_label, R.id.match_jump_label,
        )
        title = getString(R.string.settings)

        // ---- tab bar (plain views, no new deps): one page visible at a time ----
        tabButtons.add(findViewById(R.id.tab_graphics))
        tabButtons.add(findViewById(R.id.tab_controls))
        tabButtons.add(findViewById(R.id.tab_network))
        tabButtons.add(findViewById(R.id.tab_files))
        tabPages.add(findViewById(R.id.page_graphics))
        tabPages.add(findViewById(R.id.page_controls))
        tabPages.add(findViewById(R.id.page_network))
        tabPages.add(findViewById(R.id.page_files))
        // What only a multiplayer with bots reads, in a tab of its own: the
        // bots, and below them Modern Warfare 3's unlock switch, map vote and
        // match options, which the other two games have not got.
        val hasMultiplayerTab = Title.hasBots
        if (hasMultiplayerTab) {
            val tab: Button = findViewById(R.id.tab_multiplayer)
            tab.visibility = android.view.View.VISIBLE
            tabButtons.add(tab)
            tabPages.add(findViewById(R.id.page_multiplayer))
        }
        tabButtons.forEachIndexed { index, button ->
            button.setOnClickListener { selectTab(index) }
        }
        selectTab(intent.getIntExtra(EXTRA_TAB, TAB_GRAPHICS).coerceIn(0, tabButtons.lastIndex))

        // ---- picture ----
        bindGraphics()
        showGraphics()

        switch(R.id.msaa, prefs.multisampling) { prefs.multisampling = it }
        // Detail at distance is Modern Warfare 2's, the only title with the hook.
        if (Title.hasDetailBias) {
            findViewById<android.view.View>(R.id.detail_group).visibility = android.view.View.VISIBLE
            val details = resources.getStringArray(R.array.detail_biases)
            val detailLabel: TextView = findViewById(R.id.detail_label)
            val detail: SeekBar = findViewById(R.id.detail)
            detail.max = Prefs.DETAIL_BIASES.size - 1
            detail.progress = Prefs.DETAIL_BIASES.indexOf(prefs.detailBias).coerceAtLeast(0)
            detailLabel.text = getString(R.string.detail, details[detail.progress])
            detail.setOnSeekBarChangeListener(object : Simple() {
                override fun onProgressChanged(bar: SeekBar, value: Int, fromUser: Boolean) {
                    detailLabel.text = getString(R.string.detail, details[value])
                    if (fromUser) prefs.detailBias = Prefs.DETAIL_BIASES[value]
                }
            })
        }
        val sharpeningLabel: TextView = findViewById(R.id.sharpening_label)
        val sharpening: SeekBar = findViewById(R.id.sharpening)
        fun sharpeningText(tenths: Int) =
            if (tenths == 0) getString(R.string.sharpening_off) else getString(R.string.sharpening, tenths * 10)
        sharpening.max = 10                       // off, then 10% to 100%
        sharpening.progress = (prefs.sharpening / 10f).roundToInt()
        sharpeningLabel.text = sharpeningText(sharpening.progress)
        sharpening.setOnSeekBarChangeListener(object : Simple() {
            override fun onProgressChanged(bar: SeekBar, value: Int, fromUser: Boolean) {
                sharpeningLabel.text = sharpeningText(value)
                if (fromUser) prefs.sharpening = value * 10
            }
        })
        switch(R.id.prewarm_all, prefs.prewarmAllShaders) { prefs.prewarmAllShaders = it }
        switch(R.id.async_pipelines, prefs.asyncPipelines) { prefs.asyncPipelines = it }
        switch(R.id.persist_pipeline, prefs.persistPipelineCache) { prefs.persistPipelineCache = it }
        if (hasMultiplayerTab) {
            val botCountLabel: TextView = findViewById(R.id.bot_count_label)
            val botCount: SeekBar = findViewById(R.id.bot_count)
            botCount.max = Prefs.MAX_BOTS
            botCount.progress = prefs.botCount
            botCountLabel.text = botCountText(botCount.progress)
            botCount.setOnSeekBarChangeListener(object : Simple() {
                override fun onProgressChanged(bar: SeekBar, value: Int, fromUser: Boolean) {
                    botCountLabel.text = botCountText(value)
                    if (fromUser) prefs.botCount = value
                }
            })

            val botSkills = resources.getStringArray(R.array.bot_skills)
            val botSkillLabel: TextView = findViewById(R.id.bot_skill_label)
            val botSkill: SeekBar = findViewById(R.id.bot_skill)
            botSkill.max = botSkills.size - 1
            botSkill.progress = prefs.botSkill
            botSkillLabel.text = getString(R.string.bot_skill, botSkills[botSkill.progress])
            botSkill.setOnSeekBarChangeListener(object : Simple() {
                override fun onProgressChanged(bar: SeekBar, value: Int, fromUser: Boolean) {
                    botSkillLabel.text = getString(R.string.bot_skill, botSkills[value])
                    if (fromUser) prefs.botSkill = value
                }
            })
            switch(R.id.bots_against_players, prefs.botsAgainstPlayers) { prefs.botsAgainstPlayers = it }
        }
        // Advanced Warfare has the unlock switch and none of the rest
        // (runtime/aw_unlock_all.cpp): its own wording, since it has no
        // camouflages or classes to add.
        if (hasMultiplayerTab && (Title.isMw3 || Title.isAw)) {
            findViewById<android.view.View>(R.id.unlocks_group).visibility = android.view.View.VISIBLE
            // The wording first: the switch keeps the label it finds.
            if (Title.isAw) {
                findViewById<SwitchCompat>(R.id.unlock_all_items).setText(R.string.unlock_all_items_aw)
                findViewById<TextView>(R.id.unlock_all_items_hint).setText(R.string.unlock_all_items_hint_aw)
            }
            switch(R.id.unlock_all_items, prefs.unlockAllItems) { prefs.unlockAllItems = it }
        }
        if (hasMultiplayerTab && Title.isMw3) {
            findViewById<android.view.View>(R.id.mw3_multiplayer_group).visibility = android.view.View.VISIBLE
            findViewById<Button>(R.id.camo_packs).setOnClickListener { showCamoPacks() }

            matchOption(R.id.match_gravity_label, R.id.match_gravity, R.string.match_gravity,
                Prefs.MATCH_GRAVITIES, 800, prefs.matchGravity) { prefs.matchGravity = it }
            matchOption(R.id.match_speed_label, R.id.match_speed, R.string.match_speed,
                Prefs.MATCH_SPEEDS, 100, prefs.matchSpeed) { prefs.matchSpeed = it }
            matchOption(R.id.match_jump_label, R.id.match_jump, R.string.match_jump,
                Prefs.MATCH_JUMPS, 39, prefs.matchJump) { prefs.matchJump = it }
            switch(R.id.match_sprint, prefs.matchUnlimitedSprint) { prefs.matchUnlimitedSprint = it }
            switch(R.id.match_fall_damage, prefs.matchFallDamage) { prefs.matchFallDamage = it }
            switch(R.id.map_vote, prefs.mapVote) { prefs.mapVote = it }
        }

        val fovLabel: TextView = findViewById(R.id.fov_label)
        val fovSlider: SeekBar = findViewById(R.id.fov_slider)
        fovSlider.max = 45 // 65 to 110
        fovSlider.progress = (prefs.fov - 65).coerceIn(0, 45)
        fovLabel.text = getString(R.string.fov, fovSlider.progress + 65)
        fovSlider.setOnSeekBarChangeListener(object : Simple() {
            override fun onProgressChanged(bar: SeekBar, value: Int, fromUser: Boolean) {
                fovLabel.text = getString(R.string.fov, value + 65)
                if (fromUser) prefs.fov = value + 65
            }
        })

        switch(R.id.stats, prefs.showStats) { prefs.showStats = it }
        switch(R.id.fill_screen, prefs.fillScreen) { prefs.fillScreen = it }

        val textureLabel: TextView = findViewById(R.id.texture_label)
        val texture: SeekBar = findViewById(R.id.texture_budget)
        // 0 means "let the runtime decide"; then 128 MB steps to 2 GB.
        texture.max = 15
        texture.progress = if (prefs.textureBudgetMB <= 0) 0
        else (prefs.textureBudgetMB / 128).coerceIn(1, 15)
        textureLabel.text = textureText(texture.progress)
        texture.setOnSeekBarChangeListener(object : Simple() {
            override fun onProgressChanged(bar: SeekBar, value: Int, fromUser: Boolean) {
                textureLabel.text = textureText(value)
                if (fromUser) prefs.textureBudgetMB = if (value == 0) 0 else value * 128
            }
        })

        // ---- sound ----
        switch(R.id.audio, prefs.audioEnabled) { prefs.audioEnabled = it }

        // ---- controls ----
        switch(R.id.touch, prefs.touchControls) { prefs.touchControls = it }
        switch(R.id.gamepad, prefs.gamepadEnabled) { prefs.gamepadEnabled = it }
        switch(R.id.lan_accept, prefs.lanAccept) { prefs.lanAccept = it }
        val lanIface: EditText = findViewById(R.id.lan_iface)
        lanIface.setText(prefs.lanIface)
        lanIface.doAfterTextChanged { prefs.lanIface = it.toString() }
        // ---- performance reports (PerfReports.kt) ----
        if (PerfReports.game != null) {
            findViewById<android.view.View>(R.id.perf_group).visibility = android.view.View.VISIBLE
            findViewById<TextView>(R.id.perf_hint).text = getString(R.string.perf_reports_hint, PerfReports.privacyPage)
            val perfSwitch: SwitchCompat = findViewById(R.id.perf_reports)
            switch(R.id.perf_reports, PerfReports.chosen(this)) { on ->
                if (!on) PerfReports.choose(this, false)
                // Turned on only where there is somewhere to send them: a
                // queue that waits for a server that is not there is no use.
                else kotlin.concurrent.thread(name = "perf-health", isDaemon = true) {
                    val answers = PerfReports.serverAnswers(applicationContext, fresh = true)
                    runOnUiThread {
                        if (answers) PerfReports.choose(this, true)
                        else {
                            perfSwitch.isChecked = false
                            android.widget.Toast.makeText(this, R.string.perf_no_server, android.widget.Toast.LENGTH_LONG).show()
                        }
                    }
                }
            }
            findViewById<Button>(R.id.perf_example).setOnClickListener { PerfReports.showExample(this) }
            findViewById<Button>(R.id.perf_sent).setOnClickListener { PerfReports.showSent(this) }
        }
        val playerName: EditText = findViewById(R.id.player_name)
        playerName.setText(prefs.playerName)
        playerName.hint = prefs.defaultPlayerName
        playerName.doAfterTextChanged { prefs.playerName = it.toString() }
        // How the multiplayer reaches other players (GameActivity sets
        // MW2_NETWORK from it); the campaign has no network.
        if (Title.MULTIPLAYER.carried) {
            findViewById<android.view.View>(R.id.network_mode_group).visibility = android.view.View.VISIBLE
            val modes = mapOf(
                Prefs.NETWORK_ONLINE to R.id.network_online,
                Prefs.NETWORK_LAN to R.id.network_lan,
                Prefs.NETWORK_OFF to R.id.network_off,
            )
            val networkMode: RadioGroup = findViewById(R.id.network_mode)
            networkMode.check(modes.getValue(prefs.networkMode))
            networkMode.setOnCheckedChangeListener { _, checked ->
                modes.entries.firstOrNull { it.value == checked }?.let { prefs.networkMode = it.key }
            }
            val relayAddress: EditText = findViewById(R.id.relay_address)
            relayAddress.setText(prefs.relayAddress)
            relayAddress.doAfterTextChanged { prefs.relayAddress = it.toString() }
            if (Title.isMw3) {
                findViewById<android.view.View>(R.id.live_sign_in_group).visibility = android.view.View.VISIBLE
                switch(R.id.live_sign_in, prefs.liveSignIn) { prefs.liveSignIn = it }
                findViewById<android.view.View>(R.id.voice_chat_group).visibility = android.view.View.VISIBLE
                // A permission taken back in the system's settings shows as
                // the switch being off, which is what the game will do.
                switch(R.id.voice_chat, prefs.voiceChat && hasMicrophone()) { on ->
                    prefs.voiceChat = on
                    if (on && !hasMicrophone()) {
                        microphonePermission.launch(android.Manifest.permission.RECORD_AUDIO)
                    }
                }
                switch(R.id.voice_ptt, prefs.voicePushToTalk) { prefs.voicePushToTalk = it }
            }
        }
        switch(R.id.vibration, prefs.vibration) { prefs.vibration = it }
        // Which way looking turns, for a finger and the gyroscope (LookInvert),
        // and for a controller's right stick, left and right only: the titles
        // have their own Invert option for up and down.
        switch(R.id.invert_horizontal, prefs.invertLookHorizontal) { prefs.invertLookHorizontal = it }
        switch(R.id.invert_vertical, prefs.invertLookVertical) { prefs.invertLookVertical = it }
        switch(R.id.invert_pad_horizontal, prefs.invertPadHorizontal) { prefs.invertPadHorizontal = it }

        // Looking by touch (TouchLook.kt): degrees for a centimetre of
        // glass, with what that comes to on this screen beside it.
        val metrics = resources.displayMetrics
        val widest = maxOf(metrics.widthPixels, metrics.heightPixels)
        val slowest = TouchLook.MIN_DEGREES_PER_CM.roundToInt()
        slider(
            R.id.touch_look, R.id.touch_look_label,
            TouchLook.MAX_DEGREES_PER_CM.roundToInt() - slowest, prefs.touchLookSensitivity.roundToInt() - slowest,
            {
                val degrees = slowest + it
                getString(
                    R.string.touch_look, degrees,
                    TouchLook.halfTurnInScreens(degrees.toFloat(), widest, metrics.densityDpi),
                )
            },
            { prefs.touchLookSensitivity = (slowest + it).toFloat() },
        )
        // 25 to 200 percent in steps of five.
        slider(
            R.id.touch_look_vertical, R.id.touch_look_vertical_label,
            35, ((prefs.touchLookVertical * 100f).roundToInt() - 25) / 5,
            { getString(R.string.touch_look_vertical, 25 + it * 5) },
            { prefs.touchLookVertical = (25 + it * 5) / 100f },
        )
        slider(
            R.id.touch_look_aiming, R.id.touch_look_aiming_label,
            35, ((prefs.touchLookAiming * 100f).roundToInt() - 25) / 5,
            { getString(R.string.touch_look_aiming, 25 + it * 5) },
            { prefs.touchLookAiming = (25 + it * 5) / 100f },
        )
        switch(R.id.tap_to_fire, prefs.tapToFire) { prefs.tapToFire = it }
        switch(R.id.aim_when_firing, prefs.aimWhenFiring) { prefs.aimWhenFiring = it }

        // The gyroscope, where the phone has one.
        if (GyroInput(this).available) {
            val gyroChoices = resources.getStringArray(R.array.gyro_choices)
            slider(
                R.id.gyro, R.id.gyro_label, gyroChoices.size - 1, prefs.gyroMode,
                { getString(R.string.gyro_label, gyroChoices[it]) }, { prefs.gyroMode = it },
            )
            // A quarter to four times, in quarters.
            slider(
                R.id.gyro_sensitivity, R.id.gyro_sensitivity_label,
                15, (prefs.gyroSensitivity * 4f).roundToInt() - 1,
                { getString(R.string.gyro_sensitivity, (it + 1) / 4f) },
                { prefs.gyroSensitivity = (it + 1) / 4f },
            )
        } else {
            for (id in intArrayOf(R.id.gyro, R.id.gyro_label, R.id.gyro_sensitivity, R.id.gyro_sensitivity_label)) {
                findViewById<View>(id).visibility = View.GONE
            }
            findViewById<TextView>(R.id.gyro_hint).setText(R.string.gyro_missing)
        }

        // ---- the game files ----
        showPaths()
        // The way back to the install screen once the game is installed: to
        // play from the disc image instead of the copied files, or the other
        // way round, or from an image that has moved.
        findViewById<Button>(R.id.change_source).setOnClickListener {
            startActivity(Intent(this, InstallActivity::class.java))
        }

        findViewById<Button>(R.id.forget).setOnClickListener {
            AlertDialog.Builder(this)
                .setTitle(R.string.forget_install)
                .setMessage(R.string.forget_explained)
                .setNegativeButton(R.string.cancel, null)
                .setPositiveButton(R.string.reset) { _, _ ->
                    prefs.gameDirectory = ""
                    prefs.discImage = ""
                    prefs.installDiscImage = ""
                    showPaths()
                }
                .show()
        }
        if (BuildOnline.offered) {
            findViewById<android.view.View>(R.id.build_online_hint).visibility = android.view.View.VISIBLE
            val prepare: Button = findViewById(R.id.build_online)
            prepare.visibility = android.view.View.VISIBLE
            prepare.setOnClickListener {
                val installed = prefs.gameDirectory
                if (BuildOnline.inGameFolder(installed)) {
                    prepareBuildOnline { BuildOnline.fromGameFolder(this, installed) }
                } else {
                    AlertDialog.Builder(this)
                        .setTitle(R.string.build_online)
                        .setMessage(R.string.build_online_pick)
                        .setNegativeButton(R.string.cancel, null)
                        .setPositiveButton(R.string.build_online_choose) { _, _ ->
                            discImage.launch(
                                Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*")
                            )
                        }
                        .show()
                }
            }
        }
        if (GameBuild.offered(this)) {
            // The game code after an update of the app (GameUpdate.kt): from
            // never by itself, through the charger only, to any charge.
            val steps = intArrayOf(Prefs.UPDATE_CODE_NEVER, Prefs.UPDATE_CODE_CHARGING, 80, 50, 30, 15, 0)
            val label: TextView = findViewById(R.id.update_code_label)
            val bar: SeekBar = findViewById(R.id.update_code)
            fun show(step: Int) {
                label.text = when (step) {
                    Prefs.UPDATE_CODE_NEVER -> getString(R.string.update_code_never)
                    Prefs.UPDATE_CODE_CHARGING -> getString(R.string.update_code_charging)
                    0 -> getString(R.string.update_code_always)
                    else -> getString(R.string.update_code_above, step)
                }
            }
            val hint: TextView = findViewById(R.id.update_code_hint)
            for (view in listOf(label, bar, hint)) view.visibility = android.view.View.VISIBLE
            bar.max = steps.size - 1
            bar.progress = steps.indexOf(prefs.updateCodeBattery).coerceAtLeast(0)
            show(steps[bar.progress])
            bar.setOnSeekBarChangeListener(object : Simple() {
                override fun onProgressChanged(bar: SeekBar, value: Int, fromUser: Boolean) {
                    show(steps[value])
                    if (fromUser) prefs.updateCodeBattery = steps[value]
                }
            })
        }

        // ---- storage ----
        val storage: TextView = findViewById(R.id.storage)
        refreshStorage(storage)
        findViewById<Button>(R.id.clear_shaders).setOnClickListener {
            shaderCacheFiles().forEach { try { it.delete() } catch (_: Throwable) {} }
            storage.text = getString(R.string.shaders_cleared)
            refreshStorage(storage)
        }
        findViewById<Button>(R.id.clear_logs).setOnClickListener {
            listOf("game.log", "launcher.log", "last-crash-game.txt", "last-crash-launcher.txt").forEach {
                try {
                    java.io.File(filesDir, it).delete()
                } catch (_: Throwable) {
                }
            }
            storage.text = getString(R.string.logs_cleared)
            refreshStorage(storage)
        }
        findViewById<Button>(R.id.install_mods).setOnClickListener {
            modBrowse.launch(Intent(this, FileBrowserActivity::class.java))
        }
    }

    /** Writes the executables into Download with [write], off the main thread, and says what to do with them. */
    private fun prepareBuildOnline(write: () -> Unit) {
        val working = AlertDialog.Builder(this)
            .setTitle(R.string.build_online)
            .setMessage(R.string.build_online_working)
            .setCancelable(false)
            .show()
        Thread {
            val failure = try {
                write()
                null
            } catch (error: Exception) {
                error.message ?: error.toString()
            }
            runOnUiThread {
                working.dismiss()
                val dialog = AlertDialog.Builder(this).setTitle(R.string.build_online)
                if (failure != null) {
                    dialog.setMessage(getString(R.string.build_online_failed, failure))
                        .setPositiveButton(R.string.close, null)
                } else {
                    dialog.setMessage(
                        getString(
                            R.string.build_online_done,
                            BuildOnline.fileName(Title.CAMPAIGN),
                            BuildOnline.fileName(Title.MULTIPLAYER),
                        )
                    )
                        .setNegativeButton(R.string.close, null)
                        .setPositiveButton(R.string.build_online_guide) { _, _ ->
                            startActivity(Intent(Intent.ACTION_VIEW, BuildOnline.guide))
                        }
                }
                dialog.show()
            }
        }.start()
    }

    /** Slides the gold rule under the chosen tab. */
    private fun moveTabRule(index: Int) {
        val rule = findViewById<android.view.View>(R.id.tab_rule)
        val button = tabButtons[index]
        rule.post {
            val target = button.left + (button.width - rule.width) / 2f
            rule.animate().translationX(target - rule.left).setDuration(150).start()
        }
    }

    /** Custom tab bar: black-bar highlight on the chosen tab, one page shown. */
    private fun selectTab(index: Int) {
        moveTabRule(index)
        tabPages.forEachIndexed { i, page ->
            page.visibility = if (i == index) android.view.View.VISIBLE else android.view.View.GONE
        }
        // The Advanced page stands in for Graphics until a tab is chosen.
        findViewById<android.view.View>(R.id.page_graphics_advanced).visibility = android.view.View.GONE
        backFromAdvanced.isEnabled = false
        tabButtons.forEachIndexed { i, button ->
            if (i == index) {
                button.setBackgroundResource(R.drawable.bg_entry_selected)
                button.setTextColor(0xFF141417.toInt())
            } else {
                button.setBackgroundColor(android.graphics.Color.TRANSPARENT)
                button.setTextColor(0xFFE8EAED.toInt())
            }
        }
    }

    /** The Advanced page in the Graphics page's place, or Graphics again. */
    private fun showAdvanced(on: Boolean) {
        findViewById<android.view.View>(R.id.page_graphics).visibility =
            if (on) android.view.View.GONE else android.view.View.VISIBLE
        findViewById<android.view.View>(R.id.page_graphics_advanced).visibility =
            if (on) android.view.View.VISIBLE else android.view.View.GONE
        backFromAdvanced.isEnabled = on
    }

    /**
     * The presets and the Advanced page, once: what each control writes.
     * Every setting of the Advanced page makes the preset Custom when it is
     * changed by itself (Prefs), so each says the preset again after it.
     */
    private fun bindGraphics() {
        onBackPressedDispatcher.addCallback(this, backFromAdvanced)
        val names = resources.getStringArray(R.array.preset_names)
        val lines = resources.getStringArray(R.array.preset_lines)
        presetButtons.forEachIndexed { preset, id ->
            val text = android.text.SpannableString(names[preset] + "\n" + lines[preset])
            val from = names[preset].length + 1
            text.setSpan(android.text.style.RelativeSizeSpan(0.8f), from, text.length, 0)
            text.setSpan(android.text.style.ForegroundColorSpan(0xFF7D8792.toInt()), from, text.length, 0)
            findViewById<RadioButton>(id).text = text
        }
        findViewById<RadioGroup>(R.id.preset).setOnCheckedChangeListener { _, checked ->
            val preset = presetButtons.indexOf(checked)
            if (showing || preset < 0) return@setOnCheckedChangeListener
            prefs.choosePreset(preset)
            showGraphics()
        }
        findViewById<Button>(R.id.advanced_graphics).setOnClickListener { showAdvanced(true) }
        findViewById<Button>(R.id.advanced_back).setOnClickListener { showAdvanced(false) }

        val resolutionLabel: TextView = findViewById(R.id.resolution_label)
        val resolution: SeekBar = findViewById(R.id.resolution)
        resolution.max = 65                       // 35% to 100%
        resolution.setOnSeekBarChangeListener(object : Simple() {
            override fun onProgressChanged(bar: SeekBar, value: Int, fromUser: Boolean) {
                resolutionLabel.text = getString(R.string.resolution, value + 35)
                if (!fromUser) return
                prefs.resolutionScale = (value + 35) / 100f
                showPreset()
            }
        })

        // The first stop is Auto: the screen's rate, and under it the
        // fastest the device keeps (Prefs.fpsFollowsDevice).
        val fpsLabel: TextView = findViewById(R.id.fps_limit_label)
        findViewById<SeekBar>(R.id.fps_limit).setOnSeekBarChangeListener(object : Simple() {
            override fun onProgressChanged(bar: SeekBar, value: Int, fromUser: Boolean) {
                fpsLabel.text = fpsText(value)
                if (!fromUser) return
                if (value == 0) prefs.followDeviceFps() else prefs.fpsLimit = fpsStops[value - 1]
                showPreset()
            }
        })

        val shadowChoices = resources.getStringArray(R.array.sun_shadows_choices)
        val shadowLabel: TextView = findViewById(R.id.sun_shadows_label)
        val shadows: SeekBar = findViewById(R.id.sun_shadows)
        shadows.max = shadowChoices.size - 1
        shadows.setOnSeekBarChangeListener(object : Simple() {
            override fun onProgressChanged(bar: SeekBar, value: Int, fromUser: Boolean) {
                shadowLabel.text = getString(R.string.sun_shadows, shadowChoices[value])
                if (!fromUser) return
                prefs.sunShadows = value
                showPreset()
            }
        })

        val renderScaleLabel: TextView = findViewById(R.id.render_scale_label)
        val renderScale: SeekBar = findViewById(R.id.render_scale)
        renderScale.max = 2 // 0 = 1x, 1 = 2x, 2 = 3x
        renderScale.setOnSeekBarChangeListener(object : Simple() {
            override fun onProgressChanged(bar: SeekBar, value: Int, fromUser: Boolean) {
                renderScaleLabel.text = getString(R.string.render_scale, value + 1)
                if (!fromUser) return
                prefs.renderScale = value + 1
                showPreset()
            }
        })

        val sceneChoices = resources.getStringArray(R.array.scene_choices)
        val sceneLabel: TextView = findViewById(R.id.scene_label)
        val scene: SeekBar = findViewById(R.id.scene)
        scene.max = sceneChoices.size - 1
        scene.setOnSeekBarChangeListener(object : Simple() {
            override fun onProgressChanged(bar: SeekBar, value: Int, fromUser: Boolean) {
                sceneLabel.text = getString(R.string.scene, sceneChoices[value])
                if (!fromUser) return
                prefs.scene = value
                showPreset()
            }
        })

        // The app's driver or the phone's: a choice, which the app's default
        // does not come back over (Prefs.driverChoiceMade). An imported one
        // is chosen on the drivers' own screen.
        findViewById<RadioGroup>(R.id.driver).setOnCheckedChangeListener { _, checked ->
            if (showing || checked == -1) return@setOnCheckedChangeListener
            val bundled = if (checked == R.id.driver_bundled) DriverStore(this).bundled(prefs) else null
            prefs.driverChoiceMade = true
            prefs.driverDirectory = bundled?.directory?.absolutePath ?: ""
            prefs.driverLibrary = bundled?.library ?: ""
            showGraphics()
        }
        findViewById<Button>(R.id.driver_other).setOnClickListener {
            startActivity(Intent(this, DriverActivity::class.java))
        }
        findViewById<Button>(R.id.reset_graphics).setOnClickListener {
            prefs.resetGraphics()
            showGraphics()
            android.widget.Toast.makeText(this, R.string.reset_graphics_done, android.widget.Toast.LENGTH_SHORT).show()
        }
    }

    private fun fpsText(stop: Int): String =
        if (stop == 0) getString(R.string.fps_limit_auto, prefs.fpsSteps.first())
        else getString(R.string.fps_limit, fpsStops[stop - 1])

    /** The preset's button as Prefs has it. */
    private fun showPreset() {
        showing = true
        findViewById<RadioGroup>(R.id.preset).check(presetButtons[prefs.graphicsPreset])
        showing = false
    }

    /**
     * The presets and the Advanced page as Prefs has them: at the start,
     * after a preset or Reset to automatic wrote them all, and on coming
     * back from the drivers' screen or a game whose menu changed them.
     */
    private fun showGraphics() {
        showPreset()
        val resolution: SeekBar = findViewById(R.id.resolution)
        resolution.progress = ((prefs.resolutionScale * 100f) - 35f).roundToInt().coerceIn(0, 65)
        findViewById<TextView>(R.id.resolution_label).text = getString(R.string.resolution, resolution.progress + 35)

        fpsStops = prefs.fpsLimits
        val fps: SeekBar = findViewById(R.id.fps_limit)
        fps.max = fpsStops.size
        fps.progress = if (prefs.fpsFollowsDevice) 0 else fpsStops.indexOf(prefs.fpsLimit) + 1
        findViewById<TextView>(R.id.fps_limit_label).text = fpsText(fps.progress)

        val shadows: SeekBar = findViewById(R.id.sun_shadows)
        shadows.progress = prefs.sunShadows
        findViewById<TextView>(R.id.sun_shadows_label).text =
            getString(R.string.sun_shadows, resources.getStringArray(R.array.sun_shadows_choices)[shadows.progress])

        val renderScale: SeekBar = findViewById(R.id.render_scale)
        renderScale.progress = (prefs.renderScale - 1).coerceIn(0, 2)
        findViewById<TextView>(R.id.render_scale_label).text = getString(R.string.render_scale, renderScale.progress + 1)

        val scene: SeekBar = findViewById(R.id.scene)
        scene.progress = prefs.scene
        findViewById<TextView>(R.id.scene_label).text =
            getString(R.string.scene, resources.getStringArray(R.array.scene_choices)[scene.progress])

        val store = DriverStore(this)
        val bundled = store.bundled(prefs)
        val directory = prefs.driverDirectory
        findViewById<android.view.View>(R.id.driver_bundled).visibility =
            if (bundled != null) android.view.View.VISIBLE else android.view.View.GONE
        findViewById<TextView>(R.id.driver_label).text = getString(
            R.string.driver_choice,
            when {
                directory.isEmpty() -> getString(R.string.driver_system)
                else -> store.list().firstOrNull { it.directory.absolutePath == directory }?.name ?: File(directory).name
            },
        )
        showing = true
        val drivers: RadioGroup = findViewById(R.id.driver)
        when {
            directory.isEmpty() -> drivers.check(R.id.driver_phone)
            directory == bundled?.directory?.absolutePath -> drivers.check(R.id.driver_bundled)
            // An imported driver is neither.
            else -> drivers.clearCheck()
        }
        showing = false
    }

    /** The camouflage packs' own screen: which are installed, whose each is, adding one. */
    private fun showCamoPacks() {
        startActivity(Intent(this, CamoPacksActivity::class.java))
    }

    /** Extra maps/weapons/modes from an extracted mod folder into game/. */
    private fun installModFolder(folder: File) {
        Thread {
            val text = try {
                val gameDir = prefs.gameDirectory.ifEmpty {
                    (getExternalFilesDir(null) ?: filesDir).absolutePath + "/game"
                }
                val game = File(gameDir)
                if (!folder.isDirectory) throw IllegalStateException(folder.name)
                game.mkdirs()
                val copied = mutableListOf<String>()
                val skipped = mutableListOf<String>()
                folder.listFiles()
                    ?.sortedBy { it.name.lowercase() }
                    ?.forEach { file ->
                        if (!file.isFile) return@forEach
                        val lower = file.name.lowercase()
                        val loadable = lower.endsWith(".ff") || lower.endsWith(".pak") || lower.endsWith(".bik")
                        if (!loadable) {
                            val why =
                                if (lower.endsWith(".xex") || lower.endsWith(".exe") ||
                                    lower.endsWith(".dll") || lower.endsWith(".so")
                                ) getString(R.string.mods_skip_executable)
                                else getString(R.string.mods_skip_plugin)
                            skipped.add("${file.name} ($why)")
                            return@forEach
                        }
                        val to = File(game, file.name)
                        if (to.isFile && to.length() == file.length()) return@forEach
                        val part = File(game, file.name + ".part")
                        try {
                            file.inputStream().use { input ->
                                part.outputStream().use { output -> input.copyTo(output) }
                            }
                            if (!part.renameTo(to)) throw IllegalStateException("write failed")
                            copied.add(file.name)
                        } catch (_: Throwable) {
                            try {
                                part.delete()
                            } catch (_: Throwable) {
                            }
                            skipped.add("${file.name} (could not be written)")
                        }
                    }
                if (copied.isEmpty() && skipped.isEmpty()) getString(R.string.mods_nothing)
                else getString(
                    R.string.mods_installed, copied.size,
                    if (skipped.isEmpty()) "" else getString(
                        R.string.mods_skipped, skipped.joinToString("\n"),
                    ),
                ) + if (copied.isNotEmpty()) "\n\n" + copied.joinToString("\n") else ""
            } catch (e: Throwable) {
                getString(R.string.install_failed, e.message ?: "")
            }
            runOnUiThread {
                AlertDialog.Builder(this)
                    .setTitle(R.string.install_mods)
                    .setMessage(text)
                    .setPositiveButton(R.string.close, null)
                    .show()
            }
        }.start()
    }

    private fun gameBase(): java.io.File = getExternalFilesDir(null) ?: filesDir

    /**
     * Every title's caches (GameActivity.applyOptions names them). The
     * driver's pipeline cache is several files beside the one named: the
     * ones it was closed at, and their index (pipeline.cpp).
     */
    private fun shaderCacheFiles(): List<java.io.File> {
        val base = gameBase()
        return Title.carried.flatMap {
            val pipeline = "pipeline${it.cacheSuffix}.cache"
            listOf(
                java.io.File(base, "shader_cache${it.cacheSuffix}.bin"),
                java.io.File(filesDir, "shader_cache${it.cacheSuffix}.bin"),
                java.io.File(filesDir, pipeline),
            ) + filesDir.listFiles { file -> file.name.startsWith("$pipeline.") }.orEmpty()
        }
    }

    private fun refreshStorage(view: TextView) {        view.text = getString(R.string.storage_computing)
        Thread {
            fun sizeOf(dir: java.io.File): Long = try {
                if (dir.isFile) dir.length()
                else dir.walkTopDown().filter { it.isFile }.sumOf { it.length() }
            } catch (_: Throwable) {
                0L
            }
            val gameDir = prefs.gameDirectory.ifEmpty { null }?.let { java.io.File(it) }
            // Each title's saves (Title.home), which is one folder unless the
            // app carries both.
            val saves = Title.carried
                .map { java.io.File(it.home(gameDir ?: java.io.File(gameBase(), "game")), "saves") }
                .distinct()
            val rows = listOf(
                getString(R.string.storage_game) to (gameDir?.let { sizeOf(it) } ?: 0L),
                getString(R.string.saves_files) to saves.sumOf { sizeOf(it) },
                getString(R.string.storage_shaders) to shaderCacheFiles().sumOf {
                    try {
                        it.length()
                    } catch (_: Throwable) {
                        0L
                    }
                },
                getString(R.string.view_log) to listOf(
                    "game.log", "launcher.log", "last-crash-game.txt", "last-crash-launcher.txt",
                ).sumOf {
                    try {
                        java.io.File(filesDir, it).length()
                    } catch (_: Throwable) {
                        0L
                    }
                },
            )
            val text = rows.joinToString("\n") { (label, bytes) ->
                getString(R.string.storage_line, label, formatSize(bytes))
            }
            runOnUiThread { view.text = text }
        }.start()
    }

    private fun formatSize(bytes: Long): String =
        android.text.format.Formatter.formatFileSize(this, bytes)

    private fun textureText(step: Int): String =
        if (step == 0) getString(R.string.texture_budget_auto)
        else getString(R.string.texture_budget, step * 128)

    private fun botCountText(count: Int): String =
        if (count == 0) getString(R.string.bot_count_none)
        else getString(R.string.bot_count, count)

    /**
     * A match option's slider: it stops at `values`, and the label says which
     * of them is the game's own.
     */
    private fun matchOption(
        labelId: Int, barId: Int, text: Int, values: IntArray, stock: Int, initial: Int,
        onChange: (Int) -> Unit,
    ) {
        val label: TextView = findViewById(labelId)
        val bar: SeekBar = findViewById(barId)
        fun show(value: Int) {
            val named = getString(text, value)
            label.text = if (value == stock) getString(R.string.match_stock, named) else named
        }
        bar.max = values.size - 1
        bar.progress = values.indexOf(initial).coerceAtLeast(0)
        show(values[bar.progress])
        bar.setOnSeekBarChangeListener(object : Simple() {
            override fun onProgressChanged(bar: SeekBar, value: Int, fromUser: Boolean) {
                show(values[value])
                if (fromUser) onChange(values[value])
            }
        })
    }

    private fun switch(id: Int, initial: Boolean, onChange: (Boolean) -> Unit) {
        val view: SwitchCompat = findViewById(id)
        val label = view.text.toString()
        fun show(on: Boolean) {
            val state = getString(if (on) R.string.state_on else R.string.state_off)
            val text = android.text.SpannableString(label + "\n" + state)
            val from = label.length + 1
            text.setSpan(android.text.style.RelativeSizeSpan(0.8f), from, text.length, 0)
            text.setSpan(
                android.text.style.ForegroundColorSpan(if (on) 0xFFFF7A1A.toInt() else 0xFF7D8792.toInt()),
                from, text.length, 0,
            )
            view.text = text
        }
        view.isChecked = initial
        show(initial)
        view.setOnCheckedChangeListener { _, checked -> show(checked); onChange(checked) }
    }

    /** Paints the value after the last ": " of a label in the accent colour. */
    private fun colorValues(vararg ids: Int) {
        for (id in ids) {
            val view: TextView = findViewById(id)
            view.addTextChangedListener(object : android.text.TextWatcher {
                override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) = Unit
                override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) = Unit
                override fun afterTextChanged(e: android.text.Editable?) {
                    if (e == null) return
                    e.getSpans(0, e.length, android.text.style.ForegroundColorSpan::class.java)
                        .forEach { e.removeSpan(it) }
                    val cut = e.toString().lastIndexOf(": ")
                    if (cut >= 0) {
                        e.setSpan(
                            android.text.style.ForegroundColorSpan(0xFFFF7A1A.toInt()),
                            cut + 2, e.length, android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE,
                        )
                    }
                }
            })
            view.text = view.text
        }
    }

    /** A slider of [steps] steps from nothing, with a label that follows it. */
    private fun slider(
        sliderId: Int,
        labelId: Int,
        steps: Int,
        initial: Int,
        label: (Int) -> String,
        onChange: (Int) -> Unit,
    ) {
        val text: TextView = findViewById(labelId)
        val bar: SeekBar = findViewById(sliderId)
        bar.max = steps
        bar.progress = initial.coerceIn(0, steps)
        text.text = label(bar.progress)
        bar.setOnSeekBarChangeListener(object : Simple() {
            override fun onProgressChanged(view: SeekBar, value: Int, fromUser: Boolean) {
                text.text = label(value)
                if (fromUser) onChange(value)
            }
        })
    }

    /** Where the game is read from: the folder, and the disc image when it is played from one. */
    private fun showPaths() {
        val paths: TextView = findViewById(R.id.paths)
        val discs = listOf(prefs.discImage, prefs.installDiscImage).filter { it.isNotEmpty() }
        paths.text = when {
            prefs.gameDirectory.isEmpty() -> getString(R.string.paths_none)
            discs.isEmpty() -> getString(R.string.paths_line, prefs.gameDirectory)
            else -> getString(
                R.string.paths_line_disc, discs.joinToString("\n") { DiscImage.label(this, it) }, prefs.gameDirectory,
            )
        }
    }

    /** Back from the install screen, the drivers' screen or a game, which may have changed what is shown. */
    override fun onResume() {
        super.onResume()
        showPaths()
        showGraphics()
    }

    /** The two methods nobody ever needs, out of the way. */
    private abstract class Simple : SeekBar.OnSeekBarChangeListener {
        override fun onStartTrackingTouch(bar: SeekBar) = Unit
        override fun onStopTrackingTouch(bar: SeekBar) = Unit
    }
}
