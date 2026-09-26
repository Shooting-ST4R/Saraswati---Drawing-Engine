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

#include "font_ui.h"

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
    "  --tooltest                scripted visual test of the tools (selections, fill, gradient, shapes, transform)\n"
    "  --open <file>             open a document or image at startup\n"
    "  --import <file>           import an image/PSD as a new layer at startup (repeatable)\n"
    "  --save <file.psd>         save as PSD after the demo / startup (test helper)\n"
    "  --screenshot <file.png>   save the window after the benchmark/demo, or after --frames N frames\n"
    "  --frames <N>              frames before the screenshot (default 5)\n"
    "  --window <W>x<H>          initial window size\n"
    "  --exit                    quit when the benchmark / demo / screenshot is done\n";

// Neutral grey theme: every UI colour has R = G = B so the interface does not shift the
// painter's colour perception on the canvas. Only the colour picker shows colour.
static void setupStyle(float scale) {
  ImGuiStyle& st = ImGui::GetStyle();
  st.WindowRounding = 5;
  st.ChildRounding = 4;
  st.FrameRounding = 4;
  st.PopupRounding = 5;
  st.GrabRounding = 4;
  st.TabRounding = 4;
  st.ScrollbarRounding = 6;
  st.WindowBorderSize = 1;
  st.FrameBorderSize = 0;
  st.TabBorderSize = 0;
  st.WindowPadding = ImVec2(8, 8);
  st.FramePadding = ImVec2(7, 4);
  st.ItemSpacing = ImVec2(7, 5);
  st.ItemInnerSpacing = ImVec2(5, 4);
  st.ScrollbarSize = 12;
  st.GrabMinSize = 10;
  st.WindowTitleAlign = ImVec2(0.0f, 0.5f);
  st.DockingSeparatorSize = 3;
  auto g = [](float v, float a = 1.0f) { return ImVec4(v, v, v, a); };
  ImVec4* c = st.Colors;
  c[ImGuiCol_Text] = g(0.90f);
  c[ImGuiCol_TextDisabled] = g(0.50f);
  c[ImGuiCol_WindowBg] = g(0.155f);
  c[ImGuiCol_ChildBg] = g(0.155f);
  c[ImGuiCol_PopupBg] = g(0.13f, 0.98f);
  c[ImGuiCol_Border] = g(0.24f);
  c[ImGuiCol_BorderShadow] = g(0.0f, 0.0f);
  c[ImGuiCol_FrameBg] = g(0.225f);
  c[ImGuiCol_FrameBgHovered] = g(0.285f);
  c[ImGuiCol_FrameBgActive] = g(0.33f);
  c[ImGuiCol_TitleBg] = g(0.12f);
  c[ImGuiCol_TitleBgActive] = g(0.14f);
  c[ImGuiCol_TitleBgCollapsed] = g(0.12f);
  c[ImGuiCol_MenuBarBg] = g(0.12f);
  c[ImGuiCol_ScrollbarBg] = g(0.13f);
  c[ImGuiCol_ScrollbarGrab] = g(0.32f);
  c[ImGuiCol_ScrollbarGrabHovered] = g(0.40f);
  c[ImGuiCol_ScrollbarGrabActive] = g(0.48f);
  c[ImGuiCol_CheckMark] = g(0.92f);
  c[ImGuiCol_SliderGrab] = g(0.60f);
  c[ImGuiCol_SliderGrabActive] = g(0.78f);
  c[ImGuiCol_Button] = g(0.25f);
  c[ImGuiCol_ButtonHovered] = g(0.32f);
  c[ImGuiCol_ButtonActive] = g(0.40f);
  c[ImGuiCol_Header] = g(0.27f);
  c[ImGuiCol_HeaderHovered] = g(0.32f);
  c[ImGuiCol_HeaderActive] = g(0.38f);
  c[ImGuiCol_Separator] = g(0.24f);
  c[ImGuiCol_SeparatorHovered] = g(0.45f);
  c[ImGuiCol_SeparatorActive] = g(0.60f);
  c[ImGuiCol_ResizeGrip] = g(0.30f, 0.5f);
  c[ImGuiCol_ResizeGripHovered] = g(0.45f);
  c[ImGuiCol_ResizeGripActive] = g(0.60f);
  c[ImGuiCol_InputTextCursor] = g(0.95f);
  c[ImGuiCol_Tab] = g(0.14f);
  c[ImGuiCol_TabHovered] = g(0.30f);
  c[ImGuiCol_TabSelected] = g(0.22f);
  c[ImGuiCol_TabSelectedOverline] = g(0.70f);
  c[ImGuiCol_TabDimmed] = g(0.13f);
  c[ImGuiCol_TabDimmedSelected] = g(0.19f);
  c[ImGuiCol_TabDimmedSelectedOverline] = g(0.40f);
  c[ImGuiCol_DockingPreview] = g(0.75f, 0.35f);
  c[ImGuiCol_DockingEmptyBg] = g(0.20f, 0.0f);
  c[ImGuiCol_PlotLines] = g(0.70f);
  c[ImGuiCol_PlotLinesHovered] = g(0.90f);
  c[ImGuiCol_PlotHistogram] = g(0.70f);
  c[ImGuiCol_PlotHistogramHovered] = g(0.90f);
  c[ImGuiCol_TableHeaderBg] = g(0.19f);
  c[ImGuiCol_TableBorderStrong] = g(0.26f);
  c[ImGuiCol_TableBorderLight] = g(0.21f);
  c[ImGuiCol_TableRowBg] = g(0.0f, 0.0f);
  c[ImGuiCol_TableRowBgAlt] = g(1.0f, 0.03f);
  c[ImGuiCol_TextSelectedBg] = g(0.45f, 0.45f);
  c[ImGuiCol_DragDropTarget] = g(0.85f);
  c[ImGuiCol_NavCursor] = g(0.80f);
  c[ImGuiCol_NavWindowingHighlight] = g(1.0f, 0.7f);
  c[ImGuiCol_NavWindowingDimBg] = g(0.1f, 0.4f);
  c[ImGuiCol_ModalWindowDimBg] = g(0.05f, 0.5f);
  st.ScaleAllSizes(scale);
  ImFontConfig fc;
  fc.FontDataOwnedByAtlas = false;
  ImGui::GetIO().Fonts->AddFontFromMemoryTTF((void*)kFontUi, int(sizeof kFontUi), 15.0f, &fc);
  st.FontScaleDpi = scale;
}

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
    else if (a == "--tooltest") opt.toolTest = true;
    else if (a == "--open") opt.open = next();
    else if (a == "--save") opt.save = next();
    else if (a == "--import") opt.imports.push_back(next());
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
    float scale = SDL_GetWindowDisplayScale(window) / std::max(1.0f, SDL_GetWindowPixelDensity(window));
    setupStyle(scale > 0 ? scale : 1.0f);
    renderer.initImGui();
    {
      App app(window, renderer, opt);
      rc = app.run();
    }
    renderer.waitIdle();
    renderer.closeDocument();  // layer thumbnails are ImGui textures: release them before ImGui
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
