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

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fnmatch.h>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

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
  int value = 0;
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

static bool ParseCount(const std::vector<std::string>& lines, size_t* index,
                       size_t* count) {
  if (*index >= lines.size() || lines[*index].empty()) return false;
  char* end = nullptr;
  unsigned long value = strtoul(lines[*index].c_str(), &end, 10);
  if (!end || *end != '\0') return false;
  ++*index;
  *count = static_cast<size_t>(value);
  return true;
}

static bool DecodeUniSourceGlobs(const std::string& encoded,
                                 std::vector<UniSourceGlob>* globs) {
  std::string data;
  if (!DecodeBase64(encoded, &data)) return false;
  std::vector<std::string> lines;
  size_t start = 0;
  while (start < data.size()) {
    size_t end = data.find('\n', start);
    if (end == std::string::npos) return false;
    lines.push_back(data.substr(start, end - start));
    start = end + 1;
  }
  if (lines.size() < 2 || lines[0] != "G1") return false;
  size_t index = 1;
  size_t glob_count = 0;
  if (!ParseCount(lines, &index, &glob_count)) return false;
  globs->clear();
  globs->reserve(glob_count);
  for (size_t i = 0; i < glob_count; ++i) {
    if (index >= lines.size()) return false;
    UniSourceGlob glob;
    glob.pattern = lines[index++];
    size_t count = 0;
    if (glob.pattern.empty() || !ParseCount(lines, &index, &count) ||
        index + count > lines.size()) return false;
    glob.excludes.insert(glob.excludes.end(), lines.begin() + index,
                         lines.begin() + index + count);
    index += count;
    if (!ParseCount(lines, &index, &count) || index + count > lines.size())
      return false;
    glob.previous_matches.insert(glob.previous_matches.end(),
                                 lines.begin() + index,
                                 lines.begin() + index + count);
    index += count;
    globs->push_back(std::move(glob));
  }
  return index == lines.size();
}

static std::vector<std::string> PathSegments(const std::string& path) {
  std::vector<std::string> segments;
  size_t start = 0;
  while (start <= path.size()) {
    size_t end = path.find('/', start);
    if (end == std::string::npos) end = path.size();
    if (end > start) segments.push_back(path.substr(start, end - start));
    if (end == path.size()) break;
    start = end + 1;
  }
  return segments;
}

static bool MatchGlobSegments(const std::vector<std::string>& pattern,
                              const std::vector<std::string>& path,
                              size_t pattern_index, size_t path_index) {
  if (pattern_index == pattern.size()) return path_index == path.size();
  if (pattern[pattern_index] == "**") {
    for (size_t i = path_index; i <= path.size(); ++i) {
      if (MatchGlobSegments(pattern, path, pattern_index + 1, i)) return true;
    }
    return false;
  }
  return path_index < path.size() &&
         fnmatch(pattern[pattern_index].c_str(), path[path_index].c_str(),
                 FNM_PERIOD) == 0 &&
         MatchGlobSegments(pattern, path, pattern_index + 1, path_index + 1);
}

static bool MatchGlobPath(const std::string& pattern, const std::string& path) {
  return MatchGlobSegments(PathSegments(pattern), PathSegments(path), 0, 0);
}

static bool HasGlobCharacters(const std::string& segment) {
  return segment.find_first_of("*?[") != std::string::npos;
}

static bool ExpandUniSourceGlob(const UniSourceGlob& glob,
                                std::vector<std::string>* matches,
                                std::string* err) {
  const std::vector<std::string> segments = PathSegments(glob.pattern);
  size_t wildcard = 0;
  while (wildcard < segments.size() && !HasGlobCharacters(segments[wildcard]))
    ++wildcard;
  if (wildcard == segments.size()) {
    *err = "Uni source glob has no wildcard: " + glob.pattern;
    return false;
  }
  std::filesystem::path base;
  for (size_t i = 0; i < wildcard; ++i) base /= segments[i];
  if (base.empty()) base = ".";
  std::error_code ec;
  if (!std::filesystem::is_directory(base, ec)) {
    if (ec && ec != std::errc::no_such_file_or_directory) {
      *err = "cannot inspect source glob directory '" + base.string() +
             "': " + ec.message();
      return false;
    }
    matches->clear();
    return true;
  }

  matches->clear();
  const auto options = std::filesystem::directory_options::skip_permission_denied |
                       std::filesystem::directory_options::follow_directory_symlink;
  std::filesystem::recursive_directory_iterator it(base, options, ec), end;
  if (ec) {
    *err = "cannot enumerate source glob directory '" + base.string() +
           "': " + ec.message();
    return false;
  }
  for (; it != end; it.increment(ec)) {
    if (ec) {
      *err = "cannot enumerate source glob directory '" + base.string() +
             "': " + ec.message();
      return false;
    }
    std::error_code status_error;
    if (!it->is_regular_file(status_error) || status_error) continue;
    std::string path = it->path().lexically_normal().generic_string();
    if (path.rfind("./", 0) == 0) path.erase(0, 2);
    if (!MatchGlobPath(glob.pattern, path)) continue;

    // Soong's source glob exclusions are glob patterns too (including
    // recursive patterns such as **/excluded/**/*.java), not just a set of
    // paths that matched during graph generation. Reapply the same pattern
    // semantics against the live filesystem so newly-added excluded files do
    // not leak into the refreshed edge.
    bool excluded = false;
    for (const std::string& exclude : glob.excludes) {
      if (MatchGlobPath(exclude, path)) {
        excluded = true;
        break;
      }
    }
    if (!excluded) matches->push_back(std::move(path));
  }
  std::sort(matches->begin(), matches->end());
  matches->erase(std::unique(matches->begin(), matches->end()), matches->end());
  return true;
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
static const HashedStrView kRunaSourceGlobs { "runa_source_globs" };
static const HashedStrView kRunaDynamicCpp { "runa_dynamic_cpp" };
static const HashedStrView kRunaDynamicAaptCompile { "runa_dynamic_aapt_compile" };
static const HashedStrView kRunaDynamicAaptInputs { "runa_dynamic_aapt_inputs" };

struct UniCppGroup {
  std::string pattern;
  std::vector<std::string> excludes;
  std::vector<std::pair<std::string, std::string>> previous;
};

struct UniAaptGroup {
  std::string pattern;
  std::vector<std::string> excludes;
  std::string resource_root;
  std::string output_root;
  std::vector<std::pair<std::string, std::string>> previous;
};

static std::string ReplaceAll(std::string value, const std::string& from,
                              const std::string& to);

static bool DecodeUniAaptGroups(const std::string& encoded,
                                std::vector<UniAaptGroup>* groups) {
  std::string data;
  if (!DecodeBase64(encoded, &data)) return false;
  std::vector<std::string> lines;
  size_t start = 0;
  while (start < data.size()) {
    size_t end = data.find('\n', start);
    if (end == std::string::npos) return false;
    lines.push_back(data.substr(start, end - start));
    start = end + 1;
  }
  if (lines.size() < 2 || lines[0] != "A1") return false;
  size_t index = 1;
  size_t group_count = 0;
  if (!ParseCount(lines, &index, &group_count)) return false;
  groups->clear();
  groups->reserve(group_count);
  for (size_t i = 0; i < group_count; ++i) {
    if (index + 2 >= lines.size()) return false;
    UniAaptGroup group;
    group.pattern = lines[index++];
    size_t count = 0;
    if (group.pattern.empty() || !ParseCount(lines, &index, &count) ||
        index + count + 2 > lines.size()) return false;
    group.excludes.insert(group.excludes.end(), lines.begin() + index,
                          lines.begin() + index + count);
    index += count;
    group.resource_root = lines[index++];
    group.output_root = lines[index++];
    if (group.resource_root.empty() || group.output_root.empty() ||
        !ParseCount(lines, &index, &count) || index + count * 2 > lines.size())
      return false;
    for (size_t j = 0; j < count; ++j) {
      group.previous.emplace_back(lines[index], lines[index + 1]);
      index += 2;
    }
    groups->push_back(std::move(group));
  }
  return index == lines.size();
}

static bool DeriveAaptFlatPath(const std::string& source,
                               const std::string& resource_root,
                               const std::string& output_root,
                               std::string* output) {
  const std::filesystem::path relative =
      std::filesystem::path(source).lexically_relative(resource_root);
  if (relative.empty() || relative.is_absolute() ||
      *relative.begin() == "..") return false;
  std::vector<std::string> components;
  for (const auto& component : relative) components.push_back(component.string());
  if (components.empty()) return false;
  const std::filesystem::path source_path(source);
  std::string extension = source_path.extension().string();
  if (components.size() == 1) {
    // pathToAapt2Path treats the resource root itself as the output's
    // parent directory for files directly inside the root. The resource
    // directory name is prepended to the flat filename (for example,
    // res-export/.README.md -> res-export_.README.md.flat).
    const std::string stem = source_path.stem().string();
    std::filesystem::path output_path(output_root);
    output_path = output_path.parent_path();
    output_path /= std::filesystem::path(resource_root).filename().string() +
                   "_" + stem + extension + ".flat";
    *output = output_path.lexically_normal().string();
    return true;
  }
  const std::string filename = components.back();
  components.pop_back();
  std::string flag;
  if (components.back().rfind("flag", 0) == 0) {
    flag = "." + components.back().substr(4);
    components.pop_back();
    if (components.empty()) return false;
  }
  const std::string parent = components.back();
  components.pop_back();
  std::filesystem::path output_path(output_root);
  for (const std::string& component : components) output_path /= component;
  if (parent.rfind("values", 0) == 0 && extension == ".xml") extension = ".arsc";
  const std::string stem = source_path.stem().string();
  output_path /= parent + "_" + stem + flag + extension + ".flat";
  *output = output_path.lexically_normal().string();
  return true;
}

static Edge* CloneAaptCompileEdge(State* state, Edge* source_edge,
                                  const std::string& old_source,
                                  const std::string& new_source,
                                  const std::string& new_output,
                                  std::string* err) {
  std::unique_ptr<Edge> clone(new Edge());
  clone->pos_ = source_edge->pos_;
  clone->onPosResolvedToScope(source_edge->pos_.scope());
  clone->rule_ = source_edge->rule_;
  clone->pool_ = source_edge->pool_;
  clone->parse_state_ = source_edge->parse_state_;
  clone->tags_ = source_edge->tags_;
  clone->explicit_deps_ = 1;
  clone->implicit_deps_ = source_edge->implicit_deps_;
  clone->order_only_deps_ = source_edge->order_only_deps_;
  clone->explicit_outs_ = 1;
  clone->implicit_outs_ = 0;
  clone->validation_deps_ = source_edge->validation_deps_;
  clone->validations_ = source_edge->validations_;
  clone->dyndep_ = source_edge->dyndep_;
  clone->unevaled_bindings_ = source_edge->unevaled_bindings_;

  std::string source_path = new_source;
  uint64_t slash_bits = 0;
  std::string path_error;
  if (!CanonicalizePath(&source_path, &slash_bits, &path_error)) {
    *err = path_error;
    return nullptr;
  }
  Node* source_node = state->GetNode(clone->pos_.scope()->GlobalPath(source_path),
                                     slash_bits);
  source_node->UpdateFirstReference(clone->dfs_location(), slash_bits);
  clone->inputs_.push_back(source_node);
  for (size_t i = source_edge->explicit_deps_;
       i < source_edge->inputs_.size(); ++i) {
    clone->inputs_.push_back(source_edge->inputs_[i]);
  }

  std::string output_path = new_output;
  uint64_t output_slash_bits = 0;
  if (!CanonicalizePath(&output_path, &output_slash_bits, &path_error)) {
    *err = path_error;
    return nullptr;
  }
  Node* output_node = state->GetNode(clone->pos_.scope()->GlobalPath(output_path),
                                     output_slash_bits);
  output_node->UpdateFirstReference(clone->dfs_location(), output_slash_bits);
  clone->outputs_.push_back(output_node);
  for (auto& binding : clone->unevaled_bindings_) {
    binding.second = ReplaceAll(std::move(binding.second), old_source, new_source);
  }
  return clone.release();
}

static bool DecodeUniCppGroups(const std::string& encoded,
                               std::vector<UniCppGroup>* groups) {
  std::string data;
  if (!DecodeBase64(encoded, &data)) return false;
  std::vector<std::string> lines;
  size_t start = 0;
  while (start < data.size()) {
    size_t end = data.find('\n', start);
    if (end == std::string::npos) return false;
    lines.push_back(data.substr(start, end - start));
    start = end + 1;
  }
  if (lines.size() < 2 || lines[0] != "C1") return false;
  size_t index = 1;
  size_t group_count = 0;
  if (!ParseCount(lines, &index, &group_count)) return false;
  groups->clear();
  groups->reserve(group_count);
  for (size_t i = 0; i < group_count; ++i) {
    if (index >= lines.size()) return false;
    UniCppGroup group;
    group.pattern = lines[index++];
    size_t count = 0;
    if (group.pattern.empty() || !ParseCount(lines, &index, &count) ||
        index + count > lines.size()) return false;
    group.excludes.insert(group.excludes.end(), lines.begin() + index,
                          lines.begin() + index + count);
    index += count;
    if (!ParseCount(lines, &index, &count) || index + count * 2 > lines.size())
      return false;
    for (size_t j = 0; j < count; ++j) {
      group.previous.emplace_back(lines[index], lines[index + 1]);
      index += 2;
    }
    groups->push_back(std::move(group));
  }
  return index == lines.size();
}

static std::string ReplaceAll(std::string value, const std::string& from,
                              const std::string& to) {
  if (from.empty()) return value;
  size_t pos = 0;
  while ((pos = value.find(from, pos)) != std::string::npos) {
    value.replace(pos, from.size(), to);
    pos += to.size();
  }
  return value;
}

static bool DeriveObjectPath(const std::string& source,
                             const std::string& template_source,
                             const std::string& template_object,
                             std::string* object) {
  const std::string source_ext = std::filesystem::path(source).extension();
  if (source_ext.empty() ||
      source_ext != std::filesystem::path(template_source).extension())
    return false;
  const std::string template_stem =
      template_source.substr(0, template_source.size() - source_ext.size());
  const std::string object_ext = std::filesystem::path(template_object).extension();
  if (object_ext.empty()) return false;
  const std::string object_stem =
      template_object.substr(0, template_object.size() - object_ext.size());
  if (object_stem.size() < template_stem.size() ||
      object_stem.compare(object_stem.size() - template_stem.size(),
                          template_stem.size(), template_stem) != 0)
    return false;
  const std::string source_stem = source.substr(0, source.size() - source_ext.size());
  *object = object_stem.substr(0, object_stem.size() - template_stem.size()) +
            source_stem + object_ext;
  return true;
}

static Edge* FindCompileTemplate(const std::vector<Clump*>& clumps,
                                 const std::string& source,
                                 const std::string& object) {
  for (Clump* clump : clumps) {
    for (Edge* edge : clump->edges_) {
      bool has_source = false;
      for (int i = 0; i < edge->explicit_deps_; ++i) {
        if (edge->inputs_[i]->globalPath().h.str_view().AsString() == source) {
          has_source = true;
          break;
        }
      }
      if (!has_source) continue;
      for (Node* output : edge->outputs_) {
        if (output->globalPath().h.str_view().AsString() == object) return edge;
      }
    }
  }
  return nullptr;
}

static Edge* CloneCompileEdge(State* state, Edge* source_edge,
                              const std::string& old_source,
                              const std::string& old_object,
                              const std::string& new_source,
                              const std::string& new_object,
                              std::string* err) {
  std::unique_ptr<Edge> clone(new Edge());
  clone->pos_ = source_edge->pos_;
  clone->onPosResolvedToScope(source_edge->pos_.scope());
  clone->rule_ = source_edge->rule_;
  clone->pool_ = source_edge->pool_;
  clone->parse_state_ = source_edge->parse_state_;
  clone->tags_ = source_edge->tags_;
  clone->explicit_deps_ = source_edge->explicit_deps_;
  clone->implicit_deps_ = source_edge->implicit_deps_;
  clone->order_only_deps_ = source_edge->order_only_deps_;
  clone->explicit_outs_ = source_edge->explicit_outs_;
  clone->implicit_outs_ = source_edge->implicit_outs_;
  clone->validation_deps_ = source_edge->validation_deps_;
  clone->unevaled_bindings_ = source_edge->unevaled_bindings_;

  uint64_t slash_bits = 0;
  std::string path_error;
  std::string canonical_source = new_source;
  if (!CanonicalizePath(&canonical_source, &slash_bits, &path_error)) {
    *err = path_error;
    return nullptr;
  }
  Node* source_node = state->GetNode(clone->pos_.scope()->GlobalPath(canonical_source),
                                     slash_bits);
  source_node->UpdateFirstReference(clone->dfs_location(), slash_bits);
  clone->inputs_ = source_edge->inputs_;
  clone->validations_ = source_edge->validations_;
  clone->dyndep_ = source_edge->dyndep_;
  bool replaced_source = false;
  for (Node*& input : clone->inputs_) {
    if (!replaced_source &&
        input->globalPath().h.str_view().AsString() == old_source) {
      input = source_node;
      replaced_source = true;
    }
  }
  if (!replaced_source) {
    *err = "cannot clone C/C++ compile edge: source input was not found";
    return nullptr;
  }

  const std::string old_object_stem = old_object.substr(
      0, old_object.size() -
             std::filesystem::path(old_object).extension().string().size());
  const std::string new_object_stem = new_object.substr(
      0, new_object.size() -
             std::filesystem::path(new_object).extension().string().size());
  clone->outputs_.reserve(source_edge->outputs_.size());
  bool replaced_object = false;
  for (Node* output : source_edge->outputs_) {
    std::string output_path = output->globalPath().h.str_view().AsString();
    if (output_path == old_object) {
      output_path = new_object;
      replaced_object = true;
    } else if (output_path.size() >= old_object_stem.size() &&
               output_path.compare(0, old_object_stem.size(), old_object_stem) == 0) {
      output_path.replace(0, old_object_stem.size(), new_object_stem);
    } else {
      *err = "cannot map auxiliary C/C++ output '" + output_path +
             "' for newly discovered source '" + new_source + "'";
      return nullptr;
    }
    uint64_t output_slash_bits = 0;
    if (!CanonicalizePath(&output_path, &output_slash_bits, &path_error)) {
      *err = path_error;
      return nullptr;
    }
    Node* output_node = state->GetNode(clone->pos_.scope()->GlobalPath(output_path),
                                       output_slash_bits);
    output_node->UpdateFirstReference(clone->dfs_location(), output_slash_bits);
    clone->outputs_.push_back(output_node);
  }
  if (!replaced_object) {
    *err = "cannot clone C/C++ compile edge: object output was not found";
    return nullptr;
  }
  for (auto& binding : clone->unevaled_bindings_) {
    binding.second = ReplaceAll(std::move(binding.second), old_source, new_source);
    binding.second = ReplaceAll(std::move(binding.second), old_object, new_object);
  }
  return clone.release();
}

static bool ApplyUniCppGroups(State* state, const std::vector<Clump*>& clumps,
                              std::string* err) {
  std::vector<Edge*> consumers;
  std::set<std::string> old_glob_objects;
  std::set<std::string> current_glob_objects;
  for (Clump* clump : clumps) {
    for (Edge* edge : clump->edges_) {
      std::string encoded;
      if (!edge->EvaluateVariable(&encoded, kRunaDynamicCpp,
                                  edge->pos_.scope(), err))
        return false;
      if (!encoded.empty()) consumers.push_back(edge);
    }
  }

  for (Edge* consumer : consumers) {
    std::string encoded;
    if (!consumer->EvaluateVariable(&encoded, kRunaDynamicCpp,
                                    consumer->pos_.scope(), err))
      return false;
    std::vector<UniCppGroup> groups;
    if (!DecodeUniCppGroups(encoded, &groups)) {
      *err = "invalid runa_dynamic_cpp metadata";
      return false;
    }

    std::vector<std::set<std::string>> old_objects(groups.size());
    std::vector<std::map<std::string, std::string>> old_mappings(groups.size());
    std::vector<std::vector<std::pair<std::string, std::string>>> current(groups.size());
    std::vector<std::pair<std::string, std::string>> added;
    std::set<std::string> seen_sources;
    std::map<std::string, size_t> old_object_groups;
    for (size_t group_index = 0; group_index < groups.size(); ++group_index) {
      const UniCppGroup& group = groups[group_index];
      for (const auto& pair : group.previous) {
        old_mappings[group_index][pair.first] = pair.second;
        old_objects[group_index].insert(pair.second);
        old_object_groups[pair.second] = group_index;
        old_glob_objects.insert(pair.second);
      }
      std::vector<std::string> matches;
      std::string glob_error;
      UniSourceGlob glob{group.pattern, group.excludes, {}};
      if (!ExpandUniSourceGlob(glob, &matches, &glob_error)) {
        *err = glob_error;
        return false;
      }
      for (const std::string& source : matches) {
        if (!seen_sources.insert(source).second) continue;
        auto old = old_mappings[group_index].find(source);
        if (old != old_mappings[group_index].end()) {
          current[group_index].emplace_back(source, old->second);
          continue;
        }
        bool mapped = false;
        std::string object;
        for (const auto& template_pair : group.previous) {
          if (!DeriveObjectPath(source, template_pair.first,
                                template_pair.second, &object))
            continue;
          Edge* template_edge = FindCompileTemplate(clumps, template_pair.first,
                                                    template_pair.second);
          if (template_edge == nullptr) continue;
          current[group_index].emplace_back(source, object);
          added.emplace_back(source, object);
          mapped = true;
          break;
        }
        if (!mapped) {
          *err = "Runa cannot find a C/C++ compile template for newly matched source '" +
                 source + "' in glob '" + group.pattern + "'";
          return false;
        }
      }
    }

    std::vector<std::vector<Node*>> current_objects(groups.size());
    for (size_t group_index = 0; group_index < groups.size(); ++group_index) {
      current_objects[group_index].reserve(current[group_index].size());
      for (const auto& pair : current[group_index]) {
        current_glob_objects.insert(pair.second);
        std::string object = pair.second;
        uint64_t slash_bits = 0;
        std::string path_error;
        if (!CanonicalizePath(&object, &slash_bits, &path_error)) {
          *err = path_error;
          return false;
        }
        Node* node = state->GetNode(consumer->pos_.scope()->GlobalPath(object),
                                    slash_bits);
        node->UpdateFirstReference(consumer->dfs_location(), slash_bits);
        current_objects[group_index].push_back(node);
      }
    }

    std::vector<Node*> updated_inputs;
    updated_inputs.reserve(consumer->inputs_.size());
    std::set<size_t> inserted_groups;
    for (size_t i = 0; i < static_cast<size_t>(consumer->explicit_deps_); ++i) {
      const std::string path = consumer->inputs_[i]->globalPath().h.str_view().AsString();
      auto group = old_object_groups.find(path);
      if (group != old_object_groups.end()) {
        if (inserted_groups.insert(group->second).second) {
          updated_inputs.insert(updated_inputs.end(),
                                current_objects[group->second].begin(),
                                current_objects[group->second].end());
        }
      } else {
        updated_inputs.push_back(consumer->inputs_[i]);
      }
    }
    for (size_t group_index = 0; group_index < groups.size(); ++group_index) {
      if (inserted_groups.find(group_index) == inserted_groups.end()) {
        updated_inputs.insert(updated_inputs.end(),
                              current_objects[group_index].begin(),
                              current_objects[group_index].end());
      }
    }
    const int new_explicit_deps = static_cast<int>(updated_inputs.size());
    updated_inputs.insert(updated_inputs.end(),
                          consumer->inputs_.begin() + consumer->explicit_deps_,
                          consumer->inputs_.end());
    consumer->inputs_ = std::move(updated_inputs);
    consumer->explicit_deps_ = new_explicit_deps;

    // Some archive rules also pass their objects through an explicit variable
    // rather than $in. Keep that variable synchronized with the dynamic input
    // set while preserving all unrelated arguments.
    for (auto& binding : consumer->unevaled_bindings_) {
      if (binding.first.str() != "arObjs") continue;
      std::istringstream args(binding.second);
      std::ostringstream refreshed;
      std::string token;
      std::set<size_t> written_groups;
      while (args >> token) {
        auto group = old_object_groups.find(token);
        if (group != old_object_groups.end()) {
          if (written_groups.insert(group->second).second) {
            for (Node* object : current_objects[group->second])
              refreshed << object->path() << ' ';
          }
        } else {
          refreshed << token << ' ';
        }
      }
      for (size_t group_index = 0; group_index < groups.size(); ++group_index) {
        if (written_groups.find(group_index) == written_groups.end()) {
          for (Node* object : current_objects[group_index])
            refreshed << object->path() << ' ';
        }
      }
      binding.second = refreshed.str() + "\n";
    }

    for (const auto& pair : added) {
      // Find the matching template by source extension and derive its output
      // mapping from the old glob members.
      Edge* actual_template = nullptr;
      std::string old_source, old_object;
      for (const UniCppGroup& group : groups) {
        for (const auto& candidate : group.previous) {
          std::string derived;
          if (!DeriveObjectPath(pair.first, candidate.first, candidate.second,
                                &derived) || derived != pair.second)
            continue;
          actual_template = FindCompileTemplate(clumps, candidate.first,
                                                candidate.second);
          if (actual_template != nullptr) {
            old_source = candidate.first;
            old_object = candidate.second;
            break;
          }
        }
        if (actual_template != nullptr) break;
      }
      if (actual_template == nullptr) {
        *err = "Runa cannot clone a C/C++ compile action for '" + pair.first + "'";
        return false;
      }
      Edge* clone = CloneCompileEdge(state, actual_template, old_source,
                                     old_object, pair.first, pair.second, err);
      if (clone == nullptr) return false;
      clumps.back()->edge_output_count_ += clone->explicit_outs_;
      clumps.back()->edges_.push_back(clone);
    }
  }

  for (Clump* clump : clumps) {
    for (size_t edge_index = 0; edge_index < clump->edges_.size();) {
      Edge* edge = clump->edges_[edge_index];
      bool obsolete = false;
      for (Node* output : edge->outputs_) {
        const std::string path =
            output->globalPath().h.str_view().AsString();
        if (old_glob_objects.find(path) != old_glob_objects.end() &&
            current_glob_objects.find(path) == current_glob_objects.end()) {
          obsolete = true;
          break;
        }
      }
      if (!obsolete) {
        ++edge_index;
        continue;
      }
      clump->edge_output_count_ -= edge->explicit_outs_;
      clump->edges_.erase(clump->edges_.begin() + edge_index);
      delete edge;
    }
  }
  return true;
}

static bool ApplyUniAaptGroups(State* state, const std::vector<Clump*>& clumps,
                               std::string* err) {
  std::vector<Edge*> compile_edges;
  std::vector<Edge*> input_consumers;
  for (Clump* clump : clumps) {
    for (Edge* edge : clump->edges_) {
      std::string encoded;
      // The source-to-flat mapping is attached to the aapt2 compile edge.
      if (!edge->EvaluateVariable(&encoded, kRunaDynamicAaptInputs,
                                  edge->pos_.scope(), err)) return false;
      if (!encoded.empty()) compile_edges.push_back(edge);
      encoded.clear();
      // The file-list edge consumes the compiled flat outputs.
      if (!edge->EvaluateVariable(&encoded, kRunaDynamicAaptCompile,
                                  edge->pos_.scope(), err)) return false;
      if (!encoded.empty()) input_consumers.push_back(edge);
    }
  }

  std::set<std::string> expanded_groups;
  std::set<std::string> synthesized_sources;
  std::map<std::string, std::string> old_aapt_source_outputs;
  std::set<std::string> old_aapt_outputs;
  std::set<std::string> current_aapt_outputs;
  for (Edge* compile_edge : compile_edges) {
    std::string encoded;
    if (!compile_edge->EvaluateVariable(&encoded, kRunaDynamicAaptInputs,
                                        compile_edge->pos_.scope(), err)) return false;
    std::vector<UniAaptGroup> groups;
    if (!DecodeUniAaptGroups(encoded, &groups)) {
      *err = "invalid runa_dynamic_aapt_inputs metadata";
      return false;
    }
    for (const UniAaptGroup& group : groups) {
      const std::string group_key = group.pattern + "\n" + group.resource_root +
                                    "\n" + group.output_root;
      if (!expanded_groups.insert(group_key).second) continue;
      std::map<std::string, std::string> old_mapping;
      for (const auto& pair : group.previous) {
        old_mapping[pair.first] = pair.second;
        old_aapt_source_outputs[pair.first] = pair.second;
        old_aapt_outputs.insert(pair.second);
      }
      std::vector<std::string> matches;
      std::string glob_error;
      UniSourceGlob glob{group.pattern, group.excludes, {}};
      if (!ExpandUniSourceGlob(glob, &matches, &glob_error)) {
        *err = glob_error;
        return false;
      }
      for (const std::string& source : matches) {
        auto old = old_mapping.find(source);
        if (old != old_mapping.end()) {
          current_aapt_outputs.insert(old->second);
          continue;
        }
        if (!synthesized_sources.insert(source).second) continue;
        std::string output;
        if (!DeriveAaptFlatPath(source, group.resource_root, group.output_root,
                                &output)) {
          *err = "Runa cannot map newly matched aapt2 resource '" + source +
                 "' to a compiled output";
          return false;
        }
        current_aapt_outputs.insert(output);
        Edge* template_edge = nullptr;
        std::string template_source;
        for (const auto& candidate : group.previous) {
          template_edge = FindCompileTemplate(clumps, candidate.first,
                                              candidate.second);
          if (template_edge != nullptr) {
            template_source = candidate.first;
            break;
          }
        }
        if (template_edge == nullptr) {
          *err = "Runa cannot find an aapt2 compile template for resource '" +
                 source + "'";
          return false;
        }
        Edge* clone = CloneAaptCompileEdge(state, template_edge, template_source,
                                           source, output, err);
        if (clone == nullptr) return false;
        clumps.back()->edge_output_count_ += clone->explicit_outs_;
        clumps.back()->edges_.push_back(clone);
      }
    }
  }

  // Remove deleted resources from the old sharded compile outputs. Keeping a
  // stale output declaration would leave the removed resource in Runa's graph
  // even though the file-list and link consumers no longer reference it.
  for (Clump* clump : clumps) {
    for (size_t edge_index = 0; edge_index < clump->edges_.size();) {
      Edge* edge = clump->edges_[edge_index];
      std::string metadata;
      if (!edge->EvaluateVariable(&metadata, kRunaDynamicAaptInputs,
                                  edge->pos_.scope(), err)) return false;
      if (metadata.empty()) {
        ++edge_index;
        continue;
      }
      const int old_explicit_deps = edge->explicit_deps_;
      const int old_explicit_outs = edge->explicit_outs_;
      std::vector<Node*> inputs;
      for (int i = 0; i < old_explicit_deps; ++i) {
        Node* input = edge->inputs_[i];
        const std::string path = input->globalPath().h.str_view().AsString();
        auto old = old_aapt_source_outputs.find(path);
        if (old != old_aapt_source_outputs.end() &&
            current_aapt_outputs.find(old->second) == current_aapt_outputs.end())
          continue;
        inputs.push_back(input);
      }
      inputs.insert(inputs.end(), edge->inputs_.begin() + old_explicit_deps,
                    edge->inputs_.end());
      edge->inputs_ = std::move(inputs);
      edge->explicit_deps_ = static_cast<int>(edge->inputs_.size()) -
                             (edge->implicit_deps_ + edge->order_only_deps_);

      std::vector<Node*> outputs;
      for (int i = 0; i < old_explicit_outs; ++i) {
        Node* output = edge->outputs_[i];
        const std::string path = output->globalPath().h.str_view().AsString();
        if (old_aapt_outputs.find(path) != old_aapt_outputs.end() &&
            current_aapt_outputs.find(path) == current_aapt_outputs.end())
          continue;
        outputs.push_back(output);
      }
      outputs.insert(outputs.end(), edge->outputs_.begin() + old_explicit_outs,
                     edge->outputs_.end());
      edge->outputs_ = std::move(outputs);
      edge->explicit_outs_ = static_cast<int>(edge->outputs_.size()) -
                             edge->implicit_outs_;
      clump->edge_output_count_ -= old_explicit_outs - edge->explicit_outs_;
      if (edge->outputs_.empty()) {
        clump->edges_.erase(clump->edges_.begin() + edge_index);
        delete edge;
      } else {
        ++edge_index;
      }
    }
  }

  for (Edge* consumer : input_consumers) {
    std::string encoded;
    if (!consumer->EvaluateVariable(&encoded, kRunaDynamicAaptCompile,
                                    consumer->pos_.scope(), err)) return false;
    std::vector<UniAaptGroup> groups;
    if (!DecodeUniAaptGroups(encoded, &groups)) {
      *err = "invalid runa_dynamic_aapt_compile metadata";
      return false;
    }
    std::vector<std::vector<std::string>> outputs(groups.size());
    std::map<std::string, size_t> old_output_groups;
    for (size_t i = 0; i < groups.size(); ++i) {
      std::map<std::string, std::string> old_mapping;
      for (const auto& pair : groups[i].previous) {
        old_mapping[pair.first] = pair.second;
        old_output_groups[pair.second] = i;
      }
      std::vector<std::string> matches;
      std::string glob_error;
      UniSourceGlob glob{groups[i].pattern, groups[i].excludes, {}};
      if (!ExpandUniSourceGlob(glob, &matches, &glob_error)) {
        *err = glob_error;
        return false;
      }
      std::set<std::string> unique_outputs;
      for (const std::string& source : matches) {
        auto old = old_mapping.find(source);
        if (old != old_mapping.end()) {
          unique_outputs.insert(old->second);
          continue;
        }
        std::string output;
        if (!DeriveAaptFlatPath(source, groups[i].resource_root,
                                groups[i].output_root, &output)) {
          *err = "Runa cannot map newly matched aapt2 resource '" + source +
                 "' to a compiled output";
          return false;
        }
        unique_outputs.insert(output);
      }
      outputs[i].assign(unique_outputs.begin(), unique_outputs.end());
    }

    std::vector<std::vector<Node*>> group_nodes(groups.size());
    for (size_t i = 0; i < groups.size(); ++i) {
      for (const std::string& output : outputs[i]) {
        std::string canonical = output;
        uint64_t slash_bits = 0;
        std::string path_error;
        if (!CanonicalizePath(&canonical, &slash_bits, &path_error)) {
          *err = path_error;
          return false;
        }
        Node* node = state->GetNode(consumer->pos_.scope()->GlobalPath(canonical),
                                    slash_bits);
        node->UpdateFirstReference(consumer->dfs_location(), slash_bits);
        group_nodes[i].push_back(node);
      }
    }

    auto refresh_segment = [&](size_t begin, size_t end) {
      std::vector<Node*> refreshed;
      std::set<size_t> inserted;
      for (size_t pos = begin; pos < end; ++pos) {
        const std::string path =
            consumer->inputs_[pos]->globalPath().h.str_view().AsString();
        auto group = old_output_groups.find(path);
        if (group == old_output_groups.end()) {
          refreshed.push_back(consumer->inputs_[pos]);
        } else if (inserted.insert(group->second).second) {
          refreshed.insert(refreshed.end(), group_nodes[group->second].begin(),
                           group_nodes[group->second].end());
        }
      }
      return refreshed;
    };
    const size_t old_explicit_deps = consumer->explicit_deps_;
    std::vector<Node*> explicit_inputs = refresh_segment(0, old_explicit_deps);
    std::vector<Node*> implicit_inputs = refresh_segment(
        old_explicit_deps, consumer->inputs_.size());
    consumer->explicit_deps_ = static_cast<int>(explicit_inputs.size());
    explicit_inputs.insert(explicit_inputs.end(), implicit_inputs.begin(),
                           implicit_inputs.end());
    consumer->inputs_ = std::move(explicit_inputs);
  }
  return true;
}

struct ManifestLoader {
private:
  State* const state_ = nullptr;
  ThreadPool* const thread_pool_ = nullptr;
  const ManifestParserOptions options_;
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

  if (options_.uni_source_globs) {
    std::string encoded_globs;
    if (!edge->EvaluateVariable(&encoded_globs, kRunaSourceGlobs,
                                edge->pos_.scope(), err)) {
      return false;
    }
    if (!encoded_globs.empty()) {
      std::vector<UniSourceGlob> source_globs;
      if (!DecodeUniSourceGlobs(encoded_globs, &source_globs)) {
        return DecorateError(file, edge->parse_state_.final_diag_pos,
                             "invalid runa_source_globs metadata", err);
      }

      std::set<std::string> previous_matches;
      std::vector<std::string> current_matches;
      for (const UniSourceGlob& glob : source_globs) {
        previous_matches.insert(glob.previous_matches.begin(),
                                glob.previous_matches.end());
        std::vector<std::string> matches;
        std::string glob_error;
        if (!ExpandUniSourceGlob(glob, &matches, &glob_error)) {
          return DecorateError(file, edge->parse_state_.final_diag_pos,
                               glob_error, err);
        }
        current_matches.insert(current_matches.end(), matches.begin(),
                               matches.end());
      }

      std::sort(current_matches.begin(), current_matches.end());
      current_matches.erase(std::unique(current_matches.begin(),
                                        current_matches.end()),
                            current_matches.end());
      std::vector<Node*> explicit_inputs;
      explicit_inputs.reserve(edge->explicit_deps_ + current_matches.size());
      std::set<std::string> present;
      for (int i = 0; i < edge->explicit_deps_; ++i) {
        Node* input = edge->inputs_[i];
        std::string path = input->globalPath().h.str_view().AsString();
        if (previous_matches.find(path) == previous_matches.end() &&
            present.insert(path).second) {
          explicit_inputs.push_back(input);
        }
      }
      for (const std::string& match : current_matches) {
        std::string path = match;
        uint64_t slash_bits = 0;
        std::string path_error;
        if (!CanonicalizePath(&path, &slash_bits, &path_error)) {
          return DecorateError(file, edge->parse_state_.final_diag_pos,
                               path_error, err);
        }
        if (!present.insert(path).second) continue;
        Node* input = state_->GetNode(edge->pos_.scope()->GlobalPath(path),
                                      slash_bits);
        input->UpdateFirstReference(edge->dfs_location(), slash_bits);
        explicit_inputs.push_back(input);
      }

      std::vector<Node*> non_explicit_inputs(
          edge->inputs_.begin() + edge->explicit_deps_, edge->inputs_.end());
      edge->inputs_ = std::move(explicit_inputs);
      edge->inputs_.insert(edge->inputs_.end(), non_explicit_inputs.begin(),
                           non_explicit_inputs.end());
      edge->explicit_deps_ = static_cast<int>(edge->inputs_.size() -
                                              non_explicit_inputs.size());
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
    if (!ApplyUniAaptGroups(state_, clumps, err) ||
        !ApplyUniCppGroups(state_, clumps, err)) {
      return false;
    }
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
  return FinishLoading(clumps, err);
}

} // anonymous namespace

bool ManifestParser::Load(const string& filename, string* err) {
  METRIC_RECORD(".ninja load");

  ManifestFileSet file_set(file_reader_);
  const LoadedFile* file = nullptr;
  if (!file_set.LoadFile(filename, &file, err))
    return false;

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
