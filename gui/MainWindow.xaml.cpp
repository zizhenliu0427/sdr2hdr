#include "pch.h"
#include "MainWindow.xaml.h"
#include "MainWindow.xaml.g.hpp"

#include "../src/path_utils.h"
#include "../src/i18n.h"

#include <microsoft.ui.xaml.window.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <chrono>
#include <fstream>
#include <iterator>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
// wingdi.h (via windows.h) defines a GetCurrentTime macro that collides with
// IStoryboard.GetCurrentTime() in the generated animation header. Undef it.
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Windows.UI.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.Windows.AppNotifications.h>
#include <winrt/Microsoft.Windows.AppNotifications.Builder.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>

#pragma comment(lib, "comctl32.lib")

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;
using namespace Microsoft::UI::Xaml::Media::Animation;
using namespace Windows::Foundation;
using namespace Windows::Storage;
using namespace Windows::Storage::Pickers;

namespace winrt::sdr2hdr_gui::implementation
{

namespace {

// i18n helpers: pick the right literal for the current global language.
hstring locText(const char* en, const char* zh) { return winrt::to_hstring(i18n::tr(en, zh)); }
IInspectable locContent(const char* en, const char* zh) { return winrt::box_value(locText(en, zh)); }

bool isChecked(CheckBox const& box)
{
    auto v = box.IsChecked();
    return v && v.Value();
}

bool isChecked(RadioButton const& button)
{
    auto v = button.IsChecked();
    return v && v.Value();
}

std::wstring fileNameOnly(const std::wstring& path)
{
    auto pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

std::wstring exeFolder()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    if (auto s = dir.find_last_of(L"\\/"); s != std::wstring::npos) dir.resize(s + 1);
    return dir;
}

void replaceAll(std::string& s, const std::string& from, const std::string& to)
{
    for (size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos; pos += to.size())
        s.replace(pos, from.size(), to);
}

// A dependency is present if it sits next to the exe OR is resolvable on PATH.
bool depPresent(const std::wstring& name)
{
    if (GetFileAttributesW((exeFolder() + name).c_str()) != INVALID_FILE_ATTRIBUTES)
        return true;
    wchar_t found[MAX_PATH] = {};
    return SearchPathW(nullptr, name.c_str(), nullptr, MAX_PATH, found, nullptr) > 0;
}

// "M:SS" / "H:MM:SS" for a duration in seconds.
std::wstring clockStr(double seconds)
{
    if (seconds < 0.0) seconds = 0.0;
    int total = static_cast<int>(seconds + 0.5);
    int h = total / 3600, m = (total % 3600) / 60, s = total % 60;
    wchar_t buf[24];
    if (h > 0) swprintf_s(buf, L"%d:%02d:%02d", h, m, s);
    else       swprintf_s(buf, L"%d:%02d", m, s);
    return buf;
}

// The queue-row status string. Terminal states are localized here (at render
// time) so they follow the current language; only the live in-progress text
// (percent / "Merging audio…") is taken verbatim from the worker.
hstring queueStatusText(const gui::JobItem& job)
{
    switch (job.state)
    {
    case gui::JobState::Pending:
        return locText("Pending", "待处理");
    case gui::JobState::Processing:
        if (job.finalizing)
        {
            // Audio copy/mux: no reliable percentage, so show a live elapsed
            // counter next to the label. (en-GB; zh = 合并音频中…)
            double el = 0.0;
            if (job.finalizeStartedAt != std::chrono::steady_clock::time_point{})
                el = std::chrono::duration<double>(
                         std::chrono::steady_clock::now() - job.finalizeStartedAt).count();
            return hstring(std::wstring(locText("Merging audio", "合并音频中").c_str())
                           + L"…  " + clockStr(el));
        }
        return job.statusText.empty() ? locText("Processing", "处理中")
                                      : hstring(job.statusText);
    case gui::JobState::Done:
        return locText("Done", "完成");
    case gui::JobState::Failed:
        return job.errorText.empty() ? locText("Failed", "失败")
                                     : hstring(job.errorText);
    case gui::JobState::Cancelled:
        return locText("Cancelled", "已取消");
    }
    return hstring(job.statusText);
}

// Local declaration of IBufferByteAccess. We avoid <robuffer.h> on purpose: it
// introduces a global ::Windows namespace that makes this file's
// `using namespace Windows::...` directives ambiguous against winrt::Windows.
struct __declspec(uuid("905a0fef-bc53-11df-8c49-001e4fc686da")) __declspec(novtable)
IBufferByteAccessLocal : ::IUnknown
{
    virtual HRESULT __stdcall Buffer(uint8_t** value) = 0;
};

// Load the embedded app icon (app.rc id 101) into a WriteableBitmap so it can
// be shown in the XAML UI (title bar + About). Done at runtime from the Win32
// resource, so it works without ms-appx packaging.
Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap loadAppIconBitmap(int px)
{
    HICON hIcon = reinterpret_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101),
        IMAGE_ICON, px, px, LR_DEFAULTCOLOR));
    if (!hIcon) return nullptr;

    ICONINFO ii{};
    if (!GetIconInfo(hIcon, &ii)) { DestroyIcon(hIcon); return nullptr; }

    BITMAP bm{};
    GetObject(ii.hbmColor, sizeof(bm), &bm);
    const int w = bm.bmWidth, h = bm.bmHeight;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;   // top-down rows
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
    HDC hdc = GetDC(nullptr);
    GetDIBits(hdc, ii.hbmColor, 0, h, pixels.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, hdc);
    DeleteObject(ii.hbmColor);
    DeleteObject(ii.hbmMask);
    DestroyIcon(hIcon);

    // Older icons may carry no alpha channel — force fully opaque if so.
    bool anyAlpha = false;
    for (size_t i = 3; i < pixels.size(); i += 4)
        if (pixels[i]) { anyAlpha = true; break; }
    if (!anyAlpha)
        for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;

    // WriteableBitmap expects premultiplied BGRA.
    for (size_t i = 0; i < pixels.size(); i += 4)
    {
        const uint8_t a = pixels[i + 3];
        pixels[i + 0] = static_cast<uint8_t>(pixels[i + 0] * a / 255);
        pixels[i + 1] = static_cast<uint8_t>(pixels[i + 1] * a / 255);
        pixels[i + 2] = static_cast<uint8_t>(pixels[i + 2] * a / 255);
    }

    Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap wb(w, h);
    uint8_t* dst = nullptr;
    wb.PixelBuffer().as<IBufferByteAccessLocal>()->Buffer(&dst);
    if (dst) memcpy(dst, pixels.data(), pixels.size());
    wb.Invalidate();
    return wb;
}

// Window subclass:
//  - WM_GETMINMAXINFO clamps the minimum size (logical 900x620, DPI-scaled).
//  - WM_ERASEBKGND paints the client area with a theme-matched brush (passed in
//    via dwRefData). During a fast resize the XAML compositor lags one frame
//    behind the window; without this the newly exposed strip flashes black.
//    Filling it with the Mica base colour makes that gap blend in while keeping
//    the Mica material (a known WinUI 3 resize-artifact mitigation).
LRESULT CALLBACK WindowSubclassProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l,
                                    UINT_PTR, DWORD_PTR refData)
{
    switch (msg)
    {
    case WM_GETMINMAXINFO:
    {
        UINT dpi = GetDpiForWindow(hwnd);
        double scale = dpi > 0 ? dpi / 96.0 : 1.0;
        auto* mmi = reinterpret_cast<MINMAXINFO*>(l);
        mmi->ptMinTrackSize.x = static_cast<LONG>(1040 * scale);
        mmi->ptMinTrackSize.y = static_cast<LONG>(640 * scale);
        return 0;
    }
    case WM_ERASEBKGND:
    {
        if (auto brush = reinterpret_cast<HBRUSH>(refData))
        {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(w), &rc, brush);
            return 1;
        }
        break;
    }
    default:
        break;
    }
    return DefSubclassProc(hwnd, msg, w, l);
}

} // namespace

MainWindow::MainWindow()
{
    InitializeComponent();
    m_dispatcher = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
    check_hresult(m_inner.as<IWindowNative>()->get_WindowHandle(&m_hwnd));

    // Modern title bar: draw our own content into the caption area.
    auto window = m_inner.as<Microsoft::UI::Xaml::Window>();
    window.ExtendsContentIntoTitleBar(true);
    window.SetTitleBar(AppTitleBar());

    // Closing the window mid-conversion: cancel the running job and give the
    // pipeline a bounded moment to tear down -- it kills its ffmpeg children
    // and deletes the temp/partial output files. (Even on a hard kill, the
    // process-wide kill-on-close job object still reaps the children.)
    window.Closed([this](auto const&, auto const&) {
        m_queue.shutdown(8000);
    });

    // Window icon (taskbar + caption) from the app.rc resource (id 101).
    if (HICON hIcon = reinterpret_cast<HICON>(LoadImageW(
            GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101),
            IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED)))
    {
        SendMessageW(m_hwnd, WM_SETICON, ICON_BIG,   reinterpret_cast<LPARAM>(hIcon));
        SendMessageW(m_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIcon));
    }

    // Same icon inside the UI: title-bar logo and the About card.
    if (auto bmp = loadAppIconBitmap(64))  TitleBarIcon().Source(bmp);
    if (auto bmp = loadAppIconBitmap(128)) AboutIcon().Source(bmp);

    // Register for Windows toast notifications (unpackaged-app support).
    try {
        winrt::Microsoft::Windows::AppNotifications::AppNotificationManager::Default().Register();
    } catch (...) {}

    // Comfortable default size + a hard minimum so the layout never collapses.
    auto windowId = Microsoft::UI::GetWindowIdFromWindow(m_hwnd);
    if (auto appWindow = Microsoft::UI::Windowing::AppWindow::GetFromWindowId(windowId))
    {
        // AppWindow.Resize takes PHYSICAL pixels, so scale by the monitor DPI —
        // otherwise the window opens tiny on scaled (e.g. 150%) displays and the
        // min-size clamp (also DPI-scaled) only kicks in after a manual resize.
        UINT dpi = GetDpiForWindow(m_hwnd);
        double scale = dpi > 0 ? dpi / 96.0 : 1.0;
        appWindow.Resize({ static_cast<int32_t>(1120 * scale),
                           static_cast<int32_t>(780 * scale) });
        // Taller (48px) caption area, which vertically centres and enlarges the
        // system min/max/close buttons — the polished look used by Photos etc.
        if (Microsoft::UI::Windowing::AppWindowTitleBar::IsCustomizationSupported())
            appWindow.TitleBar().PreferredHeightOption(
                Microsoft::UI::Windowing::TitleBarHeightOption::Tall);
    }

    // Installs the window subclass (min-size + theme-matched resize fill), and
    // keep the fill colour in sync when the resolved theme changes.
    updateResizeBackdropColor();
    RootShell().ActualThemeChanged([this](auto&&, auto&&) {
        updateResizeBackdropColor();
        updateCaptionButtonColors();
        loadBrandLogos();   // re-tint the logo ink for the new theme
    });
    updateCaptionButtonColors();   // initial caption-button tint

    // Pick the UI language from the OS by default, then localize all strings.
    i18n::initLang(nullptr);
    applyLanguage();

    // No add/reorder animation on the queue list: progress updates must never
    // make rows visibly jump (the entrance animation used to replay every tick).
    QueueList().ItemContainerTransitions(TransitionCollection{});

    // 1 Hz refresh so the conversion ETA and the audio-merge elapsed counter
    // keep ticking even between (or after) the worker's progress callbacks.
    m_uiTimer = m_dispatcher.CreateTimer();
    m_uiTimer.Interval(std::chrono::seconds(1));
    m_uiTimer.Tick([this](auto&&, auto&&) {
        if (m_queue.isRunning()) syncQueueUi();
        else m_uiTimer.Stop();
    });

    m_uiReady = true;
    updateModeEnablement();   // HDR is the default mode → disable VSR-only options
    updateActionButtons();    // empty queue → Start/Clear disabled
    loadBrandLogos();         // optional ffmpeg / NVIDIA logos in About

    // Build (and rebuild) each logo's contour shadow once it has a real size.
    // The About page starts collapsed, so the logos lay out only when first
    // shown — SizeChanged is when their ActualSize becomes non-zero.
    FfmpegLogo().SizeChanged([this](auto&&, auto&&) {
        updateLogoShadow(FfmpegLogo(), FfmpegShadowHost());
    });
    NvidiaLogo().SizeChanged([this](auto&&, auto&&) {
        updateLogoShadow(NvidiaLogo(), NvidiaShadowHost());
    });
}

void MainWindow::Activate()
{
    m_inner.as<Microsoft::UI::Xaml::Window>().Activate();
}

void MainWindow::applyLanguage()
{
    // Navigation
    NavConvert().Content(locContent("Convert", "转换"));
    NavCommand().Content(locContent("Command line", "命令行"));
    NavSettings().Content(locContent("Settings", "设置"));
    NavAbout().Content(locContent("About", "关于"));

    // Convert page
    ConvertTitle().Text(locText("Convert", "转换"));
    DropTitle().Text(locText("Drop video files here", "拖放视频文件到此处"));
    AddFilesButton().Content(locContent("Add files…", "添加文件…"));
    ModeLabel().Text(locText("Mode", "模式"));
    OptCopyAudio().Content(locContent("Copy audio", "复制音频"));
    {
        auto audioTip = locContent(
            "Copy the source's original audio track (no re-encoding).",
            "复制源文件的原始音轨(不重新编码)。");
        ToolTipService::SetToolTip(OptCopyAudio(), audioTip);
        ToolTipService::SetToolTip(CopyAudioInfo(), audioTip);
    }
    EncoderHeaderText().Text(locText("Encoder", "编码器"));
    ToolTipService::SetToolTip(EncoderInfo(), locContent(
        "The RTX HDR/VSR conversion always runs on the GPU. This only picks the "
        "video encoder: GPU (NVENC) is fastest; CPU (software x265/x264/AV1) is "
        "slower but frees the GPU encoder and can be more compatible.",
        "RTX HDR/VSR 转换始终在 GPU 上运行。这里只选择视频编码器:"
        "GPU (NVENC) 最快;CPU (软件 x265/x264/AV1) 较慢,但不占用 GPU 编码器,兼容性可能更好。"));
    EncGpu().Content(locContent("GPU (NVENC)", "GPU (NVENC)"));
    EncCpu().Content(locContent("CPU (Software)", "CPU (软件)"));
    CodecBox().Header(locContent("Codec", "编码"));
    QueueLabel().Text(locText("Queue", "队列"));
    ClearButton().Content(locContent("Clear queue", "清空队列"));
    StartButton().Content(locContent("Start conversion", "开始转换"));

    // Command page
    CmdTitle().Text(locText("Command line", "命令行"));
    CmdDesc().Text(locText(
        "This build is also the command-line tool. Type arguments below and run "
        "them in a console — the same as launching the .exe with these arguments.",
        "本程序同时也是命令行工具。在下方输入参数并在控制台运行，"
        "效果等同于带这些参数启动 .exe。"));
    CommandInput().Header(locContent("Arguments", "参数"));
    RunCommandButton().Content(locContent("Run in console", "在控制台运行"));
    ShowHelpButton().Content(locContent("Show --help", "查看 --help"));

    // Settings page
    SettingsTitle().Text(locText("Settings", "设置"));
    ThemeBox().Header(locContent("Theme", "主题"));
    ThemeSystem().Content(locContent("Use system setting", "跟随系统"));
    ThemeLight().Content(locContent("Light", "浅色"));
    ThemeDark().Content(locContent("Dark", "深色"));
    LangBox().Header(locContent("Language", "语言"));
    OutputDirBox().Header(locContent("Default output directory (optional)",
                                     "默认输出目录 (可选)"));
    OutputDirBox().PlaceholderText(locText("Leave empty to write next to the source file",
                                           "留空则输出到源文件同级目录"));
    DepsTitle().Text(locText("Dependencies", "依赖项"));
    FfmpegDownload().Content(locContent("Download", "下载"));
    NgxDownload().Content(locContent("Download", "下载"));
    RecheckButton().Content(locContent("Re-check", "重新检测"));
    updateDependencyStatus();

    // Convert page option dropdowns (presets — manual entry only via "Custom")
    QualityHeaderText().Text(locText("Quality", "质量"));
    ToolTipService::SetToolTip(QualityInfo(), locContent(
        "Encoder quality (CQP). Lower q = better quality and bigger files. "
        "Auto matches the source bitrate; Custom lets you set q (12–28).",
        "编码质量 (CQP)。q 值越低 = 画质越好、文件越大。"
        "“自动”按源码率匹配;“自定义”可手动设置 q(12–28)。"));
    QualAuto().Content(locContent("Auto (match source)", "自动 (匹配源码率)"));
    QualHigh().Content(locContent("High (q15)", "高 (q15)"));
    QualDefault().Content(locContent("Default (q19)", "默认 (q19)"));
    QualCompact().Content(locContent("Compact (q22)", "紧凑 (q22)"));
    QualCustom().Content(locContent("Custom…", "自定义…"));
    QualityCustom().Header(locContent("q (12–28)", "q (12–28)"));

    PeakNitsHeaderText().Text(locText("Peak nits", "峰值尼特"));
    ToolTipService::SetToolTip(PeakNitsInfo(), locContent(
        "HDR target peak brightness, matched to your display. 1000 nits suits most "
        "HDR monitors; choose higher only for premium HDR displays.",
        "HDR 目标峰值亮度,匹配你的显示器。1000 尼特适合大多数 HDR 显示器;"
        "更高仅用于高端 HDR 屏。"));
    Nits400().Content(locContent("400 nits", "400 尼特"));
    Nits600().Content(locContent("600 nits", "600 尼特"));
    Nits1000().Content(locContent("1000 nits", "1000 尼特"));
    Nits2000().Content(locContent("2000 nits", "2000 尼特"));
    NitsCustom().Content(locContent("Custom…", "自定义…"));
    PeakNitsCustom().Header(locContent("nits", "尼特"));

    // VSR-only options
    ResolutionHeaderText().Text(locText("Output resolution", "输出分辨率"));
    ToolTipService::SetToolTip(ResolutionInfo(), locContent(
        "VSR upscales the video to this resolution. Choose Custom to set an exact "
        "width × height.",
        "VSR 会把视频放大到该分辨率。选“自定义”可指定精确的 宽 × 高。"));
    ResCustom().Content(locContent("Custom…", "自定义…"));
    ResW().Header(locContent("Width", "宽"));
    ResH().Header(locContent("Height", "高"));
    VsrQualityHeaderText().Text(locText("VSR quality", "VSR 质量"));
    ToolTipService::SetToolTip(VsrQualityInfo(), locContent(
        "VSR model quality, 1–4. Higher means better detail but slower. 4 is recommended.",
        "VSR 模型质量 1–4。越高画质越好但越慢,推荐 4。"));
    VsrQ1().Content(locContent("1 (fastest)", "1 (最快)"));
    VsrQ4().Content(locContent("4 (best)", "4 (最佳)"));

    // About page
    AboutTitle().Text(locText("About", "关于"));
    AboutDesc().Text(locText("RTX Video SDK — SDR to HDR / VSR converter",
                             "RTX Video SDK — SDR 转 HDR / VSR 工具"));
    PoweredBy().Text(locText("Powered by ffmpeg and NVIDIA RTX Video SDK.",
                             "基于 ffmpeg 和 英伟达 RTX Video SDK 构建。"));

    // Language "Auto" shows the detected system language after it.
    {
        const wchar_t* sys = (i18n::detectOsLang() == i18n::Lang::Zh) ? L"中文" : L"English";
        std::wstring label = std::wstring(locText("Auto", "自动").c_str()) + L" (" + sys + L")";
        LangAuto().Content(winrt::box_value(hstring(label)));
    }

    // Refresh each combo's collapsed display: WinUI does not re-read the selected
    // item's text when that item's Content is mutated, so toggle the selection.
    m_suppressCombo = true;
    auto refreshCombo = [](ComboBox const& cb) {
        int s = cb.SelectedIndex();
        cb.SelectedIndex(-1);
        cb.SelectedIndex(s);
    };
    refreshCombo(ThemeBox());
    refreshCombo(LangBox());
    refreshCombo(EncoderBox());
    refreshCombo(QualityBox());
    refreshCombo(PeakNitsBox());
    refreshCombo(ResolutionBox());
    refreshCombo(VsrQualityBox());
    m_suppressCombo = false;

    updateDepsInfoBar();
}

void MainWindow::updateModeEnablement()
{
    // VSR options apply only when VSR is involved; HDR options only when HDR is.
    const bool hdr = isChecked(ModeHdr()) || isChecked(ModeVsrHdr());
    const bool vsr = isChecked(ModeVsr()) || isChecked(ModeVsrHdr());
    ResolutionBox().IsEnabled(vsr);
    VsrQualityBox().IsEnabled(vsr);
    ResW().IsEnabled(vsr);
    ResH().IsEnabled(vsr);
    PeakNitsBox().IsEnabled(hdr);
    PeakNitsCustom().IsEnabled(hdr);

    // Dim the info icons too when their option is disabled (the fixed-colour
    // FontIcon doesn't follow the control's disabled state on its own).
    ResolutionInfo().Opacity(vsr ? 1.0 : 0.4);
    VsrQualityInfo().Opacity(vsr ? 1.0 : 0.4);
    PeakNitsInfo().Opacity(hdr ? 1.0 : 0.4);
}

void MainWindow::updateActionButtons()
{
    const bool running = m_queue.isRunning();
    const bool hasJobs = m_queue.count() > 0;
    StartButton().IsEnabled(hasJobs && !running);
    ClearButton().IsEnabled(hasJobs && !running);
}

void MainWindow::OnModeChanged(IInspectable const&, RoutedEventArgs const&)
{
    if (!m_uiReady) return;
    updateModeEnablement();
}

void MainWindow::OnQualityChanged(IInspectable const&, SelectionChangedEventArgs const&)
{
    if (!m_uiReady) return;
    QualityCustom().Visibility(QualityBox().SelectedIndex() == 4
        ? Visibility::Visible : Visibility::Collapsed);
}

void MainWindow::OnPeakNitsChanged(IInspectable const&, SelectionChangedEventArgs const&)
{
    if (!m_uiReady) return;
    PeakNitsCustom().Visibility(PeakNitsBox().SelectedIndex() == 4
        ? Visibility::Visible : Visibility::Collapsed);
}

void MainWindow::OnResolutionChanged(IInspectable const&, SelectionChangedEventArgs const&)
{
    if (!m_uiReady) return;
    const auto vis = (ResolutionBox().SelectedIndex() == 5)
        ? Visibility::Visible : Visibility::Collapsed;
    ResW().Visibility(vis);
    ResX().Visibility(vis);
    ResH().Visibility(vis);
}

void MainWindow::applyTheme(int index)
{
    auto theme = index == 1 ? ElementTheme::Light
               : index == 2 ? ElementTheme::Dark
                            : ElementTheme::Default;  // 0 = follow system
    if (auto root = RootShell().try_as<FrameworkElement>())
        root.RequestedTheme(theme);
    updateResizeBackdropColor();
    updateCaptionButtonColors();
}

void MainWindow::updateCaptionButtonColors()
{
    auto windowId = Microsoft::UI::GetWindowIdFromWindow(m_hwnd);
    auto appWindow = Microsoft::UI::Windowing::AppWindow::GetFromWindowId(windowId);
    if (!appWindow) return;
    auto tb = appWindow.TitleBar();

    using winrt::Windows::UI::Color;
    const bool dark = RootShell().ActualTheme() != ElementTheme::Light;
    const Color fg        = dark ? Color{ 255, 255, 255, 255 } : Color{ 255, 0, 0, 0 };
    const Color inactive  = dark ? Color{ 255, 155, 155, 155 } : Color{ 255, 120, 120, 120 };
    const Color clear     = Color{ 0, 0, 0, 0 };
    const Color hoverBg   = dark ? Color{ 25, 255, 255, 255 } : Color{ 18, 0, 0, 0 };
    const Color pressedBg = dark ? Color{ 50, 255, 255, 255 } : Color{ 36, 0, 0, 0 };

    tb.ButtonForegroundColor(fg);
    tb.ButtonBackgroundColor(clear);
    tb.ButtonInactiveBackgroundColor(clear);
    tb.ButtonInactiveForegroundColor(inactive);
    tb.ButtonHoverForegroundColor(fg);
    tb.ButtonHoverBackgroundColor(hoverBg);
    tb.ButtonPressedForegroundColor(fg);
    tb.ButtonPressedBackgroundColor(pressedBg);
}

void MainWindow::updateResizeBackdropColor()
{
    // Match the Mica base colour for the active theme so the one-frame resize
    // gap blends in instead of flashing black.
    auto actual = RootShell().ActualTheme();
    COLORREF color = (actual == ElementTheme::Dark) ? RGB(32, 32, 32)
                                                    : RGB(243, 243, 243);
    HBRUSH brush = CreateSolidBrush(color);

    // (Re)install the subclass, handing the brush to the proc as dwRefData.
    SetWindowSubclass(m_hwnd, WindowSubclassProc, 1, reinterpret_cast<DWORD_PTR>(brush));

    if (m_bgBrush) DeleteObject(m_bgBrush);
    m_bgBrush = brush;

    // Force the exposed area to repaint with the new brush.
    InvalidateRect(m_hwnd, nullptr, TRUE);
}

void MainWindow::updateProgressUi()
{
    const auto jobs = m_queue.snapshot();
    const bool running = m_queue.isRunning();

    if (running)
    {
        if (!m_taskbar)
        {
            m_taskbar = winrt::try_create_instance<ITaskbarList3>(CLSID_TaskbarList, CLSCTX_ALL);
            if (m_taskbar) m_taskbar->HrInit();
        }
        // Overall progress = (finished jobs ×100 + current job %) / (jobs ×100).
        double done = 0.0;
        bool anyFinalizing = false;
        for (auto const& j : jobs)
        {
            if (j.state == gui::JobState::Done || j.state == gui::JobState::Failed ||
                j.state == gui::JobState::Cancelled) done += 100.0;
            else if (j.state == gui::JobState::Processing)
            {
                done += j.progress;
                if (j.finalizing) anyFinalizing = true;
            }
        }
        if (m_taskbar && !jobs.empty())
        {
            if (anyFinalizing)
            {
                // Audio merge / verify: percentage is meaningless here, so show
                // the taskbar's marquee (indeterminate) animation instead.
                m_taskbar->SetProgressState(m_hwnd, TBPF_INDETERMINATE);
            }
            else
            {
                m_taskbar->SetProgressState(m_hwnd, TBPF_NORMAL);
                m_taskbar->SetProgressValue(m_hwnd, static_cast<ULONGLONG>(done),
                                            static_cast<ULONGLONG>(jobs.size() * 100));
            }
        }
        m_wasRunning = true;
    }
    else
    {
        if (m_taskbar) m_taskbar->SetProgressState(m_hwnd, TBPF_NOPROGRESS);
        if (m_wasRunning)
        {
            m_wasRunning = false;
            int ok = 0, failed = 0;
            for (auto const& j : jobs)
            {
                if (j.state == gui::JobState::Done) ++ok;
                else if (j.state == gui::JobState::Failed) ++failed;
            }
            showCompletionNotification(ok, failed);
        }
    }
}

void MainWindow::showCompletionNotification(int done, int failed)
{
    try {
        using namespace winrt::Microsoft::Windows::AppNotifications;
        using namespace winrt::Microsoft::Windows::AppNotifications::Builder;

        wchar_t body[192];
        if (failed == 0)
            swprintf_s(body, locText("%d file(s) converted successfully.",
                                     "%d 个文件转换完成。").c_str(), done);
        else
            swprintf_s(body, locText("Finished: %d succeeded, %d failed.",
                                     "已完成：%d 个成功，%d 个失败。").c_str(), done, failed);

        AppNotificationBuilder builder;
        builder.AddText(L"sdr2hdr");
        builder.AddText(hstring(body));
        AppNotificationManager::Default().Show(builder.BuildNotification());
    } catch (...) {}
}

void MainWindow::updateDepsInfoBar()
{
    const bool ngxMissing = !depPresent(L"nvngx_truehdr.dll") || !depPresent(L"nvngx_vsr.dll");
    const bool ffmpegMissing = !depPresent(L"ffmpeg.exe");

    if (ngxMissing)
    {
        // Cannot be auto-downloaded (NVIDIA login + EULA) — tell the user.
        DepsInfoBar().Severity(InfoBarSeverity::Warning);
        DepsInfoBar().Title(locText("RTX Video SDK files missing", "缺少 RTX Video SDK 文件"));
        DepsInfoBar().Message(locText(
            "nvngx_truehdr.dll and nvngx_vsr.dll were not found next to the app. "
            "Conversion needs them — download the NVIDIA RTX Video SDK and copy the two "
            "DLLs into this app's folder.",
            "未在程序目录找到 nvngx_truehdr.dll 与 nvngx_vsr.dll,转换需要它们。"
            "请下载 NVIDIA RTX Video SDK,并把这两个 DLL 复制到本程序所在目录。"));
        DepsInfoBar().IsOpen(true);
    }
    else if (ffmpegMissing)
    {
        // ffmpeg CAN be fetched automatically on first run.
        DepsInfoBar().Severity(InfoBarSeverity::Informational);
        DepsInfoBar().Title(locText("ffmpeg not found", "未找到 ffmpeg"));
        DepsInfoBar().Message(locText(
            "ffmpeg will be downloaded automatically the first time you start a conversion.",
            "首次开始转换时会自动下载 ffmpeg。"));
        DepsInfoBar().IsOpen(true);
    }
    else
    {
        DepsInfoBar().IsOpen(false);
    }
}

void MainWindow::updateDependencyStatus()
{
    auto themeBrush = [](const wchar_t* key) -> Media::Brush {
        auto res = Application::Current().Resources();
        auto k = winrt::box_value(winrt::hstring(key));
        return res.HasKey(k) ? res.Lookup(k).try_as<Media::Brush>() : nullptr;
    };
    auto setRow = [&](FontIcon icon, TextBlock text, bool ok) {
        icon.Glyph(ok ? L"" : L"");   // checkmark / cancel
        if (auto b = themeBrush(ok ? L"SystemFillColorSuccessBrush"
                                   : L"SystemFillColorCriticalBrush"))
            icon.Foreground(b);
        text.Text(ok ? locText("Found", "已找到") : locText("Not found", "未找到"));
    };
    setRow(FfmpegStatusIcon(), FfmpegStatusText(), depPresent(L"ffmpeg.exe"));
    setRow(NgxStatusIcon(), NgxStatusText(),
           depPresent(L"nvngx_truehdr.dll") && depPresent(L"nvngx_vsr.dll"));
}

void MainWindow::OnRecheckDepsClick(IInspectable const&, RoutedEventArgs const&)
{
    updateDependencyStatus();
    updateDepsInfoBar();
}

winrt::fire_and_forget MainWindow::loadBrandLogos()
{
    namespace Imaging  = winrt::Microsoft::UI::Xaml::Media::Imaging;
    namespace Streams  = winrt::Windows::Storage::Streams;

    // The logos carry a sentinel "ink" colour (#f2f2f2) on their text; swap it
    // to dark on a light theme so the wordmarks stay readable either way. Brand
    // colours (the green eye / icon) are untouched.
    const bool dark = RootShell().ActualTheme() != winrt::Microsoft::UI::Xaml::ElementTheme::Light;

    // rw/rh = high-res rasterisation size (matches each SVG's aspect) so the
    // vector renders crisply (supersampled) without jagged edges.
    auto load = [dark](Image target, std::wstring path,
                       double rw, double rh) -> winrt::fire_and_forget {
        std::ifstream f(path.c_str(), std::ios::binary);
        if (!f) { target.Visibility(Visibility::Collapsed); co_return; }
        std::string svg((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (!dark) {
            replaceAll(svg, "#f2f2f2", "#1b1b1b");
            replaceAll(svg, "#F2F2F2", "#1b1b1b");
        }
        try {
            Streams::InMemoryRandomAccessStream mem;
            Streams::DataWriter writer{ mem };
            writer.WriteBytes(winrt::array_view<uint8_t const>(
                reinterpret_cast<uint8_t const*>(svg.data()),
                reinterpret_cast<uint8_t const*>(svg.data() + svg.size())));
            co_await writer.StoreAsync();
            writer.DetachStream();
            mem.Seek(0);

            Imaging::SvgImageSource src;
            src.RasterizePixelWidth(rw);
            src.RasterizePixelHeight(rh);
            co_await src.SetSourceAsync(mem);
            target.Source(src);
            target.Visibility(Visibility::Visible);
        } catch (...) {
            target.Visibility(Visibility::Collapsed);
        }
    };
    const std::wstring dir = exeFolder() + L"Assets\\";
    load(FfmpegLogo(), dir + L"ffmpeg.svg", 597, 160);   // aspect ~3.73 : 1
    load(NvidiaLogo(), dir + L"nvidia.svg", 864, 160);   // cropped aspect 5.4 : 1
    co_return;
}

// Contour drop shadow for a brand logo: capture the logo's rendered pixels
// (with alpha) via a CompositionVisualSurface, use that as a DropShadow mask,
// and host the resulting sprite on the (non-clipping) Canvas behind the logo.
// The shadow then hugs the icon and every glyph instead of being a soft blob
// behind the whole rectangle. Rebuilt on SizeChanged; the surface tracks the
// live logo, so it also follows the theme re-tint automatically.
void MainWindow::updateLogoShadow(
    winrt::Microsoft::UI::Xaml::Controls::Image const& image,
    winrt::Microsoft::UI::Xaml::UIElement const& host)
{
    namespace Comp = winrt::Microsoft::UI::Composition;
    using winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview;

    auto size = image.ActualSize();   // {width, height} in float; 0 until laid out
    if (size.x < 1.0f || size.y < 1.0f) return;

    auto imgVisual = ElementCompositionPreview::GetElementVisual(image);
    auto compositor = imgVisual.Compositor();

    Comp::CompositionVisualSurface surface = compositor.CreateVisualSurface();
    surface.SourceVisual(imgVisual);
    surface.SourceSize(size);
    surface.SourceOffset({ 0.0f, 0.0f });

    Comp::CompositionSurfaceBrush maskBrush = compositor.CreateSurfaceBrush(surface);

    Comp::DropShadow shadow = compositor.CreateDropShadow();
    shadow.Mask(maskBrush);
    shadow.BlurRadius(2.2f);                       // tight = contour, not blob
    shadow.Opacity(0.42f);                         // subtle
    shadow.Offset({ 0.7f, 1.1f, 0.0f });
    shadow.Color(winrt::Windows::UI::ColorHelper::FromArgb(255, 0, 0, 0));

    Comp::SpriteVisual sprite = compositor.CreateSpriteVisual();
    sprite.Size(size);
    sprite.Shadow(shadow);

    ElementCompositionPreview::SetElementChildVisual(host, sprite);
}

void MainWindow::animatePageIn(FrameworkElement const& page)
{
    // Float-up + fade entrance with a non-linear (cubic ease-out) curve, the
    // same feel used across WinUI 3 apps when switching navigation pages.
    TranslateTransform tt;
    tt.Y(28.0);
    page.RenderTransform(tt);

    auto dur = DurationHelper::FromTimeSpan(std::chrono::milliseconds(320));

    CubicEase ease;
    ease.EasingMode(EasingMode::EaseOut);

    DoubleAnimation slide;
    slide.From(28.0);
    slide.To(0.0);
    slide.Duration(dur);
    slide.EasingFunction(ease);
    Storyboard::SetTarget(slide, tt);
    Storyboard::SetTargetProperty(slide, L"Y");

    DoubleAnimation fade;
    fade.From(0.0);
    fade.To(1.0);
    fade.Duration(dur);
    Storyboard::SetTarget(fade, page);
    Storyboard::SetTargetProperty(fade, L"Opacity");

    Storyboard sb;
    sb.Children().Append(slide);
    sb.Children().Append(fade);
    sb.Begin();
}

void MainWindow::showPage(int index)
{
    ConvertPage().Visibility(index == 0 ? Visibility::Visible : Visibility::Collapsed);
    CommandPage().Visibility(index == 1 ? Visibility::Visible : Visibility::Collapsed);
    SettingsPage().Visibility(index == 2 ? Visibility::Visible : Visibility::Collapsed);
    AboutPage().Visibility(index == 3 ? Visibility::Visible : Visibility::Collapsed);

    FrameworkElement active{ nullptr };
    switch (index)
    {
        case 0:  active = ConvertPage();  break;
        case 1:  active = CommandPage();  break;
        case 2:  active = SettingsPage(); break;
        default: active = AboutPage();    break;
    }
    if (active) animatePageIn(active);
}

void MainWindow::OnNavSelectionChanged(NavigationView const&, NavigationViewSelectionChangedEventArgs const& args)
{
    if (auto item = args.SelectedItem().try_as<NavigationViewItem>())
    {
        // Tag is authored in XAML as a string ("0".."3"); unbox as hstring,
        // not int32 — unbox_value<int32_t> on a string box throws and crashes.
        auto tag = unbox_value_or<hstring>(item.Tag(), L"0");
        showPage(_wtoi(tag.c_str()));
    }
}

void MainWindow::OnThemeChanged(IInspectable const&, SelectionChangedEventArgs const&)
{
    if (!m_uiReady || m_suppressCombo) return;
    applyTheme(ThemeBox().SelectedIndex());
}

void MainWindow::OnLangChanged(IInspectable const&, SelectionChangedEventArgs const&)
{
    if (!m_uiReady || m_suppressCombo) return;
    const int idx = LangBox().SelectedIndex();
    if (idx == 1)      i18n::initLang("en");
    else if (idx == 2) i18n::initLang("zh");
    else               i18n::initLang(nullptr);  // Auto = OS language
    applyLanguage();
    refreshQueueList();  // re-render any "Pending" placeholders in the new language
}

void MainWindow::runCommandLine(const std::wstring& args)
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);

    // Pass the GUI's current language to the CLI so its output (help, prompts)
    // matches what the user sees, unless they already specified --lang.
    std::wstring fullArgs = args;
    if (fullArgs.find(L"--lang") == std::wstring::npos)
        fullArgs += (i18n::currentLang() == i18n::Lang::Zh) ? L" --lang zh" : L" --lang en";

    // Wrap in `cmd.exe /k` so the console stays open after the tool finishes,
    // letting the user read the result. The exe re-enters as the CLI because it
    // is launched with arguments.
    std::wstring cmd = L"cmd.exe /k \"\"" + std::wstring(exe) + L"\" " + fullArgs + L"\"";
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                             CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi);
    if (ok)
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CmdInfoBar().Severity(InfoBarSeverity::Success);
        CmdInfoBar().Title(locText("Launched", "已启动"));
        CmdInfoBar().Message(locText("Running in a new console window.",
                                     "已在新的控制台窗口中运行。"));
        CmdInfoBar().IsOpen(true);
    }
    else
    {
        CmdInfoBar().Severity(InfoBarSeverity::Error);
        CmdInfoBar().Title(locText("Failed to start", "启动失败"));
        CmdInfoBar().Message(locText("Could not launch the console process.",
                                     "无法启动控制台进程。"));
        CmdInfoBar().IsOpen(true);
    }
}

void MainWindow::OnRunCommandClick(IInspectable const&, RoutedEventArgs const&)
{
    std::wstring args(CommandInput().Text().c_str());
    if (args.empty())
    {
        CmdInfoBar().Severity(InfoBarSeverity::Warning);
        CmdInfoBar().Title(locText("No arguments", "未输入参数"));
        CmdInfoBar().Message(locText("Type a command line first, e.g. in.mp4 out.mp4 --hdr",
                                     "请先输入命令行，例如 in.mp4 out.mp4 --hdr"));
        CmdInfoBar().IsOpen(true);
        return;
    }
    runCommandLine(args);
}

void MainWindow::OnShowHelpClick(IInspectable const&, RoutedEventArgs const&)
{
    runCommandLine(L"--help");
}

sdr2hdr::ProcessOptions MainWindow::buildOptionsFromUi()
{
    sdr2hdr::ProcessOptions opts;
    opts.modeSet = true;

    // Encoder: GPU (NVENC, full GPU pipeline) vs CPU (software x26x/AV1). The
    // RTX HDR/VSR inference still runs on the GPU either way — only the video
    // encode differs. A non-nvenc backend routes engine.cpp to the software
    // encode path (frames read back to the CPU encoder).
    if (EncoderBox().SelectedIndex() == 1)
    {
        opts.gpuOnly = false;
        opts.backend = "software";
    }
    else
    {
        opts.gpuOnly = true;
        opts.backend = "nvenc";
    }

    if (isChecked(ModeVsrHdr()))
        opts.mode = RtxConverter::Mode::VsrHdr;
    else if (isChecked(ModeVsr()))
        opts.mode = RtxConverter::Mode::Vsr;
    else
        opts.mode = RtxConverter::Mode::Hdr;

    // VSR output sizing + quality (only meaningful when VSR is involved).
    if (opts.mode == RtxConverter::Mode::Vsr || opts.mode == RtxConverter::Mode::VsrHdr)
    {
        switch (ResolutionBox().SelectedIndex())
        {
            case 0:  opts.targetHeight = 1080; break;
            case 1:  opts.targetHeight = 1440; break;
            case 2:  opts.targetHeight = 2160; break;
            case 3:  opts.targetHeight = 2880; break;
            case 4:  opts.targetHeight = 4320; break;
            default: // Custom W x H
                opts.outputSizeSet = true;
                opts.outW = static_cast<uint32_t>(ResW().Value());
                opts.outH = static_cast<uint32_t>(ResH().Value());
                break;
        }
        opts.vsrQuality = static_cast<uint32_t>(VsrQualityBox().SelectedIndex() + 1);
    }

    opts.copyAudio = isChecked(OptCopyAudio());

    // Peak nits preset (HDR target luminance; only used for HDR/VSR+HDR).
    switch (PeakNitsBox().SelectedIndex())
    {
        case 0:  opts.maxLum = 400;  break;
        case 1:  opts.maxLum = 600;  break;
        case 2:  opts.maxLum = 1000; break;
        case 3:  opts.maxLum = 2000; break;
        default: opts.maxLum = static_cast<uint32_t>(PeakNitsCustom().Value()); break;
    }

    const int codecIdx = CodecBox().SelectedIndex();
    if (codecIdx == 1) opts.codec = "h264";
    else if (codecIdx == 2) opts.codec = "av1";
    else opts.codec = "hevc";

    // Quality preset (manual entry only via "Custom").
    switch (QualityBox().SelectedIndex())
    {
        case 0:  opts.qualityAuto = true;  opts.quality = -1; break;
        case 1:  opts.qualityAuto = false; opts.quality = 15; break;
        case 2:  opts.qualityAuto = false; opts.quality = 19; break;
        case 3:  opts.qualityAuto = false; opts.quality = 22; break;
        default: opts.qualityAuto = false;
                 opts.quality = static_cast<int>(QualityCustom().Value()); break;
    }

    const int langIdx = LangBox().SelectedIndex();
    if (langIdx == 1) i18n::initLang("en");
    else if (langIdx == 2) i18n::initLang("zh");
    else i18n::initLang(nullptr);

    return opts;
}

std::wstring MainWindow::defaultOutputFor(const std::wstring& input)
{
    auto opts = buildOptionsFromUi();
    std::string inUtf8 = gui::wideToUtf8(input);

    std::string outDirUtf8;
    auto outDirW = OutputDirBox().Text();
    if (!outDirW.empty())
        outDirUtf8 = gui::wideToUtf8(std::wstring(outDirW.c_str()));

    if (!outDirUtf8.empty())
    {
        return gui::utf8ToWide(
            sdr2hdr::resolveOutputPath(inUtf8, outDirUtf8, true, true, opts));
    }
    return gui::utf8ToWide(
        sdr2hdr::resolveOutputPath(inUtf8, "", false, false, opts));
}

void MainWindow::addInputPaths(const std::vector<std::wstring>& paths)
{
    std::vector<std::wstring> hdrInputs;
    for (const auto& p : paths)
    {
        std::string ext = sdr2hdr::extOf(gui::wideToUtf8(p));
        if (!sdr2hdr::isVideoExt(ext)) continue;
        m_queue.addJob(p, defaultOutputFor(p));

        // Warn if the source is already HDR — sdr2hdr converts SDR -> HDR, so
        // an HDR input is usually a mistake (double-processing washes it out).
        VideoInfo info;
        if (probeVideo(gui::wideToUtf8(p), info) && info.isHdr)
            hdrInputs.push_back(fileNameOnly(p));
    }
    refreshQueueList();

    if (!hdrInputs.empty())
    {
        std::wstring names;
        for (size_t i = 0; i < hdrInputs.size() && i < 3; ++i)
        {
            if (i) names += L", ";
            names += hdrInputs[i];
        }
        if (hdrInputs.size() > 3) names += L" …";

        InputHintBar().Title(locText("Already HDR", "已经是 HDR"));
        InputHintBar().Message(hstring(
            std::wstring(locText(
                "This video already looks HDR; sdr2hdr converts SDR to HDR, so "
                "converting it again may wash out the colours. Affected: ",
                "该视频似乎已经是 HDR;sdr2hdr 是把 SDR 转换为 HDR,再转一次可能会让画面发灰。涉及文件:").c_str())
            + names));
        InputHintBar().IsOpen(true);
    }
}

// Builds one queue row: [ ✕ ] filename .......... status, with a progress bar
// underneath while it's converting. The ✕ removes that file from the queue.
UIElement MainWindow::makeQueueRow(const gui::JobItem& job)
{
    Grid row;
    row.Padding(ThicknessHelper::FromLengths(4, 6, 2, 6));
    row.RowDefinitions().Append(RowDefinition{});
    row.RowDefinitions().Append(RowDefinition{});
    row.RowDefinitions().GetAt(0).Height(GridLengthHelper::Auto());
    row.RowDefinitions().GetAt(1).Height(GridLengthHelper::Auto());
    row.ColumnDefinitions().Append(ColumnDefinition{});
    row.ColumnDefinitions().Append(ColumnDefinition{});
    row.ColumnDefinitions().Append(ColumnDefinition{});
    row.ColumnDefinitions().GetAt(0).Width(GridLength{1.0, GridUnitType::Star});
    row.ColumnDefinitions().GetAt(1).Width(GridLengthHelper::Auto());
    row.ColumnDefinitions().GetAt(2).Width(GridLengthHelper::Auto());

    // [col 2, far right] Remove button (✕). Disabled while actively converting.
    Button del;
    del.Width(30);
    del.Height(30);
    del.Padding(ThicknessHelper::FromUniformLength(0));
    del.Margin(ThicknessHelper::FromLengths(8, 0, 0, 0));
    del.Background(SolidColorBrush(Windows::UI::Colors::Transparent()));
    del.BorderThickness(ThicknessHelper::FromUniformLength(0));
    del.VerticalAlignment(VerticalAlignment::Center);
    FontIcon delIcon;
    delIcon.Glyph(L"");   // Cancel (✕)
    delIcon.FontSize(12);
    del.Content(delIcon);
    // Pending rows: remove from the queue. The actively-converting row stays
    // enabled too -- there it CANCELS the conversion (the pipeline aborts,
    // deletes its temp/partial files, and the worker moves on).
    if (job.state == gui::JobState::Processing)
        ToolTipService::SetToolTip(del, box_value(locText("Cancel conversion", "取消转换")));
    else
        ToolTipService::SetToolTip(del, box_value(locText("Remove from queue", "从队列移除")));
    hstring inputPath{ job.inputPath };
    del.Click([this, inputPath](IInspectable const&, RoutedEventArgs const&) {
        removeJobFromQueue(inputPath);
    });
    Grid::SetColumn(del, 2);
    Grid::SetRow(del, 0);

    // [col 0] File name.
    TextBlock name;
    name.Text(fileNameOnly(job.inputPath));
    name.TextTrimming(TextTrimming::CharacterEllipsis);
    name.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(name, 0);

    // [col 1] Status / failure reason.
    TextBlock status;
    status.Text(queueStatusText(job));
    status.VerticalAlignment(VerticalAlignment::Center);
    status.Margin(ThicknessHelper::FromLengths(12, 0, 0, 0));
    status.FontSize(12);
    status.Opacity(0.7);
    status.MaxWidth(240);
    status.TextTrimming(TextTrimming::CharacterEllipsis);
    if (job.state == gui::JobState::Failed && !job.errorText.empty())
        ToolTipService::SetToolTip(status, box_value(hstring(job.errorText)));
    Grid::SetColumn(status, 1);

    row.Children().Append(del);     // child 0
    row.Children().Append(name);    // child 1
    row.Children().Append(status);  // child 2

    if (job.state == gui::JobState::Processing)
    {
        ProgressBar bar;
        bar.Minimum(0.0);
        bar.Maximum(100.0);
        bar.Value(job.progress);   // job.progress is already a 0–100 percentage
        bar.IsIndeterminate(job.finalizing);  // marquee during the mux/verify step
        bar.Margin(ThicknessHelper::FromLengths(4, 6, 0, 0));
        Grid::SetRow(bar, 1);
        Grid::SetColumn(bar, 0);
        Grid::SetColumnSpan(bar, 3);
        row.Children().Append(bar); // child 3
    }

    return row;
}

void MainWindow::refreshQueueList()
{
    QueueList().Items().Clear();
    m_rowStates.clear();
    for (const auto& job : m_queue.snapshot())
    {
        QueueList().Items().Append(makeQueueRow(job));
        m_rowStates.push_back(job.state);
    }
    updateActionButtons();
}

// Progress-tick update. Rebuilding the whole list on every tick replayed the
// ListView entrance animation (rows visibly jumped). So when nothing structural
// changed (same jobs, same states) we patch the existing rows in place; only a
// real structural change (added/removed job, or a state transition that adds or
// removes the progress bar) falls back to a full rebuild.
void MainWindow::syncQueueUi()
{
    auto jobs = m_queue.snapshot();
    auto items = QueueList().Items();

    bool structural = (items.Size() != jobs.size()) || (m_rowStates.size() != jobs.size());
    if (!structural)
    {
        for (size_t i = 0; i < jobs.size(); ++i)
            if (m_rowStates[i] != jobs[i].state) { structural = true; break; }
    }
    if (structural) { refreshQueueList(); return; }

    for (uint32_t i = 0; i < jobs.size(); ++i)
    {
        auto grid = items.GetAt(i).try_as<Grid>();
        if (!grid) continue;
        auto kids = grid.Children();
        // child layout: [0]=✕ button, [1]=name, [2]=status, [3]=progress bar
        if (kids.Size() > 2)
            if (auto status = kids.GetAt(2).try_as<TextBlock>())
                status.Text(queueStatusText(jobs[i]));
        if (jobs[i].state == gui::JobState::Processing && kids.Size() > 3)
            if (auto bar = kids.GetAt(3).try_as<ProgressBar>())
            {
                bar.IsIndeterminate(jobs[i].finalizing);
                if (!jobs[i].finalizing) bar.Value(jobs[i].progress);
            }
    }
}

void MainWindow::removeJobFromQueue(hstring const& inputPath)
{
    if (m_queue.removeInput(std::wstring(inputPath.c_str())))
    {
        refreshQueueList();
        return;
    }
    // removeInput refuses the row that is actively converting -- for that one
    // the ✕ means "cancel". The worker marks it Cancelled and the row updates
    // through the normal progress callback.
    m_queue.cancelCurrent();
}

void MainWindow::OnAddFilesClick(IInspectable const&, RoutedEventArgs const&)
{
    FileOpenPicker picker;
    picker.ViewMode(PickerViewMode::List);
    picker.SuggestedStartLocation(PickerLocationId::VideosLibrary);
    picker.FileTypeFilter().Append(L".mp4");
    picker.FileTypeFilter().Append(L".mkv");
    picker.FileTypeFilter().Append(L".mov");
    picker.FileTypeFilter().Append(L".webm");
    picker.FileTypeFilter().Append(L".avi");

    check_hresult(picker.as<IInitializeWithWindow>()->Initialize(m_hwnd));
    auto op = picker.PickMultipleFilesAsync();
    op.Completed([this, op](auto const&, AsyncStatus status) {
        if (status != AsyncStatus::Completed) return;
        std::vector<std::wstring> paths;
        for (auto const& f : op.GetResults())
            paths.push_back(std::wstring(f.Path().c_str()));
        m_dispatcher.TryEnqueue([this, paths]() { addInputPaths(paths); });
    });
}

void MainWindow::OnDragOver(IInspectable const&, DragEventArgs const& e)
{
    e.AcceptedOperation(Windows::ApplicationModel::DataTransfer::DataPackageOperation::Copy);
}

void MainWindow::OnDrop(IInspectable const&, DragEventArgs const& e)
{
    if (!e.DataView().Contains(Windows::ApplicationModel::DataTransfer::StandardDataFormats::StorageItems()))
        return;
    auto op = e.DataView().GetStorageItemsAsync();
    op.Completed([this, op](auto const&, AsyncStatus status) {
        if (status != AsyncStatus::Completed) return;
        std::vector<std::wstring> paths;
        for (auto const& item : op.GetResults())
        {
            if (auto f = item.try_as<StorageFile>())
                paths.push_back(std::wstring(f.Path().c_str()));
        }
        m_dispatcher.TryEnqueue([this, paths]() { addInputPaths(paths); });
    });
}

void MainWindow::OnClearClick(IInspectable const&, RoutedEventArgs const&)
{
    if (m_queue.isRunning()) return;
    m_queue.clear();
    QueueList().Items().Clear();
    updateActionButtons();
}

void MainWindow::OnStartClick(IInspectable const&, RoutedEventArgs const&)
{
    if (m_queue.isRunning()) return;
    if (m_queue.count() == 0) return;

    updateDepsInfoBar();  // re-check in case DLLs/ffmpeg were just added

    m_queue.setOptions(buildOptionsFromUi());
    m_queue.start([this](size_t, const gui::JobItem&) {
        m_dispatcher.TryEnqueue([this]() { syncQueueUi(); updateProgressUi(); });
    });
    refreshQueueList();   // reflect the running state (disables Start/Clear)
    updateProgressUi();   // start the taskbar progress indicator
    if (m_uiTimer) m_uiTimer.Start();   // tick the ETA / merge-elapsed display
}

} // namespace winrt::sdr2hdr_gui::implementation
