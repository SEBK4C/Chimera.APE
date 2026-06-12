#include "ttl.h"

#include <cstdio>
#include <sstream>

#include "util.h"

namespace chimera {

std::string doc_iri(const std::string& doc_id) { return "<chimera://doc/" + doc_id + ">"; }
std::string chunk_iri(const std::string& chunk_id) { return "<chimera://chunk/" + chunk_id + ">"; }
std::string entity_iri(const std::string& name) { return "<chimera://entity/" + slugify(name) + ">"; }
std::string tag_iri(const std::string& tag) { return "<chimera://tag/" + slugify(tag) + ">"; }

std::string rel_iri(const std::string& predicate) {
  // lower_snake_verb: fold anything else to '_'
  std::string p;
  p.reserve(predicate.size());
  for (unsigned char c : predicate) {
    if (std::isalnum(c)) p.push_back(static_cast<char>(std::tolower(c)));
    else if (!p.empty() && p.back() != '_') p.push_back('_');
  }
  while (!p.empty() && p.back() == '_') p.pop_back();
  if (p.empty()) p = "related_to";
  return "<chimera://rel/" + p + ">";
}

std::string ttl_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04X", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  return out;
}

std::string ttl_prologue() {
  return "@prefix ch: <chimera://ontology#> .\n"
         "@prefix xsd: <http://www.w3.org/2001/XMLSchema#> .\n\n";
}

std::string emit_doc(const DocTriples& d) {
  std::ostringstream o;
  o << doc_iri(d.doc_id) << "\n    a ch:Document ;\n";
  for (const auto& p : d.paths)
    o << "    ch:locatedAt \"" << ttl_escape(p) << "\" ;\n";
  o << "    ch:checksum \"" << d.doc_id << "\" ;\n";
  o << "    ch:bytes " << d.bytes << " ;\n";
  o << "    ch:mime \"" << ttl_escape(d.mime) << "\" ;\n";
  o << "    ch:ingestedAt \"" << d.ingested_at << "\"^^xsd:dateTime";
  for (int i = 1; i <= d.chunk_count; ++i)
    o << " ;\n    ch:hasChunk " << chunk_iri(chunk_id(d.doc_id, i));
  if (!d.superseded_by.empty())
    o << " ;\n    ch:supersededBy " << doc_iri(d.superseded_by);
  o << " .\n\n";
  return o.str();
}

std::string emit_chunk(const ChunkTriples& c) {
  std::ostringstream o;
  o << chunk_iri(c.chunk_id) << "\n    a ch:Chunk ;\n";
  o << "    ch:partOf " << doc_iri(c.doc_id) << " ;\n";
  o << "    ch:ordinal " << c.ordinal << " ;\n";
  o << "    ch:byteStart " << c.byte_start << " ;\n";
  o << "    ch:byteEnd " << c.byte_end << " ;\n";
  o << "    ch:text \"" << ttl_escape(c.text) << "\"";
  if (!c.summary.empty()) o << " ;\n    ch:summary \"" << ttl_escape(c.summary) << "\"";
  if (c.extraction_failed) o << " ;\n    ch:extractionFailed true";
  if (c.has_next)
    o << " ;\n    ch:nextChunk " << chunk_iri(chunk_id(c.doc_id, c.ordinal + 1));
  for (const auto& t : c.tags) {
    std::string slug = slugify(t);
    if (!slug.empty()) o << " ;\n    ch:taggedWith " << tag_iri(t);
  }
  for (const auto& e : c.entities) {
    std::string slug = slugify(e.name);
    if (!slug.empty()) o << " ;\n    ch:mentions " << entity_iri(e.name);
  }
  o << " .\n\n";

  // Entity declarations (idempotent re-assertion is fine in RDF).
  for (const auto& e : c.entities) {
    if (slugify(e.name).empty()) continue;
    o << entity_iri(e.name) << " a ch:Entity ; ch:label \"" << ttl_escape(e.name)
      << "\" ; ch:entityType \"" << ttl_escape(e.type) << "\" .\n";
  }
  // Tag declarations.
  for (const auto& t : c.tags) {
    if (slugify(t).empty()) continue;
    o << tag_iri(t) << " a ch:Tag ; ch:label \"" << ttl_escape(t) << "\" .\n";
  }

  // Relations: receipt node + direct triple (§4).
  int rel_n = 0;
  for (const auto& r : c.relations) {
    if (slugify(r.subject).empty() || slugify(r.object).empty()) continue;
    std::string node = "_:rel_" + c.chunk_id + "_" + std::to_string(rel_n++);
    // ':' is not valid in a blank node label; normalize.
    for (auto& ch2 : node)
      if (ch2 == ':') ch2 = '_';
    node = "_:" + node.substr(2);
    char conf[32];
    std::snprintf(conf, sizeof conf, "%.2f", r.confidence);
    o << node << " a ch:Relation ;\n"
      << "    ch:subject " << entity_iri(r.subject) << " ;\n"
      << "    ch:predicate " << rel_iri(r.predicate) << " ;\n"
      << "    ch:object " << entity_iri(r.object) << " ;\n"
      << "    ch:evidencedBy " << chunk_iri(c.chunk_id) << " ;\n"
      << "    ch:confidence " << conf << " .\n";
    o << entity_iri(r.subject) << " " << rel_iri(r.predicate) << " "
      << entity_iri(r.object) << " .\n";
  }
  o << "\n";
  return o.str();
}

}  // namespace chimera
