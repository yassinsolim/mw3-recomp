package com.mw2.recomp

import android.content.Context
import android.content.SharedPreferences

/**
 * Everything the player can change, in one place, with the defaults a weak
 * phone wants. Most of these end up as one of the runtime's MW2_* switches
 * (docs/switches.md) the moment a run starts.
 */
class Prefs(context: Context) {

    private val app: Context = context.applicationContext

    private val store: SharedPreferences =
        app.getSharedPreferences("mw2", Context.MODE_PRIVATE)

    /**
     * The fastest cap this screen can show: the highest of FPS_LIMITS at or
     * under the display's fastest mode, so a 120 Hz panel (AYN Thor) starts
     * at 120 and a 60 Hz one (Retroid Pocket Flip 2) at 60. Both games hold
     * near those since the texel forms (docs/ANDROID-PROGRESS.md), so the
     * old 30 for Modern Warfare 3 on an Adreno 6xx is gone: a fresh install
     * there was left at half the rate it can now draw.
     */
    private val displayLimit: Int by lazy {
        val display = app.getSystemService(android.hardware.display.DisplayManager::class.java)
            ?.getDisplay(android.view.Display.DEFAULT_DISPLAY)
        val fastest = display?.supportedModes?.maxOfOrNull { it.refreshRate } ?: 60f
        // 3% under, as the runtime's pacing matches a mode: 118.4 Hz is 120.
        FPS_LIMITS.filter { it <= fastest * 1.03f }.maxOrNull() ?: 60
    }

    // ---- graphics ---------------------------------------------------------

    /**
     * The fraction of the screen the game actually renders at. The surface is
     * told to be that many pixels (SurfaceHolder.setFixedSize) and the
     * compositor scales it up, which costs nothing: the phone's display
     * pipeline does that anyway. At 0.7 a 1080p phone renders 756p, close to
     * the console's own, and it is the single biggest thing a weak device can
     * be given.
     */
    var resolutionScale: Float
        get() = store.getFloat(KEY_RESOLUTION, 0f).let { if (it <= 0f) 0.7f else it }
        set(value) = store.edit().putFloat(KEY_RESOLUTION, value.coerceIn(0.35f, 1.0f)).apply()

    /**
     * Internal Vulkan render scale multiplier (MW2_SCALE):
     * 1 = 1x native base dimensions, 2 = 2x multiplier, 3 = 3x multiplier.
     */
    var renderScale: Int
        get() = store.getInt(KEY_RENDER_SCALE, 1).coerceIn(1, 3)
        set(value) = store.edit().putInt(KEY_RENDER_SCALE, value.coerceIn(1, 3)).apply()

    /** Multisampling. Off by default: it is pure cost on a tiled GPU. */
    var multisampling: Boolean
        get() = store.getBoolean(KEY_MSAA, false)
        set(value) = store.edit().putBoolean(KEY_MSAA, value).apply()

    /**
     * Modern Warfare 2, detail at distance: how many times nearer than on
     * the console a model changes to its coarser meshes, one of
     * DETAIL_BIASES (MW2_LOD_BIAS, runtime/model_detail.cpp). The console's
     * until the player chooses.
     */
    var detailBias: Int
        get() = store.getInt(KEY_DETAIL_BIAS, 1).let { if (it in DETAIL_BIASES) it else 1 }
        set(value) = store.edit().putInt(KEY_DETAIL_BIAS, if (value in DETAIL_BIASES) value else 1).apply()

    /**
     * How much of the phone's memory the texture cache may hold. Zero, the
     * default, means "let the runtime decide from the device"
     * (runtime/android/perf.cpp): a fixed 128 MB is less than one MW3
     * Survival map binds, and what does not fit is drawn black.
     */
    var textureBudgetMB: Int
        get() = store.getInt(KEY_TEXTURE_MB, 0)
        set(value) = store.edit().putInt(KEY_TEXTURE_MB, value).apply()

    var showStats: Boolean
        get() = store.getBoolean(KEY_SHOW_STATS, true)
        set(value) = store.edit().putBoolean(KEY_SHOW_STATS, value).apply()

    /**
     * Stretch the picture to fill the whole screen (no black bars).
     * The surface is made 16:9 and the display scales it up.
     */
    var fillScreen: Boolean
        get() = store.getBoolean(KEY_FILL_SCREEN, false)
        set(value) = store.edit().putBoolean(KEY_FILL_SCREEN, value).apply()

    /**
     * Once per install: a saved cap goes, so the screen's own rate takes
     * over. Before the cap followed the screen, the settings screen and the
     * old 60 FPS lock saved the old defaults (30 for Modern Warfare 3 on an
     * Adreno 6xx, 60 elsewhere) as readily as a player's choice, and those
     * kept a 120 Hz Thor at 60 and a Flip 2 at 30. A saved cap cannot be told
     * from a chosen one, so every one is dropped once; a player who wants a
     * lower cap sets it again, and it is kept from then on.
     */
    private fun forgetOldDefault() {
        if (store.getBoolean(KEY_CAP_FOLLOWS_SCREEN, false)) return
        store.edit().putBoolean(KEY_CAP_FOLLOWS_SCREEN, true).remove(KEY_FPS_LIMIT).remove(KEY_60HZ).apply()
        android.util.Log.i("mw2", "prefs: the saved FPS cap was dropped once; it now starts at the screen's $displayLimit")
    }

    /**
     * Frame-rate cap, one of FPS_LIMITS (MW2_FPS_LIMIT). Until the player
     * chooses, the screen's own fastest rate (displayLimit).
     */
    var fpsLimit: Int
        get() {
            forgetOldDefault()
            val saved = store.getInt(KEY_FPS_LIMIT, 0)
            return if (saved in FPS_LIMITS) saved else displayLimit
        }
        set(value) {
            val v = if (value in FPS_LIMITS) value else 60
            store.edit().putInt(KEY_FPS_LIMIT, v).putBoolean(KEY_CAP_FOLLOWS_SCREEN, true).apply()
        }

    /**
     * The sun's shadows (MW2_SHADOWS): 0 leaves the choice to the runtime,
     * which knows the chip and the title (runtime/gpu/d3d_hooks.cpp), 1 is
     * on and 2 off. The runtime's choice takes them away on an AYN Thor at
     * its 120, and the player must be able to have them back.
     */
    var sunShadows: Int
        get() = store.getInt(KEY_SUN_SHADOWS, 0).coerceIn(0, 2)
        set(value) = store.edit().putInt(KEY_SUN_SHADOWS, value.coerceIn(0, 2)).apply()
  
    /**
     * Field of View (cg_fov). Modern phones are 20:9 ultrawide,
     * so 85 FOV gives a natural, wide cinematic view without gun distortion,
     * compared to the 65 FOV console original.
     */
    var fov: Int
        get() = store.getInt(KEY_FOV, 85).coerceIn(60, 110)
        set(value) = store.edit().putInt(KEY_FOV, value.coerceIn(60, 110)).apply()

    // ---- sound ------------------------------------------------------------

    var audioEnabled: Boolean
        get() = store.getBoolean(KEY_AUDIO, true)
        set(value) = store.edit().putBoolean(KEY_AUDIO, value).apply()

    // ---- controls ---------------------------------------------------------

    /** The on-screen pad. Off is for playing with a physical controller. */
    var touchControls: Boolean
        get() = store.getBoolean(KEY_TOUCH, true)
        set(value) = store.edit().putBoolean(KEY_TOUCH, value).apply()

    /**
     * Whether a physical controller is listened to at all. Off is not a
     * nicety: a phone that reports an accessory, or its own sensors, as a
     * gamepad pushes a stick nobody is touching, and the character walks into
     * a wall for the whole match with nothing on screen to explain it.
     */
    var gamepadEnabled: Boolean
        get() = store.getBoolean(KEY_GAMEPAD, true)
        set(value) = store.edit().putBoolean(KEY_GAMEPAD, value).apply()

    var vibration: Boolean
        get() = store.getBoolean(KEY_VIBRATION, true)
        set(value) = store.edit().putBoolean(KEY_VIBRATION, value).apply()

    var lookSensitivityX: Float
        get() = store.getFloat(KEY_LOOK_X, 1.0f)
        set(value) = store.edit().putFloat(KEY_LOOK_X, value.coerceIn(0.2f, 4.0f)).apply()

    var lookSensitivityY: Float
        get() = store.getFloat(KEY_LOOK_Y, 1.0f)
        set(value) = store.edit().putFloat(KEY_LOOK_Y, value.coerceIn(0.2f, 4.0f)).apply()

    /** 0 is raw, 1 is very smooth. A little hides the touchscreen's jitter. */
    var lookSmoothing: Float
        get() = store.getFloat(KEY_LOOK_SMOOTH, 0.35f)
        set(value) = store.edit().putFloat(KEY_LOOK_SMOOTH, value.coerceIn(0f, 0.9f)).apply()

    var invertLook: Boolean
        get() = store.getBoolean(KEY_LOOK_INVERT, false)
        set(value) = store.edit().putBoolean(KEY_LOOK_INVERT, value).apply()

    /** How far a fast flick pushes the stick: 1.0 reaches the edge readily. */
    var lookSaturation: Float
        get() = store.getFloat(KEY_LOOK_SATURATION, 1.0f)
        set(value) = store.edit().putFloat(KEY_LOOK_SATURATION, value.coerceIn(0.3f, 3.0f)).apply()

    // ---- the graphics driver ------------------------------------------------

    /** The folder of the driver the player picked, or "" for the system's. */
    var driverDirectory: String
        get() = store.getString(KEY_DRIVER_DIR, "") ?: ""
        set(value) = store.edit().putString(KEY_DRIVER_DIR, value).apply()

    var driverLibrary: String
        get() = store.getString(KEY_DRIVER_LIB, "") ?: ""
        set(value) = store.edit().putString(KEY_DRIVER_LIB, value).apply()

    /**
     * Set once the player has picked a driver themselves, the system's
     * included. Until then the build's bundled driver (DriverStore) may be
     * chosen for them; afterwards nothing but the player changes it.
     */
    var driverChoiceMade: Boolean
        get() = store.getBoolean(KEY_DRIVER_CHOICE_MADE, false)
        set(value) = store.edit().putBoolean(KEY_DRIVER_CHOICE_MADE, value).apply()

    /**
     * SHA-256 of the bundled driver package that was last unpacked, so a
     * newer one in an app update is noticed. Empty until the default has
     * been installed, which is also how an install that predates it, with a
     * driver the player picked, is told apart from one holding our default.
     */
    var bundledDriverSha: String
        get() = store.getString(KEY_BUNDLED_DRIVER_SHA, "") ?: ""
        set(value) = store.edit().putString(KEY_BUNDLED_DRIVER_SHA, value).apply()

    /** Where the bundled driver was unpacked, whether or not it is selected. */
    var bundledDriverDirectory: String
        get() = store.getString(KEY_BUNDLED_DRIVER_DIR, "") ?: ""
        set(value) = store.edit().putString(KEY_BUNDLED_DRIVER_DIR, value).apply()

    // ---- where the game lives ------------------------------------------------

    /**
     * Set once the files are in place; empty means "not installed yet".
     * Written before the setter returns: the installer starts the game right
     * after, in a process of its own that reads the file, and one written
     * later was read without the folder ("The game files are not installed",
     * seen on the Retroid Pocket Flip 2 after Install & Play).
     */
    var gameDirectory: String
        get() = store.getString(KEY_GAME_DIR, "") ?: ""
        set(value) {
            store.edit().putString(KEY_GAME_DIR, value).commit()
        }

    /**
     * The disc image the game is played from (DiscImage.kt): its path, or the
     * document's address for one chosen with the system's picker. Empty when
     * the game's files were copied into the game folder. Written before the
     * setter returns, as the game folder is, and before it.
     */
    var discImage: String
        get() = store.getString(KEY_DISC_IMAGE, "") ?: ""
        set(value) {
            store.edit().putString(KEY_DISC_IMAGE, value).commit()
        }

    /**
     * The disc image the player chose on the build screen, as [discImage]
     * names one: Install & Play offers it, so an image is chosen once.
     */
    var chosenImage: String
        get() = store.getString(KEY_CHOSEN_IMAGE, "") ?: ""
        set(value) = store.edit().putString(KEY_CHOSEN_IMAGE, value).apply()

    /** The folder the file browser last showed, so it opens where it left off. */
    var lastBrowsed: String
        get() = store.getString(KEY_LAST_BROWSED, "") ?: ""
        set(value) = store.edit().putString(KEY_LAST_BROWSED, value).apply()

    /** The update this phone was told about and skipped, by tag. */
    var seenUpdateTag: String
        get() = store.getString(KEY_UPDATE_TAG, "") ?: ""
        set(value) = store.edit().putString(KEY_UPDATE_TAG, value).apply()

    /** Shown once, the first time the player opens the app. */
    var seenWelcome: Boolean
        get() = store.getBoolean(KEY_WELCOME, false)
        set(value) = store.edit().putBoolean(KEY_WELCOME, value).apply()

    var autoLaunch: Boolean
        get() = store.getBoolean(KEY_AUTO_LAUNCH, true)
        set(value) = store.edit().putBoolean(KEY_AUTO_LAUNCH, value).apply()

    /**
     * The split-screen heat warning, shown once when starting the game with a
     * physical controller attached (player 2). Extra viewports cost frames
     * and the phone gets hot; nobody needs telling twice.
     */
    var seenPadWarning: Boolean
        get() = store.getBoolean(KEY_PAD_WARNING, false)
        set(value) = store.edit().putBoolean(KEY_PAD_WARNING, value).apply()

    /**
     * Compile every recorded pipeline synchronously at boot instead of the
     * small instant-boot budget (MW2_SHADER_PREWARM_ALL). Boot takes minutes
     * on a cold cache but there is no warmup debt left afterwards.
     */
    var prewarmAllShaders: Boolean
        get() = store.getBoolean(KEY_PREWARM_ALL, false)
        set(value) = store.edit().putBoolean(KEY_PREWARM_ALL, value).apply()

    /**
     * Compile a pipeline the game has not drawn with before on a background
     * thread, skipping its draws until it is ready (MW2_ASYNC_PIPELINES).
     * On by default: on the AYN Thor a first play of Survival lost 146 frames
     * to compiling at the draw, 25 with this, and the map was whole within
     * the intro and the first seconds of play. Off compiles at the draw.
     */
    var asyncPipelines: Boolean
        get() = store.getBoolean(KEY_ASYNC_PIPELINES, true)
        set(value) = store.edit().putBoolean(KEY_ASYNC_PIPELINES, value).apply()

    /**
     * Modern Warfare 3's multiplayer: every item reads as unlocked
     * (MW2_UNLOCK_ALL, runtime/mw3_unlock_all.cpp). The stats are not changed.
     */
    var unlockAllItems: Boolean
        get() = store.getBoolean(KEY_UNLOCK_ALL_ITEMS, true)
        set(value) = store.edit().putBoolean(KEY_UNLOCK_ALL_ITEMS, value).apply()

    /**
     * The multiplayer: how many bots join the matches this
     * device hosts (MW2_BOTS, runtime/bots.cpp), 0 for none; how well they
     * shoot, 0 Recruit to 3 Veteran (MW2_BOT_SKILL); and whether they all
     * take the side the players are not on (MW2_BOT_TEAMS).
     */
    var botCount: Int
        get() = store.getInt(KEY_BOT_COUNT, 0).coerceIn(0, MAX_BOTS)
        set(value) = store.edit().putInt(KEY_BOT_COUNT, value.coerceIn(0, MAX_BOTS)).apply()

    var botSkill: Int
        get() = store.getInt(KEY_BOT_SKILL, 1).coerceIn(0, 3)
        set(value) = store.edit().putInt(KEY_BOT_SKILL, value.coerceIn(0, 3)).apply()

    var botsAgainstPlayers: Boolean
        get() = store.getBoolean(KEY_BOTS_AGAINST_PLAYERS, false)
        set(value) = store.edit().putBoolean(KEY_BOTS_AGAINST_PLAYERS, value).apply()

    /**
     * Modern Warfare 3's multiplayer: what the matches this device hosts are
     * played with (MW2_MATCH_*, runtime/match_options.cpp) -- gravity (800 is
     * the game's), movement speed in percent, jump height (39 is the game's),
     * whether sprinting never runs out, and whether a fall hurts.
     */
    var matchGravity: Int
        get() = store.getInt(KEY_MATCH_GRAVITY, 800).let { if (it in MATCH_GRAVITIES) it else 800 }
        set(value) = store.edit().putInt(KEY_MATCH_GRAVITY, value).apply()

    var matchSpeed: Int
        get() = store.getInt(KEY_MATCH_SPEED, 100).let { if (it in MATCH_SPEEDS) it else 100 }
        set(value) = store.edit().putInt(KEY_MATCH_SPEED, value).apply()

    var matchJump: Int
        get() = store.getInt(KEY_MATCH_JUMP, 39).let { if (it in MATCH_JUMPS) it else 39 }
        set(value) = store.edit().putInt(KEY_MATCH_JUMP, value).apply()

    var matchUnlimitedSprint: Boolean
        get() = store.getBoolean(KEY_MATCH_SPRINT, false)
        set(value) = store.edit().putBoolean(KEY_MATCH_SPRINT, value).apply()

    var matchFallDamage: Boolean
        get() = store.getBoolean(KEY_MATCH_FALL_DAMAGE, true)
        set(value) = store.edit().putBoolean(KEY_MATCH_FALL_DAMAGE, value).apply()

    /**
     * Modern Warfare 3's multiplayer: the matches this device hosts end in a
     * vote for the next map, which then loads with everyone in it
     * (MW2_MAP_VOTE, runtime/map_vote.cpp).
     */
    var mapVote: Boolean
        get() = store.getBoolean(KEY_MAP_VOTE, false)
        set(value) = store.edit().putBoolean(KEY_MAP_VOTE, value).apply()

    /**
     * Keep the driver's pipeline blob on disk between runs on Mali
     * (MW2_PERSIST_PIPELINE_CACHE). Off by default: it may be unstable.
     */
    var persistPipelineCache: Boolean
        get() = store.getBoolean(KEY_PERSIST_PIPELINE, false)
        set(value) = store.edit().putBoolean(KEY_PERSIST_PIPELINE, value).apply()

    /**
     * The name other network players see (MW2_NAME). Until the player types
     * one it is the device's own (defaultPlayerName): the profile's name is
     * "Player 1" on every fresh install, and a lobby of them told nobody who
     * was who.
     */
    var playerName: String
        get() = store.getString(KEY_PLAYER_NAME, "").orEmpty().trim().ifEmpty { defaultPlayerName }
        set(value) = store.edit().putString(KEY_PLAYER_NAME, value.trim().take(MAX_PLAYER_NAME)).apply()

    /**
     * The device's model and four characters of the number Android keeps for
     * this app on this device (it outlives an update, and differs between two
     * devices of one model): "AYN Thor 3F2A". The model gives up its leading
     * words until it fits the fifteen characters a name has, so a "Retroid
     * Pocket Flip2" is "Flip2 …". A second copy on one device (-PmpInstance)
     * is another player, with other characters.
     */
    val defaultPlayerName: String by lazy {
        val number = android.provider.Settings.Secure.getString(
            app.contentResolver, android.provider.Settings.Secure.ANDROID_ID,
        ).orEmpty()
        val tail = "%04X".format("$number/${BuildConfig.MP_INSTANCE}".hashCode() and 0xFFFF)
        val room = MAX_PLAYER_NAME - tail.length - 1
        var model = android.os.Build.MODEL.orEmpty().trim()
        while (model.length > room && ' ' in model) model = model.substringAfter(' ').trim()
        "${model.take(room).trim()} $tail".trim()
    }

    /**
     * Whether the player has been asked for that name, which the first start
     * of the multiplayer does (GameActivity.start). One who typed a name in
     * Settings before there was a question has answered it.
     */
    var playerNameAsked: Boolean
        get() = store.getBoolean(KEY_PLAYER_NAME_ASKED, false) ||
            store.getString(KEY_PLAYER_NAME, "").orEmpty().isNotBlank()
        set(value) = store.edit().putBoolean(KEY_PLAYER_NAME_ASKED, value).apply()

    /** Accept LAN lobby invitations automatically (MW2_LAN_ACCEPT). */
    var lanAccept: Boolean
        get() = store.getBoolean(KEY_LAN_ACCEPT, false)
        set(value) = store.edit().putBoolean(KEY_LAN_ACCEPT, value).apply()

    /**
     * Announced network adapter (MW2_LAN_IFACE): name or address piece.
     * Empty means the first connection up. For VPN + WiFi at once.
     */
    var lanIface: String
        get() = store.getString(KEY_LAN_IFACE, "") ?: ""
        set(value) = store.edit().putString(KEY_LAN_IFACE, value).apply()

    /**
     * How the multiplayer reaches other players: NETWORK_ONLINE, through a
     * relay server; NETWORK_LAN, the local network only; NETWORK_OFF, no
     * network at all.
     */
    var networkMode: String
        get() = (store.getString(KEY_NETWORK_MODE, NETWORK_ONLINE) ?: NETWORK_ONLINE)
            .let { if (it in NETWORK_MODES) it else NETWORK_ONLINE }
        set(value) = store.edit().putString(KEY_NETWORK_MODE, value).apply()

    /** The relay server, host or host:port (MW2_SERVER). */
    var relayAddress: String
        get() = store.getString(KEY_RELAY_ADDRESS, DEFAULT_RELAY_ADDRESS) ?: DEFAULT_RELAY_ADDRESS
        set(value) = store.edit().putString(KEY_RELAY_ADDRESS, value.trim()).apply()

    /**
     * Modern Warfare 3's multiplayer, experimental: signed in to Xbox LIVE
     * (MW2_LIVE). Its Xbox LIVE menu also wants the publisher's servers, so
     * this is for testing; players meet in System Link without it.
     */
    var liveSignIn: Boolean
        get() = store.getBoolean(KEY_LIVE_SIGN_IN, false)
        set(value) = store.edit().putBoolean(KEY_LIVE_SIGN_IN, value).apply()

    /**
     * Modern Warfare 3's multiplayer: voice chat (MW2_VOICE). Off until the
     * player turns it on, which is also when the app asks for the
     * microphone: speech goes to the other players through the relay.
     */
    var voiceChat: Boolean
        get() = store.getBoolean(KEY_VOICE_CHAT, false)
        set(value) = store.edit().putBoolean(KEY_VOICE_CHAT, value).apply()

    /**
     * Send only while the Talk button is held (MW2_VOICE_PTT). Off is an
     * open microphone, as the console had: for playing on a controller with
     * the touch controls hidden.
     */
    var voicePushToTalk: Boolean
        get() = store.getBoolean(KEY_VOICE_PTT, true)
        set(value) = store.edit().putBoolean(KEY_VOICE_PTT, value).apply()

    /**
     * What the runtime is given as MW2_NETWORK: the relay when Online is
     * chosen and there is an address for one, else the local network, which
     * needs nothing set up.
     */
    val networkBackend: String
        get() = when (networkMode) {
            NETWORK_OFF -> "none"
            NETWORK_LAN -> "lan"
            else -> if (relayAddress.isBlank()) "lan" else "server"
        }

    companion object {
        /**
         * The public relay every copy starts with, host or host:port (3074
         * without one): the one place it is written. The owner's homelab
         * runs it (docs/relay.md); a player may point Online at another.
         */
        const val DEFAULT_RELAY_ADDRESS = "mw3.yassin.app:3074"
        const val NETWORK_ONLINE = "online"
        const val NETWORK_LAN = "lan"
        const val NETWORK_OFF = "off"
        val NETWORK_MODES = listOf(NETWORK_ONLINE, NETWORK_LAN, NETWORK_OFF)
        /** A gamertag's longest, which is what the title shows of a name. */
        const val MAX_PLAYER_NAME = 15
        private const val KEY_NETWORK_MODE = "networkMode"
        private const val KEY_RELAY_ADDRESS = "relayAddress"
        private const val KEY_LIVE_SIGN_IN = "liveSignIn"
        private const val KEY_FPS_LIMIT = "fpsLimit"
        private const val KEY_SUN_SHADOWS = "sunShadows"
        val FPS_LIMITS = listOf(30, 45, 60, 120)
        private const val KEY_RESOLUTION = "resolutionScale"
        private const val KEY_RENDER_SCALE = "renderScale"
        private const val KEY_MSAA = "multisampling"
        private const val KEY_DETAIL_BIAS = "detailBias"
        /** As on the console, reduced, low: what the slider stops at. */
        val DETAIL_BIASES = intArrayOf(1, 2, 4)
        private const val KEY_TEXTURE_MB = "textureBudgetMB"
        private const val KEY_SHOW_STATS = "showStats"
        private const val KEY_FILL_SCREEN = "fillScreen"
        private const val KEY_60HZ = "prefer60Hz"
        private const val KEY_CAP_FOLLOWS_SCREEN = "fpsCapFollowsScreen"
        private const val KEY_FOV = "fov"
        private const val KEY_AUDIO = "audio"
        private const val KEY_TOUCH = "touchControls"
        private const val KEY_GAMEPAD = "gamepad"
        private const val KEY_VIBRATION = "vibration"
        private const val KEY_LOOK_X = "lookX"
        private const val KEY_LOOK_Y = "lookY"
        private const val KEY_LOOK_SMOOTH = "lookSmoothing"
        private const val KEY_LOOK_INVERT = "lookInvert"
        private const val KEY_LOOK_SATURATION = "lookSaturation"
        private const val KEY_DRIVER_DIR = "driverDirectory"
        private const val KEY_DRIVER_LIB = "driverLibrary"
        private const val KEY_DRIVER_CHOICE_MADE = "driverChoiceMade"
        private const val KEY_BUNDLED_DRIVER_SHA = "bundledDriverSha"
        private const val KEY_BUNDLED_DRIVER_DIR = "bundledDriverDirectory"
        private const val KEY_GAME_DIR = "gameDirectory"
        private const val KEY_DISC_IMAGE = "discImage"
        private const val KEY_CHOSEN_IMAGE = "chosenImage"
        private const val KEY_LAST_BROWSED = "lastBrowsed"
        private const val KEY_WELCOME = "seenWelcome"
        private const val KEY_AUTO_LAUNCH = "autoLaunch"
        private const val KEY_UPDATE_TAG = "seenUpdateTag"
        private const val KEY_PAD_WARNING = "seenPadWarning"
        private const val KEY_PREWARM_ALL = "prewarmAllShaders"
        private const val KEY_ASYNC_PIPELINES = "asyncPipelines"
        private const val KEY_UNLOCK_ALL_ITEMS = "unlockAllItems"
        private const val KEY_BOT_COUNT = "botCount"
        private const val KEY_BOT_SKILL = "botSkill"
        private const val KEY_BOTS_AGAINST_PLAYERS = "botsAgainstPlayers"
        private const val KEY_MAP_VOTE = "mapVote"
        /**
         * A server has 18 slots and the host keeps one. Advanced Warfare's
         * has 12 on this console.
         */
        val MAX_BOTS: Int = if (Title.isAw) 11 else 17
        private const val KEY_MATCH_GRAVITY = "matchGravity"
        private const val KEY_MATCH_SPEED = "matchSpeed"
        private const val KEY_MATCH_JUMP = "matchJump"
        private const val KEY_MATCH_SPRINT = "matchUnlimitedSprint"
        private const val KEY_MATCH_FALL_DAMAGE = "matchFallDamage"
        /** What the match option sliders stop at; the game's own value is in each. */
        val MATCH_GRAVITIES = intArrayOf(100, 200, 400, 600, 800, 1200, 1600)
        val MATCH_SPEEDS = intArrayOf(50, 75, 100, 125, 150, 200, 300)
        val MATCH_JUMPS = intArrayOf(20, 39, 60, 80, 120, 200, 400)
        private const val KEY_PERSIST_PIPELINE = "persistPipelineCache"
        private const val KEY_PLAYER_NAME = "playerName"
        private const val KEY_PLAYER_NAME_ASKED = "playerNameAsked"
        private const val KEY_LAN_ACCEPT = "lanAccept"
        private const val KEY_LAN_IFACE = "lanIface"
        private const val KEY_VOICE_CHAT = "voiceChat"
        private const val KEY_VOICE_PTT = "voicePushToTalk"
    }
}
