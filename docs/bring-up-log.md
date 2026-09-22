
## FR-1's NTC clause: the channel was never converted, so `temp_c_at_learn` was a constant

Found 2026-09-24, continuing audit (N-67). FR-1 requires the firmware to "sample
both ladder channels **and the NTC** continuously". The ladder half was real —
every poll tick converts each channel through `AdcReader`. The NTC half had no
implementation at all:

- `ADC_CH_TEMP` was mapped to `ADC_CHANNEL_6` in `EspHal`'s `AdcPinFor` and read
  by **nothing** — every `adc_read_mv` call site named SWC1/SWC2, AUX1 or
  KEY_SENSE1/2.
- There was no NTC-to-temperature conversion anywhere in the tree — no B3380
  routine, no Steinhart-Hart, no divider inversion — so the raw millivolts would
  not have been a temperature even if read.
- Both learn paths passed a literal 0 for `temp_tenths_c`, so the field a future
  compensation is meant to consume could not hold a measurement.

Three separate surfaces said otherwise (spec 6.4's honesty paragraph, §11's FR-1
row, and a comment in `SystemOrchestrator.cpp` that quoted a sentence appearing
nowhere in the spec). Those were corrected in an earlier pass; this closes the
feature they had described.

**Fixed.** The divider values came off the schematic rather than needing the
board: `SWC.kicad_sch` carries `RT1` (`Device:Thermistor_NTC`, value `10k B3380`)
from the `TEMP_ADC` node to `GND`, and `R29` (`Device:R`, `10k`) from `+3V3` to
that node. So the part is on the **low side** — the node RISES with temperature —
and `R = R_series * V / (VDD - V)`.

`lib/Analog/NtcConvert.h` holds the inversion and the B-constant model
(`1/T = 1/T0 + ln(R/R0)/B`) in integer maths, because the config carries no
floating point. `ln` is the expansion `2*(y + y³/3 + y⁵/5 + …)` with
`y = (R-R0)/(R+R0)` in 2^16 fixed point. **Thirty series terms, not the
conventional eight**: at the hot end `y` approaches -0.9 and eight terms are
+0.8 °C wrong; thirty bring the error under 0.25 °C across -40 to +120 °C. A
tenfold-wider fixed-point scale does not help — the truncation is the error term,
not the scale.

`SystemOrchestrator::SampleNtcTenthsC` reads `ADC_CH_TEMP` and converts, and
**both** learn paths now record the result in `temp_c_at_learn`: the headless
wizard (every tick of a prompt) and `CommandRouter::RecordLearnSample` (the
app-driven session). A failed read **holds** the last good value rather than
reporting the sentinel, because the ADC returns -1 on error (N-43) and a learn
that stored "0 C" from a transient would record a temperature nothing measured.

Pinned by 8 `NtcConvert` tests whose expectations come from the **datasheet
model applied to the schematic's divider**, not from the implementation — so a
swapped divider side or a sign error fails rather than cancelling out — plus one
orchestrator test that drives a whole headless learn and checks the committed
profile's `temp_c_at_learn` is the converted value. Mutation-tested by reverting
the headless call site to the sentinel, which fails the suite.

**Still board-gated:** the conversion is validated against the datasheet's
B-constant table, not against a thermometer. Reading one room temperature and
comparing is the bring-up step that would close even that.

## The DAC fault path was three-quarters absent — and two comments asserted otherwise

Found 2026-09-24, continuing audit (N-21). Spec §6.8's I²C row promises four
things for a DAC failure: *retry with backoff*, *latch*, *release the line*,
*never drive a guessed code*. Only the last held, and two doc-comments claimed the
first two were implemented.

- **No read-back.** FR-13 step 3b and §6.1's startup sequence say "VERIFY the DAC
  is in the safe state (read back)", and `EspHal` called no `i2c_master_receive` at
  all. The firmware believed its own write.
- **No retry, no backoff.** `IHAL.h` and `EspHal.cpp` both said `dac_set_code`
  "retries with backoff internally and latches a fault on persistent failure".
  Neither was true: one synchronous `i2c_master_transmit`, one `ESP_LOGE`.
- **No `FAULT_DAC` emitter.** §7.2 gives the pattern the I²C/DAC meaning and
  nothing played it, because `dac_set_code` returns `void` so no caller could learn
  of a failure.

**Fixed.** `IHAL` gains `dac_read_code`; `EspHal` issues the MCP4728 Read Command
and `DacFrame::DecodeReadCode` (pure, host-tested) owns the byte layout — 24
sequential bytes, 3 of input register then 3 of EEPROM per channel A→D, code =
`buf[6n+2] | ((buf[6n+1] & 0x0F) << 8)`. That layout is from DS22187E Figure
5-15, cross-checked against Adafruit's driver, and pinned by a round-trip test
through the existing encoder. `DacRetry.h` holds the retry policy — 3 attempts,
1 ms then 2 ms, short on purpose because the whole sequence must fit inside the
200 ms key pulse — applied to both the code and the power-mode writes.

`SystemOrchestrator::VerifySafeIdleIdleCodes` runs the read-back per channel at
the end of `EstablishSafeIdle` and compares against the code just written.

**A mismatch releases rather than driving the read value.** The read value is
exactly what the check just declared untrustworthy, so driving it is the
"guessed code" §6.8 forbids; the firmware re-asserts the idle code, which *is*
the released state (§6.7).

**A wrong value needed a second latch.** The HAL's `dac_faulted` is set by a
failed I²C *transaction* — but a mismatch is a transaction that succeeded, so the
HAL cannot see it. `OutputVerified()` therefore folds in the orchestrator's own
`dac_verify_failed_` as well; without it FR-37's rollback gate could not see the
one failure the read-back exists to find. `ReportDacFault` plays `FAULT_DAC` once
per boot (an edge — the latch is permanent, so a per-tick report would `Play`
it every tick forever) and `Tick` checks the HAL latch, so a write that fails on
the key path is reported too, not only the boot one.

Pinned by 4 `DacFrame` read/round-trip tests, 4 `DacRetry` tests and 5
orchestrator tests, mutation-tested three ways: dropping the read-back call,
driving the read value instead of the written one, and removing
`dac_verify_failed_` from the gate — each fails the suite.

One contract drift was caught on the way: the `ack` row in `contract_schema.py`
did not declare `result`, which `ota_end`'s ack had started emitting in the
previous session's OTA work. The gate self-test (`test_gen_contract.py`) found
it; the row now declares it.

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
