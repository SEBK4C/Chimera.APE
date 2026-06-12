// §5 Stage 5 — metadata extraction via Gemma (Appendix A), with strict JSON
// validation, one retry carrying the parse error, then ch:extractionFailed.
#pragma once

#include <optional>
#include <string>

#include "organs.h"
#include "ttl.h"

namespace chimera {

struct Extraction {
  std::string summary;
  std::vector<std::string> tags;
  std::vector<EntityRef> entities;
  std::vector<RelationRef> relations;
};

// Parses + validates the model's JSON against the Appendix A schema.
// Relations whose subject/object are not in `entities` are dropped (the
// schema demands they appear; we enforce by pruning rather than failing the
// whole chunk). Returns nullopt with a reason in *err on structural failure.
std::optional<Extraction> parse_extraction(const std::string& json_text, std::string* err);

// Full call: prompt → llama → validate → (retry once) → result.
// nullopt = both attempts failed; caller marks ch:extractionFailed.
std::optional<Extraction> extract_chunk(LlamaClient& llama, const std::string& chunk_text);

extern const char* kExtractionSystemPrompt;  // Appendix A
extern const char* kSynthesisSystemPrompt;   // Appendix B

}  // namespace chimera
