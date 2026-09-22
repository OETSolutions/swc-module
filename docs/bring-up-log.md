
## The firmware dropped a binding's second action — and its first, if the app's came first

Found 2026-09-24, continuing audit (N-29). §3.5 is explicit that a binding's
`actions` are "executed in order, each independently failable" and names the
product's core case as one binding carrying *both* halves — "emit the factory key
press **and** tell the app". The app implements its half; the firmware did not.

`BindingResolve` returned a single `ResolvedAction` built from `b.actions[0]` only,
and `SystemOrchestrator`'s execute path tested that one action and only in its
`kOutVoltage` branch — every other kind, and every action past the first, fell to an
`else` that merely `ReleaseKey`d. Two distinct failures follow:

- `[OUT_VOLTAGE, APP_LAUNCH]` (the exact combination §3.5 says the list exists for)
  drove the key correctly but never played the bound `BUZZ`, if present.
- `[APP_INTENT, OUT_VOLTAGE]` — an app action FIRST — drove **no key at all**,
  because `actions[0]` was not `OUT_VOLTAGE` and the `else` released the line. The
  press silently did nothing on the wire the user was watching, while the app still
  fired its own half from the `event` frame.

Reachability was narrow but real: the app's editor only ever builds single-action
bindings and `ConfigDefault` ships none, so this needs a hand-authored or
`config_patch`-written config with two actions — legal, accepted by `ConfigValidate`
(`kMaxActionsPerBinding` is 2), and decoded without complaint.

**Fixed** by making the resolver carry the whole ordered list and the orchestrator
run it. `BindingResolve` now returns a `ResolvedBinding` (`found`, `action_count`,
`actions[]`), refusing the binding if *any* action is un-executable rather than
checking only the first. A new `SystemOrchestrator::RunBindingActions` walks the
list: the `OUT_` family and `BUZZ` are the firmware's column (§3.6's "Executed by"
table) and execute, while an app-owned kind is **skipped** — neither executed nor
treated as a release — which is what §3.5's "a failed app-side action must never
prevent the hardware key press" requires.

The order-dependency the open item flagged is resolved by the list's own order: the
`OUT_VOLTAGE` pulse is set on `key_released_at_ms` and the runner does **not** issue
a release, so a later action cannot cut it short; only an explicit `OUT_RELEASE` or
`NONE` releases. `BUZZ` still REPLACES the default `KEY_ACCEPTED` (one buzzer, `Play`
replaces rather than queues, §7.2), and an empty command band still suppresses the
acknowledgement and plays `KEY_UNKNOWN`.

Pinned by three orchestrator tests — `ABindingWithTwoActionsRunsBothInOrder`,
`AnAppActionFirstStillDrivesTheKeyAfterIt`, and
`AnAppActionAfterTheLevelDoesNotReleaseTheKey` — plus two resolver tests for the
ordered list and the any-position refusal. Mutation-tested twice: collapsing the
runner loop to one action and turning the app-kind skip back into a `ReleaseKey`
each fail the suite.

## The app's DTR went to the data interface — so `hello` was never sent

Found 2026-09-23, continuing audit. `UsbSerialTransport.open()` issues CDC
`SET_CONTROL_LINE_STATE` itself (Android's `UsbDeviceConnection` has no `setDtr`),
and it addressed the request to the **data** interface — `conn.setControlLineState(
iface.id, ...)`, where `iface` is the `USB_CLASS_CDC_DATA` interface it had just
claimed. That is the wrong interface, and the failure is total and silent.

CDC 1.2 §6.3.12 puts `SET_CONTROL_LINE_STATE` on the **communication** interface,
and the device side enforces it: TinyUSB's `cdcd_control_xfer_cb` walks its CDC
instances and matches `request->wIndex` against `p_cdc->itf_num`
(`managed_components/espressif__tinyusb/src/class/cdc/cdc_device.c:388`), and
`itf_num` is set from the **communication** interface descriptor
(`cdcd_open`, `cdc_device.c:307`). `TUD_CDC_DESCRIPTOR` emits the pair as
`(comm_id, comm_id + 1)` (`usbd.h:262`), so the data interface number matches no
instance: the loop falls out, `TU_VERIFY(itf < CFG_TUD_CDC)` fails, the class
handler returns false, and the core stalls EP0 (`process_setup_received`'s
"Returns false if unable to complete the request, causing caller to stall control
endpoints").

The chain from there is every part of the link:

1. The request STALLs, so `tud_cdc_line_state_cb` never fires.
2. `UsbLink`'s `CdcLineStateCallback` therefore never records an open, so
   `ServiceLineState` never calls `CommandRouter::OnConnected()`.
3. No `hello` is emitted — `OnConnected` is its only producer — and no config reply
   run is started.
4. The app sets `LinkState.Connected` only on `hello`, so the app sits at
   `Disconnected` forever. `getConfig` still asks, but the app's own `config_get`
   is answered by nothing.

Nothing errors anywhere. Enumeration succeeds, the interface claims, the bulk
endpoints read and write, and no frame ever arrives — which reads as a dead
adapter. The class's own comment already warned about exactly this ("getting it
wrong is silent"), which is why it is worth recording that it was wrong.

**Fixed** by resolving the communication interface rather than assuming the data
one: `selectControlLineInterface` (a top-level `internal` function so it is
JVM-testable, since the failure it guards is invisible without a device) prefers the
communication interface immediately preceding the claimed data interface and falls
back to the first one. A device with no communication interface yields null and is
reported `NotOurDevice` rather than sent a request that would stall.
`ControlLineInterfaceTest` covers the single-instance pair, a reversed enumeration
order, a two-instance composite device (where picking the *first* comm interface
would address the wrong CDC), the no-comm case, and the non-adjacent fallback.
Mutation-tested: returning `dataId` (the old behaviour) fails all five.
