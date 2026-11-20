plugins {
    id("com.android.application")
}

// The recompiled code's package (docs/game-library.md): libmw3game.so and
// libmw3mpgame.so, made from the player's own disc by setup/setup.ps1, as an
// app with no code, no screen and no permissions. Its only use is that
// Android unpacks the two libraries where the game's app may load them from
// (GameLibrary.kt). It contains the game's code and is never published: it
// is built and installed on the player's own devices, signed with their own
// machine's debug key.
//
//   -PgameLibraries=<folder>  where the libraries are, under arm64-v8a/ as
//                             jniLibs folders are; setup/setup.ps1 gives it
//   -Pmw3Suffix=.dev          as android/app/build.gradle.kts: the package a
//                             development build of the app looks for
val gameLibraries = (findProperty("gameLibraries") as String?)?.let { file(it) }
val mw3Suffix = (findProperty("mw3Suffix") as String?).orEmpty()

android {
    namespace = "com.mw3.recomp.game"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.mw3.recomp$mw3Suffix.game"
        minSdk = 26
        targetSdk = 35
        // Always the same: every build of it replaces the one installed, and
        // what it holds is told by the libraries themselves (mw2_game_abi).
        versionCode = 1
        versionName = "built from your disc"
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
// app would say only that the game code is missing.
tasks.configureEach {
    if (name.startsWith("merge") && name.endsWith("JniLibFolders")) {
        doFirst {
            val found = gameLibraries?.let { File(it, "arm64-v8a").listFiles { f -> f.name.endsWith("game.so") } }.orEmpty()
            if (found.isEmpty()) {
                throw GradleException(
                    "No game libraries: give -PgameLibraries=<folder holding arm64-v8a/libmw3game.so>. " +
                        "setup/setup.ps1 builds them from your disc and runs this for you."
                )
            }
        }
    }
}
