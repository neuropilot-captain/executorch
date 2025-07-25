/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <ATen/cpu/vec/functional.h>
#include <ATen/cpu/vec/vec.h>
#include <executorch/runtime/kernel/kernel_includes.h>

namespace torch {
namespace executor {
namespace native {

Tensor& opt_neg_out(KernelRuntimeContext& ctx, const Tensor& in, Tensor& out) {
  (void)ctx;

  // Resize for dynamic shape
  auto error = resize_tensor(out, in.sizes());
  ET_KERNEL_CHECK_MSG(
      ctx,
      error == Error::Ok,
      InvalidArgument,
      out,
      "Failed to resize output tensor.");

  ET_SWITCH_REALHBF16_TYPES(in.scalar_type(), ctx, "neg.out", CTYPE, [&] {
    at::vec::map<CTYPE>(
        [](auto x) { return x.neg(); },
        out.mutable_data_ptr<CTYPE>(),
        in.const_data_ptr<CTYPE>(),
        in.numel());
  });

  return out;
}

} // namespace native
} // namespace executor
} // namespace torch
