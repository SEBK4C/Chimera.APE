#include "organs.h"

#include <unistd.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "nlohmann/json.hpp"

#include "http.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace chimera {

namespace {
std::string env_or(const char* name, const std::string& fallback) {
  const char* v = std::getenv(name);
  return v && *v ? v : fallback;
}
bool exists(const std::string& p) { return !p.empty() && fs::exists(p); }
}  // namespace

namespace {

// APE binaries are valid ZIP archives; cosmocc exposes embedded assets under
// the virtual /zip/ path (§1: payloads ride inside the binary). First run
// extracts them into db/runtime/, version-stamped via a marker file, so
// children can be exec'd (you cannot exec /zip/... directly).
void extract_zip_payloads(const std::string& db_dir) {
  const char* kVersion = "v0.1.0";  // bump to re-extract on upgrade
  std::string bin = db_dir + "/runtime/bin";
  std::string stamp = bin + "/.extracted-" + kVersion;
  if (fs::exists(stamp)) return;
  // zipalign junks directory prefixes, so payloads sit at the /zip root
  // under their well-known names.
  static const char* kOrgans[] = {"llamafile", "qlever-server", "qlever-index",
                                  "turbovec-server"};
  bool any = false;
  for (const char* name : kOrgans) any = any || fs::exists(std::string("/zip/") + name);
  if (!any) return;  // dev build: nothing embedded
  fs::create_directories(bin);
  for (const char* name : kOrgans) {
    std::string src = std::string("/zip/") + name;
    if (!fs::exists(src)) continue;
    std::string dst = bin + "/" + name;
    std::ifstream in(src, std::ios::binary);
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    out << in.rdbuf();
    out.close();
    fs::permissions(dst, fs::perms::owner_all | fs::perms::group_read |
                             fs::perms::others_read);
  }
  // Full flavor: weights (+ multimodal projector) embedded too (D3).
  std::string mod = db_dir + "/runtime/model";
  for (const auto& f : fs::directory_iterator("/zip")) {
    if (f.path().extension() != ".gguf") continue;
    bool is_mmproj = f.path().filename().string().rfind("mmproj", 0) == 0;
    std::string dst = mod + (is_mmproj ? "/mmproj.gguf" : "/model.gguf");
    if (fs::exists(dst)) continue;
    std::fprintf(stderr, "extracting embedded %s (one-time)...\n",
                 is_mmproj ? "projector" : "model weights (several GB)");
    fs::create_directories(mod);
    std::ifstream in(f.path(), std::ios::binary);
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    out << in.rdbuf();
  }
  std::ofstream(stamp) << kVersion;
}

}  // namespace

RuntimePaths RuntimePaths::resolve(const std::string& db_dir, const std::string& model_flag) {
  extract_zip_payloads(db_dir);
  RuntimePaths r;
  std::string bin = db_dir + "/runtime/bin/";
  std::string mod = db_dir + "/runtime/model/";
  r.llamafile = env_or("CHIMERA_LLAMAFILE", bin + "llamafile");
  r.qlever_server = env_or("CHIMERA_QLEVER_SERVER", bin + "qlever-server");
  r.qlever_index = env_or("CHIMERA_QLEVER_INDEX", bin + "qlever-index");
  r.turbovec = env_or("CHIMERA_TURBOVEC", bin + "turbovec-server");
  r.model = !model_flag.empty() ? model_flag
                                : env_or("CHIMERA_MODEL", mod + "model.gguf");
  // The projector enables image/audio understanding. Look beside the model
  // for the conventional mmproj-<model>.gguf, then the runtime dir.
  r.mmproj = env_or("CHIMERA_MMPROJ", "");
  if (r.mmproj.empty() && !r.model.empty()) {
    fs::path mp = fs::path(r.model).parent_path() /
                  ("mmproj-" + fs::path(r.model).filename().string());
    if (fs::exists(mp)) r.mmproj = mp.string();
  }
  if (r.mmproj.empty() && fs::exists(mod + "mmproj.gguf")) r.mmproj = mod + "mmproj.gguf";
  return r;
}

std::optional<std::string> RuntimePaths::missing() const {
  if (!exists(llamafile)) return "llamafile binary (" + llamafile + ")";
  if (!exists(model)) return "model weights (" + model + "); pass --model PATH";
  if (!exists(qlever_server)) return "qlever-server (" + qlever_server + ")";
  if (!exists(qlever_index)) return "qlever-index (" + qlever_index + ")";
  if (!exists(turbovec)) return "turbovec-server (" + turbovec + ")";
  return std::nullopt;
}

// ---- LlamaClient -----------------------------------------------------------

std::optional<std::vector<std::vector<float>>> LlamaClient::embed(
    const std::vector<std::string>& inputs) {
  json req{{"input", inputs}};
  auto resp = http_post(port_, "/v1/embeddings", req.dump(), "application/json",
                        /*timeout_ms=*/600000);
  if (!resp || resp->status != 200) return std::nullopt;
  std::vector<std::vector<float>> out(inputs.size());
  try {
    json j = json::parse(resp->body);
    for (const auto& item : j.at("data")) {
      size_t idx = item.at("index").get<size_t>();
      auto vec = item.at("embedding").get<std::vector<float>>();
      // Defensive renormalization: cosine math downstream assumes unit norm.
      double n2 = 0;
      for (float v : vec) n2 += double(v) * v;
      if (n2 > 0) {
        float inv = static_cast<float>(1.0 / std::sqrt(n2));
        for (float& v : vec) v *= inv;
      }
      if (idx < out.size()) out[idx] = std::move(vec);
    }
  } catch (const std::exception&) {
    return std::nullopt;
  }
  for (const auto& v : out)
    if (v.empty()) return std::nullopt;
  return out;
}

std::optional<std::string> LlamaClient::chat(const std::string& system,
                                             const std::string& user,
                                             double temperature, int max_tokens) {
  json req{{"messages",
            json::array({{{"role", "system"}, {"content", system}},
                         {{"role", "user"}, {"content", user}}})},
           {"temperature", temperature},
           {"max_tokens", max_tokens}};
  auto resp = http_post(port_, "/v1/chat/completions", req.dump(),
                        "application/json", /*timeout_ms=*/600000);
  if (!resp || resp->status != 200) return std::nullopt;
  try {
    json j = json::parse(resp->body);
    const auto& msg = j.at("choices").at(0).at("message");
    std::string content = msg.value("content", "");
    if (msg.contains("content") && msg.at("content").is_null()) content = "";
    // Gemma 4 emits its thinking into reasoning_content; if max_tokens runs
    // out mid-think, content is empty but the reasoning often already
    // contains the requested output — salvage it rather than failing.
    if (content.empty()) content = msg.value("reasoning_content", "");
    return content;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::optional<std::string> LlamaClient::chat_media(const std::string& user,
                                                   const std::string& media_b64,
                                                   const std::string& mime,
                                                   double temperature, int max_tokens) {
  json part;
  if (mime.rfind("image/", 0) == 0) {
    part = {{"type", "image_url"},
            {"image_url", {{"url", "data:" + mime + ";base64," + media_b64}}}};
  } else {
    std::string format = mime == "audio/mpeg" ? "mp3" : "wav";
    part = {{"type", "input_audio"},
            {"input_audio", {{"data", media_b64}, {"format", format}}}};
  }
  json req{{"messages", json::array({{{"role", "user"},
                                      {"content", json::array({
                                           {{"type", "text"}, {"text", user}},
                                           part,
                                       })}}})},
           {"temperature", temperature},
           {"max_tokens", max_tokens}};
  auto resp = http_post(port_, "/v1/chat/completions", req.dump(),
                        "application/json", /*timeout_ms=*/600000);
  if (!resp || resp->status != 200) return std::nullopt;
  try {
    json j = json::parse(resp->body);
    const auto& msg = j.at("choices").at(0).at("message");
    std::string content = msg.value("content", "");
    if (msg.contains("content") && msg.at("content").is_null()) content = "";
    if (content.empty()) content = msg.value("reasoning_content", "");
    return content;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::optional<std::vector<float>> LlamaClient::embed_media(const std::string& media_b64) {
  if (media_marker_.empty()) {
    auto props = http_get(port_, "/props");
    if (!props || props->status != 200) return std::nullopt;
    try {
      media_marker_ = json::parse(props->body).value("media_marker", "");
    } catch (const std::exception&) {
      return std::nullopt;
    }
    if (media_marker_.empty()) return std::nullopt;
  }
  json req{{"input", {{"prompt_string", media_marker_},
                      {"multimodal_data", json::array({media_b64})}}}};
  auto resp = http_post(port_, "/v1/embeddings", req.dump(), "application/json",
                        /*timeout_ms=*/600000);
  if (!resp || resp->status != 200) return std::nullopt;  // 501 on GPU backend
  try {
    json j = json::parse(resp->body);
    auto vec = j.at("data").at(0).at("embedding").get<std::vector<float>>();
    double n2 = 0;
    for (float v : vec) n2 += double(v) * v;
    if (n2 > 0) {
      float inv = static_cast<float>(1.0 / std::sqrt(n2));
      for (float& v : vec) v *= inv;
    }
    return vec;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

// ---- QleverClient ----------------------------------------------------------

std::optional<std::string> QleverClient::query(const std::string& sparql, int timeout_ms) {
  auto resp = http_post(port_, "/", sparql, "application/sparql-query", timeout_ms);
  if (!resp || resp->status != 200) return std::nullopt;
  return resp->body;
}

bool QleverClient::update(const std::string& sparql, const std::string& access_token) {
  std::string path = "/";
  if (!access_token.empty()) path += "?access-token=" + access_token;
  auto resp = http_post(port_, path, sparql, "application/sparql-update",
                        /*timeout_ms=*/600000);
  return resp && resp->status == 200;
}

// ---- TurboVecClient --------------------------------------------------------

bool TurboVecClient::upsert(const std::vector<uint64_t>& ids,
                            const std::vector<std::vector<float>>& vectors) {
  json req{{"ids", ids}, {"vectors", vectors}};
  auto resp = http_post(port_, "/upsert", req.dump());
  return resp && resp->status == 200;
}

std::optional<std::vector<Hit>> TurboVecClient::query(const std::vector<float>& vec, int k,
                                                      const std::vector<uint64_t>& allowlist) {
  json req{{"vectors", json::array({vec})}, {"k", k}};
  if (!allowlist.empty()) req["allowlist"] = allowlist;
  auto resp = http_post(port_, "/query", req.dump());
  if (!resp || resp->status != 200) return std::nullopt;
  try {
    json j = json::parse(resp->body);
    std::vector<Hit> hits;
    const auto& ids = j.at("ids").at(0);
    const auto& scores = j.at("scores").at(0);
    for (size_t i = 0; i < ids.size() && i < scores.size(); ++i) {
      Hit h;
      h.vec_key = ids[i].get<uint64_t>();
      h.score = scores[i].get<double>();
      hits.push_back(h);
    }
    return hits;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

bool TurboVecClient::remove(const std::vector<uint64_t>& ids) {
  json req{{"ids", ids}};
  auto resp = http_post(port_, "/remove", req.dump());
  return resp && resp->status == 200;
}

bool TurboVecClient::persist() {
  auto resp = http_post(port_, "/persist", "{}");
  return resp && resp->status == 200;
}

std::optional<std::pair<int, int64_t>> TurboVecClient::info() {
  auto resp = http_get(port_, "/info");
  if (!resp || resp->status != 200) return std::nullopt;
  try {
    json j = json::parse(resp->body);
    int dim = j.at("dim").is_null() ? 0 : j.at("dim").get<int>();
    return std::make_pair(dim, j.at("count").get<int64_t>());
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

// ---- Organs ----------------------------------------------------------------

LlamaClient* Organs::llama() {
  if (llama_client_) return llama_client_.get();
  int port = pick_port();
  fs::create_directories(db_dir_ + "/logs");
  // Flag set mirrors the tested gemma4.args from the llamafile-gemma repo:
  // one instance, chat + embeddings (--embeddings + --pooling mean).
  std::vector<std::string> argv{
      paths_.llamafile, "--server", "-m", paths_.model,
      "--embeddings", "--pooling", "mean", "-c", "8192", "-np", "2",
      "--host", "127.0.0.1", "--port", std::to_string(port)};
  if (!paths_.mmproj.empty()) {
    argv.push_back("--mmproj");
    argv.push_back(paths_.mmproj);
  }
  if (!llama_child_.spawn(argv, db_dir_ + "/logs/llamafile.log")) {
    error_ = "llamafile spawn: " + llama_child_.error();
    return nullptr;
  }
  // Model load is the slow path; 7 GB from cold cache can take minutes.
  if (!llama_child_.wait_healthy(port, "/health", /*timeout_ms=*/600000)) {
    error_ = "llamafile: " + llama_child_.error();
    return nullptr;
  }
  llama_client_ = std::make_unique<LlamaClient>(port);
  return llama_client_.get();
}

bool Organs::qlever_index_exists() const {
  return fs::exists(db_dir_ + "/qlever/chimera.index.pso");
}

bool Organs::build_qlever_index(const std::string& ttl_path, std::string* err) {
  fs::create_directories(db_dir_ + "/qlever");
  fs::create_directories(db_dir_ + "/logs");
  // Run qlever-index as a one-shot child (not health-checked; just wait).
  Child c;
  std::vector<std::string> argv{
      paths_.qlever_index, "-i", db_dir_ + "/qlever/chimera",
      "-F", "ttl", "-f", ttl_path, "-W", "-S", "bm25"};
  if (!c.spawn(argv, db_dir_ + "/logs/qlever-index.log")) {
    if (err) *err = c.error();
    return false;
  }
  // qlever-index runs to completion; poll until it exits.
  while (c.running())
    ::usleep(100 * 1000);
  // Exit status is consumed inside Child; success = index files exist.
  if (!qlever_index_exists()) {
    if (err) *err = "index build failed; log tail:\n" + c.log_tail();
    return false;
  }
  return true;
}

QleverClient* Organs::qlever() {
  if (qlever_client_) return qlever_client_.get();
  if (!qlever_index_exists()) {
    error_ = "no QLever index yet (ingest first)";
    return nullptr;
  }
  int port = pick_port();
  fs::create_directories(db_dir_ + "/logs");
  std::vector<std::string> argv{
      paths_.qlever_server, "-i", db_dir_ + "/qlever/chimera",
      "-p", std::to_string(port), "-t", "--persist-updates", "-n"};
  if (!qlever_child_.spawn(argv, db_dir_ + "/logs/qlever-server.log")) {
    error_ = "qlever spawn: " + qlever_child_.error();
    return nullptr;
  }
  if (!qlever_child_.wait_healthy(port, "/ping", /*timeout_ms=*/120000)) {
    error_ = "qlever: " + qlever_child_.error();
    return nullptr;
  }
  qlever_client_ = std::make_unique<QleverClient>(port);
  return qlever_client_.get();
}

void Organs::stop_qlever() {
  qlever_client_.reset();
  qlever_child_.stop();
}

TurboVecClient* Organs::turbovec() {
  if (turbovec_client_) return turbovec_client_.get();
  // Known issue: under the cosmo APE build, rayon's thread parking degrades
  // to busy-spinning and an idle pool can eat every core (observed 2174%
  // CPU). Cap the pool; encoding batches our size don't miss the threads.
  ::setenv("RAYON_NUM_THREADS", "2", /*overwrite=*/0);
  int port = pick_port();
  fs::create_directories(db_dir_ + "/turbovec");
  fs::create_directories(db_dir_ + "/logs");
  std::vector<std::string> argv{
      paths_.turbovec, "--port", std::to_string(port),
      "--path", db_dir_ + "/turbovec/index.tvim"};
  if (!turbovec_child_.spawn(argv, db_dir_ + "/logs/turbovec.log")) {
    error_ = "turbovec spawn: " + turbovec_child_.error();
    return nullptr;
  }
  if (!turbovec_child_.wait_healthy(port, "/health", /*timeout_ms=*/30000)) {
    error_ = "turbovec: " + turbovec_child_.error();
    return nullptr;
  }
  turbovec_client_ = std::make_unique<TurboVecClient>(port);
  return turbovec_client_.get();
}

void Organs::stop_all() {
  // Vectors persist on shutdown via the server's TERM-time persist? No —
  // turbovec-server persists on /persist and /shutdown only; callers must
  // persist after writes. Stop order: model first (largest), then stores.
  llama_client_.reset();
  llama_child_.stop();
  if (turbovec_client_) http_post(turbovec_client_->port(), "/persist", "{}");
  turbovec_client_.reset();
  turbovec_child_.stop();
  qlever_client_.reset();
  qlever_child_.stop();
}

}  // namespace chimera
