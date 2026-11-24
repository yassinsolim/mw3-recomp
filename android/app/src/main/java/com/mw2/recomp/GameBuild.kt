package com.mw2.recomp

import android.app.ActivityManager
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager
import android.os.Build
import android.os.PowerManager
import android.os.SystemClock
import java.io.File
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.channels.FileChannel
import java.security.MessageDigest
import java.util.Collections
import org.json.JSONObject

/**
 * Building the game code on this device (docs/ondevice-build.md): what
 * setup\setup.ps1 does on a PC, for a player who has none. The disc's two
 * executables are recompiled to C++ and compiled into lib<title>game.so,
 * which the runtime loads from the app's own storage (GameLibrary.built).
 *
 * The programs come from the toolchain package (Toolchain.kt); everything
 * that decides whether this app's runtime will accept the result comes with
 * the app, as the assets under buildkit/ (android/app/build.gradle.kts): the
 * recompiler's configuration, the header compiled into every file, the
 * compiler's options, and the statement the runtime expects of each library
 * (cmake/game_abi.cmake). The statement of what was actually made here is
 * compared with that one before anything is compiled, so a toolchain package
 * of another version costs a minute and not the whole build.
 *
 * The work is in the app's private storage, under [root], laid out as the
 * repository is so that the configuration files are used as they are:
 *
 *     config/<title>.toml, config/<title>_switch_tables.toml
 *     <game>/tu0/default.xex, default_mp.xex     the player's executables
 *     XenonRecomp/XenonUtils/ppc_context.h       the recompiler's header
 *     ppc_<title>/                               the recompiled tree
 *     obj/<library>/                             its objects
 *
 * Every step leaves what it made under its final name only when it is whole,
 * so a build that is paused, or whose process the system ends, carries on
 * from the file it was at: [run] looks at what is there and does the rest.
 */
object GameBuild {
    /** What a title of the kit is: a library and the executable it is made from. */
    class KitTitle(json: JSONObject) {
        val library: String = json.getString("library")
        val executable: String = json.getString("executable")
        val sha256: List<String> = json.getJSONArray("sha256").let { list -> (0 until list.length()).map { list.getString(it) } }
        val toml: String = json.getString("toml")
        val tables: String = json.getString("tables")
        val tree: String = json.getString("tree")
        val defines: List<String> = json.getJSONArray("defines").let { list -> (0 until list.length()).map { list.getString(it) } }
        val recompiledWith: String = json.getString("recompiledWith")
        val expectedAbi: String = json.getString("expectedAbi")
        val title: Title? = Title.carried.firstOrNull { it.library == library }
    }

    class Kit(json: JSONObject) {
        val folder: String = json.getString("folder")
        val recompilerCommit: String = json.getJSONObject("recompiler").getString("commit")
        val recompilerPatch: String = json.getJSONObject("recompiler").getString("patchSha256")

        /** The toolchain package this app is built for, and where it is published (setup/toolchain.json). */
        val toolchainVersion: String = json.getJSONObject("toolchain").getString("version")
        val toolchainHash: String = json.getJSONObject("toolchain").getString("sha256")
        val toolchainAsset: String = json.getJSONObject("toolchain").getString("asset")
        private val toolchainRelease: String =
            "https://github.com/${json.getJSONObject("toolchain").getString("repo")}/releases"
        private val toolchainTag: String = json.getJSONObject("toolchain").getString("tag")
        val toolchainPage: String = "$toolchainRelease/tag/$toolchainTag"
        val toolchainDownload: String = "$toolchainRelease/download/$toolchainTag/$toolchainAsset"

        /** Whether [toolchain] is the package this app's game code is built with. */
        fun takes(toolchain: Toolchain): Boolean = toolchain.version == toolchainVersion &&
            toolchain.recompilerCommit == recompilerCommit && toolchain.recompilerPatch == recompilerPatch

        val compile: List<String> = strings(json, "compile")
        val link: List<String> = strings(json, "link")
        val extraSources: List<String> = strings(json, "extraSources")
        val inTextTables: Boolean = json.getBoolean("inTextTables")
        val titles: List<KitTitle> = json.getJSONArray("titles").let { list -> (0 until list.length()).map { KitTitle(list.getJSONObject(it)) } }

        private fun strings(json: JSONObject, key: String): List<String> =
            json.getJSONArray(key).let { list -> (0 until list.length()).map { list.getString(it) } }
    }

    enum class Step { IDLE, PREPARE, RECOMPILE, COMPILE, LINK }

    /** What the screen and the notification show; replaced whole, read from any thread. */
    class Progress(
        val running: Boolean = false,
        val step: Step = Step.IDLE,
        /** The library the step is at, "" where it is not one title's. */
        val library: String = "",
        val done: Int = 0,
        val total: Int = 0,
        /** Compilers running now. */
        val jobs: Int = 0,
        /** Seconds the compiling is expected to take yet, -1 while that cannot be said. */
        val secondsLeft: Long = -1,
        /** Why fewer compilers run than the device has cores: "", "memory" or "heat". */
        val held: String = "",
        val batteryCelsius: Float = 0f,
    )

    /** How a [run] ended. */
    sealed class Outcome {
        object Done : Outcome()
        object Paused : Outcome()
        class Failed(val message: String) : Outcome()
    }

    @Volatile
    var progress = Progress()
        private set

    /** While set, [run] returns [Outcome.Paused] as soon as its programs have been stopped. */
    @Volatile
    private var stopRequested = false

    private val children: MutableSet<Process> = Collections.synchronizedSet(HashSet())

    private class Stopped : Exception()
    private class Failure(message: String) : Exception(message)

    private var cachedKit: Kit? = null
    private var kitRead = false

    /** The app's build kit, or null in an app that has none (one with the game code in its own libraries). */
    @Synchronized
    fun kit(context: Context): Kit? {
        if (!kitRead) {
            kitRead = true
            cachedKit = try {
                Kit(JSONObject(context.assets.open("buildkit/kit.json").bufferedReader().use { it.readText() }))
            } catch (_: IOException) {
                null
            }
        }
        return cachedKit
    }

    /** Whether this app can build its game code here at all. */
    fun offered(context: Context): Boolean = GameLibrary.separate && kit(context) != null

    fun root(context: Context): File = File(context.filesDir, "game-build")

    /** What the build writes for a bug report: each step's time, the memory and the heat. */
    fun report(context: Context): File = File(context.getExternalFilesDir(null) ?: context.filesDir, "game-build-report.txt")

    private fun executable(context: Context, kit: Kit, title: KitTitle): File = File(File(root(context), kit.folder), title.executable)

    /** Beside a library built here: the statement it was built against, to tell without loading it whether this app still takes it. */
    private fun statementOf(library: File): File = File(library.path + ".abi")

    /** Whether [title]'s library was built here for this version of the app. */
    private fun current(context: Context, title: KitTitle): Boolean {
        val library = GameLibrary.built(context, title.title ?: return true)
        return library.isFile && try {
            statementOf(library).readText() == title.expectedAbi
        } catch (_: IOException) {
            false
        }
    }

    /** The titles whose game code a build here would make: those not built here yet, or built for another version of the app. */
    fun wanted(context: Context): List<KitTitle> = kit(context)?.titles.orEmpty().filter { it.title != null && !current(context, it) }

    /** Whether a library built here is one this version of the app no longer takes: the app was updated since. */
    fun outdated(context: Context): Boolean =
        wanted(context).any { title -> title.title?.let { GameLibrary.built(context, it).isFile } == true }

    // ---- the executables ------------------------------------------------------

    private fun sha256(file: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        file.inputStream().use { input ->
            val buffer = ByteArray(1 shl 16)
            while (true) {
                val read = input.read(buffer)
                if (read <= 0) break
                digest.update(buffer, 0, read)
            }
        }
        return digest.digest().joinToString("") { "%02x".format(it) }
    }

    /** Whether the player's executables are here already, each a build this app supports. */
    fun haveExecutables(context: Context): Boolean {
        val kit = kit(context) ?: return false
        return kit.titles.all { title -> executable(context, kit, title).let { it.isFile && sha256(it) in title.sha256 } }
    }

    /**
     * Takes one executable out of [source], [size] bytes at [offset], and
     * keeps it if it is a build this app supports. Throws an [IOException]
     * that says what is wrong with it.
     */
    private fun take(context: Context, kit: Kit, title: KitTitle, source: FileChannel, offset: Long, size: Long) {
        val target = executable(context, kit, title)
        target.parentFile?.mkdirs()
        val partial = File(target.path + ".part")
        try {
            partial.outputStream().use { output ->
                val buffer = ByteBuffer.allocate(1 shl 20)
                var done = 0L
                while (done < size) {
                    buffer.clear().limit(minOf(buffer.capacity().toLong(), size - done).toInt())
                    val read = source.read(buffer, offset + done)
                    if (read <= 0) throw IOException("the disc image ends inside ${title.executable}")
                    output.write(buffer.array(), 0, read)
                    done += read
                }
            }
            if (sha256(partial) !in title.sha256) {
                throw IOException(context.getString(R.string.build_unsupported_executable, title.executable))
            }
            if (!partial.renameTo(target)) throw IOException("${title.executable} could not be kept")
        } finally {
            partial.delete()
        }
    }

    /** The executables out of a disc image, which is only read. Throws an [IOException] that says what went wrong. */
    fun takeFromDiscImage(context: Context, image: FileChannel) {
        val kit = kit(context) ?: throw IOException("this app has no build kit")
        val disc = DiscImage(image)
        for (title in kit.titles) {
            val (offset, size) = disc.find(title.executable) ?: throw IOException("${title.executable} is not on that disc image")
            take(context, kit, title, image, offset, size)
        }
    }

    /** The executables from the installed game files. Throws an [IOException] that says what went wrong. */
    fun takeFromGameFolder(context: Context, gameDirectory: String) {
        val kit = kit(context) ?: throw IOException("this app has no build kit")
        for (title in kit.titles) {
            val file = File(gameDirectory, title.executable)
            file.inputStream().channel.use { take(context, kit, title, it, 0, file.length()) }
        }
    }

    // ---- the build -------------------------------------------------------------

    /** Lets [run] work again after a [stop]: the player's asking, never the build's own doing, so that a stop is not lost to a run just starting. */
    fun allow() {
        stopRequested = false
    }

    /** Asks a running [run] to stop where it is; what it has finished stays. */
    fun stop() {
        stopRequested = true
        synchronized(children) { children.forEach { it.destroy() } }
    }

    private fun check() {
        if (stopRequested) throw Stopped()
    }

    /**
     * Runs one program to its end with its output in [log]. Returns its exit
     * status; a program ended because the build was asked to stop throws.
     */
    private fun execute(context: Context, command: List<String>, directory: File, log: File): Int {
        check()
        log.parentFile?.mkdirs()
        val builder = ProcessBuilder(command).directory(directory).redirectErrorStream(true).redirectOutput(log)
        // The compiler's temporary files, where an app may write.
        builder.environment()["TMPDIR"] = context.cacheDir.path
        val process = builder.start()
        children.add(process)
        try {
            val status = process.waitFor()
            check()
            return status
        } finally {
            children.remove(process)
            process.destroy()
        }
    }

    /** The end of a program's output, for the player to pass on. */
    private fun tail(log: File): String = try {
        log.readLines().takeLast(12).joinToString("\n").takeLast(1500)
    } catch (_: IOException) {
        ""
    }

    private fun text(file: File): String = file.readBytes().toString(Charsets.ISO_8859_1).replace("\r", "")
    private fun hash(text: String): String =
        MessageDigest.getInstance("SHA-256").digest(text.toByteArray(Charsets.ISO_8859_1)).joinToString("") { "%02x".format(it) }

    private fun sources(tree: File): List<File> =
        tree.listFiles { file -> file.name.startsWith("ppc_recomp.") && file.name.endsWith(".cpp") }.orEmpty().sortedBy { it.path }

    /**
     * The tree's shared header and the list of the system calls the
     * executable imports, exactly as tools/genshared.py writes them: the
     * script's hash is part of what the library states, and what comes out of
     * here is checked against the app's record of the system calls.
     */
    private fun writeSharedHeader(context: Context, tree: File) {
        // The script's three patterns, looked for by hand: a regular
        // expression over the tree's 60 MB took the device over a minute.
        //   (PPC_FUNC_IMPL|PPC_WEAK_FUNC)\((name)\)     a definition
        //   \b(name)\(ctx, base\)                      a call
        //   \{ 0x[0-9A-F]+, (name) \}                   a row of the function table
        val definitions = HashSet<String>()
        val references = HashSet<String>()
        fun word(c: Char) = c == '_' || c in 'a'..'z' || c in 'A'..'Z' || c in '0'..'9'
        fun name(text: String, from: Int, to: Int): String? =
            if (to > from && !(text[from] in '0'..'9')) text.substring(from, to) else null
        for (file in sources(tree) + File(tree, "ppc_func_mapping.cpp")) {
            check()
            val text = file.readBytes().toString(Charsets.ISO_8859_1)
            for (opening in listOf("PPC_FUNC_IMPL(", "PPC_WEAK_FUNC(")) {
                var at = text.indexOf(opening)
                while (at >= 0) {
                    val from = at + opening.length
                    var to = from
                    while (to < text.length && word(text[to])) to++
                    if (to < text.length && text[to] == ')') name(text, from, to)?.let { definitions.add(it) }
                    at = text.indexOf(opening, to)
                }
            }
            var at = text.indexOf("(ctx, base)")
            while (at >= 0) {
                var from = at
                while (from > 0 && word(text[from - 1])) from--
                // The whole word before the bracket, as the pattern's \b has
                // it: one that starts with a digit is no name at all.
                name(text, from, at)?.let { references.add(it) }
                at = text.indexOf("(ctx, base)", at + 1)
            }
            at = text.indexOf("{ 0x")
            while (at >= 0) {
                var digits = at + 4
                while (digits < text.length && (text[digits] in '0'..'9' || text[digits] in 'A'..'F')) digits++
                if (digits > at + 4 && text.startsWith(", ", digits)) {
                    val from = digits + 2
                    var to = from
                    while (to < text.length && word(text[to])) to++
                    if (text.startsWith(" }", to)) name(text, from, to)?.let { references.add(it) }
                }
                at = text.indexOf("{ 0x", at + 1)
            }
        }
        references.remove("PPC_CALL_INDIRECT_FUNC")
        val header = StringBuilder(context.assets.open("buildkit/shared_header_prefix.h").bufferedReader().use { it.readText() })
        (definitions + references).sorted().forEach { header.append("PPC_FUNC(").append(it).append(");\n") }
        header.append("}\n")
        File(tree, "ppc_recomp_shared.h").writeText(header.toString(), Charsets.ISO_8859_1)
        val imports = (references - definitions).filter { it.startsWith("__imp__") }.sorted()
        File(tree, "kernel_imports.txt").writeText(imports.joinToString("\n") { it.removePrefix("__imp__") } + "\n", Charsets.ISO_8859_1)
    }

    /** What a library made from [tree] states, line for line as cmake/game_abi.cmake composes it. */
    private fun statement(root: File, title: KitTitle, tree: File): String =
        "title ${title.library}\n" + title.recompiledWith +
            "jump-tables ${hash(text(File(root, "config/${title.tables}")))}\n" +
            "code-extent ${hash(text(File(tree, "ppc_config.h")))}\n" +
            "register-context ${hash(text(File(tree, "ppc_context.h")))}\n" +
            "system-calls ${hash(text(File(tree, "kernel_imports.txt")))}\n" +
            "memory-hook ${hash(text(File(root, "runtime/mmio_hook.h")))}\n"

    /** A tree that is whole says so in this file, with the statement it was checked to make. */
    private fun treeMark(tree: File): File = File(tree, "tree.ok")

    private fun recompile(context: Context, kit: Kit, toolchain: Toolchain, title: KitTitle) {
        val root = root(context)
        val tree = File(root, title.tree)
        val logs = File(root, "logs")
        val program = "${kit.folder}/${title.executable}"
        var log = File(logs, "${title.library}_analyse.log")
        if (execute(context, listOf(toolchain.analyser.path, program, "config/${title.tables}"), root, log) != 0) {
            throw Failure(context.getString(R.string.build_tool_failed, "XenonAnalyse", tail(log)))
        }
        // Call of Duty 4 has a kind of jump table the analyser does not
        // recognise (setup\setup.ps1 runs tools/intext_switch_tables.py there).
        if (kit.inTextTables) {
            try {
                InTextTables.add(File(root, program), File(root, "config/${title.tables}"))
            } catch (error: IOException) {
                throw Failure(context.getString(R.string.build_tool_failed, "the jump tables", error.message.orEmpty()))
            }
        }
        // The recompiler crashes, rather than complaining, when its output folder is missing.
        tree.deleteRecursively()
        tree.mkdirs()
        log = File(logs, "${title.library}_recomp.log")
        val command = listOf(toolchain.recompiler.path, title.toml, "../XenonRecomp/XenonUtils/ppc_context.h")
        if (execute(context, command, File(root, "config"), log) != 0 || sources(tree).isEmpty()) {
            throw Failure(context.getString(R.string.build_tool_failed, "XenonRecomp", tail(log)))
        }
        writeSharedHeader(context, tree)
        val made = statement(root, title, tree)
        if (made != title.expectedAbi) {
            // The first line the two differ in, by its name, as the runtime would say it.
            val different = made.lines().zip(title.expectedAbi.lines()).firstOrNull { it.first != it.second }?.first?.substringBefore(' ')
            throw Failure(context.getString(R.string.build_statement_differs, title.library, different ?: "statement"))
        }
        treeMark(tree).writeText(made)
    }

    /** One file to compile, and the object it becomes. */
    private class Source(val title: KitTitle, val source: File, val objectFile: File)

    private class Measures {
        var lowestFreeMb = Long.MAX_VALUE
        var mostJobs = 0
        var hottestBattery = 0f
        var worstThermal = 0
    }

    private fun batteryCelsius(context: Context): Float {
        val battery = context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        return (battery?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0) ?: 0) / 10f
    }

    private fun thermalStatus(context: Context): Int =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            (context.getSystemService(Context.POWER_SERVICE) as PowerManager).currentThermalStatus
        } else {
            0
        }

    /**
     * How many compilers may run now, and what holds the number down. One
     * compiler of a recompiled file takes up to 250 MB (eight of them 1.4 GB
     * together, on the AYN Thor); [JOB_MB] leaves room for a larger file,
     * since the system ends the whole build if the compilers take what it
     * needs for itself; a device that says it is hot, or
     * whose battery is, gets half of them, and one alone when it is worse.
     */
    private fun jobs(context: Context, running: Int, ceiling: Int, measures: Measures): Pair<Int, String> {
        val memory = ActivityManager.MemoryInfo()
        (context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager).getMemoryInfo(memory)
        val freeMb = memory.availMem shr 20
        val battery = batteryCelsius(context)
        val thermal = thermalStatus(context)
        measures.lowestFreeMb = minOf(measures.lowestFreeMb, freeMb)
        measures.hottestBattery = maxOf(measures.hottestBattery, battery)
        measures.worstThermal = maxOf(measures.worstThermal, thermal)
        var allowed = ceiling
        var held = ""
        // The compilers running already have most of theirs: only what is
        // free beyond the reserve is for more of them.
        val byMemory = running + ((freeMb - RESERVE_MB) / JOB_MB).toInt()
        if (byMemory < allowed) {
            allowed = byMemory
            held = "memory"
        }
        val byHeat = when {
            thermal >= PowerManager.THERMAL_STATUS_CRITICAL || battery >= 50f -> 1
            thermal >= PowerManager.THERMAL_STATUS_SEVERE || battery >= 46f -> maxOf(1, ceiling / 2)
            else -> ceiling
        }
        if (byHeat < allowed) {
            allowed = byHeat
            held = "heat"
        }
        return Pair(maxOf(1, allowed), held)
    }

    /**
     * Compiles [units], as many at once as [jobs] allows; those whose object
     * is there already are done. They are [before] to [before] + their number
     * of the [all] files this build compiles, which is what the screen counts.
     */
    private fun compile(
        context: Context, kit: Kit, toolchain: Toolchain, units: List<Source>, measures: Measures, before: Int, all: Int,
    ) {
        val root = root(context)
        val pending = ArrayDeque(units.filterNot { it.objectFile.isFile })
        var done = units.size - pending.size
        val lock = Object()
        var running = 0
        var failure: String? = null
        // Fewer at once after the system ended one of them for its memory.
        var ceiling = Runtime.getRuntime().availableProcessors()
        val started = SystemClock.elapsedRealtime()
        val doneBefore = done
        val options = toolchain.compile + kit.compile + listOf(
            "-I", toolchain.simde.path, "-I", File(toolchain.simde, "simde").path,
            "-include", File(root, "runtime/mmio_hook.h").path,
        )
        synchronized(lock) {
            while ((pending.isNotEmpty() || running > 0) && !(failure != null && running == 0)) {
                if (stopRequested) {
                    // stop() has ended the compilers; their threads are let finish.
                    while (running > 0) lock.wait()
                    throw Stopped()
                }
                val (allowed, held) = jobs(context, running, ceiling, measures)
                while (failure == null && running < allowed && pending.isNotEmpty()) {
                    val unit = pending.removeFirst()
                    running++
                    measures.mostJobs = maxOf(measures.mostJobs, running)
                    Thread({
                        val partial = File(unit.objectFile.path + ".part")
                        val log = File(unit.objectFile.path + ".log")
                        val command = options + unit.title.defines.map { "-D$it" } +
                            listOf("-I", File(root, unit.title.tree).path, "-c", unit.source.path, "-o", partial.path)
                        var status = -1
                        var problem: String? = null
                        try {
                            status = execute(context, command, root, log)
                        } catch (_: Stopped) {
                        } catch (error: IOException) {
                            problem = error.message ?: error.toString()
                        }
                        synchronized(lock) {
                            running--
                            when {
                                stopRequested -> partial.delete()
                                status == 0 && partial.renameTo(unit.objectFile) -> {
                                    done++
                                    log.delete()
                                }
                                // Ended by a signal (128 + 9): the system took its
                                // memory back. Again, with one compiler fewer.
                                status == 137 && ceiling > 1 -> {
                                    ceiling--
                                    partial.delete()
                                    pending.addFirst(unit)
                                }
                                else -> failure = context.getString(R.string.build_tool_failed, unit.source.name, problem ?: tail(log))
                            }
                            lock.notifyAll()
                        }
                    }, "compile-${unit.source.name}").start()
                }
                val finished = done - doneBefore
                val elapsed = SystemClock.elapsedRealtime() - started
                progress = Progress(
                    running = true, step = Step.COMPILE, library = pending.firstOrNull()?.title?.library.orEmpty(),
                    done = before + done, total = all, jobs = running,
                    secondsLeft = if (finished >= 2) (all - before - done) * elapsed / finished / 1000 else -1,
                    held = held, batteryCelsius = batteryCelsius(context),
                )
                // Woken when a compiler ends; otherwise looked at again for memory and heat.
                lock.wait(2000)
            }
            failure?.let { throw Failure(it) }
        }
    }

    private fun link(context: Context, kit: Kit, toolchain: Toolchain, title: KitTitle, objects: List<File>, statement: String) {
        val root = root(context)
        val folder = File(root, "obj/${title.library}")
        val name = "lib${title.library}game.so"
        val output = File(folder, name)
        // The objects by a file of their names: a hundred paths are a long command line.
        val list = File(folder, "objects.rsp")
        list.writeText(objects.joinToString("\n") { "\"${it.path}\"" } + "\n")
        val log = File(root, "logs/${title.library}_link.log")
        val command = toolchain.linkBefore + kit.link + listOf(
            "-soname", name, "--dynamic-list=${File(root, "cmake/game_library.list").path}", "-o", output.path, "@${list.path}",
        ) + toolchain.linkAfter
        if (execute(context, command, root, log) != 0 || !output.isFile) {
            throw Failure(context.getString(R.string.build_tool_failed, name, tail(log)))
        }
        // In place under its own name only when whole, and the statement after
        // it: a library without one counts as not built.
        val library = GameLibrary.built(context, title.title!!)
        library.parentFile?.mkdirs()
        statementOf(library).delete()
        if (!output.renameTo(library)) throw Failure(context.getString(R.string.build_tool_failed, name, "it could not be put in place"))
        statementOf(library).writeText(statement)
    }

    /**
     * Builds the game code that is [wanted], carrying on from what an earlier
     * run left. Blocks until it is done, has failed, or [stop] was called.
     */
    fun run(context: Context): Outcome {
        val times = LinkedHashMap<String, Long>()
        val measures = Measures()
        var at = SystemClock.elapsedRealtime()
        fun lap(name: String) {
            val now = SystemClock.elapsedRealtime()
            times[name] = (times[name] ?: 0) + (now - at)
            at = now
        }
        val outcome = try {
            build(context, measures, ::lap)
            Outcome.Done
        } catch (_: Stopped) {
            Outcome.Paused
        } catch (failure: Failure) {
            Outcome.Failed(failure.message.orEmpty())
        } catch (error: Exception) {
            Outcome.Failed(error.message ?: error.toString())
        }
        try {
            report(context).appendText(
                buildString {
                    append("${Build.MANUFACTURER} ${Build.MODEL}, Android ${Build.VERSION.RELEASE}: ")
                    append(when (outcome) { is Outcome.Done -> "done"; is Outcome.Paused -> "paused"; is Outcome.Failed -> "failed: ${outcome.message}" })
                    append("\n")
                    times.forEach { (name, time) -> append("  %-28s %6.1f s\n".format(name, time / 1000.0)) }
                    append("  most compilers at once: ${measures.mostJobs}; least free memory: ${measures.lowestFreeMb} MB; ")
                    append("hottest battery: ${measures.hottestBattery} C; worst thermal status: ${measures.worstThermal}\n")
                },
            )
        } catch (_: IOException) {
            // The report is for a bug report; the build does not depend on it.
        }
        progress = Progress()
        return outcome
    }

    private fun build(context: Context, measures: Measures, lap: (String) -> Unit) {
        val kit = kit(context) ?: throw Failure("this app has no build kit")
        val toolchain = Toolchain.find(context) ?: throw Failure(context.getString(R.string.build_toolchain_missing))
        if (!kit.takes(toolchain)) throw Failure(context.getString(R.string.build_toolchain_other))
        val root = root(context)
        val titles = wanted(context)

        progress = Progress(running = true, step = Step.PREPARE)
        toolchain.unpack { done, total -> progress = Progress(running = true, step = Step.PREPARE, done = done, total = total) }
        // The kit's files, as the repository has them; again each time, since
        // an update of the app may have brought others.
        for (name in titles.map { "config/${it.toml}" } + kit.extraSources + listOf("runtime/mmio_hook.h", "cmake/game_library.list")) {
            val target = File(root, name)
            target.parentFile?.mkdirs()
            context.assets.open("buildkit/$name").use { input -> target.outputStream().use { input.copyTo(it) } }
        }
        File(root, "XenonRecomp/XenonUtils").mkdirs()
        toolchain.context.copyTo(File(root, "XenonRecomp/XenonUtils/ppc_context.h"), overwrite = true)
        lap("prepare")

        titles.forEachIndexed { index, title ->
            progress = Progress(running = true, step = Step.RECOMPILE, library = title.library, done = index, total = titles.size)
            val mark = treeMark(File(root, title.tree))
            val whole = try {
                mark.readText() == title.expectedAbi
            } catch (_: IOException) {
                false
            }
            if (!whole) recompile(context, kit, toolchain, title)
            lap("recompile ${title.library}")
        }

        // What is compiled, as CMakeLists.txt lists it: the tree's files, its
        // function table, the statement, and what the title has besides.
        val units = ArrayList<Source>()
        val objects = HashMap<String, List<File>>()
        // Objects made with other options, or for another statement, are not these.
        val made = hash((toolchain.compile + kit.compile + toolchain.version).joinToString("\n"))
        for (title in titles) {
            val tree = File(root, title.tree)
            val folder = File(root, "obj/${title.library}")
            val stamp = File(folder, "made-with")
            val wanted = made + "\n" + title.expectedAbi
            val same = try {
                stamp.readText() == wanted
            } catch (_: IOException) {
                false
            }
            if (!same) {
                folder.deleteRecursively()
                folder.mkdirs()
                stamp.writeText(wanted)
            }
            val statement = File(folder, "game_abi.cpp")
            statement.writeText(
                "// The statement of what this library was built against (cmake/game_abi.cmake).\n" +
                    "extern \"C\" __attribute__((visibility(\"default\"))) const char mw2_game_abi[] = R\"abi(${title.expectedAbi})abi\";\n",
            )
            val files = sources(tree) + File(tree, "ppc_func_mapping.cpp") + statement + kit.extraSources.map { File(root, it) }
            val mine = files.map { Source(title, it, File(folder, it.name.removeSuffix(".cpp") + ".o")) }
            units.addAll(mine)
            objects[title.library] = mine.map { it.objectFile }
        }
        // One title after the other, so that the first can be played while
        // the second is compiled; but counted together, for the time left.
        var before = 0
        for (title in titles) {
            val mine = units.filter { it.title === title }
            val all = units.size
            compile(context, kit, toolchain, mine, measures, before, all)
            before += mine.size
            lap("compile ${title.library}")
            progress = Progress(running = true, step = Step.LINK, library = title.library, done = before, total = all)
            link(context, kit, toolchain, title, objects.getValue(title.library), title.expectedAbi)
            lap("link ${title.library}")
            // The tree and the objects are a hundred megabytes and more a
            // title, and are of no use once the library is in place.
            File(root, title.tree).deleteRecursively()
            File(root, "obj/${title.library}").deleteRecursively()
        }
    }

    /** One compiler's memory at its peak, and what is left to the system, in megabytes. */
    private const val JOB_MB = 400L
    private const val RESERVE_MB = 1000L
}
