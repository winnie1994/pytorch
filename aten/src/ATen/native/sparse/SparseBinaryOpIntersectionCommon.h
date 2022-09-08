#pragma once

#include <ATen/Tensor.h>
#include <ATen/native/TensorIterator.h>
#include <ATen/Dispatch.h>
#include <ATen/native/sparse/Macros.h>
#include <ATen/ExpandUtils.h>
#include <ATen/SparseTensorUtils.h>

#ifndef AT_PER_OPERATOR_HEADERS
#include <ATen/Functions.h>
#include <ATen/NativeFunctions.h>
#else
#include <ATen/ops/arange.h>
#include <ATen/ops/empty.h>
#include <ATen/ops/_sparse_coo_tensor_with_dims_and_tensors.h>
#include <ATen/ops/from_blob.h>
#include <ATen/ops/result_type.h>
#endif

#ifdef GPUCC
#define NAME "sparse_binary_op_intersection_cuda"
#else
#define NAME "sparse_binary_op_intersection_cpu"
#endif

#define BINARY_OP_API static constexpr FUNCAPI INLINE

namespace at {
namespace native {

namespace {

using at::sparse::get_sparse_impl;

// ForwardIt: only legacy random access iterator is supported.
template<class ForwardIt, class T, bool is_lower = true>
static FUNCAPI INLINE
ForwardIt find_bound(ForwardIt first, ForwardIt last, const T& value) {
    ForwardIt RESTRICT it;
    typename std::iterator_traits<ForwardIt>::difference_type count, step;
    // NOTE: std::distance(first, last) compiles but produces wrong results on CUDA,
    // so only legacy random access iterators are safe in this code.
    count = last - first;

    while (count > 0) {
      it = first;
      step = count / 2;
      // avoiding std::advance(it, step),
      // although it does work unlike std::distance on CUDA.
      it += step;
      if (is_lower ? *it < value : value >= *it) {
        first = ++it;
        count -= step + 1;
      }
      else {
        count = step;
      }
    }
    return first;
}

template <template <typename func_t> class kernel_t>
struct KernelLauncher {
  template <typename func_t>
  static void launch(TensorIteratorBase& iter, const func_t& f) {
    kernel_t<func_t>::launch(iter, f);
  }
};

template <
  template <typename func_t> class kernel_t,
  typename binary_op_t,
  typename hash_t = int64_t,
  typename offset_t = int64_t>
void _sparse_binary_op_intersection_kernel_impl(
    Tensor& res,
    const Tensor& x_,
    const Tensor& y_,
    const std::vector<int64_t> broadcasted_shape,
    const bool is_commutative = true
) {
  // The common dtype check is relevant when op is done in-place.
  // This is because binary_of_t produces new values and it could be that
  // new_values.dtype != res.dtype. In such a case we should error out
  // as soon as possible to avoid redundant kernel runs.
  const auto common_dtype = at::result_type(x_, y_);
  TORCH_CHECK(canCast(common_dtype, res.scalar_type()),
      "Can't convert result type ", common_dtype,
      " to output ", res.scalar_type());

  using KernelLauncher = KernelLauncher<kernel_t>;

  const Tensor x = is_commutative ? x_ : x_.coalesce();
  const Tensor y = is_commutative ? y_ : y_.coalesce();

  // Given sparse tensors x and y we decide which one is source, and which one
  // is probably_coalesced. The indices of both source and probably_coalesced are
  // hashed and then the hash values of the source's indices are binary-searched
  // into the hash values of the probably_coalesced's indices.
  // If probably_coalesce is coalesced, by the property of the hashing method
  // (see below), the hash values are already sorted and we can avoid any
  // explicit sorting routines.
  Tensor probably_coalesced, source;
  std::tie(probably_coalesced, source) = [&]() -> std::tuple<Tensor, Tensor> {
    // Case 1: either x or y is coalesced.
    if ((x.is_coalesced() ^ y.is_coalesced())) {
      return x.is_coalesced()
        ? std::make_tuple(x, y)
        : std::make_tuple(y, x);
    }
    // Case 2: Both x and y are either coalesced or non-coalesced.
    // If both are coalesced, search into the larger tensor is faster.
    // Same holds when both are non-coalesced.
    else {
      Tensor larger, smaller;
      std::tie(larger, smaller) = [&]() -> std::tuple<Tensor, Tensor> {
        return x._nnz() >= y._nnz()
          ? std::make_tuple(x, y)
          : std::make_tuple(y, x);
      }();

      // If under a uniform distribution it is likely to hit many elements in larger,
      // it is best to coalesce it for better performance.
      const auto larger_sizes = larger.sizes();
      const auto sparse_dim_numel = std::accumulate(
          larger_sizes.begin(),
          larger_sizes.begin() + larger.sparse_dim(),
          1,
          std::multiplies<int64_t>());
      // If nnz > prod(larger.shape[:sparse_dim]), by the pidgeonhole principle,
      // there is at least one bucket with nnz / prod(larger.shape[:sparse_dim]) elements.
      // It provides a lower bound for the max count in the intersection.
      // This condition is very conservative as we do not check whether such an event
      // actually occurred, although it is very likely under a uniform distribution,
      // the distribution with the highest uncertainty (maximizes entropy).
      const auto max_count_lower_bound = larger._nnz() / sparse_dim_numel;
      constexpr int64_t MAX_COPIES_PER_THREAD = 50;
      return max_count_lower_bound > MAX_COPIES_PER_THREAD
        ? std::make_tuple(larger.coalesce(), smaller)
        : std::make_tuple(larger, smaller);
    }
  }();

  // The employed hash function maps a d-dim index to a linear offset
  // into a contiguous memory that is sufficient to fit a dense tensor
  // of shape broadcasted_shape(x.shape, y.shape), i.e.
  // idx -> \sum_{i = 0}^d idx[i] * hash_coeffs[i], where
  // hash_coeffs are the strides of a contiguous tensor of shape
  // broadcasted_shape(x.shape, y.shape).
  // Assuming the following order on the dimensions, i.e. the right-most dim is the
  // fastest-changing dim, and the left-most is the slowest-changing dim,
  // which is implicit in the definition of hash_coeffs,
  // it could be shown that the hash function is actually bijective and, hence,
  // is a perfect hash function (no collisions ever).
  const auto kHash = std::is_same<hash_t, int64_t>::value ? kLong : kInt;
  const auto hash_coeffs = [&]() -> Tensor {
    const auto broadcasted_sparse_dim_shape = std::vector<int64_t>(
      broadcasted_shape.begin(),
      broadcasted_shape.begin() + probably_coalesced.sparse_dim()
    );
    auto strides = contiguous_strides(broadcasted_sparse_dim_shape);
    auto strides_len = static_cast<int64_t>(strides.size());
    auto hash_coeffs = at::empty(
        {strides_len},
        probably_coalesced._indices().options().device(kCPU).dtype(kHash));
    // Copy with a potential casting. Is there a nicer way?
    for (const auto i : c10::irange(strides_len)) {
      hash_coeffs[i] = strides[i];
    }
    hash_coeffs = hash_coeffs.to(probably_coalesced.device());
    return hash_coeffs;
  }();

  const auto nnz_arange = at::arange(
      std::max(probably_coalesced._nnz(), source._nnz()),
      source._indices().options());
  const auto probably_coalesced_nnz_arange = nnz_arange.narrow(-1, 0, probably_coalesced._nnz());

  // non-const because of gcc-5/clang-5 issues
  auto sparse_dim = probably_coalesced.sparse_dim();
  // non-const because of gcc-5/clang-5 issues
  auto sdim = static_cast<uint32_t>(sparse_dim);

  // Apply the hash function to probably_coalesced.indices
  const auto probably_coalesced_indices_hash
    // Windows does not seem to like these nested captures without explicit names.
    = [&probably_coalesced, &hash_coeffs, &probably_coalesced_nnz_arange, sdim, kHash]() -> Tensor {
    const auto indices = probably_coalesced._indices();
    // non-const because of gcc-5/clang-5 issues
    auto indices_dim_stride = indices.stride(0);
    auto indices_nnz_stride = indices.stride(1);

    auto hash = at::empty({probably_coalesced._nnz()},
        indices.options().dtype(kHash));

    auto iter = TensorIteratorConfig()
      // Hash has hash_t type while probably_coalesced_nnz_arange is index_t.
      .check_all_same_dtype(false)
      .add_output(hash)
      .add_input(probably_coalesced_nnz_arange)
      .build();

    AT_DISPATCH_INDEX_TYPES(indices.scalar_type(), NAME,
        // Windows does not seem to like these nested captures without explicit names.
        [&iter, &indices, &hash_coeffs, indices_dim_stride, indices_nnz_stride, sdim]() {
        const auto* RESTRICT ptr_indices = indices.data_ptr<index_t>();
        const auto* RESTRICT ptr_hash_coeffs = hash_coeffs.template data_ptr<hash_t>();

        KernelLauncher::launch(iter,
            // Windows does not seem to like these nested captures without explicit names.
            [ptr_indices, indices_dim_stride, indices_nnz_stride, sdim, ptr_hash_coeffs]
            FUNCAPI (index_t nnz_idx) -> hash_t {
            const auto* RESTRICT ptr_indices_dim = ptr_indices + nnz_idx * indices_nnz_stride;
            auto hash = hash_t {0};
            for (uint32_t dim = 0; dim < sdim; ++dim) {
              // use only int32_t operations when hash_t == int32_t
              const auto dim_hash_coeff = ptr_hash_coeffs[dim];
              const auto dim_index = static_cast<hash_t>(ptr_indices_dim[dim * indices_dim_stride]);
              hash += dim_index * dim_hash_coeff;
            }
            return hash;
        });
    });

    return hash;
  }();

  // Now that we have hash values of probably_coalesced.indices,
  // we need to decide whether they need to get sorted.
  // The sort is not requires if probably_coalesced is coalesced.
  Tensor sorted_hash, argsort_hash;
  std::tie(sorted_hash, argsort_hash) = [&]() -> std::tuple<Tensor, Tensor> {
    if (probably_coalesced.is_coalesced()) {
      // NOTE: argsort.dtype == nnz_arange.dtype
      const auto argsort = nnz_arange.narrow(-1, 0, probably_coalesced._nnz());
      return std::make_tuple(probably_coalesced_indices_hash, argsort);
    }
    else {
      // NOTE: we want argsort.dtype == nnz_arange.dtype,
      // but sort() produces indices of type int64_t,
      // so we convert to nnz_arange.dtype to avoid issues
      // with pointer types in the kernels below.
      Tensor sorted, argsort;
      std::tie(sorted, argsort) = probably_coalesced_indices_hash.sort();
      return std::make_tuple(sorted, argsort.to(nnz_arange.scalar_type()));
    }
  }();

  // Perform hash intersection.
  // Let  s_hash = hash(source.indices),
  //     pc_hash = hash(probably_coalesced.indices), then
  // for i = 0, ..., len(s_hash) - 1:
  //     lb = <index of a value in pc_hash[argsort_hash] which is a lower bound for s_hash[i]>,
  //     up = <index of a value in pc_hash[argsort_hash] which is an upper bound for s_hash[i]>,
  //     intersection_count[i] = up - lb
  //     intersection_first_idx[i] = lb.
  //
  // intersection_count and intersection_first_idx are used to form indices at which
  // intersection values are selected.
  Tensor intersection_count, intersection_first_idx;
  std::tie(intersection_count, intersection_first_idx)
    // Windows does not seem to like these nested captures without explicit names.
    = [&source, &nnz_arange, &hash_coeffs, &sorted_hash, sdim]() -> std::tuple<Tensor, Tensor> {
    const auto source_nnz = source._nnz();
    auto intersection_buffer = at::empty({2, source_nnz}, sorted_hash.options());
    auto intersection_count = intersection_buffer.select(0, 0);
    auto intersection_first_idx = intersection_buffer.select(0, 1);

    const auto source_indices = source._indices();
    const auto source_arange = nnz_arange.narrow(-1, 0, source_nnz);
    // non-const because of gcc-5/clang-5 issues
    auto indices_dim_stride = source_indices.stride(0);
    auto indices_nnz_stride = source_indices.stride(1);
    auto dummy = at::empty({1}, source_arange.options());

    auto iter = TensorIteratorConfig()
      .set_check_mem_overlap(false)
      .add_owned_output(dummy.expand_as(source_arange))
      .add_input(source_arange)
      .build();

    AT_DISPATCH_INDEX_TYPES(source_arange.scalar_type(), NAME,
        // Windows does not seem to like these nested captures without explicit names.
        [&iter, &source_indices, &sorted_hash, &hash_coeffs, &intersection_count, &intersection_first_idx,
          indices_dim_stride, indices_nnz_stride, sdim]() {
        const auto* RESTRICT ptr_indices = source_indices.data_ptr<index_t>();
        const auto* RESTRICT ptr_sorted_hash = sorted_hash.data_ptr<hash_t>();
        const auto sorted_hash_len = sorted_hash.numel();
        const auto* RESTRICT ptr_hash_coeffs = hash_coeffs.template data_ptr<hash_t>();
        auto* RESTRICT ptr_intersection_count = intersection_count.data_ptr<hash_t>();
        auto* RESTRICT ptr_intersection_first_idx = intersection_first_idx.data_ptr<hash_t>();

        // Fusing hash computation with hash intersection.
        KernelLauncher::launch(iter,
            // Windows does not seem to like these nested captures without explicit names.
            [ptr_indices, ptr_sorted_hash, sorted_hash_len, ptr_hash_coeffs,
              ptr_intersection_count, ptr_intersection_first_idx, sdim,
              indices_dim_stride, indices_nnz_stride]
            FUNCAPI (index_t nnz_idx) -> index_t {
            // Compute hash value
            const auto* RESTRICT ptr_indices_dim = ptr_indices + nnz_idx * indices_nnz_stride;
            auto hash = hash_t {0};
            for (uint32_t dim = 0; dim < sdim; ++dim) {
              // Use only int32_t operations when hash_t == int32_t.
              const auto dim_hash_coeff = ptr_hash_coeffs[dim];
              const auto dim_index = static_cast<hash_t>(ptr_indices_dim[dim * indices_dim_stride]);
              hash += dim_index * dim_hash_coeff;
            }

            // Perform hash values intersection
            const auto* RESTRICT lb = find_bound<const hash_t*, hash_t, /*is_lower=*/true>(
                ptr_sorted_hash,
                ptr_sorted_hash + sorted_hash_len,
                hash
            );

            const auto* RESTRICT ub = find_bound<const hash_t*, hash_t, /*is_lower=*/false>(
                ptr_sorted_hash,
                ptr_sorted_hash + sorted_hash_len,
                hash
            );

            ptr_intersection_count[nnz_idx] = ub - lb;
            ptr_intersection_first_idx[nnz_idx] = lb - ptr_sorted_hash;

            return 0;
        });
    });

    return std::make_tuple(intersection_count, intersection_first_idx);
  }();

  // Using intersection_count and intersection_first_idx,
  // form indices selected_source and selected_probably_coalesced such that
  // res.values = op(
  //  source.values.index_select(0, selected_source),
  //  probably_coalesced.values.index_select(0, selected_probably_coalesced)) and
  // res.indices = selected_source_sparse_indices, which is also equivalent to
  // res.indices = source.indices.index_select(1, selected_source).
  Tensor selected_source, selected_source_sparse_indices, selected_probably_coalesced;
  std::tie(selected_source, selected_source_sparse_indices, selected_probably_coalesced)
    // Windows does not seem to like these nested captures without explicit names.
    = [&intersection_count, &intersection_first_idx, &source,
        &nnz_arange, &argsort_hash, sdim]() -> std::tuple<Tensor, Tensor, Tensor> {
    // Thread offset = shifted_offset - shift.
    // This computation is fused in kernels below.

    // hash_t might not be enough to store offset values, so we use
    // offset_t which is at least sizeof(hash_t).
    const auto kOffset = std::is_same<offset_t, int32_t>::value ? kInt : kLong;
    const auto shifted_offset = intersection_count.cumsum(-1, kOffset);

    // NOTE: unavoidable sync to get to know the result's shape.
    const auto intersection_nnz = static_cast<int64_t>(
        // shifted_offset is a 1-dim tensor, potentially empty
        shifted_offset.size(0)
        ? shifted_offset.select(-1, -1).template item<offset_t>()
        : 0);

    auto selected_buffer = at::empty({2, intersection_nnz}, intersection_count.options());
    auto selected_source = selected_buffer.select(0, 0);
    auto selected_probably_coalesced = selected_buffer.select(0, 1);
    const auto source_sparse_indices = source._indices();
    auto selected_source_sparse_indices = at::empty({source.sparse_dim(), intersection_nnz},
        source_sparse_indices.options().memory_format(at::MemoryFormat::Contiguous));
    const auto source_idx = nnz_arange.narrow(-1, 0, source._nnz());
    auto dummy = at::empty({1}, source_idx.options());

    auto iter = TensorIteratorConfig()
      .set_check_mem_overlap(false)
      .check_all_same_dtype(false)
      .add_owned_output(dummy.expand_as(source_idx))
      .add_input(source_idx) // index_t
      .add_input(intersection_count) // hash_t
      .add_input(intersection_first_idx) // hash_t
      .add_input(shifted_offset) // offset_t
      .build();

    AT_DISPATCH_INDEX_TYPES(source_idx.scalar_type(), NAME,
        // Windows does not seem to like these nested captures without explicit names.
        [&iter, &selected_source,
          &selected_probably_coalesced, &argsort_hash,
          &source_sparse_indices, &selected_source_sparse_indices,
          sdim]() {
        auto* RESTRICT ptr_selected_source = selected_source.data_ptr<hash_t>();
        auto* RESTRICT ptr_selected_probably_coalesced = selected_probably_coalesced.data_ptr<hash_t>();
        const auto* RESTRICT ptr_argsort = argsort_hash.data_ptr<index_t>();

        auto* RESTRICT ptr_selected_source_sparse_indices = selected_source_sparse_indices.data_ptr<index_t>();
        // Non-const because of Gcc5/Clang5 issues
        auto selected_source_sparse_indices_nnz_stride = static_cast<offset_t>(
            selected_source_sparse_indices.stride(1));
        auto selected_source_sparse_indices_dim_stride = static_cast<offset_t>(
            selected_source_sparse_indices.stride(0));

        const auto* RESTRICT ptr_source_sparse_indices = source_sparse_indices.data_ptr<index_t>();
        // Non-const because of Gcc5/Clang5 issues
        auto source_sparse_indices_nnz_stride = static_cast<offset_t>(
            source_sparse_indices.stride(1));
        auto source_sparse_indices_dim_stride = static_cast<offset_t>(
            source_sparse_indices.stride(0));

        KernelLauncher::launch(iter,
            // Windows does not seem to like these nested captures without explicit names.
            [ptr_selected_source,
              ptr_selected_probably_coalesced,
              ptr_argsort,
              ptr_selected_source_sparse_indices,
              selected_source_sparse_indices_nnz_stride,
              selected_source_sparse_indices_dim_stride,
              ptr_source_sparse_indices,
              source_sparse_indices_nnz_stride,
              source_sparse_indices_dim_stride,
              sdim]
            FUNCAPI (
              index_t idx,
              hash_t count,
              hash_t first_match_idx,
              offset_t shifted_offset) -> index_t {
            const auto offset = shifted_offset - static_cast<offset_t>(count);
            auto* RESTRICT ptr_selected_source_idx_out = ptr_selected_source + offset;
            auto* RESTRICT ptr_selected_probably_coalesced_idx_out = ptr_selected_probably_coalesced + offset;
            const auto* RESTRICT ptr_argsort_idx = ptr_argsort + first_match_idx;

            auto* RESTRICT ptr_selected_source_sparse_indices_out =
              ptr_selected_source_sparse_indices + offset * selected_source_sparse_indices_nnz_stride;
            const auto* RESTRICT ptr_source_sparse_indices_in =
              ptr_source_sparse_indices + idx * source_sparse_indices_nnz_stride;

            for (hash_t i = 0; i < count; ++i) {
              *ptr_selected_source_idx_out++ = idx;
              *ptr_selected_probably_coalesced_idx_out++ = *ptr_argsort_idx++;

              // res_indices = source._indices().index_select(1, selected_source)
              // The code below fuses this computation with forming
              // selected_source and selected_probably_coalesced.
              for (uint32_t d = 0; d < sdim; ++d) {
                ptr_selected_source_sparse_indices_out[d * selected_source_sparse_indices_dim_stride]
                  = ptr_source_sparse_indices_in[d * source_sparse_indices_dim_stride];
              }
              ptr_selected_source_sparse_indices_out += selected_source_sparse_indices_nnz_stride;
            }

            return 0;
        });
    });

    return std::make_tuple(selected_source, selected_source_sparse_indices, selected_probably_coalesced);
  }();

  const auto res_indices = selected_source_sparse_indices;
  // TODO: fuse 3 next kernel calls into 1.
  const auto selected_source_values = source._values().index_select(0, selected_source);
  const auto selected_probably_coalesced_values = probably_coalesced._values().index_select(0, selected_probably_coalesced);
  const auto res_values = binary_op_t::apply(selected_source_values, selected_probably_coalesced_values)
    // no-op for out-of-place calls, but we still need to cast when the op is supposed to be performed in-place
    // but binary_op_t promotes types. For example, let the op == mul, x.dtype == int8, y.dtype == uint8,
    // then mul(x, y).dtype == int16, while x.mul_(y).dtype == int8 and y.mul_(x).dtype == uint8.
    .to(res.scalar_type());
  const auto res_sparse_dim = source.sparse_dim();
  const auto res_dense_dim = res_values.dim() - 1;
  const auto res_shape = broadcasted_shape;
  const auto res_nnz = selected_source_values.size(0);

  auto* res_sparse_impl = get_sparse_impl(res);
  res_sparse_impl->raw_resize_(res_sparse_dim, res_dense_dim, res_shape);
  res_sparse_impl->set_indices_and_values_unsafe(res_indices, res_values);
  res_sparse_impl->set_nnz_and_narrow(res_nnz);
  // Result is coalesced iff arguments are coalesced, conditioned on the fact
  // that we do not check that intersection hash values are sorted and unique.
  // <= : intersection contains only unique indices (or empty), and the algorithm's
  // behavior is order-preserving. So, the result has only unique indices (or empty) which are sorted.
  // => : proof by contraposition. The contrapositive statement reads
  // `there is an uncoalesced argument => result is not coalesced`.
  // If both arguments are uncoalesced, the result is clearly uncoalesced again
  // thanks to the order-preserving behavior of the algorithm.
  // Otherwise we have a coalesced argument `probably_coalesced` and an uncoalesced `source`.
  // Since the matching beahavior of the algorithm respects the order of `source`, the result
  // will be as coalesced as `source` is, which is uncoalesced.
  res._coalesced_(source.is_coalesced() && probably_coalesced.is_coalesced());
}

template <
  template <typename func_t> class kernel_t,
  typename binary_op_t>
void _sparse_binary_op_intersection_kernel_out(
    Tensor& res,
    const Tensor& x,
    const Tensor& y,
    const bool is_commutative = true
) {
  TORCH_CHECK(
      (x.is_sparse() && y.is_sparse())
      && (x.dim() == y.dim()) && (x.sparse_dim() == y.sparse_dim())
      && (x.sizes().slice(0, x.sparse_dim()) == y.sizes().slice(0, y.sparse_dim())),
      NAME, "(): expects sparse inputs with equal dimensionality, ",
      "number of sparse dimensions, and shape of sparse dimensions");

  const auto broadcasted_shape = infer_size(x.sizes(), y.sizes());

  int64_t max_hash_val = 1;
  for (const auto d : c10::irange(x.sparse_dim())) {
    max_hash_val *= broadcasted_shape[d];
  }

  // Optimization: use 32-bit hash values when possible.
  const auto is_max_hash_32bits = max_hash_val <= std::numeric_limits<int>::max();
  // Intersection nnz could get larger than nnz of either arguments.
  // Example: probably_coalesced and source have only one unique and shared index,
  // then the size of intersection is exactly the product of their nnzs.
  // This nnz defines offsets per thread which are computed using cumsum on values
  // of hash dtype. This becomes a problem when hash_t=int32_t and res_nnz > max(int32_t).
  const auto is_max_offset_32bits = (x._nnz() * y._nnz()) <= std::numeric_limits<int>::max();

  if (is_max_hash_32bits && is_max_offset_32bits) {
    using hash_t = int32_t;
    using offset_t = int32_t;
    _sparse_binary_op_intersection_kernel_impl<kernel_t, binary_op_t, hash_t, offset_t>(
        res, x, y, broadcasted_shape, is_commutative);
  }
  else if (is_max_hash_32bits && !is_max_offset_32bits) {
    using hash_t = int32_t;
    using offset_t = int64_t;
    _sparse_binary_op_intersection_kernel_impl<kernel_t, binary_op_t, hash_t, offset_t>(
        res, x, y, broadcasted_shape, is_commutative);
  }
  else if (!is_max_hash_32bits && is_max_offset_32bits) {
    using hash_t = int64_t;
    using offset_t = int32_t;
    _sparse_binary_op_intersection_kernel_impl<kernel_t, binary_op_t, hash_t, offset_t>(
        res, x, y, broadcasted_shape, is_commutative);
  }
  else {
    using hash_t = int64_t;
    using offset_t = int64_t;
    _sparse_binary_op_intersection_kernel_impl<kernel_t, binary_op_t, hash_t, offset_t>(
        res, x, y, broadcasted_shape, is_commutative);
  }
}

} // anonymous namespace

}} // at::native
