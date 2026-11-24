plugins {
    id("com.android.application")
}

// A test app for the build tools' package (docs/ondevice-build.md): it runs a
// command as an app does, with the tools found as the game's app finds them,
// and loads a library the command made. What an adb shell may run and load
// says nothing of what an app may, so a new device or Android version is
// tried with this. Never published.
//
//   -PappSuffix=.tc   the toolchain package it looks for is
//                     com.recomp.toolchain<suffix>
val appSuffix = (findProperty("appSuffix") as String?).orEmpty()

android {
    namespace = "com.recomp.toolchain.probe"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.recomp.toolchain.probe$appSuffix"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "1"
        manifestPlaceholders["toolchainPackage"] = "com.recomp.toolchain$appSuffix"
        buildConfigField("String", "TOOLCHAIN_PACKAGE", "\"com.recomp.toolchain$appSuffix\"")
    }

    buildFeatures { buildConfig = true }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
