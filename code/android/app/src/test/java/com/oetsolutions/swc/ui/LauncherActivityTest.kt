package com.oetsolutions.swc.ui

import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.w3c.dom.Element
import java.io.File
import javax.xml.parsers.DocumentBuilderFactory

/**
 * The manifest declares which class the launcher starts; nothing else in the build
 * checks that the class exists.
 *
 * This is not hypothetical. The manifest first declared `.MainActivity`, which expands
 * against the application ID to `com.oetsolutions.swc.MainActivity` — but the class lives in
 * `com.oetsolutions.swc.ui.MainActivity`. The APK built, installed, and then died at launch
 * with `ClassNotFoundException`. Every JVM test passed, because Robolectric instantiates
 * composables directly and never resolves the manifest's activity name; `assembleDebug`
 * passed, because it does not check launchability either. Running the app on an emulator
 * is what exposed it.
 *
 * So the assertion this file makes is the one that was missing: take the string from the
 * manifest, resolve it the way Android resolves it, and require a real Activity to come
 * back.
 */
class LauncherActivityTest {

    private fun manifest(): Element {
        // The unit-test working directory is the module dir, so the source manifest is
        // one relative hop away. Reading the SOURCE manifest (not the merged one under
        // build/) is deliberate: this is a check on what we wrote, and the merged file
        // only exists after a build has already run.
        val candidates = listOf(
            File("src/main/AndroidManifest.xml"),
            File("app/src/main/AndroidManifest.xml"),
        )
        val file = candidates.firstOrNull { it.isFile }
            ?: error("could not locate AndroidManifest.xml from ${File(".").absolutePath}")
        val factory = DocumentBuilderFactory.newInstance().apply { isNamespaceAware = true }
        return factory.newDocumentBuilder().parse(file).documentElement
    }

    private fun applicationId(): String {
        // applicationId and namespace are both com.oetsolutions.swc; read the build file rather
        // than hard-code it, so a rename here fails loudly instead of silently passing.
        val build = listOf(File("build.gradle.kts"), File("app/build.gradle.kts"))
            .firstOrNull { it.isFile }
            ?: error("could not locate build.gradle.kts")
        val text = build.readText()
        val match = Regex("""applicationId\s*=\s*"([^"]+)"""").find(text)
            ?: error("no applicationId in ${build.path}")
        return match.groupValues[1]
    }

    private fun activityElements(): List<Element> {
        val nodes = manifest().getElementsByTagName("activity")
        return (0 until nodes.length).map { nodes.item(it) as Element }
    }

    @Test
    fun `the manifest declares exactly one launcher activity`() {
        val launchers = activityElements().filter { activity ->
            val filters = activity.getElementsByTagName("intent-filter")
            (0 until filters.length).any { i ->
                val filter = filters.item(i) as Element
                val actions = filter.getElementsByTagName("action")
                val categories = filter.getElementsByTagName("category")
                val hasMain = (0 until actions.length).any {
                    (actions.item(it) as Element).getAttribute("android:name") ==
                        "android.intent.action.MAIN"
                }
                val hasLauncher = (0 until categories.length).any {
                    (categories.item(it) as Element).getAttribute("android:name") ==
                        "android.intent.category.LAUNCHER"
                }
                hasMain && hasLauncher
            }
        }
        assertTrue(
            "expected exactly one MAIN/LAUNCHER activity, found ${launchers.size}",
            launchers.size == 1,
        )
    }

    @Test
    fun `the launcher activity name resolves to a real Activity subclass`() {
        val pkg = applicationId()
        val declared = activityElements()
            .flatMap { activity ->
                val filters = activity.getElementsByTagName("intent-filter")
                val isLauncher = (0 until filters.length).any { i ->
                    val categories = (filters.item(i) as Element).getElementsByTagName("category")
                    (0 until categories.length).any {
                        (categories.item(it) as Element).getAttribute("android:name") ==
                            "android.intent.category.LAUNCHER"
                    }
                }
                if (isLauncher) listOf(activity) else emptyList()
            }
            .map { it.getAttribute("android:name") }

        val name = declared.singleOrNull()
            ?: throw AssertionError("expected exactly one launcher activity, got $declared")

        // Android's own expansion rule: a leading dot means "relative to the app's
        // package"; a bare name is absolute. This is the exact step that was missing.
        val fqcn = if (name.startsWith(".")) pkg + name else if (!name.contains(".")) "$pkg.$name" else name

        val cls = try {
            Class.forName(fqcn)
        } catch (e: ClassNotFoundException) {
            throw AssertionError(
                "the manifest launches `$name`, which resolves to `$fqcn`, but that class " +
                    "is not on the classpath. The app would install and then crash at launch " +
                    "with ClassNotFoundException. Known activities: " +
                    activityElements().map { it.getAttribute("android:name") },
                e,
            )
        }

        assertTrue(
            "`$fqcn` exists but is not an android.app.Activity subclass, so it cannot be launched",
            android.app.Activity::class.java.isAssignableFrom(cls),
        )
        assertTrue(
            "`$fqcn` must be a public top-level class with a no-arg constructor for " +
                "Android to instantiate it",
            java.lang.reflect.Modifier.isPublic(cls.modifiers),
        )
    }
}
