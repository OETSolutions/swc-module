// The two front ends: a serial menu and a web page.
//
// Both drive the SAME registry through the SAME runner, so a test cannot behave
// differently depending on how it was started. Neither is required: the serial
// menu works with no network at all, and the web page survives a USB
// re-enumeration. That redundancy is deliberate -- the board has no UART header,
// so a serial-only tool goes dark if the USB console drops, and a web-only tool
// cannot test the radio that serves it.

#include <Arduino.h>
#include <WebServer.h>
#include <Wire.h>
#include <WiFi.h>
#include <string.h>

#include "Adc.h"
#include "BoardPins.h"
#include "Dac.h"
#include "Log.h"
#include "Secrets.h"
#include "Temp.h"
#include "TestRunner.h"
#include "swc_logic/Output.h"

// ---------------------------------------------------------------------------
// Web UI
// ---------------------------------------------------------------------------
static WebServer s_http(80);
static bool      s_web_up = false;
static volatile int s_web_requested = -1;   // a test index asked for by the page

// The page is deliberately one self-contained string with no external assets: it
// must work on a car bench with no internet, and a CDN reference would fail
// exactly when the tool is most needed. It re-fetches the log on a timer rather
// than using websockets, because that costs nothing and has far fewer failure
// modes on a marginal link.
static const char kPageHead[] PROGMEM =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>SWC bring-up</title><style>"
    "body{font:13px/1.45 ui-monospace,Menlo,Consolas,monospace;background:#111;color:#ddd;margin:0;padding:12px}"
    "h1{font-size:16px;margin:0 0 8px}h2{font-size:14px;margin:16px 0 6px;color:#9cf}"
    "a.b,button{display:inline-block;background:#234;color:#cfe;border:1px solid #456;"
    "border-radius:4px;padding:3px 8px;margin:2px;text-decoration:none;font:inherit;cursor:pointer}"
    "button:hover,a.b:hover{background:#345}button.all{background:#253;border-color:#475}"
    "button.sum{background:#333;border-color:#555}"
    "table{border-collapse:collapse;width:100%;margin:4px 0 12px}"
    "td,th{border-bottom:1px solid #2a2a2a;padding:3px 6px;text-align:left;vertical-align:top}"
    "th{color:#9cf;font-weight:600}"
    ".PASS{color:#6d6}.FAIL{color:#f77;font-weight:700}.BLOCKED{color:#da6}"
    ".SKIP{color:#888}.WARN{color:#dc6}.NOTRUN,.empty{color:#666}"
    "pre{background:#000;border:1px solid #2a2a2a;padding:8px;overflow:auto;"
    "max-height:60vh;white-space:pre-wrap;font:inherit}"
    ".m{color:#888;font-size:11px}</style></head><body>"
    "<h1>SWC adapter &mdash; bring-up</h1>";

static String statusClass(TestRunner::Result r)
{
    switch (r) {
        case TestRunner::Result::kPass:    return "PASS";
        case TestRunner::Result::kFail:    return "FAIL";
        case TestRunner::Result::kBlocked: return "BLOCKED";
        case TestRunner::Result::kSkip:    return "SKIP";
        case TestRunner::Result::kWarn:    return "WARN";
        default:                           return "NOTRUN";
    }
}

static void handleRoot()
{
    String h;
    h.reserve(24000);
    h += FPSTR(kPageHead);

    // Board state, at the top, because it is the context every result is read in.
    uint32_t tmv = 0;
    Adc::ReadAvgMv(Adc::kTemp, 16, &tmv);
    const float tc = Temp::CelsiusFromMv((float)tmv, 3300.0f);

    h += "<div class=m>heap " + String(ESP.getFreeHeap() / 1024) + " kB &middot; ";
    h += "NTC ";
    if (isnan(tc)) h += "n/a";
    else h += String(tc, 1) + " C";
    h += " &middot; DAC 0x" + String(Dac::Address(), HEX);
    h += " " + String(Dac::Present() ? "present" : "ABSENT");
    h += " &middot; ADC " + String(Adc::CalibrationDegraded() ? "linear fallback" : "eFuse cal");
    h += "</div>";

    h += "<h2>Actions</h2><div>";
    h += "<button class=all id=bAll onclick=\"runAll()\">Run all 30</button>";
    h += "<button class=sum onclick=\"fetch('/summary').catch(()=>{})\">Summary</button>";
    h += "<button class=sum onclick=\"fetch('/clear').catch(()=>{})\">Clear log</button>";
    h += "<a class=b href='/log'>Log only</a>";
    h += "<a class=b href='/identify'>Identify (blink + beep)</a>";
    h += "</div>";
    // A live status line. The page cannot be told "running" by the server (the
    // server is busy running the test), so the browser tracks it itself.
    h += "<div id=st class=m style='min-height:1.2em'></div>";

    h += "<h2>Tests</h2><table><tr><th>#</th><th>Test</th><th>Needs</th>"
         "<th>Result</th><th>Detail</th></tr>";

    for (size_t i = 0; i < TestRunner::Count(); ++i) {
        const TestRunner::Test *t = TestRunner::Get(i);
        if (!t) continue;
        const TestRunner::Outcome &o = TestRunner::LastOutcome(i);

        h += "<tr><td>" + String(t->number) + "</td>";
        // A button, not a link: a link would navigate (and reload) instead of
        // scheduling, which is the bug this replaces.
        h += "<td><button style='text-align:left' onclick='runTest(" +
             String(t->number) + ")'>" + String(t->title) + "</button>";
        h += "<div class=m>" + String(t->covers ? t->covers : "") + "</div></td>";
        h += "<td class=m>" + String(t->needs ? t->needs : "") + "</td>";
        const bool is_running = (TestRunner::RunningIndex() == (int)i);
        h += "<td class=" + (is_running ? String("WARN") : statusClass(o.result)) + ">" +
             (is_running ? String("RUNNING") : String(TestRunner::ResultName(o.result)));
        if (o.duration_ms) h += "<div class=m>" + String(o.duration_ms) + " ms</div>";
        h += "</td>";
        h += "<td>" + String(o.summary) + "</td></tr>";
    }
    h += "</table>";

    h += "<h2>Log</h2><pre id=l>";
    size_t len = 0;
    const char *cap = Log::Capture(&len);

    // Show only the TAIL of the log here. Escaping expands text (every '&'
    // becomes five characters), and this part has no PSRAM -- so embedding the
    // whole capture would put a 100 KB+ String on a heap that has to serve the
    // WiFi stack too. 6 KB of tail is several screens and plenty to see the
    // result of the test just run; the Log-only page and /log?raw=1 carry the
    // rest. The page's poller replaces this pane with the full text anyway.
    const size_t kTailBytes = 6144;
    size_t start = 0;
    bool truncated = false;
    if (len > kTailBytes) {
        start = len - kTailBytes;
        // Only when we HAVE truncated does the start index need moving to a line
        // boundary -- and only then is anything omitted. Doing this unconditionally
        // skipped the first line of a short log AND printed the "earlier lines
        // omitted" notice when nothing had been.
        while (start < len && cap[start] != '\n') ++start;
        if (start < len) ++start;
        truncated = true;
    }
    if (truncated) h += "(... earlier lines omitted; use the Log-only page)\n";

    // Escape the three characters that would break out of the <pre>.
    for (size_t i = start; i < len; ++i) {
        const char ch = cap[i];
        if (ch == '<') h += "&lt;";
        else if (ch == '>') h += "&gt;";
        else if (ch == '&') h += "&amp;";
        else h += ch;
    }
    h += "</pre>";

    // THE CLICK MUST NOT RELOAD. A test runs synchronously inside loop(), so while
    // one is running the HTTP server does not answer at all -- a reload issued right
    // after the click hangs for the whole test (9 s for test 20, 12 s for test 22)
    // and the browser shows a blank page. That is what "clicking does nothing"
    // looked like: the test WAS running, the page just could not see it.
    //
    // So the click only SCHEDULES the test (which the server does answer, with a 303)
    // and then the poller watches the log. When the RESULT count grows, the test has
    // finished and the page reloads ONCE to redraw the table.
    h += "<script>";
    h += "var resultCount=-1, running=null, base=null;";
    h += "function setStatus(s){document.getElementById('st').textContent=s}";
    h += "function lockButtons(on){document.querySelectorAll('button')"
         ".forEach(b=>{if(b.id!='bAll'||!on)b.disabled=on})}";
    h += "function runTest(n){if(running!==null)return;running=n;base=resultCount;"
         "setStatus('Test '+n+' requested - running... (long tests take up to ~15 s)');"
         "lockButtons(true);"
         "fetch('/run?n='+n).catch(()=>{});}";
    h += "function runAll(){if(running!==null)return;running='all';base=resultCount;"
         "setStatus('All 30 requested - running... this takes about 2 minutes');"
         "lockButtons(true);fetch('/runall').catch(()=>{});}";
    h += "function poll(){fetch('/log?raw=1',{cache:'no-store'})"
         ".then(r=>r.text()).then(t=>{"
         "const e=document.getElementById('l');if(e.textContent!==t)e.textContent=t;"
         "var c=(t.match(/^RESULT /gm)||[]).length;"
         "if(resultCount<0){resultCount=c;}"
         "else if(running!==null&&c>base){running=null;setStatus('done - reloading');"
         "lockButtons(false);location.reload();return;}"
         "}).catch(()=>{setStatus('working... (the server is busy running the test)')})"
         ".then(()=>setTimeout(poll,1500))}";
    h += "setTimeout(poll,800)</script>";
    h += "</body></html>";

    s_http.send(200, "text/html; charset=utf-8", h);
}

static void handleLog()
{
    size_t len = 0;
    const char *cap = Log::Capture(&len);
    if (s_http.hasArg("raw")) {
        s_http.send(200, "text/plain; charset=utf-8", cap);
        return;
    }
    String h = FPSTR(kPageHead);
    h += "<h2>Log</h2><pre>";
    for (size_t i = 0; i < len; ++i) {
        const char ch = cap[i];
        if (ch == '<') h += "&lt;";
        else if (ch == '>') h += "&gt;";
        else if (ch == '&') h += "&amp;";
        else h += ch;
    }
    h += "</pre><a class=b href='/'>back</a></body></html>";
    s_http.send(200, "text/html; charset=utf-8", h);
}

static void handleRun()
{
    if (s_http.hasArg("n")) {
        const long n = s_http.arg("n").toInt();
        // The page addresses tests by their printed NUMBER, not their index --
        // that is what the menu, the summary and the README all use.
        for (size_t i = 0; i < TestRunner::Count(); ++i) {
            const TestRunner::Test *t = TestRunner::Get(i);
            if (t && t->number == n) { s_web_requested = (int)i; break; }
        }
    }
    // Run the test inside the handler, NOT deferred to loop().
    //
    // The deferred version (set a flag, redirect, let loop() pick it up) is what
    // WEDGED THE SERVER: the 303 redirect left a client connection that the Arduino
    // WebServer had not finished tearing down, and the test that then ran for nine
    // seconds -- without a single handleClient() call -- left it in a state it never
    // recovered from. HTTP stayed dead indefinitely after a web-triggered test, while
    // the identical test run from the serial menu was fine. That asymmetry is what
    // pointed at the handler.
    //
    // Running it here is safe because the HTTP response is only the redirect, and
    // the page no longer depends on receiving it -- it fires the request and then
    // watches the log. So a long test blocking this one connection is acceptable:
    // the test is the thing the user asked for, and the page is already polling.
    //
    // The response is sent BEFORE the test runs, so the client is released first.
    s_http.sendHeader("Location", "/");
    s_http.send(303, "text/plain", "");
    s_http.client().stop();   // release the socket now, before the long test

    // Then run it. Anything loop() would have done, done here instead.
    if (s_web_requested >= 0) {
        Log::Printf("");
        Log::Printf("(requested from the web UI)");
        TestRunner::Run((size_t)s_web_requested);
        s_web_requested = -1;
    }
}

static void handleRunAll()
{
    // Same treatment as handleRun: respond, release the socket, then run.
    s_http.sendHeader("Location", "/");
    s_http.send(303, "text/plain", "");
    s_http.client().stop();

    Log::Printf("");
    Log::Printf("(run all, requested from the web UI)");
    TestRunner::RunAll();
}

static void handleSummary()
{
    TestRunner::PrintSummary();
    s_http.sendHeader("Location", "/");
    s_http.send(303, "text/plain", "");
}

static void handleClear()
{
    Log::ClearCapture();
    s_http.sendHeader("Location", "/");
    s_http.send(303, "text/plain", "");
}

// Blink and beep so the operator can identify WHICH board is answering -- useful
// when several are on one bench.
static void handleIdentify()
{
    const char *msg = "identify: blinking both LEDs and beeping";
    Log::Printf("%s", msg);
    for (int i = 0; i < 6; ++i) {
        digitalWrite(PIN_LED_STAT, LED_ON);
        digitalWrite(PIN_LED2, LED_ON);
        digitalWrite(PIN_BUZZ, HIGH);
        delay(80);
        digitalWrite(PIN_LED_STAT, LED_OFF);
        digitalWrite(PIN_LED2, LED_OFF);
        digitalWrite(PIN_BUZZ, LOW);
        delay(80);
    }
    digitalWrite(PIN_BUZZ, LOW);
    s_http.sendHeader("Location", "/");
    s_http.send(303, "text/plain", "");
}

static void handleNotFound()
{
    // Any path redirects to the root: this is a bench tool on a LAN, and a 404
    // page that a phone shows as an error would just be noise.
    s_http.sendHeader("Location", "/");
    s_http.send(303, "text/plain", "");
}

static bool WebBegin()
{
#if !SWC_WIFI_CONFIGURED
    Log::Printf("web UI: no WiFi credentials, so the web front end is OFF");
    return false;
#else
    WiFi.mode(WIFI_STA);
    WiFi.begin(SWC_WIFI_SSID, SWC_WIFI_PASSWORD);
    Log::Printf("web UI: associating with '%s'...", SWC_WIFI_SSID);

    const uint32_t deadline = millis() + 15000;
    while (millis() < deadline && WiFi.status() != WL_CONNECTED) {
        delay(250);
    }
    if (WiFi.status() != WL_CONNECTED) {
        Log::Printf("web UI: could not associate (status %d) -- serial menu only",
                    (int)WiFi.status());
        WiFi.mode(WIFI_OFF);
        return false;
    }

    s_http.on("/", handleRoot);
    s_http.on("/log", handleLog);
    s_http.on("/run", handleRun);
    s_http.on("/runall", handleRunAll);
    s_http.on("/summary", handleSummary);
    s_http.on("/clear", handleClear);
    s_http.on("/identify", handleIdentify);
    s_http.onNotFound(handleNotFound);
    s_http.begin();

    // mDNS would be nicer than an IP, but it is one more thing that can fail on a
    // marginal link, so the IP is printed instead and that is enough.
    Log::Printf("");
    Log::Rule('*');
    Log::Printf("  web UI:  http://%s/", WiFi.localIP().toString().c_str());
    Log::Printf("  (this survives a USB re-enumeration -- the serial menu does not)");
    Log::Rule('*');
    Log::Printf("");
    s_web_up = true;
    return true;
#endif
}

// ---------------------------------------------------------------------------
// Serial menu
// ---------------------------------------------------------------------------
// A raw hexdump of the MCP4728's read response, with a known pattern written
// first so the byte positions are unambiguous.
//
// This exists because the decoded read-back did not match what was written, and
// the only way to tell a DECODER bug from a PROTOCOL bug is to look at the bytes
// themselves. Guessing at which of the two it was would have cost more than
// adding this: the probe 0xABC splits into a distinctive high nibble (0xA) and low
// byte (0xBC), so wherever those two values appear in the response identifies the
// layout immediately.
static void PrintDacRaw()
{
    Log::Section("DAC RAW READ (diagnostic)");

    if (!Dac::Present()) {
        Log::Printf("  no DAC at 0x%02X -- run test 4", Dac::Address());
        return;
    }

    // FOUR DISTINCT, unmistakable patterns, one per channel, ALL IN NORMAL MODE.
    //
    // The first attempt used two channels and left the ADJ channel powered down,
    // which left too many unknowns: a power-down channel may read back as zeros OR
    // as its power-down code, so a zero could not be attributed. Four distinct
    // patterns remove the ambiguity entirely -- wherever 0x11, 0x22, 0x33 and 0x44
    // appear identifies each channel's byte position directly, and the layout falls
    // out of the data instead of being inferred from a datasheet recollection.
    Dac::SetSignal(1, Output::Mode::kTracking, 0x111);  // A = VOUTA, and B mirrors it
    delay(4);
    Dac::SetSignal(2, Output::Mode::kTracking, 0x333);  // C = VOUTC, and D mirrors it
    delay(4);
    // Now put B and D somewhere ELSE, still in normal mode, so input and mirror
    // are distinguishable. B and D are written directly, bypassing SetSignal --
    // this is the only place in the tool that does so, and only because mapping
    // the layout requires writing a channel the servo API deliberately controls.
    {
        uint8_t f[DacFrame::kSetSize];
        DacFrame::EncodeSet(f, DacFrame::kChannelB, DacFrame::kNormal, 0x222);
        Dac::WriteRaw(f, sizeof(f), Dac::Address());
        delay(4);
        DacFrame::EncodeSet(f, DacFrame::kChannelD, DacFrame::kNormal, 0x444);
        Dac::WriteRaw(f, sizeof(f), Dac::Address());
        delay(6);
    }
    Log::Printf("  wrote A=0x111  B=0x222  C=0x333  D=0x444, all normal mode");
    Log::Printf("  (0x11/0x22/0x33/0x44 are distinct, so each channel's byte");
    Log::Printf("   position in the response identifies itself)");
    Log::Printf("");

    // ---- the 8-byte read (command 0x08) --------------------------------
    Wire.beginTransmission(DacFrame::kAddrGeneralCall);
    Wire.write(DacFrame::kReadCmdDac);
    const uint8_t e8 = Wire.endTransmission();
    const size_t r8 = Wire.requestFrom((int)Dac::Address(), (int)DacFrame::kReadDacBytes);
    uint8_t b8[DacFrame::kReadDacBytes + 4];
    size_t n8 = 0;
    while (Wire.available() && n8 < sizeof(b8)) b8[n8++] = (uint8_t)Wire.read();

    Log::Printf("  8-byte read (cmd 0x%02X): endTransmission=%u, requested %u, got %u",
                DacFrame::kReadCmdDac, e8, (unsigned)DacFrame::kReadDacBytes, (unsigned)r8);
    for (size_t i = 0; i < n8; ++i) {
        Log::Printf("    [%2u] = 0x%02X  %3u", (unsigned)i, b8[i], b8[i]);
    }

    // ---- the 24-byte read (command 0x09) --------------------------------
    Wire.beginTransmission(DacFrame::kAddrGeneralCall);
    Wire.write(DacFrame::kReadCmdAll);
    const uint8_t e24 = Wire.endTransmission();
    const size_t r24 = Wire.requestFrom((int)Dac::Address(), (int)DacFrame::kReadAllBytes);
    uint8_t b24[DacFrame::kReadAllBytes + 4];
    size_t n24 = 0;
    while (Wire.available() && n24 < sizeof(b24)) b24[n24++] = (uint8_t)Wire.read();

    Log::Printf("");
    Log::Printf("  24-byte read (cmd 0x%02X): endTransmission=%u, requested %u, got %u",
                DacFrame::kReadCmdAll, e24, (unsigned)DacFrame::kReadAllBytes, (unsigned)r24);
    for (size_t i = 0; i < n24; ++i) {
        Log::Printf("    [%2u] = 0x%02X  %3u", (unsigned)i, b24[i], b24[i]);
    }

    // ---- locate each pattern -------------------------------------------
    Log::Printf("");
    Log::Printf("  locating the written markers:");
    const uint8_t marks[4] = {0x11, 0x22, 0x33, 0x44};
    const char  *who[4]   = {"A(VOUTA)", "B(VOUTB)", "C(VOUTC)", "D(VOUTD)"};
    for (int m = 0; m < 4; ++m) {
        for (size_t i = 0; i < n8; ++i) {
            if (b8[i] == marks[m]) Log::Printf("    0x%02X (%s) in the 8-byte read at [%u]",
                                               marks[m], who[m], (unsigned)i);
        }
        for (size_t i = 0; i < n24; ++i) {
            if (b24[i] == marks[m]) Log::Printf("    0x%02X (%s) in the 24-byte read at [%u]",
                                                marks[m], who[m], (unsigned)i);
        }
    }
    Log::Printf("");
    Log::Printf("  Each marker's byte position is where that channel's code LOW BYTE");
    Log::Printf("  sits; its HIGH nibble is in the PRECEDING byte.");

    // ---- the power-down field, by DIFFERENTIAL ---------------------------
    // Where PD1:PD0 sits in a read response is not visible from a single read: the
    // code bytes are known but the config bits are buried. Writing the SAME channel
    // with each of the four power modes and diffing the response identifies the byte
    // and the bit position from the change alone.
    Log::Printf("");
    Log::Printf("  power-mode differential on channel B (write 4 modes, diff the read):");

    uint8_t prev[24];
    bool have_prev = false;
    const DacFrame::PowerMode pms[4] = {DacFrame::kNormal, DacFrame::kGnd1k,
                                        DacFrame::kGnd100k, DacFrame::kGnd500k};
    const char *pmn[4] = {"normal(00)", "1k(01)", "100k(10)", "500k(11)"};
    for (int m = 0; m < 4; ++m) {
        uint8_t f[DacFrame::kSetSize];
        DacFrame::EncodeSet(f, DacFrame::kChannelB, (uint8_t)pms[m], 0x222);
        Dac::WriteRaw(f, sizeof(f), Dac::Address());
        delay(5);

        Wire.beginTransmission(DacFrame::kAddrGeneralCall);
        Wire.write(DacFrame::kReadCmdAll);
        Wire.endTransmission();
        Wire.requestFrom((int)Dac::Address(), (int)DacFrame::kReadAllBytes);
        uint8_t cur[24];
        size_t n = 0;
        while (Wire.available() && n < sizeof(cur)) cur[n++] = (uint8_t)Wire.read();

        Log::Printf("    PD=%s:", pmn[m]);
        for (size_t i = 0; i < n; ++i) {
            Log::Printf("      [%2u] = 0x%02X", (unsigned)i, cur[i]);
        }
        if (have_prev) {
            Log::Printf("    bytes that CHANGED vs the previous mode:");
            for (size_t i = 0; i < n && i < sizeof(prev); ++i) {
                if (cur[i] != prev[i]) {
                    Log::Printf("      [%2u] 0x%02X -> 0x%02X  (xor 0x%02X)",
                                (unsigned)i, prev[i], cur[i], prev[i] ^ cur[i]);
                }
            }
        }
        memcpy(prev, cur, sizeof(prev));
        have_prev = true;
    }
    Log::Printf("");
    Log::Printf("  The changing byte/bit identifies the PD1:PD0 field's real position.");

    // ---- IS THE ADJ CHANNEL ALIVE AT ALL? --------------------------------
    //
    // Test 12 showed tracking mode tracking perfectly (so signal ch, servo, sense,
    // divider and ADC are all good) while amplified mode sat PINNED at one level.
    // The only difference between the two is what happens to the ADJ channel, so
    // this isolates it: hold the signal code FIXED and sweep ADJ's code in NORMAL
    // mode. The transfer function predicts
    //
    //     V_KEY = 1.82*V_DAC - 0.82*V_ADJ
    //
    // so sweeping ADJ 0 -> 4095 must swing V_KEY from ~3003 mV down to ~297 mV.
    // A flat response means the ADJ channel's output is not reaching the summing
    // node at all -- which is a board/part fault, not a firmware one.
    Log::Printf("");
    Log::Printf("  ADJ channel probe: signal code FIXED at 2048, ADJ swept in NORMAL mode");
    Log::Printf("  predicted V_KEY = 1.82*1650 - 0.82*V_ADJ");
    Log::Printf("  %-10s %-10s %-14s %-12s", "ADJ code", "V_ADJ mV", "predicted mV", "sense x2 mV");

    Dac::SetSignal(1, Output::Mode::kTracking, 2048);   // signal = 2048, ADJ mirrors
    delay(60);
    const uint16_t adj_codes[6] = {0, 800, 1600, 2400, 3200, 4095};
    bool adj_moves = false;
    long first = -1, last = -1;
    for (int i = 0; i < 6; ++i) {
        // Signal in NORMAL mode at 2048; ADJ written directly so the two differ.
        uint8_t f[DacFrame::kSetSize];
        DacFrame::EncodeSet(f, DacFrame::kChannelA, DacFrame::kNormal, 2048);
        Dac::WriteRaw(f, sizeof(f), Dac::Address());
        DacFrame::EncodeSet(f, DacFrame::kChannelB, DacFrame::kNormal, adj_codes[i]);
        Dac::WriteRaw(f, sizeof(f), Dac::Address());
        delay(90);

        const int v_adj = Output::DacMvForCode(adj_codes[i]);
        const int predicted = Output::KeyMvForDacMv(Output::Mode::kAmplified,
                                                    Output::DacMvForCode(2048))
                              - (int)((82L * v_adj) / 100L);
        uint32_t sense = 0;
        Adc::ReadAvgMv(Adc::kSense1, 32, &sense);
        Log::Printf("  %-10u %-10d %-14d %-12u", adj_codes[i], v_adj, predicted, sense * 2);
        if (i == 0) first = (long)sense * 2;
        if (i == 5) last = (long)sense * 2;
        if (labs((long)sense * 2 - first) > 300) adj_moves = true;
    }
    Log::Printf("");
    if (adj_moves) {
        Log::Printf("  -> ADJ moves the output: the ADJ channel and summing node WORK.");
        Log::Printf("     The amplified-mode failure is therefore in how ADJ is set for");
        Log::Printf("     that mode (the 1k power-down), not in the channel itself.");
    } else {
        Log::Printf("  -> ADJ has NO effect on the output (first %ld, last %ld mV): the", first, last);
        Log::Printf("     ADJ channel is not reaching the summing node. Suspects, in order:");
        Log::Printf("     R61 (100k) open or unsoldered, U4's VOUTB/VOUTD not connected,");
        Log::Printf("     or U6's summing input open. This is a BOARD fault, and it is why");
        Log::Printf("     amplified mode cannot work -- gain 1.82 depends on V_ADJ.");
    }
    // ---- DOES THE POWER-DOWN MODE WORK, AND CAN IT BE AVOIDED? ------------
    //
    // The probe above proves the ADJ channel and summing node work in NORMAL mode.
    // Amplified mode (gain 1.82) needs V_ADJ = 0, and the spec gets that from the
    // ADJ channel's 1k POWER-DOWN. So: is the power-down reachable, and if not, can
    // the same voltage be produced with a NORMAL write at code 0?
    //
    // Both end in V_ADJ = 0 V, so both should give the same KEY voltage. If the
    // normal-mode version works and the power-down one does not, the fault is
    // precisely the power-down path -- and there is a clean workaround.
    Log::Printf("");
    Log::Printf("  POWER-DOWN vs NORMAL ADJ at code 0 (both should give V_ADJ = 0):");
    Log::Printf("  %-34s %-14s %s", "ADJ set to", "sense x2 mV", "expected 3003 mV");

    const uint16_t probe_code = 2048;   // V_DAC = 1650 mV -> 1.82*1650 = 3003

    // (a) the spec's way: ADJ powered down to 1k
    {
        uint8_t f[DacFrame::kSetSize];
        DacFrame::EncodeSet(f, DacFrame::kChannelA, DacFrame::kNormal, probe_code);
        Dac::WriteRaw(f, sizeof(f), Dac::Address());
        DacFrame::EncodeSet(f, DacFrame::kChannelB, DacFrame::kGnd1k, 0);
        Dac::WriteRaw(f, sizeof(f), Dac::Address());
        delay(120);
        uint32_t sense = 0;
        Adc::ReadAvgMv(Adc::kSense1, 32, &sense);
        Log::Printf("  %-34s %-14u %s", "power-down 1k (PD=01, code 0)", sense * 2,
                    (labs((long)sense * 2 - 3003) < 200) ? "MATCHES" : "<-- WRONG");
    }

    // (b) the same voltage from a NORMAL write
    {
        uint8_t f[DacFrame::kSetSize];
        DacFrame::EncodeSet(f, DacFrame::kChannelA, DacFrame::kNormal, probe_code);
        Dac::WriteRaw(f, sizeof(f), Dac::Address());
        DacFrame::EncodeSet(f, DacFrame::kChannelB, DacFrame::kNormal, 0);
        Dac::WriteRaw(f, sizeof(f), Dac::Address());
        delay(120);
        uint32_t sense = 0;
        Adc::ReadAvgMv(Adc::kSense1, 32, &sense);
        Log::Printf("  %-34s %-14u %s", "NORMAL, code 0", sense * 2,
                    (labs((long)sense * 2 - 3003) < 200) ? "MATCHES" : "<-- WRONG");
    }

    // (c) an intermediate code, to confirm (b) is really tracking a WRITTEN code
    {
        uint8_t f[DacFrame::kSetSize];
        DacFrame::EncodeSet(f, DacFrame::kChannelA, DacFrame::kNormal, probe_code);
        Dac::WriteRaw(f, sizeof(f), Dac::Address());
        DacFrame::EncodeSet(f, DacFrame::kChannelB, DacFrame::kNormal, 800);
        Dac::WriteRaw(f, sizeof(f), Dac::Address());
        delay(120);
        uint32_t sense = 0;
        Adc::ReadAvgMv(Adc::kSense1, 32, &sense);
        // expected = 3003 - 0.82*644 = 2475
        Log::Printf("  %-34s %-14u %s", "NORMAL, code 800 (expect 2475)", sense * 2,
                    (labs((long)sense * 2 - 2475) < 250) ? "MATCHES" : "<-- WRONG");
    }

    // ---- REPRODUCE TEST 12's AMPLIFIED CALL EXACTLY ----------------------
    // Every primitive above works (1k power-down, normal-at-zero, intermediate
    // code -- all match within 60 mV). Yet test 12's amplified sweep read a CONSTANT
    // 3262 mV. So either SetSignal() itself differs from the manual writes, or the
    // CONTEXT it runs in does. This runs the identical calls SetSignal makes, in the
    // identical order test 12 makes them, printing after each -- which separates the
    // two possibilities instead of continuing to reason about it.
    Log::Printf("");
    Log::Printf("  REPRODUCING test 12's amplified path via SetSignal():");
    Log::Printf("  %-8s %-12s %-14s %s", "code", "target mV", "sense x2 mV", "note");
    const uint16_t repro[5] = {2048, 2700, 3200, 3600, 3800};
    for (int i = 0; i < 5; ++i) {
        Dac::SetSignal(1, Output::Mode::kAmplified, repro[i]);
        delay(60);
        uint32_t sense = 0;
        Adc::ReadAvgMv(Adc::kSense1, 32, &sense);
        const int target = Output::KeyMvForDacMv(Output::Mode::kAmplified,
                                                 Output::DacMvForCode(repro[i]));
        Log::Printf("  %-8u %-12d %-14u %s", repro[i], target, sense * 2,
                    (labs((long)sense * 2 - target) < 300) ? "tracks" : "<-- WRONG");
    }

    // And now the SAME codes via the manual writes, for a direct comparison in the
    // same context.
    Log::Printf("");
    Log::Printf("  the same codes via MANUAL writes (signal normal + ADJ 1k):");
    Log::Printf("  %-8s %-12s %-14s %s", "code", "target mV", "sense x2 mV", "note");
    for (int i = 0; i < 5; ++i) {
        uint8_t f[DacFrame::kSetSize];
        DacFrame::EncodeSet(f, DacFrame::kChannelA, DacFrame::kNormal, repro[i]);
        Dac::WriteRaw(f, sizeof(f), Dac::Address());
        DacFrame::EncodeSet(f, DacFrame::kChannelB, DacFrame::kGnd1k, 0);
        Dac::WriteRaw(f, sizeof(f), Dac::Address());
        delay(60);
        uint32_t sense = 0;
        Adc::ReadAvgMv(Adc::kSense1, 32, &sense);
        const int target = Output::KeyMvForDacMv(Output::Mode::kAmplified,
                                                 Output::DacMvForCode(repro[i]));
        Log::Printf("  %-8u %-12d %-14u %s", repro[i], target, sense * 2,
                    (labs((long)sense * 2 - target) < 300) ? "tracks" : "<-- WRONG");
    }

    Dac::Release(1);
    Dac::Release(2);
    Log::Printf("  end of raw dump");

    // Leave the outputs released.
    Dac::Release(1);
    Dac::Release(2);
}

static void PrintMenu()
{
    Log::Printf("");
    Log::Rule('=');
    Log::Printf("  MENU");
    Log::Rule('=');

    // Group the menu by what the operator must wire, because that is the actual
    // unit of work: you attach one thing, then run the tests that need it.
    const char *groups[][2] = {
        {"no wiring needed", ""},
        {"J3 open (no head unit)", ""},
        {"loopback jumpers", ""},
        {"listen / watch", ""},
    };
    (void)groups;

    Log::Printf("  %-4s %-52s %s", "no.", "test", "result");
    for (size_t i = 0; i < TestRunner::Count(); ++i) {
        const TestRunner::Test *t = TestRunner::Get(i);
        if (!t) continue;
        const TestRunner::Outcome &o = TestRunner::LastOutcome(i);
        Log::Printf("  %-4d %-52s %s", t->number, t->title,
                    TestRunner::ResultName(o.result));
    }
    Log::Rule('-');
    Log::Printf("  type a NUMBER to run that test (1-%u)", (unsigned)TestRunner::Count());
    Log::Printf("  a  = run all, in order        s  = summary");
    Log::Printf("  c  = clear the capture buffer n  = reprint this menu");
    Log::Printf("  r  = reset all outcomes       w  = web UI address");
    Log::Printf("  ?  = full detail for every test (what each needs)");
    Log::Printf("  h  = hardware info dump      d  = DAC raw read hexdump");
    Log::Rule('=');
    Log::Printf("");
}

static void PrintDetails()
{
    Log::Section("TEST DETAIL");
    for (size_t i = 0; i < TestRunner::Count(); ++i) {
        const TestRunner::Test *t = TestRunner::Get(i);
        if (!t) continue;
        Log::Printf("%2d. %s", t->number, t->title);
        Log::Printf("    covers: %s", t->covers ? t->covers : "-");
        Log::Printf("    needs : %s", t->needs ? t->needs : "-");
    }
    Log::Printf("");
    Log::Printf("Recommended order on a fresh board:");
    Log::Printf("  1-3   nothing attached         -- power, identity, GPIO");
    Log::Printf("  4-10  nothing attached         -- I2C and the DAC");
    Log::Printf("  11    J3 open                  -- the sense path");
    Log::Printf("  12-13 a meter on J3.3 / J3.2   -- the servos");
    Log::Printf("  14-15 jumper J3.3->J2.3, J3.2->J2.2");
    Log::Printf("  16-18 the button pod / nothing -- the ladder inputs and NTC");
    Log::Printf("  19-20 listen and watch         -- buzzer and LEDs");
    Log::Printf("  21-23 the loopback + a pull-up -- the system layer");
    Log::Printf("  24-26 nothing                  -- USB, WiFi, NVS");
    Log::Printf("  27-28 both loopback jumpers    -- integration");
    Log::Printf("  29    jumpers as asked         -- connector continuity");
    Log::Printf("  30    both jumpers             -- endurance (slow)");
    Log::Printf("");
    Log::Printf("J2: 1=GND 2=SWC2 3=SWC1      J3: 1=GND 2=KEY2 3=KEY1");
    Log::Printf("J5: 1=GND 2=AUX3 3=AUX2 4=AUX1    J1: 1=GND 2=+12V");
    Log::Printf("");
}

static void PrintHardware()
{
    Log::Section("HARDWARE");
    Log::Printf("  %-20s %s", "chip", ESP.getChipModel());
    Log::Printf("  %-20s %u MHz", "cpu", ESP.getCpuFreqMHz());
    Log::Printf("  %-20s %u bytes", "flash", ESP.getFlashChipSize());
    Log::Printf("  %-20s %u bytes", "psram", ESP.getPsramSize());
    Log::Printf("  %-20s %u bytes", "free heap", ESP.getFreeHeap());
    Log::Printf("  %-20s %s", "adc calibration", Adc::CalibrationSourceName());
    Log::Printf("  %-20s 0x%02X %s", "dac", Dac::Address(),
                Dac::Present() ? "(present)" : "(NOT FOUND)");
    Log::Printf("  %-20s %s", "console", "ROM USB-Serial-JTAG (ARDUINO_USB_MODE=1)");
    Log::Printf("");
    Log::Printf("  %-20s %-6s %-8s %s", "channel", "pin", "mV", "raw");
    for (int i = 0; i < (int)Adc::kCount; ++i) {
        uint32_t mv = 0;
        uint16_t raw = 0;
        Adc::ReadAvgMv((Adc::Ch)i, 32, &mv, &raw);
        Log::Printf("  %-20s IO%-3u  %-8u %u", Adc::Name((Adc::Ch)i), Adc::Pin((Adc::Ch)i),
                    mv, raw);
    }
    Log::Printf("");
}

// Execute one deferred web request. Returns true if something ran.
static bool ServiceWebRequest()
{
    const int req = s_web_requested;
    if (req == -1) return false;
    s_web_requested = -1;

    if (req == -2) {
        TestRunner::RunAll();
        return true;
    }
    if (req >= 0) {
        Log::Printf("");
        Log::Printf("(requested from the web UI)");
        TestRunner::Run((size_t)req);
        return true;
    }
    return false;
}

// True once we have seen any input. The board cannot detect that a terminal
// ATTACHED (USB-Serial-JTAG gives no such event -- DTR is not wired to the ROM
// peripheral on this part), so the only reliable moment to greet a newly-attached
// monitor is its first keystroke. Without this, opening `pio device monitor` on an
// already-running board shows a blank screen until the user types something, which
// reads as a dead board.
static bool s_greeted = false;

static void ServiceSerial()
{
    if (!Serial.available()) return;

    if (!s_greeted) {
        s_greeted = true;
        Log::Printf("");
        Log::Printf("(a monitor just attached -- here is the menu; the board has been");
        Log::Printf(" running since boot and any earlier output is not resent)");
        PrintMenu();
    }

    const int c = Serial.read();

    // Numbers are multi-character, so collect a line at a time for digits and
    // act immediately on the single-letter commands.
    if (c >= '0' && c <= '9') {
        char buf[8];
        size_t n = 0;
        buf[n++] = (char)c;
        const uint32_t t0 = millis();
        while (n < sizeof(buf) - 1 && millis() - t0 < 1500) {
            if (Serial.available()) {
                const int d = Serial.read();
                if (d >= '0' && d <= '9') buf[n++] = (char)d;
                else break;
            } else {
                delay(5);
            }
        }
        buf[n] = '\0';
        const long num = atol(buf);

        for (size_t i = 0; i < TestRunner::Count(); ++i) {
            const TestRunner::Test *t = TestRunner::Get(i);
            if (t && t->number == num) {
                Log::Printf("");
                TestRunner::Run(i);
                PrintMenu();
                return;
            }
        }
        Log::Printf("no test numbered %ld (see the menu)", num);
        return;
    }

    switch (c) {
        case 'a': case 'A':
            Log::Printf("");
            TestRunner::RunAll();
            PrintMenu();
            break;
        case 's': case 'S':
            TestRunner::PrintSummary();
            break;
        case 'c': case 'C':
            Log::ClearCapture();
            Log::Printf("capture buffer cleared (%u lines were held)", (unsigned)0);
            break;
        case 'r': case 'R':
            TestRunner::ResetOutcomes();
            Log::Printf("all outcomes reset");
            break;
        case 'n': case 'N': case '\n': case '\r':
            PrintMenu();
            break;
        case 'w': case 'W':
            // Report the LIVE state, not the state at boot: test 25 could have run
            // (and, before it stopped tearing the link down, could have dropped it),
            // so `s_web_up` alone would print http://0.0.0.0/ and look like an answer.
            if (s_web_up && WiFi.status() == WL_CONNECTED) {
                Log::Printf("web UI: http://%s/", WiFi.localIP().toString().c_str());
            } else if (s_web_up) {
                Log::Printf("web UI: the link is DOWN (WiFi status %d) -- the page is "
                            "not reachable. Re-run test 25.", (int)WiFi.status());
            } else {
                Log::Printf("web UI is not up (no WiFi credentials, or it failed -- see test 25)");
            }
            break;
        case '?':
            PrintDetails();
            break;
        case 'h': case 'H':
            PrintHardware();
            break;
        case 'd': case 'D':
            PrintDacRaw();
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Entry points (called from main.cpp)
// ---------------------------------------------------------------------------
namespace FrontEnd {

void Begin()
{
    // LEDs and buzzer to a known-off state first: a floating pin driving them
    // during boot looks like a fault.
    pinMode(PIN_LED_STAT, OUTPUT); digitalWrite(PIN_LED_STAT, LED_OFF);
    pinMode(PIN_LED2, OUTPUT);     digitalWrite(PIN_LED2, LED_OFF);
    pinMode(PIN_BUZZ, OUTPUT);     digitalWrite(PIN_BUZZ, LOW);
    pinMode(PIN_DAC_LDAC_B, OUTPUT); digitalWrite(PIN_DAC_LDAC_B, HIGH);

    // ADC FIRST, before anything reads it. Adc::Begin() creates the oneshot unit
    // and the calibration handle; every Adc::Read* returns false until it runs.
    // Omitting it is silent in the worst way -- the unit is never created, so
    // ReadMv returns false and every caller's `mv` stays at its initialized 0, so
    // all 30 tests would measure 0 mV and report the board as comprehensively
    // broken. Nothing warns: Adc::Begin has external linkage, so -Wunused-function
    // cannot fire.
    const bool cal_ok = Adc::Begin();
    if (!cal_ok) {
        Log::Printf("ADC: no eFuse calibration available (degraded -- see test 7)");
    }
    Log::Printf("ADC bring-up: %s", Adc::CalibrationSourceName());

    // Sanity: prove the ADC actually answers before handing over to the tests,
    // so a failure here is reported as an ADC problem rather than as 30 tests
    // each independently measuring nothing.
    uint32_t probe_mv = 0;
    if (!Adc::ReadAvgMv(Adc::kSwc1, 8, &probe_mv)) {
        Log::Printf("ADC: FAILED to read IO%d -- every analog test will read 0 mV",
                    PIN_SWC1_ADC);
        Log::Printf("     run test 7 first; it reports the calibration state");
    } else {
        Log::Printf("ADC: IO%d reads %u mV (ADC is live)", PIN_SWC1_ADC, probe_mv);
    }

    const bool dac_ok = Dac::Begin();
    Log::Printf("DAC at 0x%02X: %s", Dac::Address(), dac_ok ? "present" : "NOT FOUND");
    if (!dac_ok) {
        Log::Printf("  -> run test 4 (I2C scan); every test 5+ needs the DAC");
    }

    // A boot indication the operator can see and hear with nothing attached.
    for (int i = 0; i < 2; ++i) {
        digitalWrite(PIN_LED_STAT, LED_ON); digitalWrite(PIN_LED2, LED_ON);
        delay(120);
        digitalWrite(PIN_LED_STAT, LED_OFF); digitalWrite(PIN_LED2, LED_OFF);
        delay(120);
    }

    // The menu FIRST, so a monitor attached at power-on sees it immediately. Doing
    // the WiFi association first meant up to 15 s of silence on a board that was
    // working perfectly, which looks identical to a hung one.
    PrintMenu();

    WebBegin();

    // And a final prompt, so the last thing on screen is what to do next.
    Log::Printf("type a NUMBER (1-30), 'a' for all, or '?' for help");
}

void Loop()
{
    if (ServiceWebRequest()) {
        PrintMenu();
    }
    ServiceSerial();
    if (s_web_up) s_http.handleClient();
}

}  // namespace FrontEnd
