#include "extract.h"

#include <set>

#include "nlohmann/json.hpp"

#include "util.h"

using nlohmann::json;

namespace chimera {

const char* kExtractionSystemPrompt = R"(You extract structured metadata from a text chunk. Respond with JSON only — no prose, no markdown fences. Schema:
{
  "summary": "≤50 words",
  "tags": ["3-8 kebab-case topical tags"],
  "entities": [{"name": "string", "type": "person|org|place|concept|artifact|event"}],
  "relations": [{"subject": "string", "predicate": "lower_snake_verb", "object": "string", "confidence": 0.0}]
}
Every relation's subject and object MUST appear in entities. Extract only what the text states or directly implies — no outside knowledge. If a field has nothing, return it empty.)";

const char* kSynthesisSystemPrompt =
    R"(Answer the QUESTION using only the CONTEXT below. Cite every claim with [n] markers keyed to the numbered sources. If the context is insufficient, say so plainly and name what is missing — do not improvise. Be terse. No preamble.)";

namespace {

// Models love fencing JSON despite instructions; strip fences and any
// preamble up to the first '{'.
std::string unfence(const std::string& s) {
  size_t a = s.find('{');
  size_t b = s.rfind('}');
  if (a == std::string::npos || b == std::string::npos || b < a) return s;
  return s.substr(a, b - a + 1);
}

const std::set<std::string> kEntityTypes = {"person", "org",      "place",
                                            "concept", "artifact", "event"};

}  // namespace

std::optional<Extraction> parse_extraction(const std::string& json_text, std::string* err) {
  json j;
  try {
    j = json::parse(unfence(json_text));
  } catch (const std::exception& e) {
    if (err) *err = std::string("JSON parse error: ") + e.what();
    return std::nullopt;
  }
  if (!j.is_object()) {
    if (err) *err = "top level is not an object";
    return std::nullopt;
  }

  Extraction out;
  try {
    out.summary = j.value("summary", "");
    if (j.contains("tags"))
      for (const auto& t : j.at("tags"))
        if (t.is_string() && !slugify(t.get<std::string>()).empty())
          out.tags.push_back(t.get<std::string>());

    std::set<std::string> entity_slugs;
    if (j.contains("entities"))
      for (const auto& e : j.at("entities")) {
        if (!e.is_object() || !e.contains("name")) continue;
        EntityRef ref;
        ref.name = e.at("name").get<std::string>();
        ref.type = e.value("type", "concept");
        if (!kEntityTypes.count(ref.type)) ref.type = "concept";
        if (slugify(ref.name).empty()) continue;
        entity_slugs.insert(slugify(ref.name));
        out.entities.push_back(std::move(ref));
      }

    if (j.contains("relations"))
      for (const auto& r : j.at("relations")) {
        if (!r.is_object()) continue;
        RelationRef ref;
        ref.subject = r.value("subject", "");
        ref.predicate = r.value("predicate", "");
        ref.object = r.value("object", "");
        ref.confidence = r.value("confidence", 0.0);
        // Schema: endpoints must appear in entities. Prune violators.
        if (!entity_slugs.count(slugify(ref.subject)) ||
            !entity_slugs.count(slugify(ref.object)) || ref.predicate.empty())
          continue;
        out.relations.push_back(std::move(ref));
      }
  } catch (const std::exception& e) {
    if (err) *err = std::string("schema error: ") + e.what();
    return std::nullopt;
  }

  // ≤50 words is advisory; truncate hard at ~400 chars to keep literals sane.
  if (out.summary.size() > 400) out.summary.resize(400);
  if (out.tags.size() > 8) out.tags.resize(8);
  return out;
}

std::optional<Extraction> extract_chunk(LlamaClient& llama, const std::string& chunk_text) {
  // Determinism where it matters (§1): temperature ≤ 0.2.
  auto first = llama.chat(kExtractionSystemPrompt, chunk_text, 0.1, 2048);
  std::string err;
  if (first) {
    auto parsed = parse_extraction(*first, &err);
    if (parsed) return parsed;
  } else {
    err = "no response";
  }
  // One retry with the parse error appended (§5 stage 5).
  std::string retry_user =
      chunk_text + "\n\nYour previous response was invalid (" + err +
      "). Respond again with VALID JSON matching the schema exactly.";
  auto second = llama.chat(kExtractionSystemPrompt, retry_user, 0.1, 2048);
  if (second) {
    auto parsed = parse_extraction(*second, &err);
    if (parsed) return parsed;
  }
  return std::nullopt;
}

}  // namespace chimera
