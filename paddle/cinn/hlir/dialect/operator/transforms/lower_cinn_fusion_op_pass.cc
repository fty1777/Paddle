// Copyright (c) 2024 PaddlePaddle Authors. All Rights Reserved.
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

#pragma once

#include "paddle/cinn/hlir/dialect/operator/transforms/lower_cinn_fusion_op_pass.h"

#include <unordered_map>

#include "paddle/cinn/adt/generate_map_expr.h"
#include "paddle/cinn/common/broadcast_tree.h"
#include "paddle/cinn/hlir/dialect/operator/ir/cinn_op.h"
#include "paddle/cinn/hlir/dialect/operator/ir/generate_shape_util.h"
#include "paddle/cinn/hlir/dialect/operator/ir/manual_op.h"
#include "paddle/cinn/hlir/dialect/operator/ir/op_attribute.h"
#include "paddle/cinn/hlir/dialect/operator/ir/op_dialect.h"
#include "paddle/cinn/hlir/dialect/operator/transforms/refresh_combine_pattern.h"
#include "paddle/cinn/hlir/dialect/runtime/ir/jit_kernel_op.h"
#include "paddle/cinn/hlir/dialect/runtime/ir/runtime_dialect.h"
#include "paddle/cinn/hlir/framework/pir/compilation_cache.h"
#include "paddle/cinn/hlir/framework/pir/op_lowering_group.h"
#include "paddle/cinn/hlir/framework/pir/utils.h"
#include "paddle/cinn/hlir/framework/pir_compiler.h"
#include "paddle/cinn/runtime/flags.h"
#include "paddle/fluid/pir/dialect/kernel/ir/kernel_dialect.h"
#include "paddle/fluid/pir/dialect/operator/ir/control_flow_op.h"
#include "paddle/fluid/pir/dialect/operator/ir/manual_op.h"
#include "paddle/fluid/pir/dialect/operator/ir/pd_op.h"
#include "paddle/pir/include/core/program.h"
#include "paddle/pir/include/dialect/control_flow/ir/cf_op.h"
#include "paddle/pir/include/dialect/shape/utils/dim_expr_util.h"
#include "paddle/pir/include/dialect/shape/utils/shape_or_data_expr.h"
#include "paddle/pir/include/pass/pass_registry.h"
#include "paddle/pir/include/pattern_rewrite/frozen_rewrite_pattern_set.h"

PD_DECLARE_bool(cinn_enable_map_expr);

namespace {
using OpLoweringGroup = cinn::hlir::framework::pir::OpLoweringGroup;
using OpLoweringGroupPtr = std::shared_ptr<OpLoweringGroup>;
using GroupInfoMap = std::unordered_map<::pir::Operation*, OpLoweringGroupPtr>;
using cinn::hlir::framework::pir::CompatibleInfo;
using SharedGroupHasher = OpLoweringGroup::SharedGroupHasher;
using SharedGroupComparator = OpLoweringGroup::SharedGroupComparator;
using ShapeOrDataDimExprs4ValueT =
    std::function<const symbol::ShapeOrDataDimExprs&(pir::Value)>;
using cinn::hlir::framework::CompilationCache;
using cinn::hlir::framework::PirCompiler;
using cinn::hlir::framework::pir::CINNKernelInfo;

class BroadcastTreeInfo;
using BroadcastTreeInfoMap =
    std::unordered_map<OpLoweringGroupPtr,
                       std::shared_ptr<BroadcastTreeInfo>,
                       SharedGroupHasher,
                       SharedGroupComparator>;

class BroadcastTreeInfo final {
 public:
  explicit BroadcastTreeInfo(const OpLoweringGroupPtr& group) {
    ConstructBroadcastTree(group);
  }
  const std::shared_ptr<cinn::common::BroadcastTree>& GetBroadcastTree() const;
  const cinn::adt::List<std::vector<symbol::DimExpr>> GetAllValueDimExprs()
      const;
  const std::unordered_map<pir::Value, size_t>& GetValueToDimExprIdx() const;
  bool HasMultiBranch() const;

 private:
  void ConstructBroadcastTree(const OpLoweringGroupPtr& group);

  std::shared_ptr<cinn::common::BroadcastTree> broadcast_tree_;
  cinn::adt::List<std::vector<symbol::DimExpr>> all_value_dim_exprs_;
  std::unordered_map<pir::Value, size_t> value_to_dim_expr_idx_;
};

struct PreAnalysisInfo {
  GroupInfoMap group_infos;
  BroadcastTreeInfoMap broadcast_tree_infos;
};

class FusionOpAnalysis final {
 public:
  FusionOpAnalysis(PreAnalysisInfo* pre_analysis_info, bool is_dy_shape)
      : pre_analysis_info_(pre_analysis_info), is_dy_shape_(is_dy_shape) {}
  void Run(pir::Operation* module_op) {
    RunImpl(module_op);
    PreCompileGroup();
  }

 protected:
  void RunImpl(pir::Operation* op);
  void GatherGroup(pir::Operation* fusion_op);
  void PreCompileGroup();

 private:
  PreAnalysisInfo* pre_analysis_info_;  // not_owned
  bool is_dy_shape_;
};

std::vector<pir::Value> GetBlockOutsideInput(
    const std::vector<pir::Operation*>& ops);

pir::Operation* ProcessDyShapeGroup(
    const OpLoweringGroupPtr& group,
    pir::ShapeConstraintIRAnalysis& shape_analysis,  // NOLINT
    const PreAnalysisInfo& pre_analysis_info,
    pir::PatternRewriter& rewriter  // NOLINT
);

std::unordered_map<std::string, ::pir::Attribute> GetJitKernelAttr(
    const OpLoweringGroupPtr& group) {
  auto kernel_info = CompilationCache::Instance().GetKernelInfo(group);
  std::unordered_map<std::string, ::pir::Attribute> attrs{
      {cinn::dialect::JitKernelOp::kAttrName,
       cinn::dialect::CINNKernelInfoAttribute::get(pir::IrContext::Instance(),
                                                   kernel_info)}};
  return attrs;
}

class FusionOpPattern : public pir::OpRewritePattern<cinn::dialect::FusionOp> {
 public:
  FusionOpPattern(::pir::IrContext* context,
                  const PreAnalysisInfo& pre_analysis_info)
      : pir::OpRewritePattern<cinn::dialect::FusionOp>(context),
        pre_analysis_info_(pre_analysis_info) {}

  bool MatchAndRewrite(cinn::dialect::FusionOp fusion_op,
                       pir::PatternRewriter& rewriter) const override {
    ::pir::IrContext* ctx = ::pir::IrContext::Instance();
    auto* program = fusion_op->GetParentProgram();
    auto& shape_analysis = pir::ShapeAnalysisManager::Instance().Get(program);
    VLOG(4) << "Program before lowering: \n"
            << pir::CustomPrintHelper(*program, shape_analysis.PrintHook());

    // TODO(zhangyuqin1998): Replace pir::Group with a new structure
    OpLoweringGroupPtr group = GetGroup(fusion_op);
    pir::Operation* compiled_op = ProcessGroup(group, shape_analysis, rewriter);

    for (size_t i = 0; i < fusion_op.num_results(); ++i) {
      rewriter.ReplaceAllUsesWith(fusion_op.result(i), compiled_op->result(i));
      if (shape_analysis.HasShapeOrDataForValue(fusion_op.result(i))) {
        shape_analysis.SetShapeOrDataForValue(
            compiled_op->result(i),
            shape_analysis.GetShapeOrDataForValue(fusion_op.result(i)));
      } else {
        LOG(WARNING) << "No shape_data for "
                     << fusion_op.result(i).defining_op()->name() << "_result_"
                     << i;
      }
    }
    rewriter.EraseOp(fusion_op);
    return true;
  }

 protected:
  virtual const PreAnalysisInfo& GetPreAnalysisInfo() const {
    return pre_analysis_info_;
  }

  virtual OpLoweringGroupPtr GetGroup(cinn::dialect::FusionOp fusion_op) const {
    return pre_analysis_info_.group_infos.at(fusion_op.operation());
  }

  virtual pir::Operation* ProcessGroup(
      const OpLoweringGroupPtr& group,
      pir::ShapeConstraintIRAnalysis& shape_analysis,  // NOLINT
      pir::PatternRewriter& rewriter) const {          // NOLINT
    auto group_inputs = GetBlockOutsideInput(group->ops());
    // compile group to jit_kernel_op
    std::vector<pir::Type> output_types;
    const auto& group_output_values = group->output_values();
    for (size_t i = 0; i < group_output_values.size(); ++i) {
      output_types.push_back(group_output_values[i].type());
    }
    auto jit_kernel_op = rewriter.Build<cinn::dialect::JitKernelOp>(
        group_inputs, GetJitKernelAttr(group), output_types);
    return jit_kernel_op;
  }

 private:
  const PreAnalysisInfo& pre_analysis_info_;  // not owned
};

class LowerCinnFusionOpPass : public pir::PatternRewritePass {
 public:
  LowerCinnFusionOpPass()
      : pir::PatternRewritePass("lower_cinn_fusion_op", 1) {}

  pir::RewritePatternSet InitializePatterns(pir::IrContext* context) override {
    context->GetOrRegisterDialect<cinn::dialect::RuntimeDialect>();
    context->GetOrRegisterDialect<cinn::dialect::OperatorDialect>();

    pir::RewritePatternSet ps(context);
    ps.Add<FusionOpPattern>(context, pre_analysis_info_);
    return ps;
  }

  bool CanApplyOn(pir::Operation* op) const override {
    if (op->isa<pir::ModuleOp>()) {
      VLOG(5) << "start to pre-analysis all fusion ops in ModuleOp with static "
                 "shape mode.";
      FusionOpAnalysis(&pre_analysis_info_, /*is_dy_shape=*/false).Run(op);
    }
    return op->num_regions() > 0;
  }

 private:
  mutable PreAnalysisInfo pre_analysis_info_;
};

class DyShapeFusionOpPattern : public FusionOpPattern {
 public:
  using FusionOpPattern::FusionOpPattern;

 protected:
  virtual pir::Operation* ProcessGroup(
      const OpLoweringGroupPtr& group,
      pir::ShapeConstraintIRAnalysis& shape_analysis,  // NOLINT
      pir::PatternRewriter& rewriter) const {          // NOLINT
    return ProcessDyShapeGroup(
        group, shape_analysis, GetPreAnalysisInfo(), rewriter);
  }
};

class LowerCinnDyShapeFusionOpPass : public pir::PatternRewritePass {
 public:
  LowerCinnDyShapeFusionOpPass()
      : pir::PatternRewritePass("lower_cinn_dynamic_shape_fusion_op", 1) {}

  pir::RewritePatternSet InitializePatterns(pir::IrContext* context) override {
    context->GetOrRegisterDialect<cinn::dialect::RuntimeDialect>();
    context->GetOrRegisterDialect<cinn::dialect::OperatorDialect>();

    pir::RewritePatternSet ps(context);
    ps.Add<DyShapeFusionOpPattern>(context, pre_analysis_info_);
    ps.Add<RefreshCombineOpPattern>(context);

    return ps;
  }

  bool CanApplyOn(pir::Operation* op) const override {
    if (op->isa<pir::ModuleOp>()) {
      VLOG(5) << "start to pre-analysis all fusion ops in ModuleOp with "
                 "dynamic shape mode.";
      FusionOpAnalysis(&pre_analysis_info_, /*is_dy_shape=*/true).Run(op);
    }
    return op->num_regions() > 0;
  }

 private:
  mutable PreAnalysisInfo pre_analysis_info_;
};

OpLoweringGroupPtr RebuildGroup(pir::Operation* fusion_op, bool is_dy_shape);

void FusionOpAnalysis::GatherGroup(pir::Operation* fusion_op) {
  OpLoweringGroupPtr group_ptr = RebuildGroup(fusion_op, is_dy_shape_);
  VLOG(6) << "Gather Group " << group_ptr->FuncName()
          << " for fusion_op : " << fusion_op->id();
  pre_analysis_info_->group_infos.insert({fusion_op, group_ptr});
  if (is_dy_shape_) {
    auto broadcast_tree_info = std::make_shared<BroadcastTreeInfo>(group_ptr);
    pre_analysis_info_->broadcast_tree_infos.insert(
        {group_ptr, broadcast_tree_info});
  }
}

void FusionOpAnalysis::RunImpl(pir::Operation* op) {
  if (op->isa<cinn::dialect::FusionOp>()) {
    GatherGroup(op);
    return;
  }
  for (uint32_t i = 0; i < op->num_regions(); ++i) {
    for (auto& block : op->region(i)) {
      for (auto& op : block) {
        RunImpl(&op);
      }
    }
  }
}

void FusionOpAnalysis::PreCompileGroup() {
  std::vector<OpLoweringGroupPtr> groups;
  const auto& EnqueueGroup = [&](const OpLoweringGroupPtr& group) {
    const bool has_broadcast_tree =
        pre_analysis_info_->broadcast_tree_infos.count(group) > 0;
    if (has_broadcast_tree) {
      const auto broadcast_tree =
          pre_analysis_info_->broadcast_tree_infos.at(group);
      if (broadcast_tree->HasMultiBranch()) {
        return;  // do nothing
      }
    }
    groups.push_back(group);
  };
  for (auto& group_info : pre_analysis_info_->group_infos) {
    EnqueueGroup(group_info.second);
  }
  // Build and trigger compilaion cache.
  VLOG(4) << "Parallel Pre-Compile for Group with size: " << groups.size();
  PirCompiler pir_compiler(cinn::common::DefaultNVGPUTarget());
  pir_compiler.Build(groups);
}

const std::shared_ptr<cinn::common::BroadcastTree>&
BroadcastTreeInfo::GetBroadcastTree() const {
  return broadcast_tree_;
}

const cinn::adt::List<std::vector<symbol::DimExpr>>
BroadcastTreeInfo::GetAllValueDimExprs() const {
  return all_value_dim_exprs_;
}

const std::unordered_map<pir::Value, size_t>&
BroadcastTreeInfo::GetValueToDimExprIdx() const {
  return value_to_dim_expr_idx_;
}

bool BroadcastTreeInfo::HasMultiBranch() const {
  return broadcast_tree_
      ->Has<cinn::common::BroadcastBranch<cinn::common::BroadcastTree>>();
}

void BroadcastTreeInfo::ConstructBroadcastTree(
    const OpLoweringGroupPtr& group) {
  std::unordered_set<pir::Value> value_view;
  group->WalkOps([&group, &value_view](pir::Operation* op) {
    for (size_t i = 0; i < op->num_operands(); ++i) {
      value_view.insert(op->operand_source(i));
    }
    for (size_t i = 0; i < op->num_results(); ++i) {
      value_view.insert(op->result(i));
    }
  });
  // construct broadcast tree
  VLOG(4) << "construct broadcast tree";
  for (auto value : value_view) {
    const auto& shape_dim_expr = group->GetShapeOrDataExprs(value);
    const auto& data_shape = shape_dim_expr.data();
    if (data_shape) {
      all_value_dim_exprs_->push_back(*data_shape);
    } else {
      all_value_dim_exprs_->push_back(shape_dim_expr.shape());
    }
    value_to_dim_expr_idx_[value] = all_value_dim_exprs_->size() - 1;
  }
  VLOG(6) << "before constructed. broadcast-leaf: \n"
          << ToTxtString(cinn::common::BroadcastTree(all_value_dim_exprs_));
  broadcast_tree_ = std::make_shared<cinn::common::BroadcastTree>(
      cinn::common::ConstructBroadcastTree(
          cinn::common::BroadcastLeaf(all_value_dim_exprs_)));
  VLOG(4) << "broadcast-tree: \n" << ToTxtString(*broadcast_tree_);
}

pir::Operation* CompileBroadcastTreeToConditionBlock(
    const BroadcastTreeInfo& broadcast_tree_info,
    const OpLoweringGroupPtr& group,
    pir::ShapeConstraintIRAnalysis& shape_analysis,  // NOLINT
    const std::vector<pir::Value>& group_inputs,
    const std::vector<pir::Type>& output_types,
    pir::PatternRewriter& rewriter  // NOLINT
);

pir::Operation* ProcessDyShapeGroup(
    const OpLoweringGroupPtr& group,
    pir::ShapeConstraintIRAnalysis& shape_analysis,  // NOLINT
    const PreAnalysisInfo& pre_analysis_info,
    pir::PatternRewriter& rewriter) {  // NOLINT
  // 1. construct broadcast tree
  const auto& broadcast_tree_info =
      pre_analysis_info.broadcast_tree_infos.at(group);
  auto group_inputs = GetBlockOutsideInput(group->ops());
  // has multiple branch
  if (broadcast_tree_info->HasMultiBranch()) {
    std::vector<pir::Type> output_types;
    auto group_output_values = group->GetGroupOutputValues();
    for (size_t i = 0; i < group_output_values.size(); ++i) {
      output_types.push_back(group_output_values[i].type());
    }
    return CompileBroadcastTreeToConditionBlock(*broadcast_tree_info,
                                                group,
                                                shape_analysis,
                                                group_inputs,
                                                output_types,
                                                rewriter);
  } else {  // no condition block
    // compile group to jit_kernel_op
    std::vector<pir::Type> output_types;
    const auto& group_output_values = group->output_values();
    for (size_t i = 0; i < group_output_values.size(); ++i) {
      auto base_type =
          group_output_values[i].type().dyn_cast<::pir::DenseTensorType>();
      auto dim_info = base_type.dims();
      if (shape_analysis.HasShapeOrDataForValue(group_output_values[i])) {
        auto shape = group->GetShapeOrDataExprs(group_output_values[i]).shape();
        for (size_t k = 0; k < shape.size(); ++k) {
          if (shape[k].isa<int64_t>()) {
            dim_info[k] = shape[k].Get<int64_t>();
          }
        }
      }
      auto new_type = ::pir::DenseTensorType::get(pir::IrContext::Instance(),
                                                  base_type.dtype(),
                                                  dim_info,
                                                  base_type.data_layout(),
                                                  base_type.lod(),
                                                  base_type.offset());
      output_types.push_back(new_type);
    }
    auto jit_kernel_op = rewriter.Build<cinn::dialect::JitKernelOp>(
        group_inputs, GetJitKernelAttr(group), output_types);
    return jit_kernel_op;
  }
}

std::unordered_map<::pir::Value, symbol::ShapeOrDataDimExprs>
CreateGroupShapeOrDataExprs(
    const OpLoweringGroupPtr& group,
    pir::ShapeConstraintIRAnalysis& shape_analysis  // NOLINT
);

OpLoweringGroupPtr RebuildGroup(pir::Operation* fusion_op_ptr,
                                bool is_dy_shape) {
  auto fusion_op = fusion_op_ptr->dyn_cast<cinn::dialect::FusionOp>();
  auto group = std::make_shared<OpLoweringGroup>();
  group->set_op_pattern_kind(
      cinn::hlir::framework::OpPatternKind::kElementWise);
  if (fusion_op.attributes().count("group_info")) {
    auto attr = fusion_op.attribute("group_info")
                    .dyn_cast<cinn::dialect::GroupInfoAttribute>()
                    .data();

    group->set_op_pattern_kind(attr.op_pattern_kind);
    group->set_loop_ranges(attr.loop_ranges);
    group->set_loop_ranges_expr(attr.loop_ranges_expr);

    group->set_reduce_axis(attr.reduce_axis);
    group->set_alignment_schedule_info(attr.alignment_schedule_info);
  }

  // Rebuild ops of the group
  for (auto op : fusion_op.GetOperators()) {
    if (!op->isa<::pir::YieldOp>()) {
      group->mut_ops().push_back(op);
      auto op_pattern_kind = static_cast<int>(CompatibleInfo::OpKind(*op)) >
                                     static_cast<int>(group->op_pattern_kind())
                                 ? CompatibleInfo::OpKind(*op)
                                 : group->op_pattern_kind();
      group->set_op_pattern_kind(op_pattern_kind);
    }
  }

  // Rebuild output_ops and input_ops of the group
  auto yield_op = fusion_op.GetOperators().back();
  for (size_t i = 0; i < yield_op->num_operands(); ++i) {
    auto in = yield_op->operand_source(i);
    group->mut_output_values().push_back(in);
    group->mut_output_ops().insert(in.defining_op());
  }

  // Because the group is rebuilt, the order of group.output_values generated
  // by BuildCUDAJITInfo may not be same with the order bound in the yield op,
  // so a mapping is required.
  auto& shape_analysis =
      pir::ShapeAnalysisManager::Instance().Get(fusion_op->GetParentProgram());
  group->set_value_to_shape_or_data_exprs(
      CreateGroupShapeOrDataExprs(group, shape_analysis));
  if (FLAGS_cinn_enable_map_expr) {
    cinn::adt::TryGenerateMapExprFromGroup(group);
  }
  // Rebuild other informations
  // TODO(zhangyuqin1998): Do we need group.master_ops?
  return group;
}

bool SameInputOutputShape(
    paddle::dialect::ExpandOp expand_op,
    const ShapeOrDataDimExprs4ValueT& ShapeOrDataDimExprs4Value) {
  const auto& x = ShapeOrDataDimExprs4Value(expand_op.x());
  const auto& shape = ShapeOrDataDimExprs4Value(expand_op.shape());
  const auto& out = ShapeOrDataDimExprs4Value(expand_op.out());
  if (x.data().has_value()) return false;
  if (!shape.data().has_value()) return false;
  if (out.data().has_value()) return false;
  CHECK(shape.data().value() == out.shape());
  return x.shape() == out.shape();
}

// Returns true if success
bool EraseOneExpand(
    pir::Block* block,
    pir::PatternRewriter& rewriter,  // NOLINT
    const ShapeOrDataDimExprs4ValueT& ShapeOrDataDimExprs4Value) {
  for (auto expand_it = block->begin(); expand_it != block->end();
       ++expand_it) {
    if (!expand_it->isa<paddle::dialect::ExpandOp>()) continue;
    auto expand = expand_it->dyn_cast<paddle::dialect::ExpandOp>();
    if (!SameInputOutputShape(expand, ShapeOrDataDimExprs4Value)) continue;
    auto generate_shape_op =
        expand.shape().defining_op<cinn::dialect::GenerateShapeOp>();
    CHECK_NOTNULL(generate_shape_op);
    rewriter.ReplaceAllUsesWith(expand.out(), expand.x());
    rewriter.EraseOp(expand);
    if (generate_shape_op->use_empty()) {
      rewriter.EraseOp(generate_shape_op);
    }
    return true;
  }
  return false;
}

void EraseUnnecessaryExpandsInBlock(
    pir::Block* block,
    pir::PatternRewriter& rewriter,  // NOLINT
    const ShapeOrDataDimExprs4ValueT& ShapeOrDataDimExprs4Value) {
  while (EraseOneExpand(block, rewriter, ShapeOrDataDimExprs4Value)) {
    // Do nothing.
  }
}

void ReplaceExpandWithBroadcast(pir::IrContext* ir_context,
                                pir::Block* block,
                                const OpLoweringGroupPtr& group) {
  std::vector<pir::Operation*> op_list;
  for (auto& op : *block) {
    op_list.push_back(&op);
  }
  pir::Builder builder(ir_context, block);
  for (auto* op : op_list) {
    if (op && op->isa<paddle::dialect::ExpandOp>() &&
        op->operand_source(1)
            .defining_op()
            ->isa<cinn::dialect::GenerateShapeOp>()) {
      builder.SetInsertionPointAfter(op);
      auto x_rank = op->operand_source(0)
                        .type()
                        .dyn_cast<pir::ShapedTypeInterface>()
                        .GetRank();
      auto out_rank =
          op->result(0).type().dyn_cast<pir::ShapedTypeInterface>().GetRank();
      std::vector<int64_t> broadcast_axes(x_rank, 0);
      size_t index_gap = out_rank - x_rank;
      for (size_t i = 0; i < x_rank; ++i) {
        broadcast_axes[i] = i + index_gap;
      }
      std::vector<int64_t> out_shape(out_rank, -1);
      auto broadcast = builder.Build<cinn::dialect::BroadcastOp>(
          op->operand_source(0), broadcast_axes, out_shape);
      auto broadcast_out = broadcast.result(0);
      auto expand_out = op->result(0);
      expand_out.ReplaceAllUsesWith(broadcast_out);
      group->SetShapeOrDataExprs(broadcast_out,
                                 group->GetShapeOrDataExprs(expand_out));
      CHECK(op->use_empty());
      auto generate_shape_op = op->operand_source(1).defining_op();
      op->Erase();
      if (generate_shape_op->use_empty()) {
        generate_shape_op->Erase();
      }
    }
  }
}

std::vector<pir::Value> GetBlockOutsideInput(
    const std::vector<pir::Operation*>& op_list) {
  std::vector<pir::Value> vec_res;
  std::unordered_set<::pir::Value> block_inner_output;
  for (size_t k = 0; k < op_list.size(); ++k) {
    for (size_t i = 0; i < op_list[k]->num_results(); ++i) {
      block_inner_output.insert(op_list[k]->result(i));
    }
  }

  std::unordered_set<::pir::Value> insert_value;
  for (size_t k = 0; k < op_list.size(); ++k) {
    for (size_t i = 0; i < op_list[k]->num_operands(); ++i) {
      if (!block_inner_output.count(op_list[k]->operand_source(i)) &&
          !insert_value.count(op_list[k]->operand_source(i))) {
        vec_res.push_back(op_list[k]->operand_source(i));
        insert_value.insert(op_list[k]->operand_source(i));
      }
    }
  }
  return vec_res;
}

std::tuple<pir::Value, pir::Value, pir::Value> BroadcastableToCondValue(
    const symbol::Broadcastable<symbol::DimExpr>& broadcastable_condition,
    pir::ShapeConstraintIRAnalysis& shape_analysis,  // NOLINT
    const std::vector<pir::Value>& group_inputs,
    pir::Builder& builder) {  // NOLINT
  const auto& lhs_expr = broadcastable_condition->lhs;
  const auto& rhs_expr = broadcastable_condition->rhs;
  auto ShapeOrDataDimExprs4Value = [&shape_analysis](pir::Value value) {
    return shape_analysis.GetShapeOrDataForValue(value);
  };

  std::vector<pir::Value> lhs_minimal_inputs;
  std::vector<pir::Attribute> lhs_output_dim_expr_attrs;
  cinn::dialect::GenerateShapeOp::SymbolBindings lhs_symbol_bindings;
  bool success =
      cinn::dialect::MakeGenerateShapeOpAttribute(builder.ir_context(),
                                                  ShapeOrDataDimExprs4Value,
                                                  {lhs_expr},
                                                  group_inputs,
                                                  &lhs_minimal_inputs,
                                                  &lhs_output_dim_expr_attrs,
                                                  &lhs_symbol_bindings);
  CHECK(success);
  std::vector<pir::Value> rhs_minimal_inputs;
  std::vector<pir::Attribute> rhs_output_dim_expr_attrs;
  cinn::dialect::GenerateShapeOp::SymbolBindings rhs_symbol_bindings;
  success =
      cinn::dialect::MakeGenerateShapeOpAttribute(builder.ir_context(),
                                                  ShapeOrDataDimExprs4Value,
                                                  {rhs_expr},
                                                  group_inputs,
                                                  &rhs_minimal_inputs,
                                                  &rhs_output_dim_expr_attrs,
                                                  &rhs_symbol_bindings);
  CHECK(success);

  auto lhs_value =
      builder
          .Build<cinn::dialect::GenerateShapeOp>(lhs_minimal_inputs,
                                                 lhs_output_dim_expr_attrs,
                                                 lhs_symbol_bindings)
          .out();
  auto rhs_value =
      builder
          .Build<cinn::dialect::GenerateShapeOp>(rhs_minimal_inputs,
                                                 rhs_output_dim_expr_attrs,
                                                 rhs_symbol_bindings)
          .out();

  auto const_one = builder
                       .Build<paddle::dialect::FullOp>(
                           std::vector<int64_t>{1}, 1, phi::DataType::INT64)
                       .out();
  auto lhs_eq_rhs_cond =
      builder.Build<paddle::dialect::EqualOp>(lhs_value, rhs_value).out();
  auto lhs_eq_one_cond =
      builder.Build<paddle::dialect::EqualOp>(lhs_value, const_one).out();
  auto rhs_eq_one_cond =
      builder.Build<paddle::dialect::EqualOp>(rhs_value, const_one).out();
  return std::tuple<pir::Value, pir::Value, pir::Value>(
      lhs_eq_rhs_cond, lhs_eq_one_cond, rhs_eq_one_cond);
}

OpLoweringGroupPtr CloneGroup(const OpLoweringGroupPtr& group,
                              pir::Block* block,
                              pir::IrMapping* ir_mapping) {
  return group->Clone(block, ir_mapping);
}

void UpdateGroupShapeExprs(
    const OpLoweringGroupPtr& new_group,
    const OpLoweringGroupPtr& origin_group,
    const pir::IrMapping& ir_mapping,
    const cinn::common::BroadcastLeaf& value_dim_exprs_list,
    const std::unordered_map<pir::Value, size_t>& value_to_dim_expr_idx) {
  for (const auto& [origin_val, new_val] : ir_mapping.GetMap<pir::Value>()) {
    const auto& shape_dim_expr =
        value_dim_exprs_list->at(value_to_dim_expr_idx.at(origin_val));
    const auto& origin_shape_or_data =
        origin_group->GetShapeOrDataExprs(origin_val);
    if (origin_shape_or_data.data()) {
      new_group->SetShapeOrDataExprs(
          new_val,
          symbol::ShapeOrDataDimExprs{symbol::TensorShapeOrDataDimExprs(
              std::vector<symbol::DimExpr>{shape_dim_expr.size()},
              shape_dim_expr)});
    } else {
      new_group->SetShapeOrDataExprs(
          new_val,
          symbol::ShapeOrDataDimExprs{
              symbol::TensorShapeOrDataDimExprs(shape_dim_expr)});
    }
  }
}

void SetLeafBlockByGroupView(
    const OpLoweringGroupPtr& origin_group,
    const cinn::common::BroadcastLeaf& value_dim_exprs_list,
    const std::unordered_map<pir::Value, size_t>& value_to_dim_expr_idx,
    pir::Builder& builder,  // NOLINT
    pir::Block* block,
    std::unordered_map<pir::Block*, OpLoweringGroupPtr>* group_map) {
  pir::IrMapping ir_mapping;
  auto origin_group_inputs = GetBlockOutsideInput(origin_group->ops());
  for (auto input : origin_group_inputs) {
    ir_mapping.Add(input, input);
  }

  auto new_group = CloneGroup(origin_group, block, &ir_mapping);
  CHECK_EQ(origin_group->ops().size(), new_group->ops().size());
  UpdateGroupShapeExprs(new_group,
                        origin_group,
                        ir_mapping,
                        value_dim_exprs_list,
                        value_to_dim_expr_idx);

  // Insert YieldOp for outputs
  std::vector<pir::Value> outputs;
  builder.SetInsertionPointToBlockEnd(block);
  for (auto output : origin_group->GetGroupOutputValues()) {
    outputs.push_back(ir_mapping.Lookup(output));
  }
  builder.Build<pir::YieldOp>(outputs);

  group_map->insert({block, new_group});
}

std::vector<pir::Value> GetOpOuputValues(const pir::Operation* op) {
  std::vector<pir::Value> outputs;
  outputs.reserve(op->num_results());
  for (size_t i = 0; i < op->num_results(); ++i) {
    outputs.push_back(op->result(i));
  }
  return outputs;
}

void InsertYieldOpForCondBlock(pir::Operation* cond_op,
                               pir::Builder& builder) {  // NOLINT
  if (cond_op) {
    builder.SetInsertionPointAfter(cond_op);
    builder.Build<pir::YieldOp>(GetOpOuputValues(cond_op));
  }
}

// Visit broadcast_tree by dfs
pir::Operation* CreateConditionBlock(
    const cinn::common::BroadcastTree& broadcast_tree,
    const OpLoweringGroupPtr& origin_group,
    pir::ShapeConstraintIRAnalysis& shape_analysis,  // NOLINT
    const std::unordered_map<pir::Value, size_t>& value_to_dim_expr_idx,
    const std::vector<pir::Value>& group_inputs,
    const std::vector<pir::Type>& output_types,
    pir::Builder& builder,  // NOLINT
    pir::Block* block,
    std::unordered_map<pir::Block*, OpLoweringGroupPtr>* group_map) {
  if (broadcast_tree.Has<cinn::common::BroadcastLeaf>()) {
    const auto& broadcast_leaf =
        broadcast_tree.Get<cinn::common::BroadcastLeaf>();
    SetLeafBlockByGroupView(origin_group,
                            broadcast_leaf,
                            value_to_dim_expr_idx,
                            builder,
                            block,
                            group_map);
    return nullptr;
  } else {
    const auto& branch =
        broadcast_tree
            .Get<cinn::common::BroadcastBranch<cinn::common::BroadcastTree>>();
    const auto& [lhs_eq_rhs_cond, lhs_eq_one_cond, rhs_eq_one_cond] =
        BroadcastableToCondValue(
            branch.Get<0>(), shape_analysis, group_inputs, builder);

    // lhs == rhs
    auto lhs_eq_rhs_cond_op = builder.Build<paddle::dialect::IfOp>(
        lhs_eq_rhs_cond, std::vector<pir::Type>{output_types});
    pir::Block& lhs_eq_rhs_block = lhs_eq_rhs_cond_op.true_block();
    builder.SetInsertionPointToBlockEnd(&lhs_eq_rhs_block);
    auto* lhs_eq_rhs_block_op = CreateConditionBlock(branch.Get<1>(),
                                                     origin_group,
                                                     shape_analysis,
                                                     value_to_dim_expr_idx,
                                                     group_inputs,
                                                     output_types,
                                                     builder,
                                                     &lhs_eq_rhs_block,
                                                     group_map);
    InsertYieldOpForCondBlock(lhs_eq_rhs_block_op, builder);

    pir::Block& lhs_not_eq_rhs_block = lhs_eq_rhs_cond_op.false_block();
    builder.SetInsertionPointToBlockEnd(&lhs_not_eq_rhs_block);

    // lhs != rhs && lhs == 1
    auto lhs_eq_one_cond_op = builder.Build<paddle::dialect::IfOp>(
        lhs_eq_one_cond, std::vector<pir::Type>{output_types});
    pir::Block& lhs_eq_one_block = lhs_eq_one_cond_op.true_block();
    builder.SetInsertionPointToBlockEnd(&lhs_eq_one_block);
    auto* lhs_eq_one_block_op = CreateConditionBlock(branch.Get<2>(),
                                                     origin_group,
                                                     shape_analysis,
                                                     value_to_dim_expr_idx,
                                                     group_inputs,
                                                     output_types,
                                                     builder,
                                                     &lhs_eq_one_block,
                                                     group_map);
    InsertYieldOpForCondBlock(lhs_eq_one_block_op, builder);

    // lhs != rhs && rhs == 1
    pir::Block& rhs_eq_one_block = lhs_eq_one_cond_op.false_block();
    builder.SetInsertionPointToBlockEnd(&rhs_eq_one_block);
    auto* rhs_eq_one_block_op = CreateConditionBlock(branch.Get<3>(),
                                                     origin_group,
                                                     shape_analysis,
                                                     value_to_dim_expr_idx,
                                                     group_inputs,
                                                     output_types,
                                                     builder,
                                                     &rhs_eq_one_block,
                                                     group_map);
    InsertYieldOpForCondBlock(rhs_eq_one_block_op, builder);

    builder.SetInsertionPointToBlockEnd(&lhs_not_eq_rhs_block);
    builder.Build<pir::YieldOp>(GetOpOuputValues(lhs_eq_one_cond_op));

    return lhs_eq_rhs_cond_op;
  }
}

std::unordered_map<OpLoweringGroupPtr,
                   std::unordered_map<std::string, pir::Attribute>>
CompileGroupAsOpAttribute(const std::vector<OpLoweringGroupPtr>& group_list) {
  PirCompiler pir_compiler(cinn::common::DefaultNVGPUTarget());
  auto fn_ptr_res = pir_compiler.Build(group_list);

  std::unordered_map<OpLoweringGroupPtr,
                     std::unordered_map<std::string, pir::Attribute>>
      result;
  for (size_t i = 0; i < group_list.size(); ++i) {
    std::unordered_map<std::string, ::pir::Attribute> op_attrs{
        {cinn::dialect::JitKernelOp::kAttrName,
         cinn::dialect::CINNKernelInfoAttribute::get(pir::IrContext::Instance(),
                                                     fn_ptr_res[i])},
    };
    result.insert({group_list[i], op_attrs});
  }
  return result;
}

void SimplyConditionBlock(
    pir::PatternRewriter& rewriter,  // NOLINT
    std::unordered_map<pir::Block*, OpLoweringGroupPtr>* group_map) {
  VLOG(4) << "simply condition block";
  using DoEachMutBlockGroupT =
      std::function<void(pir::Block*, const OpLoweringGroupPtr&)>;
  const auto& ForEachMutBlockGroup = [&](const DoEachMutBlockGroupT& DoEach) {
    for (auto& [block, group] : *group_map) {
      DoEach(block, group);
      std::vector<pir::Operation*> group_new_ops;
      group_new_ops.reserve(block->size());
      for (auto& op : *block) {
        if (!op.isa<pir::YieldOp>()) {
          group_new_ops.push_back(&op);
        }
      }
      group->SetOps(group_new_ops);
    }
  };
  ForEachMutBlockGroup([&](auto* block, const auto& group) {
    auto GetShapeOrDataForValue =
        [&group](pir::Value value) -> const symbol::ShapeOrDataDimExprs& {
      return group->GetShapeOrDataExprs(value);
    };
    EraseUnnecessaryExpandsInBlock(block, rewriter, GetShapeOrDataForValue);
  });
}

void CompileGroupToJitKernelOp(
    const std::vector<pir::Value>& group_inputs,
    pir::PatternRewriter& rewriter,  // NOLINT
    std::unordered_map<pir::Block*, OpLoweringGroupPtr>* group_map) {
  // prepare attribute for jit_kernel_op
  std::vector<OpLoweringGroupPtr> group_list;
  group_list.reserve(group_map->size());
  for (const auto& [_, group] : *group_map) {
    group_list.push_back(group);
  }
  auto op_attr_map = CompileGroupAsOpAttribute(group_list);
  VLOG(4) << "The size of group_map is : " << group_map->size();
  for (auto& [block, group] : *group_map) {
    std::vector<pir::Type> output_types;
    const auto& group_output_values = group->output_values();
    for (size_t i = 0; i < group_output_values.size(); ++i) {
      output_types.push_back(group_output_values[i].type());
    }
    auto& yield_op = block->back();
    CHECK(yield_op.isa<pir::YieldOp>()) << "Last op of block should be yield";
    rewriter.set_insertion_point(&yield_op);
    auto jit_kernel_op = rewriter.Build<cinn::dialect::JitKernelOp>(
        group_inputs, op_attr_map.at(group), output_types);
    CHECK(jit_kernel_op.num_results() == group_output_values.size());
    for (size_t i = 0; i < jit_kernel_op.num_results(); ++i) {
      rewriter.ReplaceAllUsesWith(group_output_values[i],
                                  jit_kernel_op.result(i));
    }

    // Delete origin group ops
    std::vector<pir::Operation*> group_ops;
    for (auto iter = block->rbegin(); iter != block->rend(); iter++) {
      if (!iter->isa<pir::YieldOp>()) {
        group_ops.push_back(&(*iter));
      }
    }
    for (auto* op : group_ops) {
      if (op->use_empty()) {
        op->Erase();
      }
    }
  }
}

pir::Operation* CompileBroadcastTreeToConditionBlock(
    const BroadcastTreeInfo& broadcast_tree_info,
    const OpLoweringGroupPtr& group,
    pir::ShapeConstraintIRAnalysis& shape_analysis,  // NOLINT
    const std::vector<pir::Value>& group_inputs,
    const std::vector<pir::Type>& output_types,
    pir::PatternRewriter& rewriter) {  // NOLINT
  // 1. broadcast tree to condition op
  VLOG(4) << "broadcast tree to condition op";
  const auto& value_to_dim_expr_idx =
      broadcast_tree_info.GetValueToDimExprIdx();
  const auto& broadcast_tree = broadcast_tree_info.GetBroadcastTree();
  std::unordered_map<pir::Block*, OpLoweringGroupPtr> group_map;
  pir::Operation* cond_op = CreateConditionBlock(*broadcast_tree,
                                                 group,
                                                 shape_analysis,
                                                 value_to_dim_expr_idx,
                                                 group_inputs,
                                                 output_types,
                                                 rewriter,
                                                 rewriter.block(),
                                                 &group_map);
  // 2. simply every condition block
  auto* program = group->ops().front()->GetParentProgram();
  VLOG(6) << "Before simply condition block: " << *program;

  SimplyConditionBlock(rewriter, &group_map);
  VLOG(6) << "After simply condition block: " << *program;

  // 3. compile condition block to jit_kernel_op
  CompileGroupToJitKernelOp(group_inputs, rewriter, &group_map);
  VLOG(6) << "compile condition block to jit_kernel_op: " << *program;

  return cond_op;
}

bool IsComplicatedDimExpr(const symbol::DimExpr& dim_expr) {
  auto lambdas = symbol::Overloaded{
      [](std::int64_t dim_expr) { return false; },
      [](const std::string& dim_expr) { return false; },
      [](const symbol::Negative<symbol::DimExpr>& dim_expr) { return true; },
      [](const symbol::Reciprocal<symbol::DimExpr>& dim_expr) { return true; },
      [](const symbol::Add<symbol::DimExpr>& dim_expr) { return true; },
      [](const symbol::Mul<symbol::DimExpr>& dim_expr) { return true; },
      [](const symbol::Max<symbol::DimExpr>& dim_expr) { return true; },
      [](const symbol::Min<symbol::DimExpr>& dim_expr) { return true; },
      [](const symbol::Broadcast<symbol::DimExpr>& dim_expr) { return true; }};
  return std::visit(lambdas, dim_expr.variant());
}

template <typename DoEachT>
void VisitEachInputValue(const OpLoweringGroupPtr& group,
                         const DoEachT& DoEach) {
  for (pir::Value value : GetBlockOutsideInput(group->ops())) {
    DoEach(value);
  }
}

template <typename DoEachT>
void VisitEachDimExprFromTensorShapeOrData(
    const symbol::TensorShapeOrDataDimExprs& shape_or_data,
    const DoEachT& DoEach) {
  for (const auto& dim_expr : shape_or_data.shape()) {
    DoEach(dim_expr);
  }
  if (!shape_or_data.data().has_value()) {
    return;
  }
  for (const auto& dim_expr : shape_or_data.data().value()) {
    DoEach(dim_expr);
  }
}

template <typename DoEachT>
void VisitEachDimExpr(const symbol::ShapeOrDataDimExprs& shape_or_data,
                      const DoEachT& DoEach) {
  auto lambdas = symbol::Overloaded{
      [&](const symbol::TensorShapeOrDataDimExprs& tensor_shape_or_data) {
        VisitEachDimExprFromTensorShapeOrData(tensor_shape_or_data, DoEach);
      },
      [&](const symbol::TensorListShapeOrDataDimExprs& tensor_list) {
        symbol::TensorListShapeOrDataDimExprs simplified_tensor_list;
        for (const symbol::TensorShapeOrDataDimExprs& tensor_shape_or_data :
             tensor_list) {
          VisitEachDimExprFromTensorShapeOrData(tensor_shape_or_data, DoEach);
        }
      }};
  return std::visit(lambdas, shape_or_data.variant());
}

std::unordered_map<symbol::DimExpr, symbol::DimExpr>
CollectSubstituteDimExprMap(
    const OpLoweringGroupPtr& group,
    pir::ShapeConstraintIRAnalysis& shape_analysis) {  // NOLINT
  std::unordered_map<symbol::DimExpr, symbol::DimExpr> dim_expr_map;
  std::unordered_set<std::string> base_dim_expr_set;

  VisitEachInputValue(group, [&](::pir::Value value) {
    if (!shape_analysis.HasShapeOrDataForValue(value)) {
      return;
    }
    auto& shape_or_data = shape_analysis.GetShapeOrDataForValue(value);
    VisitEachDimExpr(shape_or_data, [&](const symbol::DimExpr& dim_expr) {
      if (IsComplicatedDimExpr(dim_expr) &&
          dim_expr_map.find(dim_expr) == dim_expr_map.end()) {
        dim_expr_map[dim_expr] =
            symbol::DimExpr(shape_analysis.GetNextSymName());
      }
      if (dim_expr.isa<std::string>()) {
        base_dim_expr_set.insert(dim_expr.Get<std::string>());
      }
    });
  });

  const std::unordered_set<symbol::DimExpr> dim_exprs_no_outer_symbol = [&] {
    auto HasOuterBasicSymbol = [&](const symbol::DimExpr& dim_expr) {
      for (const auto& symbol : symbol::CollectDimExprSymbols(dim_expr)) {
        if (base_dim_expr_set.count(symbol) == 0) {
          return true;
        }
      }
      return false;
    };
    std::unordered_set<symbol::DimExpr> result;
    for (const auto& kv : dim_expr_map) {
      if (IsComplicatedDimExpr(kv.first) && !HasOuterBasicSymbol(kv.first)) {
        result.insert(kv.first);
      }
    }
    return result;
  }();
  for (const auto& dim_expr : dim_exprs_no_outer_symbol) {
    dim_expr_map.erase(dim_expr);
  }

  return dim_expr_map;
}

bool IsShapeOrDataNeedSubstitute(
    const symbol::ShapeOrDataDimExprs& shape_or_data,
    const std::unordered_map<symbol::DimExpr, symbol::DimExpr>& dim_expr_map) {
  bool ret = false;
  VisitEachDimExpr(shape_or_data, [&](const symbol::DimExpr& dim_expr) {
    if (dim_expr_map.find(dim_expr) != dim_expr_map.end()) {
      ret = true;
    }
  });
  return ret;
}

symbol::TensorShapeOrDataDimExprs SubstituteTensorShapeOrData(
    const symbol::TensorShapeOrDataDimExprs& shape_or_data,
    const std::unordered_map<symbol::DimExpr, symbol::DimExpr>& dim_expr_map) {
  const auto& SimplifyDimExpr =
      [&](const std::vector<symbol::DimExpr>& original_dim_expr)
      -> std::vector<symbol::DimExpr> {
    std::vector<symbol::DimExpr> simplified_dim_expr{};
    for (const symbol::DimExpr& dim_expr : original_dim_expr) {
      simplified_dim_expr.push_back(symbol::SimplifyDimExpr(
          symbol::SubstituteDimExpr(dim_expr, dim_expr_map)));
    }
    return simplified_dim_expr;
  };

  std::vector<symbol::DimExpr> simplified_shape =
      SimplifyDimExpr(shape_or_data.shape());
  if (!shape_or_data.data().has_value()) {
    return symbol::ShapeOrData<symbol::DimExpr>(simplified_shape);
  }
  std::vector<symbol::DimExpr> simplified_data =
      SimplifyDimExpr(shape_or_data.data().value());
  return symbol::ShapeOrData<symbol::DimExpr>(simplified_shape,
                                              simplified_data);
}

symbol::ShapeOrDataDimExprs SubstituteShapeOrData(
    const symbol::ShapeOrDataDimExprs& shape_or_data,
    const std::unordered_map<symbol::DimExpr, symbol::DimExpr>& dim_expr_map) {
  auto lambdas = symbol::Overloaded{
      [&](const symbol::TensorShapeOrDataDimExprs& tensor_shape_or_data) {
        return symbol::ShapeOrDataDimExprs(
            SubstituteTensorShapeOrData(tensor_shape_or_data, dim_expr_map));
      },
      [&](const symbol::TensorListShapeOrDataDimExprs& tensor_list) {
        symbol::TensorListShapeOrDataDimExprs simplified_tensor_list;
        for (symbol::TensorShapeOrDataDimExprs tensor_shape_or_data :
             tensor_list) {
          simplified_tensor_list.push_back(
              SubstituteTensorShapeOrData(tensor_shape_or_data, dim_expr_map));
        }
        return symbol::ShapeOrDataDimExprs(simplified_tensor_list);
      }};
  return std::visit(lambdas, shape_or_data.variant());
}

symbol::ShapeOrDataDimExprs TrySubstitute(
    const symbol::ShapeOrDataDimExprs& shape_or_data,
    const std::unordered_map<symbol::DimExpr, symbol::DimExpr>& dim_expr_map) {
  if (!IsShapeOrDataNeedSubstitute(shape_or_data, dim_expr_map)) {
    return shape_or_data;
  }
  return SubstituteShapeOrData(shape_or_data, dim_expr_map);
}

std::unordered_map<::pir::Value, symbol::ShapeOrDataDimExprs>
CreateGroupShapeOrDataExprs(
    const OpLoweringGroupPtr& group,
    pir::ShapeConstraintIRAnalysis& shape_analysis) {  // NOLINT
  std::unordered_map<symbol::DimExpr, symbol::DimExpr> dim_expr_map =
      CollectSubstituteDimExprMap(group, shape_analysis);
  std::unordered_map<::pir::Value, symbol::ShapeOrDataDimExprs> value2shape;
  for (auto* op : group->ops()) {
    for (size_t i = 0; i < op->num_operands(); ++i) {
      auto operand = op->operand_source(i);
      if (operand && value2shape.find(operand) == value2shape.end() &&
          shape_analysis.HasShapeOrDataForValue(operand)) {
        VLOG(6) << "Add value_to_shape_or_data_exprs for " << operand.impl();
        value2shape.insert(
            {operand,
             TrySubstitute(shape_analysis.GetShapeOrDataForValue(operand),
                           dim_expr_map)});
      }
    }
    for (size_t i = 0; i < op->num_results(); ++i) {
      auto result = op->result(i);
      if (result && value2shape.find(result) == value2shape.end() &&
          shape_analysis.HasShapeOrDataForValue(result)) {
        VLOG(6) << "Add value_to_shape_or_data_exprs for " << result.impl();
        value2shape.insert(
            {result,
             TrySubstitute(shape_analysis.GetShapeOrDataForValue(result),
                           dim_expr_map)});
      }
    }
  }
  VLOG(5) << group.get()
          << " value_to_shape_or_data_exprs.size() : " << value2shape.size();
  return value2shape;
}
}  // namespace

namespace cinn::dialect::ir {
std::unique_ptr<::pir::Pass> CreateLowerCinnFusionOpPass() {
  return std::make_unique<LowerCinnFusionOpPass>();
}

std::unique_ptr<::pir::Pass> CreateLowerCinnDyShapeFusionOpPass() {
  return std::make_unique<LowerCinnDyShapeFusionOpPass>();
}

}  // namespace cinn::dialect::ir

// REGISTER_IR_PASS(cinn_group_lowering, LowerCinnFusionOpPass);
