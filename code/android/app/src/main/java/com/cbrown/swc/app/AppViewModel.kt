package com.oetsolutions.swc.app

import com.oetsolutions.swc.action.ActionOutcome
import com.oetsolutions.swc.action.ActionRunner
import com.oetsolutions.swc.contract.Frames
import com.oetsolutions.swc.link.AckResult
import com.oetsolutions.swc.link.Frame
import com.oetsolutions.swc.link.LinkProblem
import com.oetsolutions.swc.link.LinkState
import com.oetsolutions.swc.link.SwcClient
import com.oetsolutions.swc.link.SwcTransport
import com.oetsolutions.swc.model.Action
import com.oetsolutions.swc.model.BindingChannel
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

    /**
     * Ask the device to open its maintenance window (spec 8.2), which is what
     * turns the radio on.
     *
     * The window is shown as open only on a NON-NACKED reply. A nack means the
     * device refused -- it has no orchestrator, so it cannot enter the mode -- and
     * saying "WiFi is on" there would send the user hunting for an access point
     * that does not exist, which is worse than the request having failed.
     */
    fun enterMaintenance() {
        setMaintenance(true)
    }

    fun exitMaintenance() {
        setMaintenance(false)
    }

    private fun setMaintenance(want: Boolean) {
        if (_link.value.maintenanceBusy) return
        scope.launch {
            _link.value = _link.value.copy(maintenanceBusy = true, maintenanceProblem = null)
            val result = try {
                if (want) client.enterMaintenance() else client.exitMaintenance()
            } catch (e: Exception) {
                AckResult.Nacked("link", e.message ?: "the link failed")
            }
            _link.value = when (result) {
                is AckResult.Ok -> _link.value.copy(
                    maintenanceBusy = false,
                    maintenanceOpen = want,
                    maintenanceProblem = null,
                )
                is AckResult.Nacked -> _link.value.copy(
                    maintenanceBusy = false,
                    maintenanceProblem = "The device refused: ${result.err} (${result.detail}).",
                )
                AckResult.Timeout -> _link.value.copy(
                    maintenanceBusy = false,
                    maintenanceProblem = "The device did not answer. Check the cable and try again.",
                )
            }
        }
    }

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
                val channel = frame.fields["channel"]?.jsonPrimitive?.intOrNull
                if (gesture != null && level != null) {
                    _ladder.value = _ladder.value.copy(
                        liveMv = level,
                        channelName = channelNameFor(channel),
                        lastGesture = Gesture.fromWireName(gesture) ?: Gesture.NONE,
                        lastGestureButton = id,
                    )
                    // Spec 3.6: an app-side kind is the APP's job, and the firmware
                    // has already released the line rather than hold a key with no
                    // action behind it. Resolving here is what makes an app-side
                    // binding do anything at all. Skipped for a null button: no
                    // binding can name a button that was not recognised.
                    if (id != null && gesture != "NONE") runAppSideAction(channel, id, gesture)
                }
            }

            // Spec 4.3's `log`: a diagnostic line the device raised. The one
            // producer is FR-18's clamp warning, so an ignored frame here means a
            // stored config value the device refused to drive as written goes
            // unmentioned -- the user's symptom would be "that button does the
            // wrong thing" with nothing anywhere saying why.
            Frames.LOG -> {
                val level = frame.fields["level"]?.jsonPrimitive?.content ?: "INFO"
                val msg = frame.fields["msg"]?.jsonPrimitive?.content
                if (msg != null) {
                    // Bounded: a device that logs on every poll tick must not grow
                    // the state without limit. The newest are the useful ones.
                    val kept = (_link.value.logs + "$level: $msg").takeLast(kMaxLogLines)
                    _link.value = _link.value.copy(logs = kept)
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

            // Spec 4.3's `status`, emitted every 2 s while connected. It carries
            // the CONFIG's state, which spec 4.3 says exists so a config fault has
            // "a name the app could read" (§6.8's `config_state: defaults`).
            //
            // This branch used to read `rail_mv` into the ladder's idle. That field
            // has no producer anywhere in the firmware (grep-confirmed, spec N-22),
            // so the read was dead -- and it would have been WRONG if it had ever
            // been sent: `rail_mv` is the +3V3 rail (~3300), not the wheel's idle
            // KEY level (~2835), and `LadderScreen` divides every band by whatever
            // it is given. The idle the view needs already comes from the config
            // (`onConfig` reads `ladder.learnedIdleMv`), so nothing is lost by
            // dropping it. `vbus_present`, `output_safe` and `uptime_ms` are
            // deliberately not mirrored yet: no screen consumes them, and inventing
            // a place for them is not this fix.
            Frames.STATUS -> {
                val cs = frame.fields["config_state"]?.jsonPrimitive?.content
                if (cs != null) _link.value = _link.value.copy(configState = cs)
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
    private fun runAppSideAction(channelIndex: Int?, buttonId: String, gestureName: String) {
        val gesture = Gesture.fromWireName(gestureName) ?: return
        // The channel filter mirrors `BindingResolve` (firmware) exactly: a binding
        // fires for the channel the press came from, or for `ANY`. Filtering on the
        // button id ALONE was a live defect once the board carries two channels with
        // the same button names (the common case -- `vol_up` on both): a press on
        // SWC1's `vol_up` would also fire SWC2's `vol_up` binding, launching an app
        // or sending an intent the second channel never asked for. The firmware
        // resolves channel+button; matching any less makes the app's half of spec
        // 3.6 fire MORE bindings than the device did.
        val asSwc = when (channelIndex) {
            0 -> BindingChannel.SWC1
            1 -> BindingChannel.SWC2
            // An unknown or absent channel cannot be matched against a binding's
            // channel field without guessing. The firmware only resolves the two SWC
            // channels here (an AUX input has no ladder and never arrives as a
            // channel index), so anything else matches nothing but `ANY`.
            else -> null
        }
        // ONLY THE FIRST matching binding, mirroring `BindingResolve`, which
        // returns the first match and stops. Running EVERY match fires app-side
        // actions the device never resolved: with an `ANY` wildcard and a
        // channel-specific binding on the same triple (`vol_up` SINGLE on both is
        // the common case), the app would launch TWO apps for one press while the
        // device acted on one binding. The app's half of spec 3.6 must fire the
        // same binding the device did, not more of them.
        val binding = client.config.value.bindings.firstOrNull {
            it.enabled && it.button == buttonId && it.gesture == gesture &&
                (it.channel == BindingChannel.ANY || (asSwc != null && it.channel == asSwc))
        } ?: return

        // EVERY action in that binding, not just the first. Spec 3.5 makes the
        // list ordered and best-effort, and the product's core case is one binding
        // with "the factory key press AND tell the app" -- the firmware runs the
        // `OUT_` half, the app runs the rest. Skipping a kind the firmware owns is
        // what makes those the firmware's rather than a second, competing
        // implementation.
        val outcomes = mutableListOf<String>()
        for (action in binding.actions) {
            val outcome = when (action.kind.wireName) {
                "OUT_VOLTAGE", "OUT_RELEASE", "NONE", "BUZZ" ->
                    // The firmware's half of spec 3.6. It has already done it.
                    //
                    // BUZZ is included because the firmware executes it: spec 3.6
                    // puts it in the firmware's column, and it plays the named
                    // §7.2 pattern (replacing the default KEY_ACCEPTED).
                    null
                else -> runOne(action)
            }
            if (outcome != null) outcomes += "$buttonId $gestureName: $outcome"
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
            is ActionOutcome.NotImplemented ->
                "${outcome.kind} is the app's to run, but this build does not implement it yet"
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
     * One cell per (channel, button, gesture), with the config's own binding for
     * that triple.
     *
     * **Every channel that has a ladder gets its cells**, not just the first. The
     * board is two-channel (FR-9) and the firmware binds per channel (spec 3.5,
     * `BindingResolve`); a grid built from `channels.firstOrNull()` showed SWC2's
     * bindings nowhere and could not edit them, and its channel-blind edit key
     * (see [editKey]) then made an SWC1 edit delete SWC2's same-named binding.
     */
    private fun buildCells(config: Config): List<BindingCell> {
        // The three GESTURES a user assigns by hand. `NONE` is a protocol value
        // (spec 3.6) but it is not one a user performs: the firmware emits it for
        // a press it could not classify, and such an event carries `button: null`
        // (spec 4.3), so it can never resolve a binding -- `BindingResolve` refuses
        // an event whose button index is past the ladder. There is therefore no
        // behaviour a `NONE` cell could bind, and no cell is offered for it. A
        // config carrying a `NONE` binding still round-trips untouched.
        val gestures = listOf(Gesture.SINGLE, Gesture.DOUBLE, Gesture.LONG)
        return config.channels.flatMapIndexed { index, channel ->
            val asSwc = swcChannel(index) ?: return@flatMapIndexed emptyList()
            channel.ladder.buttons.flatMap { b ->
                gestures.map { g ->
                    val key = editKey(asSwc, b.id, g.wireName)
                    val edit = pendingEdits[key]
                    BindingCell(
                        channel = asSwc,
                        buttonId = b.id,
                        buttonName = b.name.ifEmpty { b.id },
                        gesture = g.wireName,
                        action = if (pendingEdits.containsKey(key)) edit
                        else resolvedBindingFor(config, asSwc, b.id, g)?.actions?.firstOrNull(),
                    )
                }
            }
        }
    }

    /** The SWC channel a ladder index names, or null for an index with no ladder. */
    private fun swcChannel(index: Int): BindingChannel? = when (index) {
        0 -> BindingChannel.SWC1
        1 -> BindingChannel.SWC2
        else -> null
    }

    /**
     * The binding the DEVICE would fire for (channel, button, gesture), if any.
     *
     * This mirrors `BindingResolve` rather than querying for an exact channel
     * match, and the difference is the `ANY` wildcard: an `ANY` binding fires from
     * EITHER channel, so a cell whose channel has no binding of its own but whose
     * config holds an `ANY` one must show THAT binding -- the device would. A
     * channel-exact lookup would draw the cell empty and let the user "bind" a
     * gesture the device already acts on.
     *
     * The scan stops at the first match, exactly as the resolver does, so an `ANY`
     * binding that appears EARLIER in the table wins -- see [withEdits] for why the
     * ordering that makes this true is a correctness requirement, not a nicety.
     */
    private fun resolvedBindingFor(
        config: Config,
        channel: BindingChannel,
        button: String,
        gesture: Gesture,
    ): com.oetsolutions.swc.model.Binding? = config.bindings.firstOrNull {
        it.enabled && it.button == button && it.gesture == gesture &&
            (it.channel == channel || it.channel == BindingChannel.ANY)
    }

    /**
     * The identity of an edit: channel, button and gesture, all three.
     *
     * A binding is matched on all three (`BindingResolve`), so an edit must key on
     * all three or it names a different triple than the one it replaces. Keying on
     * `button/gesture` alone was a live data-loss defect: `vol_up` is on BOTH
     * ladders (both wheels have volume), so an edit to SWC1's `vol_up` SINGLE
     * produced the same key as SWC2's, and [withEdits] then dropped the device's
     * SWC2 binding as "edited" -- deleting it in a save the user was told worked.
     */
    private fun editKey(channel: BindingChannel, button: String, gesture: String): String =
        "${channel.wireName}/$button/$gesture"

    /** Record an edit locally. It is not sent until [save]. */
    fun editBinding(cell: BindingCell, action: Action?) {
        pendingEdits[editKey(cell.channel, cell.buttonId, cell.gesture)] = action
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
        if (config.channels.isEmpty()) return config
        // Rebuild the binding list: keep every binding whose TRIPLE was NOT edited,
        // then add one per edited triple that has an action.
        //
        // The filter is channel-scoped for the same reason the edit key is: a
        // binding is `(channel, button, gesture)`, so dropping by `button/gesture`
        // alone removed an UNEDITED binding on the other channel whenever the two
        // ladders shared a button id -- which is the normal case (`vol_up`).
        val kept = config.bindings.filter { b ->
            !pendingEdits.containsKey(editKey(b.channel, b.button, b.gesture.wireName))
        }
        val added = pendingEdits.mapNotNull { (key, action) ->
            val parts = key.split("/")
            if (parts.size != 3) return@mapNotNull null
            val (channelName, button, gestureName) = parts
            val channel = BindingChannel.fromWireName(channelName) ?: return@mapNotNull null
            val gesture = Gesture.fromWireName(gestureName) ?: return@mapNotNull null
            if (action == null || action.kind.wireName == "NONE") return@mapNotNull null
            // The button must be on the LADDER THE EDIT'S CHANNEL names -- not
            // merely on SOME ladder. `BindingResolve` looks the id up in
            // `channels[channel_index].ladder`, so a binding whose channel's ladder
            // does not hold the id is never found. Checking "on any ladder" would
            // pass an SWC2 edit of a button that exists only on SWC1, and the
            // firmware's `BindingNamesARealInput` would then refuse the whole
            // config. `vol_up` is on BOTH ladders (the common case), so the check
            // must be per-channel or it drops a legitimate second-channel edit.
            //
            // A button that is on NO ladder the config still has is a STALE edit:
            // its key came from a grid built against an earlier config, and a later
            // refresh (a `connect()` re-reads) can re-learn the button away.
            // Emitting a binding for it is worse than dropping it -- the firmware
            // refuses it and the app's `problems()` cannot catch it (it does not
            // mirror that rule), so the WHOLE save would be nacked and every valid
            // edit lost behind "device did not accept". (An AUX id never reaches
            // here: `buildCells` keys only ladder buttons.)
            if (!buttonOnThatLadder(config, channel, button)) return@mapNotNull null
            com.oetsolutions.swc.model.Binding(
                id = "${button}-${gestureName}",
                channel = channel,
                button = button,
                gesture = gesture,
                enabled = true,
                actions = listOf(action),
            )
        }
        // The CHANNELS are carried through untouched. This function edits
        // bindings, and a binding names its own channel, so the two are
        // independent -- trimming the list to `firstOrNull()` here would send
        // back a config whose SWC2 channel is gone, replacing the device's
        // learned second ladder with nothing on every save.
        //
        // The EDITS come FIRST, ahead of everything kept, because
        // `BindingResolve` returns the FIRST match -- so a binding's POSITION is
        // its precedence. An `ANY` binding fires from either channel (spec 3.5),
        // and `kept` retains one; if it sat ahead of the edit it would win, and
        // the channel-specific binding the user just made would be silently
        // ineffective. Appending the edits puts the more specific binding ahead of
        // the wildcard, which is the precedence the user expects from editing one
        // channel's cell. (For the ordinary case -- no `ANY` binding for the
        // triple -- `kept` holds nothing that could shadow the edit, so the order
        // is immaterial; it matters only here.)
        return config.copy(bindings = added + kept)
    }

    /**
     * Whether [buttonId] is on the ladder [channel] names.
     *
     * Only SWC1/SWC2 have ladders, so those are the two channels this can answer
     * for; an AUX button is a separate table (`cfg.aux`) and never appears in the
     * bindings grid, so it never reaches here. A channel with no ladder (an index
     * past the two, or AUX) answers false.
     */
    private fun buttonOnThatLadder(config: Config, channel: BindingChannel, buttonId: String): Boolean {
        val index = when (channel) {
            BindingChannel.SWC1 -> 0
            BindingChannel.SWC2 -> 1
            else -> return false
        }
        val ch = config.channels.getOrNull(index) ?: return false
        return ch.ladder.buttons.any { it.id == buttonId }
    }

    /**
     * Report what this build can honestly say about updates.
     *
     * **It does NOT check anything, and it must not pretend to.** The previous
     * version reported `UpToDate(version)` whenever a device was connected — a
     * comparison against nothing. The screen then said "This device is running
     * X, which is the current release", which is a claim about the world that no
     * code verified: the app has no manifest client and no `INTERNET` permission,
     * so it has never seen a release manifest.
     *
     * Spec §9.5 says the app "should also be able to perform the check over its own
     * internet connection" and push the result over USB, precisely because the
     * ESP32 may have no WiFi in the car. That is not implemented — it needs a
     * pinned-CA TLS client and the INTERNET permission — and it is recorded as open
     * item N-12. Until then the truthful report is that this build cannot check,
     * rather than a green "up to date" that would stop a user looking for an update
     * that does exist.
     *
     * The firmware CAN check, over WiFi in maintenance mode, which is what the next
     * line points at.
     */
    fun checkForUpdates() {
        scope.launch {
            _update.value = _update.value.copy(inProgress = true)
            val version = _link.value.firmwareVersion
            _update.value = _update.value.copy(
                inProgress = false,
                currentVersion = version ?: "—",
                status = when {
                    version == null -> UpdateStatus.Failed("No device is connected.")
                    else -> UpdateStatus.Failed(
                        "This build has no release-manifest client, so it cannot check " +
                            "for updates (spec §9.5). Open the device's maintenance page " +
                            "over WiFi to check there."
                    )
                },
            )
        }
    }

    fun close() = client.close()

    private companion object {
        /** How many device log lines the link screen keeps. */
        const val kMaxLogLines = 20
    }
}
