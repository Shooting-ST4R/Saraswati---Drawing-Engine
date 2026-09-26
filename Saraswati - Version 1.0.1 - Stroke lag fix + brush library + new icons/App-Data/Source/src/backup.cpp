// Automatic versioned backups and crash recovery.
//
// Every few minutes (when the picture changed) the document is written to
// User-Data/backups/<document>/<time>.sbk without ever blocking the UI: each layer's painted area
// is read back asynchronously (GPU -> host, fence), a worker thread waits for it, run-length
// encodes the transparent parts and appends it to a temporary file; the main thread only polls.
// The finished file is renamed into place, so an interrupted backup never counts. A lock file
// marks a running session: if it is still there at the next start, the app crashed and offers
// to restore the newest backup.
#include "app.h"
#include "fileio.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>

namespace fs = std::filesystem;

static const char kMagic[8] = {'S', 'A', 'R', 'B', 'A', 'K', '0', '3'};

namespace {
template <class T> void put(std::string& s, T v) { s.append(reinterpret_cast<const char*>(&v), sizeof v); }
void putStr(std::string& s, const std::string& v) { put<uint32_t>(s, uint32_t(v.size())); s += v; }
template <class T> bool get(std::ifstream& f, T& v) { return bool(f.read(reinterpret_cast<char*>(&v), sizeof v)); }
bool getStr(std::ifstream& f, std::string& v) {
  uint32_t n;
  if (!get(f, n) || n > (1u << 20)) return false;
  v.resize(n);
  return bool(f.read(v.data(), n));
}

std::string safeName(std::string s) {
  for (char& c : s)
    if (!(std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == ' ' || (unsigned char)c >= 0x80)) c = '_';
  if (s.empty()) s = "Untitled";
  return s;
}

std::string timeStamp() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char b[32];
  std::strftime(b, sizeof b, "%Y-%m-%d_%H-%M-%S", &tm);
  return b;
}
}  // namespace

std::string App::backupRoot() const {
  if (userData.empty()) return {};
  auto u = (fs::u8path(userData) / "backups").u8string();
  return std::string(reinterpret_cast<const char*>(u.c_str()), u.size());
}

void App::backupStartup() {
  if (userData.empty()) return;
  std::error_code ec;
  fs::path root = fs::u8path(backupRoot());
  fs::create_directories(root, ec);
  fs::path lock = root / "session.lock";
  crashedLastTime = fs::exists(lock, ec);
  std::ofstream(lock) << "running\n";
  if (crashedLastTime) {
    listBackups();
    if (!backups.empty()) showRestore = true;
  }
}

void App::backupShutdown() {
  if (backupJob.valid()) backupJob.wait();
  R.finishAsyncRead(backupRead);
  if (backupFile.is_open()) backupFile.close();
  if (!backupTmp.empty()) { std::error_code ec; fs::remove(fs::u8path(backupTmp), ec); }
  if (!userData.empty()) {
    std::error_code ec;
    fs::remove(fs::u8path(backupRoot()) / "session.lock", ec);
  }
}

void App::listBackups() {
  backups.clear();
  std::error_code ec;
  fs::path root = fs::u8path(backupRoot());
  if (root.empty() || !fs::is_directory(root, ec)) return;
  for (auto& d : fs::directory_iterator(root, ec)) {
    if (!d.is_directory(ec)) continue;
    for (auto& f : fs::directory_iterator(d.path(), ec)) {
      if (f.path().extension() != ".sbk") continue;
      BackupInfo b;
      b.path = reinterpret_cast<const char*>(f.path().u8string().c_str());
      b.document = reinterpret_cast<const char*>(d.path().filename().u8string().c_str());
      b.when = reinterpret_cast<const char*>(f.path().stem().u8string().c_str());
      b.bytes = uint64_t(fs::file_size(f.path(), ec));
      backups.push_back(b);
    }
  }
  std::sort(backups.begin(), backups.end(), [](const BackupInfo& a, const BackupInfo& b) { return a.when > b.when; });
}

// Called every frame: starts a backup when due, then feeds it one layer at a time.
void App::tickBackup() {
  if (userData.empty() || !R.hasDocument()) return;
  uint64_t now = SDL_GetTicksNS();
  if (backupStage == 0) {
    if (!prefs.backupOn) return;
    if (lastBackupNs == 0) { lastBackupNs = now; return; }
    if (now - lastBackupNs < uint64_t(prefs.backupMinutes) * 60000000000ull) return;
    if (R.revision == backupRevision && R.docSerial == backupDoc) { lastBackupNs = now; return; }
    if (R.stroking() || R.busy() || xf.active || textEdit.active || saving) return;  // try again next frame
    startBackup();
    return;
  }
  // a layer is being read / written
  if (backupJob.valid()) {
    if (backupJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    bool ok = backupJob.get();
    R.finishAsyncRead(backupRead);
    if (!ok) { abortBackup("write failed"); return; }
  }
  if (R.docSerial != backupDoc) { abortBackup("document changed"); return; }
  // next layer with pixels
  while (backupNext < backupLayers.size()) {
    const BackupLayer& L = backupLayers[backupNext++];
    int li = R.indexOf(L.id);
    std::string head;
    put<uint32_t>(head, L.id);
    put<uint32_t>(head, L.parentId);
    putStr(head, L.name);
    put<uint8_t>(head, L.visible);
    put<float>(head, L.opacity);
    put<int32_t>(head, L.mode);
    put<uint8_t>(head, L.lockAlpha);
    put<uint8_t>(head, uint8_t(L.folder | L.passThrough << 1 | L.expanded << 2 | L.clip << 3));
    put<uint32_t>(head, L.folderId);
    put<ToneFx>(head, L.tone);
    put<LayerColorFx>(head, L.lcolor);
    putStr(head, L.meta);
    bool empty = li < 0 || L.folder || L.x1 <= L.x0 || L.y1 <= L.y0;
    put<int32_t>(head, empty ? 0 : L.x0);
    put<int32_t>(head, empty ? 0 : L.y0);
    put<uint32_t>(head, empty ? 0 : uint32_t(L.x1 - L.x0));
    put<uint32_t>(head, empty ? 0 : uint32_t(L.y1 - L.y0));
    backupFile.write(head.data(), std::streamsize(head.size()));
    if (empty) continue;
    std::string err;
    backupRead = R.readRegionAsync(false, li, L.x0, L.y0, uint32_t(L.x1 - L.x0), uint32_t(L.y1 - L.y0), err);
    if (!backupRead) { abortBackup(err); return; }
    std::shared_ptr<AsyncRead> rd = backupRead;
    std::ofstream* out = &backupFile;
    // worker: wait for the GPU copy, encode each row as runs of transparent / raw pixels
    backupJob = std::async(std::launch::async, [rd, out]() {
      if (!rd->wait()) return false;
      std::string buf;
      for (uint32_t y = 0; y < rd->h; ++y) {
        const uint8_t* row = rd->pixel(rd->x, rd->y + int(y));
        uint32_t x = 0;
        while (x < rd->w) {
          uint32_t s = x;
          while (x < rd->w && row[x * 4 + 3] == 0) ++x;
          put<uint32_t>(buf, x - s);  // transparent run
          s = x;
          while (x < rd->w && row[x * 4 + 3] != 0) ++x;
          put<uint32_t>(buf, x - s);  // pixel run
          buf.append(reinterpret_cast<const char*>(row + s * 4), size_t(x - s) * 4);
        }
        if (buf.size() > (8u << 20)) { out->write(buf.data(), std::streamsize(buf.size())); buf.clear(); }
      }
      out->write(buf.data(), std::streamsize(buf.size()));
      return bool(*out);
    });
    return;
  }
  // all layers written: finish the file, rename into place, drop old versions
  backupFile.write("END!", 4);
  backupFile.close();
  std::error_code ec;
  fs::path fin = fs::u8path(backupTmp);
  fin.replace_extension(".sbk");
  fs::rename(fs::u8path(backupTmp), fin, ec);
  backupTmp.clear();
  backupStage = 0;
  lastBackupNs = now;
  backupRevision = backupStartRevision;
  lastBackupText = "Backed up " + timeStamp().substr(11, 5);
  lastBackupText[lastBackupText.size() - 3] = ':';
  std::vector<fs::path> files;
  for (auto& f : fs::directory_iterator(fin.parent_path(), ec))
    if (f.path().extension() == ".sbk") files.push_back(f.path());
  std::sort(files.begin(), files.end());
  while (int(files.size()) > prefs.backupKeep) { fs::remove(files.front(), ec); files.erase(files.begin()); }
}

void App::startBackup() {
  std::error_code ec;
  std::string doc = documentPath.empty() ? std::string("Untitled")
                                         : reinterpret_cast<const char*>(fs::u8path(documentPath).stem().u8string().c_str());
  fs::path dir = fs::u8path(backupRoot()) / fs::u8path(safeName(doc));
  fs::create_directories(dir, ec);
  fs::path tmp = dir / fs::u8path(timeStamp() + ".tmp");
  backupFile.open(tmp, std::ios::binary | std::ios::trunc);
  if (!backupFile) { lastBackupNs = SDL_GetTicksNS(); return; }
  backupTmp = reinterpret_cast<const char*>(tmp.u8string().c_str());
  backupLayers.clear();
  for (const Layer& l : R.layers)
    backupLayers.push_back({l.id, l.parentId, l.name, l.visible, l.opacity, int(l.mode), l.lockAlpha, l.bx0, l.by0, l.bx1, l.by1,
                            l.folder, l.passThrough, l.expanded, l.clip, l.folderId, l.tone, l.lcolor, layerMeta(R.indexOf(l.id))});
  std::string head(kMagic, 8);
  put<uint32_t>(head, R.docW);
  put<uint32_t>(head, R.docH);
  put<uint8_t>(head, R.whitePaper);
  for (int k = 0; k < 3; ++k) put<float>(head, R.paperColor[k]);
  put<float>(head, R.docDpi);
  putStr(head, documentPath);
  put<uint32_t>(head, uint32_t(backupLayers.size()));
  backupFile.write(head.data(), std::streamsize(head.size()));
  backupNext = 0;
  backupStage = 1;
  backupDoc = R.docSerial;
  backupStartRevision = R.revision;
}

void App::abortBackup(const std::string& why) {
  SDL_Log("backup skipped: %s", why.c_str());
  if (backupJob.valid()) backupJob.wait();
  R.finishAsyncRead(backupRead);
  backupFile.close();
  std::error_code ec;
  if (!backupTmp.empty()) fs::remove(fs::u8path(backupTmp), ec);
  backupTmp.clear();
  backupStage = 0;
  lastBackupNs = SDL_GetTicksNS();
}

bool App::restoreBackup(const std::string& path) {
  std::ifstream f(fs::u8path(path), std::ios::binary);
  char magic[8];
  auto fail = [&](const std::string& m) { error("Could not restore the backup:\n" + m); return false; };
  if (!f.read(magic, 8) || memcmp(magic, kMagic, 8) != 0) return fail("not a Saraswati backup");
  uint32_t w, h, n;
  uint8_t white;
  float paper[3], dpi = 350;
  std::string docPath;
  if (!get(f, w) || !get(f, h) || !get(f, white) || !get(f, paper[0]) || !get(f, paper[1]) || !get(f, paper[2]) || !get(f, dpi) ||
      !getStr(f, docPath) || !get(f, n))
    return fail("damaged header");
  if (!newDocument(w, h, white != 0)) return false;
  for (int k = 0; k < 3; ++k) R.paperColor[k] = paper[k];
  R.docDpi = dpi;
  std::map<uint32_t, uint32_t> idMap;
  std::vector<std::pair<int, uint32_t>> parents, folders;  // layer index, old parent / folder id
  std::vector<std::pair<uint32_t, std::string>> metas;
  bool first = true;
  std::string err;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t id, parent, lw, lh;
    int32_t mode, x0, y0;
    uint8_t vis, lock, bits;
    std::string meta;
    uint32_t folderId;
    ToneFx tone;
    LayerColorFx lcolor;
    float op;
    std::string name;
    if (!get(f, id) || !get(f, parent) || !getStr(f, name) || !get(f, vis) || !get(f, op) || !get(f, mode) || !get(f, lock) ||
        !get(f, bits) || !get(f, folderId) || !get(f, tone) || !get(f, lcolor) || !getStr(f, meta) || !get(f, x0) || !get(f, y0) ||
        !get(f, lw) || !get(f, lh))
      return fail("damaged layer " + std::to_string(i));
    int idx = (bits & 1) ? R.addFolder(int(R.layers.size()), err, false) : R.addLayer(int(R.layers.size()), err, false);
    if (idx < 0) return fail(err);
    if (!(bits & 1)) first = false;
    Layer& L = R.layers[size_t(idx)];
    L.folderId = 0;
    L.passThrough = (bits & 2) != 0;
    L.expanded = (bits & 4) != 0;
    L.clip = (bits & 8) != 0;
    L.tone = tone;
    L.lcolor = lcolor;
    if (folderId) folders.push_back({idx, folderId});
    if (!meta.empty()) metas.push_back({L.id, meta});
    L.name = name;
    L.visible = vis != 0;
    L.opacity = op;
    L.mode = BlendMode(std::clamp<int>(mode, 0, int(BlendMode::Count) - 1));
    L.lockAlpha = lock != 0;
    idMap[id] = L.id;
    if (parent) parents.push_back({idx, parent});
    if (!lw || !lh) continue;
    std::vector<uint8_t> px(size_t(lw) * lh * 4, 0);
    for (uint32_t y = 0; y < lh; ++y) {
      uint32_t x = 0;
      while (x < lw) {
        uint32_t skip, run;
        if (!get(f, skip) || !get(f, run) || x + skip + run > lw) return fail("damaged pixels");
        x += skip;
        if (!f.read(reinterpret_cast<char*>(&px[(size_t(y) * lw + x) * 4]), std::streamsize(run) * 4)) return fail("damaged pixels");
        x += run;
      }
    }
    if (!R.uploadLayerPixels(idx, x0, y0, lw, lh, px.data(), err)) return fail(err);
  }
  for (auto& [idx, parent] : parents)
    if (idMap.count(parent)) R.layers[size_t(idx)].parentId = idMap[parent];
  for (auto& [idx, folder] : folders)
    if (idMap.count(folder)) R.layers[size_t(idx)].folderId = idMap[folder];
  if (!first && R.layers.size() > 1) R.eraseLayerNoUndo(0);  // the empty starting layer of the new document
  for (auto& [id, m] : metas) applyLayerMeta(R.indexOf(id), m);
  active = int(R.layers.size()) - 1;
  while (active > 0 && R.layers[size_t(active)].folder) --active;
  documentPath = docPath;
  R.markCachesDirty();
  savedRevision = ~0ull;  // restored work is not saved yet
  showToast("Restored the backup from " + fs::u8path(path).stem().string());
  return true;
}

void App::drawRestoreDialog() {
  if (showRestore) { listBackups(); ImGui::OpenPopup("Restore from backup"); showRestore = false; }
  float sc = ImGui::GetStyle().FontScaleDpi;
  ImGui::SetNextWindowSize(ImVec2(620 * sc, 420 * sc), ImGuiCond_Appearing);
  if (!ImGui::BeginPopupModal("Restore from backup", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
  if (crashedLastTime) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.35f, 1));
    ImGui::TextWrapped("Saraswati did not close normally last time. Your work was backed up automatically - pick a version to "
                       "restore it.");
    ImGui::PopStyleColor();
  } else {
    ImGui::TextWrapped("Automatic backups, newest first. Restoring opens the backup as a new, unsaved document.");
  }
  ImGui::Spacing();
  ImGui::BeginChild("list", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.4f), ImGuiChildFlags_Borders);
  if (backups.empty()) ImGui::TextDisabled("No backups yet.");
  if (ImGui::BeginTable("bk", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
    for (int i = 0; i < int(backups.size()); ++i) {
      const BackupInfo& b = backups[size_t(i)];
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::PushID(i);
      std::string when = b.when;
      std::replace(when.begin(), when.end(), '_', ' ');
      if (when.size() >= 19) { when[13] = ':'; when[16] = ':'; }
      if (ImGui::Selectable(when.c_str(), restorePick == i, ImGuiSelectableFlags_SpanAllColumns)) restorePick = i;
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(b.document.c_str());
      ImGui::TableNextColumn();
      ImGui::TextDisabled("%s", fmtBytes(double(b.bytes)).c_str());
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::EndChild();
  bool can = restorePick >= 0 && restorePick < int(backups.size());
  ImGui::BeginDisabled(!can);
  if (ImGui::Button("Restore", ImVec2(140 * sc, 0))) {
    std::string p = backups[size_t(restorePick)].path;
    ImGui::CloseCurrentPopup();
    crashedLastTime = false;
    restoreBackup(p);
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Close", ImVec2(140 * sc, 0))) { ImGui::CloseCurrentPopup(); crashedLastTime = false; }
  ImGui::SameLine();
  if (ImGui::Button("Open backups folder")) {
    std::string url = "file:///" + backupRoot();
    SDL_OpenURL(url.c_str());
  }
  ImGui::EndPopup();
}
