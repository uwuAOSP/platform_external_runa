// Copyright 2018 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "manifest_parser.h"

#include <assert.h>
#include <cerrno>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fnmatch.h>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

#include "disk_interface.h"
#include "eval_env.h"
#include "lexer.h"
#include "metrics.h"
#include "parallel_map.h"
#include "state.h"
#include "util.h"
#include "version.h"
#include "manifest_chunk_parser.h"

using manifest_chunk::Clump;
using manifest_chunk::DefaultTarget;
using manifest_chunk::Include;
using manifest_chunk::ParserItem;
using manifest_chunk::RequiredVersion;

extern char** environ;

namespace {

static bool DecorateError(const LoadedFile& file, size_t file_offset,
                          const std::string& message, std::string* err) {
  assert(file_offset <= file.content().size());
  return DecorateErrorWithLocation(file.filename(), file.content().data(),
                                   file_offset, message, err);
}

/// Split a single manifest file into chunks, parse the chunks in parallel, and
/// return the resulting parser output.
static std::vector<ParserItem> ParseManifestChunks(const LoadedFile& file,
                                                   ThreadPool* thread_pool,
                                                   bool experimentalEnvvar) {
  std::vector<ParserItem> result;
  const std::vector<StringPiece> chunk_views =
    manifest_chunk::SplitManifestIntoChunks(file.content_with_nul());

  METRIC_RECORD(".ninja load : parse chunks");

  for (std::vector<ParserItem>& chunk_items :
      ParallelMap(thread_pool, chunk_views, [&file, experimentalEnvvar](StringPiece view) {
    std::vector<ParserItem> chunk_items;
    manifest_chunk::ParseChunk(file, view, &chunk_items, experimentalEnvvar);
    return chunk_items;
  })) {
    std::move(chunk_items.begin(), chunk_items.end(),
              std::back_inserter(result));
  }
  return result;
}

struct UniSourceGlob {
  std::string pattern;
  std::vector<std::string> excludes;
  std::vector<std::string> previous_matches;
};

static bool DecodeBase64(const std::string& encoded, std::string* decoded) {
  static const std::string alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  uint32_t value = 0;
  int bits = -8;
  decoded->clear();
  for (unsigned char c : encoded) {
    size_t index = alphabet.find(c);
    if (index == std::string::npos) return false;
    value = (value << 6) | static_cast<int>(index);
    bits += 6;
    if (bits >= 0) {
      decoded->push_back(static_cast<char>((value >> bits) & 0xff));
      bits -= 8;
    }
  }
  return bits == -4 || bits == -6 || bits == -8;
}

static std::string GraphDeltaDigest(StringPiece content) {
  const uint64_t seeds[] = {
      1469598103934665603ULL, 1099511628211ULL,
      7809847782465536322ULL, 9650029242287828579ULL};
  std::string result;
  for (uint64_t seed : seeds) {
    uint64_t hash = seed;
    for (unsigned char byte : content) {
      hash ^= byte;
      hash *= 1099511628211ULL;
    }
    char encoded[17];
    snprintf(encoded, sizeof(encoded), "%016llx",
             static_cast<unsigned long long>(hash));
    result += encoded;
  }
  return result;
}

static void AppendGraphDigestField(std::string* data, const std::string& value) {
  *data += std::to_string(value.size()) + ":" + value + "\n";
}

static std::string GraphDeltaEdgeDigest(const Edge* edge) {
  std::string data;
  AppendGraphDigestField(&data, edge->rule_->name());
  auto append_nodes = [&data](const std::vector<Node*>& nodes) {
    AppendGraphDigestField(&data, std::to_string(nodes.size()));
    for (Node* node : nodes) AppendGraphDigestField(&data, node->globalPath().h.str_view().AsString());
  };
  append_nodes(edge->outputs_);
  append_nodes(edge->inputs_);
  append_nodes(edge->validations_);
  // The input vector stores explicit, implicit and order-only inputs in that
  // order. Keep the category counts in the canonical payload so a replace
  // cannot accidentally accept an edge with a different dependency class.
  AppendGraphDigestField(&data, std::to_string(edge->explicit_outs_));
  AppendGraphDigestField(&data, std::to_string(edge->implicit_outs_));
  AppendGraphDigestField(&data, std::to_string(edge->explicit_deps_));
  AppendGraphDigestField(&data, std::to_string(edge->implicit_deps_));
  AppendGraphDigestField(&data, std::to_string(edge->order_only_deps_));
  for (const auto& binding : edge->unevaled_bindings_) {
    AppendGraphDigestField(&data, binding.first.str());
    AppendGraphDigestField(&data, binding.second);
  }
  return GraphDeltaDigest(data);
}

static void ReserveSpaceInScopeTables(Scope* scope,
                                      const std::vector<ParserItem>& items) {
  // Reserve extra space in the scope's bindings/rules hash tables.
  METRIC_RECORD(".ninja load : alloc scope tables");
  size_t new_bindings = 0;
  size_t new_rules = 0;
  for (const auto& item : items) {
    if (item.kind == ParserItem::kClump) {
      Clump* clump = item.u.clump;
      new_bindings += clump->bindings_.size();
      new_rules += clump->rules_.size();
    }
  }
  scope->ReserveTableSpace(new_bindings, new_rules);
}

struct ManifestFileSet {
  ManifestFileSet(FileReader* file_reader) : file_reader_(file_reader) {}

  bool LoadFile(const std::string& filename, const LoadedFile** result,
                std::string* err);

  ~ManifestFileSet() {
    // It can take a surprising long time to unmap all the manifest files
    // (e.g. ~70 ms for Android, 25 ms for Chromium).
    METRIC_RECORD(".ninja load : unload files");
    loaded_files_.clear();
  }

  // TODO: use fork() so Getcwd() is no longer needed.
  bool Getcwd(std::string* out_path, std::string* err);
  bool Chdir(const std::string dir, std::string* err);
  std::vector<std::string> GetFileNames();
  std::string GraphDigest() const {
    if (loaded_files_.size() == 1)
      return GraphDeltaDigest(loaded_files_[0]->content());
    std::string data = "GD1-manifest-tree\n";
    for (const auto& file : loaded_files_)
      AppendGraphDigestField(&data, GraphDeltaDigest(file->content()));
    return GraphDeltaDigest(data);
  }

private:
  FileReader *file_reader_ = nullptr;

  /// Keep all the manifests in memory until all the manifest parsing work is
  /// complete, because intermediate parsing results (i.e. StringPiece) may
  /// point into the loaded files.
  std::vector<std::unique_ptr<LoadedFile>> loaded_files_;
};

bool ManifestFileSet::Getcwd(std::string* out_path, std::string* err) {
  return file_reader_->Getcwd(out_path, err);
}

bool ManifestFileSet::Chdir(const std::string dir, std::string* err) {
  return file_reader_->Chdir(dir, err);
}

bool ManifestFileSet::LoadFile(const std::string& filename,
                               const LoadedFile** result,
                               std::string* err) {
  std::unique_ptr<LoadedFile> loaded_file;
  if (file_reader_->LoadFile(filename, &loaded_file, err) != FileReader::Okay) {
    *err = "loading '" + filename + "': " + *err;
    return false;
  }
  loaded_files_.push_back(std::move(loaded_file));
  *result = loaded_files_.back().get();
  return true;
}

std::vector<std::string> ManifestFileSet::GetFileNames() {
  std::vector<std::string> result;
  result.reserve(loaded_files_.size());
  for (const auto& lf : loaded_files_) {
    result.push_back(lf->filename());
  }
  return result;
}

struct DfsParser {
  DfsParser(ManifestFileSet* file_set, State* state, ThreadPool* thread_pool, const ManifestParserOptions& options)
      : options_(options), file_set_(file_set), state_(state), thread_pool_(thread_pool) {}

private:
  void HandleRequiredVersion(const RequiredVersion& item, Scope* scope);
  bool HandleInclude(Include& item, const LoadedFile& file, Scope* scope,
                     const LoadedFile** child_file, Scope** child_scope,
                     std::string* err);
  bool LoadIncludeOrSubninja(Include& include, const LoadedFile& file,
                             Scope* scope, std::vector<Clump*>* out_clumps,
                             std::string* err);
  bool HandlePool(Pool* pool, Scope* scope, const LoadedFile& file, std::string* err);
  bool HandleClump(Clump* clump, const LoadedFile& file, Scope* scope,
                   std::string* err);

public:
  const ManifestParserOptions options_;
  /// Load the tree of manifest files and do initial parsing of all the chunks.
  /// This function always runs on the main thread.
  bool LoadManifestTree(const LoadedFile& file, Scope* scope,
                        std::vector<Clump*>* out_clumps, std::string* err);

private:
  ManifestFileSet* file_set_;
  State* state_;
  ThreadPool* thread_pool_;
};

void DfsParser::HandleRequiredVersion(const RequiredVersion& item,
                                      Scope* scope) {
  std::string version;
  EvaluateBindingInScope(&version, item.version_,
                         scope->GetCurrentEndOfScope());
  CheckNinjaVersion(version);
}

bool DfsParser::HandleInclude(Include& include, const LoadedFile& file,
                              Scope* scope, const LoadedFile** child_file,
                              Scope** child_scope, std::string* err) {
  std::string path;
  EvaluatePathInScope(&path, include.path_, scope->GetCurrentEndOfScope());
  if (!file_set_->LoadFile(path, child_file, err)) {
    return DecorateError(file, include.diag_pos_, std::string(*err), err);
  }
  if (!include.chdir_plus_slash_.empty()) {
    // Evaluate chdir expression and add '/' so the following is always valid:
    // chdir_plus_slash_ + node.path() == node.globalPath()
    include.chdir_plus_slash_.clear();
    EvaluatePathInScope(&include.chdir_plus_slash_, include.chdir_,
                        scope->GetCurrentEndOfScope());
    include.chdir_plus_slash_ += '/';
    include.chdir_.str_ = StringPiece(include.chdir_plus_slash_);

    // Create scope so that subninja chdir variable lookups cannot see parent_
    char** cmdEnviron = NULL;
    if (!include.envvar.empty()) {
      // Make a copy of environ and update or add envvar's.
      size_t len = 1;  // len of 1 counts the NULL terminator.
      for (char** p = environ; *p; p++) {
        len++;
      }
      len += include.envvar.size();
      cmdEnviron = new char*[len];
      char** dst = cmdEnviron;
      for (char** p = environ; *p; p++) {
        const char* eq = strchr(*p, '=');
        if (eq && eq > *p &&
            include.envvar.find(string(*p, eq - *p)) != include.envvar.end()) {
          // include.envvar overrides the var at *p. Skip *p.
          continue;
        }
        size_t varlen = strlen(*p);
        *dst = new char[varlen + 1];
        strncpy(*dst, *p, varlen);
        (*dst)[varlen] = 0;
        dst++;
      }
      for (auto i = include.envvar.begin(); i != include.envvar.end(); i++) {
        if (i->second.empty()) {
          // An empty variable always is fully unset.
          //
          // bash may complain (bash has an option, set -u, as a form of lint for this)
          // and ninja does not provide a facility for a variable set to "" (empty string)
          // since "" is used to signal to ninja the variable must be unset.
          continue;
        }
        string out = i->first + "=" + i->second;
        *dst = new char[out.size() + 1];
        strncpy(*dst, out.c_str(), out.size());
        (*dst)[out.size()] = 0;
        dst++;
      }
      *dst = NULL;  // terminate the new cmdEnviron with a NULL pointer
    }
    *child_scope = new Scope(scope, include.chdir_plus_slash_, cmdEnviron);
    (*child_scope)->AddAllBuiltinRules();
  } else if (include.new_scope_) {
    *child_scope = new Scope(scope->GetCurrentEndOfScope());
  } else {
    *child_scope = scope;
  }
  return true;
}

bool DfsParser::LoadIncludeOrSubninja(Include& include, const LoadedFile& file,
                                      Scope* scope,
                                      std::vector<Clump*>* out_clumps,
                                      std::string* err) {
  const LoadedFile* child_file = nullptr;
  Scope* child_scope = nullptr;
  std::string prev_cwd;

  if (!HandleInclude(include, file, scope, &child_file, &child_scope, err))
    return false;

  if (!include.chdir_plus_slash_.empty()) {
    // The subninja chdir must be treated the same as if the ninja
    // invocation were done solely inside the chdir. Save the current
    // working dir and chdir into the subninja.
    if (!file_set_->Getcwd(&prev_cwd, err)) {
      *err = "subninja chdir \"" + include.chdir_plus_slash_ + "\": " + *err;
      return false;
    }
    if (!file_set_->Chdir(include.chdir_plus_slash_, err)) {
      *err = "subninja chdir \"" + include.chdir_plus_slash_ + "\": " + *err;
      return false;
    }
  }

  if (!LoadManifestTree(*child_file, child_scope, out_clumps, err))
    return false;

  if (!include.chdir_plus_slash_.empty()) {
    // Restore the directory used by the parent of the subninja chdir.
    // TODO: fork() could be used, though fork() and pthreads do not mix.
    // but that would eliminate the need to restore the directory afterward.
    if (!file_set_->Chdir(prev_cwd, err)) {
      *err = "subninja chdir \"" + include.chdir_plus_slash_ + "\": restore " + prev_cwd + ": " + *err;
      return false;
    }

    // Still need to find all references to Nodes that this Scope owns. The
    // Node could not have known it was in this Scope until now.
    if (!out_clumps->empty()) {
      out_clumps->back()->owner_scope_.push_back(child_scope);
    }
  }
  return true;
}

bool DfsParser::HandlePool(Pool* pool, Scope* scope, const LoadedFile& file,
                           std::string* err) {
  std::string depth_string;
  EvaluateBindingInScope(&depth_string, pool->parse_state_.depth,
                         pool->pos_.scope_pos());
  pool->depth_ = atol(depth_string.c_str());
  if (pool->depth_ < 0) {
    return DecorateError(file, pool->parse_state_.depth_diag_pos,
                         "invalid pool depth", err);
  }
  if (!state_->AddPool(pool, scope)) {
    return DecorateError(file, pool->parse_state_.pool_name_diag_pos,
                         "duplicate pool '" + pool->name() + "'", err);
  }
  return true;
}

bool DfsParser::HandleClump(Clump* clump, const LoadedFile& file, Scope* scope,
                            std::string* err) {
  METRIC_RECORD(".ninja load : scope setup");
  // Allocate DFS and scope positions for the clump.
  clump->pos_.scope = scope->AllocDecls(clump->decl_count());
  clump->pos_.dfs_location = state_->AllocDfsLocation(clump->decl_count());
  {
    METRIC_RECORD(".ninja load : scope setup : bindings");
    for (Binding* binding : clump->bindings_) {
      scope->AddBinding(binding);
    }
  }
  {
    METRIC_RECORD(".ninja load : scope setup : rules");
    for (Rule* rule : clump->rules_) {
      if (!scope->AddRule(rule)) {
        return DecorateError(file, rule->parse_state_.rule_name_diag_pos,
                             "duplicate rule '" + rule->name() + "'", err);
      }
    }
  }
  {
    for (Edge* edge : clump->edges_) {
      edge->onPosResolvedToScope(clump->pos_.scope.scope);
    }
  }
  for (Pool* pool : clump->pools_) {
    if (!HandlePool(pool, scope, file, err)) {
      return false;
    }
  }
  return true;
}

bool DfsParser::LoadManifestTree(const LoadedFile& file, Scope* scope,
                                 std::vector<Clump*>* out_clumps,
                                 std::string* err) {
  std::vector<ParserItem> items = ParseManifestChunks(file, thread_pool_, options_.experimentalEnvvar);
  ReserveSpaceInScopeTables(scope, items);

  // With the chunks parsed, do a depth-first parse of the ninja manifest using
  // the results of the parallel parse.
  for (auto& item_nonconst : items) {
    const auto& item = item_nonconst;
    switch (item.kind) {
    case ParserItem::kError:
      *err = item.u.error->msg_;
      return false;

    case ParserItem::kRequiredVersion:
      HandleRequiredVersion(*item.u.required_version, scope);
      break;

    case ParserItem::kInclude:
      if (!LoadIncludeOrSubninja(*item_nonconst.u.include, file, scope,
                                 out_clumps, err))
        return false;
      break;

    case ParserItem::kClump:
      if (!HandleClump(item.u.clump, file, scope, err))
        return false;
      out_clumps->push_back(item.u.clump);
      break;

    default:
      assert(false && "unrecognized kind of ParserItem");
      abort();
    }
  }

  return true;
}

/// Parse an edge's path and add it to a vector on the Edge object.
static inline bool AddPathToEdge(State* state, const Edge& edge,
                                 std::vector<Node*>* out_vec,
                                 const LoadedFile& file, Lexer& lexer,
                                 std::string* err) {
  HashedStrView key;
  uint64_t slash_bits = 0;

  StringPiece canon_path = lexer.PeekCanonicalPath();
  if (!canon_path.empty()) {
    key = canon_path;
  } else {
    LexedPath path;
    if (!lexer.ReadPath(&path, err) || path.str_.empty()) {
      assert(false && "manifest file apparently changed during parsing");
      abort();
    }

    thread_local std::string tls_work_buf;
    std::string& work_buf = tls_work_buf;
    work_buf.clear();
    EvaluatePathOnEdge(&work_buf, path, edge);

    std::string path_err;
    if (!CanonicalizePath(&work_buf, &slash_bits, &path_err)) {
      return DecorateError(file, edge.parse_state_.final_diag_pos, path_err,
                           err);
    }
    key = work_buf;
  }

  Node* node = state->GetNode(edge.pos_.scope()->GlobalPath(key), 0);
  node->UpdateFirstReference(edge.dfs_location(), slash_bits);
  out_vec->push_back(node);
  return true;
}

static const HashedStrView kPool { "pool" };
static const HashedStrView kDeps { "deps" };
static const HashedStrView kDyndep { "dyndep" };
static const HashedStrView kTags { "tags" };

#include "manifest_source_globs.h"

// Prepare every replacement before changing the clump edge lists. No adjacency
// has been published at this point, so deleted edges cannot leave stale links.
static bool ApplyGraphDelta(State* state, const std::vector<Clump*>& clumps,
                            const std::vector<GraphDeltaOperation>& operations,
                            std::string* err) {
  if (operations.empty()) return true;
  auto fail = [&](const std::string& message) {
    *err = "graph delta: " + message;
    return false;
  };
  std::map<std::string, Edge*> producers;
  std::map<Edge*, Clump*> owners;
  for (Clump* clump : clumps) {
    for (Edge* edge : clump->edges_) {
      owners[edge] = clump;
      for (Node* node : edge->outputs_) {
        if (!producers.emplace(node->globalPath().h.str_view().AsString(), edge).second)
          return fail("ambiguous base output");
      }
    }
  }
  std::set<Edge*> removed;
  std::set<std::string> ids;
  std::vector<std::pair<Clump*, std::unique_ptr<Edge>>> prepared;
  for (const auto& op : operations) {
    if (op.output.empty() || !ids.insert(op.output).second)
      return fail("empty or repeated operation identity");
    auto old = producers.find(op.output);
    if (op.kind == GraphDeltaOperation::Add) {
      if (old != producers.end()) return fail("addition already exists: " + op.output);
    } else {
      if (old == producers.end() || !removed.insert(old->second).second)
        return fail("missing or repeatedly modified edge: " + op.output);
      if (!op.expected_digest.empty() && GraphDeltaEdgeDigest(old->second) != op.expected_digest)
        return fail("stale edge digest: " + op.output);
    }
    if (op.kind == GraphDeltaOperation::Remove) continue;
    auto context = producers.find(op.edge.context_output);
    if (context == producers.end()) return fail("missing context edge");
    Edge* anchor = context->second;
    std::unique_ptr<Edge> edge(new Edge());
    edge->pos_ = anchor->pos_;
    edge->onPosResolvedToScope(anchor->pos_.scope());
    edge->rule_ = Scope::LookupRuleAtPos(op.edge.rule, edge->pos_.scope_pos());
    if (!edge->rule_) return fail("unknown rule: " + op.edge.rule);
    std::set<std::string> keys;
    for (const auto& binding : op.edge.bindings) {
      if (binding.first.empty() || !keys.insert(binding.first).second)
        return fail("empty or repeated binding");
      edge->unevaled_bindings_.emplace_back(HashedStr(binding.first), binding.second);
    }
    auto paths = [&](const std::vector<std::string>& source, std::vector<Node*>* dest) {
      for (const auto& path : source) {
        std::string canonical = path;
        uint64_t bits = 0;
        if (path.empty() || !CanonicalizePath(&canonical, &bits, err)) return false;
        Node* node = state->GetNode(edge->pos_.scope()->GlobalPath(canonical), bits);
        node->UpdateFirstReference(edge->dfs_location(), bits);
        dest->push_back(node);
      }
      return true;
    };
    if (!paths(op.edge.outputs, &edge->outputs_) ||
        !paths(op.edge.implicit_outputs, &edge->outputs_) ||
        !paths(op.edge.inputs, &edge->inputs_) ||
        !paths(op.edge.implicit_inputs, &edge->inputs_) ||
        !paths(op.edge.order_only_inputs, &edge->inputs_) ||
        !paths(op.edge.validations, &edge->validations_)) return false;
    if (edge->outputs_.empty()) return fail("edge has no outputs");
    bool has_identity = false;
    for (Node* output : edge->outputs_)
      has_identity |= output->globalPath().h.str_view().AsString() == op.output;
    if (op.kind == GraphDeltaOperation::Add && !has_identity)
      return fail("edge payload does not contain its identity output");
    edge->explicit_outs_ = op.edge.outputs.size();
    edge->implicit_outs_ = op.edge.implicit_outputs.size();
    edge->explicit_deps_ = op.edge.inputs.size();
    edge->implicit_deps_ = op.edge.implicit_inputs.size();
    edge->order_only_deps_ = op.edge.order_only_inputs.size();
    edge->validation_deps_ = op.edge.validations.size();
    std::string pool;
    if (!edge->EvaluateVariable(&pool, kPool, edge->pos_.scope(), err, EdgeEval::kParseTime)) return false;
    edge->pool_ = pool.empty() ? &State::kDefaultPool :
        state->LookupPoolAtPos(pool, edge->pos_.scope_pos(), edge->dfs_location());
    if (!edge->pool_) return fail("unknown pool: " + pool);
    // Dyndep requires additional initialization; reject rather than silently
    // dropping dynamic dependencies until the wire protocol supports it.
    std::string dyndep;
    if (!edge->EvaluateVariable(&dyndep, kDyndep, edge->pos_.scope(), err)) return false;
    if (!dyndep.empty()) return fail("dyndep edges are not supported yet");
    if (!edge->EvaluateVariable(&edge->tags_, kTags, edge->pos_.scope(), err)) return false;
    prepared.emplace_back(owners[anchor], std::move(edge));
  }
  std::set<Node*> outputs;
  for (const auto& pair : owners) {
    if (removed.count(pair.first)) continue;
    outputs.insert(pair.first->outputs_.begin(), pair.first->outputs_.end());
  }
  for (const auto& pair : prepared)
    for (Node* output : pair.second->outputs_)
      if (!outputs.insert(output).second) return fail("multiple producers in resulting graph");
  for (Clump* clump : clumps) {
    auto& edges = clump->edges_;
    edges.erase(std::remove_if(edges.begin(), edges.end(),
                             [&](Edge* edge) { return removed.count(edge); }), edges.end());
  }
  for (auto& pair : prepared) pair.first->edges_.push_back(pair.second.release());
  return true;
}

static bool ReadGraphDeltaFile(const std::string& filename,
                               std::vector<GraphDeltaOperation>* operations,
                               const std::string& expected_digest,
                               std::string* err) {
  std::ifstream input(filename);
  if (!input) { *err = "cannot open graph delta file '" + filename + "'"; return false; }
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(input, line)) lines.push_back(line);
  size_t index = 0;
  auto read = [&](std::string* value) {
    if (index >= lines.size()) return false;
    *value = lines[index++]; return true;
  };
  auto count = [&](size_t* value) {
    std::string text;
    if (!read(&text) || text.empty()) return false;
    char* end = nullptr; unsigned long parsed = strtoul(text.c_str(), &end, 10);
    if (*end != '\0') return false; *value = parsed; return true;
  };
  auto field = [&](std::string* value) {
    std::string encoded;
    return read(&encoded) && DecodeBase64(encoded, value);
  };
  std::string version;
  std::string base_digest;
  size_t operation_count;
  if (!read(&version) || version != "GD1" || !read(&base_digest) ||
      base_digest.size() != 64 ||
      base_digest.find_first_not_of("0123456789abcdef") != std::string::npos ||
      !count(&operation_count)) {
    *err = "invalid graph delta header"; return false;
  }
  if (!expected_digest.empty() && base_digest != expected_digest) {
    *err = "stale graph delta: base manifest digest mismatch (delta " +
           base_digest + ", manifest " + expected_digest + ")";
    return false;
  }
  operations->clear(); operations->reserve(operation_count);
  for (size_t i = 0; i < operation_count; ++i) {
    std::string kind, output, expected_digest, context, rule;
    if (!read(&kind) || !field(&output) || !field(&expected_digest) ||
        !field(&context) || !field(&rule)) {
      *err = "invalid graph delta operation"; return false;
    }
    GraphDeltaOperation operation;
    operation.kind = kind == "add" ? GraphDeltaOperation::Add :
                     kind == "remove" ? GraphDeltaOperation::Remove :
                     kind == "replace" ? GraphDeltaOperation::Replace :
                     GraphDeltaOperation::Add;
    if (kind != "add" && kind != "remove" && kind != "replace") {
      *err = "invalid graph delta operation kind"; return false;
    }
    operation.output = output; operation.edge.context_output = context;
    operation.expected_digest = expected_digest;
    operation.edge.rule = rule;
    if (kind == "remove" && expected_digest.empty()) {
      *err = "remove graph delta operation has no expected digest"; return false;
    }
    if (kind == "remove") {
      operations->push_back(std::move(operation));
      continue;
    }
    auto list = [&](std::vector<std::string>* values) {
      size_t n; if (!count(&n)) return false; values->reserve(n);
      for (size_t j = 0; j < n; ++j) { std::string value; if (!field(&value)) return false; values->push_back(std::move(value)); }
      return true;
    };
    if (!list(&operation.edge.outputs) || !list(&operation.edge.implicit_outputs) ||
        !list(&operation.edge.inputs) || !list(&operation.edge.implicit_inputs) ||
        !list(&operation.edge.order_only_inputs) || !list(&operation.edge.validations)) {
      *err = "invalid graph delta edge paths"; return false;
    }
    size_t binding_count;
    if (!count(&binding_count)) { *err = "invalid graph delta bindings"; return false; }
    for (size_t j = 0; j < binding_count; ++j) {
      std::string key, value;
      if (!field(&key) || !field(&value)) { *err = "invalid graph delta binding"; return false; }
      value.push_back('\n');
      operation.edge.bindings.emplace_back(std::move(key), std::move(value));
    }
    operations->push_back(std::move(operation));
  }
  if (index != lines.size()) { *err = "trailing graph delta data"; return false; }
  return true;
}

struct ManifestLoader {
private:
  State* const state_ = nullptr;
  ThreadPool* const thread_pool_ = nullptr;
  ManifestParserOptions options_;
  const bool quiet_ = false;

  bool AddEdgeToGraph(Edge* edge, const LoadedFile& file, std::string* err);

  /// This function runs on a worker thread and adds a clump's declarations to
  /// the build graph.
  bool FinishAddingClumpToGraph(Clump* clump, std::string* err);

  bool FinishLoading(const std::vector<Clump*>& clumps, std::string* err);

public:
  ManifestLoader(State* state, ThreadPool* thread_pool,
                 ManifestParserOptions options, bool quiet)
      : state_(state), thread_pool_(thread_pool), options_(options),
        quiet_(quiet) {}

  bool Load(ManifestFileSet* file_set, const LoadedFile& root_manifest,
            std::string* err);
};

bool ManifestLoader::AddEdgeToGraph(Edge* edge, const LoadedFile& file,
                                    std::string* err) {

  const ScopePosition edge_pos = edge->pos_.scope_pos();

  // Look up the edge's rule.
  edge->rule_ = Scope::LookupRuleAtPos(edge->parse_state_.rule_name, edge_pos);
  if (edge->rule_ == nullptr) {
    std::string msg = "unknown build rule '" +
        edge->parse_state_.rule_name.AsString() + "'";
    return DecorateError(file, edge->parse_state_.rule_name_diag_pos, msg, err);
  }

  // Now that the edge's bindings are available, check whether the edge has a
  // pool. This check requires the full edge+rule evaluation system.
  std::string pool_name;
  if (!edge->EvaluateVariable(&pool_name, kPool, edge->pos_.scope(), err, EdgeEval::kParseTime))
    return false;
  if (pool_name.empty()) {
    edge->pool_ = &State::kDefaultPool;
  } else {
    edge->pool_ = state_->LookupPoolAtPos(pool_name, edge_pos, edge->pos_.dfs_location());
    if (edge->pool_ == nullptr) {
      return DecorateError(file, edge->parse_state_.final_diag_pos,
                           "unknown pool name '" + pool_name + "'", err);
    }
  }

  std::string tags;
  if (!edge->EvaluateVariable(&tags, kTags, edge->pos_.scope(), err, EdgeEval::kParseTime)) {
    return false;
  }
  if (!tags.empty()) {
    edge->tags_ = tags;
  }

  edge->outputs_.reserve(edge->explicit_outs_ + edge->implicit_outs_);
  edge->inputs_.reserve(edge->explicit_deps_ + edge->implicit_deps_ +
                        edge->order_only_deps_);
  edge->validations_.reserve(edge->validation_deps_);

  // Add the input and output nodes. We already lexed them in the first pass,
  // but we couldn't add them because scope bindings weren't available. To save
  // memory, the first pass only recorded the lexer position of each category
  // of input/output nodes, rather than each path's location.
  Lexer lexer(file.filename(), file.content(), file.content().data());
  for (const Edge::DeferredPathList& path_list :
      edge->parse_state_.deferred_path_lists) {
    std::vector<Node*>* vec =
        path_list.type == Edge::DeferredPathList::INPUT ? &edge->inputs_ :
        path_list.type == Edge::DeferredPathList::OUTPUT ? &edge->outputs_ :
        &edge->validations_;
    lexer.ResetPos(path_list.lexer_pos);
    for (int i = 0; i < path_list.count; ++i) {
      if (!AddPathToEdge(state_, *edge, vec, file, lexer, err))
        return false;
    }
    // Verify that there are no more paths to parse.
    LexedPath path;
    if (!lexer.ReadPath(&path, err) || !path.str_.empty()) {
      assert(false && "manifest file apparently changed during parsing");
      abort();
    }
  }

  // This compatibility mode filters nodes from the edge->inputs_ list; do it
  // before linking the edge inputs and nodes.
  if (options_.phony_cycle_action_ == kPhonyCycleActionWarn &&
      edge->maybe_phonycycle_diagnostic()) {
    // CMake 2.8.12.x and 3.0.x incorrectly write phony build statements that
    // reference themselves.  Ninja used to tolerate these in the build graph
    // but that has since been fixed.  Filter them out to support users of those
    // old CMake versions.
    Node* out = edge->outputs_[0];
    std::vector<Node*>::iterator new_end =
        std::remove(edge->inputs_.begin(), edge->inputs_.end(), out);
    if (new_end != edge->inputs_.end()) {
      edge->inputs_.erase(new_end, edge->inputs_.end());
      --edge->explicit_deps_;
      if (!quiet_) {
        Warning("phony target '%s' names itself as an input; ignoring "
                "[-w phonycycle=warn]", out->globalPath().h.data());
      }
    }
  }

  // Multiple outputs aren't (yet?) supported with depslog.
  std::string deps_type;
  if (!edge->EvaluateVariable(&deps_type, kDeps, edge->pos_.scope(), err, EdgeEval::kParseTime))
    return false;
  if (!deps_type.empty() && edge->outputs_.size() - edge->implicit_outs_ > 1) {
    return DecorateError(file, edge->parse_state_.final_diag_pos,
                         "multiple outputs aren't (yet?) supported by depslog; "
                         "bring this up on the mailing list if it affects you",
                         err);
  }

  // Lookup, validate, and save any dyndep binding.  It will be used later
  // to load generated dependency information dynamically, but it must
  // be one of our manifest-specified inputs.
  std::string dyndep;
  if (!edge->EvaluateVariable(&dyndep, kDyndep, edge->pos_.scope(), err, EdgeEval::kParseTime))
    return false;
  if (!dyndep.empty()) {
    uint64_t slash_bits;
    if (!CanonicalizePath(&dyndep, &slash_bits, err))
      return false;
    edge->dyndep_ = state_->GetNode(edge->pos_.scope()->GlobalPath(dyndep), 0);
    edge->dyndep_->set_dyndep_pending(true);
    vector<Node*>::iterator dgi =
      std::find(edge->inputs_.begin(), edge->inputs_.end(), edge->dyndep_);
    if (dgi == edge->inputs_.end()) {
      return DecorateError(file, edge->parse_state_.final_diag_pos,
                           "dyndep '" + dyndep + "' is not an input", err);
    }
  }

  return true;
}

bool ManifestLoader::FinishAddingClumpToGraph(Clump* clump, std::string* err) {
  std::string work_buf;

  // Precompute all binding values. Discard each evaluated string -- we just
  // need to make sure each binding's value isn't coming from the mmap'ed
  // manifest anymore.
  for (Binding* binding : clump->bindings_) {
    work_buf.clear();
    binding->Evaluate(&work_buf);
  }

  for (Edge* edge : clump->edges_) {
    if (!AddEdgeToGraph(edge, clump->file_, err))
      return false;
  }

  return true;
}

bool ManifestLoader::FinishLoading(const std::vector<Clump*>& clumps,
                                   std::string* err) {
  {
    // Most of this pass's time is spent adding the edges. (i.e. The time spent
    // evaluating the bindings is negligible.)
    METRIC_RECORD(".ninja load : edge setup");

    size_t output_count = 0;
    for (Clump* clump : clumps)
      output_count += clump->edge_output_count_;

    // Construct the initial graph of input/output nodes. Select an initial size
    // that's likely to keep the number of collisions low. The number of edges'
    // non-implicit outputs is a decent enough proxy for the final number of
    // nodes. (I see acceptable performance even with a much lower number of
    // buckets, e.g. 100 times fewer.)
    state_->paths_.reserve(state_->paths_.size() + output_count * 3);

    if (!PropagateError(err, ParallelMap(thread_pool_, clumps,
        [this](Clump* clump) {
      std::string err;
      FinishAddingClumpToGraph(clump, &err);
      return err;
    }))) {
      return false;
    }
  }
  if (options_.uni_source_globs) {
    METRIC_RECORD(".ninja load : expand source sets");
    if (!ApplyUniMappedSourceSets(state_, clumps, err)) return false;
  }
  if (!ApplyGraphDelta(state_, clumps, options_.graph_delta, err)) return false;
  if (!options_.graph_delta_file.empty()) {
    std::vector<GraphDeltaOperation> operations;
    if (!ReadGraphDeltaFile(options_.graph_delta_file, &operations,
                            options_.graph_delta_base_digest, err) ||
        !ApplyGraphDelta(state_, clumps, operations, err)) return false;
  }
  {
    // Record the in-edge for each node that's built by an edge. Detect
    // duplicate edges.
    //
    // With dupbuild=warn (the default until 1.9.0), when two edges generate the
    // same node, remove the duplicate node from the output list of the later
    // edge. If all of an edge's outputs are removed, remove the edge from the
    // graph.
    METRIC_RECORD(".ninja load : link edge outputs");
    for (Clump* clump : clumps) {
      for (size_t edge_idx = 0; edge_idx < clump->edges_.size(); ) {
        // Scan all Edge outputs and link them to the Node objects.
        Edge* edge = clump->edges_[edge_idx];
        for (size_t i = 0; i < edge->outputs_.size(); ) {
          Node* output = edge->outputs_[i];
          if (output->in_edge() == nullptr) {
            output->set_in_edge(edge);
            ++i;
            continue;
          }
          // Two edges produce the same output node.
          if (options_.dupe_edge_action_ == kDupeEdgeActionError) {
            return DecorateError(clump->file_,
                                 edge->parse_state_.final_diag_pos,
                                 "multiple rules generate " + output->globalPath().h.str_view().AsString() +
                                 " [-w dupbuild=err]", err);
          } else {
            if (!quiet_) {
              Warning("multiple rules generate %s. "
                      "builds involving this target will not be correct; "
                      "continuing anyway [-w dupbuild=warn]",
                      output->globalPath().h.data());
            }
            if (edge->is_implicit_out(i))
              --edge->implicit_outs_;
            else
              --edge->explicit_outs_;
            edge->outputs_.erase(edge->outputs_.begin() + i);
          }
        }
        if (edge->outputs_.empty()) {
          // All outputs of the edge are already created by other edges. Remove
          // this edge from the graph. This removal happens before the edge's
          // inputs are linked to nodes.
          clump->edges_.erase(clump->edges_.begin() + edge_idx);
          continue;
        }
        ++edge_idx;
      }
    }
  }
  {
    // Now that all invalid edges are removed from the graph, record an out-edge
    // on each node that's needed by an edge.
    METRIC_RECORD(".ninja load : link edge inputs");
    ParallelMap(thread_pool_, clumps, [](Clump* clump) {
      for (Edge* edge : clump->edges_) {
        for (Node* input : edge->inputs_) {
          input->AddOutEdge(edge);
        }
        for (Node* validation : edge->validations_) {
          validation->AddValidationOutEdge(edge);
        }
      }
    });
  }
  {
    // Find all references to Nodes that newly minted Scopes now own. The Node
    // could not have known it was in this Scope until now. Expressions inside
    // a chdir were converted to their globalPath() and now get converted back,
    // but it makes the global node hash table simple. Note globalPath() is
    // unchanged after this transformation.
    for (Clump* clump : clumps) {
      for (Scope* scope : clump->owner_scope_) {
        const std::string& chdir = scope->chdir();
        // ConcurrentHashMap has no erase() member - emulate it with 'cleaned'
        State::Paths cleaned(state_->paths_.size());
        bool wasCleaned = false;

        for (State::Paths::iterator i = state_->paths_.begin();
            i != state_->paths_.end(); ++i) {
          if (!i->first.str_view().AsString().compare(0, chdir.size(), chdir)) {
            Node* node = i->second;
            if (node->scope() != scope) {
              wasCleaned = true;
              node->resetScopeTo(scope);
            }
          }
          cleaned.insert(std::make_pair(i->second->globalPath().h, i->second));
        }
        if (wasCleaned)
          state_->paths_.swap(cleaned);
      }
    }
  }
  {
    METRIC_RECORD(".ninja load : default targets");

    for (Clump* clump : clumps) {
      for (DefaultTarget* target : clump->default_targets_) {
        std::string path;
        EvaluatePathInScope(&path, target->parsed_path_,
                            target->pos_.scope_pos());
        uint64_t slash_bits;  // Unused because this only does lookup.
        std::string path_err;
        if (!CanonicalizePath(&path, &slash_bits, &path_err))
          return DecorateError(clump->file_, target->diag_pos_, path_err, err);

        Node* node =
            state_->LookupNodeAtPos(target->pos_.scope()->GlobalPath(path),
                                    target->pos_.dfs_location());
        if (node == nullptr) {
          return DecorateError(clump->file_, target->diag_pos_,
                               "unknown target '" + path + "'", err);
        }
        // The .ninja file inside a 'subninja chdir' cannot leak its 'default'
        // into the parent .ninja file.
        if (node->scope()->chdir().empty()) {
          state_->AddDefault(node);
        }
      }
    }
  }
  {
    // Add the clump edges into the global edge vector, and assign edge IDs.
    // Edge IDs are used for custom protobuf-based Ninja frontends. An edge's ID
    // is equal to its index in the global edge vector, so delay the assignment
    // of edge IDs until we've removed duplicate edges above (dupbuild=warn).

    METRIC_RECORD(".ninja load : build edge table");

    // Copy edges to the global edge table.
    size_t old_size = state_->edges_.size();
    size_t new_size = old_size;
    for (Clump* clump : clumps) {
      new_size += clump->edges_.size();
    }
    state_->edges_.reserve(new_size);
    for (Clump* clump : clumps) {
      std::copy(clump->edges_.begin(), clump->edges_.end(),
                std::back_inserter(state_->edges_));
    }
    // Assign edge IDs.
    ParallelMap(thread_pool_, IntegralRange<size_t>(old_size, new_size),
        [this](size_t idx) {
      state_->edges_[idx]->id_ = idx;
    });
  }

  return true;
}

bool ManifestLoader::Load(ManifestFileSet* file_set,
                          const LoadedFile& root_manifest, std::string* err) {
  DfsParser dfs_parser(file_set, state_, thread_pool_, options_);
  std::vector<Clump*> clumps;
  if (!dfs_parser.LoadManifestTree(root_manifest, &state_->root_scope_, &clumps,
                                   err)) {
    return false;
  }
  if (!options_.graph_delta_file.empty())
    options_.graph_delta_base_digest = file_set->GraphDigest();
  return FinishLoading(clumps, err);
}

} // anonymous namespace

bool ManifestParser::Load(const string& filename, string* err) {
  METRIC_RECORD(".ninja load");

  ManifestFileSet file_set(file_reader_);
  const LoadedFile* file = nullptr;
  if (!file_set.LoadFile(filename, &file, err))
    return false;
  if (!options_.graph_delta_file.empty())
    options_.graph_delta_base_digest = GraphDeltaDigest(file->content());

  std::unique_ptr<ThreadPool> thread_pool = CreateThreadPool();
  ManifestLoader loader(state_, thread_pool.get(), options_, false);
  bool result = loader.Load(&file_set, *file, err);
  state_->manifest_files = file_set.GetFileNames();
  return result;
}

bool ManifestParser::ParseTest(const string& input, string* err) {
  ManifestFileSet file_set(file_reader_);
  std::unique_ptr<ThreadPool> thread_pool = CreateThreadPool();
  ManifestLoader loader(state_, thread_pool.get(), options_, true);
  return loader.Load(&file_set, HeapLoadedFile("input", input), err);
}
