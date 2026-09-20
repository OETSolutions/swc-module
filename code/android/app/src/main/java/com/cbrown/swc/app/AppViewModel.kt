package com.oetsolutions.swc.app

import com.oetsolutions.swc.action.ActionOutcome
import com.oetsolutions.swc.action.ActionRunner
import com.oetsolutions.swc.contract.Frames
import com.oetsolutions.swc.link.Frame
import com.oetsolutions.swc.link.LinkProblem
import com.oetsolutions.swc.link.LinkState
import com.oetsolutions.swc.link.SwcClient
import com.oetsolutions.swc.link.SwcTransport
import com.oetsolutions.swc.model.Action
import com.oetsolutions.swc.model.Config
import com.oetsolutions.swc.model.ConfigJson
import com.oetsolutions.swc.model.Gesture
import com.oetsolutions.swc.model.LadderButton
import com.oetsolutions.swc.ui.BindingCell
import com.oetsolutions.swc.ui.BindingUiState
import com.oetsolutions.swc.ui.LadderUiState
import com.oetsolutions.swc.ui.LearnedButton
import com.oetsolutions.swc.ui.LinkUiState
import com.oetsolutions.swc.ui.UpdateStatus
import com.oetsolutions.swc.ui.UpdateUiState
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.int
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive

/**
 * The join between the protocol client and the four screens.
 *
 * **This class is the piece the plan was missing.** Tasks 20, 21 and 22 built the
 * model and client, the screens, and the CI gates, and no task composed them:
 * `SwcClient` was referenced only by its own test, `ActionRunner` by nothing at
 * all, and `MainActivity` rendered every screen against default state with no-op
 * callbacks. The app built, tested and rendered, and could not do anything. This
 * is that composition, and it is deliberately logic-only so it can be tested on
 * the JVM against a fake transport rather than only by hand on a phone.
 *
 * It owns no Android API except where a screen type requires one, so the whole
 * state machine below — link status, the live reading, the bindings grid, the
 * update status — is exercised by `AppViewModelTest` with no emulator.
 */
class AppViewModel(
    private val client: SwcClient,
    private val scope: CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.Default),
    /**
     * Persisting a config. Injected because the real write needs the device, and
     * the screen's job is to show the RESULT — a nack, a timeout, a refusal — not
     * to know how the bytes travel.
     */
    private val saveConfig: suspend (Config) -> Boolean = { c -> client.setConfig(c) is com.oetsolutions.swc.link.AckResult.Ok },
    /**
     * Running a kind the FIRMWARE does not execute, or null when this build has no
     * way to (a test, a preview).
     *
     * Spec 3.6 splits the action library in two: the `OUT_` family is executed by
     * the firmware, and **everything else is executed by Android**. The firmware
     * confirms a recognized press with `event` and then, for an app-side kind,
     * releases the line rather than holding a key with no action behind it — the
     * phantom-key hazard. So the app must run the action on receipt, and without
     * this it never did: `ActionRunner` had no caller at all.
     *
     * A FUNCTION rather than an `ActionRunner`, because the real class needs a
     * `Context` and is not open. Injecting the three-argument call keeps the
     * dispatch decision testable on the JVM, with `ActionRunner` left as the thin
     * `when` it already is.
     */
    private val runAppAction: ((kind: String, target: String, payload: String) -> ActionOutcome)? = null,
) {

    private val _actionOutcomes = MutableStateFlow<List<String>>(emptyList())

    /**
     * App-side actions that could not run, stated so the user can act.
     *
     * This is the surface for spec 3.6's Android BAL limitation: an app launched
     * from the background may be refused by the system (hence `targetSdk` 34), and
     * "the button did nothing" is the outcome `ActionOutcome` exists to prevent.
     */
    val actionOutcomes: StateFlow<List<String>> = _actionOutcomes.asStateFlow()

    private val _link = MutableStateFlow(LinkUiState())
    val link: StateFlow<LinkUiState> = _link.asStateFlow()

    private val _ladder = MutableStateFlow(LadderUiState(idleMv = 0, buttons = emptyList()))
    val ladder: StateFlow<LadderUiState> = _ladder.asStateFlow()

    private val _bindings = MutableStateFlow(BindingUiState())
    val bindings: StateFlow<BindingUiState> = _bindings.asStateFlow()

    private val _update = MutableStateFlow(UpdateUiState())
    val update: StateFlow<UpdateUiState> = _update.asStateFlow()

    private var pendingEdits: MutableMap<String, Action?> = mutableMapOf()

    init {
        // `run()` never returns: it is the transport's only consumer, and the
        // client throws if the flow completes because a USB link must not end
        // while the app is alive. Launching it here means every frame the device
        // sends is routed exactly once.
        scope.launch { client.run() }
        scope.launch { client.state.collect { _link.value = linkStateFor(it) } }
        scope.launch { client.frames.collect { onFrame(it) } }
        scope.launch { client.config.collect { onConfig(it) } }
    }

    /** Ask the device to identify itself, and read its config back. */
    fun connect() {
        scope.launch {
            try {
                client.connect()
                client.getConfig()
            } catch (e: Exception) {
                // A failed connect is a link problem, not a crash: the device works
                // without the app, so the app must survive not reaching it.
                _link.value = _link.value.copy(problem = LinkProblem.NoDevice)
            }
        }
    }

    /**
     * The transport's own enumeration result, which is the ONLY source for two of
     * the five problems.
     *
     * A missing USB permission or a non-adapter device on the bus is known before
     * any frame is exchanged, so `SwcClient` can never report them — without this
     * call the four-state [LinkProblem] design would be unreachable and every
     * enumeration failure would render as the generic "No device found".
     */
    fun reportOpenProblem(problem: LinkProblem?) {
        _link.value = _link.value.copy(problem = problem)
    }

    fun retry() = connect()

    private fun linkStateFor(state: LinkState): LinkUiState {
        // The problem is REPLACED, not merged with `?:`. A problem that survived a
        // successful connect would tell the user their cable is unplugged while the
        // device is answering, and the stale message never clears on its own.
        val problem = when (state) {
            is LinkState.VersionMismatch ->
                LinkProblem.VersionMismatch(state.firmware, state.app)
            is LinkState.Failed -> LinkProblem.NoDevice
            is LinkState.Connected -> null
            LinkState.Disconnected -> _link.value.problem
        }
        return _link.value.copy(link = state, problem = problem)
    }

    private fun onFrame(frame: Frame) {
        when (frame.type) {
            // `firmwareVersion` only. There is deliberately NO protocol-version
            // check here: the firmware writes `hello.protocol_v` from the SAME
            // constant as the envelope's `v` (`kNdjsonProtocolVersion`), so the
            // client's envelope check in `handle()` is the one home for that fact.
            // A second check here could only ever disagree with it — and worse,
            // it would be checking a frame the client already refused to dispatch
            // on a mismatch.
            Frames.HELLO -> {
                val fw = frame.fields["fw_version"]?.jsonPrimitive?.content
                _link.value = _link.value.copy(firmwareVersion = fw, problem = null)
            }

            // Spec 4.3's core event. This is what makes the live ladder live:
            // outside a learn run, `event` is the only frame carrying a level, so
            // without it the view has nothing to render (spec 4.3 notes).
            Frames.EVENT -> {
                // `button` is the learned ID, which is what ties the event to the
                // button the view draws. An index here would need re-deriving.
                // It is deliberately read as a nullable: FR-12 reports an
                // unrecognised press as `button: null`, and treating a null as
                // "nothing to do" would leave the live ladder frozen at its last
                // value while the device is in fact seeing every press.
                val id = frame.fields["button"]
                    ?.takeIf { it !is JsonNull }
                    ?.jsonPrimitive?.content
                val level = frame.fields["level_mv"]?.jsonPrimitive?.intOrNull
                val gesture = frame.fields["gesture"]?.jsonPrimitive?.content
                if (gesture != null && level != null) {
                    _ladder.value = _ladder.value.copy(
                        liveMv = level,
                        channelName = channelNameFor(frame.fields["channel"]?.jsonPrimitive?.intOrNull),
                        lastGesture = Gesture.fromWireName(gesture) ?: Gesture.NONE,
                        lastGestureButton = id,
                    )
                    // Spec 3.6: an app-side kind is the APP's job, and the firmware
                    // has already released the line rather than hold a key with no
                    // action behind it. Resolving here is what makes an app-side
                    // binding do anything at all. Skipped for a null button: no
                    // binding can name a button that was not recognised.
                    if (id != null && gesture != "NONE") runAppSideAction(id, gesture)
                }
            }

            // During a learn run the device streams the filtered level instead.
            // Same `level_mv` field name as `event`, deliberately: both frames
            // report the FR-3 filtered value, so the view reads one field name.
            Frames.LADDER_SAMPLE -> {
                val level = frame.fields["level_mv"]?.jsonPrimitive?.intOrNull
                if (level != null && level > 0) {
                    _ladder.value = _ladder.value.copy(liveMv = level)
                }
            }

            Frames.STATUS -> {
                val rail = frame.fields["rail_mv"]?.jsonPrimitive?.intOrNull
                if (rail != null && rail > 0) {
                    _ladder.value = _ladder.value.copy(idleMv = rail)
                }
            }
        }
    }

    private fun channelNameFor(index: Int?): String {
        val channels = client.config.value.channels
        if (index == null || index < 0 || index >= channels.size) return _ladder.value.channelName
        return channels[index].name.ifEmpty { "SWC${index + 1}" }
    }

    /**
     * Run the actions bound to (button, gesture), for the kinds Android executes.
     *
     * Spec 3.6's split: the firmware does the `OUT_` family, and every other kind is
     * the app's. A kind neither side knows is reported rather than dropped — a
     * binding the user made that silently does nothing is the failure mode
     * `ActionOutcome` exists to prevent.
     *
     * **The firmware still executes its own actions for this press.** This is not a
     * replacement path: the device drives the KEY line and confirms with `event`
     * regardless, and spec 3.5 requires that a failed app-side action never
     * prevents the hardware key press. So a failure here is reported and nothing
     * else is affected.
     */
    private fun runAppSideAction(buttonId: String, gestureName: String) {
        val gesture = Gesture.fromWireName(gestureName) ?: return
        val bindings = client.config.value.bindings.filter {
            it.enabled && it.button == buttonId && it.gesture == gesture
        }
        if (bindings.isEmpty()) return

        val outcomes = mutableListOf<String>()
        for (binding in bindings) {
            for (action in binding.actions) {
                val outcome = when (action.kind.wireName) {
                    "OUT_VOLTAGE", "OUT_RELEASE", "NONE", "BUZZ" ->
                        // The firmware's half of spec 3.6. It has already done it.
                        null
                    else -> runOne(action)
                }
                if (outcome != null) outcomes += "$buttonId $gestureName: $outcome"
            }
        }
        _actionOutcomes.value = outcomes
    }

    /** One action, through the injected runner if this build has one. */
    private fun runOne(action: Action): String? {
        val run = runAppAction
            ?: return "${action.kind.wireName} needs the app, but no action runner is available"
        val outcome = run(action.kind.wireName, action.target, action.payload)
        return when (outcome) {
            ActionOutcome.Ran -> null
            is ActionOutcome.AppNotInstalled -> "${outcome.pkg} is not installed"
            is ActionOutcome.NoHandler -> "nothing on this phone handles ${outcome.action}"
            is ActionOutcome.Blocked -> outcome.reason
            is ActionOutcome.NotAppSide -> "${outcome.kind} is not an app-side action"
        }
    }


    /**
     * The device's config became known (from a `config_get` reply run).
     *
     * This is what turns the empty screens into real ones: the ladder view gets
     * the learned buttons, and the bindings grid gets one cell per button×gesture
     * that the config actually binds.
     */
    private fun onConfig(config: Config) {
        val channel = config.channels.firstOrNull()
        _ladder.value = _ladder.value.copy(
            idleMv = channel?.ladder?.learnedIdleMv ?: _ladder.value.idleMv,
            channelName = channel?.name?.ifEmpty { null } ?: _ladder.value.channelName,
            buttons = channel?.ladder?.buttons.orEmpty().map { b: LadderButton ->
                LearnedButton(id = b.id, name = b.name.ifEmpty { b.id },
                    mvCenter = b.mvCenter, mvTolerance = b.mvTolerance)
            },
        )
        _bindings.value = _bindings.value.copy(
            cells = buildCells(config),
            problems = ConfigJson.problems(config).map { it.toString() },
        )
    }

    /**
     * One cell per button×gesture, with the config's own binding for that pair.
     *
     * The GESTURE list is the protocol's four, not the three the plan's example
     * used: NONE exists on the wire (spec 3.6) and a grid that cannot express it
     * could not show a binding the device might hold.
     */
    private fun buildCells(config: Config): List<BindingCell> {
        val channel = config.channels.firstOrNull() ?: return emptyList()
        val gestures = listOf(Gesture.SINGLE, Gesture.DOUBLE, Gesture.LONG)
        return channel.ladder.buttons.flatMap { b ->
            gestures.map { g ->
                val edit = pendingEdits["${b.id}/${g.wireName}"]
                BindingCell(
                    buttonId = b.id,
                    buttonName = b.name.ifEmpty { b.id },
                    gesture = g.wireName,
                    action = if (pendingEdits.containsKey("${b.id}/${g.wireName}")) edit
                    else config.bindings
                        .firstOrNull { it.button == b.id && it.gesture == g }
                        ?.actions?.firstOrNull(),
                )
            }
        }
    }

    /** Record an edit locally. It is not sent until [save]. */
    fun editBinding(cell: BindingCell, action: Action?) {
        pendingEdits["${cell.buttonId}/${cell.gesture}"] = action
        _bindings.value = _bindings.value.copy(cells = buildCells(client.config.value))
    }

    /**
     * Write the edited bindings to the device.
     *
     * The local model is adopted only on a NON-NACKED reply — `setConfig` enforces
     * that — so a rejected config never becomes what the app displays. On success
     * the pending edits are dropped, because they are now in the device's config.
     */
    fun save() {
        scope.launch {
            val current = client.config.value
            val merged = withEdits(current)
            val problems = ConfigJson.problems(merged)
            if (problems.isNotEmpty()) {
                _bindings.value = _bindings.value.copy(problems = problems.map { it.toString() })
                return@launch
            }
            val ok = try {
                saveConfig(merged)
            } catch (e: Exception) {
                false
            }
            if (ok) {
                pendingEdits.clear()
                _bindings.value = _bindings.value.copy(problems = emptyList())
            } else {
                _bindings.value = _bindings.value.copy(
                    problems = listOf("The device did not accept the configuration."),
                )
            }
        }
    }

    private fun withEdits(config: Config): Config {
        val channel = config.channels.firstOrNull() ?: return config
        // Rebuild the binding list: keep every binding whose pair was NOT edited,
        // then add one per edited pair that has an action.
        val kept = config.bindings.filter { b ->
            !pendingEdits.containsKey("${b.button}/${b.gesture.wireName}")
        }
        val added = pendingEdits.mapNotNull { (key, action) ->
            val (button, gestureName) = key.split("/")
            val gesture = Gesture.fromWireName(gestureName) ?: return@mapNotNull null
            if (action == null || action.kind.wireName == "NONE") return@mapNotNull null
            com.oetsolutions.swc.model.Binding(
                id = "${button}-${gestureName}",
                channel = config.bindings.firstOrNull()?.channel
                    ?: com.oetsolutions.swc.model.BindingChannel.SWC1,
                button = button,
                gesture = gesture,
                enabled = true,
                actions = listOf(action),
            )
        }
        return config.copy(
            channels = listOf(channel),
            bindings = kept + added,
        )
    }

    fun checkForUpdates() {
        scope.launch {
            _update.value = _update.value.copy(inProgress = true)
            val version = _link.value.firmwareVersion
            _update.value = _update.value.copy(
                inProgress = false,
                currentVersion = version ?: "—",
                status = if (version == null) UpdateStatus.Failed("No device is connected.")
                else UpdateStatus.UpToDate(version),
            )
        }
    }

    fun close() = client.close()
}
