plugins {
    id("com.android.application")
}

// The recompiled code's package (docs/game-library.md): the campaign's library
// and the multiplayer's (libmw3game.so and libmw3mpgame.so, or libmw2game.so
// and libmw2mpgame.so), made from the player's own disc by setup/setup.ps1,
// as an app with no code, no screen and no permissions. Its only use is that
// Android unpacks the two libraries where the game's app may load them from
// (GameLibrary.kt). It contains the game's code and is never published: it
// is built and installed on the player's own devices, signed with their own
// machine's debug key.
//
//   -PgameLibraries=<folder>  where the libraries are, under arm64-v8a/ as
//                             jniLibs folders are, and nothing else: all of
//                             the folder is packaged; setup/setup.ps1 gives it
//   -Pgame=mw2                Modern Warfare 2's package (com.mw2.recomp.game),
//   -Pgame=cod4               Call of Duty 4's (com.cod4.recomp.game),
//   -Pgame=aw                 Advanced Warfare's (com.aw.recomp.game),
//   -Pgame=bo3                Black Ops III's (com.bo3.recomp.game);
//                             Modern Warfare 3's without it
//   -PappSuffix=.dev          as android/app/build.gradle.kts: the package a
//                             development build of the app looks for
val gameLibraries = (findProperty("gameLibraries") as String?)?.let { file(it) }
val game = (findProperty("game") as String?) ?: "mw3"
val gameNames = mapOf(
    "mw3" to "Modern Warfare 3", "mw2" to "Modern Warfare 2", "cod4" to "Modern Warfare (Call of Duty 4)",
    "aw" to "Advanced Warfare", "bo3" to "Black Ops III",
)
if (game !in gameNames) throw GradleException("-Pgame must be one of ${gameNames.keys.joinToString()}, not '$game'")
val appSuffix = (findProperty("appSuffix") as String?).orEmpty()

android {
    namespace = "com.mw3.recomp.game"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.$game.recomp$appSuffix.game"
        minSdk = 26
        targetSdk = 35
        // Always the same: every build of it replaces the one installed, and
        // what it holds is told by the libraries themselves (mw2_game_abi).
        versionCode = 1
        versionName = "built from your disc"
        manifestPlaceholders["gameName"] = gameNames.getValue(game)
        ndk { abiFilters += "arm64-v8a" }
    }

    sourceSets {
        getByName("main") { gameLibraries?.let { jniLibs.srcDir(it) } }
    }

    packaging {
        jniLibs {
            // Real files in the package's library folder: the game's app
            // loads them by path.
            useLegacyPackaging = true
        }
    }
}

// A package without the libraries would install and be of no use, and the
// app would say only that the game code is missing. One with the other game's
// libraries beside its own would be twice the size for nothing.
tasks.configureEach {
    if (name.startsWith("merge") && name.endsWith("JniLibFolders")) {
        doFirst {
            val all = gameLibraries?.let { File(it, "arm64-v8a").listFiles() }.orEmpty()
            // Advanced Warfare's may hold a third: its multiplayer as the
            // title update makes it, for a player's own map packs (docs/dlc.md).
            // Black Ops III's second is its zombies'.
            val second = if (game == "bo3") "libbo3zmgame.so" else "lib${game}mpgame.so"
            val own = setOf("lib${game}game.so", second) + if (game == "aw") setOf("libawmp17game.so") else emptySet()
            val needed = setOf("lib${game}game.so", second)
            if (all.none { it.name in needed }) {
                throw GradleException(
                    "No game libraries: give -PgameLibraries=<folder holding arm64-v8a/lib${game}game.so>. " +
                        "setup/setup.ps1 builds them from your disc and runs this for you."
                )
            }
            val others = all.filter { it.name !in own }
            if (others.isNotEmpty()) {
                throw GradleException(
                    "${others.joinToString { it.name }} in $gameLibraries/arm64-v8a would be packaged too: " +
                        "the folder must hold only ${own.joinToString(", ")}."
                )
            }
        }
    }
}
