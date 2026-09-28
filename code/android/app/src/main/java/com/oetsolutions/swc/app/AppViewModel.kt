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
import com.oetsolutions.swc.ui.PushResult
import com.oetsolutions.swc.ui.UpdateStatus
import com.oetsolutions.swc.ui.UpdateUiState
import com.oetsolutions.swc.update.ImageDownloader
import com.oetsolutions.swc.update.ManifestCheck
import com.oetsolutions.swc.update.ManifestFetcher
import com.oetsolutions.swc.update.ReleaseDecision
import com.oetsolutions.swc.update.ReleaseManifest
import com.oetsolutions.swc.update.kReleaseManifestUrl
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.doubleOrNull
import kotlinx.serialization.json.int
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.longOrNull

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
     * Whether [scope] is OURS to cancel in [close].
     *
     * True for the default (a scope this class created), false for an injected one
     * (every test passes its own on the test scheduler, and cancelling a scope the
     * caller owns would be a surprising side effect). This is what makes [close]
     * able to stop the collectors without reaching into someone else's job.
     */
    private val ownsScope: Boolean = true,
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

    /**
     * Fetches the release manifest over the phone's own connection (spec §9.5,
     * open item N-12). Null means this build cannot check — a test, a preview, a
     * build without the permission — and `checkForUpdates` says so rather than
     * claiming the device is current.
     *
     * Injected so the whole decision path is JVM-testable with no network: the real
     * app passes `HttpManifestFetcher`, every test passes a fake.
     */
    private val fetchManifest: ManifestFetcher? = null,

    /** The manifest URL, overridable so a test is never coupled to the real host. */
    private val manifestUrl: String = kReleaseManifestUrl,

    /**
     * Where the manifest fetch runs. Injected so a JVM test can keep the whole
     * check on its virtual clock: `Dispatchers.IO` is not a `TestDispatcher`, so a
     * hard-coded IO dispatch would let `advanceUntilIdle()` return before the fetch
     * completed and make the update tests flaky. Defaults to IO, which is what the
     * real app wants for a network round trip.
     */
    private val ioDispatcher: kotlinx.coroutines.CoroutineDispatcher = Dispatchers.IO,

    /**
     * Downloads a released image (spec §9.5 step 5) for [installAvailableUpdate] to
     * verify and push. Null means this build does not install from a release — a
     * test, a preview — and the install path is a no-op rather than a crash.
     */
    private val downloadImage: ImageDownloader? = null,

    /**
     * The wall clock, in milliseconds since the epoch, for stamping `updated_at_ms`
     * on a config the user edits (spec N-28).
     *
     * **The app is the only side that CAN stamp it.** The firmware has no RTC — its
     * own clock is `now_ms`, an uptime — so a device-authored stamp would be a boot
     * counter, not a time. Android has a real wall clock, so the app is the one
     * writer that can give the field the meaning its name promises. The field was
     * declared, decoded and re-encoded while nothing ever assigned it, so a config
     * edited many times still reported the timestamp it arrived with, or `0`.
     *
     * Injected so a JVM test can assert the stamp without depending on the real
     * clock. The default is the production one.
     */
    private val nowMs: () -> Long = { System.currentTimeMillis() },
) {

    private val _actionOutcomes = MutableStateFlow<List<String>>(emptyList())

    /**
     * App-side actions that could not run, stated so the user can act.
     *
     * This is the surface for spec 3.6's Android BAL limitation: an app launched
     * from the background may be refused by the system (hence `targetSdk` 34), and
     * "the button did nothing" is the outcome `ActionOutcome` exists to prevent.
     *
     * **Mirrored into [LinkUiState.actionProblems], which is what the screen
     * renders.** This flow existed and nothing collected it: no screen and not
     * `MainActivity` referenced it, so an app-side action that could not run was
     * computed, classified, worded, and then dropped on the floor -- the exact
     * "the button did nothing" outcome the whole `ActionOutcome` type exists to
     * prevent, silently reintroduced at the last step. `AppViewModel`'s own
     * doc-comment claimed the four screens consume it. It is the same shape as
     * N-24 (`UsbCdc::dropped_`, counted and never reported) and N-22 (`rail_mv`,
     * read with no producer): a diagnostic that terminates inside the class that
     * produced it reports nothing.
     *
     * The flow stays the single home for the OUTCOME; the link state carries a
     * copy for rendering, written by the collector in `init`.
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

    /**
     * The release the last check found to be installable, or null.
     *
     * Held so [installAvailableUpdate] has the url/size/sha256 to download and
     * verify against. It is cleared by any check that does not end in NEWER, so a
     * stale release cannot be installed after a later check found none — the same
     * "the state you act on must be the state you last observed" rule the rest of
     * this class follows.
     */
    private var _availableRelease: com.oetsolutions.swc.update.ReleaseInfo? = null

    init {
        // `run()` never returns: it is the transport's only consumer, and the
        // client throws if the flow completes because a USB link must not end
        // while the app is alive. Launching it here means every frame the device
        // sends is routed exactly once.
        scope.launch { client.run() }
        scope.launch { client.state.collect { _link.value = linkStateFor(it) } }
        scope.launch { client.frames.collect { onFrame(it) } }
        scope.launch { client.config.collect { onConfig(it) } }
        // The app-side failure surface, mirrored into the link state the Link
        // screen renders. Without this the flow was produced and read by nothing.
        scope.launch { actionOutcomes.collect { outcomes ->
            _link.value = _link.value.copy(actionProblems = outcomes)
        } }
        // The keepalive loop is NOT started here. It is a periodic timer, and a
        // driver launched on this scope would be a never-completing `delay` loop in
        // `init` -- which makes every `advanceUntilIdle()` in the JVM suite spin
        // forever, since a virtual clock never runs out of future tasks. The
        // mechanism ([SwcClient.SilenceTick]) is unit-tested directly, the driver
        // ([SwcClient.DriveLiveness]) is tested with a virtual clock, and the
        // lifecycle that owns the periodic loop is `MainActivity.onCreate`.
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

    /**
     * Try again: RE-ENUMERATE the USB bus, then connect.
     *
     * **It used to be `connect()` alone, which could not recover anything.** The
     * transport was opened once, in `MainActivity.onCreate`, and nothing ever
     * enumerated again — there is no `ACTION_USB_DEVICE_ATTACHED` receiver. So an
     * app opened BEFORE the adapter was plugged in showed "No device found"
     * permanently, and "Try again" only sent a `ping` through a transport with no
     * connection (`write` returns early when `connection == null`), i.e. it wrote
     * nothing and re-reported the same problem forever.
     *
     * Opening here rather than in `onCreate` is also what makes the retry do real
     * work: the user plugging the adapter in and tapping the button now enumerates
     * the device that appeared.
     */
    fun retry() {
        scope.launch {
            try {
                reportOpenProblem(client.reopen())
            } catch (e: Exception) {
                _link.value = _link.value.copy(problem = LinkProblem.NoDevice)
                return@launch
            }
            connect()
        }
    }


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

    /**
     * Turn the per-press "click" on or off and persist it to the device.
     *
     * The click is a device SETTING (`settings.key_click_enabled`), off by default.
     * It is written immediately — unlike a binding edit it has no Save step, so the
     * user hears the change as soon as they toggle rather than after a second
     * action. The optimistic UI state is reverted to the device's answer by the
     * config push that a successful set triggers.
     */
    fun setKeyClick(enabled: Boolean) {
        scope.launch {
            // Show the switch moved immediately; a failed write re-renders from the
            // device's config (`onConfig`), so the toggle cannot lie for long.
            _bindings.value = _bindings.value.copy(keyClickEnabled = enabled)
            val current = client.config.value
            val merged = current.copy(
                settings = current.settings.copy(keyClickEnabled = enabled),
            )
            try {
                saveConfig(merged)
            } catch (e: Exception) {
                // Leave the optimistic state; the next config push corrects it.
            }
        }
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
            // The reason is CARRIED, not collapsed onto NoDevice. "Failed" is the
            // state for a device that is enumerated and answering but whose
            // conversation broke -- a torn config run, a digest mismatch, an
            // over-long line. Mapping it onto NoDevice told the user to check a
            // cable that was working, and gave them no way to tell a flaky transfer
            // from a genuinely absent device.
            is LinkState.Failed -> LinkProblem.LinkFailed(state.reason)
            // The app's own liveness finding (spec §4.4, N-27). Its own problem
            // rather than `LinkFailed`: the conversation did not fail, it stopped,
            // and the fix is a cable or a power cycle rather than a retry.
            LinkState.SilenceExpired -> LinkProblem.SilenceExpired
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
                // The LIVE idle the device normalized `level_mv` against (spec
                // 6.3's `V_ADC_idle`). This is the denominator `LadderClassify`
                // actually used, so the view can reproduce the device's decision
                // as a ratio instead of comparing absolute millivolts against the
                // config's learn-TIME rail (open item N-25). Read as nullable:
                // a frame from a build that predates the field still parses, and
                // the view falls back to the config's idle.
                val idle = frame.fields["idle_mv"]?.jsonPrimitive?.intOrNull
                val gesture = frame.fields["gesture"]?.jsonPrimitive?.content
                val channel = frame.fields["channel"]?.jsonPrimitive?.intOrNull
                if (gesture != null && level != null) {
                    // The app-side outcome describes THIS press, so it is cleared
                    // here -- at the one place a press arrives -- rather than
                    // inside `runAppSideAction`, which the unrecognized-press
                    // branch below never reaches. Clearing only there would leave
                    // a failed action's warning on the Link screen through every
                    // subsequent press the device could not attribute to a
                    // button, and the user has no way to tell when it stopped
                    // being true.
                    _actionOutcomes.value = emptyList()
                    // The WHOLE view follows the pressing channel: its bands, its
                    // rail and its name together. The screen is a single-channel
                    // diagnostic, and an event carries ONE channel's reading, so a
                    // view that relabeled itself (as this once did) while still
                    // drawing channel 0's bands and rail would plot SWC2's reading
                    // against SWC1's scale and `matched()` it against SWC1's
                    // windows -- reporting the wrong button for a perfectly healthy
                    // second wheel. The label and the bands must move together or
                    // neither.
                    //
                    // **Only a WHEEL channel has a ladder to plot.** An AUX press
                    // (wire channels 2/3/4) is a switch, not a ladder: it has no
                    // learned windows and no rail to scale against, so feeding its
                    // level into this view would plot a ~100 mV switch closure
                    // against the wheel's ~2835 mV idle and draw every band in the
                    // wrong place. A frame the firmware could not name a channel
                    // for is the same case. The AUX press is still fully handled
                    // below (its app-side actions run); only the BANDS AND RAIL are
                    // left alone.
                    //
                    // The READING still updates either way: `level_mv` is real
                    // whatever the channel, and leaving the view frozen at its last
                    // value would look like a device that stopped seeing presses.
                    val shown = if (channel != null && channel < 2)
                        ladderFor(client.config.value, channel) else null
                    val next = _ladder.value.copy(liveMv = level, liveIdleMv = idle)
                    _ladder.value = if (shown == null) next else next.copy(
                        idleMv = shown.idleMv,
                        channelName = shown.channelName,
                        buttons = shown.buttons,
                    )
                    // The gesture label always follows, for the same reason as the
                    // reading: it describes the press, not the scale.
                    _ladder.value = _ladder.value.copy(
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
            //
            // The frame ALSO carries the run's `channel` (spec 4.3), and the view
            // follows it exactly as it follows an `event`'s. Ignoring it was the
            // same wrong-scale hazard as the `event` path had: a learn run on
            // channel 1 streamed its readings into a view still scaled to channel
            // 0's rail and buttons -- and this is the screen the user watches WHILE
            // learning, so a mis-scaled band there is what makes them think their
            // press landed on the wrong button. The channel is read as this frame's
            // own field, never inherited from the last `event`.
            Frames.LADDER_SAMPLE -> {
                val level = frame.fields["level_mv"]?.jsonPrimitive?.intOrNull
                val channel = frame.fields["channel"]?.jsonPrimitive?.intOrNull
                if (level != null) {
                    // A 0 is the device saying "NO READING", not a millivolt value.
                    // `EmitLadderSample` emits 0 when there is no orchestrator to
                    // sample (`sys_ == nullptr`) and `FilteredLevelMv` returns 0 for
                    // an unreadable or stale conversion, and its comment states the
                    // contract: "0 mV is unambiguous: it is below the ladder's floor,
                    // so the app renders it as 'no reading' rather than as a real
                    // level." This branch did the opposite -- `level > 0` dropped the
                    // frame, so the LAST real reading stayed on screen and the user
                    // watching during a learn saw a number the device had already
                    // abandoned. Blanking the reading is what the producer intends and
                    // what the screen renders as "Reading: —".
                    val shown = ladderFor(client.config.value, channel)
                    _ladder.value = _ladder.value.copy(
                        liveMv = if (level > 0) level else null,
                        idleMv = shown.idleMv,
                        channelName = shown.channelName,
                        buttons = shown.buttons,
                    )
                }
            }

            // Spec 4.3's `status`, emitted every 2 s while connected. It carries
            // the CONFIG's state, which spec 4.3 says exists so a config fault has
            // "a name the app could read" (§6.8's `config_state: defaults`).
            //
            // The two diagnostic fields this branch used to lack now arrive:
            // `temp_c` (the last good NTC reading, as a decimal; JSON null when
            // nothing has been measured) and `heap_free`. Neither has a screen
            // yet, so they are recorded on the link state rather than dropped --
            // a value the transport delivers and the app forgets is the same
            // "produced, consumed by nobody" shape the link counters were (N-24).
            // The `rail_mv` this branch used to read stays gone: it was the +3V3
            // rail with no producer, and the quantity the ladder view needs is
            // the LIVE idle, which now arrives per press on `event.idle_mv`.
            // Spec 4.3's `maintenance`: the device's own report of its window, and
            // the ONLY delivery path for the two per-device secrets (spec 8.3
            // option 1). The board has no display and no printed label, so the BLE
            // Proof-of-Possession and the page token are derived from its MAC and
            // shown HERE, over the already-trusted USB link, for the user to type
            // into the Espressif app or to open the setup page with.
            //
            // The frame is the authority on whether the window is open, not the
            // app's own enter/exit request: the device also opens the window on a
            // 3 s AUX1 hold, which no app request produced. The request's
            // optimistic set stays, so the button responds immediately; this
            // corrects it if the device disagreed.
            //
            // Every field is empty when `active` is false, so a closed window
            // cannot leave a stale secret on screen -- which is why they are
            // assigned rather than merged.
            Frames.MAINTENANCE -> {
                val active = frame.fields["active"]?.jsonPrimitive?.booleanOrNull ?: false
                _link.value = _link.value.copy(
                    maintenanceOpen = active,
                    maintenancePop = frame.fields["pop"]?.jsonPrimitive?.content.orEmpty(),
                    maintenanceToken = frame.fields["token"]?.jsonPrimitive?.content.orEmpty(),
                    maintenancePageUrl = frame.fields["page_url"]?.jsonPrimitive?.content.orEmpty(),
                    maintenanceBleName = frame.fields["ble_name"]?.jsonPrimitive?.content.orEmpty(),
                    maintenanceTrigger =
                        frame.fields["trigger"]?.jsonPrimitive?.content.orEmpty(),
                    maintenanceBleFailures =
                        frame.fields["ble_failures"]?.jsonPrimitive?.intOrNull ?: 0,
                    // A frame that reports the window closed clears any stale
                    // "the device refused" message from an earlier attempt.
                    maintenanceProblem = if (active) _link.value.maintenanceProblem else null,
                )
            }
            Frames.STATUS -> {
                val cs = frame.fields["config_state"]?.jsonPrimitive?.content
                if (cs != null) _link.value = _link.value.copy(configState = cs)
                // `temp_c` is nullable on purpose: absent and JSON-null both mean
                // "no reading", and neither is 0 C. `JsonNull.jsonPrimitive` is a
                // JsonPrimitive whose `doubleOrNull` is null, so a JSON null needs
                // no special case here -- it falls through exactly like an absent
                // field.
                frame.fields["temp_c"]?.jsonPrimitive?.doubleOrNull?.let { t: Double ->
                    _link.value = _link.value.copy(lastTempC = t)
                }
                frame.fields["heap_free"]?.jsonPrimitive?.longOrNull?.let { h: Long ->
                    _link.value = _link.value.copy(lastHeapFree = h)
                }
                // The DEVICE-side loss counters, spec 4.3's `status`. `lostFrames`
                // above counts `link_gap`, which the device reports when the APP's
                // frame went missing; these two are the other direction -- frames
                // the device's OWN transport refused, inbound (staging-ring
                // overflow) and outbound (TX buffer full). Both were counted and
                // read by no one: the counter had a unit test and nothing else, so
                // a refused command still vanished with no explanation on either
                // end. Reported here rather than in its own `link_gap`-style frame
                // because `status` already arrives every 2 s, which is the app's
                // one guaranteed look at the link without asking for it.
                frame.fields["tx_dropped"]?.jsonPrimitive?.int?.let {
                    _link.value = _link.value.copy(deviceTxDropped = it)
                }
                frame.fields["rx_overflows"]?.jsonPrimitive?.int?.let {
                    _link.value = _link.value.copy(deviceRxOverflows = it)
                }
            }

            // Spec 4.3's `link_gap`: the DEVICE lost one of the app's outgoing
            // frames. The firmware tracks the app's `seq` and emits this when it
            // skips ahead, so a dropped frame is not indistinguishable from a
            // command the device ignored. This branch did not exist -- `link_gap`
            // was the one inbound frame type the app had no handler for, while it
            // DID define the constant -- so a lost `config_chunk` or `learn_commit`
            // vanished: the transfer failed and nothing said the bytes never
            // arrived, which is the "detected but not reported" failure the frame
            // was added to prevent.
            //
            // Counted rather than overwritten: gaps accumulate, and a cable that
            // drops one frame in a thousand will drop several over a session. The
            // count is what tells the user their link is unreliable rather than
            // their device.
            Frames.LINK_GAP -> {
                val n = _link.value.lostFrames + 1
                _link.value = _link.value.copy(lostFrames = n)
            }
        }
    }

    /**
     * The ladder the live view should draw: the named channel's, or the first
     * channel's when no channel is named.
     *
     * The [LadderUiState] is single-channel by construction (one rail, one button
     * list), so "which channel is on screen" is one choice made in one place
     * rather than three fields that can disagree. `onConfig` shows the first
     * channel; an `event` switches to the channel it came from. An index that
     * names no channel leaves the last good ladder on screen rather than blanking
     * it for a frame the firmware could not name.
     */
    private fun ladderFor(config: Config, index: Int?): LadderShown {
        val channels = config.channels
        val at = index ?: 0
        val chosen = channels.getOrNull(at)
            ?: return LadderShown(
                idleMv = _ladder.value.idleMv,
                channelName = _ladder.value.channelName,
                buttons = _ladder.value.buttons,
            )
        return LadderShown(
            idleMv = chosen.ladder.learnedIdleMv,
            channelName = chosen.name.ifEmpty { "SWC${at + 1}" },
            buttons = chosen.ladder.buttons.map { b: LadderButton ->
                LearnedButton(id = b.id, name = b.name.ifEmpty { b.id },
                    mvCenter = b.mvCenter, mvTolerance = b.mvTolerance,
                    learnedAtRailMv = b.learnedAtRailMv)
            },
        )
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
        // The outcome describes THIS press. A press with nothing app-side to run
        // clears it, rather than leaving the previous press's failure on the Link
        // screen indefinitely -- see the clear at the `EVENT` branch, which is
        // the one place a press arrives.
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
            // An AUX press names its input AFTER the two wheel channels: the
            // firmware encodes AUX1..3 as `kAuxWireChannelBase + index`, i.e.
            // 2/3/4. That base is `kMaxChannels` (SystemOrchestrator.h), and the
            // firmware's `ServiceAux` only ever emits AUX2 (3) and AUX3 (4) as
            // gesture events -- AUX1 is the programming hold and is not a gesture
            // source. Decoding all three keeps the app's map the inverse of the
            // firmware's, rather than silently mismatching an index.
            in 2..4 -> when (channelIndex) {
                2 -> BindingChannel.AUX1
                3 -> BindingChannel.AUX2
                else -> BindingChannel.AUX3
            }
            // An unknown or absent channel cannot be matched against a binding's
            // channel field without guessing. Anything else matches nothing but
            // `ANY`.
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
            is ActionOutcome.Privileged ->
                "${outcome.command} needs root, an accessibility service, or a system-app " +
                    "install on this head unit"
        }
    }


    /**
     * The three fields of [LadderUiState] that describe WHICH ladder is on screen.
     *
     * They travel as one value because they must move together: a view that took
     * the name from one channel and the bands from another would draw a reading
     * against the wrong scale. [ladderFor] is the only producer.
     */
    private data class LadderShown(
        val idleMv: Int,
        val channelName: String,
        val buttons: List<LearnedButton>,
    )

    /**
     * The device's config became known (from a `config_get` reply run).
     *
     * This is what turns the empty screens into real ones: the ladder view gets
     * the learned buttons, and the bindings grid gets one cell per button×gesture
     * that the config actually binds.
     */
    private fun onConfig(config: Config) {
        val shown = ladderFor(config, null)
        _ladder.value = _ladder.value.copy(
            idleMv = shown.idleMv,
            channelName = shown.channelName,
            buttons = shown.buttons,
        )
        _bindings.value = _bindings.value.copy(
            cells = buildCells(config),
            problems = ConfigJson.problems(config).map { it.toString() },
            keyClickEnabled = config.settings.keyClickEnabled,
        )
        // The maintenance card names the window's length, and the window is a
        // SETTING (`maintenance_timeout_ms`), not a constant — so the card must
        // read the device's own value rather than a "5 minutes" literal. A device
        // configured for 20 minutes otherwise tells the user 5.
        _link.value = _link.value.copy(
            maintenanceTimeoutMs = config.settings.maintenanceTimeoutMs,
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
        val ladderCells = config.channels.flatMapIndexed { index, channel ->
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
        // The AUX gesture inputs (spec 3.1/3.5). AUX2 and AUX3 are bindable; AUX1
        // is NOT, because it carries the programming (1.5 s) and maintenance (3 s)
        // holds (spec 7.5/8.2) and the firmware's validator refuses a binding on
        // it. Offering an AUX1 cell would let the user make an edit the device
        // rejects -- so the grid starts at index 1, the same place the firmware's
        // `ServiceAux` starts.
        //
        // Each AUX input carries exactly ONE button (its own id), so unlike a
        // ladder there is no per-button fan-out: three cells, one per gesture.
        val auxCells = config.aux.drop(1).flatMap { a ->
            val channel = auxChannelFor(config, a.id) ?: return@flatMap emptyList()
            gestures.map { g ->
                val key = editKey(channel, a.id, g.wireName)
                val edit = pendingEdits[key]
                BindingCell(
                    channel = channel,
                    buttonId = a.id,
                    buttonName = a.id,
                    gesture = g.wireName,
                    action = if (pendingEdits.containsKey(key)) edit
                    else resolvedBindingFor(config, channel, a.id, g)?.actions?.firstOrNull(),
                )
            }
        }
        return ladderCells + auxCells
    }

    /**
     * The binding channel an AUX input id names, or null when the config's `aux`
     * table does not hold it.
     *
     * The index in `cfg.aux` is what selects AUX1/AUX2/AUX3 (spec 3.1), so the
     * lookup is by identity of the entry, not by parsing its id.
     */
    private fun auxChannelFor(config: Config, auxId: String): BindingChannel? {
        val at = config.aux.indexOfFirst { it.id == auxId }
        return when (at) {
            0 -> BindingChannel.AUX1
            1 -> BindingChannel.AUX2
            2 -> BindingChannel.AUX3
            else -> null
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

    /**
     * The binding `id` for a triple the user just authored: spec 3.5's own `bN`.
     *
     * **An ordinal slug, deliberately NOT derived from the button id.** `Binding.id`
     * is `char[kBindingIdLen]` (16) in `ConfigModel.h` and `ConfigJson.problems()`
     * refuses anything at or over that width. Building it as
     * `"${button}-${gesture}"` from a headless-learned button id -- `swc1_bt10`,
     * which is the only production learn vocabulary (the app has no learning
     * screen) -- came to exactly 16 characters, so the app refused its OWN edit
     * ("id must be under 16 chars") and Save was disabled: the user could not bind
     * those buttons at all.
     *
     * Truncating the button to fit is worse than an ordinal, because it LIES: slot
     * 10's `swc1_bt10` truncated to the last 8 characters is `swc1_bt1`, an id that
     * reads as slot 1 -- and no length-preserving scheme survives a button id that
     * is itself 15 characters.
     *
     * Nothing READS a binding id: the firmware resolves on
     * `channel`/`button`/`gesture` (`BindingResolve` never looks at `id`), and the
     * app's grid keys on the same triple. So a collision with a device-supplied id
     * in the kept list is inert, which is what makes an ordinal a safe label rather
     * than a key.
     */
    private fun bindingIdFor(ordinal: Int): String = "b$ordinal"

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
                // A VALIDATION refusal, so it gates Save -- the user can fix it by
                // editing a cell.
                _bindings.value = _bindings.value.copy(
                    problems = problems.map { it.toString() },
                    saveError = null,
                )
                return@launch
            }
            val ok = try {
                // Stamp the edit time (spec N-28): the app is the only side with a
                // real wall clock, so it is the one writer that can make
                // `updated_at_ms` mean what its name says. Stamped BEFORE the save
                // so the value that reaches the device is the one the user's edit
                // produced; a config the app never edits keeps whatever stamp it
                // arrived with.
                saveConfig(merged.copy(updatedAtMs = nowMs()))
            } catch (e: Exception) {
                false
            }
            if (ok) {
                pendingEdits.clear()
                _bindings.value = _bindings.value.copy(problems = emptyList(), saveError = null)
            } else {
                // A RUNTIME failure. It goes to `saveError`, NOT to `problems`.
                //
                // It used to be written into `problems` -- the same field the
                // screen gates Save on -- so a device nack or a link timeout
                // disabled the button. The pending edit was still rendered in its
                // cell, so the change looked live while the device held the old
                // bindings, and there was no way to retry: the config was fine and
                // the failure was transient, but the only recovery was to edit an
                // unrelated cell. A save failure must be visible AND retryable.
                _bindings.value = _bindings.value.copy(
                    problems = emptyList(),
                    saveError = "The device did not accept the configuration. " +
                        "Nothing was changed on the device; try again.",
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
        var emitted = 0
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
            // does not hold the id is never found: an SWC2 edit of a button that
            // exists only on SWC1 would be DEAD on the device.
            //
            // The firmware does NOT catch this -- `BindingNamesARealInput` accepts
            // any id that is on ANY channel's ladder (ConfigCodec.cpp), so the save
            // would SUCCEED and the binding would simply never fire. We drop the
            // edit instead of writing a binding we know can never resolve. (Note
            // the asymmetry with the STALE case below, where the id is on no ladder
            // at all and the firmware genuinely does refuse.)
            //
            // `vol_up` is on BOTH ladders (the common case), so the check must be
            // per-channel or it drops a legitimate second-channel edit.
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
                // An ordinal label, never derived from the button id -- see
                // [bindingIdFor] for the headless-learned id that made the old
                // derivation a self-refusal. The counter is `kept` plus the edits
                // emitted so far, so the ids stay distinct within one save.
                id = bindingIdFor(kept.size + emitted),
                channel = channel,
                button = button,
                gesture = gesture,
                enabled = true,
                actions = listOf(action),
            ).also { ++emitted }
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
     * Whether [buttonId] is on the input [channel] names.
     *
     * For SWC1/SWC2 that is the channel's ladder; for AUX1/AUX2/AUX3 it is that
     * entry of `cfg.aux`, whose id is the input's single button. A channel with no
     * such input answers false.
     *
     * The firmware's `BindingNamesARealInput` accepts an id that is on ANY channel
     * or in ANY AUX entry, so it does not enforce that a binding's button belongs
     * to the input its channel names; a mismatch would be stored and then never
     * resolve. This is the stricter, per-input check the save relies on.
     */
    private fun buttonOnThatLadder(config: Config, channel: BindingChannel, buttonId: String): Boolean {
        val index = when (channel) {
            BindingChannel.SWC1 -> 0
            BindingChannel.SWC2 -> 1
            BindingChannel.AUX1, BindingChannel.AUX2, BindingChannel.AUX3 -> {
                val at = channel.ordinal - BindingChannel.AUX1.ordinal
                return config.aux.getOrNull(at)?.id == buttonId
            }
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
     * ESP32 may have no WiFi in the car. **That is implemented (N-12 resolved
     * 2026-09-24):** [fetchManifest] is a [ManifestFetcher] (a pinned-CA TLS client
     * over `HttpURLConnection`, `INTERNET` declared), and [checkForUpdates] below
     * fetches, decides with [ReleaseManifest]'s Kotlin semver, and maps the outcome
     * to the screen. The `fetcher == null` branch is not the normal path — it is the
     * case where a caller constructed this without a fetcher (as a unit test does),
     * and it refuses truthfully rather than showing a green "up to date".
     *
     * The firmware CAN also check, over WiFi in maintenance mode, which is what the
     * "could not reach the release server" message points at.
     */
    fun checkForUpdates() {
        scope.launch {
            val version = _link.value.firmwareVersion
            if (version == null) {
                _update.value = _update.value.copy(
                    currentVersion = "—",
                    status = UpdateStatus.Failed("No device is connected."),
                    inProgress = false,
                )
                return@launch
            }
            val fetcher = fetchManifest
            if (fetcher == null) {
                // No client in this build. Truthful refusal, never a green "up to
                // date" that would hide a real update -- see the class note.
                _update.value = _update.value.copy(
                    currentVersion = version,
                    status = UpdateStatus.Failed(
                        "This build cannot check for updates (no release client). " +
                            "Open the device's maintenance page over WiFi to check there."
                    ),
                    inProgress = false,
                )
                return@launch
            }

            // Real work now, so the in-progress gate is live: set before the fetch,
            // cleared in `finally` so a thrown transport error cannot leave the
            // button disabled forever.
            _update.value = _update.value.copy(
                currentVersion = version,
                status = UpdateStatus.Unknown,
                inProgress = true,
            )
            try {
                val body = fetchManifestBody(fetcher)
                _update.value = _update.value.copy(
                    status = statusFor(version, body),
                    inProgress = false,
                )
            } catch (e: Exception) {
                // The device is unaffected and keeps running whatever it was: the
                // failure is the CHECK, and the screen says exactly that.
                _update.value = _update.value.copy(
                    status = UpdateStatus.Failed(
                        "Could not reach the release server: " +
                            (e.message ?: e.javaClass.simpleName)
                    ),
                    inProgress = false,
                )
            }
        }
    }

    /**
     * Fetch on IO, decide on the caller's dispatcher. `Dispatchers.IO` because the
     * fetch is a network round trip and this runs from the UI; the parse and compare
     * are cheap and stay where the state lives.
     */
    private suspend fun fetchManifestBody(fetcher: ManifestFetcher): String =
        kotlinx.coroutines.withContext(ioDispatcher) { fetcher.fetch(manifestUrl) }

    /** Map a manifest body to the screen's status, folding the malformed case in. */
    private fun statusFor(current: String, body: String): UpdateStatus =
        when (val check = ReleaseManifest.check(body, current)) {
            ManifestCheck.Malformed -> {
                _availableRelease = null
                UpdateStatus.Failed(
                    "The release manifest could not be read. The device is unaffected."
                )
            }
            is ManifestCheck.Decided -> {
                // Remembered ONLY for an installable release, and cleared otherwise,
                // so `installAvailableUpdate` can never act on a release a later
                // check found is not being offered.
                _availableRelease =
                    if (check.decision == ReleaseDecision.NEWER) check.info else null
                when (check.decision) {
                    ReleaseDecision.UP_TO_DATE -> UpdateStatus.UpToDate(current)
                    ReleaseDecision.NEWER ->
                        UpdateStatus.Newer(current, check.info.latestVersion)
                    ReleaseDecision.NOT_NEWER -> UpdateStatus.NotNewer(
                        current, check.info.latestVersion
                    )
                    ReleaseDecision.TOO_OLD_TO_UPGRADE_FROM -> UpdateStatus.TooOldToUpgradeFrom(
                        current, check.info.latestVersion, check.info.minFromVersion
                    )
                    // Unreachable: a malformed manifest is `ManifestCheck.Malformed`
                    // above, never a `Decided`. Listed so adding a decision is a
                    // compile error here rather than a silent fallthrough.
                    ReleaseDecision.MALFORMED -> UpdateStatus.Failed(
                        "The release manifest could not be read. The device is unaffected."
                    )
                }
            }
        }

    /**
     * Push a firmware image over USB (spec §9.3). `image` is the file the user
     * picked; [UpdateUiState.pushResult] carries the outcome back to the screen.
     *
     * The push is guarded so a second tap cannot open a second run the device
     * would refuse as `run_open` — the same shape as `maintenanceBusy`. Progress
     * comes from the client's per-chunk callback, so the bar reflects real acks
     * rather than a timer.
     *
     * A `not_supported` refusal is reported distinctly: a host/dev image has no
     * partitions to write, so the honest message is "this device cannot install
     * over USB", not a generic failure.
     */
    fun pushFirmwareOverUsb(image: ByteArray) {
        if (_update.value.pushInProgress) return
        scope.launch { pushImage(image) }
    }

    /**
     * Install the release just found, over USB (spec §9.5's full app path: check,
     * download, verify, then push the resulting file over USB).
     *
     * **The image is verified BEFORE any of it reaches the device**, per §9.5 step
     * 3: `sha256` and `size_bytes` are checked against the manifest that was
     * checked. A download that does not match is discarded whole — the device's own
     * `ImageVerify` gate would refuse it too, but refusing here means a substituted
     * or truncated image never occupies the link at all, and the message names the
     * cause (a bad hash) rather than the device's vaguer "verify failed".
     *
     * Does nothing if no release was found or one is already installing.
     */
    fun installAvailableUpdate() {
        if (_update.value.pushInProgress) return
        val release = _availableRelease ?: return
        val downloader = downloadImage ?: return
        scope.launch {
            _update.value = _update.value.copy(
                pushInProgress = true,
                pushSent = 0,
                // Unknown until the download lands; the manifest's figure is a good
                // pre-download estimate and is corrected below.
                pushTotal = release.sizeBytes.toInt(),
                pushResult = null,
            )
            val image = try {
                kotlinx.coroutines.withContext(ioDispatcher) { downloader.download(release.url) }
            } catch (e: Exception) {
                _update.value = _update.value.copy(
                    pushInProgress = false,
                    pushResult = PushResult.Failed(
                        "could not download the update: " + (e.message ?: e.javaClass.simpleName)
                    ),
                )
                return@launch
            }
            // Size first, then digest: a truncated download then reports the
            // specific cause rather than the vaguer checksum failure -- the same
            // order the device's own `ImageVerifyEnd` uses.
            if (image.size.toLong() != release.sizeBytes) {
                _update.value = _update.value.copy(
                    pushInProgress = false,
                    pushResult = PushResult.Failed(
                        "the downloaded image was ${image.size} bytes, not the " +
                            "${release.sizeBytes} the release declares"
                    ),
                )
                return@launch
            }
            if (sha256Hex(image) != release.sha256.lowercase()) {
                _update.value = _update.value.copy(
                    pushInProgress = false,
                    pushResult = PushResult.Failed(
                        "the downloaded image did not match the release's checksum"
                    ),
                )
                return@launch
            }
            _update.value = _update.value.copy(pushTotal = image.size)
            pushImage(image)
        }
    }

    /**
     * The push itself, shared by the file picker and the release download.
     *
     * Assumes the caller has already set `pushInProgress` and the totals, so the
     * two entry points do not each re-derive the progress bookkeeping; it owns only
     * the transfer and its outcome.
     */
    private suspend fun pushImage(image: ByteArray) {
        _update.value = _update.value.copy(
            pushInProgress = true,
            pushSent = 0,
            pushTotal = image.size,
            pushResult = null,
        )
        val result = try {
            client.pushFirmware(image, onProgress = { sent, _ ->
                _update.value = _update.value.copy(pushSent = sent)
            })
        } catch (e: Exception) {
            AckResult.Nacked("link", e.message ?: "the link failed")
        }
        _update.value = _update.value.copy(
            pushInProgress = false,
            pushResult = when (result) {
                is AckResult.Ok -> PushResult.Installed
                is AckResult.Nacked -> when (result.err) {
                    "not_supported" ->
                        PushResult.Refused("this build has no update slot to write")
                    else -> PushResult.Refused("${result.err} (${result.detail})")
                }
                AckResult.Timeout ->
                    PushResult.Failed("the device stopped answering mid-transfer")
            },
        )
    }

    /**
     * Tear down: close the transport AND stop this view model's coroutines.
     *
     * **Cancelling the scope is the half that was missing, and its absence leaked.**
     * `MainActivity.onDestroy` calls this and drops its reference, but the five
     * collectors and `client.run()` — all launched on `scope` — kept collecting
     * flows that never complete, holding the old view model, client and transport
     * alive for the process's lifetime. That is a leak on any activity recreation
     * the launch mode and `configChanges` do not absorb.
     *
     * `client.run()` "never returns" by design, so nothing inside it will stop on
     * its own; cancelling the scope is the only thing that ends it.
     */
    fun close() {
        client.close()
        if (ownsScope) scope.cancel()
    }


    private companion object {
        /** How many device log lines the link screen keeps. */
        const val kMaxLogLines = 20

        /**
         * Lowercase hex SHA-256, matching the firmware's in-tree implementation and
         * `SwcClient`'s own — spec §9.5 step 3's download verification.
         */
        fun sha256Hex(data: ByteArray): String =
            java.security.MessageDigest.getInstance("SHA-256")
                .digest(data).joinToString("") { "%02x".format(it) }
    }
}
