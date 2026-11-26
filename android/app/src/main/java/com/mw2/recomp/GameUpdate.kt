package com.mw2.recomp

import android.app.job.JobInfo
import android.app.job.JobParameters
import android.app.job.JobScheduler
import android.app.job.JobService
import android.content.BroadcastReceiver
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager
import kotlin.concurrent.thread

/**
 * The game code kept in step with the app, with nothing for the player to
 * do (docs/ondevice-build.md). An update of the app that expects other code
 * than the device has (GameBuild.outdated: told from the libraries'
 * statements, without starting the game) starts the build by itself, in the
 * service that keeps any build going (GameBuildService), from the
 * executables the app kept or the disc image chosen before:
 *
 *  - the moment the update is installed (AppUpdatedReceiver), so that the
 *    code is there before the game is next opened;
 *  - when the launcher is opened, for whatever kept it from starting then;
 *  - when the charger is connected (ChargerJob), where the battery was what
 *    kept it.
 *
 * The player decides how far it may use the battery (Prefs.updateCodeBattery),
 * and may turn it off, which leaves the launcher's "Build the game code".
 */
object GameUpdate {
    /** The battery's charge in a hundred, and whether a charger is connected. */
    private fun battery(context: Context): Pair<Int, Boolean> {
        val battery = context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        val level = battery?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
        val scale = battery?.getIntExtra(BatteryManager.EXTRA_SCALE, 100) ?: 100
        val plugged = (battery?.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) ?: 0) != 0
        // A device that tells nothing of its battery is taken to have none.
        return Pair(if (level < 0 || scale <= 0) 100 else level * 100 / scale, plugged)
    }

    /** The battery's charge now, for the launcher to say what the update waits for. */
    fun batteryLevel(context: Context): Int = battery(context).first

    /** Whether a build the player did not start may run now: on the charger, or with the charge they asked it to leave. */
    fun batteryAllows(context: Context): Boolean {
        val (level, plugged) = battery(context)
        return plugged || level >= Prefs(context).updateCodeBattery
    }

    /** Whether the update is the app's to start: the player has not turned it off, and it has not stopped for them. */
    fun enabled(context: Context): Boolean =
        Prefs(context).updateCodeBattery != Prefs.UPDATE_CODE_NEVER && !GameBuildService.State(context).stopped

    /**
     * Starts the update if there is one to do and it may run, or carries on
     * with a build the system ended. From anywhere; [done] is called when it
     * has decided, on another thread: telling what the libraries state reads
     * files.
     */
    fun consider(context: Context, done: () -> Unit = {}) {
        val app = context.applicationContext
        thread(name = "game-update") {
            try {
                decide(app)
            } finally {
                done()
            }
        }
    }

    private fun decide(context: Context) {
        if (!GameBuild.offered(context)) return
        val state = GameBuildService.State(context)
        if (state.wanted) {
            GameBuildService.resumeIfInterrupted(context)
            return
        }
        if (!enabled(context) || !GameBuild.outdated(context)) return
        if (batteryAllows(context)) GameBuildService.start(context, automatic = true) else waitForCharger(context)
    }

    /** Has [consider] called again when the charger is connected. */
    fun waitForCharger(context: Context) {
        val job = JobInfo.Builder(CHARGER_JOB, ComponentName(context, ChargerJob::class.java)).setRequiresCharging(true).build()
        (context.getSystemService(Context.JOB_SCHEDULER_SERVICE) as JobScheduler).schedule(job)
    }

    private const val CHARGER_JOB = 0x4D5A
}

/** The app was replaced by another version of itself: the update is the app's to try again, whatever stopped the last. */
class AppUpdatedReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_MY_PACKAGE_REPLACED) return
        GameBuildService.State(context).stopped = false
        val pending = goAsync()
        GameUpdate.consider(context) { pending.finish() }
    }
}

/** The charger was connected while an update waited for it. */
class ChargerJob : JobService() {
    override fun onStartJob(parameters: JobParameters): Boolean {
        GameUpdate.consider(this) { jobFinished(parameters, false) }
        return true
    }

    override fun onStopJob(parameters: JobParameters): Boolean = false
}
