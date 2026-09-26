// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

// Minimal Qt-free entry point for the Steam Deck / Steam Machine build
// (VITA3K_STEAMOS_NATIVE). No game picker: the title to boot is passed on
// the command line via the same --installed-path/-r flag the Qt and Android
// frontends already use (see config::init_config), so a Steam "non-Steam
// game" shortcut launches a specific title directly. Boot sequence and run
// loop are adapted from vita3k/android/jni/{native_bootstrap,main_android}.cpp,
// which already proved this whole path (config, app::init, IME, dialogs,
// gamepad touchpad/gyro) works with no Qt involved - this file strips the
// JNI/Android-specific glue and swaps in the desktop XDG paths already used
// by the Qt build (app::init_paths) plus a plain X11/Wayland FrameHost.

#include "interface.h"

#include <app/functions.h>
#include <app/session_controller.h>
#include <compat/functions.h>
#include <compat/state.h>
#include <config/functions.h>
#include <config/version.h>
#include <ctrl/functions.h>
#include <dialog/state.h>
#include <emuenv/state.h>
#include <ime/functions.h>
#include <ime/state.h>
#include <modules/module_parent.h>
#include <motion/event_handler.h>
#include <motion/functions.h>
#include <renderer/frame_host.h>
#include <renderer/functions.h>
#include <renderer/state.h>
#include <touch/functions.h>
#include <util/log.h>
#include <util/string_utils.h>

#include <SDL3/SDL.h>

#include <cstring>
#include <filesystem>
#include <optional>

namespace {

// Small, platform-independent IME-state helpers mirroring
// android/jni/android_state.cpp (is_any_ime_active, is_ime_dialog_active,
// finish_ime_dialog, cancel_ime_dialog) - inlined here rather than pulled from
// the android/ directory, since they only touch dialog/ime state, not JNI.
bool is_ime_dialog_active(const EmuEnvState &emuenv) {
    return emuenv.common_dialog.type == IME_DIALOG
        && emuenv.common_dialog.status == SCE_COMMON_DIALOG_STATUS_RUNNING;
}

bool is_any_ime_active(const EmuEnvState &emuenv) {
    return emuenv.ime.state || is_ime_dialog_active(emuenv);
}

void finish_ime_dialog(EmuEnvState &emuenv) {
    auto &dialog = emuenv.common_dialog;
    auto &ime = emuenv.ime;

    std::lock_guard<std::recursive_mutex> dialog_lock(dialog.mutex);
    std::lock_guard<std::mutex> ime_lock(ime.mutex);

    const size_t copy_len = std::min(static_cast<size_t>(ime.str.length()),
        static_cast<size_t>(dialog.ime.max_length));
    if (dialog.ime.result) {
        std::memcpy(dialog.ime.result, ime.str.c_str(), copy_len * sizeof(uint16_t));
        dialog.ime.result[copy_len] = 0;
    }

    const std::string utf8 = string_utils::utf16_to_utf8(ime.str);
    std::snprintf(dialog.ime.text, sizeof(dialog.ime.text), "%s", utf8.c_str());
    dialog.ime.status = SCE_IME_DIALOG_BUTTON_ENTER;
    dialog.status = SCE_COMMON_DIALOG_STATUS_FINISHED;
    dialog.result = SCE_COMMON_DIALOG_RESULT_OK;
}

void cancel_ime_dialog(EmuEnvState &emuenv) {
    auto &dialog = emuenv.common_dialog;
    if (!dialog.ime.cancelable)
        return;

    std::lock_guard<std::recursive_mutex> dialog_lock(dialog.mutex);
    dialog.ime.status = SCE_IME_DIALOG_BUTTON_CLOSE;
    dialog.status = SCE_COMMON_DIALOG_STATUS_FINISHED;
    dialog.result = SCE_COMMON_DIALOG_RESULT_USER_CANCELED;
}

// Desktop counterpart of android/jni/main_android.cpp's AndroidFrameHost:
// same renderer::FrameHost contract, but the display handle is read from
// SDL's generic X11/Wayland window properties instead of Android's opaque
// window pointer, and font_dirs() points at the usual Linux font locations.
class LinuxNativeFrameHost final : public renderer::FrameHost {
public:
    explicit LinuxNativeFrameHost(SDL_Window *window = nullptr, SDL_GLContext *gl_context = nullptr)
        : m_window(window)
        , m_gl_context(gl_context) {
    }

    renderer::DisplayHandle handle() const override {
        if (!m_window)
            return {};

        const SDL_PropertiesID props = SDL_GetWindowProperties(m_window);

        if (void *wl_display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr)) {
            if (void *wl_surface = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr))
                return renderer::WaylandDisplayHandle{ wl_display, wl_surface };
        }

        if (void *x11_display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr)) {
            const auto x11_window = static_cast<std::uintptr_t>(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
            return renderer::X11DisplayHandle{
                .display = x11_display,
                .window = x11_window,
                .connection = nullptr,
            };
        }

        LOG_ERROR("Could not determine window system - neither Wayland nor X11 window properties are set");
        return {};
    }

    int drawable_width() const override {
        int width = 960;
        int height = 544;
        SDL_GetWindowSizeInPixels(m_window, &width, &height);
        return width;
    }

    int drawable_height() const override {
        int width = 960;
        int height = 544;
        SDL_GetWindowSizeInPixels(m_window, &width, &height);
        return height;
    }

    std::vector<std::string> font_dirs() const override {
        return { "/usr/share/fonts/", "/usr/local/share/fonts/" };
    }

    void *get_proc_address(const char *name) const override {
        return reinterpret_cast<void *>(SDL_GL_GetProcAddress(name));
    }

    unsigned int default_fbo() const override {
        return 0;
    }

    bool make_current() override {
        if (!m_gl_context || !*m_gl_context)
            return false;
        return SDL_GL_MakeCurrent(m_window, *m_gl_context);
    }

    void done_current() override {
        SDL_GL_MakeCurrent(m_window, nullptr);
    }

    void swap_buffers() override {
        SDL_GL_SwapWindow(m_window);
    }

    bool set_vsync(bool enabled) override {
        return SDL_GL_SetSwapInterval(enabled ? 1 : 0);
    }

    void prepare_for_render_thread() override {
        if (m_gl_context && *m_gl_context)
            SDL_GL_MakeCurrent(m_window, nullptr);
    }

    void destroy_render_context() override {
        if (!m_gl_context || !*m_gl_context)
            return;

        SDL_GL_DestroyContext(*m_gl_context);
        *m_gl_context = nullptr;
    }

private:
    SDL_Window *m_window = nullptr;
    SDL_GLContext *m_gl_context = nullptr;
};

// IME keyboard input plumbing - identical to android/jni/main_android.cpp's
// handlers, minus nothing: the IME/overlay system itself is already Qt-free.
bool handle_ime_keydown(EmuEnvState &emuenv, const SDL_KeyboardEvent &event) {
    if (!is_any_ime_active(emuenv))
        return false;

    auto &ime = emuenv.ime;
    const bool dialog_ime_active = is_ime_dialog_active(emuenv);

    switch (event.key) {
    case SDLK_BACKSPACE: {
        {
            std::lock_guard<std::mutex> lock(ime.mutex);
            ime_backspace(ime);
        }
        return true;
    }

    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        if (dialog_ime_active) {
            finish_ime_dialog(emuenv);
        } else {
            std::lock_guard<std::mutex> lock(ime.mutex);
            ime.push_event(SCE_IME_EVENT_PRESS_ENTER);
        }
        return true;

    case SDLK_ESCAPE:
        if (dialog_ime_active) {
            cancel_ime_dialog(emuenv);
        } else {
            std::lock_guard<std::mutex> lock(ime.mutex);
            ime.push_event(SCE_IME_EVENT_PRESS_CLOSE);
        }
        return true;

    case SDLK_LEFT: {
        {
            std::lock_guard<std::mutex> lock(ime.mutex);
            ime_cursor_left(ime);
        }
        return true;
    }

    case SDLK_RIGHT: {
        {
            std::lock_guard<std::mutex> lock(ime.mutex);
            ime_cursor_right(ime);
        }
        return true;
    }

    default:
        return false;
    }
}

void handle_ime_text_editing(EmuEnvState &emuenv, const char *text) {
    if (!is_any_ime_active(emuenv))
        return;

    {
        std::lock_guard<std::mutex> lock(emuenv.ime.mutex);
        ime_set_preedit(emuenv.ime, string_utils::utf8_to_utf16(text ? text : ""));
    }
}

void handle_ime_text_input(EmuEnvState &emuenv, const char *text) {
    if (!is_any_ime_active(emuenv) || !text || text[0] == '\0')
        return;

    std::string filtered_text;
    filtered_text.reserve(std::strlen(text));
    for (const char *ch = text; *ch != '\0'; ++ch) {
        if (*ch != '\n' && *ch != '\r')
            filtered_text.push_back(*ch);
    }

    if (filtered_text.empty())
        return;

    {
        std::lock_guard<std::mutex> lock(emuenv.ime.mutex);
        ime_commit_text(emuenv.ime, string_utils::utf8_to_utf16(filtered_text));
    }
}

} // namespace

int main(int argc, char *argv[]) {
    Root root_paths;
    app::init_paths(root_paths);

    if (!fs::exists(root_paths.get_vita_fs_path()))
        fs::create_directories(root_paths.get_vita_fs_path());

    if (logging::init(root_paths, true) != Success)
        return InitConfigFailed;

    LOG_INFO("{}", window_title);
    LOG_INFO("Steam Deck / Steam Machine native build (no Qt)");

    Config cfg{};
    EmuEnvState emuenv;
    const auto config_err = config::init_config(cfg, argc, argv, root_paths);
    fs::create_directories(cfg.get_vita_fs_path());

    if (config_err != Success) {
        // A CLI-only action (pkg/pup install, delete-title, etc.) was already
        // handled inside init_config for QuitRequested; anything else is a
        // genuine config failure.
        return config_err == QuitRequested ? Success : InitConfigFailed;
    }

    if (!cfg.run_app_path.has_value()) {
        LOG_ERROR("No game to launch - pass --installed-path/-r <title id> "
                  "(this is what a Steam shortcut's Launch Options field should call this binary with).");
        return InitConfigFailed;
    }

    if (!app::init(emuenv, cfg, root_paths)) {
        LOG_ERROR("Emulated environment initialization failed.");
        return 1;
    }

    emuenv.vulkan_device_info = std::make_unique<renderer::VulkanDeviceInfo>(renderer::enumerate_vulkan_devices());

    if (emuenv.cfg.controller_binds.empty() || emuenv.cfg.controller_binds.size() != 15
        || emuenv.cfg.controller_axis_binds.empty() || emuenv.cfg.controller_axis_binds.size() != 6)
        app::reset_controller_binding(emuenv);

    init_libraries(emuenv);

    if (!app::init_apps_list(emuenv))
        LOG_ERROR("Failed to initialize apps list.");

    app::load_users(emuenv);
    if (!app::ensure_current_user(emuenv)) {
        LOG_ERROR("Failed to initialize active user.");
        return 1;
    }
    compat::load_from_disk(emuenv.compat, std::filesystem::path(emuenv.cache_path.string()));

    AppLaunchRequest launch_request{ .app_path = *emuenv.cfg.run_app_path };
    emuenv.cfg.run_app_path.reset();

    app::AppSessionController session(emuenv);

    int exit_code = 0;
    bool relaunch_requested = false;

    do {
        relaunch_requested = false;

        SDL_Window *window = nullptr;
        SDL_GLContext gl_context = nullptr;
        LinuxNativeFrameHost frame_host;
        std::optional<AppLaunchRequest> pending_launch_request;

        const auto cleanup_launch = [&](const app::AppSessionStopReason reason) {
            session.stop(reason);
            if (window) {
                SDL_DestroyWindow(window);
                window = nullptr;
            }
            SDL_Quit();
        };

        LOG_INFO("Booting game '{}'", launch_request.app_path);

        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC | SDL_INIT_SENSOR)) {
            LOG_ERROR("SDL_Init failed: {}", SDL_GetError());
            exit_code = SDLInitFailed;
            session.stop(app::AppSessionStopReason::LaunchFailure);
            break;
        }

        SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS, "1");

        refresh_controllers(emuenv.ctrl, emuenv);

        if (!session.begin_launch(launch_request, launch_request.reason != AppLaunchReason::LoadExec)) {
            LOG_ERROR("Could not find app '{}' in apps list.", launch_request.app_path);
            exit_code = 1;
            cleanup_launch(app::AppSessionStopReason::LaunchFailure);
            break;
        }

        SDL_WindowFlags window_flags = SDL_WINDOW_RESIZABLE;
        if (emuenv.backend_renderer == renderer::Backend::OpenGL) {
            window_flags |= SDL_WINDOW_OPENGL;
            if (!SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE)
                || !SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4)
                || !SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 4)
                || !SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1)
                || !SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0)) {
                LOG_ERROR("Failed to configure OpenGL context attributes: {}", SDL_GetError());
                exit_code = 1;
                cleanup_launch(app::AppSessionStopReason::LaunchFailure);
                break;
            }
        } else if (emuenv.backend_renderer == renderer::Backend::Vulkan) {
            window_flags |= SDL_WINDOW_VULKAN;
        }

        window = SDL_CreateWindow(emuenv.current_app_title.c_str(), 960, 544, window_flags);
        if (!window) {
            LOG_ERROR("SDL_CreateWindow failed: {}", SDL_GetError());
            exit_code = 1;
            cleanup_launch(app::AppSessionStopReason::LaunchFailure);
            break;
        }
        frame_host = LinuxNativeFrameHost(window, &gl_context);

        if (emuenv.backend_renderer == renderer::Backend::OpenGL) {
            gl_context = SDL_GL_CreateContext(window);
            if (!gl_context) {
                LOG_ERROR("Failed to create OpenGL context: {}", SDL_GetError());
                exit_code = 1;
                cleanup_launch(app::AppSessionStopReason::LaunchFailure);
                break;
            }

            if (!SDL_GL_MakeCurrent(window, gl_context)) {
                LOG_ERROR("Failed to make OpenGL context current: {}", SDL_GetError());
                exit_code = 1;
                cleanup_launch(app::AppSessionStopReason::LaunchFailure);
                break;
            }

            if (!SDL_GL_SetSwapInterval(static_cast<int>(emuenv.cfg.current_config.v_sync)))
                LOG_WARN("Failed to set OpenGL swap interval: {}", SDL_GetError());
        }

        if (!session.initialize_renderer(frame_host)) {
            LOG_ERROR("Failed to initialise renderer.");
            exit_code = 1;
            cleanup_launch(app::AppSessionStopReason::LaunchFailure);
            break;
        }

        if (!session.initialize_runtime()) {
            LOG_ERROR("Failed late initialisation.");
            exit_code = 1;
            cleanup_launch(app::AppSessionStopReason::LaunchFailure);
            break;
        }

        if (!session.load_and_run()) {
            LOG_ERROR("Failed to load or start the app session.");
            exit_code = 1;
            cleanup_launch(app::AppSessionStopReason::LaunchFailure);
            break;
        }

        if (auto request = emuenv.take_app_launch_request())
            pending_launch_request = std::move(request);

        LOG_INFO("Game started: {} ({})", emuenv.current_app_title, launch_request.app_path);
        app::LaunchRuntimeMetrics runtime_metrics{};

        bool running = !pending_launch_request.has_value();
        bool text_input_active = false;
        while (running) {
            // SDL only delivers TEXT_EDITING/TEXT_INPUT while text input is explicitly started;
            // Android's soft-keyboard show/hide (set_keyboard_active) does the equivalent implicitly.
            const bool want_text_input = is_any_ime_active(emuenv);
            if (want_text_input != text_input_active) {
                if (want_text_input)
                    SDL_StartTextInput(window);
                else
                    SDL_StopTextInput(window);
                text_input_active = want_text_input;
            }

            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                switch (event.type) {
                case SDL_EVENT_QUIT:
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    running = false;
                    break;

                case SDL_EVENT_KEY_DOWN:
                    handle_ime_keydown(emuenv, event.key);
                    break;

                case SDL_EVENT_TEXT_EDITING:
                    handle_ime_text_editing(emuenv, event.edit.text);
                    break;

                case SDL_EVENT_TEXT_INPUT:
                    handle_ime_text_input(emuenv, event.text.text);
                    break;

                case SDL_EVENT_FINGER_DOWN:
                case SDL_EVENT_FINGER_MOTION:
                case SDL_EVENT_FINGER_UP:
                    // Steam Deck's built-in touchscreen maps naturally onto the Vita's front touch surface.
                    handle_touch_event(emuenv.touch, event.tfinger);
                    break;

                case SDL_EVENT_GAMEPAD_ADDED:
                case SDL_EVENT_GAMEPAD_REMOVED:
                    refresh_controllers(emuenv.ctrl, emuenv);
                    break;

                case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
                case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
                case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
                    handle_touchpad_event(emuenv.touch, event.gtouchpad);
                    break;

                case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
                    handle_motion_event(emuenv, event.gsensor.sensor, event.gsensor);
                    break;
                case SDL_EVENT_SENSOR_UPDATE:
                    handle_motion_event(emuenv, SDL_GetSensorTypeForID(event.sensor.which), event.sensor);
                    break;

                default:
                    break;
                }
            }

            if (!pending_launch_request) {
                if (auto request = emuenv.take_app_launch_request()) {
                    pending_launch_request = std::move(request);
                    running = false;
                }
            }

            app::update_runtime_metrics(emuenv, runtime_metrics);

            if (!session.is_running())
                running = false;

            if (running)
                SDL_Delay(16);
        }

        LOG_INFO("Shutting down game");

        if (pending_launch_request) {
            launch_request = std::move(*pending_launch_request);
            relaunch_requested = true;
            LOG_INFO("Relaunching in-process with self '{}'", launch_request.self_path);
        }

        cleanup_launch(relaunch_requested
                ? app::AppSessionStopReason::Relaunch
                : app::AppSessionStopReason::UserRequest);
    } while (relaunch_requested);

    return exit_code;
}
