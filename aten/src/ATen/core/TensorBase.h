#pragma once

#include <c10/core/Device.h>
#include <c10/core/Layout.h>
#include <c10/core/MemoryFormat.h>
#include <c10/core/ScalarType.h>
#include <c10/core/ScalarTypeToTypeMeta.h>
#include <c10/core/Storage.h>
#include <c10/core/SymIntArrayRef.h>
#include <c10/core/TensorImpl.h>
#include <c10/core/TensorOptions.h>
#include <c10/core/UndefinedTensorImpl.h>
#include <c10/core/WrapDimMinimal.h>
#include <c10/util/C++17.h>
#include <c10/util/Exception.h>
#include <c10/util/ExclusivelyOwned.h>
#include <c10/util/ExclusivelyOwnedTensorTraits.h>
#include <c10/util/MaybeOwned.h>
#include <optional>
#include <c10/util/intrusive_ptr.h>

#include <ATen/core/NamedTensor.h>
#include <ATen/core/QuantizerBase.h>
#include <ATen/core/TensorAccessor.h>
#include <ATen/StorageUtils.h>

namespace c10 {
class Scalar;
}

namespace torch::autograd {

struct Node;

} // namespace torch::autograd

namespace at {

class Tensor;
class TensorBase;

// Convert Tensor to TensorBase without any need to include Tensor.h
TORCH_API const TensorBase& get_tensor_base(const Tensor& t);

namespace impl {
inline bool variable_excluded_from_dispatch() {
#ifdef C10_MOBILE
  // Please read the comment in `VariableFallbackKernel.cpp` about the background of this change.
  return true;
#else
  return c10::impl::tls_local_dispatch_key_set().excluded_.isSupersetOf(c10::autograd_dispatch_keyset);
#endif
}

}

// NOTE: [Tensor vs. TensorBase]
//
// Tensor, being the central data structure in PyTorch, gets used and
// its header included almost everywhere. Unfortunately this means
// every time an operator signature is updated or changed in
// native_functions.yaml, you (and every other PyTorch developer) need
// to recompile all of ATen and its dependencies.
//
// TensorBase aims to break up these header dependencies, and improve
// incremental build times for all PyTorch developers. TensorBase
// represents a reference counted handle to TensorImpl, exactly the
// same as Tensor. However, TensorBase doesn't have code generated
// methods in its API and thus no dependence on native_functions.yaml.
//
// Usage tips
// ----------
// - You can `#define TORCH_ASSERT_NO_OPERATORS` at the top of a .cpp
//   or .cu file to ensure it has no header dependencies on
//   native_functions.yaml (direct or indirect).
// - Tensor inherits from TensorBase, so functions taking
//   `const TensorBase &` are callable with Tensor as well.
// - TensorBase can be converted to Tensor with `Tensor(tensor_base)`,
//   but this requires a reference-count bump. OptionalTensorRef, on
//   the other hand, can materialize a `const Tensor &` without
//   touching the reference-count.
class TORCH_API TensorBase {
 public:
  struct unsafe_borrow_t { explicit unsafe_borrow_t() = default; };

 protected:
  // Create a Tensor with a +0 reference count. Special care must be
  // taken to avoid decrementing this reference count at destruction
  // time. Intended to support MaybeOwnedTraits<Tensor>.
  explicit TensorBase(unsafe_borrow_t, const TensorBase& rhs)
      : impl_(c10::intrusive_ptr<at::TensorImpl, UndefinedTensorImpl>(rhs.impl_.get(), c10::raw::DontIncreaseRefcount{})) {}
  friend MaybeOwnedTraits<TensorBase>;

 public:
  TensorBase() = default;
  // This constructor should not be used by end users and is an implementation
  // detail invoked by autogenerated code.
  explicit TensorBase(
      c10::intrusive_ptr<TensorImpl, UndefinedTensorImpl> tensor_impl)
      : impl_(std::move(tensor_impl)) {
    if (impl_.get() == nullptr) {
      throw std::runtime_error("TensorImpl with nullptr is not supported");
    }
  }
  TensorBase(const TensorBase&) = default;
  TensorBase(TensorBase&&) noexcept = default;
  ~TensorBase() noexcept = default;

 public:
  // Creates a new wrapper from TensorImpl. Intentionally a free method because
  // it should be used with care. Checks necessary invariants
  static TensorBase wrap_tensor_impl(
      c10::intrusive_ptr<TensorImpl, UndefinedTensorImpl> tensor_impl) {
    TensorBase r(std::move(tensor_impl));
    r.enforce_invariants();
    return r;
  }

  int64_t dim() const {
    return impl_->dim();
  }
  int64_t storage_offset() const {
    return impl_->storage_offset();
  }

  TensorBase contiguous(MemoryFormat memory_format=MemoryFormat::Contiguous) const {
    if (is_contiguous_or_false(memory_format)) {
      return *this;
    } else {
      return __dispatch_contiguous(memory_format);
    }
  }

  /// Should be used if *this can reasonably be expected to be contiguous and
  /// performance is important.
  /// Compared to contiguous, it saves a reference count
  /// increment/decrement if *this is already contiguous, at the cost
  /// in all cases of an extra pointer of stack usage, an extra branch
  /// to access, and an extra branch at destruction time.
  c10::MaybeOwned<TensorBase> expect_contiguous(
      MemoryFormat memory_format=MemoryFormat::Contiguous) const &;

  // Use .contiguous() instead. Trying to borrow from a prvalue
  // will only lead to trouble and dangling references.
  c10::MaybeOwned<TensorBase> expect_contiguous(
      MemoryFormat memory_format=MemoryFormat::Contiguous) && = delete;

  const TensorBase& fill_(const c10::Scalar& scalar) const;
  const TensorBase& zero_() const;

  TensorBase to(at::TensorOptions options={}, bool non_blocking=false, bool copy=false, std::optional<at::MemoryFormat> memory_format=std::nullopt) const;

  bool is_complex() const {
    return at::isComplexType(this->scalar_type());
  }

  bool is_floating_point() const {
    return at::isFloatingType(this->scalar_type());
  }

  bool is_signed() const {
    return at::isSignedType(this->scalar_type());
  }

  c10::SymInt sym_size(int64_t dim) const {
    return impl_->sym_size(dim);
  }

  c10::SymInt sym_stride(int64_t dim) const {
    const auto sizes = this->sym_strides();
    const auto ndim = static_cast<int64_t>(sizes.size());
    // false is passed to maybe_wrap_dim so behavior is identical to array access (but with wrapping)
    return sizes[c10::maybe_wrap_dim(dim, ndim, /*wrap_scalar=*/false)];

  }

  int64_t size(int64_t dim) const {
    return impl_->size(dim);
  }

  int64_t stride(int64_t dim) const {
    const auto strides = this->strides();
    const auto ndim = static_cast<int64_t>(strides.size());
    // false is passed to maybe_wrap_dim so behavior is identical to array access (but with wrapping)
    return strides[c10::maybe_wrap_dim(dim, ndim, /*wrap_scalar=*/false)];
  }

  TensorImpl * unsafeGetTensorImpl() const {
    return impl_.get();
  }
  TensorImpl * unsafeReleaseTensorImpl() {
    return impl_.release();
  }
  const c10::intrusive_ptr<TensorImpl, UndefinedTensorImpl>& getIntrusivePtr() const {
    return impl_;
  }

  c10::intrusive_ptr<TensorImpl, UndefinedTensorImpl> unsafeReleaseIntrusivePtr() {
    return std::move(impl_);
  }

  bool defined() const {
    return impl_;
  }

  void reset() {
    impl_.reset();
  }

#if defined (_MSC_VER)
  TensorBase& operator=(const TensorBase& x) & {
    impl_ = x.impl_;
    return *this;
  };
  TensorBase& operator=(TensorBase&& x) & noexcept {
    impl_ = std::move(x.impl_);
    return *this;
  }
#else
  TensorBase& operator=(const TensorBase& x) & = default;
  TensorBase& operator=(TensorBase&& x) & noexcept = default;
#endif

  // Ban assignment to rvalues, since at::Tensor (weirdly) performs a deep copy here
  TensorBase& operator=(const TensorBase&) && = delete;
  TensorBase& operator=(TensorBase&&) && noexcept = delete;

  bool is_same(const TensorBase& other) const noexcept {
    return impl_ == other.impl_;
  }
  size_t use_count() const noexcept {
    return impl_.use_count();
  }
  size_t weak_use_count() const noexcept {
    return impl_.weak_use_count();
  }

  std::string toString() const;

  IntArrayRef sizes() const {
    return impl_->sizes();
  }
  c10::SymIntArrayRef sym_sizes() const {
    return impl_->sym_sizes();
  }
  c10::SymIntArrayRef sym_strides() const {
    return impl_->sym_strides();
  }
  IntArrayRef strides() const {
    return impl_->strides();
  }
  // See impl::get_opt_names in ATen/NamedTensor.h for docs.
  std::optional<DimnameList> opt_names() const {
    return impl::get_opt_names(unsafeGetTensorImpl());
  }
  // See impl::get_names in ATen/NamedTensor.h for docs.
  DimnameList names() const {
    return impl::get_names(unsafeGetTensorImpl());
  }
  int64_t ndimension() const {
    return dim();
  }

  bool is_contiguous(at::MemoryFormat memory_format=at::MemoryFormat::Contiguous) const {
    return impl_->is_contiguous(memory_format);
  }

  // Like is_contiguous, but more dynamic shape-friendly. May return a symbolic representation of
  // contiguity instead of SymTrue SymFalse, when results are data-dependent.
  c10::SymBool sym_is_contiguous(at::MemoryFormat memory_format=at::MemoryFormat::Contiguous) const {
    if (impl_->has_symbolic_sizes_strides()) {
      return impl_->sym_is_contiguous(memory_format);
    }
    return impl_->is_contiguous(memory_format);
  }

  // Like is_contiguous, but more dynamic shape-friendly. Can returns
  // false instead of throwing data-dependent errors for tensors with unbacked
  // sizes or strides.
  bool is_contiguous_or_false(at::MemoryFormat memory_format=at::MemoryFormat::Contiguous) const {
    if (impl_->has_symbolic_sizes_strides()) {
      return impl_->sym_is_contiguous(memory_format).guard_or_false(__FILE__, __LINE__);
    }
    return impl_->is_contiguous(memory_format);
  }

  bool is_non_overlapping_and_dense() const {
    return impl_->is_non_overlapping_and_dense();
  }

  at::MemoryFormat suggest_memory_format(
      bool channels_last_strides_exact_match = false) const {
    // Setting channels_last_strides_exact_match to true forces function to
    // check 0,1 - sized dimension strides.
    if (layout() == at::kStrided) {
      if (impl_->is_strides_like_channels_last()) {
        if (!channels_last_strides_exact_match ||
            get_channels_last_strides_2d(sizes()) == strides()) {
          return at::MemoryFormat::ChannelsLast;
        }
      }
      else if (impl_->is_strides_like_channels_last_3d()) {
        if (!channels_last_strides_exact_match ||
            get_channels_last_strides_3d(sizes()) == strides()) {
          return at::MemoryFormat::ChannelsLast3d;
        }
      }
    }
    return at::MemoryFormat::Contiguous;
  }

  // Total bytes consumed by the "view" of elements of the array.  Does not
  // include size of metadata.  The number reported here does not necessarily
  // correspond to the true physical memory consumed by a tensor; instead,
  // it reports the memory the tensor would take *if* it were contiguous.
  // Defined to be numel() * itemsize()
  size_t nbytes() const {
    TORCH_CHECK(layout () != at::kSparse,
                "nbytes is not defined for sparse tensors.  If you want the size of the constituent " \
                "tensors, add the nbytes of the indices and values.  If you want the size of the  " \
                "equivalent dense tensor, multiply numel() by element_size()");
    return impl_->numel() * impl_->itemsize();
  }

  c10::SymInt sym_nbytes() const {
    TORCH_CHECK(layout () != at::kSparse,
                "nbytes is not defined for sparse tensors.  If you want the size of the constituent " \
                "tensors, add the nbytes of the indices and values.  If you want the size of the  " \
                "equivalent dense tensor, multiply numel() by element_size()");
    return impl_->sym_numel() * impl_->itemsize();
  }

  int64_t numel() const {
    return impl_->numel();
  }

  c10::SymInt sym_numel() const {
    return impl_->sym_numel();
  }

  c10::SymInt sym_storage_offset() const {
    return impl_->sym_storage_offset();
  }

  // Length of one array element in bytes.  This is the traditional
  // Numpy naming.
  size_t itemsize() const {
    return impl_->itemsize();
  }

  // Same as itemsize().  This is the PyTorch naming.
  int64_t element_size() const {
    return static_cast<int64_t>(impl_->itemsize());
  }

  DispatchKeySet key_set() const {
    return impl_->key_set();
  }
  ScalarType scalar_type() const {
    return typeMetaToScalarType(impl_->dtype());
  }
  bool has_storage() const {
    return defined() && impl_->has_storage();
  }
  const Storage& storage() const {
    return impl_->storage();
  }
  bool is_alias_of(const at::TensorBase& other) const{
    return impl_->storage().is_alias_of(other.storage());
  }

  // Move the storage backend to shm based
  // to enable memory sharing across processes.
  //
  // NB1: the ideal behavior of this API still requires further discussion
  // but for now we are inclined to keep it consistent with existing THP behavior
  // https://github.com/pytorch/pytorch/blob/4dca9bde0552afc67b5b74f4a0696fe6055709c4/torch/storage.py#L196-L212
  // so we don't assert on anything here and rely on caller knowing
  // what it's doing.
  //
  // NB2: this currently provides Linux fd based shm support only
  // to simplify the storage lifetime management logic in ATen
  // and similarly for now we are not adding support for file system based
  // shm support like in THP due to additional GC manager support needed
  // to prevent leaks.
  // As such, calling this from non supported systems (e.g. Windows) would fail.
  void share_memory_() {
    at::share_memory_(*this);
  }

  inline bool _is_zerotensor() const {
    return impl_->_is_zerotensor();
  }

  inline void _set_zero(bool zero) const {
    impl_->_set_zero(zero);
  }

  inline bool is_conj() const {
    return impl_->is_conj();
  }

  // sets the conjugate bit of a tensor.
  // NOTE: Conjugate bit is supposed to be a read-only field. Only change this, if you are sure
  // that's what you want. Changing this might lead to incorrect behavior since conjugation is
  // a lazy operation and we rely on this bit to determine if a conjugation needs to be materialized.
  inline void _set_conj(bool conjugate) const {
    impl_->_set_conj(conjugate);
  }

  inline bool is_neg() const {
    return impl_->is_neg();
  }

  // sets the negative bit of a tensor.
  // NOTE: Negative bit is supposed to be a read-only field. Only change this, if you are sure
  // that's what you want. Changing this might lead to incorrect behavior since we rely on this
  // bit to determine if a negation needs to be materialized.
  inline void _set_neg(bool negative) const {
    impl_->_set_neg(negative);
  }

  /// Returns a `Tensor`'s layout.
  Layout layout() const {
    return impl_->layout();
  }

  /// Returns a `Tensor`'s dtype (`TypeMeta`).
  caffe2::TypeMeta dtype() const {
    return impl_->dtype();
  }

  /// Returns a `Tensor`'s device.
  inline Device device() const {
    return impl_->device();
  }

  /// Returns a `Tensor`'s device index.
  DeviceIndex get_device() const {
    // NB: this is not a native function to avoid dispatching overhead.
    return impl_->get_device();
  }

  /// Returns if a `Tensor` has CPU backend.
  bool is_cpu() const {
    // NB: this is not a native function to avoid dispatching overhead.
    return impl_->is_cpu();
  }

  /// Returns if a `Tensor` has CUDA backend.
  bool is_cuda() const {
    // NB: this is not a native function to avoid dispatching overhead.
    return impl_->is_cuda();
  }

  /// Returns if a `Tensor` has IPU backend.
  bool is_ipu() const {
    // NB: this is not a native function to avoid dispatching overhead.
    return impl_->is_ipu();
  }

  /// Returns if a `Tensor` has XPU backend.
  bool is_xpu() const {
    // NB: this is not a native function to avoid dispatching overhead.
    return impl_->is_xpu();
  }

  /// Returns if a `Tensor` has XLA backend.
  bool is_xla() const {
    return impl_->is_xla();
  }

  /// Returns if a `Tensor` has MTIA backend.
  bool is_mtia() const {
    return impl_->is_mtia();
  }

  /// Returns if a `Tensor` has HPU backend.
  bool is_hpu() const {
    return impl_->is_hpu();
  }

  /// Returns if a `Tensor` has Lazy backend.
  bool is_lazy() const {
    return impl_->is_lazy();
  }

  /// Returns if a `Tensor` has HIP backend.
  bool is_hip() const {
    // NB: this is not a native function to avoid dispatching overhead.
    return impl_->is_hip();
  }

  /// Returns if a `Tensor` has VE backend.
  bool is_ve() const {
    // NB: this is not a native function to avoid dispatching overhead.
    return impl_->is_ve();
  }

  /// Returns if a `Tensor` has PrivateUse1 backend.
  bool is_privateuseone() const {
    // NB: this is not a native function to avoid dispatching overhead.
    return impl_->is_privateuseone();
  }

  /// Returns if a `Tensor` has sparse backend.
  bool is_sparse() const {
    // NB: this is not a native function to avoid dispatching overhead.
    return impl_->is_sparse();
  }

  /// Returns is a `Tensor` has a sparse CSR backend.
  bool is_sparse_csr() const {
    // NB: this is not a native function to avoid dispatching overhead.
    return impl_->is_sparse_csr();
  }

  /// Returns if a `Tensor` is mkldnn tensor.
  bool is_mkldnn() const {
    // NB: this is not a native function to avoid dispatching overhead.
    return impl_->is_mkldnn();
  }

  /// Returns if a `Tensor` is mps tensor.
  bool is_mps() const {
    // NB: this is not a native function to avoid dispatching overhead.
    return impl_->is_mps();
  }

  /// Returns if a `Tensor` is maia tensor.
  bool is_maia() const {
    // NB: this is not a native function to avoid dispatching overhead.
    return impl_->is_maia();
  }

  /// Returns if a `Tensor` is vulkan tensor.
  bool is_vulkan() const {
    // NB: this is not a native function to avoid dispatching overhead.
    return impl_->is_vulkan();
  }

  /// Returns if a `Tensor` is metal tensor.
  bool is_metal() const {
    // NB: this is not a native function to avoid dispatching overhead.
    return impl_->is_metal();
  }

  /// Returns if a `Tensor` has quantized backend.
  bool is_quantized() const {
    // NB: this is not a native function to avoid dispatching overhead.
    return impl_->is_quantized();
  }

  /// Returns if a `Tensor` is a meta tensor.  Meta tensors can
  /// also have other designations.
  bool is_meta() const {
    return impl_->is_meta();
  }

  /// Returns if a `Tensor` is an inference tensor.
  bool is_inference() const {
    return impl_->is_inference();
  }

  // Returns if a `Tensor` is a NestedTensor.
  bool is_nested() const {
    return impl_->is_nested();
  }

  /// If a tensor is a quantized tensor, returns its quantizer
  /// TODO: it's not in native_functions.yaml yet as it's not exposed to python
  QuantizerPtr quantizer() const;

  /// Returns if a `Tensor` has any dimension names
  bool has_names() const {
    // If a user is using unnamed tensors, then we can short-circuit right here.
    // Otherwise, impl::has_names attempts to retrieve names.
    if (!impl_->has_named_tensor_meta()) {
      return false;
    }
    return impl::has_names(unsafeGetTensorImpl());
  }

  /// Returns a `Tensor`'s dimension names data structure
  const NamedTensorMeta* get_named_tensor_meta() const {
    return static_cast<NamedTensorMeta*>(impl_->named_tensor_meta());
  }

  NamedTensorMeta* get_named_tensor_meta() {
    return static_cast<NamedTensorMeta*>(impl_->named_tensor_meta());
  }

  /// Returns the `TensorOptions` corresponding to this `Tensor`. Defined in
  /// TensorOptions.h.
  TensorOptions options() const {
    return TensorOptions().dtype(dtype())
                          .device(device())
                          .layout(layout());
  }

  const void* const_data_ptr() const {
    return this->unsafeGetTensorImpl()->data();
  }

  void* mutable_data_ptr() const {
    return this->unsafeGetTensorImpl()->mutable_data();
  }

  // TODO(#97856) Make this return a const pointer. This currently
  //              returns a non-const pointer because of the large
  //              number of clients that we still want to audit before
  //              migrating to mutable_data_ptr().
  void* data_ptr() const {
    return mutable_data_ptr();
  }

  template <typename T, std::enable_if_t<!std::is_const_v<T>, int> = 0>
  const T* const_data_ptr() const;

  template <typename T, std::enable_if_t<std::is_const_v<T>, int> = 0>
  const std::remove_const_t<T>* const_data_ptr() const;

  template <typename T>
  T* mutable_data_ptr() const;

  // Legacy interface during the migration to indicate that a callsite
  // has not been audited for mutability.
  //
  // Do not add new uses of this, use const_data_ptr() if possible,
  // mutable_data_ptr() otherwise.
  //
  // TODO(#97856) Make this return a const pointer. This is currently
  //              const because of the vast number of clients that
  //              rely on this.
  template <typename T>
  T* data_ptr() const;

  // Purposely not defined here to avoid inlining
  void print() const;

  // Return a `TensorAccessor` for CPU `Tensor`s. You have to specify scalar type and
  // dimension.
  template<typename T, size_t N>
  TensorAccessor<T,N> accessor() const& {
    static_assert(N > 0, "accessor is used for indexing tensor, for scalars use *data_ptr<T>()");
    TORCH_CHECK(dim() == N, "TensorAccessor expected ", N, " dims but tensor has ", dim());
    T* ptr = nullptr;
    if constexpr (std::is_const_v<T>) {
      ptr = const_data_ptr<T>();
    } else {
      ptr = mutable_data_ptr<T>();
    }
    return TensorAccessor<T,N>(ptr,sizes().data(),strides().data());
  }
  template<typename T, size_t N>
  TensorAccessor<T,N> accessor() && = delete;

  // Return a `GenericPackedTensorAccessor` for CUDA `Tensor`s. You have to specify scalar type and
  // dimension. You can optionally specify RestrictPtrTraits as a template parameter to
  // cast the data pointer to a __restrict__ pointer.
  // In order to use this, your CUDA kernel has to take a corresponding GenericPackedTensorAccessor
  // as an argument.
  template<typename T, size_t N, template <typename U> class PtrTraits = DefaultPtrTraits, typename index_t = int64_t>
  GenericPackedTensorAccessor<T,N,PtrTraits,index_t> generic_packed_accessor() const& {
    static_assert(N > 0, "accessor is used for indexing tensor, for scalars use *data_ptr<T>()");
    TORCH_CHECK(dim() == N, "TensorAccessor expected ", N, " dims but tensor has ", dim());
    T* ptr = nullptr;
    if constexpr (std::is_const_v<T>) {
      ptr = const_data_ptr<T>();
    } else {
      ptr = mutable_data_ptr<T>();
    }
    return GenericPackedTensorAccessor<T,N,PtrTraits,index_t>(static_cast<typename PtrTraits<T>::PtrType>(ptr),sizes().data(),strides().data());
  }
  template<typename T, size_t N, template <typename U> class PtrTraits = DefaultPtrTraits, typename index_t = int64_t>
  GenericPackedTensorAccessor<T,N> generic_packed_accessor() && = delete;

  template<typename T, size_t N, template <typename U> class PtrTraits = DefaultPtrTraits>
  PackedTensorAccessor32<T,N,PtrTraits> packed_accessor32() const& {
    TORCH_CHECK(
        impl_->numel() <=
            static_cast<int64_t>(std::numeric_limits<int32_t>::max()),
        "numel needs to be smaller than int32_t max; otherwise, please use packed_accessor64");
    return generic_packed_accessor<T,N,PtrTraits,int32_t>();
  }
  template<typename T, size_t N, template <typename U> class PtrTraits = DefaultPtrTraits>
  PackedTensorAccessor32<T,N,PtrTraits> packed_accessor32() && = delete;

  template<typename T, size_t N, template <typename U> class PtrTraits = DefaultPtrTraits>
  PackedTensorAccessor64<T,N,PtrTraits> packed_accessor64() const& {
    return generic_packed_accessor<T,N,PtrTraits,int64_t>();
  }
  template<typename T, size_t N, template <typename U> class PtrTraits = DefaultPtrTraits>
  PackedTensorAccessor64<T,N,PtrTraits> packed_accessor64() && = delete;

  // ~~~~~ Autograd API ~~~~~

  /// \fn bool is_leaf() const;
  ///
  /// All Tensors that have `requires_grad()` which is ``false`` will be leaf Tensors by convention.
  ///
  /// For Tensors that have `requires_grad()` which is ``true``, they will be leaf Tensors if they were
  /// created by the user. This means that they are not the result of an operation and so
  /// `grad_fn()` is `nullptr`.
  ///
  /// Only leaf Tensors will have their `grad()` populated during a call to `backward()`.
  /// To get `grad()` populated for non-leaf Tensors, you can use `retain_grad()`.
  ///
  /// Example:
  /// @code
  /// auto a = torch::rand(10, torch::requires_grad());
  /// std::cout << a.is_leaf() << std::endl; // prints `true`
  ///
  /// auto b = torch::rand(10, torch::requires_grad()).to(torch::kCUDA);
  /// std::cout << b.is_leaf() << std::endl; // prints `false`
  /// // b was created by the operation that cast a cpu Tensor into a cuda Tensor
  ///
  /// auto c = torch::rand(10, torch::requires_grad()) + 2;
  /// std::cout << c.is_leaf() << std::endl; // prints `false`
  /// // c was created by the addition operation
  ///
  /// auto d = torch::rand(10).cuda();
  /// std::cout << d.is_leaf() << std::endl; // prints `true`
  /// // d does not require gradients and so has no operation creating it (that is tracked by the autograd engine)
  ///
  /// auto e = torch::rand(10).cuda().requires_grad_();
  /// std::cout << e.is_leaf() << std::endl; // prints `true`
  /// // e requires gradients and has no operations creating it
  ///
  /// auto f = torch::rand(10, torch::device(torch::kCUDA).requires_grad(true));
  /// std::cout << f.is_leaf() << std::endl; // prints `true`
  /// // f requires grad, has no operation creating it
  /// @endcode

  /// \fn void backward(const Tensor & gradient={}, std::optional<bool> retain_graph=std::nullopt, bool create_graph=false, std::optional<TensorList> inputs=std::nullopt) const;
  ///
  /// Computes the gradient of current tensor with respect to graph leaves.
  ///
  /// The graph is differentiated using the chain rule. If the tensor is
  /// non-scalar (i.e. its data has more than one element) and requires
  /// gradient, the function additionally requires specifying ``gradient``.
  /// It should be a tensor of matching type and location, that contains
  /// the gradient of the differentiated function w.r.t. this Tensor.
  ///
  /// This function accumulates gradients in the leaves - you might need to
  /// zero them before calling it.
  ///
  /// \param gradient Gradient w.r.t. the
  ///     tensor. If it is a tensor, it will be automatically converted
  ///     to a Tensor that does not require grad unless ``create_graph`` is True.
  ///     None values can be specified for scalar Tensors or ones that
  ///     don't require grad. If a None value would be acceptable then
  ///     this argument is optional.
  /// \param retain_graph If ``false``, the graph used to compute
  ///     the grads will be freed. Note that in nearly all cases setting
  ///     this option to True is not needed and often can be worked around
  ///     in a much more efficient way. Defaults to the value of
  ///     ``create_graph``.
  /// \param create_graph If ``true``, graph of the derivative will
  ///     be constructed, allowing to compute higher order derivative
  ///     products. Defaults to ``false``.
  /// \param inputs Inputs w.r.t. which the gradient will be accumulated into
  ///     ``at::Tensor::grad``. All other Tensors will be ignored. If not
  ///     provided, the gradient is accumulated into all the leaf Tensors
  ///     that were used to compute the current tensor.
  ///     When inputs are provided and a given input is not a leaf,
  ///     the current implementation will call its grad_fn (even though it is not strictly needed to get this gradients).
  ///     It is an implementation detail on which the user should not rely.
  ///     See https://github.com/pytorch/pytorch/pull/60521#issuecomment-867061780 for more details.

  /// \fn Tensor detach() const;
  ///
  /// Returns a new Tensor, detached from the current graph.
  /// The result will never require gradient.

  /// \fn Tensor & detach_() const;
  ///
  /// Detaches the Tensor from the graph that created it, making it a leaf.
  /// Views cannot be detached in-place.

  /// \fn void retain_grad() const;
  ///
  /// Enables this Tensor to have their :attr:`grad` populated during
  /// :func:`backward`. This is a no-op for leaf tensors.

  /// \fn bool retains_grad() const;
  ///
  /// Is ``true`` if this Tensor is non-leaf and its :attr:`grad` is enabled to be
  /// populated during :func:`backward`, ``false`` otherwise.

  const TensorBase& set_requires_grad(bool requires_grad) const {
    impl_->set_requires_grad(requires_grad);
    return *this;
  }
  bool requires_grad() const {
    return impl_->requires_grad();
  }

  // The Forward AD API functions below are low level and are not to be used by end
  // users who should use the API provided in torch/csrc/autograd.h

  /// This function returns the forward gradient for this Tensor at the given level.
  const Tensor& _fw_grad(uint64_t level) const {
    return impl_->_fw_grad(level, *this);
  }

  /// This function can be used to set the value of the forward grad.
  /// Note that the given new_grad might not be used directly if it has different
  /// metadata (size/stride/storage offset) compared to this Tensor. In that case,
  /// new_grad content will be copied into a new Tensor
  void _set_fw_grad(const TensorBase& new_grad, uint64_t level, bool is_inplace_op) const {
    impl_->_set_fw_grad(new_grad, *this, level, is_inplace_op);
  }

  /// NOTE: This is similar to the legacy `.data()` function on `Variable`, and is intended
  /// to be used from functions that need to access the `Variable`'s equivalent `Tensor`
  /// (i.e. `Tensor` that shares the same storage and tensor metadata with the `Variable`).
  ///
  /// One notable difference with the legacy `.data()` function is that changes to the
  /// returned `Tensor`'s tensor metadata (e.g. sizes / strides / storage / storage_offset)
  /// will not update the original `Variable`, due to the fact that this function
  /// shallow-copies the `Variable`'s underlying TensorImpl.
  at::TensorBase tensor_data() const;

  /// NOTE: `var.variable_data()` in C++ has the same semantics as `tensor.data`
  /// in Python, which create a new `Variable` that shares the same storage and
  /// tensor metadata with the original `Variable`, but with a completely new
  /// autograd history.
  ///
  /// NOTE: If we change the tensor metadata (e.g. sizes / strides /
  /// storage / storage_offset) of a variable created from `var.variable_data()`, those
  /// changes will not update the original variable `var`. In `.variable_data()`, we set
  /// `allow_tensor_metadata_change_` to false to make such changes explicitly illegal,
  /// in order to prevent users from changing metadata of `var.variable_data()`
  /// and expecting the original variable `var` to also be updated.
  at::TensorBase variable_data() const;

  // Gradient Node and Edges
  //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  /// Gets the gradient function of the `Variable`. If this is a leaf variable,
  /// the pointer returned will be null.
  ///
  /// For View Variables:
  /// Gets the up-to-date grad_fn. If the shared data or base was modified, we
  /// re-create the grad_fn to express the up-to-date view relationship between
  /// this and the base Variable.
  const std::shared_ptr<torch::autograd::Node>& grad_fn() const;

  // Hooks
  //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  template <typename T>
  using hook_return_void_t = std::enable_if_t<std::is_void_v<typename std::invoke_result_t<T&, TensorBase>>, unsigned>;
  template <typename T>
  using hook_return_var_t = std::enable_if_t<std::is_same_v<typename std::invoke_result_t<T&, TensorBase>, TensorBase>, unsigned>;

  /// Registers a backward hook.
  ///
  /// The hook will be called every time a gradient with respect to the Tensor is computed.
  /// The hook should have one of the following signature:
  /// ```
  /// hook(TensorBase grad) -> TensorBase
  /// ```
  /// ```
  /// hook(TensorBase grad) -> void
  /// ```
  /// The hook should not modify its argument, but it can optionally return a new gradient
  /// which will be used in place of `grad`.
  ///
  /// This function returns the index of the hook in the list which can be used to remove hook.
  ///
  /// Example:
  /// @code
  /// auto v = torch::tensor({0., 0., 0.}, torch::requires_grad());
  /// auto h = v.register_hook([](torch::Tensor grad){ return grad * 2; }); // double the gradient
  /// v.backward(torch::tensor({1., 2., 3.}));
  /// // This prints:
  /// // ```
  /// //  2
  /// //  4
  /// //  6
  /// // [ CPUFloatType{3} ]
  /// // ```
  /// std::cout << v.grad() << std::endl;
  /// v.remove_hook(h);  // removes the hook
  /// @endcode
  template <typename T>
  hook_return_void_t<T> register_hook(T&& hook) const;
  template <typename T>
  hook_return_var_t<T> register_hook(T&& hook) const;

protected:
  unsigned _register_hook(std::function<TensorBase(const TensorBase&)> hook) const;

public:

  /// Remove hook at given position
  void remove_hook(unsigned pos) const;

  // Variable methods
  //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  bool is_leaf() const;

  int64_t output_nr() const;

  void set_data(const TensorBase & new_data) const;

  TensorBase data() const;

  int64_t _version() const;

  void retain_grad() const;

  bool retains_grad() const;

  const TensorBase& requires_grad_(bool _requires_grad=true) const;

  // View Variables
  //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  /// Returns true if this `Variable` is a view of another `Variable`.
  bool is_view() const;

  /// Returns the `Variable` that this `Variable` is a view of. If this
  /// `Variable` is not a view, throw a `std::runtime_error`.
  const TensorBase& _base() const;

  // Miscellaneous
  //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  const std::string& name() const;

protected:
  void enforce_invariants();
  c10::intrusive_ptr<TensorImpl, UndefinedTensorImpl> impl_;

private:
  TensorBase __dispatch_contiguous(c10::MemoryFormat) const;
};

inline DeviceIndex get_device(const TensorBase& self) {
  return self.get_device();
}

template <typename T>
auto TensorBase::register_hook(T&& hook) const -> TensorBase::hook_return_void_t<T> {
  // Return the grad argument in case of a hook with void return type to have an
  // std::function with Tensor return type
  static_assert(std::is_same_v<decltype(hook(TensorBase())), void>,
                "Expected hook to return void");
  return _register_hook([fn=std::forward<T>(hook)](const TensorBase& grad) {
    fn(grad);
    return TensorBase();
  });
}

template <typename T>
auto TensorBase::register_hook(T&& hook) const -> TensorBase::hook_return_var_t<T> {
  return _register_hook(std::forward<T>(hook));
}

namespace detail {
// Helper creator for Tensor class which doesn't requires the users to pass
// in an intrusive_ptr instead it just converts the argument passed to
// requested intrusive_ptr type.
template <typename T, typename... Args>
TensorBase make_tensor_base(Args&&... args) {
  return TensorBase(c10::make_intrusive<T>(std::forward<Args>(args)...));
}

} // namespace detail

inline DispatchKey legacyExtractDispatchKey(const TensorBase& t) {
  return legacyExtractDispatchKey(t.key_set());
}

} // namespace at

namespace c10 {
template <>
struct MaybeOwnedTraits<at::TensorBase> {
  using owned_type = at::TensorBase;
  using borrow_type = at::TensorBase;

  static borrow_type createBorrow(const owned_type& from) {
    // NOTE: this can be implemented without the special
    // unsafe_borrow_t Tensor constructor as
    //
    // return borrow_type(c10::intrusive_ptr<at::TensorImpl, at::UndefinedTensorImpl>::reclaim(from.unsafeGetTensorImpl()));
    //
    // but that hurts inlining due to the nullptr check in the
    // Tensor(c10::intrusive_ptr<...>) constructor. We already know
    // that from.impl_ isn't null because from is a valid Tensor, so
    // we needn't do the check again. (using __builtin_assume can
    // avoid this, but wouldn't be portable to MSVC.)
    return borrow_type(borrow_type::unsafe_borrow_t{}, from);
  }

  static void assignBorrow(borrow_type& lhs, const borrow_type& rhs) {
    lhs.unsafeReleaseTensorImpl();
    // See above note: this can be implemented with public API
    // similarly to createBorrow(), but that would hurt inlining.
    lhs = borrow_type(borrow_type::unsafe_borrow_t{}, rhs);
  }

  static void destroyBorrow(borrow_type& toDestroy) {
    toDestroy.unsafeReleaseTensorImpl(); // "leak" it, but it was already +0.
  }

  static const owned_type& referenceFromBorrow(const borrow_type& borrow) {
    return borrow;
  }

  static const owned_type* pointerFromBorrow(const borrow_type& borrow) {
    return &borrow;
  }

  static bool debugBorrowIsValid(const borrow_type& /*borrow*/) {
    return true;
  }
};

template <>
struct ExclusivelyOwnedTraits<at::TensorBase> : public c10::ExclusivelyOwnedTensorTraits<at::TensorBase> {};
} // namespace c10

namespace at {

inline c10::MaybeOwned<TensorBase> borrow_from_optional_tensor(
    const std::optional<TensorBase>& opt) {
  return opt.has_value()
    ? c10::MaybeOwned<TensorBase>::borrowed(*opt)
    : c10::MaybeOwned<TensorBase>::owned(std::in_place);
}

inline c10::MaybeOwned<TensorBase> TensorBase::expect_contiguous(MemoryFormat memory_format) const & {
  if (is_contiguous(memory_format)) {
    return c10::MaybeOwned<TensorBase>::borrowed(*this);
  } else {
    return c10::MaybeOwned<TensorBase>::owned(__dispatch_contiguous(memory_format));
  }
}

namespace symint {

template <typename T>
using enable_if_symint = std::enable_if_t<std::is_same_v<T, c10::SymInt>>;
template <typename T>
using enable_if_int = std::enable_if_t<std::is_same_v<T, int64_t>>;

template <typename T, typename = enable_if_symint<T>>
c10::SymIntArrayRef sizes(const TensorBase& t) { return t.sym_sizes(); }
template <typename T, typename = enable_if_int<T>>
IntArrayRef sizes(const TensorBase& t) { return t.sizes(); }

template <typename T, typename = enable_if_symint<T>>
c10::SymInt size(const TensorBase& t, int64_t dim) { return t.sym_size(dim); }
template <typename T, typename = enable_if_int<T>>
int64_t size(const TensorBase& t, int64_t dim) { return t.size(dim); }

template <typename T, typename = enable_if_symint<T>>
c10::SymIntArrayRef strides(const TensorBase& t) { return t.sym_strides(); }
template <typename T, typename = enable_if_int<T>>
IntArrayRef strides(const TensorBase& t) { return t.strides(); }

template <typename T, typename = enable_if_symint<T>>
c10::SymInt numel(const TensorBase& t) { return t.sym_numel(); }
template <typename T, typename = enable_if_int<T>>
int64_t numel(const TensorBase& t) { return t.numel(); }

} // namespace symint

} // namespace at
