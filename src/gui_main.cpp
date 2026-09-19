#include "wasapi_bridge.h"
#include "resource.h"

#include <Windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <climits>
#include <cstdlib>
#include <cwctype>
#include <exception>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using stereo_surround::BridgeOptions;
using stereo_surround::BridgeMonitor;
using stereo_surround::BridgeRuntimeControls;
using stereo_surround::EndpointInfo;
using stereo_surround::ProcessingMode;

constexpr ProcessingMode kDefaultSpatialMode = ProcessingMode::transaural_game;
constexpr wchar_t kWindowClass[] = L"StereoSurroundVirtualizerWindow";
constexpr wchar_t kWindowTitle[] = L"Stereo Surround 7.1 Virtualizer";
constexpr UINT kWorkerFinished = WM_APP + 1;
constexpr UINT_PTR kMonitorTimer = 1;
constexpr wchar_t kGeometryWindowClass[] = L"TransauralGeometrySettingsWindow";
HFONT g_ui_font = nullptr;
HFONT g_label_font = nullptr;

enum ControlId : int {
    kCaptureCombo = 100,
    kRenderCombo,
    kRefreshButton,
    kModeCombo,
    kAbSwitchButton,
    kTransauralStrengthSlider,
    kTransauralSideWidthSlider,
    kSpeakerGeometryButton,
    kRearProfileCombo,
    kUse24KhzCheck,
    kFixedChannelHeadroomCheck,
    kLatencySlider,
    kResyncButton,
    kStartBridgeButton,
    kStopButton,
    kFileEdit,
    kBrowseButton,
    kChannelCombo,
    kPlayTestButton,
    kStatusEdit,
    kGameReferenceButton,
    kGameWildButton,
};

enum GeometryControlId : int {
    kGeometryDirectRadio = 500,
    kGeometryMeasuredRadio,
    kGeometrySpanSlider,
    kGeometrySpacingEdit,
    kGeometryDistanceEdit,
    kGeometryHeadWidthEdit,
    kGeometryApplyButton,
    kGeometryCancelButton,
};

struct SpeakerGeometry {
    bool use_measurements = false;
    float direct_span_degrees = 20.0F;
    float speaker_spacing_cm = 48.3F;
    float listening_distance_cm = 75.0F;
    float head_width_cm = 18.0F;
};

struct Completion {
    std::wstring text;
    bool error = false;
};

struct AppState {
    HWND window = nullptr;
    HWND capture_combo = nullptr;
    HWND render_combo = nullptr;
    HWND refresh_button = nullptr;
    HWND mode_combo = nullptr;
    HWND ab_switch_button = nullptr;
    HWND transaural_strength_slider = nullptr;
    HWND transaural_strength_label = nullptr;
    HWND transaural_side_width_slider = nullptr;
    HWND transaural_side_width_label = nullptr;
    HWND game_reference_button = nullptr;
    HWND game_wild_button = nullptr;
    HWND spatial_monitor_label = nullptr;
    HWND speaker_geometry_button = nullptr;
    HWND speaker_geometry_summary = nullptr;
    HWND rear_profile_combo = nullptr;
    HWND use_24khz_check = nullptr;
    HWND fixed_channel_headroom_check = nullptr;
    HWND latency_slider = nullptr;
    HWND latency_label = nullptr;
    HWND resync_button = nullptr;
    HWND monitor_format_label = nullptr;
    HWND queue_progress = nullptr;
    HWND queue_label = nullptr;
    HWND rate_label = nullptr;
    HWND stats_label = nullptr;
    std::array<HWND, 8> channel_meters{};
    std::array<float, 8> displayed_peaks{};
    HWND start_button = nullptr;
    HWND stop_button = nullptr;
    HWND file_edit = nullptr;
    HWND browse_button = nullptr;
    HWND channel_combo = nullptr;
    HWND play_button = nullptr;
    HWND status_edit = nullptr;
    std::vector<EndpointInfo> capture_endpoints;
    std::vector<EndpointInfo> render_endpoints;
    std::jthread worker;
    BridgeRuntimeControls controls;
    BridgeMonitor monitor;
    std::atomic_bool stop_requested{false};
    std::uint64_t previous_captured_frames = 0;
    std::uint64_t previous_rendered_frames = 0;
    ULONGLONG previous_monitor_tick = 0;
    double smoothed_capture_rate = 0.0;
    double smoothed_render_rate = 0.0;
    bool running = false;
    bool live_controls_active = false;
    ProcessingMode active_spatial_mode = kDefaultSpatialMode;
    SpeakerGeometry geometry{};
};

struct GeometryDialogState {
    AppState* app = nullptr;
    SpeakerGeometry working{};
    HWND window = nullptr;
    HWND direct_radio = nullptr;
    HWND measured_radio = nullptr;
    HWND span_slider = nullptr;
    HWND span_label = nullptr;
    HWND spacing_edit = nullptr;
    HWND distance_edit = nullptr;
    HWND head_width_edit = nullptr;
    HWND result_label = nullptr;
};

[[nodiscard]] bool isTransauralMode(ProcessingMode mode) {
    return mode == ProcessingMode::transaural_natural ||
           mode == ProcessingMode::transaural_hyper ||
           mode == ProcessingMode::transaural_game;
}

[[nodiscard]] const wchar_t* processingModeName(ProcessingMode mode) {
    switch (mode) {
        case ProcessingMode::transaural_natural:
            return L"Transaural Natural";
        case ProcessingMode::transaural_hyper:
            return L"Transaural Hyper";
        case ProcessingMode::transaural_game:
            return L"Game 7.1";
        case ProcessingMode::matrix:
            return L"Naive";
    }
    return L"Unknown";
}

[[nodiscard]] ProcessingMode processingModeFromCombo(HWND combo) {
    switch (static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0))) {
        case 0:
            return ProcessingMode::transaural_natural;
        case 1:
            return ProcessingMode::transaural_hyper;
        case 2:
            return ProcessingMode::matrix;
        case 3:
            return ProcessingMode::transaural_game;
        default:
            return ProcessingMode::transaural_game;
    }
}

[[nodiscard]] int processingModeComboIndex(ProcessingMode mode) {
    switch (mode) {
        case ProcessingMode::transaural_natural:
            return 0;
        case ProcessingMode::transaural_hyper:
            return 1;
        case ProcessingMode::matrix:
            return 2;
        case ProcessingMode::transaural_game:
            return 3;
    }
    return 1;
}

void setControlFont(HWND control, bool small_label) {
    HFONT selected_font = small_label ? g_label_font : g_ui_font;
    SendMessageW(control, WM_SETFONT,
                 reinterpret_cast<WPARAM>(
                     selected_font != nullptr
                         ? selected_font
                         : GetStockObject(DEFAULT_GUI_FONT)),
                 TRUE);
}

HWND makeControl(DWORD extended_style, const wchar_t* class_name,
                 const wchar_t* text, DWORD style, int x, int y, int width,
                 int height, HWND parent, int id) {
    HWND control = CreateWindowExW(
        extended_style, class_name, text, style | WS_CHILD | WS_VISIBLE, x, y,
        width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    const bool small_label =
        lstrcmpiW(class_name, L"STATIC") == 0 ||
        (lstrcmpiW(class_name, L"BUTTON") == 0 &&
         (style & BS_TYPEMASK) == BS_GROUPBOX);
    setControlFont(control, small_label);
    return control;
}

[[nodiscard]] float measuredSpeakerSpan(const SpeakerGeometry& geometry) {
    if (geometry.listening_distance_cm <= 0.0F) {
        return 0.0F;
    }
    return static_cast<float>(
        2.0 * std::atan(0.5 * geometry.speaker_spacing_cm /
                        geometry.listening_distance_cm) *
        180.0 / std::acos(-1.0));
}

[[nodiscard]] float effectiveSpeakerSpan(const SpeakerGeometry& geometry) {
    return geometry.use_measurements ? measuredSpeakerSpan(geometry)
                                     : geometry.direct_span_degrees;
}

[[nodiscard]] float effectiveSpeakerDistanceMetres(
    const SpeakerGeometry& geometry) {
    if (!geometry.use_measurements) {
        // A direct span alone cannot determine finite-distance geometry. Use a
        // neutral desktop-scale distance that is close to the far-field model.
        return 1.0F;
    }
    return std::hypot(geometry.listening_distance_cm,
                      0.5F * geometry.speaker_spacing_cm) /
           100.0F;
}

[[nodiscard]] std::wstring oneDecimal(float value) {
    std::wostringstream text;
    text << std::fixed << std::setprecision(1) << value;
    return text.str();
}

void setEditNumber(HWND edit, float value) {
    const std::wstring text = oneDecimal(value);
    SetWindowTextW(edit, text.c_str());
}

[[nodiscard]] bool readEditNumber(HWND edit, float& value) {
    wchar_t buffer[64]{};
    GetWindowTextW(edit, buffer, static_cast<int>(std::size(buffer)));
    // Accept either common decimal separator without depending on the process
    // locale. Geometry values never use thousands separators.
    for (wchar_t& character : buffer) {
        if (character == L',') {
            character = L'.';
        }
    }
    errno = 0;
    wchar_t* end = nullptr;
    const float parsed = std::wcstof(buffer, &end);
    if (end == buffer || errno == ERANGE || !std::isfinite(parsed)) {
        return false;
    }
    while (*end != L'\0' && std::iswspace(*end) != 0) {
        ++end;
    }
    if (*end != L'\0') {
        return false;
    }
    value = parsed;
    return true;
}

void updateGeometrySummary(AppState& state) {
    const float span = effectiveSpeakerSpan(state.geometry);
    std::wstring summary;
    if (state.geometry.use_measurements) {
        summary = oneDecimal(state.geometry.speaker_spacing_cm) + L" / " +
                  oneDecimal(state.geometry.listening_distance_cm) +
                  L" cm\r\n" + oneDecimal(span) + L" deg";
    } else {
        summary = L"Direct " + oneDecimal(span) + L" deg";
    }
    summary += L", head " + oneDecimal(state.geometry.head_width_cm) + L" cm";
    SetWindowTextW(state.speaker_geometry_summary, summary.c_str());
}

void appendStatus(AppState& state, const std::wstring& text);

void publishGeometryToRunningProcessor(AppState& state) {
    state.controls.speaker_span_degrees.store(
        effectiveSpeakerSpan(state.geometry), std::memory_order_relaxed);
    state.controls.speaker_distance_metres.store(
        effectiveSpeakerDistanceMetres(state.geometry),
        std::memory_order_relaxed);
    state.controls.head_width_metres.store(
        state.geometry.head_width_cm / 100.0F, std::memory_order_relaxed);
    state.controls.geometry_generation.fetch_add(1,
                                                  std::memory_order_release);
}

void readGeometryDialogEdits(GeometryDialogState& dialog) {
    static_cast<void>(
        readEditNumber(dialog.spacing_edit, dialog.working.speaker_spacing_cm));
    static_cast<void>(readEditNumber(dialog.distance_edit,
                                     dialog.working.listening_distance_cm));
    static_cast<void>(
        readEditNumber(dialog.head_width_edit, dialog.working.head_width_cm));
    dialog.working.direct_span_degrees = static_cast<float>(
        SendMessageW(dialog.span_slider, TBM_GETPOS, 0, 0));
}

void updateGeometryDialog(GeometryDialogState& dialog) {
    readGeometryDialogEdits(dialog);
    EnableWindow(dialog.span_slider, !dialog.working.use_measurements);
    EnableWindow(dialog.spacing_edit, dialog.working.use_measurements);
    EnableWindow(dialog.distance_edit, dialog.working.use_measurements);

    const std::wstring span_label =
        L"Direct total speaker span: " +
        oneDecimal(dialog.working.direct_span_degrees) + L" deg";
    SetWindowTextW(dialog.span_label, span_label.c_str());

    const float measured_span = measuredSpeakerSpan(dialog.working);
    const float selected_span = effectiveSpeakerSpan(dialog.working);
    const float measured_radial_distance = std::hypot(
        dialog.working.listening_distance_cm,
        0.5F * dialog.working.speaker_spacing_cm);
    const float dsp_distance_cm =
        100.0F * effectiveSpeakerDistanceMetres(dialog.working);
    std::wstring result =
        L"DSP span: " + oneDecimal(selected_span) +
        L" deg    Measured span: " + oneDecimal(measured_span) +
        L" deg    Toe-in each: " + oneDecimal(0.5F * measured_span) +
        L" deg\r\nDSP head-to-speaker radial distance: " +
        oneDecimal(dsp_distance_cm) + L" cm    Measured radial distance: " +
        oneDecimal(measured_radial_distance) +
        L" cm. Toe-in is physical placement guidance only.\r\nDirect-angle "
        L"mode uses a nominal 100 cm distance; measured mode uses the actual "
        L"spacing, distance, and effective head diameter in the DSP.";
    SetWindowTextW(dialog.result_label, result.c_str());
}

[[nodiscard]] bool validateGeometry(const SpeakerGeometry& geometry,
                                    std::wstring& error) {
    if (geometry.direct_span_degrees < 8.0F ||
        geometry.direct_span_degrees > 60.0F) {
        error = L"Direct speaker span must be from 8 to 60 degrees.";
        return false;
    }
    if (geometry.speaker_spacing_cm < 10.0F ||
        geometry.speaker_spacing_cm > 200.0F) {
        error = L"Speaker-center spacing must be from 10 to 200 cm.";
        return false;
    }
    if (geometry.listening_distance_cm < 20.0F ||
        geometry.listening_distance_cm > 300.0F) {
        error = L"Head distance must be from 20 to 300 cm.";
        return false;
    }
    if (geometry.head_width_cm < 13.0F || geometry.head_width_cm > 24.0F) {
        error = L"Effective head diameter must be from 13 to 24 cm.";
        return false;
    }
    const float span = effectiveSpeakerSpan(geometry);
    if (span < 8.0F || span > 60.0F) {
        error = L"The measured layout produces a span outside the DSP's 8 to "
                L"60 degree range. Adjust spacing or distance.";
        return false;
    }
    return true;
}

LRESULT CALLBACK geometryWindowProcedure(HWND window, UINT message,
                                         WPARAM wparam, LPARAM lparam) {
    auto* dialog = reinterpret_cast<GeometryDialogState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        dialog = static_cast<GeometryDialogState*>(create->lpCreateParams);
        dialog->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(dialog));
    }

    switch (message) {
        case WM_CREATE: {
            makeControl(0, L"STATIC",
                        L"Horizontal geometry controls the transaural filter. "
                        L"Measure from speaker center to speaker center and from "
                        L"their midpoint line to the point between your ears.",
                        0, 28, 20, 832, 48, window, -1);
            dialog->direct_radio = makeControl(
                0, L"BUTTON", L"Use direct angle",
                BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 28, 82, 220, 32,
                window, kGeometryDirectRadio);
            dialog->measured_radio = makeControl(
                0, L"BUTTON", L"Calculate from measurements",
                BS_AUTORADIOBUTTON | WS_TABSTOP, 320, 82, 310, 32, window,
                kGeometryMeasuredRadio);
            SendMessageW(dialog->direct_radio, BM_SETCHECK,
                         dialog->working.use_measurements ? BST_UNCHECKED
                                                          : BST_CHECKED,
                         0);
            SendMessageW(dialog->measured_radio, BM_SETCHECK,
                         dialog->working.use_measurements ? BST_CHECKED
                                                          : BST_UNCHECKED,
                         0);

            dialog->span_label = makeControl(
                0, L"STATIC", L"Direct total speaker span", 0, 28, 126, 360,
                26, window, -1);
            dialog->span_slider = makeControl(
                0, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP,
                28, 154, 832, 46, window, kGeometrySpanSlider);
            SendMessageW(dialog->span_slider, TBM_SETRANGE, TRUE,
                         MAKELPARAM(8, 60));
            SendMessageW(dialog->span_slider, TBM_SETTICFREQ, 4, 0);
            SendMessageW(dialog->span_slider, TBM_SETPOS, TRUE,
                         static_cast<LPARAM>(
                             std::lround(dialog->working.direct_span_degrees)));

            makeControl(0, L"STATIC", L"Speaker-center spacing (cm)", 0, 28,
                        222, 250, 26, window, -1);
            dialog->spacing_edit = makeControl(
                WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP,
                300, 216, 140, 38, window, kGeometrySpacingEdit);
            setEditNumber(dialog->spacing_edit,
                          dialog->working.speaker_spacing_cm);
            makeControl(0, L"STATIC", L"Head distance from speaker line (cm)",
                        0, 470, 222, 235, 26, window, -1);
            dialog->distance_edit = makeControl(
                WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP,
                720, 216, 140, 38, window, kGeometryDistanceEdit);
            setEditNumber(dialog->distance_edit,
                          dialog->working.listening_distance_cm);

            makeControl(0, L"STATIC", L"Effective head diameter (cm)", 0, 28,
                        290, 250, 26, window, -1);
            dialog->head_width_edit = makeControl(
                WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP,
                300, 284, 140, 38, window, kGeometryHeadWidthEdit);
            setEditNumber(dialog->head_width_edit,
                          dialog->working.head_width_cm);
            makeControl(0, L"STATIC", L"Usually leave this at 18 cm; it changes "
                                         L"the modeled inter-ear delay.",
                        0, 470, 290, 390, 40, window, -1);

            dialog->result_label = makeControl(
                WS_EX_CLIENTEDGE, L"STATIC", L"", SS_LEFT, 28, 354, 832, 104,
                window, -1);
            makeControl(0, L"BUTTON", L"Apply / preview",
                        BS_DEFPUSHBUTTON | WS_TABSTOP,
                        490, 482, 170, 48, window, kGeometryApplyButton);
            makeControl(0, L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP,
                        690, 482, 170, 48, window, kGeometryCancelButton);
            updateGeometryDialog(*dialog);
            return 0;
        }
        case WM_HSCROLL:
            if (dialog != nullptr &&
                reinterpret_cast<HWND>(lparam) == dialog->span_slider) {
                updateGeometryDialog(*dialog);
                return 0;
            }
            break;
        case WM_COMMAND:
            if (dialog == nullptr) {
                return 0;
            }
            switch (LOWORD(wparam)) {
                case kGeometryDirectRadio:
                case kGeometryMeasuredRadio:
                    if (HIWORD(wparam) == BN_CLICKED) {
                        dialog->working.use_measurements =
                            LOWORD(wparam) == kGeometryMeasuredRadio;
                        updateGeometryDialog(*dialog);
                    }
                    return 0;
                case kGeometrySpacingEdit:
                case kGeometryDistanceEdit:
                case kGeometryHeadWidthEdit:
                    if (HIWORD(wparam) == EN_CHANGE) {
                        updateGeometryDialog(*dialog);
                    }
                    return 0;
                case kGeometryApplyButton: {
                    SpeakerGeometry candidate = dialog->working;
                    if (!readEditNumber(dialog->spacing_edit,
                                        candidate.speaker_spacing_cm) ||
                        !readEditNumber(dialog->distance_edit,
                                        candidate.listening_distance_cm) ||
                        !readEditNumber(dialog->head_width_edit,
                                        candidate.head_width_cm)) {
                        MessageBoxW(window, L"Enter valid numeric geometry values.",
                                    L"Speaker geometry", MB_OK | MB_ICONWARNING);
                        return 0;
                    }
                    candidate.direct_span_degrees = static_cast<float>(
                        SendMessageW(dialog->span_slider, TBM_GETPOS, 0, 0));
                    std::wstring error;
                    if (!validateGeometry(candidate, error)) {
                        MessageBoxW(window, error.c_str(), L"Speaker geometry",
                                    MB_OK | MB_ICONWARNING);
                        return 0;
                    }
                    dialog->app->geometry = candidate;
                    dialog->working = candidate;
                    updateGeometrySummary(*dialog->app);
                    if (dialog->app->running) {
                        publishGeometryToRunningProcessor(*dialog->app);
                        appendStatus(
                            *dialog->app,
                            L"Live geometry preview requested: " +
                                oneDecimal(effectiveSpeakerSpan(candidate)) +
                                L" deg, head " +
                                oneDecimal(candidate.head_width_cm) + L" cm.");
                    }
                    updateGeometryDialog(*dialog);
                    return 0;
                }
                case kGeometryCancelButton:
                    DestroyWindow(window);
                    return 0;
                default:
                    break;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void showGeometrySettings(AppState& state) {
    GeometryDialogState dialog;
    dialog.app = &state;
    dialog.working = state.geometry;

    RECT parent_rect{};
    GetWindowRect(state.window, &parent_rect);
    constexpr int width = 910;
    constexpr int height = 580;
    const int x = parent_rect.left +
                  std::max(0L, (parent_rect.right - parent_rect.left - width) / 2);
    const int y = parent_rect.top +
                  std::max(0L, (parent_rect.bottom - parent_rect.top - height) / 2);
    HWND geometry_window = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kGeometryWindowClass,
        L"Detailed speaker geometry", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        x, y, width, height, state.window, nullptr, GetModuleHandleW(nullptr),
        &dialog);
    if (geometry_window == nullptr) {
        MessageBoxW(state.window, L"Could not open the geometry settings.",
                    L"Speaker geometry", MB_OK | MB_ICONERROR);
        return;
    }

    EnableWindow(state.window, FALSE);
    ShowWindow(geometry_window, SW_SHOW);
    UpdateWindow(geometry_window);
    MSG message{};
    while (IsWindow(geometry_window)) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) {
            break;
        }
        if (!IsDialogMessageW(geometry_window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(state.window, TRUE);
    SetForegroundWindow(state.window);
}

void appendStatus(AppState& state, const std::wstring& text) {
    const int length = GetWindowTextLengthW(state.status_edit);
    SendMessageW(state.status_edit, EM_SETSEL, length, length);
    std::wstring line = text;
    line += L"\r\n";
    SendMessageW(state.status_edit, EM_REPLACESEL, FALSE,
                 reinterpret_cast<LPARAM>(line.c_str()));
}

void setRunning(AppState& state, bool running, bool live_controls = false) {
    state.running = running;
    state.live_controls_active = running && live_controls;
    EnableWindow(state.capture_combo, !running);
    EnableWindow(state.render_combo, !running);
    EnableWindow(state.refresh_button, !running);
    EnableWindow(state.mode_combo, !running);
    EnableWindow(state.ab_switch_button, state.live_controls_active);
    const bool transaural_active = isTransauralMode(state.active_spatial_mode);
    EnableWindow(state.transaural_strength_slider,
                 transaural_active);
    EnableWindow(state.transaural_side_width_slider,
                 transaural_active);
    const bool game_active = state.active_spatial_mode == ProcessingMode::transaural_game;
    EnableWindow(state.game_reference_button,game_active);
    EnableWindow(state.game_wild_button,game_active);
    EnableWindow(state.speaker_geometry_button,
                 transaural_active);
    EnableWindow(state.rear_profile_combo, !running && transaural_active);
    EnableWindow(state.use_24khz_check, !running);
    // The audio workers poll this atomic in both live-bridge and WAV-test mode.
    EnableWindow(state.fixed_channel_headroom_check, TRUE);
    EnableWindow(state.latency_slider,
                 !running || state.live_controls_active);
    EnableWindow(state.resync_button, state.live_controls_active);
    EnableWindow(state.start_button,
                 !running && !state.capture_endpoints.empty() &&
                     !state.render_endpoints.empty());
    EnableWindow(state.file_edit, !running);
    EnableWindow(state.browse_button, !running);
    EnableWindow(state.channel_combo, !running);
    EnableWindow(state.play_button, !running && !state.render_endpoints.empty());
    EnableWindow(state.stop_button, running);
}

void fillCombo(HWND combo, const std::vector<EndpointInfo>& endpoints) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (const EndpointInfo& endpoint : endpoints) {
        SendMessageW(combo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(endpoint.name.c_str()));
    }
    if (!endpoints.empty()) {
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
    }
}

void refreshEndpoints(AppState& state) {
    try {
        state.capture_endpoints = stereo_surround::enumerateCaptureEndpoints();
        state.render_endpoints = stereo_surround::enumerateRenderEndpoints();
        fillCombo(state.capture_combo, state.capture_endpoints);
        fillCombo(state.render_combo, state.render_endpoints);
        appendStatus(state,
                     L"Found " + std::to_wstring(state.capture_endpoints.size()) +
                         L" active capture endpoint(s) and " +
                         std::to_wstring(state.render_endpoints.size()) +
                         L" active render endpoint(s).");
    } catch (const std::exception& error) {
        appendStatus(state, L"Endpoint refresh failed: " +
                                std::wstring(error.what(), error.what() +
                                                               std::char_traits<char>::length(
                                                                   error.what())));
    }
    setRunning(state, false);
}

[[nodiscard]] BridgeOptions selectedOptions(const AppState& state) {
    BridgeOptions options;
    options.mode = processingModeFromCombo(state.mode_combo);
    options.spatial_mode = options.mode == ProcessingMode::matrix
                               ? state.active_spatial_mode
                               : options.mode;
    options.transaural_strength = static_cast<float>(
                                      SendMessageW(state.transaural_strength_slider,
                                                   TBM_GETPOS, 0, 0)) /
                                  100.0F;
    options.transaural_side_width = static_cast<float>(
                                        SendMessageW(
                                            state.transaural_side_width_slider,
                                            TBM_GETPOS, 0, 0)) /
                                    100.0F;
    options.speaker_span_degrees = effectiveSpeakerSpan(state.geometry);
    options.speaker_distance_metres =
        effectiveSpeakerDistanceMetres(state.geometry);
    options.head_width_metres = state.geometry.head_width_cm / 100.0F;
    options.transaural_rear_profile = static_cast<unsigned>(std::clamp(
        static_cast<int>(SendMessageW(state.rear_profile_combo, CB_GETCURSEL,
                                      0, 0)),
        0, 4));
    options.use_24khz_stream =
        SendMessageW(state.use_24khz_check, BM_GETCHECK, 0, 0) == BST_CHECKED;
    options.fixed_channel_headroom =
        SendMessageW(state.fixed_channel_headroom_check, BM_GETCHECK, 0, 0) ==
        BST_CHECKED;
    options.target_latency_ms = static_cast<unsigned>(
        SendMessageW(state.latency_slider, TBM_GETPOS, 0, 0));
    return options;
}

void updateAbSwitchLabel(AppState& state, ProcessingMode mode) {
    const std::wstring label =
        std::wstring(L"A/B active: ") + processingModeName(mode);
    SetWindowTextW(state.ab_switch_button, label.c_str());
}

void switchLiveProcessingMode(AppState& state) {
    if (!state.live_controls_active) {
        return;
    }
    const ProcessingMode current =
        state.controls.mode.load(std::memory_order_relaxed);
    const ProcessingMode next = current == ProcessingMode::matrix
                                    ? state.active_spatial_mode
                                    : ProcessingMode::matrix;
    state.controls.mode.store(next, std::memory_order_relaxed);
    SendMessageW(state.mode_combo, CB_SETCURSEL,
                 processingModeComboIndex(next), 0);
    updateAbSwitchLabel(state, next);
    appendStatus(state, std::wstring(L"A/B switched to ") +
                            processingModeName(next) + L" processing.");
}

void updateTransauralStrength(AppState& state) {
    const auto percent =
        SendMessageW(state.transaural_strength_slider, TBM_GETPOS, 0, 0);
    const bool game = state.active_spatial_mode == ProcessingMode::transaural_game;
    const std::wstring label = std::wstring(game ? L"Spatial mix: " : L"Legacy strength: ") +
        std::to_wstring(percent) + (game ? L"%  (100% = full)" : L"%  (Hyper: 125%)");
    SetWindowTextW(state.transaural_strength_label, label.c_str());
    if (state.live_controls_active || state.running) {
        state.controls.transaural_strength.store(
            static_cast<float>(percent) / 100.0F, std::memory_order_relaxed);
    }
}

void updateTransauralSideWidth(AppState& state) {
    const auto percent =
        SendMessageW(state.transaural_side_width_slider, TBM_GETPOS, 0, 0);
    const bool game = state.active_spatial_mode == ProcessingMode::transaural_game;
    const std::wstring label = std::wstring(game ? L"Expansion: " : L"Legacy side width: ") +
        std::to_wstring(percent) + (game ? L"%  (0 ref / 150 wild)" : L"%  (0 = XTC)");
    SetWindowTextW(state.transaural_side_width_label, label.c_str());
    if (state.live_controls_active || state.running) {
        state.controls.transaural_side_width.store(
            static_cast<float>(percent) / 100.0F, std::memory_order_relaxed);
    }
}

void updateProcessorSelection(AppState& state) {
    const ProcessingMode selected = processingModeFromCombo(state.mode_combo);
    if (selected != ProcessingMode::matrix) {
        state.active_spatial_mode = selected;
    }
    const bool game = state.active_spatial_mode == ProcessingMode::transaural_game;
    SendMessageW(state.transaural_strength_slider,TBM_SETRANGEMAX,TRUE,game ? 100 : 150);
    if (game && SendMessageW(state.transaural_strength_slider,TBM_GETPOS,0,0) > 100)
        SendMessageW(state.transaural_strength_slider,TBM_SETPOS,TRUE,100);
    updateTransauralStrength(state);
    updateTransauralSideWidth(state);
    setRunning(state, state.running, state.live_controls_active);
    updateAbSwitchLabel(state, selected);
}

void updateLatencyTarget(AppState& state) {
    const auto milliseconds =
        SendMessageW(state.latency_slider, TBM_GETPOS, 0, 0);
    const std::wstring label =
        L"Target queue: " + std::to_wstring(milliseconds) + L" ms";
    SetWindowTextW(state.latency_label, label.c_str());
    if (state.live_controls_active) {
        state.controls.target_latency_ms.store(
            static_cast<unsigned>(milliseconds), std::memory_order_relaxed);
    }
}

void resetMonitorSampling(AppState& state) {
    state.previous_captured_frames = 0;
    state.previous_rendered_frames = 0;
    state.previous_monitor_tick = GetTickCount64();
    state.smoothed_capture_rate = 0.0;
    state.smoothed_render_rate = 0.0;
    state.displayed_peaks.fill(0.0F);
}

void updateMonitorUi(AppState& state) {
    if (state.spatial_monitor_label) {
        const float peak = state.monitor.spatial_pre_peak.exchange(0,std::memory_order_relaxed);
        const float gain = state.monitor.spatial_limiter_gain.exchange(1,std::memory_order_relaxed);
        std::wostringstream info;
        info << std::fixed << std::setprecision(1) << L"Spatial DSP: peak ";
        if (peak > 0) info << 20*std::log10(peak) << L" dBFS";
        else info << L"--";
        info << L"   Limiter reduction " << -20*std::log10(std::max(gain, .000001F))
             << L" dB  (aim: 0)";
        SetWindowTextW(state.spatial_monitor_label,info.str().c_str());
    }
    const unsigned input_rate =
        state.monitor.input_sample_rate.load(std::memory_order_relaxed);
    const unsigned output_rate =
        state.monitor.output_sample_rate.load(std::memory_order_relaxed);
    const unsigned input_channels =
        state.monitor.input_channels.load(std::memory_order_relaxed);
    const unsigned output_channels =
        state.monitor.output_channels.load(std::memory_order_relaxed);

    std::wostringstream format;
    if (input_rate == 0 || output_rate == 0) {
        format << L"Bridge monitor: waiting for a live bridge";
    } else {
        format << L"Format: " << input_channels << L" ch / " << input_rate
               << L" Hz  ->  " << output_channels << L" ch / " << output_rate
               << L" Hz";
    }
    SetWindowTextW(state.monitor_format_label, format.str().c_str());

    const std::size_t queue_frames =
        state.monitor.queue_frames.load(std::memory_order_relaxed);
    const std::size_t target_frames =
        state.monitor.target_frames.load(std::memory_order_relaxed);
    const std::size_t capacity_frames =
        state.monitor.queue_capacity_frames.load(std::memory_order_relaxed);
    const std::size_t progress_max = std::max<std::size_t>(2, target_frames * 2);
    SendMessageW(state.queue_progress, PBM_SETRANGE32, 0,
                 static_cast<LPARAM>(std::min<std::size_t>(
                     progress_max, static_cast<std::size_t>(INT_MAX))));
    SendMessageW(state.queue_progress, PBM_SETPOS,
                 static_cast<WPARAM>(std::min(queue_frames, progress_max)), 0);

    const double queue_ms = input_rate == 0
                                ? 0.0
                                : 1000.0 * static_cast<double>(queue_frames) /
                                      static_cast<double>(input_rate);
    const double target_ms = input_rate == 0
                                 ? 0.0
                                 : 1000.0 * static_cast<double>(target_frames) /
                                       static_cast<double>(input_rate);
    const double capacity_ms = input_rate == 0
                                   ? 0.0
                                   : 1000.0 *
                                         static_cast<double>(capacity_frames) /
                                         static_cast<double>(input_rate);
    std::wostringstream queue;
    queue << std::fixed << std::setprecision(1) << L"Queue " << queue_ms
          << L" ms / target " << target_ms << L" ms  (capacity "
          << capacity_ms << L" ms, "
          << (state.monitor.primed.load(std::memory_order_relaxed) ? L"running"
                                                                   : L"priming")
          << L")";
    SetWindowTextW(state.queue_label, queue.str().c_str());

    const std::uint64_t captured =
        state.monitor.captured_frames.load(std::memory_order_relaxed);
    const std::uint64_t rendered =
        state.monitor.rendered_frames.load(std::memory_order_relaxed);
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG elapsed = now - state.previous_monitor_tick;
    if (elapsed > 0 && state.previous_monitor_tick != 0) {
        const double capture_rate =
            1000.0 * static_cast<double>(captured -
                                         state.previous_captured_frames) /
            static_cast<double>(elapsed);
        const double render_rate =
            1000.0 * static_cast<double>(rendered -
                                         state.previous_rendered_frames) /
            static_cast<double>(elapsed);
        constexpr double smoothing = 0.25;
        state.smoothed_capture_rate = state.smoothed_capture_rate == 0.0
                                          ? capture_rate
                                          : state.smoothed_capture_rate *
                                                    (1.0 - smoothing) +
                                                capture_rate * smoothing;
        state.smoothed_render_rate = state.smoothed_render_rate == 0.0
                                         ? render_rate
                                         : state.smoothed_render_rate *
                                                   (1.0 - smoothing) +
                                               render_rate * smoothing;
    }
    state.previous_captured_frames = captured;
    state.previous_rendered_frames = rendered;
    state.previous_monitor_tick = now;

    std::wostringstream rates;
    rates << std::fixed << std::setprecision(1) << L"Capture "
          << state.smoothed_capture_rate << L" Hz";
    if (input_rate != 0) {
        rates << L" (" << 100.0 * state.smoothed_capture_rate / input_rate
              << L"%)";
    }
    rates << L"   Render " << state.smoothed_render_rate << L" Hz";
    if (output_rate != 0) {
        rates << L" (" << 100.0 * state.smoothed_render_rate / output_rate
              << L"%)";
    }
    rates << L"   Adaptive resample " << std::setprecision(3)
          << state.monitor.resample_percent.load(std::memory_order_relaxed)
          << L"%";
    SetWindowTextW(state.rate_label, rates.str().c_str());

    std::wostringstream statistics;
    statistics << L"Underruns "
               << state.monitor.output_underruns.load(std::memory_order_relaxed)
               << L"   Overflows "
               << state.monitor.input_overflows.load(std::memory_order_relaxed)
               << L"   Input discontinuities "
               << state.monitor.input_discontinuities.load(
                      std::memory_order_relaxed)
               << L"   Manual resyncs "
               << state.monitor.resyncs.load(std::memory_order_relaxed);
    SetWindowTextW(state.stats_label, statistics.str().c_str());

    for (std::size_t channel = 0; channel < state.channel_meters.size();
         ++channel) {
        const float measured = state.monitor.input_peaks[channel].load(
            std::memory_order_relaxed);
        state.displayed_peaks[channel] =
            std::max(measured, state.displayed_peaks[channel] * 0.78F);
        const float decibels =
            20.0F * std::log10(std::max(state.displayed_peaks[channel], 1.0e-4F));
        const int meter_position = static_cast<int>(
            1000.0F * std::clamp((decibels + 60.0F) / 60.0F, 0.0F, 1.0F));
        SendMessageW(state.channel_meters[channel], PBM_SETPOS,
                     static_cast<WPARAM>(meter_position), 0);
    }
}

[[nodiscard]] int comboSelection(HWND combo) {
    return static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
}

[[nodiscard]] std::wstring editText(HWND edit) {
    const int length = GetWindowTextLengthW(edit);
    std::wstring text(static_cast<std::size_t>(length) + 1U, L'\0');
    if (length > 0) {
        GetWindowTextW(edit, text.data(), length + 1);
    }
    text.resize(static_cast<std::size_t>(length));
    return text;
}

void postCompletion(HWND window, std::wstring text, bool error) {
    auto completion = std::make_unique<Completion>();
    completion->text = std::move(text);
    completion->error = error;
    Completion* raw = completion.release();
    if (!PostMessageW(window, kWorkerFinished, 0,
                      reinterpret_cast<LPARAM>(raw))) {
        delete raw;
    }
}

void initializeWorker(AppState& state, bool live_controls) {
    if (state.worker.joinable()) {
        state.worker.join();
    }
    state.stop_requested.store(false, std::memory_order_relaxed);
    if (live_controls) {
        state.monitor.reset();
        resetMonitorSampling(state);
    }
    setRunning(state, true, live_controls);
}

void startBridge(AppState& state) {
    const int capture_index = comboSelection(state.capture_combo);
    const int render_index = comboSelection(state.render_combo);
    if (capture_index < 0 || render_index < 0 ||
        static_cast<std::size_t>(capture_index) >= state.capture_endpoints.size() ||
        static_cast<std::size_t>(render_index) >= state.render_endpoints.size()) {
        appendStatus(state, L"Select both an input and output endpoint.");
        return;
    }

    const EndpointInfo input = state.capture_endpoints[static_cast<std::size_t>(capture_index)];
    const EndpointInfo output = state.render_endpoints[static_cast<std::size_t>(render_index)];
    const BridgeOptions options = selectedOptions(state);
    if (options.mode != ProcessingMode::matrix) {
        state.active_spatial_mode = options.mode;
    }
    state.controls.mode.store(options.mode, std::memory_order_relaxed);
    state.controls.transaural_strength.store(options.transaural_strength,
                                             std::memory_order_relaxed);
    state.controls.transaural_side_width.store(options.transaural_side_width,
                                               std::memory_order_relaxed);
    state.controls.speaker_span_degrees.store(options.speaker_span_degrees,
                                              std::memory_order_relaxed);
    state.controls.speaker_distance_metres.store(
        options.speaker_distance_metres, std::memory_order_relaxed);
    state.controls.head_width_metres.store(options.head_width_metres,
                                           std::memory_order_relaxed);
    state.controls.fixed_channel_headroom.store(
        options.fixed_channel_headroom, std::memory_order_relaxed);
    state.controls.target_latency_ms.store(options.target_latency_ms,
                                           std::memory_order_relaxed);
    updateAbSwitchLabel(state, options.mode);
    initializeWorker(state, true);
    appendStatus(state, L"Starting live bridge: " + input.name + L" -> " +
                            output.name + L" (" +
                            processingModeName(options.mode) + L", spatial " +
                            std::to_wstring(static_cast<int>(
                                options.transaural_strength * 100.0F)) +
                            L"%, side " +
                            std::to_wstring(static_cast<int>(
                                options.transaural_side_width * 100.0F)) +
                            L"% at " +
                            std::to_wstring(static_cast<int>(
                                options.speaker_span_degrees)) +
                            L" deg, distance " +
                            oneDecimal(options.speaker_distance_metres * 100.0F) +
                            L" cm, head " +
                            oneDecimal(options.head_width_metres * 100.0F) +
                            L" cm, stream " +
                            (options.use_24khz_stream ? L"24 kHz" : L"endpoint rate") +
                            (options.fixed_channel_headroom
                                 ? L", fixed 1/8 channel headroom"
                                 : L", normal channel level") +
                            L", queue " +
                            std::to_wstring(options.target_latency_ms) + L" ms)");

    state.worker = std::jthread([&state, input, output, options] {
        const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        try {
            if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE) {
                throw std::runtime_error("Worker COM initialization failed");
            }
            stereo_surround::runWasapiBridge(input, output, options,
                                              state.controls, state.monitor,
                                              state.stop_requested);
            postCompletion(state.window, L"Live bridge stopped.", false);
        } catch (const std::exception& error) {
            const std::string message(error.what());
            postCompletion(state.window,
                           L"Live bridge failed: " +
                               std::wstring(message.begin(), message.end()),
                           true);
        }
        if (com_result == S_OK || com_result == S_FALSE) {
            CoUninitialize();
        }
    });
}

void browseForWav(AppState& state) {
    std::vector<wchar_t> path(32'768, L'\0');
    const std::wstring current = editText(state.file_edit);
    if (!current.empty() && current.size() + 1 < path.size()) {
        std::copy(current.begin(), current.end(), path.begin());
    }

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = state.window;
    dialog.lpstrFilter = L"WAV audio (*.wav)\0*.wav\0All files (*.*)\0*.*\0\0";
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    dialog.lpstrDefExt = L"wav";
    if (GetOpenFileNameW(&dialog)) {
        SetWindowTextW(state.file_edit, path.data());
    }
}

void startFileTest(AppState& state) {
    const int render_index = comboSelection(state.render_combo);
    const int channel_index = comboSelection(state.channel_combo);
    const std::wstring path = editText(state.file_edit);
    if (render_index < 0 ||
        static_cast<std::size_t>(render_index) >= state.render_endpoints.size()) {
        appendStatus(state, L"Select an output endpoint.");
        return;
    }
    if (channel_index < 0 || channel_index >= 8) {
        appendStatus(state, L"Select a virtual speaker channel.");
        return;
    }
    if (path.empty()) {
        appendStatus(state, L"Select a PCM/float WAV test file.");
        return;
    }

    const EndpointInfo output = state.render_endpoints[static_cast<std::size_t>(render_index)];
    const BridgeOptions options = selectedOptions(state);
    state.controls.transaural_strength.store(options.transaural_strength,
                                             std::memory_order_relaxed);
    state.controls.transaural_side_width.store(options.transaural_side_width,
                                               std::memory_order_relaxed);
    state.controls.speaker_span_degrees.store(options.speaker_span_degrees,
                                              std::memory_order_relaxed);
    state.controls.speaker_distance_metres.store(
        options.speaker_distance_metres, std::memory_order_relaxed);
    state.controls.head_width_metres.store(options.head_width_metres,
                                           std::memory_order_relaxed);
    state.controls.fixed_channel_headroom.store(
        options.fixed_channel_headroom, std::memory_order_relaxed);
    initializeWorker(state, false);
    appendStatus(state, L"Playing WAV through the selected virtual channel -> " +
                            output.name);

    state.worker = std::jthread([&state, path, channel_index, output, options] {
        const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        try {
            if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE) {
                throw std::runtime_error("Worker COM initialization failed");
            }
            stereo_surround::playChannelTestFile(
                path, static_cast<std::size_t>(channel_index), output, options,
                state.controls, state.stop_requested, &state.monitor);
            postCompletion(state.window, L"Channel test finished.", false);
        } catch (const std::exception& error) {
            const std::string message(error.what());
            postCompletion(state.window,
                           L"Channel test failed: " +
                               std::wstring(message.begin(), message.end()),
                           true);
        }
        if (com_result == S_OK || com_result == S_FALSE) {
            CoUninitialize();
        }
    });
}

void createUi(AppState& state, HWND window) {
    state.window = window;
    makeControl(0, L"STATIC", L"7.1 capture endpoint", 0, 24, 18, 260, 24,
                window, -1);
    state.capture_combo = makeControl(
        0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 24,
        44, 900, 260, window, kCaptureCombo);
    state.refresh_button = makeControl(0, L"BUTTON", L"Refresh",
                                       BS_PUSHBUTTON | WS_TABSTOP, 944, 42, 246,
                                       40, window, kRefreshButton);

    makeControl(0, L"STATIC", L"Stereo output endpoint", 0, 24, 92, 260, 24,
                window, -1);
    state.render_combo = makeControl(
        0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 24,
        118, 900, 260, window, kRenderCombo);

    makeControl(0, L"BUTTON", L"Processor", BS_GROUPBOX, 16, 166, 1180, 240,
                window, -1);
    makeControl(0, L"STATIC", L"Processing path", 0, 32, 192, 220, 24, window,
                -1);
    state.mode_combo = makeControl(
        0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, 32, 218, 270,
        190, window, kModeCombo);
    SendMessageW(state.mode_combo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"Legacy Natural"));
    SendMessageW(state.mode_combo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"Legacy Hyper"));
    SendMessageW(state.mode_combo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"Naive stereo downmix"));
    SendMessageW(state.mode_combo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"Game 7.1 - reference / wild"));
    SendMessageW(state.mode_combo, CB_SETCURSEL, 3, 0);
    state.ab_switch_button = makeControl(
        0, L"BUTTON", L"A/B active: Game 7.1",
        BS_PUSHBUTTON | WS_TABSTOP, 900,
        212, 230, 48, window, kAbSwitchButton);

    makeControl(0, L"STATIC", L"Physical speaker geometry", 0, 320, 192, 190, 24,
                window, -1);
    state.speaker_geometry_button = makeControl(
        0, L"BUTTON", L"Detailed settings...", BS_PUSHBUTTON | WS_TABSTOP, 320,
        216, 180, 42, window, kSpeakerGeometryButton);
    state.speaker_geometry_summary = makeControl(
        0, L"STATIC", L"Direct 20.0 deg, head 18.0 cm", 0, 515, 216, 170, 42,
        window, -1);

    makeControl(0, L"STATIC", L"Rear cue profile", 0, 700, 192, 170, 24,
                window, -1);
    state.rear_profile_combo = makeControl(
        0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, 700, 218, 180,
        180, window, kRearProfileCombo);
    constexpr std::array<const wchar_t*, 5> rear_profiles = {
        L"A - lower notch", L"B", L"C - default", L"D", L"E - higher notch"};
    for (const wchar_t* profile : rear_profiles) {
        SendMessageW(state.rear_profile_combo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(profile));
    }
    SendMessageW(state.rear_profile_combo, CB_SETCURSEL, 2, 0);

    makeControl(0,L"STATIC",L"Game 7.1 quick comparison",0,32,270,270,24,window,-1);
    state.game_reference_button = makeControl(0,L"BUTTON",L"Reference",BS_PUSHBUTTON|WS_TABSTOP,
        32,300,125,40,window,kGameReferenceButton);
    state.game_wild_button = makeControl(0,L"BUTTON",L"Wild 150%",BS_PUSHBUTTON|WS_TABSTOP,
        168,300,130,40,window,kGameWildButton);
    state.spatial_monitor_label = makeControl(0,L"STATIC",L"Spatial DSP: waiting",0,
        415,354,755,30,window,-1);
    state.transaural_strength_label = makeControl(
        0, L"STATIC", L"Transaural strength: 125%", 0, 320,
        270, 280, 24, window, -1);
    state.transaural_strength_slider = makeControl(
        0, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, 320,
        292, 280, 42, window, kTransauralStrengthSlider);
    SendMessageW(state.transaural_strength_slider, TBM_SETRANGE, TRUE,
                 MAKELPARAM(0, 150));
    SendMessageW(state.transaural_strength_slider, TBM_SETTICFREQ, 25, 0);
    SendMessageW(state.transaural_strength_slider, TBM_SETPOS, TRUE, 100);

    state.transaural_side_width_label = makeControl(
        0, L"STATIC", L"Side width: 0%  (pure XTC)", 0, 630, 270,
        280, 24, window, -1);
    state.transaural_side_width_slider = makeControl(
        0, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, 630,
        292, 280, 42, window, kTransauralSideWidthSlider);
    SendMessageW(state.transaural_side_width_slider, TBM_SETRANGE, TRUE,
                 MAKELPARAM(0, 150));
    SendMessageW(state.transaural_side_width_slider, TBM_SETTICFREQ, 25, 0);
    SendMessageW(state.transaural_side_width_slider, TBM_SETPOS, TRUE, 0);

    state.use_24khz_check = makeControl(
        0, L"BUTTON", L"24 kHz stream  (lower DSP cost)",
        BS_AUTOCHECKBOX | WS_TABSTOP, 930, 292, 245, 38, window,
        kUse24KhzCheck);
    SendMessageW(state.use_24khz_check, BM_SETCHECK, BST_UNCHECKED, 0);

    state.fixed_channel_headroom_check = makeControl(
        0, L"BUTTON", L"Fixed 1/8 channel headroom  (-18 dB)",
        BS_AUTOCHECKBOX | WS_TABSTOP, 32, 350, 360, 38, window,
        kFixedChannelHeadroomCheck);
    SendMessageW(state.fixed_channel_headroom_check, BM_SETCHECK,
                 BST_UNCHECKED, 0);

    state.start_button = makeControl(0, L"BUTTON", L"Start live 7.1 -> stereo",
                                     BS_DEFPUSHBUTTON | WS_TABSTOP, 24, 422, 380,
                                     48, window, kStartBridgeButton);
    state.stop_button = makeControl(0, L"BUTTON", L"Stop",
                                    BS_PUSHBUTTON | WS_TABSTOP, 420, 422, 200, 48,
                                    window, kStopButton);

    makeControl(0, L"BUTTON", L"Live bridge monitor and buffering", BS_GROUPBOX,
                16, 482, 1180, 220, window, -1);
    state.monitor_format_label = makeControl(
        0, L"STATIC", L"Bridge monitor: waiting for a live bridge", 0, 32, 508,
        580, 24, window, -1);
    state.queue_progress = makeControl(
        0, PROGRESS_CLASSW, L"", PBS_SMOOTH, 32, 540, 500, 25, window, -1);
    SendMessageW(state.queue_progress, PBM_SETRANGE32, 0, 1000);
    SendMessageW(state.queue_progress, PBM_SETBARCOLOR, 0, RGB(40, 180, 70));
    state.queue_label = makeControl(0, L"STATIC", L"Queue 0.0 ms", 0, 550, 541,
                                    620, 24, window, -1);
    state.rate_label = makeControl(0, L"STATIC", L"Capture 0 Hz   Render 0 Hz",
                                   0, 32, 573, 720, 24, window, -1);
    state.stats_label = makeControl(
        0, L"STATIC",
        L"Underruns 0   Overflows 0   Input discontinuities 0   Manual resyncs 0",
        0, 32, 604, 720, 24, window, -1);
    state.latency_label = makeControl(0, L"STATIC", L"Target queue: 35 ms", 0,
                                      32, 643, 190, 24, window, -1);
    state.latency_slider = makeControl(
        0, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, 218,
        630, 350, 42, window, kLatencySlider);
    SendMessageW(state.latency_slider, TBM_SETRANGE, TRUE, MAKELPARAM(10, 250));
    SendMessageW(state.latency_slider, TBM_SETTICFREQ, 20, 0);
    SendMessageW(state.latency_slider, TBM_SETPOS, TRUE, 35);
    state.resync_button = makeControl(0, L"BUTTON", L"Resync queue",
                                      BS_PUSHBUTTON | WS_TABSTOP, 580, 630, 160,
                                      40, window, kResyncButton);

    constexpr std::array<const wchar_t*, 8> meter_names = {
        L"FL", L"FR", L"FC", L"LFE", L"BL", L"BR", L"SL", L"SR"};
    makeControl(0, L"STATIC", L"7.1 input levels", 0, 810, 506, 340, 24, window,
                -1);
    for (std::size_t channel = 0; channel < meter_names.size(); ++channel) {
        const int x = 800 + static_cast<int>(channel) * 47;
        state.channel_meters[channel] = makeControl(
            0, PROGRESS_CLASSW, L"", PBS_VERTICAL | PBS_SMOOTH, x, 535, 30, 105,
            window, -1);
        SendMessageW(state.channel_meters[channel], PBM_SETRANGE32, 0, 1000);
        SendMessageW(state.channel_meters[channel], PBM_SETBARCOLOR, 0,
                     RGB(55, 190, 65));
        makeControl(0, L"STATIC", meter_names[channel], SS_CENTER, x - 4, 647, 38,
                    22, window, -1);
    }

    makeControl(0, L"BUTTON", L"Channel test (does not use capture endpoint)",
                BS_GROUPBOX, 16, 716, 1180, 116, window, -1);
    state.file_edit = makeControl(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                  ES_AUTOHSCROLL | WS_TABSTOP, 32, 745, 850, 36,
                                  window, kFileEdit);
    state.browse_button = makeControl(0, L"BUTTON", L"Browse WAV...",
                                      BS_PUSHBUTTON | WS_TABSTOP, 900, 743, 280,
                                      40, window, kBrowseButton);
    state.channel_combo = makeControl(
        0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, 32, 788, 400, 190,
        window, kChannelCombo);
    constexpr const wchar_t* channels[] = {
        L"Front left (FL)", L"Front right (FR)", L"Center (FC)",
        L"Low-frequency effects (LFE)", L"Back left (BL)",
        L"Back right (BR)", L"Side left (SL)", L"Side right (SR)"};
    for (const wchar_t* channel : channels) {
        SendMessageW(state.channel_combo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(channel));
    }
    SendMessageW(state.channel_combo, CB_SETCURSEL, 4, 0);
    state.play_button = makeControl(0, L"BUTTON", L"Play in selected channel",
                                    BS_PUSHBUTTON | WS_TABSTOP, 450, 786, 360, 42,
                                    window, kPlayTestButton);

    state.status_edit = makeControl(
        WS_EX_CLIENTEDGE, L"EDIT", L"Start at low speaker volume.\r\n",
        ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL, 16, 846, 1180,
        86, window, kStatusEdit);
    updateTransauralStrength(state);
    updateTransauralSideWidth(state);
    updateGeometrySummary(state);
    updateProcessorSelection(state);
    setRunning(state, false);
    refreshEndpoints(state);
    SetTimer(window, kMonitorTimer, 250, nullptr);
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wparam,
                                 LPARAM lparam) {
    AppState* state =
        reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = static_cast<AppState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    switch (message) {
        case WM_CREATE:
            createUi(*state, window);
            return 0;
        case WM_HSCROLL:
            if (state != nullptr &&
                reinterpret_cast<HWND>(lparam) ==
                    state->transaural_strength_slider) {
                updateTransauralStrength(*state);
                return 0;
            }
            if (state != nullptr &&
                reinterpret_cast<HWND>(lparam) ==
                    state->transaural_side_width_slider) {
                updateTransauralSideWidth(*state);
                return 0;
            }
            if (state != nullptr &&
                reinterpret_cast<HWND>(lparam) == state->latency_slider) {
                updateLatencyTarget(*state);
                return 0;
            }
            break;
        case WM_TIMER:
            if (state != nullptr && wparam == kMonitorTimer) {
                updateMonitorUi(*state);
                return 0;
            }
            break;
        case WM_COMMAND:
            if (state == nullptr) {
                return 0;
            }
            switch (LOWORD(wparam)) {
                case kGameReferenceButton:
                case kGameWildButton:
                    if (state->active_spatial_mode == ProcessingMode::transaural_game) {
                        SendMessageW(state->transaural_strength_slider,TBM_SETPOS,TRUE,100);
                        SendMessageW(state->transaural_side_width_slider,TBM_SETPOS,TRUE,
                            LOWORD(wparam) == kGameWildButton ? 150 : 0);
                        updateTransauralStrength(*state);
                        updateTransauralSideWidth(*state);
                    }
                    return 0;
                case kModeCombo:
                    if (HIWORD(wparam) == CBN_SELCHANGE && !state->running) {
                        updateProcessorSelection(*state);
                    }
                    return 0;
                case kRefreshButton:
                    refreshEndpoints(*state);
                    return 0;
                case kSpeakerGeometryButton:
                    showGeometrySettings(*state);
                    return 0;
                case kStartBridgeButton:
                    if (!state->running) {
                        startBridge(*state);
                    }
                    return 0;
                case kStopButton:
                    state->stop_requested.store(true, std::memory_order_relaxed);
                    appendStatus(*state, L"Stopping...");
                    return 0;
                case kAbSwitchButton:
                    switchLiveProcessingMode(*state);
                    return 0;
                case kResyncButton:
                    if (state->live_controls_active) {
                        state->controls.resync_generation.fetch_add(
                            1, std::memory_order_relaxed);
                        appendStatus(*state,
                                     L"Manual queue resync requested.");
                    }
                    return 0;
                case kFixedChannelHeadroomCheck:
                    if (HIWORD(wparam) == BN_CLICKED) {
                        const bool enabled =
                            SendMessageW(state->fixed_channel_headroom_check,
                                         BM_GETCHECK, 0, 0) == BST_CHECKED;
                        state->controls.fixed_channel_headroom.store(
                            enabled, std::memory_order_relaxed);
                        appendStatus(
                            *state,
                            enabled
                                ? L"Fixed 1/8 channel headroom enabled (-18.06 dB)."
                                : L"Fixed 1/8 channel headroom disabled.");
                    }
                    return 0;
                case kBrowseButton:
                    browseForWav(*state);
                    return 0;
                case kPlayTestButton:
                    if (!state->running) {
                        startFileTest(*state);
                    }
                    return 0;
                default:
                    break;
            }
            break;
        case kWorkerFinished: {
            std::unique_ptr<Completion> completion(
                reinterpret_cast<Completion*>(lparam));
            if (state != nullptr) {
                if (state->worker.joinable()) {
                    state->worker.join();
                }
                appendStatus(*state, completion->text);
                setRunning(*state, false);
            }
            return 0;
        }
        case WM_CLOSE:
            if (state != nullptr) {
                state->stop_requested.store(true, std::memory_order_relaxed);
                if (state->worker.joinable()) {
                    state->worker.join();
                }
            }
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            KillTimer(window, kMonitorTimer);
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX common_controls{
        sizeof(common_controls),
        ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&common_controls);

    const int font_height = -MulDiv(11, static_cast<int>(GetDpiForSystem()), 72);
    const int label_font_height =
        -MulDiv(9, static_cast<int>(GetDpiForSystem()), 72);
    g_ui_font = CreateFontW(font_height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_label_font = CreateFontW(
        label_font_height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE) {
        MessageBoxW(nullptr, L"COM initialization failed.", L"Endpoint demo",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = windowProcedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    window_class.hIconSm = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 16, 16,
        LR_DEFAULTCOLOR));
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kWindowClass;
    RegisterClassExW(&window_class);

    WNDCLASSEXW geometry_window_class = window_class;
    geometry_window_class.lpfnWndProc = geometryWindowProcedure;
    geometry_window_class.hIcon = nullptr;
    geometry_window_class.hIconSm = nullptr;
    geometry_window_class.lpszClassName = kGeometryWindowClass;
    RegisterClassExW(&geometry_window_class);

    AppState state;
    HWND window = CreateWindowExW(
        0, kWindowClass, kWindowTitle,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
            WS_MAXIMIZEBOX | WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, 1220, 990, nullptr, nullptr, instance,
        &state);
    if (window == nullptr) {
        if (g_ui_font != nullptr) {
            DeleteObject(g_ui_font);
            g_ui_font = nullptr;
        }
        if (g_label_font != nullptr) {
            DeleteObject(g_label_font);
            g_label_font = nullptr;
        }
        if (com_result == S_OK || com_result == S_FALSE) {
            CoUninitialize();
        }
        return 1;
    }
    ShowWindow(window, show_command);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (com_result == S_OK || com_result == S_FALSE) {
        CoUninitialize();
    }
    if (g_ui_font != nullptr) {
        DeleteObject(g_ui_font);
        g_ui_font = nullptr;
    }
    if (g_label_font != nullptr) {
        DeleteObject(g_label_font);
        g_label_font = nullptr;
    }
    return static_cast<int>(message.wParam);
}
