#ifndef _NPY_ARRAY_CTORS_H_
#define _NPY_ARRAY_CTORS_H_

#ifdef __cplusplus
extern "C" {
#endif

NPY_NO_EXPORT PyObject *
PyArray_NewFromDescr(PyTypeObject *subtype, PyArray_Descr *descr, int nd,
                     intp *dims, intp *strides, void *data,
                     int flags, PyObject *obj);

NPY_NO_EXPORT PyObject *PyArray_New(PyTypeObject *, int nd, intp *,
                             int, intp *, void *, int, int, PyObject *);

NPY_NO_EXPORT PyObject *
PyArray_FromAny(PyObject *op, PyArray_Descr *newtype, int min_depth,
                int max_depth, int flags, PyObject *context);

NPY_NO_EXPORT PyObject *
PyArray_CheckFromAny(PyObject *op, PyArray_Descr *descr, int min_depth,
                     int max_depth, int requires, PyObject *context);

NPY_NO_EXPORT PyObject *
PyArray_FromArray(PyArrayObject *arr, PyArray_Descr *newtype, int flags);

NPY_NO_EXPORT PyObject *
PyArray_FromStructInterface(PyObject *input);

NPY_NO_EXPORT PyObject *
PyArray_FromInterface(PyObject *input);

NPY_NO_EXPORT PyObject *
PyArray_FromArrayAttr(PyObject *op, PyArray_Descr *typecode,
                      PyObject *context);

NPY_NO_EXPORT PyObject *
PyArray_EnsureArray(PyObject *op);

NPY_NO_EXPORT PyObject *
PyArray_EnsureAnyArray(PyObject *op);

NPY_NO_EXPORT int
PyArray_MoveInto(PyArrayObject *dest, PyArrayObject *src);

NPY_NO_EXPORT int
PyArray_CopyAnyInto(PyArrayObject *dest, PyArrayObject *src);

NPY_NO_EXPORT PyObject *
PyArray_CheckAxis(PyArrayObject *arr, int *axis, int flags);

/* TODO: Put the order parameter in PyArray_CopyAnyInto and remove this */
NPY_NO_EXPORT int
PyArray_CopyAsFlat(PyArrayObject *dst, PyArrayObject *src,
                                NPY_ORDER order);

/* FIXME: remove those from here */
NPY_NO_EXPORT size_t
_array_fill_strides(intp *strides, intp *dims, int nd, size_t itemsize,
                    int inflag, int *objflags);

NPY_NO_EXPORT void
_unaligned_strided_byte_copy(char *dst, intp outstrides, char *src,
                             intp instrides, intp N, int elsize);

NPY_NO_EXPORT void
_strided_byte_swap(void *p, intp stride, intp n, int size);

NPY_NO_EXPORT void
copy_and_swap(void *dst, void *src, int itemsize, intp numitems,
              intp srcstrides, int swap);

NPY_NO_EXPORT void
byte_swap_vector(void *p, intp n, int size);

NPY_NO_EXPORT int
PyArray_AssignFromSequence(PyArrayObject *self, PyObject *v);

/*
 * A slight generalization of PyArray_GetArrayParamsFromObject,
 * which also returns whether the input data contains any numpy.NA
 * values.
 *
 * This isn't exposed in the public API.
 */
NPY_NO_EXPORT int
PyArray_GetArrayParamsFromObjectEx(PyObject *op,
                        PyArray_Descr *requested_dtype,
                        npy_bool writeable,
                        PyArray_Descr **out_dtype,
                        int *out_ndim, npy_intp *out_dims,
                        int *out_contains_na,
                        PyArrayObject **out_arr, PyObject *context);

/* Returns 1 if the arrays have overlapping data, 0 otherwise */
NPY_NO_EXPORT int
_arrays_overlap(PyArrayObject *arr1, PyArrayObject *arr2);

/*
 * Calls arr_of_subclass.__array_wrap__(towrap), in order to make 'towrap'
 * have the same ndarray subclass as 'arr_of_subclass'.
 */
NPY_NO_EXPORT PyArrayObject *
PyArray_SubclassWrap(PyArrayObject *arr_of_subclass, PyArrayObject *towrap);

#ifdef __cplusplus
}
#endif

#endif
