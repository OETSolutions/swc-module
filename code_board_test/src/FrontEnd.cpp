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
#include "TestTask.h"
#include "driver/gpio.h"
#include "tests/SetupPrompts.h"
#include "swc_logic/Output.h"

// ---------------------------------------------------------------------------
// Web UI
// ---------------------------------------------------------------------------
static WebServer s_http(80);
static bool      s_web_up = false;

// Set by any HTTP handler that runs. The operator-wait pump (see WaitPump) uses it to
// treat "the operator clicked or refreshed" as "continue" -- the web UI has no
// keystroke, so a request is the equivalent signal.
static volatile bool s_continue_seen = false;
// The page is deliberately one self-contained string with no external assets: it
// must work on a car bench with no internet, and a CDN reference would fail
// exactly when the tool is most needed. It re-fetches the log on a timer rather
// than using websockets, because that costs nothing and has far fewer failure
// modes on a marginal link.
static const char kPageHead[] PROGMEM =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>SWC bring-up</title><style>"
    ":root{--bg:#0f1115;--fg:#e6e6e6;--dim:#8b93a1;--line:#252a33;--acc:#4aa3ff;"
    "--ok:#3ddc84;--bad:#ff5c5c;--warn:#ffc857;--panel:#161a21}"
    "*{box-sizing:border-box}"
    "body{font:13px/1.5 ui-monospace,Menlo,Consolas,monospace;background:var(--bg);"
    "color:var(--fg);margin:0;padding:0 16px 24px}"
    "h1{font-size:17px;margin:0;font-weight:600}"
    "h2{font-size:13px;margin:22px 0 8px;color:var(--dim);text-transform:uppercase;"
    "letter-spacing:.08em;font-weight:600}"
    "#bar{position:sticky;top:0;z-index:30;background:var(--panel);"
    "border-bottom:1px solid var(--line);padding:10px 14px;margin:0 -16px 14px;"
    "display:flex;align-items:center;gap:12px;flex-wrap:wrap}"
    "#dot{width:10px;height:10px;border-radius:50%;background:var(--dim);flex:0 0 auto}"
    ".busy #dot{background:var(--acc);animation:p 1s infinite}"
    "@keyframes p{0%,100%{opacity:1}50%{opacity:.25}}"
    "#head{font-weight:600}"
    "#sub{color:var(--dim)}"
    ".sp{flex:1}"
    "#act{display:none;background:#3a2f00;border:2px solid var(--warn);"
    "border-radius:8px;padding:14px 16px;margin:0 0 16px}"
    "#act.on{display:block;animation:glow 1.4s infinite}"
    "@keyframes glow{0%,100%{box-shadow:0 0 0 0 rgba(255,200,87,.5)}"
    "50%{box-shadow:0 0 0 8px rgba(255,200,87,0)}}"
    "#act .t{color:var(--warn);font-weight:700;font-size:14px;letter-spacing:.06em;"
    "text-transform:uppercase;margin-bottom:6px}"
    "#act .w{font-size:15px;margin-bottom:12px}"
    "button,a.b{background:#232a35;color:var(--fg);border:1px solid #39424f;"
    "border-radius:6px;padding:6px 12px;margin:2px 2px 2px 0;font:inherit;"
    "cursor:pointer;text-decoration:none;display:inline-block}"
    "button:hover:not(:disabled),a.b:hover{background:#2d3644;border-color:var(--acc)}"
    "button:disabled{opacity:.4;cursor:not-allowed}"
    "#go{background:var(--warn);color:#241c00;border-color:var(--warn);"
    "font-weight:700;font-size:15px;padding:10px 20px}"
    "button.t{background:transparent;border:1px solid var(--line);text-align:left;"
    "width:100%;padding:6px 8px}"
    "table{border-collapse:collapse;width:100%}td,th{border-bottom:1px solid var(--line);"
    "padding:5px 8px;text-align:left;vertical-align:top}"
    "th{color:var(--dim);font-weight:600;font-size:12px}"
    "tr.running{background:#12233a}"
    ".PASS{color:var(--ok);font-weight:600}.FAIL{color:var(--bad);font-weight:700}"
    ".BLOCKED{color:var(--warn)}.SKIP{color:var(--dim)}.WARN{color:var(--warn)}"
    ".NOTRUN{color:#525a68}.RUNNING{color:var(--acc);font-weight:700}"
    "pre{background:#0a0c10;border:1px solid var(--line);border-radius:6px;padding:10px;"
    "overflow:auto;max-height:52vh;white-space:pre-wrap;font:inherit;margin:0}"
    ".m{color:var(--dim);font-size:11px}"
    "</style></head><body>";

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
    h.reserve(26000);
    h += FPSTR(kPageHead);

    // ---- the always-visible status bar ---------------------------------
    h += "<div id=bar><span id=dot></span><span id=head>Ready</span>"
         "<span id=sub></span><span class=sp></span>";
    h += "<button id=bAll class=all onclick=\"runAll()\">Run all</button>";
    h += "<button onclick=\"fetch('/summary').catch(()=>{})\">Summary</button>";
    h += "<button onclick=\"fetch('/clear').then(()=>setTimeout(refreshLog,300))"
         ".catch(()=>{})\">Clear log</button>";
    h += "<a class=b href='/identify'>Identify board</a></div>";

    // ---- the action banner (hidden until a test asks for something) ----
    h += "<div id=act><div class=t>&#9888; Action required</div>"
         "<div class=w id=actw></div>"
         "<button id=go onclick=\"continue_()\">Done &mdash; continue &#9654;</button></div>";

    uint32_t tmv = 0;
    Adc::ReadAvgMv(Adc::kTemp, 16, &tmv);
    const float tc = Temp::CelsiusFromMv((float)tmv, 3300.0f);

    h += "<h2>Board</h2><div class=m>heap " + String(ESP.getFreeHeap() / 1024) +
         " kB &middot; NTC ";
    h += isnan(tc) ? String("n/a") : String(tc, 1) + " C";
    h += " &middot; DAC 0x" + String(Dac::Address(), HEX) + " " +
         String(Dac::Present() ? "present" : "ABSENT");
    h += " &middot; ADC " +
         String(Adc::CalibrationDegraded() ? "linear fallback" : "eFuse cal");
    h += "</div>";

    // ---- the tests -----------------------------------------------------
    h += "<h2>Tests</h2><table><tr><th style='width:34px'>#</th><th>Test</th>"
         "<th>Needs</th><th style='width:90px'>Result</th><th>Detail</th></tr>";

    for (size_t i = 0; i < TestRunner::Count(); ++i) {
        const TestRunner::Test *t = TestRunner::Get(i);
        if (!t) continue;
        const TestRunner::Outcome &o = TestRunner::LastOutcome(i);
        const bool running = (TestTask::RunningIndex() == (int)i);

        h += "<tr id=r" + String(t->number) + (running ? " class=running>" : ">");
        h += "<td>" + String(t->number) + "</td>";
        h += "<td><button class=t onclick='runTest(" + String(t->number) + ")'>" +
             String(t->title) + "</button>";
        h += "<div class=m>" + String(t->covers ? t->covers : "") + "</div></td>";
        h += "<td class=m>" + String(t->needs ? t->needs : "") + "</td>";
        h += "<td class=" + String(running ? "RUNNING" : statusClass(o.result)) + ">" +
             String(running ? "RUNNING" : TestRunner::ResultName(o.result));
        if (o.duration_ms && !running)
            h += "<div class=m>" + String(o.duration_ms) + " ms</div>";
        h += "</td><td>" + String(o.summary) + "</td></tr>";
    }
    h += "</table>";

    h += "<h2>Log <button id=jump style='display:none;font-size:11px;padding:2px 8px' "
         "onclick='jumpBottom()'>&#9660; jump to latest</button></h2>"
         "<pre id=l>waiting for the first output...</pre>";

    // ---- the logic -----------------------------------------------------
    //
    // The page NEVER reloads while work is in flight and never assumes a click
    // landed: it polls /state and re-renders from the server's own answer. A dropped
    // request therefore self-corrects on the next poll instead of leaving the page
    // stuck -- and because tests now run on their own task, the server answers
    // normally even while a test is running or waiting for the operator.
    h += "<script>";
    h += "var polling=false,lastRunning=-1,done=0;";
    h += "function el(i){return document.getElementById(i)}";
    h += "function setBar(busy,run,res,prompt){";
    h += "var bar=document.querySelector('#bar');";
    h += "bar.className=busy?'busy':'';";
    h += "el('head').textContent=busy?('Running test '+(run+1)):'Ready';";
    h += "el('sub').textContent=busy?('&nbsp;'+(res-done)+' of 1 finished this run')"
         ".replace('&nbsp;','')"
         ":((res>0)?(res+' test'+(res==1?'':'s')+' completed this session')"
         ":'Pick a test below');";
    h += "el('bAll').disabled=busy;";
    h += "var a=el('act');";
    h += "if(prompt&&prompt.length){el('actw').textContent=prompt;a.className='on';}";
    h += "else{a.className='';}";
    h += "}";
    // THE LOG FOLLOWS ONLY IF YOU ARE ALREADY AT THE BOTTOM.
    //
    // The first version forced scrollTop to scrollHeight on every 1.2 s poll, so
    // scrolling up was impossible -- the poll yanked you back down before you could
    // read anything. Now the pane is only auto-scrolled when it was already at (or
    // within ~24 px of) the bottom, which is the standard "tail -f" behaviour: it
    // follows the output while you watch it, and leaves you alone the moment you
    // scroll up to read something.
    //
    // A "jump to latest" affordance is offered instead of forcing it: when you are
    // scrolled away from the bottom, the bar shows it and clicking returns you.
    h += "function atBottom(e){return e.scrollHeight-e.scrollTop-e.clientHeight<24}";
    h += "function refreshLog(){fetch('/log?raw=1',{cache:'no-store'})"
         ".then(r=>r.text()).then(t=>{"
         "var e=el('l');"
         "var follow=atBottom(e);"          // decide BEFORE replacing the text
         "if(e.textContent!==t){e.textContent=t;}"
         "if(follow)e.scrollTop=e.scrollHeight;"
         "var away=!atBottom(e);"
         "el('jump').style.display=away?'inline-block':'none';"
         "}).catch(()=>{})}";
    h += "function jumpBottom(){var e=el('l');e.scrollTop=e.scrollHeight;"
         "el('jump').style.display='none';}";
    h += "function poll(){";
    h += "fetch('/state',{cache:'no-store'}).then(r=>r.json()).then(s=>{";
    h += "setBar(s.busy,s.running,s.results,s.prompt);";
    h += "if(s.running!==lastRunning){lastRunning=s.running;"
         "if(s.running<0){location.reload();return;}}";   // one reload, only when a test ENDS
    h += "}).catch(()=>{});";
    h += "refreshLog();";
    h += "setTimeout(poll,1200);";
    h += "}";
    h += "function runTest(n){";
    h += "el('act').className='';";
    h += "el('head').textContent='Starting test '+n+'...';";
    h += "fetch('/run?n='+n).then(r=>r.text()).then(t=>{";
    h += "if(t!=='started')setBar(false,-1,0,'');";
    h += "}).catch(()=>{});}";
    h += "function runAll(){el('head').textContent='Starting all tests...';"
         "fetch('/runall').catch(()=>{});}";
    h += "function continue_(){el('act').className='';"
         "el('head').textContent='Continuing...';"
         "fetch('/continue').catch(()=>{});}";
    h += "poll();";
    h += "</script></body></html>";

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
    bool ok = false;
    if (s_http.hasArg("n")) {
        ok = TestTask::RequestByNumber(s_http.arg("n").toInt());
    }
    s_http.send(200, "text/plain", ok ? "started" : "busy");
}

static void handleRunAll()
{
    s_http.send(200, "text/plain", TestTask::RequestAll() ? "started" : "busy");
}

// The operator says "I did it". Only meaningful while a test is waiting.
static void handleContinue()
{
    TestTask::SignalContinue();
    s_http.send(200, "text/plain", "ok");
}

// The page's status poll. Returns a tiny JSON object: the page re-renders its state
// from this, so it always reflects reality even if a click was missed.
static void handleState()
{
    // A tiny JSON status object. The page polls this and re-renders from it, so its
    // display always reflects reality -- if a click was dropped or a request timed
    // out, the next poll corrects the page rather than leaving it stuck.
    String j;
    j.reserve(320);
    j += "{\"busy\":";
    j += TestTask::Busy() ? "true" : "false";
    j += ",\"running\":" + String(TestTask::RunningIndex());
    j += ",\"results\":" + String((int)TestRunner::CompletedCount());
    j += ",\"prompt\":\"";
    // The prompt is operator-facing text going into JSON; strip the two characters
    // that would break the string. It is our own text, not untrusted input.
    const char *pr = TestTask::CurrentPrompt();
    for (const char *c = pr; c && *c; ++c) {
        if (*c == '"' || *c == '\\') j += '\\';
        if (*c != '\n') j += *c;
    }
    j += "\"}";
    s_http.send(200, "application/json", j);
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

// The operator says "I did it, go on". This is the ONLY request that continues a
// waiting test: the page's own poll must not, or the wait would satisfy itself
// without the operator having done anything.
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
    s_http.on("/continue", handleContinue);
    s_http.on("/state", handleState);
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

// The AUX pin probe, done with the ADC and the IDF GPIO pull API.
//
// THE FIRST VERSION OF THIS WAS INVALID and it is worth saying why, because it very
// nearly produced a false hardware verdict. It used pinMode()+digitalRead() to test
// whether each pin followed the internal pull -- but these pins are ATTACHED TO THE
// ADC PERIPHERAL (we configured them with adc_oneshot), and while that attachment
// holds, digitalRead does not reflect the pad. The tell was that it called SWC1
// "stuck HIGH under pull-down" -- and SWC1 is a known-good pin that reads 3173 mV and
// passes every loopback test. A probe that contradicts a known-good pin is a broken
// probe.
//
// So the pull is applied with gpio_set_pull_mode(), which acts on the pad directly and
// coexists with the ADC mux, and the RESULT is read with the ADC -- the instrument
// that is actually trusted here.
//
// What it distinguishes, using the fact that the internal pull-up is ~45k against the
// board's external 10k (R17/R18/R19):
//
//   * internal pull-up raises the reading  -> the pin is ALIVE and reachable. If it
//     was low WITHOUT the pull, the external 10k pull-up is missing or open -- that is
//     the difference between "R17 absent" and "shorted to GND".
//   * internal pull-up changes NOTHING    -> something external is holding the node
//     down hard (a short), or the pin is damaged.
static void ProbeAuxPins()
{
    Log::Section("AUX PIN PROBE (internal pull-up via ADC)");

    struct Row { Adc::Ch ch; uint8_t pin; const char *name; const char *ext; };
    const Row rows[] = {
        {Adc::kAux1, PIN_AUX1, "AUX1", "R17"},
        {Adc::kAux2, PIN_AUX2, "AUX2", "R18"},
        {Adc::kAux3, PIN_AUX3, "AUX3", "R19"},
        {Adc::kSwc1, PIN_SWC1_ADC, "SWC1 (known good)", "R15"},
    };

    Log::Printf("  internal pull-up is ~45k; the board's external pull-up is 10k.");
    Log::Printf("  If enabling the internal pull RAISES a low reading, the external 10k");
    Log::Printf("  is missing/open. If it changes nothing, the node is shorted down.");
    Log::Printf("");
    Log::Printf("  %-18s %-12s %-12s %s", "pin", "pull off mV", "pull up mV", "verdict");

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        // Read with no internal pull (the resting state the board's own 10k sets).
        gpio_set_pull_mode((gpio_num_t)rows[i].pin, GPIO_FLOATING);
        delay(20);
        uint32_t off = 0;
        Adc::ReadAvgMv(rows[i].ch, 64, &off);

        // Now add the ~45k internal pull-up.
        gpio_set_pull_mode((gpio_num_t)rows[i].pin, GPIO_PULLUP_ONLY);
        delay(20);
        uint32_t on = 0;
        Adc::ReadAvgMv(rows[i].ch, 64, &on);

        // Restore to the ADC's normal (no-pull) state.
        gpio_set_pull_mode((gpio_num_t)rows[i].pin, GPIO_FLOATING);
        delay(10);

        const long delta = (long)on - (long)off;
        const char *verdict;
        if (off > 2500) {
            verdict = "rests HIGH already (external pull present)";
        } else if (delta > 500) {
            verdict = "RISES with the internal pull -> EXTERNAL PULL-UP MISSING/OPEN";
        } else if (off < 200 && delta < 100) {
            verdict = "held down hard -> SHORTED TO GND (or pin damaged)";
        } else {
            verdict = "partially pulled -> external path present but weak";
        }
        Log::Printf("  %-18s %-12u %-12u %s", rows[i].name, off, on, verdict);
        if (i < 3 && delta > 500) {
            Log::Printf("      -> check %s (10k to 3V3) and its solder joint", rows[i].ext);
        }
    }

    Log::Printf("");
    Log::Printf("  SWC1 is the control: it is known good, so if IT reports anything other");
    Log::Printf("  than 'rests HIGH', this probe is not measuring what it thinks it is.");

    // ---------------------------------------------------------------------
    // The AUX test rig, line by line.
    //
    // Test 31 drives these three spare pins to pull the AUX inputs down. When only
    // some inputs respond, the cause is one of: the wire is on the wrong test point,
    // the spare pin cannot drive, or the AUX input cannot be pulled. Those are very
    // different and the AUX reading alone cannot separate them -- so this drives each
    // spare pin HIGH and LOW and reports BOTH ends: what the spare pin does, and what
    // the AUX input does in response.
    // ---------------------------------------------------------------------
    // ---------------------------------------------------------------------
    // The AUX test rig: the full 3x3 RESPONSE MATRIX.
    //
    // Which spare pin pulls which AUX input is assumed by test 31, and if that
    // assumption is wrong (a wire one test point over, or two wires swapped) the test
    // reports "no response" for a line that is perfectly healthy -- it is just on a
    // different point than the code believes.
    //
    // So this drives each spare pin LOW in turn, one at a time, and records which AUX
    // inputs move. The resulting matrix says exactly how the rig is actually wired,
    // which is a fact the ADC can measure and my assumptions cannot.
    // ---------------------------------------------------------------------
    Log::Printf("");
    Log::Printf("  AUX test rig -- which spare pin pulls which AUX input?");
    Log::Printf("  Driving each spare pin LOW, one at a time:");
    Log::Printf("");

    const uint8_t stimpins[3] = {PIN_AUX_STIM1, PIN_AUX_STIM2, PIN_AUX_STIM3};
    const char *stimnames[3] = {"IO16/TP5", "IO21/TP6", "IO43/TP7"};
    const Adc::Ch auxch[3] = {Adc::kAux1, Adc::kAux2, Adc::kAux3};
    const char *auxnames[3] = {"AUX1/J5.4", "AUX2/J5.3", "AUX3/J5.2"};

    // Baseline: everything floating.
    for (int k = 0; k < 3; ++k) pinMode(stimpins[k], INPUT);
    delay(30);
    uint32_t base[3];
    for (int k = 0; k < 3; ++k) Adc::ReadAvgMv(auxch[k], 32, &base[k]);

    Log::Printf("  %-11s %-12s %-12s %-12s", "spare pin", "AUX1 mV", "AUX2 mV", "AUX3 mV");
    Log::Printf("  %-11s %-12u %-12u %-12u", "(all float)", base[0], base[1], base[2]);

    int pulled[3] = {-1, -1, -1};      // for each AUX, which stim pin pulled it
    for (int k = 0; k < 3; ++k) {
        gpio_reset_pin((gpio_num_t)stimpins[k]);
        gpio_set_pull_mode((gpio_num_t)stimpins[k], GPIO_FLOATING);
        pinMode(stimpins[k], OUTPUT);
        digitalWrite(stimpins[k], HIGH);
        delay(5);
        const int rb_hi = digitalRead(stimpins[k]);
        digitalWrite(stimpins[k], LOW);
        delay(30);
        const int rb_lo = digitalRead(stimpins[k]);
        uint32_t mv[3];
        for (int j = 0; j < 3; ++j) Adc::ReadAvgMv(auxch[j], 32, &mv[j]);
        gpio_set_pull_mode((gpio_num_t)stimpins[k], GPIO_FLOATING);
        pinMode(stimpins[k], INPUT);

        // Reading the OUTPUT pin back separates "this pin cannot drive" from "the
        // wire is not connected": a pin that will not follow itself is a pad problem,
        // and one that follows itself but moves no AUX input is a wiring problem.
        Log::Printf("  %-11s %-12u %-12u %-12u   (pin reads back HIGH=%d LOW=%d -> %s)",
                    stimnames[k], mv[0], mv[1], mv[2], rb_hi, rb_lo,
                    (rb_hi == 1 && rb_lo == 0) ? "pin drives" : "PIN WILL NOT DRIVE");
        for (int j = 0; j < 3; ++j) {
            if (base[j] > 1500 && mv[j] < 1200) pulled[j] = k;
        }
    }

    Log::Printf("");
    // Report the wiring as measured, and flag any disagreement with what test 31
    // assumes (stim k <-> AUX k).
    for (int j = 0; j < 3; ++j) {
        if (pulled[j] < 0) {
            Log::Printf("  %s: NOT pulled by any spare pin -- no wire, or the wire is "
                        "on an untested point.", auxnames[j]);
        } else if (pulled[j] == j) {
            Log::Printf("  %s: pulled by %s  (as expected)", auxnames[j],
                        stimnames[pulled[j]]);
        } else {
            Log::Printf("  %s: pulled by %s  <-- SWAPPED, test 31 expects %s",
                        auxnames[j], stimnames[pulled[j]], stimnames[j]);
        }
    }
    Log::Printf("");
    Log::Printf("  Test 31 DISCOVERS this mapping itself, so the order does not matter and");
    Log::Printf("  a swapped wire is not a fault. What this matrix is for:");
    Log::Printf("    - an input pulled by NO pin  -> that wire is missing or on a bad point");
    Log::Printf("    - two inputs pulled by the SAME pin -> a short between them");
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
    Log::Printf("  p  = AUX pin probe (internal pull-up/down differential)");
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

static bool s_greeted = false;

static void ServiceSerial()
{
    if (!Serial.available()) return;

    // A test waiting for the operator OWNS the serial input. Both tasks read the same
    // UART, so without this the menu handler consumes the keystroke the test is
    // waiting for: it looks like ENTER does nothing and the test times out anyway.
    if (TestTask::WaitingForOperator()) return;

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

        // Through the TASK, exactly as the web page does. One execution path, so the
        // two front ends cannot behave differently -- and a direct call here would
        // run the test on the web/loop task, which is what locked the page up.
        if (TestTask::RequestByNumber((int)num)) {
            Log::Printf("starting test %ld...", num);
        } else if (TestTask::Busy()) {
            Log::Printf("a test is already running -- wait for it to finish");
        } else {
            Log::Printf("no test numbered %ld (see the menu)", num);
        }
        return;
    }

    switch (c) {
        case 'a': case 'A':
            if (TestTask::RequestAll()) {
                Log::Printf("running all tests...");
            } else {
                Log::Printf("a test is already running -- wait for it to finish");
            }
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
        case 'p': case 'P':
            ProbeAuxPins();
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

    // The test task MUST be created before anything can submit a job to it. Omitting
    // this is silent in the worst way: s_jobs stays null, every submit returns false,
    // and the web page reports "busy" forever while the serial menu does nothing --
    // which reads as a dead board rather than as a missing init. Same class of defect
    // as the Adc::Begin() omission found earlier in this project.
    TestTask::Begin();
    if (!TestTask::Ready()) {
        Log::Printf("TESTS: the test task FAILED to start -- no test can run");
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
    // The web server and the serial menu ONLY. Tests run on their own task
    // (include/TestTask.h), so this loop stays responsive even while a test is
    // running or waiting for the operator -- which is the whole point of the
    // split. Nothing here may block.
    ServiceSerial();
    if (s_web_up) s_http.handleClient();
    delay(2);
}

}  // namespace FrontEnd
