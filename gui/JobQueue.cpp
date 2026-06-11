#include "pch.h"
#include "JobQueue.h"

#include "../src/path_utils.h"
#include "../src/deps.h"
#include "../src/i18n.h"
#include "../src/win32_utils.h"

#include <Windows.h>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace gui {

namespace {

// CLI flags equivalent to the given ProcessOptions (mirrors parseArgs in
// main.cpp). Built in UTF-8, converted to wide with the paths at the end.
std::string optionsToCliFlags(const sdr2hdr::ProcessOptions& o)
{
    std::ostringstream f;
    switch (o.mode)
    {
        case RtxConverter::Mode::Vsr:    f << " --vsr";     break;
        case RtxConverter::Mode::VsrHdr: f << " --vsr-hdr"; break;
        default:                         f << " --hdr";     break;
    }
    if (o.mode != RtxConverter::Mode::Hdr)
    {
        if      (o.outputSizeSet)        f << " --output-size " << o.outW << "x" << o.outH;
        else if (o.targetHeight == 1080) f << " --1080p";
        else if (o.targetHeight == 4320) f << " --8k";
        else if (o.scaleSet)             f << " --scale " << o.scale;
        // (default / targetHeight 2160 == --4k, which is the CLI default)
        f << " --vsr-quality " << o.vsrQuality;
    }
    if (o.mode != RtxConverter::Mode::Vsr)
    {
        f << " --max-lum "     << o.maxLum
          << " --contrast "    << o.contrast
          << " --saturation "  << o.saturation
          << " --middle-grey " << o.middleGray;
    }
    f << " --backend " << o.backend
      << " --codec "   << o.codec;
    if (o.qualityAuto)        f << " --quality auto";
    else if (o.quality >= 0)  f << " --quality " << o.quality;
    if (!o.preset.empty())    f << " --preset " << o.preset;
    if (!o.copyAudio)         f << " --no-audio";
    if (!o.hwDecode)          f << " --no-hw-decode";
    if (!o.gpuOnly)           f << " --legacy";
    if (!o.vfrPts)            f << " --no-vfr-pts";
    if (o.ngxSessions > 0)    f << " --ngx-sessions " << o.ngxSessions;
    if (o.verbose)            f << " --verbose";
    return f.str();
}

// The console-engine worker shipped next to the GUI exe.
std::wstring workerExePath()
{
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    const auto slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    p = p.substr(0, slash + 1) + L"sdr2hdr_cli.exe";
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES ? p : std::wstring{};
}

} // namespace

std::string wideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), len, nullptr, nullptr);
    return s;
}

std::wstring utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    return w;
}

size_t JobQueue::addJob(std::wstring input, std::wstring output)
{
    std::lock_guard lock(m_mutex);
    JobItem j;
    j.inputPath  = std::move(input);
    j.outputPath = std::move(output);
    j.statusText = L"Pending";
    m_jobs.push_back(std::move(j));
    return m_jobs.size() - 1;
}

bool JobQueue::removeInput(const std::wstring& inputPath)
{
    std::lock_guard lock(m_mutex);
    for (auto it = m_jobs.begin(); it != m_jobs.end(); ++it)
    {
        if (it->inputPath == inputPath)
        {
            // Never yank a job that's actively converting on the worker thread.
            if (it->state == JobState::Processing) return false;
            m_jobs.erase(it);
            return true;
        }
    }
    return false;
}

void JobQueue::clear()
{
    if (m_running.load()) return;
    std::lock_guard lock(m_mutex);
    m_jobs.clear();
}

size_t JobQueue::count() const
{
    std::lock_guard lock(m_mutex);
    return m_jobs.size();
}

std::vector<JobItem> JobQueue::snapshot() const
{
    std::lock_guard lock(m_mutex);
    return m_jobs;
}

void JobQueue::start(UiUpdateFn onUpdate)
{
    if (m_running.exchange(true)) return;
    m_cancel = false;
    m_cancelCurrent = false;
    if (m_worker.joinable()) m_worker.join();
    m_worker = std::thread([this, cb = std::move(onUpdate)]() mutable {
        workerLoop(cb);
        m_running = false;
    });
}

void JobQueue::cancel()
{
    m_cancel = true;
    cancelCurrent();          // also abort the conversion in flight
}

void JobQueue::cancelCurrent()
{
    m_cancelCurrent = true;
    // Worker-process jobs listen on a named event rather than the atomic.
    std::lock_guard lock(m_mutex);
    if (m_curCancelEvent) SetEvent(static_cast<HANDLE>(m_curCancelEvent));
}

void JobQueue::shutdown(unsigned waitMs)
{
    cancel();
    // Give the pipeline a moment to tear down cleanly (it deletes its temp
    // video-only file and the partial output on cancel). If it doesn't make
    // it in time, the kill-on-close job object still takes the ffmpeg
    // children down with the process.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
    while (m_running.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (!m_running.load())
    {
        if (m_worker.joinable()) m_worker.join();
    }
    else if (m_worker.joinable())
    {
        m_worker.detach();
    }
}

void JobQueue::updateJob(size_t i, UiUpdateFn& onUpdate)
{
    JobItem copy;
    {
        std::lock_guard lock(m_mutex);
        if (i >= m_jobs.size()) return;
        copy = m_jobs[i];
    }
    if (onUpdate) onUpdate(i, copy);
}

sdr2hdr::ProcessResult JobQueue::runWorker(
    const std::wstring& workerExe,
    size_t jobIndex,
    const std::wstring& input,
    const std::wstring& output,
    const std::function<void(const sdr2hdr::ProcessProgress&)>& progressCb)
{
    using namespace sdr2hdr::win32;
    sdr2hdr::ProcessResult result;

    // Named cancel event the worker watches (--cancel-event).
    wchar_t evtName[96];
    swprintf_s(evtName, L"Local\\sdr2hdr_cancel_%lu_%zu",
               GetCurrentProcessId(), jobIndex);
    HANDLE hEvt = CreateEventW(nullptr, TRUE, FALSE, evtName);
    {
        std::lock_guard lock(m_mutex);
        m_curCancelEvent = hEvt;
        // Cancel may have been requested before the worker even spawned.
        if (m_cancelCurrent.load() && hEvt) SetEvent(hEvt);
    }

    std::wstring cmd = L"\"" + workerExe + L"\" \"" + input + L"\" -o \"" +
                       output + L"\"" +
                       utf8ToWide(optionsToCliFlags(m_opts)) +
                       L" --cancel-event " + evtName;

    PipePair pOut{};
    PROCESS_INFORMATION pi{};
    bool launched =
        makePipe(pOut, /*childReads=*/false) &&
        launch(wideToUtf8(cmd), nullptr, pOut.writeEnd, pOut.writeEnd, pi);
    if (!launched)
    {
        if (pOut.readEnd)  CloseHandle(pOut.readEnd);
        if (pOut.writeEnd) CloseHandle(pOut.writeEnd);
        std::lock_guard lock(m_mutex);
        m_curCancelEvent = nullptr;
        if (hEvt) CloseHandle(hEvt);
        result.ok = false;
        result.errorDetail = "Failed to start conversion worker process.";
        return result;
    }
    CloseHandle(pOut.writeEnd);

    // Stream the worker's merged stdout+stderr: progress lines update the UI,
    // and the tail doubles as the error detail if it fails.
    std::string acc, tail;
    sdr2hdr::ProcessProgress pu{};
    char buf[4096];
    for (;;)
    {
        DWORD got = 0;
        if (!ReadFile(pOut.readEnd, buf, sizeof(buf), &got, nullptr) || !got)
            break;
        acc.append(buf, got);

        size_t pos;
        while ((pos = acc.find_first_of("\r\n")) != std::string::npos)
        {
            std::string line = acc.substr(0, pos);
            acc.erase(0, pos + 1);
            if (line.empty()) continue;

            unsigned long long done = 0, total = 0;
            double pct = 0, fps = 0;
            if (sscanf_s(line.c_str(), " Frame %llu / %llu (%lf%%, %lf fps)",
                         &done, &total, &pct, &fps) == 4)
            {
                pu.framesDone  = done;
                pu.framesTotal = total;
                pu.percent     = pct;
                pu.fps         = fps;
                if (progressCb) progressCb(pu);
                continue;
            }
            if (line.find("mux: copying audio") != std::string::npos)
            {
                pu.finalizing = true;
                pu.percent    = 100.0;
                if (progressCb) progressCb(pu);
            }
            tail += line;
            tail += '\n';
            if (tail.size() > 2048) tail.erase(0, tail.size() - 2048);
        }
    }
    CloseHandle(pOut.readEnd);

    WaitForSingleObject(pi.hProcess, 30000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    {
        std::lock_guard lock(m_mutex);
        m_curCancelEvent = nullptr;
    }
    if (hEvt) CloseHandle(hEvt);

    result.ok = (exitCode == 0);
    result.exitCode = static_cast<int>(exitCode);
    if (!result.ok)
    {
        // Last few output lines are the most specific error we have.
        while (!tail.empty() && (tail.back() == '\n' || tail.back() == '\r'))
            tail.pop_back();
        const size_t lastBlock = tail.size() > 400 ? tail.size() - 400 : 0;
        result.errorDetail = exitCode == 0xC0000409 || exitCode == 0xC0000005
            ? "Conversion worker crashed (driver fault). See log."
            : tail.substr(lastBlock);
        if (result.errorDetail.empty())
            result.errorDetail = "Conversion worker exited with code " +
                                 std::to_string(exitCode);
    }
    return result;
}

void JobQueue::workerLoop(UiUpdateFn onUpdate)
{
    // The conversion pipeline (and ffmpeg/shell helpers) may use COM; the CLI
    // initialises it on its main thread, so this worker thread must too.
    struct ComInit {
        bool ok;
        ComInit()  { ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
        ~ComInit() { if (ok) CoUninitialize(); }
    } comInit;

    if (!sdr2hdr::deps::ensureFfmpeg(false))
    {
        std::lock_guard lock(m_mutex);
        for (auto& j : m_jobs)
        {
            j.state = JobState::Failed;
            j.errorText = L"ffmpeg not found";
            j.statusText = L"Failed";
        }
        for (size_t i = 0; i < m_jobs.size(); ++i)
            updateJob(i, onUpdate);
        return;
    }
    if (!sdr2hdr::deps::checkNgxDlls())
    {
        std::lock_guard lock(m_mutex);
        for (auto& j : m_jobs)
        {
            j.state = JobState::Failed;
            j.errorText = L"NVNGX DLLs missing";
            j.statusText = L"Failed";
        }
        for (size_t i = 0; i < m_jobs.size(); ++i)
            updateJob(i, onUpdate);
        return;
    }

    sdr2hdr::applyDefaultQuality(m_opts);

    size_t total = 0;
    {
        std::lock_guard lock(m_mutex);
        total = m_jobs.size();
    }

    for (size_t i = 0; i < total; ++i)
    {
        if (m_cancel.load()) break;

        std::string inputUtf8;
        std::string outputUtf8;
        {
            std::lock_guard lock(m_mutex);
            auto& j = m_jobs[i];
            j.state = JobState::Processing;
            j.startedAt = std::chrono::steady_clock::now();
            j.statusText = L"Processing";
            j.progress = 0;
            j.finalizing = false;
            j.finalizeStartedAt = std::chrono::steady_clock::time_point{};
            inputUtf8  = wideToUtf8(j.inputPath);
            outputUtf8 = wideToUtf8(j.outputPath);
        }
        updateJob(i, onUpdate);

        auto progressCb = [this, i, &onUpdate](const sdr2hdr::ProcessProgress& p) {
            {
                std::lock_guard lock(m_mutex);
                if (i >= m_jobs.size()) return;
                auto& j = m_jobs[i];
                j.progress = p.percent;
                j.fps = p.fps;
                j.finalizing = p.finalizing;
                if (p.finalizing)
                {
                    // Frames done; the long tail here is copying/merging the
                    // source audio. The "Merging audio… M:SS" text (with a live
                    // elapsed counter) is rendered GUI-side; just stamp the start.
                    if (j.finalizeStartedAt == std::chrono::steady_clock::time_point{})
                        j.finalizeStartedAt = std::chrono::steady_clock::now();
                }
                else
                {
                    // "45%  127.0 fps  ~2:13" — ETA from remaining frames / fps.
                    wchar_t buf[160];
                    if (p.fps > 0.1 && p.framesTotal > p.framesDone)
                    {
                        double etaSec = static_cast<double>(p.framesTotal - p.framesDone) / p.fps;
                        int total = static_cast<int>(etaSec + 0.5);
                        int h = total / 3600, m = (total % 3600) / 60, s = total % 60;
                        if (h > 0)
                            swprintf_s(buf, L"%.0f%%  %.1f fps  ~%d:%02d:%02d", p.percent, p.fps, h, m, s);
                        else
                            swprintf_s(buf, L"%.0f%%  %.1f fps  ~%d:%02d", p.percent, p.fps, m, s);
                    }
                    else
                    {
                        swprintf_s(buf, L"%.0f%%  %.1f fps", p.percent, p.fps);
                    }
                    j.statusText = buf;
                }
            }
            updateJob(i, onUpdate);
        };

        sdr2hdr::ProcessResult result;
        const std::wstring workerExe = workerExePath();
        if (!workerExe.empty())
        {
            // Out-of-process conversion (crash isolation -- see runWorker).
            std::wstring inW, outW;
            {
                std::lock_guard lock(m_mutex);
                inW  = m_jobs[i].inputPath;
                outW = m_jobs[i].outputPath;
            }
            result = runWorker(workerExe, i, inW, outW, progressCb);
        }
        else
        {
            // Worker exe missing: fall back to converting in-process.
            try {
                result = sdr2hdr::processFile(
                    inputUtf8, outputUtf8, m_opts, progressCb, &m_cancelCurrent);
            }
            catch (const winrt::hresult_error& e) {
                result.ok = false;
                result.errorDetail = winrt::to_string(e.message());
            }
            catch (const std::exception& e) {
                result.ok = false;
                result.errorDetail = std::string("Exception: ") + e.what();
            }
            catch (...) {
                result.ok = false;
                result.errorDetail = "Unknown error during conversion.";
            }
        }
        if (!result.ok && !result.errorDetail.empty())
        {
            std::ofstream log("sdr2hdr_convert_error.log", std::ios::app);
            log << "[job " << i << "] " << inputUtf8 << "\n  -> "
                << result.errorDetail << "\n";
        }

        // Consume the per-job cancel so the next queued job still runs when
        // only this row's ✕ was clicked (a full queue cancel re-arms it via
        // m_cancel below).
        const bool jobCancelled = m_cancelCurrent.exchange(false);

        {
            std::lock_guard lock(m_mutex);
            if (i >= m_jobs.size()) continue;
            auto& j = m_jobs[i];
            if (jobCancelled && !result.ok)
            {
                j.state = JobState::Cancelled;
                j.statusText = L"Cancelled";
            }
            else if (result.ok)
            {
                j.state = JobState::Done;
                j.progress = 100.0;
                j.statusText = L"Done";
            }
            else
            {
                j.state = JobState::Failed;
                j.statusText = L"Failed";
                j.errorText = utf8ToWide(result.errorDetail);
            }
        }
        updateJob(i, onUpdate);
    }
}

} // namespace gui
