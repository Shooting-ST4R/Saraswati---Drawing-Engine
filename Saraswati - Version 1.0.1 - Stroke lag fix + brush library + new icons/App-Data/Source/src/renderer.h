// Saraswati GPU engine: Vulkan device, swapchain, document layers, stroke mask,
// screen caches, dab dispatch, stroke commit, undo, and the per-frame composite.
#pragma once
#include "vk_util.h"
#include <SDL3/SDL.h>
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct ImDrawData;

enum class Tip : int { Hard = 0, Textured = 1, Soft = 2 };

// Layer blend modes (Clip Studio Paint's list). Values are the shader's mode ids.
enum class BlendMode : int {
  Normal, Darken, Multiply, ColorBurn, LinearBurn, Subtract, Lighten, Screen, ColorDodge,
  GlowDodge, Add, AddGlow, Overlay, SoftLight, HardLight, Difference, VividLight, LinearLight,
  PinLight, HardMix, Exclusion, DarkerColor, LighterColor, Divide, Hue, Saturation, Color,
  Brightness, Count
};
const char* blendModeName(BlendMode m);

// One brush footprint: elliptical (cosA/sinA = tip angle, invRound = 1 / roundness).
struct Dab { float x, y, radius, alpha, cosA, sinA, invRound, pad; };

struct Layer {
  uint32_t id = 0;
  std::string name;
  GpuImage image;
  VkDescriptorSet set = VK_NULL_HANDLE;
  bool visible = true;
  float opacity = 1.0f;
  BlendMode mode = BlendMode::Normal;
  bool lockAlpha = false;  // paint only where the layer already has pixels
  // conservative bounds of painted pixels (empty when x0 >= x1)
  int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
  // small preview for the layer panel (rendered on the GPU)
  GpuImage thumb;
  VkDescriptorSet thumbSet0 = VK_NULL_HANDLE;
  VkDescriptorSet thumbTex = VK_NULL_HANDLE;
  bool thumbDirty = true;
};

// Layer properties that are undoable as one step.
struct LayerProps {
  std::string name;
  bool visible = true;
  float opacity = 1.0f;
  BlendMode mode = BlendMode::Normal;
  bool lockAlpha = false;
};

struct View {
  double panX = 0, panY = 0;  // document point at the window centre
  double zoom = 1;            // screen px per document px
  double rotation = 0;        // radians
  bool operator==(const View&) const = default;
};

// What the brush does to the mask and how the stroke is committed.
struct StrokeStyle {
  float hardness = 1.0f, texStrength = 0.0f, texScale = 6.0f;
  bool buildUp = false;  // airbrush accumulation (else max: even strokes)
  float color[3] = {0, 0, 0};
  float opacity = 1.0f;
  bool eraser = false;
};

struct RendererOptions {
  std::string vulkanDriver;  // explicit driver / loader library
  bool cpuVulkan = false;
  int gpuIndex = -1;
  bool validation = false;
};

// Non-blocking readback: GPU copies into host buffers (bands of rows), signalled by a fence.
// A worker thread calls wait() and reads the mapped bands; the main thread calls
// Renderer::finishAsyncRead() afterwards to release everything.
struct AsyncRead {
  VkDevice device = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  std::vector<GpuBuffer> bands;
  uint32_t bandRows = 0;
  int x = 0, y = 0;
  uint32_t w = 0, h = 0;
  GpuImage tile;                          // merged: flatten target
  VkDescriptorSet tileSet = VK_NULL_HANDLE;
  bool wait() const { return vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS; }
  const uint8_t* pixel(int px, int py) const {  // premultiplied RGBA, document coordinates
    uint32_t r = uint32_t(py - y);
    return (const uint8_t*)bands[r / bandRows].mapped + (size_t(r % bandRows) * w + uint32_t(px - x)) * 4;
  }
};

// Transform tool: a floating copy of pixels (straight alpha) + its descriptor sets.
struct Floating {
  GpuImage image;
  VkDescriptorSet set = VK_NULL_HANDLE;       // set 2 for the stamp shader
  VkDescriptorSet imguiTex = VK_NULL_HANDLE;  // ImGui texture for the live preview
  uint32_t w = 0, h = 0;
};

struct LayerLimit {
  bool ok = false;
  std::string reason;
  int maxLayers = 0;
  VkDeviceSize layerBytes = 0, maskBytes = 0, usableBytes = 0;
};

// CPU time spent in each part of Renderer::renderFrame (for the frame-spike log).
struct FrameCpuStats {
  double fenceMs = 0, acquireMs = 0, recordMs = 0, submitMs = 0, presentMs = 0, thumbMs = 0, undoMs = 0;
};

struct GpuTimings {  // milliseconds; -1 when timestamps are unavailable
  double dabs = -1, commit = -1, caches = -1, composite = -1, total = -1;
};

class Renderer {
 public:
  static constexpr int kFrames = 2;
  static constexpr uint32_t kTile = 256;
  static constexpr uint32_t kMaxDabsPerFrame = 65536;

  // ---- setup (call before SDL window creation / after) ----
  static bool loadVulkan(const RendererOptions& opt, std::string& err);  // before creating the window
  void init(SDL_Window* window, const RendererOptions& opt);
  void shutdown();
  void initImGui();

  // ---- document ----
  LayerLimit computeLayerLimit(uint32_t w, uint32_t h);
  bool newDocument(uint32_t w, uint32_t h, bool whitePaper, std::string& err);
  bool hasDocument() const { return docW > 0; }
  int addLayer(int index, std::string& err, bool undoable = true);  // inserts at index; returns index or -1
  int duplicateLayer(int index, std::string& err);
  void deleteLayer(int index);
  void moveLayer(int index, int delta) { moveLayerTo(index, index + delta); }
  void moveLayerTo(int from, int to);  // one undo step
  int indexOf(uint32_t id) const { return findLayer(id); }
  void closeDocument() { destroyDocument(); runDeferred(true); }
  void recordLayerProps(int index);  // call before changing name/visibility/opacity/mode/lock (undo step)
  void updateThumbnails(int) {}  // thumbnails are now rendered inside renderFrame (no GPU waits)
  int heldLayers() const;            // deleted layers kept alive by undo
  uint64_t revision = 0;             // id of the current document state (undo restores earlier ids)
  void bumpRevision() { revision = ++revCounter; }
  uint64_t revCounter = 0;
  // Selection snapshots live in the app; the renderer keeps them in the shared undo history.
  // swap(data, x, y, w, h, bbox/active) exchanges the stored region with the current one.
  std::function<void(std::vector<uint8_t>&, int, int, int, int, int*)> selectionSwap;
  void pushSelectionUndo(std::vector<uint8_t>&& region, int x, int y, int w, int h, const int bbox[5]);
  bool uploadLayerPixels(int index, int x, int y, uint32_t w, uint32_t h, const uint8_t* premulRgba, std::string& err);
  bool readLayerPixels(int index, std::vector<uint8_t>& premulRgba, std::string& err);  // whole layer
  bool readMergedPixels(std::vector<uint8_t>& premulRgba, std::string& err) {             // flattened doc
    return readMergedRegion(0, 0, docW, docH, premulRgba, err);
  }
  bool readMergedRegion(int x, int y, uint32_t w, uint32_t h, std::vector<uint8_t>& premulRgba, std::string& err);

  // ---- strokes ----
  void beginStroke(int layerIndex, const StrokeStyle& style);
  void queueDabs(const std::vector<Dab>& dabs) { pendingDabs.insert(pendingDabs.end(), dabs.begin(), dabs.end()); }
  void endStroke() { if (strokeActive) strokeEnding = true; }
  bool stroking() const { return strokeActive; }
  bool busy() const { return strokeActive || !pendingDabs.empty() || !undoOps.empty(); }

  // ---- tools (renderer_tools.cpp) ----
  // Paints `cov` (8-bit coverage, w x h at x, y) into the layer with the style's colour /
  // eraser, through the normal commit (undo + selection clipping). Applied next frame.
  bool paintCoverage(int layer, int x, int y, uint32_t w, uint32_t h, const uint8_t* cov, const StrokeStyle& st, std::string& err);
  // Linear gradient (colour at p0 -> transparent at p1) over the given rect, next frame.
  bool paintGradient(int layer, float x0, float y0, float x1, float y1, int rx0, int ry0, int rx1, int ry1, const StrokeStyle& st);
  bool uploadSelection(const uint8_t* docSizeSel, int x, int y, uint32_t w, uint32_t h, std::string& err);
  void setSelectionActive(bool on) { if (selectionActive != on) { selectionActive = on; } }
  bool selectionActive = false;
  std::shared_ptr<AsyncRead> readRegionAsync(bool merged, int layer, int x, int y, uint32_t w, uint32_t h, std::string& err);
  void finishAsyncRead(std::shared_ptr<AsyncRead>& r);
  uint64_t docSerial = 0;  // increases with every new/opened document
  bool readLayerRegion(int index, int x, int y, uint32_t w, uint32_t h, std::vector<uint8_t>& premulRgba, std::string& err);
  bool createFloating(uint32_t w, uint32_t h, const uint8_t* straightRgba, Floating& f, std::string& err);
  void destroyFloating(Floating& f);
  // Draws the floating image into the layer through `inv` (document px -> floating px, row-major
  // 3x3 homography) over the rect; one undo step.
  bool stamp(int layer, const Floating& f, const double inv[9], int rx0, int ry0, int rx1, int ry1, std::string& err);
  // Eyedropper: reads the composited colour at a window pixel during the next frame.
  void requestPick(int x, int y) { pickX = x; pickY = y; pickPending = true; }
  bool takePick(float rgba[4]) { if (!pickReady) return false; for (int i = 0; i < 4; ++i) rgba[i] = pickValue[i]; pickReady = false; return true; }

  // ---- undo ----
  void undo() { if (!strokeActive) undoOps.push_back(1); }
  void redo() { if (!strokeActive) undoOps.push_back(0); }
  void undoDiscard() { undoOps.push_back(2); }  // undo the last step without keeping it for redo
  bool canUndo() const { return !undoStack.empty(); }
  bool canRedo() const { return !redoStack.empty(); }
  size_t undoBytes() const;
  size_t undoSteps() const { return undoStack.size(); }

  // ---- frame ----
  struct FrameParams {
    View view;
    int activeLayer = 0;
    ImDrawData* imgui = nullptr;
    std::string screenshotPath;  // non-empty: save this frame as PNG (waits for the GPU)
  };
  // Returns false if the frame was skipped (e.g. swapchain out of date / minimised).
  bool renderFrame(const FrameParams& p);
  void markCachesDirty() { cachesDirty = true; }
  void setPresentMode(VkPresentModeKHR m) { if (m != presentMode) { presentMode = m; swapchainDirty = true; } }
  void waitIdle();
  void onResize() { swapchainDirty = true; }

  // ---- state readable by the app ----
  std::vector<Layer> layers;
  uint32_t docW = 0, docH = 0;
  bool whitePaper = true;
  LayerLimit limit;
  std::string deviceName, driverInfo;
  bool cpuEmulation = false;
  bool timestampsSupported = false;
  VkExtent2D extent{};
  VkPresentModeKHR presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
  std::vector<VkPresentModeKHR> presentModes;
  GpuTimings lastTimings;
  FrameCpuStats lastCpu;
  uint32_t lastFrameDabs = 0;
  uint64_t totalDabs = 0;
  double totalDabMegapixels = 0;
  uint32_t maxImageDim = 0;
  bool maskR16 = true;
  VkDeviceSize layerBytesTotal() const;
  VkDeviceSize memoryBudgetBytes = 0;   // device-local heap (or budget)
  VkDeviceSize undoBudgetBytes = 0;
  std::string lastError;               // e.g. out of memory during a stroke commit

  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t queueFamily = 0;
  VkRenderPass renderPass = VK_NULL_HANDLE;
  uint32_t swapImageCount = 0;
  uint32_t apiVersion = VK_API_VERSION_1_1;

 private:
  struct Chunk { GpuBuffer buf; uint32_t firstTile = 0, tileCount = 0, cls = 0; };
  enum class UndoKind { Tiles, Props, Order, Deleted, Added, Selection };
  struct UndoEntry {
    UndoKind kind = UndoKind::Tiles;
    uint32_t layerId = 0;
    std::vector<VkRect2D> tiles;
    std::vector<Chunk> chunks;
    size_t bytes = 0;
    LayerProps props;             // Props
    std::vector<uint32_t> order;  // Order: layer ids bottom to top
    Layer held;                   // Deleted: the layer, kept alive for undo
    int index = 0;                // Deleted: where it was
    uint64_t revBefore = 0, revAfter = 0;
    std::vector<uint8_t> sel;     // Selection: 8-bit region x, y, w, h (in rect)
    int rect[4] = {};
    int selState[5] = {};         // Selection: bbox x0, y0, x1, y1 + active
  };
  void growBounds(Layer& l, int x0, int y0, int x1, int y1);
  bool applyLayerUndo(UndoEntry& e);  // non-tile kinds; swaps the entry with the current state
  bool forceTransparentPaper = false;
  struct FrameSlot {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    GpuBuffer dabBuf;
    VkDescriptorSet set0 = VK_NULL_HANDLE;
    uint64_t frameNumber = 0;
    bool queried = false;
  };

  void createDevice(const RendererOptions& opt);
  void createPipelines();
  void createSwapchain();
  void destroySwapchain();
  void createScreenImages();
  void destroyDocument();
  void writeSet0(FrameSlot& s);
  bool createLayer(Layer& l, std::string& err);
  void destroyLayer(Layer& l);
  void defer(std::function<void()> fn);
  void runDeferred(bool all);
  VkCommandBuffer beginOneShot();
  void endOneShot(VkCommandBuffer cmd);

  void recordUndoOps(VkCommandBuffer cmd);
  void recordDabs(VkCommandBuffer cmd, FrameSlot& s);
  void recordCommit(VkCommandBuffer cmd);
  void recordCaches(VkCommandBuffer cmd, const FrameParams& p);
  void recordFrameComposite(VkCommandBuffer cmd, const FrameParams& p);
  bool copyTiles(VkCommandBuffer cmd, Layer& layer, const std::vector<VkRect2D>& tiles,
                 std::vector<Chunk>& chunks, size_t& bytes, bool toBuffer);
  void dropUndo(UndoEntry& e);
  void pushUndo(UndoEntry&& e);
  std::vector<VkRect2D> tilesForRect(int x0, int y0, int x1, int y1) const;
  void markDirtyRect(int x0, int y0, int x1, int y1);
  bool uploadRegion(VkImage img, int x, int y, uint32_t w, uint32_t h, uint32_t bpp,
                    const std::function<void(uint32_t row, uint8_t* dst)>& fillRow, std::string& err);
  void readTimestamps(FrameSlot& s);
  void recordThumbnails(VkCommandBuffer cmd);
  uint64_t lastThumbNs = 0;
  // recycled host buffers for undo tiles (size classes of 4, 16 and 64 tiles)
  static constexpr uint32_t kChunkClasses[3] = {4, 16, 64};
  std::vector<GpuBuffer> chunkPool[3];
  bool acquireChunk(uint32_t tiles, GpuBuffer& out, uint32_t& cls);
  void releaseChunk(GpuBuffer b, uint32_t cls);
  void prewarmChunks();
  // the stroke's undo entry, filled tile by tile while the stroke is drawn (spreads the copy cost)
  void copyPendingUndo(VkCommandBuffer cmd, Layer& layer);
  void releaseStrokeUndo();
  int findLayer(uint32_t id) const;
  void pushView(VkCommandBuffer cmd, const View& v, VkExtent2D ext, int flags, float opacity, BlendMode mode);

  SDL_Window* window = nullptr;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  VkFormat swapFormat = VK_FORMAT_B8G8R8A8_UNORM;
  std::vector<VkImage> swapImages;
  std::vector<VkImageView> swapViews;
  std::vector<VkFramebuffer> framebuffers;
  std::vector<VkSemaphore> renderFinished;  // per swapchain image
  bool swapchainDirty = false;
  bool swapTransferSrc = false;
  VkPhysicalDeviceMemoryProperties memProps{};
  bool hasMemoryBudget = false;
  VkDeviceSize maxAllocationSize = 0;
  float timestampPeriod = 1;

  VkDescriptorSetLayout set0Layout = VK_NULL_HANDLE, set1Layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
  VkDescriptorPool descPool = VK_NULL_HANDLE;
  VkPipeline dabPipe = VK_NULL_HANDLE, commitPipe = VK_NULL_HANDLE, cachePipe = VK_NULL_HANDLE,
             framePipe = VK_NULL_HANDLE, presentPipe = VK_NULL_HANDLE, gradPipe = VK_NULL_HANDLE,
             stampPipe = VK_NULL_HANDLE;
  VkQueryPool queryPool = VK_NULL_HANDLE;
  VkCommandPool oneShotPool = VK_NULL_HANDLE;

  std::array<FrameSlot, kFrames> slots;
  uint64_t frameCounter = 0, completedFrame = 0;
  std::vector<std::pair<uint64_t, std::function<void()>>> deferred;

  // document GPU state
  GpuImage mask;
  GpuImage sel;  // selection coverage, same format family as the mask (R8 or R32F)
  GpuBuffer pickBuf;
  bool pickPending = false, pickReady = false;
  int pickX = 0, pickY = 0;
  float pickValue[4] = {};
  GpuImage below, above, work;  // window-size screen caches + frame result
  bool cachesDirty = true;
  View cacheView{};
  int cacheActive = -1;
  VkExtent2D cacheExtent{};
  bool aboveSimple = true;  // every visible layer above the active one is Normal -> "above" cache usable
  uint32_t nextLayerId = 1;

  // stroke state
  bool strokeActive = false, strokeEnding = false;
  uint32_t strokeLayerId = 0;
  StrokeStyle style;
  std::vector<Dab> pendingDabs;
  size_t pendingOffset = 0;
  int32_t sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0;  // stroke bbox (x1/y1 exclusive), empty if x0 >= x1
  std::vector<uint8_t> tileDirty;
  uint32_t tilesX = 0, tilesY = 0;
  std::vector<uint32_t> dirtyTiles;
  UndoEntry strokeUndo;
  bool strokeUndoOk = true;
  size_t undoCopied = 0;

  std::deque<UndoEntry> undoStack, redoStack;
  std::vector<int> undoOps;  // 1 = undo, 0 = redo, 2 = undo and discard
};
