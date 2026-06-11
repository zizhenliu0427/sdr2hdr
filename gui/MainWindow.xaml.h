#pragma once

#include "MainWindow.g.h"
#include "JobQueue.h"

#include "../src/engine.h"

#include <shobjidl.h>   // ITaskbarList3 (taskbar progress)
#include <winrt/Microsoft.UI.Dispatching.h>

namespace winrt::sdr2hdr_gui::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();

        void Activate();
        void OnNavSelectionChanged(
            winrt::Microsoft::UI::Xaml::Controls::NavigationView const& sender,
            winrt::Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const& args);
        void OnAddFilesClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnStartClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnClearClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnDragOver(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::DragEventArgs const& args);
        void OnDrop(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::DragEventArgs const& args);
        void OnThemeChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
        void OnLangChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
        void OnRunCommandClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnShowHelpClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnModeChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnQualityChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
        void OnPeakNitsChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
        void OnResolutionChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
        void OnRecheckDepsClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);

    private:
        void showPage(int index);
        void updateModeEnablement();
        void updateActionButtons();
        void refreshQueueList();
        void syncQueueUi();   // in-place progress update (no full rebuild → no jump)
        winrt::Microsoft::UI::Xaml::UIElement makeQueueRow(const gui::JobItem& job);
        void removeJobFromQueue(winrt::hstring const& inputPath);
        void addInputPaths(const std::vector<std::wstring>& paths);
        sdr2hdr::ProcessOptions buildOptionsFromUi();
        std::wstring defaultOutputFor(const std::wstring& input);
        void applyLanguage();
        void applyTheme(int index);
        void updateCaptionButtonColors();  // theme-match the system min/max/close buttons
        void animatePageIn(winrt::Microsoft::UI::Xaml::FrameworkElement const& page);
        void runCommandLine(const std::wstring& args);
        void updateResizeBackdropColor();  // theme-matched fill for the resize gap
        void updateProgressUi();           // taskbar progress + completion toast
        void showCompletionNotification(int done, int failed);
        void updateDepsInfoBar();          // warn when ffmpeg / NVNGX DLLs are missing
        void updateDependencyStatus();     // Settings page found/missing rows
        winrt::fire_and_forget loadBrandLogos();  // ffmpeg / NVIDIA logos in About
        void updateLogoShadow(                    // contour drop shadow per logo
            winrt::Microsoft::UI::Xaml::Controls::Image const& image,
            winrt::Microsoft::UI::Xaml::UIElement const& host);

        gui::JobQueue m_queue;
        Microsoft::UI::Dispatching::DispatcherQueue m_dispatcher{ nullptr };
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_uiTimer{ nullptr };  // 1 Hz status refresh
        HWND m_hwnd{ nullptr };
        HBRUSH m_bgBrush{ nullptr };
        winrt::com_ptr<ITaskbarList3> m_taskbar{ nullptr };
        bool m_uiReady{ false };       // guards SelectionChanged handlers during init
        bool m_suppressCombo{ false }; // guards combo refresh from re-triggering handlers
        bool m_wasRunning{ false };    // detects the running -> idle transition
        std::vector<gui::JobState> m_rowStates;  // last-rendered per-row state (rebuild trigger)
    };
}

namespace winrt::sdr2hdr_gui::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}
