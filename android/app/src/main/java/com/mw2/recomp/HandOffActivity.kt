package com.mw2.recomp

import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.os.SystemClock
import androidx.appcompat.app.AppCompatActivity
import java.io.File

/**
 * The step between one title and the other.
 *
 * The campaign's menu starts the multiplayer, and the multiplayer's the
 * campaign, by asking the console's loader for the other executable. Here
 * that is another native library, and a process holds one for its whole life,
 * so the run's process (":game") has to end and a new one start. A process
 * cannot start its own successor after it is gone: this activity lives in the
 * launcher's process, is put in front by the run as its last act, waits until
 * the run's process has really ended, and starts the run again. What the new
 * run is to be is already written down (RunRequest).
 */
class HandOffActivity : AppCompatActivity() {

    private val handler = Handler(Looper.getMainLooper())

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val process = intent.getIntExtra(EXTRA_PROCESS, 0)
        val patience = SystemClock.uptimeMillis() + PATIENCE_MS
        var killed = false
        handler.post(object : Runnable {
            override fun run() {
                // Another process of this app, so its /proc entry is visible
                // here for exactly as long as it lives.
                if (process > 0 && File("/proc/$process").exists()) {
                    // It ends itself within moments of asking (the runtime's
                    // _exit). One that has not is stuck in its shutdown, and
                    // nothing of it is worth waiting for.
                    if (!killed && SystemClock.uptimeMillis() > patience) {
                        Process.killProcess(process)
                        killed = true
                    }
                    handler.postDelayed(this, POLL_MS)
                    return
                }
                startActivity(Intent(this@HandOffActivity, GameActivity::class.java))
                finish()
            }
        })
    }

    override fun onDestroy() {
        handler.removeCallbacksAndMessages(null)
        super.onDestroy()
    }

    companion object {
        private const val EXTRA_PROCESS = "process"
        private const val PATIENCE_MS = 3000L
        private const val POLL_MS = 50L

        /**
         * Called in the run's process, which is about to end: records what
         * the next run is and puts this activity in front to start it.
         */
        fun start(context: Context, request: RunRequest) {
            request.write(context)
            context.startActivity(
                Intent(context, HandOffActivity::class.java)
                    .putExtra(EXTRA_PROCESS, Process.myPid())
                    .addFlags(Intent.FLAG_ACTIVITY_NO_ANIMATION),
            )
        }
    }
}
