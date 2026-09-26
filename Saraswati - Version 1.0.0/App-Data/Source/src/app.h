// Application: window events, pen input, view, panels, benchmark and test helpers.
#pragma once
#include "brush.h"
#include "renderer.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

struct Options {
  RendererOptions renderer;
  bool benchmark = false;
  uint32_t docW = 0, docH = 0;  // 0 = default
  float brushPx = 0;           // 0 = default
  Tip tip = Tip::Hard;
  bool tipSet = false;
  int layers = 1;
  bool exitAfter = false;
  std::string screenshot;
  int frames = 5;
  bool demo = false;
  bool toolTest = false;
  std::string open;             // file to open at startup
  std::vector<std::string> imports;  // import as layers at startup (test helper)
  std::string save;             // save as PSD when the demo / startup is done (test helper)
  int windowW = 1600, windowH = 1000;
};

enum class ToolId : int {
  Brush, Eyedropper, Fill, Gradient, Line, Rect, Ellipse, SelRect, SelEllipse, Lasso, Wand, Transform, Hand, Count
};

class App {
 public:
  App(SDL_Window* w, Renderer& r, const Options& o);
  int run();

 private:
  // input
  void handleEvent(const SDL_Event& e);
  void pointerDown(float x, float y, float pressure, bool eraser, bool pen, uint64_t tNs);
  void pointerMove(float x, float y, float pressure, bool pen, uint64_t tNs);
  void pointerUp(bool pen);
  void handleKey(const SDL_KeyboardEvent& k, bool down);
  // strokes in document coordinates (used by input, benchmark and demo)
  void strokeBegin(const PenSample& s, bool eraser);
  void strokeAdd(const PenSample& s);
  void strokeEnd();
  void flushDabs(uint64_t inputNs);
  // view
  void screenToDoc(double sx, double sy, double& dx, double& dy) const;
  void zoomAt(double sx, double sy, double factor);
  void fitView();
  // UI
  void drawUI();
  void drawBrushPanel();
  void drawColorPanel();
  void buildDefaultLayout(unsigned int dockId);
  void drawLayerPanel();
  void drawPerfPanel();
  void drawNewDocDialog();
  void drawFileDialogs();
  void error(const std::string& msg);
  // actions
  bool newDocument(uint32_t w, uint32_t h, bool white);
  void addLayer();
  // benchmark / demo
  void startBenchmark(uint32_t w, uint32_t h, float brush, Tip tip, int layers);
  void tickBenchmark();
  void finishBenchmark();
  void buildDemo();
  void buildToolTest();
  void tickDemo();
  // files
  void openFile(const std::string& path);
  void importAsLayer(const std::string& path);
  void saveFile(const std::string& path);

  SDL_Window* window;
  Renderer& R;
  Options opt;
  bool running = true;
  int framesToRender = 3;
  float density = 1;

  View view;
  int active = 0;
  BrushSettings brushes[3];
  int tipIndex = 0;
  bool eraserToggle = false;
  float color[3] = {0.08f, 0.08f, 0.1f};
  BrushEngine engine;
  bool strokeFromPen = false;

  // pointer state
  float penPressure = 1;
  bool penDownPending = false;  // pen touched; stroke starts on the first pressure/motion sample
  float penDownX = 0, penDownY = 0;
  bool penDownEraser = false;
  uint64_t penDownNs = 0;
  float lastX = 0, lastY = 0;
  bool spaceDown = false, shiftDown = false, ctrlDown = false;
  enum class Drag { None, Pan, Rotate } drag = Drag::None;
  float dragX = 0, dragY = 0;
  double dragStartAngle = 0, dragStartRot = 0;
  bool mouseOverCanvas = true;

  // stats
  uint64_t pendingInputNs = 0;
  double lastLatencyMs = 0, avgLatencyMs = 0;
  double cpuFrameMs = 0;
  uint64_t lastFrameNs = 0;

  // ---- tools (tools.cpp) ----
  ToolId tool = ToolId::Brush;
  void setTool(ToolId t);
  void drawToolbar();
  void drawToolOptions();
  void drawToolOverlay();
  bool toolDown(double dx, double dy, float sx, float sy);
  void toolMove(double dx, double dy, float sx, float sy);
  void toolUp(double dx, double dy);
  void toolKey(SDL_Keycode key, bool ctrl, bool shift, bool alt);
  void docToScreen(double dx, double dy, float& sx, float& sy) const;  // framebuffer px
  StrokeStyle currentStyle(bool eraser) const;
  // selection (CPU copy is the source of truth; the GPU copy is for shading/clipping)
  std::vector<uint8_t> selCpu;
  int selX0 = 0, selY0 = 0, selX1 = 0, selY1 = 0;
  bool selActive = false;
  void resetSelection();
  void combineSelection(int x, int y, uint32_t w, uint32_t h, const std::vector<uint8_t>& cov, int op);
  void selectAll();
  void deselect();
  void invertSelection();
  void fillSelection(bool erase);
  bool floodMask(double dx, double dy, float tol, bool contiguous, std::vector<uint8_t>& mask, int& x0, int& y0, int& x1, int& y1);
  // transform session
  struct Xform {
    bool active = false;
    Floating fl;
    int srcX = 0, srcY = 0;
    double q[4][2] = {}, start[4][2] = {};
    int drag = 0;  // 0 none, 1 move, 2 scale corner, 3 distort corner, 4 rotate
    int corner = 0;
    double gx = 0, gy = 0, a0 = 0;
  } xf;
  void startTransform();
  void applyTransform();
  void cancelTransform();
  // tool state
  bool toolDrag = false;
  double t0x = 0, t0y = 0, t1x = 0, t1y = 0;
  std::vector<double> lasso;
  int selOp = 0;  // 0 replace, 1 add, 2 subtract
  float fillTol = 0.08f, wandTol = 0.08f;
  bool fillContiguous = true, wandContiguous = true, fillGrow = true, shapeFilled = false;
  bool haveLastStroke = false;
  double lastEndX = 0, lastEndY = 0;
  bool picking = false;
  bool altDown = false;

  // UI state
  bool showColor = true;
  bool resetLayout = false;
  std::string iniPath;
  // free canvas area (the dockspace's central node), in framebuffer pixels
  float canvasX = 0, canvasY = 0, canvasW = 0, canvasH = 0;
  bool showBrush = true, showLayers = true, showPerf = true, showNewDoc = false;
  int newW = 3000, newH = 2000;
  bool newWhite = true;
  LayerLimit newLimit;
  int newLimitW = 0, newLimitH = 0;
  std::string errorMsg;
  bool errorOpen = false;
  char renameBuf[128] = {};
  int renameFor = -1;
  std::string userData;
  std::string documentPath;
  // pending results from SDL's async file dialogs (callbacks may run on another thread)
  enum DialogKind { DlgOpen = 1, DlgImport = 2, DlgSave = 3 };
  std::mutex dialogMutex;
  std::vector<std::pair<int, std::string>> dialogResults;
  void showDialog(int kind);
  void processDialogResults();
  // background PSD writer
  std::thread saveThread;
  std::atomic<bool> saving{false};
  std::atomic<float> saveProgress{0};
  std::mutex saveMutex;
  std::string saveMessage;
  bool saveDone = false;
  bool savedForTest = false;
public:
  ~App();
private:

  // benchmark
  struct Bench {
    bool running = false, ended = false;
    uint64_t t0 = 0;
    std::vector<PenSample> samples;
    std::vector<double> times;
    size_t next = 0;
    std::vector<double> frameMs, gpuMs, gpuDabMs, latencyMs;
    uint64_t dabs0 = 0;
    double mp0 = 0;
    uint32_t docW = 0, docH = 0;
    float brush = 0;
    Tip tip = Tip::Hard;
    int layers = 1;
    std::string report;
  } bench;
  int benchUiW = 8000, benchUiH = 8000, benchUiBrush = 1000, benchUiTip = 0, benchUiLayers = 3;

  // scripted demo (visual test)
  std::vector<std::function<void()>> demo;
  size_t demoStep = 0;
  bool demoDone = false;
  int framesRendered = 0;
  bool screenshotTaken = false;
};
