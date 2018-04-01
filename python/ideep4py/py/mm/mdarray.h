/*
 *Copyright (c) 2018 Intel Corporation.
 *
 *Permission is hereby granted, free of charge, to any person obtaining a copy
 *of this software and associated documentation files (the "Software"), to deal
 *in the Software without restriction, including without limitation the rights
 *to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *copies of the Software, and to permit persons to whom the Software is
 *furnished to do so, subject to the following conditions:
 *
 *The above copyright notice and this permission notice shall be included in
 *all copies or substantial portions of the Software.
 *
 *THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 *THE SOFTWARE.
 *
 */


#ifndef _MDARRAY_H_
#define _MDARRAY_H_
#include <Python.h>
#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION
#include <numpy/arrayobject.h>
#include <numpy/ndarraytypes.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
#include <numeric>
#include <memory>
#include <forward_list>
#include <stdexcept>
#include <type_traits>
#include <swigpyrun.h>
#include "ideep.hpp"
#include "utils.h"

namespace implementation {
  class mdarray;
}

class reorderer;

using py_handle = std::shared_ptr<implementation::mdarray>;

namespace implementation {

#if PY_VERSION_HEX >= 0x03000000
  int g_init();
#else
  void g_init();
#endif

#define NPY_ARRAY_SURROGATE_ENTRY(mdarray) \
  PyObject *surrogate = PyArray_FromAny(mdarray, nullptr, 0, 0 \
      , NPY_ARRAY_ELEMENTSTRIDES, nullptr)   \

#define NPY_ARRAY_SURROGATE_EXIT()

#define nb_unary_map_impl(method) \
  PyObject * m_ ## method ## _map_impl(PyObject *self) { \
    NPY_ARRAY_SURROGATE_ENTRY(self); \
                                \
    if (surrogate == nullptr)   \
      return nullptr;           \
                                \
    PyObject *res = PyNumber_ ## method(surrogate); \
    Py_DECREF(surrogate);   \
    NPY_ARRAY_SURROGATE_EXIT(); \
    return res;   \
  } \

#define nb_unary_map(method) \
  nb_unary_map_impl(method) \
  PyObject * m_ ## method (PyObject *self) {    \
    return m_ ## method ## _map_impl(self); \
  } \

#define nb_binary_map_impl(method) \
  PyObject * m_ ## method ## _map_impl(PyObject *self, PyObject *o) {   \
    PyObject *left = self, *right = o;                                  \
    if (is_mdarray(left)) {                                             \
      left = PyArray_FromAny(left, nullptr, 0, 0                        \
        , NPY_ARRAY_ELEMENTSTRIDES, nullptr);                           \
    }                                                                   \
    if (is_mdarray(right)) {                                            \
      right = PyArray_FromAny(right, nullptr, 0, 0                      \
        , NPY_ARRAY_ELEMENTSTRIDES, nullptr);                           \
    }                                                                   \
    PyObject *res = PyNumber_ ## method(left, right);                   \
    if (left != self)                                                   \
      Py_DECREF(left);                                                  \
    if (right != o)                                                     \
      Py_DECREF(right);                                                 \
    return res;                                                         \
  }

#define nb_binary_map_impl_with_target_func(method, tfunc) \
  PyObject * m_ ## method ## _map_impl(PyObject *self, PyObject *o) {    \
    NPY_ARRAY_SURROGATE_ENTRY(self); \
                                \
    if (surrogate == nullptr)   \
      return nullptr;           \
                                \
    PyObject *res = PyNumber_ ## tfunc(surrogate, o); \
    Py_DECREF(surrogate);   \
    NPY_ARRAY_SURROGATE_EXIT(); \
    return res;   \
  }

#define nb_binary_map(method) \
  nb_binary_map_impl(method) \
  PyObject * m_ ## method (PyObject *self, PyObject *o) {    \
    return m_ ## method ## _map_impl(self, o); \
  } \

#define nb_ternary_map_impl(method) \
  PyObject * m_ ## method ## _map_impl(PyObject *self, PyObject *o1, PyObject *o2) {    \
    NPY_ARRAY_SURROGATE_ENTRY(self); \
                                \
    if (surrogate == nullptr)   \
      return nullptr;           \
                                \
    PyObject *res = PyNumber_ ## method(surrogate, o1, o2); \
    Py_DECREF(surrogate); \
    NPY_ARRAY_SURROGATE_EXIT(); \
    return res;   \
  }

#define nb_ternary_map(method) \
  nb_ternary_map_impl(method) \
  PyObject * m_ ## method (PyObject *self, PyObject *o1, PyObject *o2) {    \
    return m_ ## method ## _map_impl(self, o1, o2); \
  } \

class mdarray : public ideep::tensor {
public:
  using tensor = ideep::tensor;
  using data_type_t = mkldnn::memory::data_type;
  using dims_t = mkldnn::memory::dims;
  using format_t = ideep::format;
  using error = mkldnn::error;
  using scratch_allocator = ideep::utils::scratch_allocator;
  using reorder = ideep::reorder;

  typedef size_t size_type;

  mdarray() = default;
  virtual ~mdarray() = default;

  // FIXME: memory controled by tensor
  mdarray(const tensor &t) :
      tensor(t),
      buff_(std::shared_ptr<scratch_allocator::byte<tensor>>(
          reinterpret_cast<scratch_allocator::byte<tensor> *>(
          get_data_handle()), [](scratch_allocator::byte<tensor> *) {})),
      view_(nullptr) {}

  // Share from a mdarray
  // * If src mdarray is a memory entity, this mdarray shares the buffer.
  // * If src mdarray is a memory view, this mdarray shares the view.
  // this_mdarray->buff_ (src is entity)
  // this_mdarray->view->rb(other)->data_ (src is view)
  mdarray(const mdarray &m) :
      tensor(m),
      buff_(m.get_shared_buff()),
      view_(nullptr) {
    Py_buffer *view = nullptr;
    if (m.view_.get()) {
      // m is view
      view = new Py_buffer;
      // FIXME: Some operations break attributes consistence between
      // mdarray's view and its tensor. e.g. mdarray/tensor->reshape
      // TODO: To hook reshape like operations to keep array attributes
      // between view and tensor.
      // Currently, to make sure what `reshape` like does before using.
      // Directly copy here under assumption of attributes consistence.
      memcpy((void *)(view), (void *)(m.view_.get()), sizeof(Py_buffer));
      Py_INCREF(m.view_->obj);
    } else {
      // m is entity
    }
    view_.reset(view);
  }

  mdarray(dims_t dims, data_type_t dt) :
      tensor({dims, dt, [dims]() {
            return ndims2format(dims.size());
          } ()}, [&]() {
            return reinterpret_cast<void *>(
                new scratch_allocator::byte<tensor>[dims2size(dims, dt)]);
          } ()),
      buff_(std::shared_ptr<scratch_allocator::byte<tensor>>(
          reinterpret_cast<scratch_allocator::byte<tensor> *>(
          get_data_handle()), [](scratch_allocator::byte<tensor> *) {})),
      view_(nullptr) {}

  mdarray(Py_buffer *view, char input_type='d') :
      tensor({[&]() {
            return dims_t(view->shape, view->shape + view->ndim);
          } (), [&]() {
            data_type_t dt;
            std::string format(view->format);
            if (std::string::npos != format.find_last_of('f')) {
              dt = data_type_t::f32;
            } else if (std::string::npos != format.find_last_of('i')) {
              dt = data_type_t::s32;
            } else if (std::string::npos != format.find_last_of('h')) {
              dt = data_type_t::s16;
            } else if (std::string::npos != format.find_last_of('b')) {
              dt = data_type_t::s8;
            } else if (std::string::npos != format.find_last_of('B')) {
              dt = data_type_t::u8;
            } else {
              throw error(mkldnn_invalid_arguments,
                  std::string("mdarray does not support data type: ") + format);
            }
            return dt;
          } (), [&]() {
            return ndims2format(view->ndim, input_type);
          } ()}, [&]() {
            char *buf = (char *)view->buf;
            // FIXME: 32 byte
            if ((unsigned long long)buf & (SYS_MEMORY_ALIGNMENT - 1)) {
              buf = reinterpret_cast<char *>(
                  new scratch_allocator::byte<tensor>[view->len]);
              // TODO: 4k per thread
              fast_memcpy(buf, (char *)view->buf, view->len);
            }
            return (void *)buf;
          } ()),
      buff_([&] () {
            if (get_data_handle() != view->buf) {
              return std::shared_ptr<scratch_allocator::byte<tensor>>(
                  reinterpret_cast<scratch_allocator::byte<tensor> *>(
                  get_data_handle()),
                  [] (scratch_allocator::byte<tensor> *p) { delete [] p; });
            } else {
              // FIXME: iDeep integration
              return std::shared_ptr<scratch_allocator::byte<tensor>>(
                  reinterpret_cast<scratch_allocator::byte<tensor> *>(
                  view->buf), [] (scratch_allocator::byte<tensor> *p) {});
            }
          } ()), view_(view) {
    /* TODO: input_type */
    if (get_data_handle() != view->buf) {
      view_.reset();
    }
  }

  static bool is_mdarray(PyObject *o);

  //FIXME
  inline void unpickled_data(void *pdata) {
    //data_.reset(reinterpret_cast<avx::byte *>(pdata));
    //m_.set_data_handle(pdata);
    return;
  }

  // PEP 3118 interface
  int build_view(Py_buffer *view, int flags, const reorderer &reorder);

  // PyObject *__getstate__(void) const;

  // void __setstate__(PyObject *state);

  PyObject *py_mdarray_from(PyObject *o) const;

  /// d = a * x + b * y, using x's format
  template<class T>
  static void axpby(tensor &dst, T a, const tensor &x, T b, const tensor &y);

  /// Interface to directly contact python
  template<class T>
  PyObject *axpby(T a, T b, PyObject *o);

  template<class T>
  PyObject *inplace_axpby(T a, PyObject *self, T b, PyObject *o);

  PyObject *flat(void);

  PyObject *reshape(py_handle *self, std::vector<int> dims);

  PyObject *m_mult_div(PyObject *self, PyObject *o, int mult_or_div, bool inplace);

  // PyObject *sum(std::vector<int> axis, bool keepdims);

  // PEP: 3118 Buffer Protocol Producer
  virtual int getbuffer(PyObject *obj, Py_buffer *view, int flags);

  PyObject *getattro(PyObject *self, PyObject *name);

  PyObject *m_Add(PyObject *self, PyObject *o);
  nb_binary_map_impl(Add);
  PyObject *m_InPlaceAdd(PyObject *self, PyObject *o);
  nb_binary_map_impl(InPlaceAdd);
  PyObject *m_Subtract(PyObject *self, PyObject *o);
  nb_binary_map_impl(Subtract);
  PyObject *m_InPlaceSubtract(PyObject *self, PyObject *o);
  nb_binary_map_impl(InPlaceSubtract);
  PyObject *m_Multiply(PyObject *self, PyObject *o);
  nb_binary_map_impl(Multiply);
  PyObject *m_InPlaceMultiply(PyObject *self, PyObject *o);
  nb_binary_map_impl(InPlaceMultiply);
  // SWIG: nb_true_divide (no slot) <= nb_divide
  PyObject *m_Divide(PyObject *self, PyObject *o);
#if PY_VERSION_HEX < 0x03000000
  nb_binary_map_impl(Divide);
#else
  nb_binary_map_impl_with_target_func(Divide, TrueDivide);
#endif
  PyObject *m_InPlaceDivide(PyObject *self, PyObject *o);
#if PY_VERSION_HEX < 0x03000000
  nb_binary_map_impl(InPlaceDivide);
#else
  nb_binary_map_impl_with_target_func(InPlaceDivide, InPlaceTrueDivide);
#endif

  nb_binary_map(Remainder);
  nb_binary_map(Divmod);
  nb_unary_map(Negative);
  nb_unary_map(Positive);
  nb_unary_map(Absolute);
  nb_unary_map(Invert);
  nb_binary_map(Lshift);
  nb_binary_map(Rshift);
  nb_binary_map(And);
  nb_binary_map(Xor);
  nb_binary_map(Or);
  nb_binary_map(InPlaceRemainder);
  nb_ternary_map(InPlacePower);
  nb_binary_map(InPlaceLshift);
  nb_binary_map(InPlaceRshift);
  nb_binary_map(InPlaceAnd);
  nb_binary_map(InPlaceXor);
  nb_binary_map(InPlaceOr);
  nb_binary_map(FloorDivide);
  nb_binary_map(InPlaceFloorDivide);
#if (PY_VERSION_HEX >= 0x03000000)
  nb_binary_map(MatrixMultiply);
  nb_binary_map(InPlaceMatrixMultiply);
#endif

  Py_ssize_t mp_length(PyObject *self);
  PyObject *mp_subscript(PyObject *self, PyObject *op);
  int mp_ass_subscript(PyObject *self, PyObject *ind, PyObject *op);

  inline tensor &get_tensor() { return *this; }

  inline void reset_tensor(tensor &dst) {
      init(dst.get_descriptor(), dst.get_data_handle()); }

  inline std::shared_ptr<scratch_allocator::byte<tensor>>
  get_shared_buff() const { return buff_; }

private:
  static inline size_t dims2size(dims_t &dims, data_type_t dt) {
    size_t itemsize;
    switch(dt) {
    case data_type_t::f32:
    case data_type_t::s32:
      itemsize = 4;
      break;
    case data_type_t::s16:
      itemsize = 2;
      break;
    case data_type_t::u8:
    case data_type_t::s8:
      itemsize = 1;
      break;
    default:
      throw error(mkldnn_invalid_arguments, std::string(
          "mdarray does not support data type: ") + std::to_string(dt));
    }

    size_t nelems = 1;
    for (int d = 0; d < dims.size(); d++)
      nelems *= dims[d];

    return nelems * itemsize;
  }

  static inline
  format_t ndims2format(int ndims, char input_type = 'd')
  {
    switch (ndims) {
    case 1:
      return format_t::x;
    case 2:
      return (input_type == 'd') ? format_t::nc : format_t::oi;
    case 4:
      return (input_type == 'd') ? format_t::nchw : format_t::oihw;
    default:
      throw error(mkldnn_invalid_arguments, std::string(
          "MKLDNN does not support dimensions") + std::to_string(ndims));
      return format_t::format_undef;
    }
  }

  struct view_manager {
    void operator() (const Py_buffer *view) {
      PyBuffer_Release(const_cast<Py_buffer *>(view));
      delete view;
    }
  };

  // FIXME: --> char[]
  std::shared_ptr<scratch_allocator::byte<tensor>> buff_;
  std::unique_ptr<const Py_buffer, view_manager> view_;

protected:
  reorderer *sync_reorder_;
};
}

class reorderer {
public:
  static constexpr int MAX_NDIM = 12; //XXX: For now

  using tensor = ideep::tensor;
  using data_type_t = mkldnn::memory::data_type;
  using format_t = ideep::format;
  using reorder = ideep::reorder;
  using descriptor = tensor::descriptor;
  using scratch_allocator = ideep::utils::scratch_allocator;
  using mdarray = implementation::mdarray;

  bool non_trivial_;
  mdarray dst_;
  std::shared_ptr<scratch_allocator::byte<tensor>> data_;

  int ndims_;
  int size_;
  char format_[4];
  ssize_t itemsize_;
  ssize_t strides_[MAX_NDIM];
  ssize_t shape_[MAX_NDIM];

  void _collect_buffer_info() {
    ndims_ = dst_.ndims();

    switch(dst_.get_data_type()) {
    case data_type_t::f32:
      strcpy(format_, "f");
      itemsize_ = 4;
      break;
    case data_type_t::s32:
      strcpy(format_, "i");
      itemsize_ = 4;
      break;
    case data_type_t::s16:
      strcpy(format_, "h");
      itemsize_ = 2;
      break;
    case data_type_t::s8:
      strcpy(format_, "b");
      itemsize_ = 1;
      break;
    case data_type_t::u8:
      strcpy(format_, "B");
      itemsize_ = 1;
      break;
    default:
      break;
    }

    auto _dims = dst_.get_dims();
    for (int i = 0; i < ndims_; i ++) {
      shape_[i] = _dims[i];
    }

    ssize_t sd = itemsize_;

    for (int i = ndims_ - 1; i >= 0; --i) {
      strides_[i] = sd;
      sd *= shape_[i];
    }
  }

  inline void *data() const { return reinterpret_cast<void *>(data_.get()); }

public:
  reorderer(const mdarray &src) :
      non_trivial_(!src.is_public_format()),
      dst_([&] () {
        if (non_trivial()) {
          mdarray dst;
          dst.init({src.get_dims(), src.get_data_type(),
              descriptor::public_compatible_format(src.get_descriptor())});
          return dst;
        } else {
          return src;
      }} ()),
      size_(src.get_nelems()) {
    if (non_trivial()) {
      data_ = std::shared_ptr<scratch_allocator::byte<tensor>>(
          new scratch_allocator::byte<tensor>[dst_.get_size()],
          [](scratch_allocator::byte<tensor> *p) { delete [] p; });
      dst_.set_data_handle(reinterpret_cast<void *>(data_.get()));
    } else {
      data_ = src.get_shared_buff();
    }

    _collect_buffer_info();
  }

  void fire(const mdarray &src) {
    if (non_trivial())
      reorder::compute(src, dst_);
  }

  void sync(const mdarray &src) {
    if (non_trivial())
      reorder::compute(dst_, src);
  }

  inline bool non_trivial() const {
    return non_trivial_;
  }
};

class mdarray : public py_handle {
public:
  using tensor = ideep::tensor;
  using data_type_t = mkldnn::memory::data_type;

  mdarray() {};

  mdarray(tensor &tensor) :
      py_handle(std::make_shared<implementation::mdarray>(tensor)) {}

  mdarray(mkldnn::memory::dims &dims, mkldnn::memory::data_type dt) :
      py_handle(std::make_shared<implementation::mdarray>(dims, dt)) {}

  mdarray(Py_buffer *view, char input_type='d') :
      py_handle(std::make_shared<implementation::mdarray>(view, input_type)) {}

  static PyObject *mdarray_shape_get(mdarray *self) {
    implementation::mdarray *m = self->get();
    auto dims = m->get_dims();
    auto ndims = m->ndims();
    PyObject *intTuple = PyTuple_New(ndims);

    if (!intTuple)
      goto fail;

    for (int i = 0; i < ndims; i++) {
      PyObject *o = PyLong_FromLong(dims[i]);

      if (!o) {
        Py_DECREF(intTuple);
        intTuple = NULL;
        goto fail;
      }

      PyTuple_SET_ITEM(intTuple, i, o);
    }

    fail:
      return intTuple;
  }

  static PyObject *mdarray_dtype_get(mdarray *self) {
    implementation::mdarray *m = self->get();
    PyArray_Descr *pd;

    // Translate our data_type to numpy one
    switch (m->get_data_type()) {
    case data_type_t::f32:
      pd = PyArray_DescrFromType(NPY_FLOAT);
      break;
    case data_type_t::s32:
      pd= PyArray_DescrFromType(NPY_INT);
      break;
    case data_type_t::s16:
      pd= PyArray_DescrFromType(NPY_INT16);
      break;
    case data_type_t::s8:
      pd= PyArray_DescrFromType(NPY_INT8);
      break;
    case data_type_t::u8:
      pd= PyArray_DescrFromType(NPY_UINT8);
      break;
    default:
      PyErr_SetString(PyExc_ValueError, "Bad mdarray data_type");
      return nullptr;
    }

    return reinterpret_cast<PyObject *>(pd);
  }

  static long mdarray_size_get(mdarray *self) {
    return self->get()->get_nelems();
  }

  static long mdarray_ndim_get(mdarray *self) {
    return self->get()->ndims();
  }

  static bool mdarray_is_mdarray_get(mdarray *self) {
    return true;
  }
};

class reorder_buffer : reorderer {
public:
  reorder_buffer(const py_handle in) :
    reorderer(*in.get()) {}
};

#endif // _MDARRAY_H_
