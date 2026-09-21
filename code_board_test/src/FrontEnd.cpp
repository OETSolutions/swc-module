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
    h += "<button class=all onclick=\"fetch('/runall').then(refresh)\">Run all 30</button>";
    h += "<button class=sum onclick=\"fetch('/summary').then(refresh)\">Summary</button>";
    h += "<button class=sum onclick=\"fetch('/clear').then(refresh)\">Clear log</button>";
    h += "<a class=b href='/log'>Log only</a>";
    h += "<a class=b href='/identify'>Identify (blink + beep)</a>";
    h += "</div>";

    h += "<h2>Tests</h2><table><tr><th>#</th><th>Test</th><th>Needs</th>"
         "<th>Result</th><th>Detail</th></tr>";

    for (size_t i = 0; i < TestRunner::Count(); ++i) {
        const TestRunner::Test *t = TestRunner::Get(i);
        if (!t) continue;
        const TestRunner::Outcome &o = TestRunner::LastOutcome(i);

        h += "<tr><td>" + String(t->number) + "</td>";
        h += "<td><a href='/run?n=" + String(t->number) + "'>" + String(t->title) + "</a>";
        h += "<div class=m>" + String(t->covers ? t->covers : "") + "</div></td>";
        h += "<td class=m>" + String(t->needs ? t->needs : "") + "</td>";
        h += "<td class=" + statusClass(o.result) + ">" + TestRunner::ResultName(o.result);
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
    size_t start = (len > kTailBytes) ? (len - kTailBytes) : 0;
    // Start on a line boundary so the first visible line is not a fragment.
    while (start < len && cap[start] != '\n') ++start;
    if (start < len) ++start;
    if (start > 0) h += "(... earlier lines omitted; use the Log-only page)\n";

    // Escape the three characters that would break out of the <pre>.
    for (size_t i = start; i < len; ++i) {
        const char ch = cap[i];
        if (ch == '<') h += "&lt;";
        else if (ch == '>') h += "&gt;";
        else if (ch == '&') h += "&amp;";
        else h += ch;
    }
    h += "</pre>";

    h += "<script>function refresh(){location.reload()}";
    h += "function poll(){fetch('/log?raw=1').then(r=>r.text()).then(t=>{"
         "const e=document.getElementById('l');if(e.textContent!==t)e.textContent=t;})"
         ".catch(()=>{}).then(()=>setTimeout(poll,2000))}";
    h += "setTimeout(poll,2000)</script>";
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
    // Running the test inside the HTTP handler would block the response for as
    // long as the test takes (test 30 runs for minutes). So the request just
    // schedules it and redirects; loop() runs it and the page picks up the log.
    s_http.sendHeader("Location", "/");
    s_http.send(303, "text/plain", "");
}

static void handleRunAll()
{
    // Same deferral as handleRun, for the same reason.
    s_web_requested = -2;   // sentinel: run everything
    s_http.sendHeader("Location", "/");
    s_http.send(303, "text/plain", "");
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
    Log::Printf("  h  = hardware info dump");
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

static void ServiceSerial()
{
    if (!Serial.available()) return;
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
            if (s_web_up) {
                Log::Printf("web UI: http://%s/", WiFi.localIP().toString().c_str());
            } else {
                Log::Printf("web UI is not up (no WiFi, or it failed -- see test 25)");
            }
            break;
        case '?':
            PrintDetails();
            break;
        case 'h': case 'H':
            PrintHardware();
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

    Log::Printf("ADC bring-up: %s", Adc::CalibrationSourceName());
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

    WebBegin();
    PrintMenu();
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
