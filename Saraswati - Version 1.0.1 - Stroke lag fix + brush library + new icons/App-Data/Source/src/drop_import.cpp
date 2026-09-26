// Drag and drop + brush import: files (images, documents, brushes, brush packs) and text drops
// from browsers (image links, data: URIs). The file type decides what happens.
#include "app.h"
#include "brush_import.h"
#include "fileio.h"
#include "stb_image.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <urlmon.h>
#endif

namespace fs = std::filesystem;

static std::string lowerExt(const std::string& path) {
  std::string e = reinterpret_cast<const char*>(fs::u8path(path).extension().u8string().c_str());
  for (char& c : e) c = char(std::tolower((unsigned char)c));
  return e;
}

// ---------------------------------------------------------------------------
// .zip brush packs: every .abr / .sut inside is imported

static std::vector<std::pair<std::string, std::string>> unzip(const std::string& path) {
  std::vector<std::pair<std::string, std::string>> files;
  std::ifstream f(fs::u8path(path), std::ios::binary);
  std::string z((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  auto u16 = [&](size_t p) { return p + 2 <= z.size() ? uint32_t((unsigned char)z[p] | (unsigned char)z[p + 1] << 8) : 0u; };
  auto u32 = [&](size_t p) { return p + 4 <= z.size() ? u16(p) | u16(p + 2) << 16 : 0u; };
  size_t eocd = z.size() >= 22 ? z.size() - 22 : std::string::npos;
  while (eocd != std::string::npos && eocd + 4 <= z.size() && u32(eocd) != 0x06054b50) eocd = eocd ? eocd - 1 : std::string::npos;
  if (eocd == std::string::npos || eocd + 22 > z.size()) return files;
  uint32_t count = u16(eocd + 10);
  size_t p = u32(eocd + 16);
  for (uint32_t i = 0; i < count && p + 46 <= z.size() && u32(p) == 0x02014b50; ++i) {
    uint32_t method = u16(p + 10), csize = u32(p + 20), usize = u32(p + 24);
    uint32_t nlen = u16(p + 28), xlen = u16(p + 30), clen = u16(p + 32), local = u32(p + 42);
    std::string name = z.substr(p + 46, nlen);
    p += 46 + nlen + xlen + clen;
    if (local + 30 > z.size() || u32(local) != 0x04034b50) continue;
    size_t data = local + 30 + u16(local + 26) + u16(local + 28);
    if (data + csize > z.size() || usize > (1u << 30)) continue;
    std::string ext = name.size() > 4 ? name.substr(name.size() - 4) : name;
    for (char& c : ext) c = char(std::tolower((unsigned char)c));
    if (ext != ".abr" && ext != ".sut") continue;
    if (method == 0) files.push_back({name, z.substr(data, csize)});
    else if (method == 8) {
      int outLen = 0;
      char* out = stbi_zlib_decode_noheader_malloc(z.data() + data, int(csize), &outLen);
      if (out) { files.push_back({name, std::string(out, size_t(outLen))}); free(out); }
    }
  }
  return files;
}

void App::importBrushFile(const std::string& path) {
  std::string ext = lowerExt(path);
  std::vector<std::pair<std::string, std::string>> items;  // (display name, file path)
  std::vector<std::string> temps;
  if (ext == ".zip") {
    fs::path tmpDir = fs::temp_directory_path() / "saraswati-brush-import";
    std::error_code ec;
    fs::create_directories(tmpDir, ec);
    int n = 0;
    for (auto& [name, data] : unzip(path)) {
      fs::path t = tmpDir / ("brush" + std::to_string(n++) + (name.size() > 4 ? name.substr(name.size() - 4) : std::string(".abr")));
      std::ofstream(t, std::ios::binary).write(data.data(), std::streamsize(data.size()));
      std::string tp = reinterpret_cast<const char*>(t.u8string().c_str());
      temps.push_back(tp);
      std::string display = fs::path(name).stem().string();
      items.push_back({display, tp});
    }
    if (items.empty()) { error("The package contains no Photoshop (.abr) or Clip Studio (.sut) brushes."); return; }
  } else {
    items.push_back({"", path});
  }
  importReport.clear();
  int added = 0;
  for (auto& [display, file] : items) {
    BrushImport bi;
    std::string err;
    std::string e = lowerExt(file);
    bool ok = e == ".abr" ? importAbr(file, bi, err) : importSut(file, bi, err);
    std::string label = display.empty() ? fs::u8path(file).filename().string() : display;
    if (!ok) { importReport.push_back(label + ": " + err); continue; }
    std::vector<std::string> names(bi.images.size());
    for (size_t i = 0; i < bi.images.size(); ++i) names[i] = addTipImage(bi.images[i].name, bi.images[i].w, bi.images[i].h, std::move(bi.images[i].alpha));
    for (const TipRef& r : bi.tipRefs) {
      BrushSettings& b = bi.brushes[r.brush];
      for (size_t t : r.tips) if (t < names.size()) b.tips.push_back(names[t]);
      if (r.texture < names.size()) b.texture = names[r.texture];
      if (r.dual < names.size()) b.dualTip = names[r.dual];
    }
    for (BrushSettings& b : bi.brushes) {
      if (!display.empty() && b.group.rfind("brush", 0) == 0) b.group = display;
      if (!b.notes.empty()) importReport.push_back(b.name + ": " + b.notes);
      brushes.push_back(b);
      ++added;
    }
    for (auto& n : bi.fileNotes) importReport.push_back(n);
  }
  std::error_code ec;
  for (auto& t : temps) fs::remove(fs::u8path(t), ec);
  if (added) {
    tipIndex = int(brushes.size()) - 1;
    switchToolState(ToolId::Brush, false);
    toolGroupShown.clear();
    importReport.insert(importReport.begin(), "Imported " + std::to_string(added) + " brush" + (added == 1 ? "" : "es") + ".");
  }
  showImportReport = true;
  saveSettings();
}

void App::drawImportReport() {
  if (showImportReport) { ImGui::OpenPopup("Brush import"); showImportReport = false; }
  float sc = ImGui::GetStyle().FontScaleDpi;
  ImGui::SetNextWindowSize(ImVec2(620 * sc, 380 * sc), ImGuiCond_Appearing);
  if (!ImGui::BeginPopupModal("Brush import", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
  ImGui::BeginChild("rep", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.3f), ImGuiChildFlags_Borders);
  for (size_t i = 0; i < importReport.size(); ++i) {
    if (i == 0) ImGui::TextUnformatted(importReport[i].c_str());
    else ImGui::TextWrapped("- %s", importReport[i].c_str());
  }
  if (importReport.size() > 1)
    ImGui::TextDisabled("\nAnything listed above is not reproduced yet; the rest of each brush is.");
  ImGui::EndChild();
  if (ImGui::Button("OK", ImVec2(120 * sc, 0))) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// Drops

bool App::dropIsOverLayers(float x, float y) const {
  return x >= layersPanelMin.x && x <= layersPanelMax.x && y >= layersPanelMin.y && y <= layersPanelMax.y;
}

void App::handleDropFile(const std::string& path, float x, float y) {
  std::string ext = lowerExt(path);
  if (ext == ".abr" || ext == ".sut" || ext == ".zip") { importBrushFile(path); return; }
  if (!R.hasDocument() || ctrlDown) { requestAction(PA_OpenPath, path); return; }  // Ctrl: open as a document
  if (isPsdPath(path) && !dropIsOverLayers(x, y)) { importAsLayer(path); return; }
  // an image: a new layer - centred where it was dropped on the canvas
  double dx = -1, dy = -1;
  float fx = x * density, fy = y * density;
  if (!dropIsOverLayers(x, y) && fx >= canvasX && fy >= canvasY && fx < canvasX + canvasW && fy < canvasY + canvasH) screenToDoc(fx, fy, dx, dy);
  importAsLayer(path, dx, dy);
}

// text drops: browsers give the image's address (http / file / data:)
void App::handleDropText(const std::string& text, float x, float y) {
  std::string t = text.substr(0, text.find_first_of("\r\n"));
  if (t.rfind("file://", 0) == 0) {  // file:///C:/... or file:///home/...
    std::string p = t.substr(7);
    std::string dec;
    for (size_t i = 0; i < p.size(); ++i) {
      if (p[i] == '%' && i + 2 < p.size()) { dec += char(std::strtol(p.substr(i + 1, 2).c_str(), nullptr, 16)); i += 2; }
      else dec += p[i];
    }
#ifdef _WIN32
    if (dec.size() > 2 && dec[0] == '/' && dec[2] == ':') dec.erase(0, 1);
#endif
    handleDropFile(dec, x, y);
    return;
  }
  fs::path tmp = fs::temp_directory_path() / "saraswati-drop";
  std::error_code ec;
  fs::create_directories(tmp, ec);
  if (t.rfind("data:image/", 0) == 0) {  // inline image (base64)
    size_t comma = t.find(',');
    if (comma == std::string::npos || t.find(";base64") > comma) { showToast("Unsupported dropped data"); return; }
    static const std::string b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = comma + 1; i < t.size(); ++i) {
      size_t v = b64.find(t[i]);
      if (v == std::string::npos) continue;
      acc = acc << 6 | uint32_t(v);
      bits += 6;
      if (bits >= 8) { bits -= 8; out += char((acc >> bits) & 0xff); }
    }
    fs::path f = tmp / "dropped-image";
    std::ofstream(f, std::ios::binary).write(out.data(), std::streamsize(out.size()));
    handleDropFile(reinterpret_cast<const char*>(f.u8string().c_str()), x, y);
    return;
  }
  if (t.rfind("http://", 0) == 0 || t.rfind("https://", 0) == 0) {
#ifdef _WIN32
    std::string name = t.substr(t.find_last_of('/') + 1);
    name = name.substr(0, name.find_first_of("?#"));
    if (name.empty() || name.find('.') == std::string::npos) name = "dropped-image.png";
    fs::path f = tmp / fs::u8path(name);
    std::wstring url(t.begin(), t.end());
    showToast("Downloading the dropped image...");
    if (URLDownloadToFileW(nullptr, url.c_str(), f.c_str(), 0, nullptr) == S_OK) {
      handleDropFile(reinterpret_cast<const char*>(f.u8string().c_str()), x, y);
      return;
    }
    error("Could not download the dropped image:\n" + t);
#else
    showToast("Dropping web links is supported on Windows; save the image and drop the file");
#endif
    return;
  }
  showToast("Drop images, documents or brush files (.abr, .sut, .zip)");
}
