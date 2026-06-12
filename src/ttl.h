// §4 — Turtle emission for the chimera ontology.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace chimera {

struct EntityRef {
  std::string name;  // surface form
  std::string type;  // person|org|place|concept|artifact|event
};

struct RelationRef {
  std::string subject;    // entity surface form
  std::string predicate;  // lower_snake_verb
  std::string object;     // entity surface form
  double confidence = 0.0;
};

struct ChunkTriples {
  std::string chunk_id;
  std::string doc_id;
  int ordinal = 1;
  int64_t byte_start = 0;
  int64_t byte_end = 0;
  std::string text;
  std::string summary;                 // empty until extraction
  bool extraction_failed = false;
  std::vector<std::string> tags;       // raw tag strings (slugified on emit)
  std::vector<EntityRef> entities;
  std::vector<RelationRef> relations;
  bool has_next = false;               // emit ch:nextChunk to ordinal+1
};

struct DocTriples {
  std::string doc_id;
  std::vector<std::string> paths;
  int64_t bytes = 0;
  std::string mime;
  std::string modality = "text";  // emitted as ch:modality when not text
  std::string ingested_at;  // ISO-8601
  int chunk_count = 0;
  std::string superseded_by;  // empty unless superseding
};

// IRI helpers (§2.2).
std::string doc_iri(const std::string& doc_id);
std::string chunk_iri(const std::string& chunk_id);
std::string entity_iri(const std::string& name);   // slugified
std::string tag_iri(const std::string& tag);       // slugified
std::string rel_iri(const std::string& predicate); // lower_snake checked

// Turtle string literal escaping (backslash, quote, control chars).
std::string ttl_escape(const std::string& s);

// The @prefix header every batch starts with.
std::string ttl_prologue();

// Serialize one document / chunk to Turtle. Entities also get
// `a ch:Entity ; ch:label` triples; relations emit both the ch:Relation
// receipt node and the direct triple (§4).
std::string emit_doc(const DocTriples& d);
std::string emit_chunk(const ChunkTriples& c);

}  // namespace chimera
