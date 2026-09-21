#include "TestTask.h"

#include <Arduino.h>
#include <string.h>

#include "Log.h"
#include "TestRunner.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

namespace TestTask {

// What the task has been asked to do.
enum class Job : int { kNone = -1, kAll = -2 };

static QueueHandle_t s_jobs = nullptr;
static TaskHandle_t  s_task = nullptr;

static volatile int  s_running = -1;    // test index in flight, or -1
static volatile bool s_busy = false;

// The operator prompt. Written by both tasks, read by both -- guarded by the log's
// recursive mutex, which is already held on every path that touches it.
static char s_prompt[160] = {0};
static volatile bool s_continue = false;

void SignalContinue() { s_continue = true; }

const char *CurrentPrompt() { return s_prompt; }

int  RunningIndex() { return s_running; }
bool Busy()         { return s_busy; }

// ---------------------------------------------------------------------------
// The prompt
// ---------------------------------------------------------------------------
bool AskOperator(const char *what, uint32_t timeout_ms)
{
    Log::Lock();
    strncpy(s_prompt, what ? what : "", sizeof(s_prompt) - 1);
    s_prompt[sizeof(s_prompt) - 1] = '\0';
    Log::Unlock();

    // Show it loudly on BOTH front ends. On serial it is the only signal; on the page
    // it is rendered as a banner, so this line exists mostly for the serial console.
    Log::Printf("");
    Log::Rule('*');
    Log::Printf("  ACTION REQUIRED: %s", s_prompt);
    Log::Printf("  then press ENTER here, or click Continue on the web page");
    Log::Rule('*');
    Log::Printf("");

    // Clear any keystroke or click that arrived BEFORE the prompt, so a stray input
    // cannot satisfy it silently.
    while (Serial.available()) Serial.read();
    s_continue = false;

    const uint32_t t0 = millis();
    while (millis() - t0 < timeout_ms) {
        if (Serial.available()) {
            while (Serial.available()) Serial.read();
            s_prompt[0] = '\0';
            return true;
        }
        if (s_continue) {
            s_continue = false;
            s_prompt[0] = '\0';
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    Log::Printf("  (no response within %u s -- continuing without it)",
                (unsigned)(timeout_ms / 1000));
    s_prompt[0] = '\0';
    return false;
}

// ---------------------------------------------------------------------------
// The task
// ---------------------------------------------------------------------------
static void RunJob(int job)
{
    if (job == (int)Job::kAll) {
        Log::Printf("");
        Log::Printf("=== running ALL tests, requested from the %s ===", "front end");
        for (size_t i = 0; i < TestRunner::Count(); ++i) {
            s_running = (int)i;
            TestRunner::Run(i);
        }
        TestRunner::PrintSummary();
    } else if (job >= 0 && job < (int)TestRunner::Count()) {
        s_running = job;
        TestRunner::Run((size_t)job);
    }
    s_running = -1;
    s_busy = false;
}

static void TaskBody(void *)
{
    for (;;) {
        int job = (int)Job::kNone;
        // Block until asked. A long timeout is fine: this task does nothing when idle.
        if (xQueueReceive(s_jobs, &job, portMAX_DELAY) == pdTRUE) {
            RunJob(job);
        }
    }
}

bool Ready() { return s_jobs != nullptr && s_task != nullptr; }

void Begin()
{
    if (s_jobs) return;                 // idempotent
    s_jobs = xQueueCreate(4, sizeof(int));
    if (!s_jobs) {
        Log::Printf("TESTS: could not create the job queue");
        return;
    }

    // A generous stack: the test bodies use snprintf into local buffers, the NVS and
    // WiFi tests make library calls, and a stack overflow here would crash mid-suite
    // in a way that looks like a hardware fault. 8 KB costs nothing on a part with
    // 320 KB of RAM.
    //
    // Priority 1, same as the Arduino loop task: these tests are latency-insensitive
    // and must not starve the WiFi stack, which runs above both.
    if (xTaskCreatePinnedToCore(TaskBody, "tests", 8192, nullptr, 1, &s_task, 1) != pdPASS) {
        s_task = nullptr;
        Log::Printf("TESTS: could not create the test task (out of memory?)");
    }
}

static bool Submit(int job)
{
    if (!s_jobs) return false;
    if (s_busy) return false;              // one at a time
    s_busy = true;
    if (xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        s_busy = false;
        return false;
    }
    return true;
}

bool RequestByNumber(int number)
{
    for (size_t i = 0; i < TestRunner::Count(); ++i) {
        const TestRunner::Test *t = TestRunner::Get(i);
        if (t && t->number == number) return Submit((int)i);
    }
    return false;
}

bool RequestAll() { return Submit((int)Job::kAll); }

}  // namespace TestTask
