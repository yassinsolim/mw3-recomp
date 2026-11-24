package com.mw2.recomp

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.widget.Button
import android.widget.EditText
import android.widget.RadioGroup
import android.widget.SeekBar
import android.widget.TextView
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
            R.id.sun_shadows_label, R.id.fov_label, R.id.texture_label, R.id.look_x_label,
            R.id.look_y_label, R.id.smoothing_label, R.id.bot_count_label, R.id.bot_skill_label,
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
        val resolutionLabel: TextView = findViewById(R.id.resolution_label)
        val resolution: SeekBar = findViewById(R.id.resolution)
        resolution.max = 65                       // 35% to 100%
        resolution.progress = ((prefs.resolutionScale * 100f) - 35f).roundToInt().coerceIn(0, 65)
        resolutionLabel.text = getString(R.string.resolution, resolution.progress + 35)
        resolution.setOnSeekBarChangeListener(object : Simple() {
            override fun onProgressChanged(bar: SeekBar, value: Int, fromUser: Boolean) {
                resolutionLabel.text = getString(R.string.resolution, value + 35)
                if (fromUser) prefs.resolutionScale = (value + 35) / 100f
            }
        })

        val renderScaleLabel: TextView = findViewById(R.id.render_scale_label)
        val renderScale: SeekBar = findViewById(R.id.render_scale)
        renderScale.max = 2 // 0 = 1x, 1 = 2x, 2 = 3x
        renderScale.progress = (prefs.renderScale - 1).coerceIn(0, 2)
        renderScaleLabel.text = getString(R.string.render_scale, renderScale.progress + 1)
        renderScale.setOnSeekBarChangeListener(object : Simple() {
            override fun onProgressChanged(bar: SeekBar, value: Int, fromUser: Boolean) {
                renderScaleLabel.text = getString(R.string.render_scale, value + 1)
                if (fromUser) prefs.renderScale = value + 1
            }
        })

        switch(R.id.msaa, prefs.multisampling) { prefs.multisampling = it }
        // Detail at distance is Modern Warfare 2's, the only title with the hook.
        if (!Title.isMw3) {
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
        val fpsLabel: TextView = findViewById(R.id.fps_limit_label)
val fpsSlider: SeekBar = findViewById(R.id.fps_limit)
fpsSlider.max = Prefs.FPS_LIMITS.size - 1
fpsSlider.progress = Prefs.FPS_LIMITS.indexOf(prefs.fpsLimit).coerceAtLeast(0)
fpsLabel.text = getString(R.string.fps_limit, Prefs.FPS_LIMITS[fpsSlider.progress])
fpsSlider.setOnSeekBarChangeListener(object : Simple() {
    override fun onProgressChanged(bar: SeekBar, value: Int, fromUser: Boolean) {
        val fps = Prefs.FPS_LIMITS[value]
        fpsLabel.text = getString(R.string.fps_limit, fps)
        if (fromUser) prefs.fpsLimit = fps
    }
})
        val shadowChoices = resources.getStringArray(R.array.sun_shadows_choices)
        val shadowLabel: TextView = findViewById(R.id.sun_shadows_label)
        val shadows: SeekBar = findViewById(R.id.sun_shadows)
        shadows.max = shadowChoices.size - 1
        shadows.progress = prefs.sunShadows
        shadowLabel.text = getString(R.string.sun_shadows, shadowChoices[shadows.progress])
        shadows.setOnSeekBarChangeListener(object : Simple() {
            override fun onProgressChanged(bar: SeekBar, value: Int, fromUser: Boolean) {
                shadowLabel.text = getString(R.string.sun_shadows, shadowChoices[value])
                if (fromUser) prefs.sunShadows = value
            }
        })
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
        switch(R.id.invert, prefs.invertLook) { prefs.invertLook = it }

        slider(
            R.id.look_x, R.id.look_x_label, prefs.lookSensitivityX,
            { getString(R.string.look_x, it) }, { prefs.lookSensitivityX = it },
        )
        slider(
            R.id.look_y, R.id.look_y_label, prefs.lookSensitivityY,
            { getString(R.string.look_y, it) }, { prefs.lookSensitivityY = it },
        )

        val smoothingLabel: TextView = findViewById(R.id.smoothing_label)
        val smoothing: SeekBar = findViewById(R.id.smoothing)
        smoothing.max = 90
        smoothing.progress = (prefs.lookSmoothing * 100f).roundToInt().coerceIn(0, 90)
        smoothingLabel.text = getString(R.string.smoothing, smoothing.progress)
        smoothing.setOnSeekBarChangeListener(object : Simple() {
            override fun onProgressChanged(bar: SeekBar, value: Int, fromUser: Boolean) {
                smoothingLabel.text = getString(R.string.smoothing, value)
                if (fromUser) prefs.lookSmoothing = value / 100f
            }
        })

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
                                ) "executables are never taken from a mod"
                                else "needs the CoD Xe plugin, which this port does not run"
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

    /** A 0.2 to 4.0 slider with a label that follows it. */
    private fun slider(
        sliderId: Int,
        labelId: Int,
        initial: Float,
        label: (Float) -> String,
        onChange: (Float) -> Unit,
    ) {
        val text: TextView = findViewById(labelId)
        val bar: SeekBar = findViewById(sliderId)
        bar.max = 38                                     // 0.2 .. 4.0 in tenths
        bar.progress = ((initial - 0.2f) * 10f).roundToInt().coerceIn(0, 38)
        text.text = label(initial)
        bar.setOnSeekBarChangeListener(object : Simple() {
            override fun onProgressChanged(view: SeekBar, value: Int, fromUser: Boolean) {
                val amount = 0.2f + value / 10f
                text.text = label(amount)
                if (fromUser) onChange(amount)
            }
        })
    }

    /** Where the game is read from: the folder, and the disc image when it is played from one. */
    private fun showPaths() {
        val paths: TextView = findViewById(R.id.paths)
        val disc = prefs.discImage
        paths.text = when {
            prefs.gameDirectory.isEmpty() -> getString(R.string.paths_none)
            disc.isEmpty() -> getString(R.string.paths_line, prefs.gameDirectory)
            else -> getString(R.string.paths_line_disc, DiscImage.label(this, disc), prefs.gameDirectory)
        }
    }

    /** Back from the install screen, which may have changed it. */
    override fun onResume() {
        super.onResume()
        showPaths()
    }

    /** The two methods nobody ever needs, out of the way. */
    private abstract class Simple : SeekBar.OnSeekBarChangeListener {
        override fun onStartTrackingTouch(bar: SeekBar) = Unit
        override fun onStopTrackingTouch(bar: SeekBar) = Unit
    }
}
