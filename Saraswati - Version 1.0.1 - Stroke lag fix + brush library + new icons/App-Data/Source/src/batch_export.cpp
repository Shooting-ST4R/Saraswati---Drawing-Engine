// Version Save and Batch Export.
//   Files are named "YYYY MM DD - name - Version N.psd": the date is when the picture was started
//   and never changes, Version Save writes the next version next to the current one.
//   Batch Export writes the list of images the user set up (format, size, quality) next to the .psd:
//   "YYYY MM DD - name - Version N - S50% Q95.jpg", "... - S100%.png" (the extension is the format).
#include "app.h"
#include "fileio.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

namespace {
fs::path u8p(const std::string& s) { return fs::path(reinterpret_cast<const char8_t*>(s.c_str())); }
std::string u8s(const fs::path& p) { return reinterpret_cast<const char*>(p.u8string().c_str()); }

}  // namespace

std::string todayStamp() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char b[16];
  std::strftime(b, sizeof b, "%Y %m %d", &tm);
  return b;
}

namespace {
std::string today() { return todayStamp(); }

// "2026 09 22 - name - Version 3" -> date, name, 3
bool parseVersioned(const std::string& stem, std::string& date, std::string& name, int& ver) {
  auto dig = [&](size_t i) { return i < stem.size() && std::isdigit((unsigned char)stem[i]); };
  if (stem.size() < 25 || !(dig(0) && dig(1) && dig(2) && dig(3) && stem[4] == ' ' && dig(5) && dig(6) && stem[7] == ' ' &&
                            dig(8) && dig(9)) || stem.compare(10, 3, " - ") != 0)
    return false;
  size_t v = stem.rfind(" - Version ");
  if (v == std::string::npos || v < 13) return false;
  std::string num = stem.substr(v + 11);
  if (num.empty() || num.size() > 6) return false;
  for (char c : num) if (!std::isdigit((unsigned char)c)) return false;
  date = stem.substr(0, 10);
  name = stem.substr(13, v - 13);
  ver = std::stoi(num);
  return !name.empty();
}

// first free "date - name - Version N.psd" in dir, N >= from
fs::path freeVersion(const fs::path& dir, const std::string& date, const std::string& name, int from) {
  std::error_code ec;
  for (int n = std::max(1, from);; ++n) {
    fs::path p = dir / u8p(date + " - " + name + " - Version " + std::to_string(n) + ".psd");
    if (!fs::exists(p, ec) || n > 99999) return p;
  }
}
}  // namespace

std::vector<ExportSpec> defaultBatchSpecs() {
  return {{ExportSpec::Jpeg, 100, 100}, {ExportSpec::Png, 100, 100}, {ExportSpec::Jpeg, 50, 95}, {ExportSpec::Png, 50, 100}};
}

std::string batchExportName(const std::string& psdPath, const ExportSpec& s) {
  fs::path p = u8p(psdPath);
  std::string stem = u8s(p.stem());
  // the extension is the format; "Q" only on lossy files
  std::string suffix = " - S" + std::to_string(s.scale) + "%" + (s.lossy() ? " Q" + std::to_string(s.quality) : std::string());
  static const char* const ext[] = {".jpg", ".png", ".webp"};
  return u8s(p.parent_path() / u8p(stem + suffix + ext[std::clamp(s.format, 0, 2)]));
}

std::string batchSpecsToString(const std::vector<ExportSpec>& v) {
  std::string o;
  static const char* const fmt[] = {"jpg", "png", "webp"};
  for (const ExportSpec& s : v)
    o += std::string(fmt[std::clamp(s.format, 0, 2)]) + "," + std::to_string(s.scale) + "," + std::to_string(s.quality) + "," +
         (s.lossless ? "1" : "0") + ";";
  return o.empty() ? "none" : o;
}

std::vector<ExportSpec> batchSpecsFromString(const std::string& str) {
  std::vector<ExportSpec> v;
  std::stringstream in(str);
  std::string item;
  while (std::getline(in, item, ';')) {
    char fmt[8] = {};
    int sc = 100, q = 100, ll = 0;
    if (sscanf(item.c_str(), "%7[a-z],%d,%d,%d", fmt, &sc, &q, &ll) >= 3) {
      std::string f = fmt;
      int format = f == "png" ? ExportSpec::Png : f == "webp" ? ExportSpec::Webp : ExportSpec::Jpeg;
      v.push_back({format, std::clamp(sc, 1, 100), std::clamp(q, 1, 100), ll != 0});
    }
  }
  return v;
}

// The name a first save of a new document gets: "today - <typed name> - Version 1.psd" (a name that
// is already dated and versioned is kept). An existing file is never overwritten by the renaming.
std::string App::datedSavePath(const std::string& chosen) const {
  fs::path p = u8p(chosen);
  std::string date, name, stem = u8s(p.stem());
  int ver = 0;
  if (parseVersioned(stem, date, name, ver)) return chosen;
  return u8s(freeVersion(p.parent_path(), today(), stem, 1));
}

void App::versionSave() {
  if (!R.hasDocument() || saving || R.stroking()) return;
  if (documentPath.empty()) {  // never saved: ask for a name, the date and "Version 1" are added to it
    versionDialog = true;
    showDialog(DlgSave);
    return;
  }
  fs::path p = u8p(documentPath);
  std::string date, name;
  int ver = 0;
  if (!parseVersioned(u8s(p.stem()), date, name, ver)) {  // a file from before: its versions start today
    date = today();
    name = u8s(p.stem());
    ver = 0;
  }
  saveFile(u8s(freeVersion(p.parent_path(), date, name, ver + 1)));
}

void App::batchExport() {
  if (!R.hasDocument() || R.stroking()) return;
  if (saving) { showToast("Still saving - try again in a moment"); return; }
  if (batchSpecs.empty()) { showBatchSettings = true; return; }
  if (documentPath.empty() || (batchSaveFirst && modified())) {
    // the exports are named after (and match) the saved .psd: save first, then export
    pendingAction = PA_BatchExport;
    continueAfterSave = true;
    if (documentPath.empty()) showDialog(DlgSave);
    else saveFile(documentPath);
    return;
  }
  if (textEdit.active) commitText();
  if (xf.active) applyTransform();
  if (hoverPreviewOn || previewing) cancelPreviews();
  if (engine.active()) strokeEnd();
  if (R.stroking()) return;
  R.waitIdle();
  auto merged = std::make_shared<ImageRGBA>();
  merged->w = R.docW;
  merged->h = R.docH;
  std::string err;
  try {
    if (!R.readMergedPixels(merged->rgba, err)) { error("Batch export failed: " + err); return; }
  } catch (const std::bad_alloc&) {
    error("Not enough memory to export this document right now.");
    return;
  }
  // file names (two identical entries get " (2)")
  std::vector<std::pair<ExportSpec, std::string>> jobs;
  for (const ExportSpec& s : batchSpecs) {
    std::string n = batchExportName(documentPath, s);
    for (int k = 2; std::any_of(jobs.begin(), jobs.end(), [&](auto& j) { return j.second == n; }); ++k) {
      fs::path q = u8p(batchExportName(documentPath, s));
      n = u8s(q.parent_path() / u8p(u8s(q.stem()) + " (" + std::to_string(k) + ")" + u8s(q.extension())));
    }
    jobs.push_back({s, n});
  }
  // scaled copies first (from premultiplied pixels), then the full-size ones from the same buffer
  std::stable_sort(jobs.begin(), jobs.end(), [](auto& a, auto& b) { return a.first.scale < b.first.scale; });
  if (saveThread.joinable()) saveThread.join();
  saving = true;
  saveVerifying = false;
  saveProgress = 0;
  pathBeforeSave = documentPath;
  revisionAtSave = savedRevision;  // exporting does not count as saving the document
  saveThread = std::thread([this, merged, jobs] {
    std::string e, failed;
    int done = 0;
    bool unpremultiplied = false;
    try {
      for (size_t i = 0; i < jobs.size(); ++i) {
        const ExportSpec& s = jobs[i].first;
        std::string one;
        bool ok;
        if (s.scale < 100) {
          ImageRGBA small = scaleImage(*merged, uint32_t(std::lround(merged->w * s.scale / 100.0)),
                                       uint32_t(std::lround(merged->h * s.scale / 100.0)));
          unpremultiply(small.rgba);
          ok = exportImage(jobs[i].second, small, one, s.format == ExportSpec::Webp && s.lossless ? 0 : s.quality);
        } else {
          if (!unpremultiplied) { unpremultiply(merged->rgba); unpremultiplied = true; }
          ok = exportImage(jobs[i].second, *merged, one, s.format == ExportSpec::Webp && s.lossless ? 0 : s.quality);
        }
        if (ok) ++done;
        else failed += "\n" + one;
        saveProgress = float(i + 1) / float(jobs.size());
      }
    } catch (const std::bad_alloc&) {
      failed += "\nNot enough memory for the remaining files.";
    }
    std::lock_guard<std::mutex> lock(saveMutex);
    saveMessage = failed.empty() ? std::string() : "Batch export: " + std::to_string(done) + " of " + std::to_string(jobs.size()) +
                                                       " files written." + failed;
    saveOk = false;
    if (failed.empty()) saveToast = "Batch export: " + std::to_string(done) + " files written next to the .psd";
    saveDone = true;
    saving = false;
  });
}

void App::drawBatchSettings() {
  if (!showBatchSettings) return;
  float sc = ImGui::GetStyle().FontScaleDpi;
  ImGui::SetNextWindowSize(ImVec2(640 * sc, 460 * sc), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Batch Export settings", &showBatchSettings, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
  }
  bool changed = false;
  ImGui::TextWrapped("Batch Export writes these images next to the .psd file, all in one go. The list is kept for next time.");
  ImGui::Spacing();
  int remove = -1;
  if (ImGui::BeginTable("specs", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 90 * sc);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Quality", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("File name ends with", ImGuiTableColumnFlags_WidthFixed, 150 * sc);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 28 * sc);
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < batchSpecs.size(); ++i) {
      ExportSpec& s = batchSpecs[i];
      ImGui::PushID(int(i));
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-1);
      changed |= ImGui::Combo("##f", &s.format, "JPEG\0PNG\0WebP\0");
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-1);
      changed |= ImGui::SliderInt("##s", &s.scale, 1, 100, "%d %%", ImGuiSliderFlags_AlwaysClamp);
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-1);
      if (s.format == ExportSpec::Webp) {
        changed |= ImGui::Checkbox("Lossless", &s.lossless);
        if (!s.lossless) {
          ImGui::SameLine();
          ImGui::SetNextItemWidth(-1);
          changed |= ImGui::SliderInt("##q", &s.quality, 1, 100, "Q %d", ImGuiSliderFlags_AlwaysClamp);
        }
      } else if (s.format == ExportSpec::Jpeg) {
        changed |= ImGui::SliderInt("##q", &s.quality, 1, 100, "Q %d", ImGuiSliderFlags_AlwaysClamp);
      } else {
        ImGui::TextDisabled("lossless");
      }
      ImGui::TableNextColumn();
      std::string n = u8s(u8p(batchExportName("x.psd", s)).filename());
      ImGui::TextUnformatted(n.c_str() + 1);  // " - S50% Q95.jpg" without the "x"
      ImGui::TableNextColumn();
      if (ImGui::SmallButton("x")) remove = int(i);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this file from the batch");
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  if (remove >= 0) { batchSpecs.erase(batchSpecs.begin() + remove); changed = true; }
  if (ImGui::Button("+ Add file")) { batchSpecs.push_back(batchSpecs.empty() ? ExportSpec{} : batchSpecs.back()); changed = true; }
  ImGui::SameLine();
  if (ImGui::Button("Reset to default")) { batchSpecs = defaultBatchSpecs(); changed = true; }
  ImGui::Spacing();
  changed |= ImGui::Checkbox("Save the document first when it has unsaved changes", &batchSaveFirst);
  ImGui::Separator();
  // what pressing the button makes, with the real names
  std::string base = documentPath.empty() ? u8s(fs::path(u8p(today() + " - Untitled - Version 1.psd"))) : documentPath;
  ImGui::TextDisabled("Batch Export creates %zu file%s:", batchSpecs.size(), batchSpecs.size() == 1 ? "" : "s");
  for (const ExportSpec& s : batchSpecs) {
    ImGui::BulletText("%s", u8s(u8p(batchExportName(base, s)).filename()).c_str());
    double side = std::max(R.docW, R.docH) * s.scale / 100.0;
    if (s.format == ExportSpec::Webp && R.hasDocument() && side > kWebpMaxSide + 0.5) {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "  too big for WebP (max %u px) - use %d %% or less", kWebpMaxSide,
                         int(kWebpMaxSide * 100.0 / std::max(R.docW, R.docH)));
    }
  }
  ImGui::Spacing();
  bool can = R.hasDocument() && !saving && !R.stroking() && !batchSpecs.empty();
  ImGui::BeginDisabled(!can);
  if (ImGui::Button("Export now", ImVec2(140 * sc, 0))) batchExport();
  ImGui::EndDisabled();
  if (changed) saveSettings();
  ImGui::End();
}
