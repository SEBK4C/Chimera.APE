// chimera — orchestrator CLI (§8). Parsing only; pipelines live in
// ingest.cpp / search.cpp behind pipeline.h.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "pipeline.h"

using namespace chimera;

namespace {

[[noreturn]] void usage(int code) {
  std::fprintf(stderr, R"(chimera — one binary, three organs, zero regrets

  chimera ingest <dir> [--db P] [--include G]... [--exclude G]... [--paranoid]
  chimera --search "Q" [--db P] [--k N] [--hops N] [--ctx-budget N] [--json]
  chimera --search-file img.png|clip.wav [--search "Q"] [--db P] [...]
  chimera verify  [--db P] [--fix] [--paranoid]
  chimera status  [--db P]
  chimera vacuum  [--db P]
  chimera sparql "..." [--db P]
  chimera <dir>            (same as: chimera ingest <dir>)

Global: --model PATH, --verbose, --version

Runtime override env vars (until zip-store payload extraction lands):
  CHIMERA_LLAMAFILE, CHIMERA_MODEL, CHIMERA_QLEVER_SERVER,
  CHIMERA_QLEVER_INDEX, CHIMERA_TURBOVEC
)");
  std::exit(code);
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  std::string cmd;
  std::vector<std::string> pos;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&](const char* flag) -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", flag);
        usage(2);
      }
      return argv[++i];
    };
    if (a == "--search") { cmd = "search"; o.query = need("--search"); }
    else if (a == "--search-file") { cmd = "search"; o.search_file = need("--search-file"); }
    else if (a == "--db") o.db = need("--db");
    else if (a == "--include") o.include.push_back(need("--include"));
    else if (a == "--exclude") o.exclude.push_back(need("--exclude"));
    else if (a == "--paranoid") o.paranoid = true;
    else if (a == "--fix") o.fix = true;
    else if (a == "--json") o.json = true;
    else if (a == "--k") o.k = std::atoi(need("--k").c_str());
    else if (a == "--hops") o.hops = std::atoi(need("--hops").c_str());
    else if (a == "--ctx-budget") o.ctx_budget = std::atoi(need("--ctx-budget").c_str());
    else if (a == "--model") o.model = need("--model");
    else if (a == "--verbose") o.verbose = true;
    else if (a == "--threads") (void)need("--threads");  // accepted; sequential v1
    else if (a == "--gpu") (void)need("--gpu");          // accepted; CPU v1
    else if (a == "--version") { std::puts("chimera 0.1.0-dev"); return 0; }
    else if (a == "--help" || a == "-h") usage(0);
    else if (!a.empty() && a[0] == '-') {
      std::fprintf(stderr, "unknown flag %s\n", a.c_str());
      usage(2);
    } else {
      pos.push_back(a);
    }
  }

  if (cmd.empty()) {
    if (pos.empty()) usage(2);
    if (pos[0] == "ingest" || pos[0] == "verify" || pos[0] == "status" ||
        pos[0] == "vacuum" || pos[0] == "sparql") {
      cmd = pos[0];
      if (pos.size() > 1) {
        if (cmd == "sparql") o.sparql_text = pos[1];
        else o.dir = pos[1];
      }
    } else {
      cmd = "ingest";  // positional sugar
      o.dir = pos[0];
    }
  } else if (!pos.empty()) {
    o.dir = pos[0];
  }

  try {
    if (cmd == "ingest") return run_ingest(o);
    if (cmd == "search") return run_search(o);
    if (cmd == "status") return run_status(o);
    if (cmd == "verify") return run_verify(o);
    if (cmd == "vacuum") return run_vacuum(o);
    if (cmd == "sparql") return run_sparql(o);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "chimera: %s\n", e.what());
    return 1;
  }
  usage(2);
}
