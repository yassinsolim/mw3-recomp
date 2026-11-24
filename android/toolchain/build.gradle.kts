plugins {
    id("com.android.application")
}

// The build tools' package (docs/ondevice-build.md): clang with lld, and the
// recompiler with its analyser, built for the phone, and what clang compiles
// against. An app with no code, no screen and no permissions, as the game
// code's package is (android/game): its only use is that Android unpacks the
// programs where an app may run them from, and that the game's app can read
// the rest out of the package. It holds nothing of any game and is published
// beside the apps.
//
//   -PtoolchainFiles=<folder>  jniLibs/ and assets/ as tools/toolchain/package.py
//                              writes them; tools/toolchain/build.sh gives it
//   -PappSuffix=.tc            another package name, for a test beside the
//                              published one
val toolchainFiles = (findProperty("toolchainFiles") as String?)?.let { file(it) }
val appSuffix = (findProperty("appSuffix") as String?).orEmpty()

fun setting(name: String): String? = System.getenv(name)?.trim()?.ifEmpty { null }

// The package's version is the toolchain's own (tools/toolchain/versions.sh),
// which toolchain.json states as well.
val toolchainVersion = Regex("""(?m)^TOOLCHAIN_VERSION=(\d+)$""")
    .find(rootProject.file("../tools/toolchain/versions.sh").readText())?.groupValues?.get(1)?.toInt()
    ?: throw GradleException("tools/toolchain/versions.sh no longer names TOOLCHAIN_VERSION")

android {
    namespace = "com.recomp.toolchain"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.recomp.toolchain$appSuffix"
        minSdk = 26
        targetSdk = 35
        versionCode = toolchainVersion
        versionName = toolchainVersion.toString()
        ndk { abiFilters += "arm64-v8a" }
    }

    sourceSets {
        getByName("main") {
            toolchainFiles?.let {
                jniLibs.srcDir(File(it, "jniLibs"))
                assets.srcDir(File(it, "assets"))
            }
        }
    }

    // As the app's (android/app/build.gradle.kts): the key given in the
    // environment, which is the release's, or the debug key.
    signingConfigs {
        val keystorePath = setting("MW2_KEYSTORE")
        if (keystorePath != null && file(keystorePath).exists()) {
            create("supplied") {
                storeFile = file(keystorePath)
                storePassword = setting("MW2_KEYSTORE_PASSWORD")
                keyAlias = setting("MW2_KEY_ALIAS")
                keyPassword = setting("MW2_KEY_PASSWORD") ?: setting("MW2_KEYSTORE_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            signingConfig = signingConfigs.findByName("supplied") ?: signingConfigs.getByName("debug")
        }
    }

    packaging {
        jniLibs {
            // Real files in the package's library folder: Android runs a
            // program an app brings from nowhere else.
            useLegacyPackaging = true
            // They are programs, stripped already; the NDK's strip is not
            // asked to look at them.
            keepDebugSymbols += "**/*.so"
        }
    }
}

// A package without the programs would install and be of no use.
tasks.configureEach {
    if (name.startsWith("merge") && name.endsWith("JniLibFolders")) {
        doFirst {
            val programs = toolchainFiles?.let { File(it, "jniLibs/arm64-v8a").listFiles() }.orEmpty()
            if (programs.none { it.name == "libllvm.so" }) {
                throw GradleException(
                    "No toolchain: give -PtoolchainFiles=<folder holding jniLibs/arm64-v8a/libllvm.so>. " +
                        "tools/toolchain/build.sh builds it and runs this for you."
                )
            }
        }
    }
}
