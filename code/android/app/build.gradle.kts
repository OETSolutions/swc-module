plugins {
    id("com.android.application")
    // The Compose Compiler is a KOTLIN plugin, and its version must match the
    // Kotlin compiler. AGP 9 supplies Kotlin 2.2.10 itself (verified:
    // `:app:dependencies --configuration kotlinCompilerClasspath` resolves
    // kotlin-compiler-embeddable:2.2.10), so this is pinned to 2.2.10 to match.
    // A mismatch fails the build with a compiler-plugin version error rather than
    // anything about Compose.
    id("org.jetbrains.kotlin.plugin.compose") version "2.2.10"
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
        // 1.0.0: the first release with the complete no-app feature set — the
        // headless Pico-style gesture programming (§8.2) and the slot-level
        // defaults that make single/double/long reach three head-unit functions
        // with no app. `versionCode` must increase by at least 1 per published
        // artifact; it is reset to a plain monotonic integer rather than derived
        // from the name, so a future hotfix cannot collide.
        versionCode = 2
        versionName = "1.0.0"
    }

    // Compose is enabled HERE (Task 21, the UI) and not in Task 20, where it
    // would have pulled in a compiler plugin and a Compose Compiler for code that
    // did not exist yet.
    buildFeatures {
        compose = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    // Unit tests are the gate for this module: the protocol client and codec are
    // pure JVM logic and must never need a device or emulator to verify.
    testOptions {
        unitTests.isReturnDefaultValues = true
        // Robolectric needs the Android resources and manifest for the unit-test
        // variant; without this it cannot build an application context and every
        // Compose test fails at setup rather than on its assertion.
        unitTests.isIncludeAndroidResources = true
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

    implementation(platform("androidx.compose:compose-bom:2024.10.01"))
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.activity:activity-compose:1.9.3")
    debugImplementation("androidx.compose.ui:ui-tooling")

    testImplementation("org.jetbrains.kotlinx:kotlinx-coroutines-test:1.9.0")
    testImplementation("junit:junit:4.13.2")

    // The UI tests run on the JVM under Robolectric, not on a device.
    //
    // The plan specified `connectedDebugAndroidTest`, which needs an emulator or a
    // board. Robolectric runs the SAME Compose semantics assertions on the JVM, so
    // the test that matters -- "the live view marks which button the device
    // classifies" -- actually gates every build instead of being a gate nobody can
    // run. `createComposeRule()` works unchanged under Robolectric.
    testImplementation("org.robolectric:robolectric:4.14.1")
    testImplementation("androidx.compose.ui:ui-test-junit4")
    debugImplementation("androidx.compose.ui:ui-test-manifest")
    testImplementation("androidx.test.ext:junit:1.2.1")

    // UsbSerialTransport is deliberately still absent: this task is the UI. The
    // real transport is its own task, and the fake one in the tests is what keeps
    // the client and the screens verifiable without a board.
}
