// Application: window events, pen input, view, panels, benchmark and test helpers.
#pragma once
#include "brush.h"
#include <imgui.h>
#include "renderer.h"
#include "shortcuts.h"
#include <array>
#include <cstring>
#include <atomic>
#include <functional>
#include <future>
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
  bool brushTest = false;
  bool splineTest = false;
  std::string open;             // file to open at startup
  std::vector<std::string> imports;  // import as layers at startup (test helper)
  std::string save;             // save as PSD when the demo / startup is done (test helper)
  int windowW = 1600, windowH = 1000;
  bool windowSet = false;
};

enum class ToolId : int {
  Brush, Eyedropper, Fill, Gradient, Line, Rect, Ellipse, SelRect, SelEllipse, Lasso, Wand, Transform, Hand, Count
};

void applyUiScale(float scale);  // main.cpp

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
  void strokeBegin(const PenSample& s, bool eraser, bool spline = true);
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
  void buildBrushTest();
  void buildSplineTest();
  bool pendingBrushStroke = false;
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
  std::vector<BrushSettings> brushes;  // the brush library (built-in + custom)
  int tipIndex = 0;                     // current brush
  int brushIndex(const std::string& name) const;
  int legacyTip(int t) const;           // 0 hard, 1 textured, 2 soft (tests / benchmark)
  uint32_t strokeSeed = 1;
  char newBrushName[64] = {};
  // icons (Lucide, rasterised at startup into one texture)
  struct IconUV { float u0, v0, u1, v1; };
  std::vector<std::pair<std::string, IconUV>> iconUV;
  Floating iconAtlas;
  void loadIcons();
  bool icon(const char* name, IconUV& uv) const;
  void drawIcon(ImDrawList* dl, const char* name, ImVec2 centre, float size, ImU32 col) const;
  bool iconButton(const char* id, const char* name, float size, bool selected, const char* tip, bool enabled = true);
  bool eraserToggle = false;
  float color[3] = {0.08f, 0.08f, 0.1f};
  BrushEngine engine;
  bool strokeFromPen = false;

  // pointer state
  float penPressure = 1;
  bool penDownPending = false;  // pen touched; stroke starts on the first real pressure sample
  bool penHasPressure = false;  // this pen has reported pressure at least once
  std::vector<std::pair<float, float>> penDownMoves;  // motion received before the first pressure
  bool dragFromPen = false;     // the current pan/rotate drag was started by the pen
  float penDownX = 0, penDownY = 0;
  bool penDownEraser = false;
  uint64_t penDownNs = 0;
  float lastX = 0, lastY = 0;
  bool spaceDown = false, shiftDown = false, ctrlDown = false;  // spaceDown: the pan key is held
  bool rotateDown = false;                                        // the rotate-view key is held
  SDL_Keycode panKeyHeld = 0, rotateKeyHeld = 0;
  // keyboard shortcuts (Preferences > Shortcuts)
  KeyCombo keys[kActCount][2];
  void resetShortcuts();
  int findAction(const KeyCombo& c) const;
  std::string shortcutLabel(Act a) const;
  std::string skBuf[kActCount];
  const char* sk(Act a);  // primary key of an action, for menus (nullptr if none)
  void runAction(Act a, SDL_Keycode key, bool repeat);
  void selectToolAct(Act a);
  bool toolActActive(Act a) const;
  // spring-loaded tools: hold a tool key to use it, release to go back
  struct ToolHold {
    bool active = false, used = false;
    SDL_Keycode key = 0;
    ToolId prevTool = ToolId::Brush;
    bool prevEraser = false;
    uint64_t downNs = 0;
  } toolHold;
  bool restorePending = false;
  ToolId restoreTool = ToolId::Brush;
  bool restoreEraser = false;
  void releaseToolHold(bool forceRestore);
  void tickToolRestore();
  void releaseAllKeys();
  int captureAct = -1, captureSlot = 0;  // Preferences: waiting for a key to bind
  std::string captureMsg;
  bool captureKey(const SDL_KeyboardEvent& k);
  void drawShortcutsPage();
  enum class Drag { None, Pan, Rotate } drag = Drag::None;
  float dragX = 0, dragY = 0;
  double dragStartAngle = 0, dragStartRot = 0;
  bool mouseOverCanvas = true;

  // stats
  uint64_t pendingInputNs = 0;
  double lastLatencyMs = 0, avgLatencyMs = 0;
  double cpuFrameMs = 0;
  uint64_t lastFrameNs = 0;
  uint64_t lastActivityNs = 0;

  // ---- tools (tools.cpp) ----
  ToolId tool = ToolId::Brush;
  void setTool(ToolId t);
  void drawToolbar();
  // Navigator / Tool Group panels (panels.cpp)
  bool showNav = true, showToolGroup = true;
  void drawNavigator();
  void drawToolGroup();
  void drawBrushLibraryButtons();
  void keepCanvasCentre(const std::function<void()>& change);
  void rotateView(double radians);
  void flipView();
  void updateBrushPreviews();
  Floating previewAtlas;
  std::string previewKey, toolGroupShown;
  uint64_t previewBuiltNs = 0;
  int previewW = 0, previewH = 0, lastTipForGroup = -1;
  // the eraser tool keeps its own brush (e.g. Hard eraser), the brush tool its own
  int paintTip = -1, eraseTip = -1;
  void switchToolState(ToolId t, bool eraser);
  void drawToolOptions();
  void drawToolOverlay();
  bool toolDown(double dx, double dy, float sx, float sy);
  void toolMove(double dx, double dy, float sx, float sy);
  void toolUp(double dx, double dy);
  void toolKey(SDL_Keycode key, bool ctrl, bool shift, bool alt);
  void docToScreen(double dx, double dy, float& sx, float& sy) const;  // framebuffer px
  StrokeStyle currentStyle(bool eraser);  // also records the colour in the recent-colours strip
  void noteColorUsed();
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
  // Fill / magic wand: the pixels are read back on the main thread (only the painted bounds),
  // the flood runs on a worker thread, and the result is applied when it is ready.
  struct FloodResult {
    bool ok = false, isFill = false, hover = false;
    int op = 0;
    uint32_t layerId = 0;
    int x = 0, y = 0;
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> cov;
  };
  std::future<FloodResult> floodJob;
  std::shared_ptr<AsyncRead> floodRead;
  uint64_t floodRevision = 0, floodDoc = 0;
  bool flooding = false;
  void startFlood(double dx, double dy, bool isFill, bool hover = false);
  // live previews: line / shapes / gradient while dragging; fill / wand while hovering
  bool previewing = false, previewDirty = false;
  uint32_t previewSeed = 1;
  bool isShapePreviewTool() const {
    return tool == ToolId::Line || tool == ToolId::Rect || tool == ToolId::Ellipse || tool == ToolId::Gradient;
  }
  void startShapePreview();
  void updateShapePreview();
  void cancelPreviews();
  int hoverX = -1, hoverY = -1;
  bool hoverDirty = false, hoverPreviewOn = false;
  FloodResult hoverResult;
  uint64_t hoverRevision = 0, hoverDoc = 0;
  float hoverColor[4] = {-1, 0, 0, 0};
  void updateHover(double dx, double dy);
  void showHoverPreview();
  std::shared_ptr<AsyncRead> floodCache;
  std::string floodCacheKey;
  void pollFlood();
  bool fillSampleAll = false, wandSampleAll = false;
  void snapshotSelection(int x0, int y0, int x1, int y1);  // selection undo step for this area
  // transform session
  struct Xform {
    bool active = false;
    uint32_t layerId = 0;  // the layer the pixels were lifted from
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
  bool showColorSliders = false, pickerWheel = false;
  float pickPrev[3] = {};

  bool hideUI = false;          // Tab: canvas only
  // ---- preferences (Edit > Preferences, Ctrl+K) ----
  struct Prefs {
    bool startMaximized = true, perfAtStart = true, confirmUnsaved = true;
    int uiScalePct = 0;            // 0 = follow Windows display scaling
    bool smoothStrokes = true, mousePixelFix = true;
    int barrelLower = 1, barrelUpper = 2;  // 0 nothing, 1 pan, 2 pick colour, 3 toggle eraser
    float wheelZoom = 1.2f;
    int undoSteps = 50;
    float undoGB = 0;              // 0 = automatic (25 % of RAM, max 16 GB)
    bool springTools = true;       // holding a tool key switches only while held
    int holdMs = 250;              // held at least this long (or used) = temporary
  } prefs;
  bool showPrefs = false;
  int prefsPage = 0;
  void drawPrefs();
  void applyPrefs();
  float uiScaleNow() const;
  void penButton(int action, float x, float y, bool down);
  // borderless window: draggable part of the menu bar (window coordinates), read by the hit test
  float dragX0 = 0, dragX1 = 0, dragH = 0;
  void drawWindowButtons();
  // brush-size preview in the middle of the canvas
  float lastSizeSeen = -1;
  uint64_t sizePreviewUntil = 0;
  void drawSizePreview();
  // unsaved-changes guard
  uint64_t savedRevision = 0;
  enum PendingAction { PA_None, PA_Quit, PA_New, PA_OpenDialog, PA_OpenPath };
  int pendingAction = PA_None;
  std::string pendingPath;
  bool askUnsaved = false, continueAfterSave = false;
  uint64_t revisionAtSave = 0;
  bool saveOk = false;
  void requestAction(int a, const std::string& path = {});
  void performAction();
  bool modified() const { return R.hasDocument() && R.revision != savedRevision; }
  void updateTitle();
  std::string lastTitle;
  // colour
  float bgColor[3] = {1, 1, 1};
  float lastHue = 0;
  std::vector<std::array<float, 3>> recentColors;
  // status / toast
  std::string toast;
  uint64_t toastUntil = 0;
  void drawStatusBar();
  void showToast(const std::string& msg);
  // Edit operations (edit_ops.cpp)
  struct ClipImage {
    bool ok = false;
    int x = 0, y = 0;
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba, png;  // straight RGBA; PNG for other apps
    std::string name;
  } clip;
  bool clipping = false, clipPasteAfter = false, clipExternal = false;
  std::future<ClipImage> clipJob;
  std::shared_ptr<AsyncRead> clipRead;
  void copySelection(bool cut, bool pasteAfter = false);
  void pollClipboard();
  void pasteClipboard(bool internalOnly = false);
  void clearOutsideSelection();
  struct AdjustDlg { int type = 0; float p[4] = {0, 0, 0, 0}; bool open = false, preview = true; } adj;
  void openAdjust(int type);
  void drawAdjustDialog();
  void transformLayer(int kind);  // 0 flip H, 1 flip V, 2 rotate 90 cw, 3 rotate 90 ccw, 4 rotate 180
  void growSelection(int px);     // negative = shrink
  int selGrowShrink = 1, selGrowPx = 5;
  bool growPopup = false;
  void drawSelectionBar();
  void drawUnsavedDialog();
  // brush size drag (Ctrl+Alt+drag)
  bool sizeDrag = false;
  float sizeDragX = 0, sizeDragY = 0, sizeDragStart = 0;
  // layer rename popup
  int renameIndex = -1;
  char renameText[128] = {};
  // settings
  std::string settingsPath, lastFolder;
  void loadSettings();
  void saveSettings();
  void exportFlat(const std::string& path);
  bool resetLayout = false;
  std::string iniPath;
  // free canvas area (the dockspace's central node), in framebuffer pixels
  float canvasX = 0, canvasY = 0, canvasW = 0, canvasH = 0;
  bool showBrush = true, showLayers = true, showPerf = false, showNewDoc = false;
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
  enum DialogKind { DlgOpen = 1, DlgImport = 2, DlgSave = 3, DlgExport = 4 };
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
