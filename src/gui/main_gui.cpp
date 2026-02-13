// ProjectUltra GUI - Cross-platform modem interface
// Uses Dear ImGui with SDL2 + OpenGL 2.1 for maximum compatibility

#include "app.hpp"
#include "startup_trace.hpp"

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include <SDL.h>
#include <SDL_opengl.h>

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <chrono>
#include <ctime>
#include <exception>
#include <vector>
#include <ultra/logging.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

FILE* g_startup_log_file = nullptr;
std::string g_startup_log_path;

void writeStartupLog(const char* fmt, ...) {
    if (!g_startup_log_file) return;

    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_now{};
#ifdef _WIN32
    localtime_s(&tm_now, &t);
#else
    localtime_r(&t, &tm_now);
#endif

    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now);
    std::fprintf(g_startup_log_file, "[%s] ", ts);

    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_startup_log_file, fmt, args);
    va_end(args);
    std::fprintf(g_startup_log_file, "\n");
    std::fflush(g_startup_log_file);
}

void initStartupLog() {
    namespace fs = std::filesystem;
    std::vector<fs::path> candidates;
    candidates.emplace_back(fs::path("logs") / "startup.log");
    candidates.emplace_back("startup.log");

#ifdef _WIN32
    if (const char* temp = std::getenv("TEMP")) {
        candidates.emplace_back(fs::path(temp) / "ProjectUltra" / "startup.log");
    }
#else
    if (const char* temp = std::getenv("TMPDIR")) {
        candidates.emplace_back(fs::path(temp) / "projectultra_startup.log");
    }
    candidates.emplace_back("/tmp/projectultra_startup.log");
#endif

    for (const auto& path : candidates) {
        std::error_code ec;
        if (!path.parent_path().empty()) {
            fs::create_directories(path.parent_path(), ec);
        }
        g_startup_log_file = std::fopen(path.string().c_str(), "w");
        if (g_startup_log_file) {
            g_startup_log_path = path.string();
            break;
        }
    }

    if (g_startup_log_file) {
        writeStartupLog("ProjectUltra GUI startup log initialized");
    }
}

void closeStartupLog() {
    if (g_startup_log_file) {
        writeStartupLog("ProjectUltra GUI startup log closing");
        std::fclose(g_startup_log_file);
        g_startup_log_file = nullptr;
    }
}

#ifdef _WIN32
void showFatalStartupMessage(const std::string& msg) {
    MessageBoxA(nullptr, msg.c_str(), "ProjectUltra Startup Error", MB_ICONERROR | MB_OK);
}

std::string modulePathForAddress(void* addr) {
    if (!addr) {
        return "<unknown module>";
    }
    HMODULE mod = nullptr;
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(addr),
            &mod)) {
        return "<unknown module>";
    }

    char path[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(mod, path, static_cast<DWORD>(sizeof(path)));
    if (len == 0 || len >= sizeof(path)) {
        return "<unknown module>";
    }
    return std::string(path, len);
}

LONG WINAPI startupUnhandledExceptionFilter(EXCEPTION_POINTERS* ex) {
    unsigned long code = 0;
    void* addr = nullptr;
    if (ex && ex->ExceptionRecord) {
        code = ex->ExceptionRecord->ExceptionCode;
        addr = ex->ExceptionRecord->ExceptionAddress;
    }
    std::string module = modulePathForAddress(addr);
    writeStartupLog("Unhandled exception: code=0x%08lX address=%p module=%s", code, addr, module.c_str());

    std::string msg = "Unhandled exception in startup path.\n";
    char details[512];
    std::snprintf(details, sizeof(details), "code=0x%08lX address=%p\nmodule=%s", code, addr, module.c_str());
    msg += details;
    if (!g_startup_log_path.empty()) {
        msg += "\n\nStartup log: " + g_startup_log_path;
    }
    showFatalStartupMessage(msg);
    closeStartupLog();
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

}  // namespace

int main(int argc, char* argv[]) {
    initStartupLog();
#ifdef _WIN32
    if (!g_startup_log_path.empty()) {
        _putenv_s("ULTRA_STARTUP_LOG", g_startup_log_path.c_str());
    }
    SetUnhandledExceptionFilter(startupUnhandledExceptionFilter);
#endif
    std::set_terminate([]() {
        writeStartupLog("std::terminate invoked");
#ifdef _WIN32
        std::string msg = "Fatal terminate() during startup/runtime.";
        if (!g_startup_log_path.empty()) {
            msg += "\n\nStartup log: " + g_startup_log_path;
        }
        showFatalStartupMessage(msg);
#endif
        closeStartupLog();
        std::_Exit(3);
    });

    // Set log level to INFO to avoid DEBUG log spam slowing down UI
    // (DEBUG logs every frame in pollRxAudio() cause significant lag)
    ultra::setLogLevel(ultra::LogLevel::INFO);
    writeStartupLog("Log level set to INFO");

    // Parse command line arguments
    ultra::gui::App::Options opts;
    bool force_software_renderer = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-sim") {
            opts.enable_sim = true;
        } else if (arg == "-rec") {
            opts.record_audio = true;
            // Check if next arg is a path (doesn't start with -)
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                opts.record_path = argv[++i];
            }
        } else if (arg == "--software" || arg == "-sw") {
            force_software_renderer = true;
            opts.safe_startup = true;
            opts.disable_waterfall = true;
        } else if (arg == "--no-waterfall") {
            opts.disable_waterfall = true;
        } else if (arg == "--waterfall") {
            opts.disable_waterfall = false;
        }
    }
    writeStartupLog(
        "Parsed arguments: sim=%d, rec=%d, software_renderer=%d, disable_waterfall=%d",
        opts.enable_sim ? 1 : 0,
        opts.record_audio ? 1 : 0,
        force_software_renderer ? 1 : 0,
        opts.disable_waterfall ? 1 : 0
    );

    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
        const char* sdl_err = SDL_GetError();
        std::string msg = std::string("SDL_Init failed: ") + (sdl_err ? sdl_err : "<unknown>");
        std::fprintf(stderr, "Error: %s\n", msg.c_str());
        writeStartupLog("%s", msg.c_str());
#ifdef _WIN32
        if (!g_startup_log_path.empty()) {
            showFatalStartupMessage(msg + "\n\nStartup log: " + g_startup_log_path);
        } else {
            showFatalStartupMessage(msg);
        }
#endif
        closeStartupLog();
        return 1;
    }
    writeStartupLog("SDL initialized");

    SDL_Window* window = nullptr;
    SDL_GLContext gl_context = nullptr;
    SDL_Renderer* sdl_renderer = nullptr;
    bool using_software_renderer = force_software_renderer;

    auto failStartup = [&](const std::string& msg) -> int {
        std::fprintf(stderr, "Error: %s\n", msg.c_str());
        writeStartupLog("%s", msg.c_str());
#ifdef _WIN32
        if (!g_startup_log_path.empty()) {
            showFatalStartupMessage(msg + "\n\nStartup log: " + g_startup_log_path);
        } else {
            showFatalStartupMessage(msg);
        }
#endif
        if (sdl_renderer) {
            SDL_DestroyRenderer(sdl_renderer);
            sdl_renderer = nullptr;
        }
        if (gl_context) {
            SDL_GL_DeleteContext(gl_context);
            gl_context = nullptr;
        }
        if (window) {
            SDL_DestroyWindow(window);
            window = nullptr;
        }
        SDL_Quit();
        closeStartupLog();
        return 1;
    };

    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!using_software_renderer) {
        // Setup OpenGL 2.1 context (works on old hardware if driver is stable)
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        window_flags = (SDL_WindowFlags)(window_flags | SDL_WINDOW_OPENGL);
        writeStartupLog("OpenGL renderer path selected");
    } else {
        writeStartupLog("Software renderer path selected (--software)");
    }

    window = SDL_CreateWindow(
        "ProjectUltra - High-Speed HF Modem",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        1024, 600,
        window_flags
    );

    if (!window) {
        const char* sdl_err = SDL_GetError();
        std::string msg = std::string("SDL_CreateWindow failed: ") + (sdl_err ? sdl_err : "<unknown>");
        return failStartup(msg);
    }
    writeStartupLog("Window created");

    if (using_software_renderer) {
        writeStartupLog("Creating SDL renderer (accelerated + vsync)");
        sdl_renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!sdl_renderer) {
            writeStartupLog("Accelerated SDL renderer unavailable: %s", SDL_GetError());
            writeStartupLog("Falling back to SDL software renderer");
            sdl_renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
        }
        if (!sdl_renderer) {
            const char* sdl_err = SDL_GetError();
            std::string msg = std::string("SDL_CreateRenderer failed: ") + (sdl_err ? sdl_err : "<unknown>");
            return failStartup(msg);
        }

        SDL_RendererInfo renderer_info{};
        if (SDL_GetRendererInfo(sdl_renderer, &renderer_info) == 0) {
            writeStartupLog(
                "SDL renderer ready: name=%s flags=0x%x",
                renderer_info.name ? renderer_info.name : "<unknown>",
                renderer_info.flags
            );
        } else {
            writeStartupLog("SDL renderer ready (SDL_GetRendererInfo failed: %s)", SDL_GetError());
        }
    } else {
        gl_context = SDL_GL_CreateContext(window);
        if (!gl_context) {
            const char* sdl_err = SDL_GetError();
            std::string msg = std::string("SDL_GL_CreateContext failed: ") + (sdl_err ? sdl_err : "<unknown>");
            return failStartup(msg);
        }
        writeStartupLog("OpenGL context created");

        writeStartupLog("Calling SDL_GL_MakeCurrent");
        if (SDL_GL_MakeCurrent(window, gl_context) != 0) {
            const char* sdl_err = SDL_GetError();
            std::string msg = std::string("SDL_GL_MakeCurrent failed: ") + (sdl_err ? sdl_err : "<unknown>");
            return failStartup(msg);
        }
        writeStartupLog("SDL_GL_MakeCurrent succeeded");

        writeStartupLog("Calling SDL_GL_SetSwapInterval(1)");
        if (SDL_GL_SetSwapInterval(1) != 0) {
            // Non-fatal on some drivers; keep running but record detail.
            writeStartupLog("SDL_GL_SetSwapInterval failed/non-vsync: %s", SDL_GetError());
        } else {
            writeStartupLog("SDL_GL_SetSwapInterval succeeded");
        }
    }

    // Setup Dear ImGui context
    writeStartupLog("Calling IMGUI_CHECKVERSION");
    IMGUI_CHECKVERSION();
    writeStartupLog("IMGUI_CHECKVERSION passed");
    writeStartupLog("Calling ImGui::CreateContext");
    ImGui::CreateContext();
    writeStartupLog("ImGui::CreateContext succeeded");
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    writeStartupLog("ImGui IO initialized");

    // Setup style - dark theme
    writeStartupLog("Applying ImGui style");
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 2.0f;
    style.GrabRounding = 2.0f;
    writeStartupLog("ImGui style applied");

    // Setup Platform/Renderer backends
    if (using_software_renderer) {
        writeStartupLog("Calling ImGui_ImplSDL2_InitForSDLRenderer");
        if (!ImGui_ImplSDL2_InitForSDLRenderer(window, sdl_renderer)) {
            return failStartup("ImGui_ImplSDL2_InitForSDLRenderer failed");
        }
        writeStartupLog("ImGui_ImplSDL2_InitForSDLRenderer succeeded");

        writeStartupLog("Calling ImGui_ImplSDLRenderer2_Init");
        if (!ImGui_ImplSDLRenderer2_Init(sdl_renderer)) {
            return failStartup("ImGui_ImplSDLRenderer2_Init failed");
        }
        writeStartupLog("ImGui_ImplSDLRenderer2_Init succeeded");
    } else {
        writeStartupLog("Calling ImGui_ImplSDL2_InitForOpenGL");
        if (!ImGui_ImplSDL2_InitForOpenGL(window, gl_context)) {
            return failStartup("ImGui_ImplSDL2_InitForOpenGL failed");
        }
        writeStartupLog("ImGui_ImplSDL2_InitForOpenGL succeeded");

        writeStartupLog("Calling ImGui_ImplOpenGL2_Init");
        if (!ImGui_ImplOpenGL2_Init()) {
            return failStartup("ImGui_ImplOpenGL2_Init failed");
        }
        writeStartupLog("ImGui_ImplOpenGL2_Init succeeded");
    }

    // Create application with parsed options
    writeStartupLog("Constructing App");
    ultra::gui::startupTrace("main_gui", "before-app-construction");
    ultra::gui::App app(opts);
    writeStartupLog("App initialized");

    // Main loop
    bool running = true;
    while (running) {
        // Poll events
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);

            if (event.type == SDL_QUIT) {
                running = false;
            }
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_CLOSE &&
                event.window.windowID == SDL_GetWindowID(window)) {
                running = false;
            }
        }

        // Start ImGui frame
        if (using_software_renderer) {
            ImGui_ImplSDLRenderer2_NewFrame();
            ImGui_ImplSDL2_NewFrame();
        } else {
            ImGui_ImplOpenGL2_NewFrame();
            ImGui_ImplSDL2_NewFrame();
        }
        ImGui::NewFrame();

        // Render application UI
        app.render();

        // Rendering
        ImGui::Render();
        if (using_software_renderer) {
            SDL_SetRenderDrawColor(sdl_renderer, 26, 26, 31, 255);
            SDL_RenderClear(sdl_renderer);
            ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), sdl_renderer);
            SDL_RenderPresent(sdl_renderer);
        } else {
            glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
            glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
            SDL_GL_SwapWindow(window);
        }
    }

    // Cleanup
    if (using_software_renderer) {
        ImGui_ImplSDLRenderer2_Shutdown();
    } else {
        ImGui_ImplOpenGL2_Shutdown();
    }
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    if (sdl_renderer) {
        SDL_DestroyRenderer(sdl_renderer);
        sdl_renderer = nullptr;
    }
    if (gl_context) {
        SDL_GL_DeleteContext(gl_context);
        gl_context = nullptr;
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    closeStartupLog();

    return 0;
}
