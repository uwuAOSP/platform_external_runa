/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RUNA_GRAPH_DELTA_H_
#define RUNA_GRAPH_DELTA_H_

#include <string>
#include <utility>
#include <vector>

// A complete edge description. Paths are relative to the context edge's scope.
// Bindings use Ninja expression syntax, exactly as on a manifest edge.
struct GraphDeltaEdge {
  std::string context_output;
  std::string rule;
  std::vector<std::string> outputs, implicit_outputs;
  std::vector<std::string> inputs, implicit_inputs, order_only_inputs;
  std::vector<std::string> validations;
  std::vector<std::pair<std::string, std::string>> bindings;
};

struct GraphDeltaOperation {
  enum Kind { Add, Remove, Replace };
  Kind kind = Add;
  // A canonical global output identifies an existing edge. For additions this
  // must name one of the new outputs. It is not a transient scheduler edge ID.
  std::string output;
  std::string expected_digest;
  GraphDeltaEdge edge;
};

#endif
