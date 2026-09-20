plugins {
    id("com.android.application")
}

android {
    namespace = "com.oetsolutions.swc"

    // compileSdk and targetSdk are DIFFERENT knobs and are deliberately not equal.
    // compileSdk is "which API can I reference" and only needs to be installed;
    // targetSdk is "which behavioral contract do I opt into" and is a product
    // decision. See the targetSdk note below.
    //
    // 36 rather than the plan's 34: this machine has android-36.1 and android-37.0
    // installed, not android-34, and raising compileSdk is safe -- it grants no
    // behavior change on its own. (`36.1` is a minor-versioned platform; AGP
    // accepts the integer 36 for it.)
    compileSdk = 36

    defaultConfig {
        applicationId = "com.oetsolutions.swc"
        minSdk = 26
        // targetSdk stays 34, exactly as the plan requires, and 34 < compileSdk is
        // the normal, supported arrangement. Android 15's background-activity-launch
        // (BAL) hardening blocks an app from starting an activity while it is in the
        // background -- which is precisely what this app's launch-app action does
        // from a background service. Raising this requires re-reading the spec's BAL
        // section first; it is a product decision, not a build detail.
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0"
    }

    // `buildFeatures { compose = true }` is deliberately NOT set here. This task is
    // the model and the protocol client, both pure JVM; Compose is the UI task, and
    // starting Kotlin 2.0 the Compose Compiler Gradle plugin must be applied
    // alongside it. Enabling it now would add a plugin and a compiler for code that
    // does not exist yet -- and, as it happens, fails the build on its own:
    // "Starting in Kotlin 2.0, the Compose Compiler Gradle plugin is required when
    // compose is enabled."

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    // Unit tests are the gate for this module: the protocol client and codec are
    // pure JVM logic and must never need a device or emulator to verify.
    testOptions {
        unitTests.isReturnDefaultValues = true
    }
}

// Kotlin comes from AGP 9's BUILT-IN support -- there is deliberately no
// `org.jetbrains.kotlin.android` plugin here.
//
// AGP 9 registers the `kotlin` extension itself, so applying the standalone Kotlin
// plugin fails the build outright: "Cannot add extension with name 'kotlin', as
// there is an extension already registered with that name." (Verified against AGP
// 9.1.0's own `BuiltInKotlinCreationConfig` classes.) Using the built-in support
// also means the Kotlin version is AGP's own, so there is no AGP/Kotlin pairing to
// keep in step -- one fewer version to pin, and one fewer way to fail.
//
// The JVM target follows `compileOptions.sourceCompatibility` above.

dependencies {
    // The wire contract (Task 19) is generated into the app's own source tree by
    // code/tools/gen_contract_kotlin.py, so it needs no dependency entry.
    implementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.7.3")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.9.0")

    testImplementation("org.jetbrains.kotlinx:kotlinx-coroutines-test:1.9.0")
    testImplementation("junit:junit:4.13.2")

    // Compose and the USB/transport libraries land with their own tasks (21 and the
    // transport task): this task is the model and the protocol client only, and a
    // dependency nothing references is a dependency that hides a missing one.
}
