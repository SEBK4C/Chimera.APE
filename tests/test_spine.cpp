// Assert-based tests for the spine components. No framework: each CHECK
// prints context on failure and the process exits non-zero.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <fstream>
#include <string>

#include "chunk.h"
#include "hash.h"
#include "manifest.h"
#include "ttl.h"
#include "util.h"
#include "walk.h"

namespace fs = std::filesystem;
using namespace chimera;

static int g_failures = 0;
#define CHECK(cond)                                                   \
  do {                                                                \
    if (!(cond)) {                                                    \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                   \
    }                                                                 \
  } while (0)

static std::string tmpdir() {
  std::string d = (fs::temp_directory_path() / "chimera-test-XXXXXX").string();
  std::vector<char> buf(d.begin(), d.end());
  buf.push_back('\0');
  if (!mkdtemp(buf.data())) std::abort();
  return std::string(buf.data());
}

static void write_file(const std::string& p, const std::string& content) {
  fs::create_directories(fs::path(p).parent_path());
  std::ofstream(p, std::ios::binary) << content;
}

static void test_slug() {
  CHECK(slugify("Acme Corp") == "acme-corp");
  CHECK(slugify("  The   Internet!! ") == "the-internet");
  CHECK(slugify("foo_bar.baz") == "foo-bar-baz");
  CHECK(slugify("ALL-CAPS") == "all-caps");
  CHECK(slugify("") == "");
  CHECK(slugify("---") == "");
}

static void test_chunk_id() {
  CHECK(chunk_id("abcd1234abcd1234", 7) == "abcd1234abcd1234:0007");
  CHECK(chunk_id("x", 1234) == "x:1234");
}

static void test_triple_tap() {
  std::string d = tmpdir();
  // Small file: hashed whole.
  write_file(d + "/small.txt", "hello chimera");
  auto h1 = triple_tap(d + "/small.txt");
  CHECK(h1 && h1->size() == 16);
  // Determinism + content sensitivity.
  write_file(d + "/small2.txt", "hello chimera");
  CHECK(*triple_tap(d + "/small2.txt") == *h1);
  write_file(d + "/small3.txt", "hello chimerb");
  CHECK(*triple_tap(d + "/small3.txt") != *h1);

  // Large file (>192 KiB): middle edit must change the hash...
  std::string big(400 * 1024, 'a');
  write_file(d + "/big.txt", big);
  auto hb = triple_tap(d + "/big.txt");
  big[200 * 1024] = 'b';  // dead center: inside the middle tap
  write_file(d + "/big.txt", big);
  CHECK(*triple_tap(d + "/big.txt") != *hb);

  // ...but an edit in an unsampled region is (by design, §2.1) missed by the
  // sampled hash and caught by --paranoid.
  std::string sneaky(400 * 1024, 'a');
  write_file(d + "/sneaky.txt", sneaky);
  auto hs = triple_tap(d + "/sneaky.txt");
  auto hs_p = triple_tap(d + "/sneaky.txt", /*paranoid=*/true);
  sneaky[100 * 1024] = 'b';  // between head and middle taps
  write_file(d + "/sneaky.txt", sneaky);
  CHECK(*triple_tap(d + "/sneaky.txt") == *hs);                  // blind spot
  CHECK(*triple_tap(d + "/sneaky.txt", true) != *hs_p);          // caught
  CHECK(triple_tap(d + "/does-not-exist.txt") == std::nullopt);

  fs::remove_all(d);
}

static void test_vec_key() {
  CHECK(vec_key("abc:0001") != vec_key("abc:0002"));
  CHECK(vec_key("abc:0001") == vec_key("abc:0001"));
}

static void test_glob() {
  CHECK(glob_match("*.md", "readme.md"));
  CHECK(glob_match("*.md", "docs/readme.md"));      // component match
  CHECK(!glob_match("*.md", "readme.txt"));
  CHECK(glob_match("docs/*.md", "docs/readme.md"));
  CHECK(!glob_match("docs/*.md", "docs/sub/readme.md"));  // '*' stops at '/'
  CHECK(glob_match("docs/**", "docs/sub/readme.md"));
  CHECK(glob_match("?at.txt", "cat.txt"));
  CHECK(glob_match("node_modules", "a/node_modules/b"));  // bare name matches component
}

static void test_binary_sniff() {
  CHECK(looks_binary("ab\0cd", 5));
  CHECK(!looks_binary("plain ascii", 11));
  CHECK(!looks_binary("UTF-8: caf\xc3\xa9", 12));
  CHECK(looks_binary("bad \xff\xfe seq", 11));
}

static void test_walk() {
  std::string d = tmpdir();
  write_file(d + "/a.md", "# A\n\nhello");
  write_file(d + "/code/x.py", "def f():\n    pass\n");
  write_file(d + "/.git/config", "should be skipped");
  write_file(d + "/node_modules/y.js", "skip");
  write_file(d + "/.chimera/manifest.db", "skip");
  write_file(d + "/img.png", std::string("\x89PNG\0\0", 6));
  write_file(d + "/raw.bin", std::string("\0\1\2", 3));  // .bin ext blocked
  write_file(d + "/noext", std::string("text but\0null", 13));  // sniffed out
  write_file(d + "/logs/x.log", "log line");
  write_file(d + "/.chimeraignore", "*.log\n# comment\nsecret/\n");
  write_file(d + "/secret/s.txt", "hidden");

  std::vector<std::string> seen;
  std::map<std::string, std::string> modality;
  walk(d, {}, [&](const WalkEntry& e) {
    seen.push_back(e.rel_path);
    modality[e.rel_path] = e.modality;
  });
  std::sort(seen.begin(), seen.end());
  // img.png is admitted as image-modality media (multimodal pipeline);
  // raw.bin / noext stay excluded as unsniffable binaries.
  CHECK(seen.size() == 3);
  CHECK(seen[0] == "a.md");
  CHECK(seen[1] == "code/x.py");
  CHECK(seen[2] == "img.png");
  CHECK(modality["img.png"] == "image");
  CHECK(modality["a.md"] == "text");

  // include/exclude
  std::vector<std::string> only_py;
  WalkOptions wo;
  wo.include = {"*.py"};
  walk(d, wo, [&](const WalkEntry& e) { only_py.push_back(e.rel_path); });
  CHECK(only_py.size() == 1 && only_py[0] == "code/x.py");

  fs::remove_all(d);
}

static void test_chunker() {
  // Prose: paragraphs accumulate; a doc smaller than target = one chunk.
  auto one = chunk_text("# T\n\npara one\n\npara two\n", "text/markdown");
  CHECK(one.size() == 1);
  CHECK(one[0].ordinal == 1);
  CHECK(one[0].byte_start == 0);
  CHECK(one[0].byte_end == static_cast<int64_t>(std::string("# T\n\npara one\n\npara two\n").size()));

  // Big doc: tiling offsets, overlap in text, no mid-sentence cuts at seams.
  std::string big;
  for (int i = 0; i < 200; ++i)
    big += "Paragraph " + std::to_string(i) + " has several sentences. Here is another one. And a third one.\n\n";
  auto chunks = chunk_text(big, "text/markdown");
  CHECK(chunks.size() > 1);
  CHECK(chunks[0].byte_start == 0);
  for (size_t i = 1; i < chunks.size(); ++i) {
    CHECK(chunks[i].byte_start == chunks[i - 1].byte_end);  // tiling
    CHECK(chunks[i].ordinal == static_cast<int>(i) + 1);
  }
  CHECK(chunks.back().byte_end == static_cast<int64_t>(big.size()));
  // Overlap: chunk 2's text should be longer than its span.
  CHECK(static_cast<int64_t>(chunks[1].text.size()) >
        chunks[1].byte_end - chunks[1].byte_start);

  // Code: function boundaries respected (no chunk starts mid-function body).
  std::string code;
  for (int i = 0; i < 120; ++i)
    code += "def fn_" + std::to_string(i) + "():\n    x = 1\n    return x\n\n";
  auto cchunks = chunk_text(code, "text/x-python");
  CHECK(cchunks.size() > 1);
  for (const auto& c : cchunks) {
    CHECK(code.compare(static_cast<size_t>(c.byte_start), 4, "def ") == 0);
  }

  CHECK(chunk_text("", "text/plain").empty());
}

static void test_strip_html() {
  CHECK(strip_html("<p>Hello <b>world</b></p>").find("Hello") != std::string::npos);
  CHECK(strip_html("<script>evil()</script>visible").find("evil") == std::string::npos);
  CHECK(strip_html("<script>evil()</script>visible").find("visible") != std::string::npos);
  CHECK(strip_html("a &amp; b &lt;c&gt;").find("a & b <c>") != std::string::npos);
}

static void test_ttl() {
  CHECK(ttl_escape("a\"b\\c\nd") == "a\\\"b\\\\c\\nd");
  CHECK(rel_iri("Acquired By!") == "<chimera://rel/acquired_by>");
  CHECK(entity_iri("Acme Corp") == "<chimera://entity/acme-corp>");

  ChunkTriples ct;
  ct.chunk_id = "deadbeefdeadbeef:0001";
  ct.doc_id = "deadbeefdeadbeef";
  ct.ordinal = 1;
  ct.byte_start = 0;
  ct.byte_end = 10;
  ct.text = "say \"hi\"";
  ct.summary = "greeting";
  ct.tags = {"Greetings Stuff"};
  ct.entities = {{"Acme Corp", "org"}};
  ct.relations = {{"Acme Corp", "acquired", "WidgetCo", 0.93}};
  ct.has_next = true;
  std::string t = emit_chunk(ct);
  CHECK(t.find("ch:text \"say \\\"hi\\\"\"") != std::string::npos);
  CHECK(t.find("ch:taggedWith <chimera://tag/greetings-stuff>") != std::string::npos);
  CHECK(t.find("ch:mentions <chimera://entity/acme-corp>") != std::string::npos);
  CHECK(t.find("ch:nextChunk <chimera://chunk/deadbeefdeadbeef:0002>") != std::string::npos);
  CHECK(t.find("<chimera://entity/acme-corp> <chimera://rel/acquired> <chimera://entity/widgetco> .") != std::string::npos);
  CHECK(t.find("ch:evidencedBy <chimera://chunk/deadbeefdeadbeef:0001>") != std::string::npos);
  CHECK(t.find("ch:confidence 0.93") != std::string::npos);

  DocTriples dt;
  dt.doc_id = "deadbeefdeadbeef";
  dt.paths = {"a/b.md"};
  dt.bytes = 42;
  dt.mime = "text/markdown";
  dt.ingested_at = "2026-06-13T00:00:00Z";
  dt.chunk_count = 2;
  std::string dd = emit_doc(dt);
  CHECK(dd.find("ch:hasChunk <chimera://chunk/deadbeefdeadbeef:0001>") != std::string::npos);
  CHECK(dd.find("ch:hasChunk <chimera://chunk/deadbeefdeadbeef:0002>") != std::string::npos);
  CHECK(dd.find("ch:checksum \"deadbeefdeadbeef\"") != std::string::npos);
}

static void test_manifest() {
  std::string d = tmpdir();
  {
    Manifest m;
    m.open(d + "/manifest.db");
    CHECK(!m.path_info("x.md"));
    m.insert_doc("doc1", 100, "text/markdown");
    m.upsert_path("x.md", 111, 100, "doc1");
    auto pi = m.path_info("x.md");
    CHECK(pi && pi->doc_id == "doc1" && pi->mtime == 111 && pi->size == 100);
    CHECK(m.has_doc("doc1"));
    CHECK(!m.has_doc("doc2"));

    ChunkRow c;
    c.chunk_id = "doc1:0001";
    c.doc_id = "doc1";
    c.ordinal = 1;
    c.vec_key = vec_key(c.chunk_id);
    c.byte_start = 0;
    c.byte_end = 50;
    m.insert_chunk(c);
    auto back = m.chunk_by_vec_key(c.vec_key);
    CHECK(back && *back == "doc1:0001");
    CHECK(m.chunks_of("doc1").size() == 1);

    m.set_stage("doc1", Stage::kChunked);
    CHECK(m.stage_of("doc1") == static_cast<int>(Stage::kChunked));

    // Supersession + vacuum bookkeeping.
    m.insert_doc("doc2", 120, "text/markdown");
    m.supersede("doc1", "doc2");
    auto sup = m.superseded_docs();
    CHECK(sup.size() == 1 && sup[0] == "doc1");
    m.forget_doc("doc1");
    CHECK(m.superseded_docs().empty());
    CHECK(m.chunks_of("doc1").empty());

    m.set_meta("embedding_dim", "3840");
    CHECK(m.get_meta("embedding_dim") == std::optional<std::string>("3840"));

    auto sc = m.scoreboard();
    CHECK(sc.docs == 1);  // doc2 (doc1 superseded)
  }
  // Reopen: persistence.
  {
    Manifest m;
    m.open(d + "/manifest.db");
    CHECK(m.has_doc("doc2"));
  }
  fs::remove_all(d);
}

int main() {
  test_slug();
  test_chunk_id();
  test_triple_tap();
  test_vec_key();
  test_glob();
  test_binary_sniff();
  test_walk();
  test_chunker();
  test_strip_html();
  test_ttl();
  test_manifest();
  if (g_failures) {
    std::fprintf(stderr, "%d FAILURES\n", g_failures);
    return 1;
  }
  std::puts("all spine tests passed");
  return 0;
}
