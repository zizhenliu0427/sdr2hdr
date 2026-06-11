#pragma once

#include "../src/engine.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace gui {

enum class JobState
{
    Pending,
    Processing,
    Done,
    Failed,
    Cancelled
};

struct JobItem
{
    std::wstring inputPath;
    std::wstring outputPath;
    JobState state = JobState::Pending;
    double progress = 0.0;
    double fps = 0.0;
    bool finalizing = false;   // post-encode mux/verify step (indeterminate bar)
    std::wstring statusText;
    std::wstring errorText;
    std::chrono::steady_clock::time_point startedAt{};
    std::chrono::steady_clock::time_point finalizeStartedAt{};  // for audio-merge elapsed
};

// Runs jobs sequentially on a background thread; marshals UI updates via callback.
class JobQueue
{
public:
    using UiUpdateFn = std::function<void(size_t index, const JobItem&)>;

    void setOptions(sdr2hdr::ProcessOptions opts) { m_opts = std::move(opts); }
    const sdr2hdr::ProcessOptions& options() const { return m_opts; }

    size_t addJob(std::wstring input, std::wstring output);
    // Removes the job whose input path matches; refuses if it is mid-conversion.
    bool removeInput(const std::wstring& inputPath);
    void clear();
    size_t count() const;
    std::vector<JobItem> snapshot() const;

    void start(UiUpdateFn onUpdate);
    // Stop the whole queue: aborts the in-flight conversion and skips the rest.
    void cancel();
    // Abort only the job currently converting (queue ✕ on a running row);
    // the worker moves on to the next pending job.
    void cancelCurrent();
    // App shutdown: cancel everything and wait (bounded) for the worker to
    // tear down -- the pipeline deletes its temp/partial files on cancel.
    void shutdown(unsigned waitMs = 8000);
    bool isRunning() const { return m_running.load(); }

    ~JobQueue() { shutdown(2000); }

private:
    void workerLoop(UiUpdateFn onUpdate);
    void updateJob(size_t i, UiUpdateFn& onUpdate);
    // Run one conversion in an out-of-process worker (the console engine).
    // Crash isolation: NVIDIA's NGX init has a stack-smashing bug on hybrid
    // -graphics machines that fast-fails the whole process; in a worker it
    // becomes a "Failed" row instead of taking the GUI down.
    sdr2hdr::ProcessResult runWorker(const std::wstring& workerExe,
                                     size_t jobIndex,
                                     const std::wstring& input,
                                     const std::wstring& output,
                                     const std::function<void(const sdr2hdr::ProcessProgress&)>& progressCb);

    mutable std::mutex m_mutex;
    std::vector<JobItem> m_jobs;
    sdr2hdr::ProcessOptions m_opts{};
    std::thread m_worker;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_cancelCurrent{false};
    void* m_curCancelEvent = nullptr;   // HANDLE of the running job's cancel event
};

std::string wideToUtf8(const std::wstring& w);
std::wstring utf8ToWide(const std::string& s);

} // namespace gui
