// MantisCubeSolver — background solver task.
//
// The solver runs on core 0 so the UI task on core 1 keeps rendering at full
// rate. Nothing here is in the portable core: cube_core, cube_endgame and
// cube_policy have no Arduino dependency and host-compile unchanged. This
// file is the only Arduino-aware part of the solver.
//
// The search yields every few thousand states, which feeds the watchdog and
// publishes progress. There is no dynamic allocation: the task stack is the
// only thing FreeRTOS creates, and the solver arena is static.
#pragma once

#include "cube_core.h"
#include "cube_policy.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

struct McSolverJob {
    McCube start;
    McSolution solution;
    McProgress progress;
    McPolicyOptions options;
    volatile bool running;
    volatile bool cancel;
    volatile bool complete;
    uint32_t startedMs;
    uint32_t finishedMs;
};

#ifdef ARDUINO

namespace {

McSolverJob* g_mcRunningJob = nullptr;
TaskHandle_t g_solverTask = nullptr;

bool mcTaskYield(McProgress& p, void* user) {
    McSolverJob* job = (McSolverJob*)user;
    (void)p;
    // One tick is enough to service the idle task and reset the watchdog.
    vTaskDelay(1);
    return !job->cancel;
}

void mcSolverEntry(void* param) {
    McSolverJob* job = (McSolverJob*)param;
    job->startedMs = millis();
    job->options.yieldFn = mcTaskYield;
    job->options.yieldUser = job;

    mcPolicySolve(job->start, job->solution, job->progress, job->options);

    job->finishedMs = millis();
    job->complete = true;
    job->running = false;
    g_solverTask = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

// Kick off a solve on core 0. Safe to call from the UI task.
inline bool mcSolveAsync(McSolverJob& job, const McCube& start,
                         const McPolicyOptions& opts,
                         uint32_t stackWords = 8192) {
    if (job.running) return false;

    job.start = start;
    job.options = opts;
    job.running = true;
    job.cancel = false;
    job.complete = false;
    job.solution.found = false;
    job.solution.length = 0;
    job.progress.done = false;
    job.progress.ply = 0;
    job.progress.phase = 0;
    job.progress.bestLength = 0;
    job.progress.statesSeen = 0;
    g_mcRunningJob = &job;

    BaseType_t ok = xTaskCreatePinnedToCore(
        mcSolverEntry, "mcSolve", stackWords, &job, 1, &g_solverTask, 0);
    if (ok != pdPASS) {
        job.running = false;
        return false;
    }
    return true;
}

inline void mcCancelAsync(McSolverJob& job) {
    if (job.running) job.cancel = true;
}

inline uint32_t mcElapsedMs(const McSolverJob& job) {
    if (!job.startedMs) return 0;
    return (job.complete ? job.finishedMs : millis()) - job.startedMs;
}

#else   // host build: run synchronously so the tkinter tab sees identical results

inline bool mcSolveAsync(McSolverJob& job, const McCube& start,
                         const McPolicyOptions& opts,
                         uint32_t stackWords = 0) {
    (void)stackWords;
    job.start = start;
    job.options = opts;
    job.running = true;
    job.cancel = false;
    job.complete = false;
    mcPolicySolve(job.start, job.solution, job.progress, job.options);
    job.running = false;
    job.complete = true;
    return true;
}

inline void mcCancelAsync(McSolverJob& job) { job.cancel = true; }
inline uint32_t mcElapsedMs(const McSolverJob&) { return 0; }

#endif

// Progress fraction for the UI, 0..1. Deliberately monotonic: it advances with
// frames completed and best-length found, never jumping backwards, because a
// bar that retreats reads as a crash.
inline float mcProgressFraction(const McSolverJob& job) {
    if (job.complete) return 1.0f;
    if (!job.running) return 0.0f;
    uint8_t frames = job.options.frames ? job.options.frames : 1;
    float byFrame = (float)job.progress.phase / (float)(frames + 1);
    float byPly = (float)job.progress.ply
                / (float)(job.options.maxDepth ? job.options.maxDepth : 24);
    float f = byFrame + byPly / (float)(frames + 1);
    return f > 0.99f ? 0.99f : f;
}
