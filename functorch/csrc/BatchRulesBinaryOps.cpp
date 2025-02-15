// Copyright (c) Facebook, Inc. and its affiliates.
// All rights reserved.
//
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree.

#include <functorch/csrc/BatchRulesHelper.h>
#include <functorch/csrc/PlumbingHelper.h>
#include <functorch/csrc/InPlacePlumbing.h>
#include <ATen/Operators.h>

namespace at { namespace functorch {

static void handleScalarTypePromotion(Tensor& logical_scalar_tensor, Tensor& second) {
  auto result_type = at::native::result_type(logical_scalar_tensor[0], second);
  if (logical_scalar_tensor.scalar_type() != result_type) {
    logical_scalar_tensor = logical_scalar_tensor.to(result_type);
  }
  if (second.scalar_type() != result_type) {
    second = second.to(result_type);
  }
}

template <typename F, F Func, typename... ExtraArgs>
std::tuple<Tensor,optional<int64_t>> binary_pointwise_batch_rule(
    const Tensor& tensor, optional<int64_t> tensor_batch_dim,
    const Tensor& other, optional<int64_t> other_batch_dim,
    ExtraArgs... extra_args) {
  // compute max logical rank
  auto tensor_logical_rank = rankWithoutBatchDim(tensor, tensor_batch_dim);
  auto other_logical_rank = rankWithoutBatchDim(other, other_batch_dim);
  auto max_logical_rank = std::max(tensor_logical_rank, other_logical_rank);

  auto tensor_ = moveBatchDimToFront(tensor, tensor_batch_dim);
  auto other_ = moveBatchDimToFront(other, other_batch_dim);

  // In the (0D, ND) case, type promotion semantics are different :/
  auto tensor_is_logical_scalar = (tensor_logical_rank == 0 && tensor_batch_dim.has_value());
  auto other_is_logical_scalar = (other_logical_rank == 0 && other_batch_dim.has_value());
  if (tensor_is_logical_scalar && !other_is_logical_scalar) {
    handleScalarTypePromotion(tensor_, other_);
  }
  if (other_is_logical_scalar && !tensor_is_logical_scalar) {
    handleScalarTypePromotion(other_, tensor_);
  }

  // If the dimensions aren't aligned, we need to line them up.
  // Tensor[B, 3] + Tensor[2, 5, 3] -> Tensor[B, 1, 1, 3] + Tensor[2, 5, 3]
  // Note that only tensors that have a batch dim need to be modified.
  // Tensor[B, 2, 3, 5] + Tensor[5] -> no changes needed
  tensor_ = maybePadToLogicalRank(tensor_, tensor_batch_dim, max_logical_rank);
  other_ = maybePadToLogicalRank(other_, other_batch_dim, max_logical_rank);

  auto result = Func(tensor_, other_, std::forward<ExtraArgs>(extra_args)...);
  return std::make_tuple( std::move(result), 0 );
}

template <typename M, M Meth, typename... ExtraArgs>
void binary_pointwise_inplace_batch_rule(
    Tensor& tensor, optional<int64_t> tensor_batch_dim,
    const Tensor& other, optional<int64_t> other_batch_dim,
    ExtraArgs... extra_args) {
  if (!tensor_batch_dim && other_batch_dim) {
    vmapIncompatibleInplaceError("inplace arithmetic");
  }

  // compute max logical rank
  auto tensor_logical_rank = rankWithoutBatchDim(tensor, tensor_batch_dim);
  auto other_logical_rank = rankWithoutBatchDim(other, other_batch_dim);
  auto max_logical_rank = std::max(tensor_logical_rank, other_logical_rank);

  auto tensor_ = moveBatchDimToFront(tensor, tensor_batch_dim);
  auto other_ = moveBatchDimToFront(other, other_batch_dim);

  // If the dimensions aren't aligned, we need to line them up.
  // Tensor[B, 3] + Tensor[2, 5, 3] -> Tensor[B, 1, 1, 3] + Tensor[2, 5, 3]
  // Note that only tensors that have a batch dim need to be modified.
  // Tensor[B, 2, 3, 5] + Tensor[5] -> no changes needed
  tensor_ = maybePadToLogicalRank(tensor_, tensor_batch_dim, max_logical_rank);
  other_ = maybePadToLogicalRank(other_, other_batch_dim, max_logical_rank);

  (tensor_.*Meth)(other_, std::forward<ExtraArgs>(extra_args)...);
}

template <typename F, F Func>
std::tuple<Tensor,optional<int64_t>> comparison_pointwise_batch_rule(
    const Tensor& tensor, optional<int64_t> tensor_batch_dim,
    const Tensor& other, optional<int64_t> other_batch_dim) {
  // compute max logical rank
  auto tensor_logical_rank = rankWithoutBatchDim(tensor, tensor_batch_dim);
  auto other_logical_rank = rankWithoutBatchDim(other, other_batch_dim);
  auto max_logical_rank = std::max(tensor_logical_rank, other_logical_rank);

  auto tensor_ = moveBatchDimToFront(tensor, tensor_batch_dim);
  auto other_ = moveBatchDimToFront(other, other_batch_dim);

  // If the dimensions aren't aligned, we need to line them up.
  // Tensor[B, 3] + Tensor[2, 5, 3] -> Tensor[B, 1, 1, 3] + Tensor[2, 5, 3]
  // Note that only tensors that have a batch dim need to be modified.
  // Tensor[B, 2, 3, 5] + Tensor[5] -> no changes needed
  tensor_ = maybePadToLogicalRank(tensor_, tensor_batch_dim, max_logical_rank);
  other_ = maybePadToLogicalRank(other_, other_batch_dim, max_logical_rank);

  auto result = Func(tensor_, other_);
  return std::make_tuple( std::move(result), 0 );
}


std::tuple<Tensor,optional<int64_t>> clamp_tensor_batch_rule(
    const Tensor& self, optional<int64_t> self_bdim,
    const optional<Tensor>& min, optional<int64_t> min_bdim, const optional<Tensor>& max, optional<int64_t> max_bdim) {
  int64_t self_logical_rank = rankWithoutBatchDim(self, self_bdim);
  int64_t out_logical_rank = self_logical_rank;
  if (min.has_value()) {
    out_logical_rank = std::max(out_logical_rank, rankWithoutBatchDim(*min, min_bdim));
  }
  if (max.has_value()) {
    out_logical_rank = std::max(out_logical_rank, rankWithoutBatchDim(*max, max_bdim));
  }

  c10::optional<Tensor> min_ = nullopt;
  c10::optional<Tensor> max_ = nullopt;
  auto self_ = moveBatchDimToFront(self, self_bdim);
  if (min.has_value()) {
    min_ = moveBatchDimToFront(*min, min_bdim);
  }
  if (max.has_value()) {
    max_ = moveBatchDimToFront(*max, max_bdim);
  }
  // todo(chilli): Are there weird type promotion semantics here we need to worry about?

  // If the dimensions aren't aligned, we need to line them up.
  // Tensor[B, 3] + Tensor[2, 5, 3] -> Tensor[B, 1, 1, 3] + Tensor[2, 5, 3]
  // Note that only tensors that have a batch dim need to be modified.
  // Tensor[B, 2, 3, 5] + Tensor[5] -> no changes needed
  self_ = maybePadToLogicalRank(self_, self_bdim, out_logical_rank);
  if (min_.has_value()) {
    min_ = maybePadToLogicalRank(*min_, min_bdim, out_logical_rank);
  }
  if (max_.has_value()) {
    max_ = maybePadToLogicalRank(*max_, max_bdim, out_logical_rank);
  }

  auto result = at::clamp(self_, min_, max_);
  return std::make_tuple( std::move(result), 0 );
}

std::tuple<Tensor,optional<int64_t>> pow_scalar_tensor_batch_rule(
    const Scalar& other,
    const Tensor& tensor, optional<int64_t> tensor_batch_dim) {
  return std::make_tuple( at::pow(other, tensor), tensor_batch_dim );
}

std::tuple<Tensor,optional<int64_t>> _s_where_batch_rule(
    const Tensor& condition, optional<int64_t> condition_bdim,
    const Tensor& self, optional<int64_t> self_bdim, const Tensor& other, optional<int64_t> other_bdim) {
  auto condition_ = moveBatchDimToFront(condition, condition_bdim);
  auto self_ = moveBatchDimToFront(self, self_bdim);
  auto other_ = moveBatchDimToFront(other, other_bdim);
  return std::make_tuple(at::where(condition_, self_, other_), 0);
}

TORCH_LIBRARY_IMPL(aten, FT_BATCHED_KEY, m) {
#define BINARY_POINTWISE_WITH_SCALAR(op) \
  VMAP_SUPPORT(#op".Tensor", SINGLE_ARG(binary_pointwise_batch_rule<decltype(&ATEN_FN2(op, Tensor)), &op, const Scalar&>));

#define BINARY_POINTWISE(op) \
  VMAP_SUPPORT(#op".Tensor", SINGLE_ARG(binary_pointwise_batch_rule<decltype(&ATEN_FN2(op, Tensor)), &at::op>));

  BINARY_POINTWISE_WITH_SCALAR(add);
  VMAP_SUPPORT("add.Scalar", SINGLE_ARG(basic_unary_batch_rule<decltype(&ATEN_FN2(add, Scalar)), &at::add, const Scalar&, const Scalar&>));
  VMAP_SUPPORT("atan2", SINGLE_ARG(binary_pointwise_batch_rule<decltype(&ATEN_FN(atan2)), &at::atan2>));

  VMAP_SUPPORT("clamp",
      SINGLE_ARG(basic_unary_batch_rule<decltype(&ATEN_FN(clamp)), &at::clamp, const optional<Scalar>&, const optional<Scalar>&>));
  VMAP_SUPPORT("clamp.Tensor", clamp_tensor_batch_rule);
  VMAP_SUPPORT("clamp_min.Tensor",
      SINGLE_ARG(binary_pointwise_batch_rule<decltype(&ATEN_FN2(clamp_min, Tensor)), &at::clamp_min>));
  VMAP_SUPPORT("clamp_min",
      SINGLE_ARG(basic_unary_batch_rule<decltype(&ATEN_FN(clamp_min)), &at::clamp_min, const Scalar&>));
  VMAP_SUPPORT("clamp_max.Tensor",
      SINGLE_ARG(binary_pointwise_batch_rule<decltype(&ATEN_FN2(clamp_max, Tensor)), &at::clamp_max>));
  VMAP_SUPPORT("clamp_max",
      SINGLE_ARG(basic_unary_batch_rule<decltype(&ATEN_FN(clamp_max)), &at::clamp_max, const Scalar&>));


  BINARY_POINTWISE(div);
  VMAP_SUPPORT("div.Scalar", SINGLE_ARG(basic_unary_batch_rule<decltype(&ATEN_FN2(div, Scalar)), &at::div, const Scalar&>));
  VMAP_SUPPORT("div.Scalar_mode", SINGLE_ARG(basic_unary_batch_rule<decltype(&ATEN_FN2(div, Scalar_mode)), &at::div, const Scalar&, c10::optional<string_view>>));
  VMAP_SUPPORT("div.Tensor_mode", SINGLE_ARG(binary_pointwise_batch_rule<decltype(&ATEN_FN2(div, Tensor_mode)), &at::div, c10::optional<string_view>>));

  VMAP_SUPPORT("maximum", SINGLE_ARG(binary_pointwise_batch_rule<decltype(&ATEN_FN(maximum)), &at::maximum>));
  VMAP_SUPPORT("minimum", SINGLE_ARG(binary_pointwise_batch_rule<decltype(&ATEN_FN(minimum)), &at::minimum>));

  BINARY_POINTWISE(mul);
  VMAP_SUPPORT("mul.Scalar", SINGLE_ARG(basic_unary_batch_rule<decltype(&ATEN_FN2(mul, Scalar)), &at::mul, const Scalar&>));

  // at::pow has three out-of-place overloads
  VMAP_SUPPORT("pow.Tensor_Tensor", SINGLE_ARG(binary_pointwise_batch_rule<decltype(&ATEN_FN2(pow, Tensor_Tensor)), &at::pow>));
  VMAP_SUPPORT("pow.Tensor_Scalar", SINGLE_ARG(basic_unary_batch_rule<decltype(&ATEN_FN2(pow, Tensor_Scalar)), &at::pow, const Scalar&>));
  VMAP_SUPPORT("pow.Scalar", pow_scalar_tensor_batch_rule);

  BINARY_POINTWISE_WITH_SCALAR(sub);
  VMAP_SUPPORT("sub.Scalar", SINGLE_ARG(basic_unary_batch_rule<decltype(&ATEN_FN2(sub, Scalar)), &at::sub, const Scalar&, const Scalar&>));

  BINARY_POINTWISE_WITH_SCALAR(rsub);
  VMAP_SUPPORT("rsub.Scalar", SINGLE_ARG(basic_unary_batch_rule<decltype(&ATEN_FN2(rsub, Scalar)), &at::rsub, const Scalar&, const Scalar&>));

  VMAP_SUPPORT("sigmoid_backward", SINGLE_ARG(
        binary_pointwise_batch_rule<decltype(&at::sigmoid_backward), &at::sigmoid_backward>));
  VMAP_SUPPORT("tanh_backward", SINGLE_ARG(binary_pointwise_batch_rule<decltype(&at::tanh_backward), &at::tanh_backward>));
  VMAP_SUPPORT("threshold_backward", SINGLE_ARG(
        binary_pointwise_batch_rule<decltype(&at::threshold_backward), &at::threshold_backward, const Scalar&>));

  VMAP_SUPPORT("fmin", SINGLE_ARG(binary_pointwise_batch_rule<decltype(&ATEN_FN(fmin)), &at::fmin>));
  VMAP_SUPPORT("fmax", SINGLE_ARG(binary_pointwise_batch_rule<decltype(&ATEN_FN(fmax)), &at::fmax>));

  OP_DECOMPOSE2(max, other);
  OP_DECOMPOSE2(min, other);

  OP_DECOMPOSE2(where, self);
  OP_DECOMPOSE2(where, ScalarSelf);
  OP_DECOMPOSE2(where, ScalarOther);
  OP_DECOMPOSE2(where, Scalar);
  OP_DECOMPOSE(where);
  VMAP_SUPPORT("_s_where", _s_where_batch_rule);

  using TensorScalarInplaceT = Tensor& (Tensor::*)(const Tensor&, const Scalar&) const;
  using ScalarScalarInplaceT = Tensor& (Tensor::*)(const Scalar&, const Scalar&) const;
  using TensorInplaceT = Tensor& (Tensor::*)(const Tensor&) const;
  using ScalarInplaceT = Tensor& (Tensor::*)(const Scalar&) const;

  m.impl("add_.Tensor", inplacePlumbing2<
     DECLTYPE_AUTO(&binary_pointwise_inplace_batch_rule<TensorScalarInplaceT, &Tensor::add_, const Scalar&>),
     const Scalar&>);
  m.impl("add_.Scalar", inplacePlumbing1<
     DECLTYPE_AUTO(&unary_inplace_batch_rule<ScalarScalarInplaceT, &Tensor::add_, const Scalar&, const Scalar&>),
     const Scalar&, const Scalar&>);
  m.impl("sub_.Tensor", inplacePlumbing2<
     DECLTYPE_AUTO(&binary_pointwise_inplace_batch_rule<TensorScalarInplaceT, &Tensor::sub_, const Scalar&>),
     const Scalar&>);
  m.impl("sub_.Scalar", inplacePlumbing1<
     DECLTYPE_AUTO(&unary_inplace_batch_rule<ScalarScalarInplaceT, &Tensor::sub_, const Scalar&, const Scalar&>),
     const Scalar&, const Scalar&>);
  m.impl("mul_.Tensor", inplacePlumbing2<
     DECLTYPE_AUTO(&binary_pointwise_inplace_batch_rule<TensorInplaceT, &Tensor::mul_>)>);
  m.impl("mul_.Scalar", inplacePlumbing1<
     DECLTYPE_AUTO(&unary_inplace_batch_rule<ScalarInplaceT, &Tensor::mul_, const Scalar&>),
     const Scalar&>);
  m.impl("div_.Tensor", inplacePlumbing2<
     DECLTYPE_AUTO(&binary_pointwise_inplace_batch_rule<TensorInplaceT, &Tensor::div_>)>);
  m.impl("div_.Scalar", inplacePlumbing1<
     DECLTYPE_AUTO(&unary_inplace_batch_rule<ScalarInplaceT, &Tensor::div_, const Scalar&>),
     const Scalar&>);

#define COMPARISON_POINTWISE(op) \
  VMAP_SUPPORT(#op".Tensor", \
      SINGLE_ARG(comparison_pointwise_batch_rule<decltype(&ATEN_FN2(op, Tensor)), &at::op>)); \
  VMAP_SUPPORT(#op".Scalar", \
      SINGLE_ARG(basic_unary_batch_rule<decltype(&ATEN_FN2(op, Scalar)), &at::op, const Scalar&>));

  COMPARISON_POINTWISE(eq);
  COMPARISON_POINTWISE(gt);
  COMPARISON_POINTWISE(ge);
  COMPARISON_POINTWISE(le);
  COMPARISON_POINTWISE(lt);
  COMPARISON_POINTWISE(ne);

#undef COMPARISON_POINTWISE
#undef SINGLE_ARG
#undef BINARY_POINTWISE_BATCH_RULE_SCALAR
#undef BINARY_POINTWISE_BATCH_RULE
#undef BINARY_POINTWISE_WITH_SCALAR
#undef BINARY_POINTWISE
}

}}
