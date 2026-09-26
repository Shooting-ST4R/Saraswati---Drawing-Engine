// Saraswati — entry point: command line, SDL window, Vulkan, Dear ImGui.
#include "app.h"

#include <SDL3/SDL_main.h>
#include <SDL3/SDL_vulkan.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

static const char* kUsage =
    "Saraswati " SARASWATI_VERSION "\n"
    "  --cpu-vulkan              use a CPU Vulkan driver (SwiftShader from Chrome/Edge on Windows, lavapipe on Linux)\n"
    "  --vulkan-driver <path>    load a specific Vulkan driver / loader library\n"
    "  --gpu <index>             pick a physical device by index\n"
    "  --validation              enable VK_LAYER_KHRONOS_validation if installed\n"
    "  --doc <W>x<H>             document size (default 3000x2000; benchmark 8000x8000)\n"
    "  --brush <px>              brush size\n"
    "  --tip hard|textured|soft  brush tip\n"
    "  --layers <N>              benchmark: number of layers\n"
    "  --benchmark               play a synthetic 4 s zig-zag stroke and write a report to User-Data/logs\n"
    "  --demo                    scripted visual test (tips, layers, blend modes, undo/redo, view)\n"
    "  --open <file>             open a document or image at startup\n"
    "  --screenshot <file.png>   save the window after the benchmark/demo, or after --frames N frames\n"
    "  --frames <N>              frames before the screenshot (default 5)\n"
    "  --window <W>x<H>          initial window size\n"
    "  --exit                    quit when the benchmark / demo / screenshot is done\n";

static bool parseSize(const char* s, uint32_t& w, uint32_t& h) {
  unsigned a = 0, b = 0;
  if (sscanf(s, "%ux%u", &a, &b) != 2 && sscanf(s, "%uX%u", &a, &b) != 2) return false;
  w = a;
  h = b;
  return a > 0 && b > 0;
}

int main(int argc, char** argv) {
#ifdef _WIN32
  // GUI-subsystem exe: print to the console we were started from, if any (benchmark reports).
  if (AttachConsole(ATTACH_PARENT_PROCESS)) {
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
  }
#endif
  Options opt;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", a.c_str()); exit(2); }
      return argv[++i];
    };
    if (a == "--cpu-vulkan") opt.renderer.cpuVulkan = true;
    else if (a == "--vulkan-driver") opt.renderer.vulkanDriver = next();
    else if (a == "--gpu") opt.renderer.gpuIndex = atoi(next());
    else if (a == "--validation") opt.renderer.validation = true;
    else if (a == "--doc") { if (!parseSize(next(), opt.docW, opt.docH)) { fprintf(stderr, "bad --doc\n"); return 2; } }
    else if (a == "--brush") opt.brushPx = float(atof(next()));
    else if (a == "--tip") {
      std::string t = next();
      opt.tipSet = true;
      opt.tip = t == "textured" ? Tip::Textured : t == "soft" ? Tip::Soft : Tip::Hard;
    }
    else if (a == "--layers") opt.layers = atoi(next());
    else if (a == "--benchmark") opt.benchmark = true;
    else if (a == "--demo") opt.demo = true;
    else if (a == "--open") opt.open = next();
    else if (a == "--screenshot") opt.screenshot = next();
    else if (a == "--frames") opt.frames = atoi(next());
    else if (a == "--window") {
      uint32_t w, h;
      if (parseSize(next(), w, h)) { opt.windowW = int(w); opt.windowH = int(h); }
    }
    else if (a == "--exit") opt.exitAfter = true;
    else if (a == "--help" || a == "-h") { printf("%s", kUsage); return 0; }
    else { fprintf(stderr, "unknown option %s\n%s", a.c_str(), kUsage); return 2; }
  }

  SDL_SetHint(SDL_HINT_PEN_MOUSE_EVENTS, "1");
  SDL_SetHint(SDL_HINT_PEN_TOUCH_EVENTS, "0");
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  std::string err;
  if (!Renderer::loadVulkan(opt.renderer, err)) {
    fprintf(stderr, "%s\n", err.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Saraswati", err.c_str(), nullptr);
    SDL_Quit();
    return 1;
  }
  SDL_Window* window = SDL_CreateWindow("Saraswati " SARASWATI_VERSION, opt.windowW, opt.windowH,
                                        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window) {
    fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }
  int rc = 0;
  Renderer renderer;
  try {
    renderer.init(window, opt.renderer);
    SDL_Log("Vulkan device: %s (%s)%s", renderer.deviceName.c_str(), renderer.driverInfo.c_str(),
            renderer.cpuEmulation ? " [CPU emulation]" : "");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    float scale = SDL_GetWindowDisplayScale(window);
    if (scale > 0 && scale != 1.0f) {
      ImGui::GetStyle().ScaleAllSizes(scale / std::max(1.0f, SDL_GetWindowPixelDensity(window)));
      ImGui::GetStyle().FontScaleDpi = scale / std::max(1.0f, SDL_GetWindowPixelDensity(window));
    }
    renderer.initImGui();
    {
      App app(window, renderer, opt);
      rc = app.run();
    }
    renderer.waitIdle();
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    renderer.shutdown();
  } catch (const std::exception& ex) {
    fprintf(stderr, "fatal: %s\n", ex.what());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Saraswati", ex.what(), window);
    rc = 1;
  }
  SDL_DestroyWindow(window);
  SDL_Quit();
  return rc;
}
