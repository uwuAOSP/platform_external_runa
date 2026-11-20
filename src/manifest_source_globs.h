/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */

// Internal manifest-loader helpers. Included inside manifest_parser.cc's
// anonymous namespace, after the parser and graph types are available.
// Compatibility with the source-set declarations already emitted by Soong is
// intentional: membership changes must not require another Soong invocation.
#ifndef RUNA_MANIFEST_SOURCE_GLOBS_H_
#define RUNA_MANIFEST_SOURCE_GLOBS_H_

static const HashedStrView kRunaSourceGlobs{ "runa_source_globs" };
static const HashedStrView kRunaDynamicCpp{ "runa_dynamic_cpp" };
static const HashedStrView kRunaDynamicAaptCompile{
  "runa_dynamic_aapt_compile"
};
static const HashedStrView kRunaDynamicAaptInputs{ "runa_dynamic_aapt_inputs" };

static bool ParseCount(const std::vector<std::string>& lines, size_t* index,
                       size_t* count) {
  if (*index >= lines.size() || lines[*index].empty())
    return false;
  const std::string& text = lines[*index];
  if (text.find_first_not_of("0123456789") != std::string::npos)
    return false;
  errno = 0;
  char* end = nullptr;
  unsigned long value = strtoul(text.c_str(), &end, 10);
  if (errno == ERANGE || *end != '\0')
    return false;
  ++*index;
  *count = static_cast<size_t>(value);
  return true;
}

static bool DecodeSourceSetLines(const std::string& encoded,
                                 const char* version,
                                 std::vector<std::string>* lines) {
  std::string data;
  if (!DecodeBase64(encoded, &data))
    return false;
  lines->clear();
  size_t start = 0;
  while (start < data.size()) {
    size_t end = data.find('\n', start);
    if (end == std::string::npos)
      return false;
    lines->push_back(data.substr(start, end - start));
    start = end + 1;
  }
  return lines->size() >= 2 && (*lines)[0] == version;
}

static bool DecodeUniSourceGlobs(const std::string& encoded,
                                 std::vector<UniSourceGlob>* globs) {
  std::vector<std::string> lines;
  if (!DecodeSourceSetLines(encoded, "G1", &lines))
    return false;
  size_t index = 1, count = 0;
  if (!ParseCount(lines, &index, &count) || count > lines.size())
    return false;
  globs->clear();
  for (size_t i = 0; i < count; ++i) {
    if (index >= lines.size())
      return false;
    UniSourceGlob glob;
    glob.pattern = lines[index++];
    size_t size = 0;
    if (glob.pattern.empty() || !ParseCount(lines, &index, &size) ||
        size > lines.size() - index)
      return false;
    glob.excludes.assign(lines.begin() + index, lines.begin() + index + size);
    index += size;
    if (!ParseCount(lines, &index, &size) || size > lines.size() - index)
      return false;
    glob.previous_matches.assign(lines.begin() + index,
                                 lines.begin() + index + size);
    index += size;
    globs->push_back(std::move(glob));
  }
  return index == lines.size();
}

static std::vector<std::string> PathSegments(const std::string& path) {
  std::vector<std::string> result;
  size_t start = 0;
  while (start <= path.size()) {
    size_t end = path.find('/', start);
    if (end == std::string::npos)
      end = path.size();
    if (end > start)
      result.push_back(path.substr(start, end - start));
    if (end == path.size())
      break;
    start = end + 1;
  }
  return result;
}

static bool MatchGlobSegments(const std::vector<std::string>& pattern,
                              const std::vector<std::string>& path, size_t p,
                              size_t s) {
  if (p == pattern.size())
    return s == path.size();
  if (pattern[p] == "**") {
    for (size_t i = s; i <= path.size(); ++i) {
      if (MatchGlobSegments(pattern, path, p + 1, i))
        return true;
    }
    return false;
  }
  return s < path.size() &&
         fnmatch(pattern[p].c_str(), path[s].c_str(), FNM_PERIOD) == 0 &&
         MatchGlobSegments(pattern, path, p + 1, s + 1);
}

static bool MatchGlobPath(const std::string& pattern, const std::string& path) {
  return MatchGlobSegments(PathSegments(pattern), PathSegments(path), 0, 0);
}

static bool HasGlobCharacters(const std::string& segment) {
  return segment.find_first_of("*?[") != std::string::npos;
}

// A directory can only be visited if its components are a prefix of a
// possible file match. In particular, Blueprint's ** traversal does not
// descend into dot directories unless the pattern names them explicitly.
static bool CanContainGlobMatch(const std::vector<std::string>& pattern,
                                const std::vector<std::string>& path,
                                size_t p, size_t s) {
  if (p == pattern.size())
    return false;
  if (s == path.size())
    return true;
  if (pattern[p] == "**") {
    return CanContainGlobMatch(pattern, path, p + 1, s) ||
           (path[s][0] != '.' && CanContainGlobMatch(pattern, path, p, s + 1));
  }
  return fnmatch(pattern[p].c_str(), path[s].c_str(), FNM_PERIOD) == 0 &&
         CanContainGlobMatch(pattern, path, p + 1, s + 1);
}

static bool ExpandUniSourceGlob(const UniSourceGlob& glob,
                                std::vector<std::string>* matches,
                                std::string* err) {
  const auto segments = PathSegments(glob.pattern);
  size_t wildcard = 0;
  while (wildcard < segments.size() && !HasGlobCharacters(segments[wildcard]))
    ++wildcard;
  if (wildcard == segments.size()) {
    *err = "source glob has no wildcard: " + glob.pattern;
    return false;
  }
  std::filesystem::path base = std::filesystem::path(glob.pattern).is_absolute()
                                   ? std::filesystem::path("/")
                                   : std::filesystem::path();
  for (size_t i = 0; i < wildcard; ++i)
    base /= segments[i];
  if (base.empty())
    base = ".";
  std::error_code ec;
  matches->clear();
  if (!std::filesystem::is_directory(base, ec)) {
    if (ec && ec != std::errc::no_such_file_or_directory &&
        ec != std::errc::not_a_directory) {
      *err = "cannot inspect source glob directory '" + base.string() +
             "': " + ec.message();
      return false;
    }
    return true;
  }
  const auto options =
      std::filesystem::directory_options::follow_directory_symlink;
  std::filesystem::recursive_directory_iterator it(base, options, ec), end;
  if (ec) {
    *err = "cannot enumerate source glob directory '" + base.string() +
           "': " + ec.message();
    return false;
  }
  for (; it != end; it.increment(ec)) {
    if (ec)
      break;
    std::string path = it->path().lexically_normal().generic_string();
    if (path.rfind("./", 0) == 0)
      path.erase(0, 2);
    const auto path_segments = PathSegments(path);
    if (!CanContainGlobMatch(segments, path_segments, 0, 0))
      it.disable_recursion_pending();
    else if (it.depth() >= 255) {
      *err = "source glob directory nesting exceeds 255 levels: " + path;
      return false;
    }
    if (!MatchGlobSegments(segments, path_segments, 0, 0))
      continue;
    std::error_code status_error;
    if (!it->is_regular_file(status_error)) {
      if (status_error &&
          status_error != std::errc::no_such_file_or_directory) {
        *err = "cannot inspect source glob entry '" + it->path().string() +
               "': " + status_error.message();
        return false;
      }
      continue;
    }
    bool excluded = false;
    for (const auto& exclude : glob.excludes) {
      if (MatchGlobPath(exclude, path)) {
        excluded = true;
        break;
      }
    }
    if (!excluded)
      matches->push_back(std::move(path));
  }
  if (ec) {
    *err = "cannot enumerate source glob directory '" + base.string() +
           "': " + ec.message();
    return false;
  }
  std::sort(matches->begin(), matches->end());
  matches->erase(std::unique(matches->begin(), matches->end()), matches->end());
  return true;
}

struct UniCppGroup {
  std::string pattern;
  std::vector<std::string> excludes;
  std::vector<std::pair<std::string, std::string>> previous;
};

// One filesystem enumeration per declaration per manifest load. Many Java
// edges and resource consumers share a source set. Nothing is persisted across
// loads, so added/deleted files are always observed on the next invocation.
struct SourceSetExpansionCache {
  std::map<std::vector<std::string>, std::vector<std::string>> matches;

  bool Expand(const UniSourceGlob& glob, std::vector<std::string>* result,
              std::string* err) {
    auto excludes = glob.excludes;
    std::sort(excludes.begin(), excludes.end());
    std::vector<std::string> key{ glob.pattern };
    key.insert(key.end(), excludes.begin(), excludes.end());
    auto found = matches.find(key);
    if (found == matches.end()) {
      std::vector<std::string> current;
      if (!ExpandUniSourceGlob(glob, &current, err))
        return false;
      found = matches.emplace(std::move(key), std::move(current)).first;
    }
    *result = found->second;
    return true;
  }
};

struct UniAaptGroup : UniCppGroup {
  std::string resource_root;
  std::string output_root;
};

static bool SetResourceRoots(UniCppGroup*, const std::string&,
                             const std::string&);
static bool SetResourceRoots(UniAaptGroup*, const std::string&,
                             const std::string&);

template <typename Group>
static bool DecodeUniMappedGroups(const std::string& encoded,
                                  const char* version, bool resource_roots,
                                  std::vector<Group>* groups) {
  std::vector<std::string> lines;
  if (!DecodeSourceSetLines(encoded, version, &lines))
    return false;
  size_t index = 1, count = 0;
  if (!ParseCount(lines, &index, &count) || count > lines.size())
    return false;
  groups->clear();
  for (size_t i = 0; i < count; ++i) {
    if (index >= lines.size())
      return false;
    Group group;
    group.pattern = lines[index++];
    size_t size = 0;
    if (group.pattern.empty() || !ParseCount(lines, &index, &size) ||
        size > lines.size() - index)
      return false;
    group.excludes.assign(lines.begin() + index, lines.begin() + index + size);
    index += size;
    if (resource_roots) {
      if (lines.size() - index < 2)
        return false;
      // The overload below leaves ordinary source/object groups unchanged.
      if (!SetResourceRoots(&group, lines[index], lines[index + 1]))
        return false;
      index += 2;
    }
    if (!ParseCount(lines, &index, &size) || size > (lines.size() - index) / 2)
      return false;
    for (size_t j = 0; j < size; ++j, index += 2) {
      if (lines[index].empty() || lines[index + 1].empty())
        return false;
      group.previous.emplace_back(lines[index], lines[index + 1]);
    }
    groups->push_back(std::move(group));
  }
  return index == lines.size();
}

static bool SetResourceRoots(UniCppGroup*, const std::string&,
                             const std::string&) {
  return false;
}

static bool SetResourceRoots(UniAaptGroup* group, const std::string& resource,
                             const std::string& output) {
  group->resource_root = resource;
  group->output_root = output;
  return !resource.empty() && !output.empty();
}

static bool DecodeUniCppGroups(const std::string& encoded,
                               std::vector<UniCppGroup>* groups) {
  return DecodeUniMappedGroups(encoded, "C1", false, groups);
}

static bool DecodeUniAaptGroups(const std::string& encoded,
                                std::vector<UniAaptGroup>* groups) {
  return DecodeUniMappedGroups(encoded, "A1", true, groups);
}

static std::string ReplaceAll(std::string value, const std::string& from,
                              const std::string& to) {
  if (from.empty())
    return value;
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
  const std::string ext = std::filesystem::path(source).extension();
  if (ext.empty() || ext != std::filesystem::path(template_source).extension())
    return false;
  const std::string template_stem =
      template_source.substr(0, template_source.size() - ext.size());
  const std::string object_ext =
      std::filesystem::path(template_object).extension();
  if (object_ext.empty())
    return false;
  const std::string object_stem =
      template_object.substr(0, template_object.size() - object_ext.size());
  if (object_stem.size() < template_stem.size() ||
      object_stem.compare(object_stem.size() - template_stem.size(),
                          template_stem.size(), template_stem) != 0)
    return false;
  *object = object_stem.substr(0, object_stem.size() - template_stem.size()) +
            source.substr(0, source.size() - ext.size()) + object_ext;
  return true;
}

static bool DeriveAaptFlatPath(const std::string& source,
                               const std::string& resource_root,
                               const std::string& output_root,
                               std::string* output) {
  const auto relative =
      std::filesystem::path(source).lexically_relative(resource_root);
  if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
    return false;
  std::vector<std::string> components;
  for (const auto& part : relative)
    components.push_back(part.string());
  if (components.empty())
    return false;
  const std::filesystem::path source_path(source);
  std::string extension = source_path.extension().string();
  std::filesystem::path result(output_root);
  if (components.size() == 1) {
    result = result.parent_path() /
             (std::filesystem::path(resource_root).filename().string() + "_" +
              source_path.stem().string() + extension + ".flat");
  } else {
    components.pop_back();
    std::string flag;
    if (components.back().rfind("flag", 0) == 0) {
      flag = "." + components.back().substr(4);
      components.pop_back();
      if (components.empty())
        return false;
    } else if (components.size() >= 2 &&
               components[components.size() - 2].rfind("flag", 0) == 0) {
      // Soong also accepts res/flagNAME/values/file.xml, not just
      // res/values/flagNAME/file.xml.
      flag = "." + components[components.size() - 2].substr(4);
      components.erase(components.end() - 2);
    }
    const std::string parent = components.back();
    components.pop_back();
    for (const auto& part : components)
      result /= part;
    if (parent.rfind("values", 0) == 0 && extension == ".xml")
      extension = ".arsc";
    result /=
        parent + "_" + source_path.stem().string() + flag + extension + ".flat";
  }
  *output = result.lexically_normal().string();
  return true;
}

// Index already-parsed producers once, instead of rescanning the entire graph
// for every new source and every link/archive consumer.
using SourceSetProducers = std::unordered_map<std::string, Edge*>;

static Edge* FindCompileTemplate(const SourceSetProducers& producers,
                                 const std::string& source,
                                 const std::string& output) {
  auto found = producers.find(output);
  if (found == producers.end())
    return nullptr;
  Edge* edge = found->second;
  for (int i = 0; i < edge->explicit_deps_; ++i) {
    if (edge->inputs_[i]->globalPath().h.str_view().AsString() == source)
      return edge;
  }
  return nullptr;
}

static Node* SourceSetNode(State* state, Edge* edge, std::string path,
                           std::string* err) {
  uint64_t slash_bits = 0;
  if (!CanonicalizePath(&path, &slash_bits, err))
    return nullptr;
  Node* node = state->GetNode(edge->pos_.scope()->GlobalPath(path), slash_bits);
  node->UpdateFirstReference(edge->dfs_location(), slash_bits);
  return node;
}

static Edge* CloneCompileEdge(State* state, Edge* original,
                              const std::string& old_source,
                              const std::string& old_output,
                              const std::string& source,
                              const std::string& output, bool resource,
                              std::string* err) {
  std::unique_ptr<Edge> clone(new Edge());
  clone->pos_ = original->pos_;
  clone->onPosResolvedToScope(original->pos_.scope());
  clone->rule_ = original->rule_;
  clone->pool_ = original->pool_;
  clone->parse_state_ = original->parse_state_;
  clone->tags_ = original->tags_;
  clone->explicit_deps_ = original->explicit_deps_;
  clone->implicit_deps_ = original->implicit_deps_;
  clone->order_only_deps_ = original->order_only_deps_;
  clone->explicit_outs_ = original->explicit_outs_;
  clone->implicit_outs_ = original->implicit_outs_;
  clone->validation_deps_ = original->validation_deps_;
  clone->validations_ = original->validations_;
  clone->dyndep_ = original->dyndep_;
  clone->unevaled_bindings_ = original->unevaled_bindings_;
  Node* input = SourceSetNode(state, clone.get(), source, err);
  if (!input)
    return nullptr;
  if (resource) {
    // Resource compile actions may batch many old inputs/outputs. A new
    // resource gets a single-input action, not a clone of that whole batch.
    clone->explicit_deps_ = 1;
    clone->explicit_outs_ = 1;
    clone->implicit_outs_ = 0;
    clone->inputs_.push_back(input);
    clone->inputs_.insert(clone->inputs_.end(),
                          original->inputs_.begin() + original->explicit_deps_,
                          original->inputs_.end());
    Node* out = SourceSetNode(state, clone.get(), output, err);
    if (!out)
      return nullptr;
    clone->outputs_.push_back(out);
  } else {
    clone->inputs_ = original->inputs_;
    bool replaced = false;
    for (Node*& node : clone->inputs_) {
      if (!replaced &&
          node->globalPath().h.str_view().AsString() == old_source) {
        node = input;
        replaced = true;
      }
    }
    if (!replaced) {
      *err = "compile template source input not found";
      return nullptr;
    }
    const std::string old_stem = old_output.substr(
        0, old_output.size() -
               std::filesystem::path(old_output).extension().string().size());
    const std::string new_stem = output.substr(
        0, output.size() -
               std::filesystem::path(output).extension().string().size());
    bool has_output = false;
    for (Node* node : original->outputs_) {
      std::string path = node->globalPath().h.str_view().AsString();
      if (path == old_output) {
        path = output;
        has_output = true;
      } else if (path.compare(0, old_stem.size(), old_stem) == 0)
        path.replace(0, old_stem.size(), new_stem);
      else {
        *err = "cannot map auxiliary compile output '" + path + "'";
        return nullptr;
      }
      Node* out = SourceSetNode(state, clone.get(), path, err);
      if (!out)
        return nullptr;
      clone->outputs_.push_back(out);
    }
    if (!has_output) {
      *err = "compile template output not found";
      return nullptr;
    }
  }
  for (auto& binding : clone->unevaled_bindings_) {
    // A synthesized action must not carry the original batch's membership
    // declaration and accidentally expand the group a second time.
    if (binding.first.str() == "runa_dynamic_aapt_inputs" ||
        binding.first.str() == "runa_source_globs")
      binding.second.clear();
    else {
      binding.second =
          ReplaceAll(std::move(binding.second), old_source, source);
      if (!resource) {
        const std::string old_stem = old_output.substr(
            0,
            old_output.size() -
                std::filesystem::path(old_output).extension().string().size());
        const std::string new_stem = output.substr(
            0, output.size() -
                   std::filesystem::path(output).extension().string().size());
        binding.second =
            ReplaceAll(std::move(binding.second), old_stem, new_stem);
      } else {
        binding.second =
            ReplaceAll(std::move(binding.second), old_output, output);
      }
    }
  }
  return clone.release();
}

// Replace complete dependency segments separately, keeping category counts
// correct even when the dynamic collection is an implicit input.
static void RefreshSourceSetInputs(
    Edge* edge, const std::map<std::string, size_t>& old_groups,
    const std::vector<std::vector<Node*>>& current) {
  std::set<size_t> inserted;
  auto segment = [&](size_t begin, size_t end) {
    std::vector<Node*> result;
    for (size_t i = begin; i < end; ++i) {
      auto group = old_groups.find(
          edge->inputs_[i]->globalPath().h.str_view().AsString());
      if (group == old_groups.end())
        result.push_back(edge->inputs_[i]);
      else if (inserted.insert(group->second).second)
        result.insert(result.end(), current[group->second].begin(),
                      current[group->second].end());
    }
    return result;
  };
  const size_t explicit_end = edge->explicit_deps_;
  const size_t implicit_end = explicit_end + edge->implicit_deps_;
  auto explicit_inputs = segment(0, explicit_end);
  auto implicit_inputs = segment(explicit_end, implicit_end);
  auto order_inputs = segment(implicit_end, edge->inputs_.size());
  // Empty original groups have no anchor. Append their new members to the
  // explicit segment; nonempty groups retain the category of their anchor.
  for (size_t i = 0; i < current.size(); ++i) {
    if (!inserted.count(i))
      explicit_inputs.insert(explicit_inputs.end(), current[i].begin(),
                             current[i].end());
  }
  edge->explicit_deps_ = static_cast<int>(explicit_inputs.size());
  edge->implicit_deps_ = static_cast<int>(implicit_inputs.size());
  edge->order_only_deps_ = static_cast<int>(order_inputs.size());
  explicit_inputs.insert(explicit_inputs.end(), implicit_inputs.begin(),
                         implicit_inputs.end());
  explicit_inputs.insert(explicit_inputs.end(), order_inputs.begin(),
                         order_inputs.end());
  edge->inputs_ = std::move(explicit_inputs);
}

static bool ApplyUniCppGroups(State* state, const std::vector<Clump*>& clumps,
                              SourceSetProducers* producers,
                              const std::vector<Edge*>& consumers,
                              SourceSetExpansionCache* expansion,
                              std::string* err) {
  std::set<std::string> old_outputs, current_outputs;
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
    std::map<std::string, size_t> old_groups;
    std::vector<std::vector<Node*>> current(groups.size());
    std::set<std::string> seen;
    for (size_t i = 0; i < groups.size(); ++i) {
      const auto& group = groups[i];
      std::map<std::string, std::string> previous;
      for (const auto& pair : group.previous) {
        previous[pair.first] = pair.second;
        old_groups[pair.second] = i;
        old_outputs.insert(pair.second);
      }
      std::vector<std::string> matches;
      if (!expansion->Expand({ group.pattern, group.excludes, {} }, &matches,
                             err))
        return false;
      for (const auto& source : matches) {
        if (!seen.insert(source).second)
          continue;
        std::string output;
        auto old = previous.find(source);
        if (old != previous.end())
          output = old->second;
        else {
          Edge* original = nullptr;
          std::string old_source, old_output;
          for (const auto& candidate : group.previous) {
            if (!DeriveObjectPath(source, candidate.first, candidate.second,
                                  &output))
              continue;
            original = FindCompileTemplate(*producers, candidate.first,
                                           candidate.second);
            if (original) {
              old_source = candidate.first;
              old_output = candidate.second;
              break;
            }
          }
          if (!original) {
            *err = "no compile template for new source '" + source +
                   "'; regenerate the Soong graph";
            return false;
          }
          if (producers->count(output) &&
              !FindCompileTemplate(*producers, source, output)) {
            *err = "conflicting dynamic compile output '" + output + "'";
            return false;
          }
          if (!producers->count(output)) {
            Edge* clone =
                CloneCompileEdge(state, original, old_source, old_output,
                                 source, output, false, err);
            if (!clone)
              return false;
            clumps.back()->edge_output_count_ += clone->explicit_outs_;
            clumps.back()->edges_.push_back(clone);
            for (Node* node : clone->outputs_)
              (*producers)[node->globalPath().h.str_view().AsString()] = clone;
          }
        }
        current_outputs.insert(output);
        Node* node = SourceSetNode(state, consumer, output, err);
        if (!node)
          return false;
        current[i].push_back(node);
      }
    }
    RefreshSourceSetInputs(consumer, old_groups, current);
    for (auto& binding : consumer->unevaled_bindings_) {
      if (binding.first.str() != "arObjs")
        continue;
      std::istringstream input(binding.second);
      std::ostringstream output;
      std::set<size_t> inserted;
      std::string token;
      while (input >> token) {
        auto group = old_groups.find(token);
        if (group == old_groups.end())
          output << token << ' ';
        else if (inserted.insert(group->second).second)
          for (Node* node : current[group->second])
            output << node->path() << ' ';
      }
      for (size_t i = 0; i < current.size(); ++i)
        if (!inserted.count(i))
          for (Node* node : current[i])
            output << node->path() << ' ';
      binding.second = output.str() + "\n";
    }
  }
  for (Clump* clump : clumps) {
    auto& edges = clump->edges_;
    for (auto it = edges.begin(); it != edges.end();) {
      Edge* edge = *it;
      bool obsolete = false;
      for (Node* node : edge->outputs_) {
        const std::string path = node->globalPath().h.str_view().AsString();
        if (old_outputs.count(path) && !current_outputs.count(path))
          obsolete = true;
      }
      if (!obsolete) {
        ++it;
        continue;
      }
      clump->edge_output_count_ -= edge->explicit_outs_;
      it = edges.erase(it);
      delete edge;
    }
  }
  return true;
}

static bool ApplyUniAaptGroups(State* state, const std::vector<Clump*>& clumps,
                               SourceSetProducers* producers,
                               const std::vector<Edge*>& compile_edges,
                               const std::vector<Edge*>& consumers,
                               SourceSetExpansionCache* expansion,
                               std::string* err) {
  std::set<std::string> expanded, old_outputs, current_outputs;
  for (Edge* compile : compile_edges) {
    std::string encoded;
    if (!compile->EvaluateVariable(&encoded, kRunaDynamicAaptInputs,
                                   compile->pos_.scope(), err))
      return false;
    std::vector<UniAaptGroup> groups;
    if (!DecodeUniAaptGroups(encoded, &groups)) {
      *err = "invalid runa_dynamic_aapt_inputs metadata";
      return false;
    }
    for (const auto& group : groups) {
      // Full declaration includes exclusions and mappings. Resource paths can
      // occur in multiple modules, each with a different compiled output.
      std::string key =
          group.pattern + "\n" + group.resource_root + "\n" + group.output_root;
      for (const auto& exclude : group.excludes)
        key += "\n!" + exclude;
      if (!expanded.insert(key).second)
        continue;
      std::map<std::string, std::string> previous;
      for (const auto& pair : group.previous) {
        previous[pair.first] = pair.second;
        old_outputs.insert(pair.second);
      }
      std::vector<std::string> matches;
      if (!expansion->Expand({ group.pattern, group.excludes, {} }, &matches,
                             err))
        return false;
      for (const auto& source : matches) {
        auto old = previous.find(source);
        std::string output;
        if (old != previous.end())
          output = old->second;
        else if (!DeriveAaptFlatPath(source, group.resource_root,
                                     group.output_root, &output)) {
          *err = "cannot map resource '" + source + "'";
          return false;
        }
        current_outputs.insert(output);
        if (producers->count(output) &&
            !FindCompileTemplate(*producers, source, output)) {
          *err = "conflicting dynamic resource output '" + output + "'";
          return false;
        }
        if (old != previous.end() || producers->count(output))
          continue;
        Edge* original = nullptr;
        std::string old_source, old_output;
        for (const auto& candidate : group.previous) {
          original = FindCompileTemplate(*producers, candidate.first,
                                         candidate.second);
          if (original) {
            old_source = candidate.first;
            old_output = candidate.second;
            break;
          }
        }
        if (!original || original->implicit_outs_ != 0) {
          *err = "no compatible resource compile template for '" + source +
                 "'; regenerate the Soong graph";
          return false;
        }
        Edge* clone = CloneCompileEdge(state, original, old_source, old_output,
                                       source, output, true, err);
        if (!clone)
          return false;
        clumps.back()->edge_output_count_ += clone->explicit_outs_;
        clumps.back()->edges_.push_back(clone);
        (*producers)[output] = clone;
      }
    }
  }
  // Remove deleted members from batched compile actions. Only declared
  // compile edges are edited; cloned single-file actions need no pruning.
  for (Edge* edge : compile_edges) {
    std::string encoded;
    if (!edge->EvaluateVariable(&encoded, kRunaDynamicAaptInputs,
                                edge->pos_.scope(), err))
      return false;
    std::vector<UniAaptGroup> groups;
    if (!DecodeUniAaptGroups(encoded, &groups))
      return false;
    std::set<std::string> removed_sources;
    for (const auto& group : groups) {
      for (const auto& pair : group.previous) {
        if (!current_outputs.count(pair.second))
          removed_sources.insert(pair.first);
      }
    }
    std::vector<Node*> inputs;
    for (int i = 0; i < edge->explicit_deps_; ++i) {
      Node* node = edge->inputs_[i];
      if (!removed_sources.count(node->globalPath().h.str_view().AsString()))
        inputs.push_back(node);
    }
    const int explicit_count = static_cast<int>(inputs.size());
    inputs.insert(inputs.end(), edge->inputs_.begin() + edge->explicit_deps_,
                  edge->inputs_.end());
    edge->inputs_ = std::move(inputs);
    edge->explicit_deps_ = explicit_count;
    const int old_count = edge->explicit_outs_;
    std::vector<Node*> outputs;
    for (int i = 0; i < old_count; ++i) {
      Node* node = edge->outputs_[i];
      const auto path = node->globalPath().h.str_view().AsString();
      if (!old_outputs.count(path) || current_outputs.count(path))
        outputs.push_back(node);
    }
    const int output_count = static_cast<int>(outputs.size());
    outputs.insert(outputs.end(), edge->outputs_.begin() + old_count,
                   edge->outputs_.end());
    edge->outputs_ = std::move(outputs);
    edge->explicit_outs_ = output_count;
  }
  for (Clump* clump : clumps) {
    auto& edges = clump->edges_;
    clump->edge_output_count_ = 0;
    for (auto it = edges.begin(); it != edges.end();) {
      if ((*it)->outputs_.empty()) {
        delete *it;
        it = edges.erase(it);
      } else {
        clump->edge_output_count_ += (*it)->explicit_outs_;
        ++it;
      }
    }
  }
  for (Edge* consumer : consumers) {
    std::string encoded;
    if (!consumer->EvaluateVariable(&encoded, kRunaDynamicAaptCompile,
                                    consumer->pos_.scope(), err))
      return false;
    std::vector<UniAaptGroup> groups;
    if (!DecodeUniAaptGroups(encoded, &groups)) {
      *err = "invalid runa_dynamic_aapt_compile metadata";
      return false;
    }
    std::vector<std::vector<Node*>> current(groups.size());
    std::map<std::string, size_t> old_groups;
    for (size_t i = 0; i < groups.size(); ++i) {
      const auto& group = groups[i];
      std::map<std::string, std::string> previous;
      for (const auto& pair : group.previous) {
        previous[pair.first] = pair.second;
        old_groups[pair.second] = i;
      }
      std::vector<std::string> matches;
      if (!expansion->Expand({ group.pattern, group.excludes, {} }, &matches,
                             err))
        return false;
      std::set<std::string> outputs;
      for (const auto& source : matches) {
        std::string output;
        auto old = previous.find(source);
        if (old != previous.end())
          output = old->second;
        else if (!DeriveAaptFlatPath(source, group.resource_root,
                                     group.output_root, &output)) {
          *err = "cannot map resource '" + source + "'";
          return false;
        }
        outputs.insert(output);
      }
      for (const auto& output : outputs) {
        Node* node = SourceSetNode(state, consumer, output, err);
        if (!node)
          return false;
        current[i].push_back(node);
      }
    }
    RefreshSourceSetInputs(consumer, old_groups, current);
  }
  return true;
}

static bool RefreshUniSourceInputs(State*, Edge*, SourceSetExpansionCache*,
                                   std::string*);

static bool ApplyUniMappedSourceSets(State* state,
                                     const std::vector<Clump*>& clumps,
                                     std::string* err) {
  SourceSetProducers producers;
  SourceSetExpansionCache expansion;
  std::vector<Edge*> cpp, compile, resources, source_edges;
  std::set<std::string> wanted_outputs;
  for (Clump* clump : clumps) {
    for (Edge* edge : clump->edges_) {
      std::string encoded;
      if (!edge->EvaluateVariable(&encoded, kRunaDynamicCpp, edge->pos_.scope(),
                                  err))
        return false;
      if (!encoded.empty()) {
        cpp.push_back(edge);
        std::vector<UniCppGroup> groups;
        if (!DecodeUniCppGroups(encoded, &groups)) {
          *err = "invalid runa_dynamic_cpp metadata";
          return false;
        }
        for (const auto& group : groups)
          for (const auto& pair : group.previous)
            wanted_outputs.insert(pair.second);
      }
      encoded.clear();
      if (!edge->EvaluateVariable(&encoded, kRunaDynamicAaptInputs,
                                  edge->pos_.scope(), err))
        return false;
      if (!encoded.empty()) {
        compile.push_back(edge);
        std::vector<UniAaptGroup> groups;
        if (!DecodeUniAaptGroups(encoded, &groups)) {
          *err = "invalid runa_dynamic_aapt_inputs metadata";
          return false;
        }
        for (const auto& group : groups)
          for (const auto& pair : group.previous)
            wanted_outputs.insert(pair.second);
      }
      encoded.clear();
      if (!edge->EvaluateVariable(&encoded, kRunaDynamicAaptCompile,
                                  edge->pos_.scope(), err))
        return false;
      if (!encoded.empty())
        resources.push_back(edge);
      encoded.clear();
      if (!edge->EvaluateVariable(&encoded, kRunaSourceGlobs,
                                  edge->pos_.scope(), err))
        return false;
      if (!encoded.empty())
        source_edges.push_back(edge);
    }
  }
  std::set<Edge*> templates;
  if (!wanted_outputs.empty()) {
    for (Clump* clump : clumps) {
      for (Edge* edge : clump->edges_) {
        for (Node* output : edge->outputs_) {
          const std::string path = output->globalPath().h.str_view().AsString();
          if (wanted_outputs.count(path)) {
            producers.emplace(path, edge);
            templates.insert(edge);
          }
        }
      }
    }
  }
  // Older manifests can tag a single-file compile as consuming a whole glob
  // when the original glob had one member. Do not expand that template's
  // inputs: instantiate separate compile actions through the mapped groups.
  for (Edge* edge : source_edges) {
    if (!templates.count(edge) &&
        !RefreshUniSourceInputs(state, edge, &expansion, err))
      return false;
  }
  return ApplyUniAaptGroups(state, clumps, &producers, compile, resources,
                            &expansion, err) &&
         ApplyUniCppGroups(state, clumps, &producers, cpp, &expansion, err);
}

static bool RefreshUniSourceInputs(State* state, Edge* edge,
                                   SourceSetExpansionCache* expansion,
                                   std::string* err) {
  std::string encoded;
  if (!edge->EvaluateVariable(&encoded, kRunaSourceGlobs, edge->pos_.scope(),
                              err))
    return false;
  if (encoded.empty())
    return true;
  std::vector<UniSourceGlob> groups;
  if (!DecodeUniSourceGlobs(encoded, &groups)) {
    *err = "invalid runa_source_globs metadata";
    return false;
  }
  std::set<std::string> old, current;
  for (const auto& group : groups) {
    old.insert(group.previous_matches.begin(), group.previous_matches.end());
    std::vector<std::string> matches;
    if (!expansion->Expand(group, &matches, err))
      return false;
    current.insert(matches.begin(), matches.end());
  }
  std::vector<Node*> inputs;
  std::set<std::string> present;
  for (int i = 0; i < edge->explicit_deps_; ++i) {
    Node* node = edge->inputs_[i];
    const auto path = node->globalPath().h.str_view().AsString();
    if (!old.count(path) && present.insert(path).second)
      inputs.push_back(node);
  }
  for (const auto& path : current) {
    if (!present.insert(path).second)
      continue;
    Node* node = SourceSetNode(state, edge, path, err);
    if (!node)
      return false;
    inputs.push_back(node);
  }
  const int count = static_cast<int>(inputs.size());
  inputs.insert(inputs.end(), edge->inputs_.begin() + edge->explicit_deps_,
                edge->inputs_.end());
  edge->inputs_ = std::move(inputs);
  edge->explicit_deps_ = count;
  return true;
}

#endif  // RUNA_MANIFEST_SOURCE_GLOBS_H_
