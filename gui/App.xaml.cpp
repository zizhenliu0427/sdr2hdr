#include "pch.h"
#define DISABLE_XAML_GENERATED_MAIN
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "App.xaml.g.hpp"
#include "../src/engine.h"
#include <fstream>
#include <cstdio>
#include <string>
#include <exception>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace {

// Append a line to sdr2hdr_debug.log next to the exe (absolute path, so it's
// found regardless of the working directory).
void appLog(std::string const& msg)
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    if (auto s = p.find_last_of(L"\\/"); s != std::wstring::npos) p.resize(s + 1);
    p += L"sdr2hdr_debug.log";
    std::ofstream log(p, std::ios::app);
    log << msg << "\n";
}

// Uncaught C++ exceptions go through std::terminate (NOT the SEH filter), so log
// the in-flight exception's details here before aborting.
void onTerminate()
{
    std::string detail = "[TERMINATE]";
    if (auto e = std::current_exception())
    {
        try { std::rethrow_exception(e); }
        catch (winrt::hresult_error const& x)
        {
            char b[40]; std::snprintf(b, sizeof(b), " hresult 0x%08X ",
                static_cast<unsigned>(static_cast<int32_t>(x.code())));
            detail += b; detail += winrt::to_string(x.message());
        }
        catch (std::exception const& x) { detail += " std::exception: "; detail += x.what(); }
        catch (...) { detail += " unknown C++ exception"; }
    }
    appLog(detail);
    std::abort();
}

// When the merged binary is launched with command-line arguments, behave as the
// console tool (sdr2hdr::cliMain) instead of opening the window. WinUI apps are
// /SUBSYSTEM:WINDOWS, so we attach to the parent console (or allocate one) and
// re-point the CRT std streams at it before running the CLI.
int runAsCli()
{
    if (::GetConsoleWindow() == nullptr)
    {
        if (!::AttachConsole(ATTACH_PARENT_PROCESS))
            ::AllocConsole();
    }
    FILE* f = nullptr;
    ::freopen_s(&f, "CONOUT$", "w", stdout);
    ::freopen_s(&f, "CONOUT$", "w", stderr);
    ::freopen_s(&f, "CONIN$",  "r", stdin);

    // cliMain re-derives a UTF-8 argv from GetCommandLineW(), so the ANSI
    // __argv we pass here is only a placeholder.
    return sdr2hdr::cliMain(__argc, __argv);
}

} // namespace

namespace winrt::sdr2hdr_gui::implementation
{
    App::App()
    {
        // Catch UI-thread exceptions (these bypass the SEH filter and would
        // otherwise silently close the app). Log, then keep the app alive.
        UnhandledException([](IInspectable const&,
                              Microsoft::UI::Xaml::UnhandledExceptionEventArgs const& e)
        {
            char b[24]; std::snprintf(b, sizeof(b), "0x%08X",
                static_cast<unsigned>(static_cast<int32_t>(e.Exception())));
            appLog(std::string("[XAML UnhandledException] ") + b + " " +
                   winrt::to_string(e.Message()));
            e.Handled(true);
        });

        try
        {
            InitializeComponent();
        }
        catch (winrt::hresult_error const& ex)
        {
            appLog(std::string("[InitializeComponent] hresult ") +
                   winrt::to_string(ex.message()));
            throw;
        }
    }

    void App::OnLaunched(LaunchActivatedEventArgs const&)
    {
        try
        {
            window = make<MainWindow>();
            window.Activate();
        }
        catch (winrt::hresult_error const& ex)
        {
            std::ofstream log("crash_app.log", std::ios::app);
            log << "OnLaunched failed: " << std::hex << ex.code() << "\nMessage: " << winrt::to_string(ex.message()) << "\n";
            throw;
        }
    }
}

// Last-resort logger for native crashes (e.g. access violations) that aren't
// C++ exceptions — writes the exception code + faulting address to a file so a
// silent "the app just closed" becomes diagnosable.
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info)
{
    char b[96];
    std::snprintf(b, sizeof(b), "[SEH] code 0x%08X at 0x%p",
        info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0u,
        info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr);
    appLog(b);
    return EXCEPTION_EXECUTE_HANDLER;
}

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    // Merged CLI: any command-line argument means "run as the console tool".
    // No arguments (double-click) shows the GUI.
    if (__argc > 1)
        return runAsCli();

    ::SetUnhandledExceptionFilter(CrashFilter);
    std::set_terminate(onTerminate);
    appLog("[start] GUI launching");

    try
    {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        ::winrt::Microsoft::UI::Xaml::Application::Start(
            [](auto&&)
            {
                ::winrt::make<::winrt::sdr2hdr_gui::implementation::App>();
            });
    }
    catch (winrt::hresult_error const& ex)
    {
        std::ofstream log("crash.log");
        log << "HResult Error: " << std::hex << ex.code() << "\nMessage: " << winrt::to_string(ex.message()) << "\n";
    }
    catch (std::exception const& ex)
    {
        std::ofstream log("crash.log");
        log << "Exception: " << ex.what() << "\n";
    }
    catch (...)
    {
        std::ofstream log("crash.log");
        log << "Unknown exception!\n";
    }
    return 0;
}
