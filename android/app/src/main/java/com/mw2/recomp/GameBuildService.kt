package com.mw2.recomp

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import kotlin.concurrent.thread

/**
 * Keeps the build of the game code (GameBuild.kt) going while the player is
 * elsewhere. It is a quarter of an hour or more of every core, and nobody
 * watches a progress bar that long: without a foreground service the app's
 * process is frozen the moment the player leaves it, and ended soon after.
 *
 * What the player asked for is kept in preferences, not in this object, so
 * that a build whose process the system ended anyway carries on by itself:
 * the service is sticky while a build is wanted, and the launcher starts it
 * again when it finds one wanted and none running. The build itself looks at
 * the files and does what is left (GameBuild.run).
 *
 * In the launcher's process, not the run's: it has nothing of the runtime's
 * and must not be ended with a game.
 */
class GameBuildService : Service() {

    private val handler = Handler(Looper.getMainLooper())
    private var worker: Thread? = null
    private var wakeLock: PowerManager.WakeLock? = null

    private val tick = object : Runnable {
        override fun run() {
            notifications().notify(NOTIFICATION_ID, working())
            handler.postDelayed(this, 2000)
        }
    }

    /** Nothing binds to it; the screen reads GameBuild.progress. */
    override fun onBind(intent: Intent?): IBinder? = null

    private fun notifications() = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager

    private fun open(): PendingIntent = PendingIntent.getActivity(
        this, 0, Intent(this, BuildActivity::class.java), PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
    )

    private fun working(): Notification {
        val progress = GameBuild.progress
        return Notification.Builder(this, CHANNEL)
            .setSmallIcon(android.R.drawable.stat_sys_download)
            .setContentTitle(getString(R.string.build_title))
            .setContentText(describe(this, progress))
            .setProgress(progress.total, progress.done, progress.total == 0)
            .setContentIntent(open())
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setShowWhen(false)
            .build()
    }

    override fun onCreate() {
        super.onCreate()
        // IMPORTANCE_LOW: a line in the shade with a bar, no sound.
        notifications().createNotificationChannel(
            NotificationChannel(CHANNEL, getString(R.string.build_channel), NotificationManager.IMPORTANCE_LOW),
        )
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
                startForeground(NOTIFICATION_ID, working(), ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE)
            } else {
                startForeground(NOTIFICATION_ID, working())
            }
        } catch (_: RuntimeException) {
            // Started again by the system with the app in the background,
            // where newer Androids refuse a foreground service: the build
            // waits for the launcher to be opened (MainActivity.onResume).
            stopSelf()
            return START_NOT_STICKY
        }
        val state = State(this)
        if (!state.wanted || state.paused) {
            // Nothing to do: a pause that arrived with no build running.
            if (worker == null) stopSelf()
            return START_NOT_STICKY
        }
        if (worker == null) {
            // The processor must not sleep with the screen off; the lock goes
            // with the build, and by itself after longer than any build takes.
            wakeLock = (getSystemService(Context.POWER_SERVICE) as PowerManager)
                .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "mw2:game-build").apply { acquire(6 * 60 * 60 * 1000L) }
            handler.post(tick)
            worker = thread(name = "game-build") {
                val outcome = GameBuild.run(applicationContext)
                handler.post { finished(outcome) }
            }
        }
        return START_STICKY
    }

    private fun finished(outcome: GameBuild.Outcome) {
        val state = State(this)
        worker = null
        handler.removeCallbacks(tick)
        wakeLock?.let { if (it.isHeld) it.release() }
        wakeLock = null
        val said = when (outcome) {
            is GameBuild.Outcome.Done -> {
                state.wanted = false
                getString(R.string.build_done)
            }
            is GameBuild.Outcome.Failed -> {
                state.wanted = false
                state.error = outcome.message
                getString(R.string.build_failed)
            }
            // Paused by the player; unless they have asked for it again
            // since, in which case the build starts over from what it has.
            is GameBuild.Outcome.Paused -> if (state.paused) null else {
                start(this)
                return
            }
        }
        stopForeground(STOP_FOREGROUND_REMOVE)
        if (said != null) {
            // News, unlike the progress: it stays until the player has seen it.
            notifications().notify(
                RESULT_ID,
                Notification.Builder(this, CHANNEL)
                    .setSmallIcon(android.R.drawable.stat_sys_download_done)
                    .setContentTitle(getString(R.string.build_title))
                    .setContentText(said)
                    .setContentIntent(open())
                    .setAutoCancel(true)
                    .build(),
            )
        }
        stopSelf()
    }

    override fun onDestroy() {
        handler.removeCallbacks(tick)
        // Ended with a build running (the app was removed from the recents
        // with the service refused): the compilers must not outlive it.
        if (worker != null) GameBuild.stop()
        wakeLock?.let { if (it.isHeld) it.release() }
        super.onDestroy()
    }

    /** What the player asked for, which outlives the process. */
    class State(context: Context) {
        private val store = context.getSharedPreferences("game-build", Context.MODE_PRIVATE)

        /** A build was started and has neither finished nor failed. */
        var wanted: Boolean
            get() = store.getBoolean("wanted", false)
            set(value) = store.edit().putBoolean("wanted", value).apply()

        var paused: Boolean
            get() = store.getBoolean("paused", false)
            set(value) = store.edit().putBoolean("paused", value).apply()

        /** Why the last build stopped, "" when it did not fail or the player has read it. */
        var error: String
            get() = store.getString("error", "").orEmpty()
            set(value) = store.edit().putString("error", value).apply()
    }

    companion object {
        private const val CHANNEL = "mw2-build"
        private const val NOTIFICATION_ID = 0x4D58
        private const val RESULT_ID = 0x4D59

        /** Starts the build, or carries on with a paused one. From an activity in front. */
        fun start(context: Context) {
            val state = State(context)
            state.wanted = true
            state.paused = false
            state.error = ""
            GameBuild.allow()
            (context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager).cancel(RESULT_ID)
            context.startForegroundService(Intent(context, GameBuildService::class.java))
        }

        /** Stops the compilers; what is finished stays, and [start] carries on from it. */
        fun pause(context: Context) {
            State(context).paused = true
            GameBuild.stop()
        }

        /**
         * A build the player started that no process is doing any more: the
         * system ended it, or the device was restarted. Called by the
         * activities when they come to the front, which is when a foreground
         * service may be started.
         */
        fun resumeIfInterrupted(context: Context) {
            val state = State(context)
            if (state.wanted && !state.paused && !GameBuild.progress.running) start(context)
        }

        /** [progress] in a line, for the notification and the screen. */
        fun describe(context: Context, progress: GameBuild.Progress): String {
            val title = GameBuild.kit(context)?.titles?.firstOrNull { it.library == progress.library }?.title
            val name = context.getString(if (title == Title.MULTIPLAYER) R.string.build_name_multiplayer else R.string.build_name_campaign)
            return when (progress.step) {
                GameBuild.Step.IDLE -> context.getString(R.string.build_starting)
                GameBuild.Step.PREPARE ->
                    if (progress.total == 0) context.getString(R.string.build_starting)
                    else context.getString(R.string.build_step_prepare, progress.done, progress.total)
                GameBuild.Step.RECOMPILE -> context.getString(R.string.build_step_recompile, name)
                GameBuild.Step.LINK -> context.getString(R.string.build_step_link, name)
                GameBuild.Step.COMPILE -> {
                    val left = when {
                        progress.secondsLeft < 0 -> ""
                        progress.secondsLeft < 90 -> context.getString(R.string.build_left_soon)
                        else -> context.getString(R.string.build_left_minutes, (progress.secondsLeft + 30) / 60)
                    }
                    context.getString(R.string.build_step_compile, progress.done, progress.total, name) + left
                }
            }
        }
    }
}
