/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/service/layout_normalization.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/strings/str_join.h"
#include "tensorflow/compiler/xla/client/padding.h"
#include "tensorflow/compiler/xla/permutation_util.h"
#include "tensorflow/compiler/xla/service/dfs_hlo_visitor_with_default.h"
#include "tensorflow/compiler/xla/service/hlo_creation_utils.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/service/hlo_module.h"
#include "tensorflow/compiler/xla/service/pattern_matcher.h"
#include "tensorflow/compiler/xla/service/shape_inference.h"
#include "tensorflow/compiler/xla/shape.h"
#include "tensorflow/compiler/xla/statusor.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/stream_executor/lib/statusor.h"

namespace xla {
namespace {

// Layout normalization visitor. Aims to achieve the global postcondition that
// every layout is strictly descending (the layout permutation is effectively
// applied to the shape itself).
//
// Local precondition for every call:
//    -> Input is a bitcast from a normalized layout.
class LayoutNormalizationVisitor : public DfsHloRewriteVisitor {
 public:
  // Default action: ensure local postcondition that any input is always a
  // bitcast from canonical layout for any rewrites of the HLO users.
  //
  // Bitcast to descending layout and then bitcast back to make sure that shapes
  // match.
  Status DefaultAction(HloInstruction* hlo) override {
    if (!hlo->user_count()) {
      // The local postcondition does not have to apply to the case when there
      // are no users.
      return OkStatus();
    }
    auto users = hlo->users();
    auto shape = hlo->shape();
    if (shape.IsTuple() || shape.IsToken()) {
      // GTEs will be transformed individually, tokens should be skipped.
      return OkStatus();
    }

    auto normalized_shape = Normalize(shape);
    auto bc_to_normalized = MakeBitcastHlo(hlo, normalized_shape);
    auto bc_to_orig = MakeBitcastHlo(bc_to_normalized, shape);
    TF_RETURN_IF_ERROR(hlo->ReplaceUsesWith(users, bc_to_orig));
    MarkAsChanged();
    return OkStatus();
  }

 private:
  // Forces the layout to be descending and removes degenerate dimensions
  // without altering physical layout.
  Shape Normalize(const Shape& s) {
    return ShapeUtil::DropDegenerateDimensions(
        ShapeUtil::MakeShapeWithDescendingLayoutAndSamePhysicalLayout(s));
  }
};

}  // end namespace

StatusOr<bool> LayoutNormalization::Run(HloModule* module) {
  return LayoutNormalizationVisitor{}.RunOnModule(module);
}

}  // end namespace xla
