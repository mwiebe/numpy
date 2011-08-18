#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "structmember.h"

/*#include <stdio.h>*/
#define NPY_NO_DEPRECATED_API
#define _MULTIARRAYMODULE
#define NPY_NO_PREFIX
#include "numpy/arrayobject.h"

#include "npy_config.h"

#include "numpy/npy_3kcompat.h"

#include "common.h"
#include "iterators.h"
#include "mapping.h"
#include "na_singleton.h"
#include "lowlevel_strided_loops.h"
#include "item_selection.h"

#define SOBJ_NOTFANCY 0
#define SOBJ_ISFANCY 1
#define SOBJ_BADARRAY 2
#define SOBJ_TOOMANY 3
#define SOBJ_LISTTUP 4

static PyObject *
array_subscript_simple(PyArrayObject *self, PyObject *op);

/******************************************************************************
 ***                    IMPLEMENT MAPPING PROTOCOL                          ***
 *****************************************************************************/

NPY_NO_EXPORT Py_ssize_t
array_length(PyArrayObject *self)
{
    if (PyArray_NDIM(self) != 0) {
        return PyArray_DIMS(self)[0];
    } else {
        PyErr_SetString(PyExc_TypeError, "len() of unsized object");
        return -1;
    }
}

NPY_NO_EXPORT PyObject *
array_big_item(PyArrayObject *self, npy_intp i)
{
    char *item;
    PyArrayObject *ret;
    npy_intp dim0;

    if(PyArray_NDIM(self) == 0) {
        PyErr_SetString(PyExc_IndexError,
                        "0-d arrays can't be indexed");
        return NULL;
    }

    /* Bounds check and get the data pointer */
    dim0 = PyArray_DIM(self, 0);
    if (i < 0) {
        i += dim0;
    }
    if (i < 0 || i >= dim0) {
        PyErr_SetString(PyExc_IndexError,"index out of bounds");
        return NULL;
    }
    item = PyArray_DATA(self) + i * PyArray_STRIDE(self, 0);

    /* Create the view array */
    Py_INCREF(PyArray_DESCR(self));
    ret = (PyArrayObject *)PyArray_NewFromDescr(Py_TYPE(self),
                                              PyArray_DESCR(self),
                                              PyArray_NDIM(self)-1,
                                              PyArray_DIMS(self)+1,
                                              PyArray_STRIDES(self)+1, item,
              PyArray_FLAGS(self) & ~(NPY_ARRAY_MASKNA | NPY_ARRAY_OWNMASKNA),
                                              (PyObject *)self);
    if (ret == NULL) {
        return NULL;
    }

    /* Take a view of the mask if it exists */
    if (PyArray_HASMASKNA(self)) {
        PyArrayObject_fieldaccess *fa = (PyArrayObject_fieldaccess *)ret;

        fa->maskna_dtype = PyArray_MASKNA_DTYPE(self);
        Py_INCREF(fa->maskna_dtype);
        fa->maskna_data = PyArray_MASKNA_DATA(self) +
                          i * PyArray_MASKNA_STRIDES(self)[0];
        if (fa->nd > 0) {
            memcpy(fa->maskna_strides, PyArray_MASKNA_STRIDES(self)+1,
                                        fa->nd * sizeof(npy_intp));
        }
        fa->flags |= NPY_ARRAY_MASKNA;
    }

    /* Set the base object */
    Py_INCREF(self);
    if (PyArray_SetBaseObject(ret, (PyObject *)self) < 0) {
        Py_DECREF(ret);
        return NULL;
    }

    PyArray_UpdateFlags(ret, NPY_ARRAY_C_CONTIGUOUS | NPY_ARRAY_F_CONTIGUOUS);
    return (PyObject *)ret;
}

NPY_NO_EXPORT int
_array_ass_item(PyArrayObject *self, Py_ssize_t i, PyObject *v)
{
    return array_ass_big_item(self, (npy_intp) i, v);
}

/* contains optimization for 1-d arrays */
NPY_NO_EXPORT PyObject *
array_item_nice(PyArrayObject *self, Py_ssize_t i)
{
    if (PyArray_NDIM(self) == 1) {
        char *item;
        npy_intp dim0;

        /* Bounds check and get the data pointer */
        dim0 = PyArray_DIM(self, 0);
        if (i < 0) {
            i += dim0;
        }
        if (i < 0 || i >= dim0) {
            PyErr_SetString(PyExc_IndexError,"index out of bounds");
            return NULL;
        }
        item = PyArray_DATA(self) + i * PyArray_STRIDE(self, 0);

        if (PyArray_HASMASKNA(self)) {
            npy_mask maskvalue;

            maskvalue = (npy_mask)*(PyArray_MASKNA_DATA(self) +
                                    i * PyArray_MASKNA_STRIDES(self)[0]);
            if (!NpyMaskValue_IsExposed(maskvalue)) {
                NpyNA_fields *fna;

                fna = (NpyNA_fields *)NpyNA_Type.tp_new(&NpyNA_Type, NULL, NULL);
                if (fna == NULL) {
                    return NULL;
                }

                fna->dtype = PyArray_DESCR(self);
                Py_INCREF(fna->dtype);

                if (PyArray_MASKNA_DTYPE(self)->type_num == NPY_MASK) {
                    fna->payload = NpyMaskValue_GetPayload(maskvalue);
                }

                return (PyObject *)fna;
            }
        }

        return PyArray_Scalar(item, PyArray_DESCR(self), (PyObject *)self);
    }
    else {
        return PyArray_Return(
                (PyArrayObject *) array_big_item(self, (npy_intp) i));
    }
}

NPY_NO_EXPORT int
array_ass_big_item(PyArrayObject *self, npy_intp i, PyObject *v)
{
    PyArrayObject *tmp;
    char *item;
    npy_intp dim0;
    int ret;

    if (v == NULL) {
        PyErr_SetString(PyExc_ValueError,
                        "can't delete array elements");
        return -1;
    }

    if (!PyArray_ISWRITEABLE(self)) {
        PyErr_SetString(PyExc_RuntimeError,
                        "array is not writeable");
        return -1;
    }

    if (PyArray_NDIM(self) == 0) {
        PyErr_SetString(PyExc_IndexError,
                        "0-d arrays can't be indexed.");
        return -1;
    }


    /* For multi-dimensional arrays and NA masked arrays, use CopyObject */
    if (PyArray_NDIM(self) > 1 || PyArray_HASMASKNA(self)) {
        tmp = (PyArrayObject *)array_big_item(self, i);
        if(tmp == NULL) {
            return -1;
        }
        ret = PyArray_CopyObject(tmp, v);
        Py_DECREF(tmp);
        return ret;
    }

    /* Bounds check and get the data pointer */
    dim0 = PyArray_DIM(self, 0);
    if (i < 0) {
        i += dim0;
    }
    if (i < 0 || i >= dim0) {
        PyErr_SetString(PyExc_IndexError,"index out of bounds");
        return -1;
    }
    item = PyArray_DATA(self) + i * PyArray_STRIDE(self, 0);

    return PyArray_DESCR(self)->f->setitem(v, item, self);
}

/* -------------------------------------------------------------- */

static void
_swap_axes(PyArrayMapIterObject *mit, PyArrayObject **ret, int getmap)
{
    PyObject *new;
    int n1, n2, n3, val, bnd;
    int i;
    PyArray_Dims permute;
    npy_intp d[NPY_MAXDIMS];
    PyArrayObject *arr;

    permute.ptr = d;
    permute.len = mit->nd;

    /*
     * arr might not have the right number of dimensions
     * and need to be reshaped first by pre-pending ones
     */
    arr = *ret;
    if (PyArray_NDIM(arr) != mit->nd) {
        for (i = 1; i <= PyArray_NDIM(arr); i++) {
            permute.ptr[mit->nd-i] = PyArray_DIMS(arr)[PyArray_NDIM(arr)-i];
        }
        for (i = 0; i < mit->nd-PyArray_NDIM(arr); i++) {
            permute.ptr[i] = 1;
        }
        new = PyArray_Newshape(arr, &permute, PyArray_ANYORDER);
        Py_DECREF(arr);
        *ret = (PyArrayObject *)new;
        if (new == NULL) {
            return;
        }
    }

    /*
     * Setting and getting need to have different permutations.
     * On the get we are permuting the returned object, but on
     * setting we are permuting the object-to-be-set.
     * The set permutation is the inverse of the get permutation.
     */

    /*
     * For getting the array the tuple for transpose is
     * (n1,...,n1+n2-1,0,...,n1-1,n1+n2,...,n3-1)
     * n1 is the number of dimensions of the broadcast index array
     * n2 is the number of dimensions skipped at the start
     * n3 is the number of dimensions of the result
     */

    /*
     * For setting the array the tuple for transpose is
     * (n2,...,n1+n2-1,0,...,n2-1,n1+n2,...n3-1)
     */
    n1 = mit->iters[0]->nd_m1 + 1;
    n2 = mit->iteraxes[0];
    n3 = mit->nd;

    /* use n1 as the boundary if getting but n2 if setting */
    bnd = getmap ? n1 : n2;
    val = bnd;
    i = 0;
    while (val < n1 + n2) {
        permute.ptr[i++] = val++;
    }
    val = 0;
    while (val < bnd) {
        permute.ptr[i++] = val++;
    }
    val = n1 + n2;
    while (val < n3) {
        permute.ptr[i++] = val++;
    }
    new = PyArray_Transpose(*ret, &permute);
    Py_DECREF(*ret);
    *ret = (PyArrayObject *)new;
}

static PyObject *
PyArray_GetMap(PyArrayMapIterObject *mit)
{

    PyArrayObject *ret, *temp;
    PyArrayIterObject *it;
    int index;
    int swap;
    PyArray_CopySwapFunc *copyswap;

    /* Unbound map iterator --- Bind should have been called */
    if (mit->ait == NULL) {
        return NULL;
    }

    /* This relies on the map iterator object telling us the shape
       of the new array in nd and dimensions.
    */
    temp = mit->ait->ao;
    Py_INCREF(PyArray_DESCR(temp));
    ret = (PyArrayObject *)
        PyArray_NewFromDescr(Py_TYPE(temp),
                             PyArray_DESCR(temp),
                             mit->nd, mit->dimensions,
                             NULL, NULL,
                             PyArray_ISFORTRAN(temp),
                             (PyObject *)temp);
    if (ret == NULL) {
        return NULL;
    }

    /*
     * Now just iterate through the new array filling it in
     * with the next object from the original array as
     * defined by the mapping iterator
     */

    if ((it = (PyArrayIterObject *)PyArray_IterNew((PyObject *)ret)) == NULL) {
        Py_DECREF(ret);
        return NULL;
    }
    index = it->size;
    swap = (PyArray_ISNOTSWAPPED(temp) != PyArray_ISNOTSWAPPED(ret));
    copyswap = PyArray_DESCR(ret)->f->copyswap;
    PyArray_MapIterReset(mit);
    while (index--) {
        copyswap(it->dataptr, mit->dataptr, swap, ret);
        PyArray_MapIterNext(mit);
        PyArray_ITER_NEXT(it);
    }
    Py_DECREF(it);

    /* check for consecutive axes */
    if ((mit->subspace != NULL) && (mit->consec)) {
        if (mit->iteraxes[0] > 0) {  /* then we need to swap */
            _swap_axes(mit, &ret, 1);
        }
    }
    return (PyObject *)ret;
}

static int
PyArray_SetMap(PyArrayMapIterObject *mit, PyObject *op)
{
    PyArrayObject *arr = NULL;
    PyArrayIterObject *it;
    int index;
    int swap;
    PyArray_CopySwapFunc *copyswap;
    PyArray_Descr *descr;

    /* Unbound Map Iterator */
    if (mit->ait == NULL) {
        return -1;
    }
    descr = PyArray_DESCR(mit->ait->ao);
    Py_INCREF(descr);
    arr = (PyArrayObject *)PyArray_FromAny(op, descr,
                                0, 0, NPY_ARRAY_FORCECAST, NULL);
    if (arr == NULL) {
        return -1;
    }
    if ((mit->subspace != NULL) && (mit->consec)) {
        if (mit->iteraxes[0] > 0) {  /* then we need to swap */
            _swap_axes(mit, &arr, 0);
            if (arr == NULL) {
                return -1;
            }
        }
    }

    /* Be sure values array is "broadcastable"
       to shape of mit->dimensions, mit->nd */

    if ((it = (PyArrayIterObject *)\
         PyArray_BroadcastToShape((PyObject *)arr,
                                    mit->dimensions, mit->nd))==NULL) {
        Py_DECREF(arr);
        return -1;
    }

    index = mit->size;
    swap = (PyArray_ISNOTSWAPPED(mit->ait->ao) !=
            (PyArray_ISNOTSWAPPED(arr)));
    copyswap = PyArray_DESCR(arr)->f->copyswap;
    PyArray_MapIterReset(mit);
    /* Need to decref arrays with objects in them */
    if (PyDataType_FLAGCHK(descr, NPY_ITEM_HASOBJECT)) {
        while (index--) {
            PyArray_Item_INCREF(it->dataptr, PyArray_DESCR(arr));
            PyArray_Item_XDECREF(mit->dataptr, PyArray_DESCR(arr));
            memmove(mit->dataptr, it->dataptr, PyArray_ITEMSIZE(arr));
            /* ignored unless VOID array with object's */
            if (swap) {
                copyswap(mit->dataptr, NULL, swap, arr);
            }
            PyArray_MapIterNext(mit);
            PyArray_ITER_NEXT(it);
        }
        Py_DECREF(arr);
        Py_DECREF(it);
        return 0;
    }
    while(index--) {
        memmove(mit->dataptr, it->dataptr, PyArray_ITEMSIZE(arr));
        if (swap) {
            copyswap(mit->dataptr, NULL, swap, arr);
        }
        PyArray_MapIterNext(mit);
        PyArray_ITER_NEXT(it);
    }
    Py_DECREF(arr);
    Py_DECREF(it);
    return 0;
}

NPY_NO_EXPORT int
count_new_axes_0d(PyObject *tuple)
{
    int i, argument_count;
    int ellipsis_count = 0;
    int newaxis_count = 0;

    argument_count = PyTuple_GET_SIZE(tuple);
    for (i = 0; i < argument_count; ++i) {
        PyObject *arg = PyTuple_GET_ITEM(tuple, i);
        if (arg == Py_Ellipsis && !ellipsis_count) {
            ellipsis_count++;
        }
        else if (arg == Py_None) {
            newaxis_count++;
        }
        else {
            break;
        }
    }
    if (i < argument_count) {
        PyErr_SetString(PyExc_IndexError,
                        "0-d arrays can only use a single ()"
                        " or a list of newaxes (and a single ...)"
                        " as an index");
        return -1;
    }
    if (newaxis_count > NPY_MAXDIMS) {
        PyErr_SetString(PyExc_IndexError, "too many dimensions");
        return -1;
    }
    return newaxis_count;
}

NPY_NO_EXPORT PyObject *
add_new_axes_0d(PyArrayObject *arr,  int newaxis_count)
{
    PyArrayObject *other;
    npy_intp dimensions[NPY_MAXDIMS];
    int i;

    for (i = 0; i < newaxis_count; ++i) {
        dimensions[i]  = 1;
    }
    Py_INCREF(PyArray_DESCR(arr));
    other = (PyArrayObject *)PyArray_NewFromDescr(Py_TYPE(arr),
                                PyArray_DESCR(arr),
                                newaxis_count, dimensions,
                                NULL, PyArray_DATA(arr),
                                PyArray_FLAGS(arr),
                                (PyObject *)arr);
    if (other == NULL) {
        return NULL;
    }
    Py_INCREF(arr);
    if (PyArray_SetBaseObject(other, (PyObject *)arr) < 0) {
        Py_DECREF(other);
        return NULL;
    }
    return (PyObject *)other;
}


/* This checks the args for any fancy indexing objects */

static int
fancy_indexing_check(PyObject *args)
{
    int i, n;
    int retval = SOBJ_NOTFANCY;

    if (PyTuple_Check(args)) {
        n = PyTuple_GET_SIZE(args);
        if (n >= NPY_MAXDIMS) {
            return SOBJ_TOOMANY;
        }
        for (i = 0; i < n; i++) {
            PyObject *obj = PyTuple_GET_ITEM(args,i);
            if (PyArray_Check(obj)) {
                int type_num = PyArray_DESCR((PyArrayObject *)obj)->type_num;
                if (PyTypeNum_ISINTEGER(type_num) ||
                                        PyTypeNum_ISBOOL(type_num)) {
                    retval = SOBJ_ISFANCY;
                }
                else {
                    retval = SOBJ_BADARRAY;
                    break;
                }
            }
            else if (PySequence_Check(obj)) {
                retval = SOBJ_ISFANCY;
            }
        }
    }
    else if (PyArray_Check(args)) {
        int type_num = PyArray_DESCR((PyArrayObject *)args)->type_num;
        if (PyTypeNum_ISINTEGER(type_num) || PyTypeNum_ISBOOL(type_num)) {
            return SOBJ_ISFANCY;
        }
        else {
            return SOBJ_BADARRAY;
        }
    }
    else if (PySequence_Check(args)) {
        /*
         * Sequences < NPY_MAXDIMS with any slice objects
         * or newaxis, or Ellipsis is considered standard
         * as long as there are also no Arrays and or additional
         * sequences embedded.
         */
        retval = SOBJ_ISFANCY;
        n = PySequence_Size(args);
        if (n < 0 || n >= NPY_MAXDIMS) {
            return SOBJ_ISFANCY;
        }
        for (i = 0; i < n; i++) {
            PyObject *obj = PySequence_GetItem(args, i);
            if (obj == NULL) {
                return SOBJ_ISFANCY;
            }
            if (PyArray_Check(obj)) {
                int type_num = PyArray_DESCR((PyArrayObject *)obj)->type_num;
                if (PyTypeNum_ISINTEGER(type_num) ||
                                            PyTypeNum_ISBOOL(type_num)) {
                    retval = SOBJ_LISTTUP;
                }
                else {
                    retval = SOBJ_BADARRAY;
                }
            }
            else if (PySequence_Check(obj)) {
                retval = SOBJ_LISTTUP;
            }
            else if (PySlice_Check(obj) || obj == Py_Ellipsis ||
                                                    obj == Py_None) {
                retval = SOBJ_NOTFANCY;
            }
            Py_DECREF(obj);
            if (retval > SOBJ_ISFANCY) {
                return retval;
            }
        }
    }
    return retval;
}

/*
 * Called when treating array object like a mapping -- called first from
 * Python when using a[object] unless object is a standard slice object
 * (not an extended one).
 *
 * There are two situations:
 *
 *   1 - the subscript is a standard view and a reference to the
 *   array can be returned
 *
 *   2 - the subscript uses Boolean masks or integer indexing and
 *   therefore a new array is created and returned.
 */

NPY_NO_EXPORT PyObject *
array_subscript_simple(PyArrayObject *self, PyObject *op)
{
    npy_intp dimensions[NPY_MAXDIMS], strides[NPY_MAXDIMS];
    npy_intp maskna_strides[NPY_MAXDIMS];
    npy_intp offset, maskna_offset;
    int nd;
    PyArrayObject *ret;
    npy_intp value;

    /*
     * PyNumber_Index was introduced in Python 2.5 because of NumPy.
     * http://www.python.org/dev/peps/pep-0357/
     * Let's use it for indexing!
     *
     * Unfortunately, SciPy and possibly other code seems to rely
     * on the lenient coercion. :(
     */
#if 0 /*PY_VERSION_HEX >= 0x02050000*/
    PyObject *ind = PyNumber_Index(op);
    if (ind != NULL) {
        value = PyArray_PyIntAsIntp(ind);
        Py_DECREF(ind);
    }
    else {
        value = -1;
    }
#else
    value = PyArray_PyIntAsIntp(op);
#endif
    if (value == -1 && PyErr_Occurred()) {
        PyErr_Clear();
    }
    else {
        return array_big_item(self, value);
    }

    /* Standard (view-based) Indexing */
    if (PyArray_HASMASKNA(self)) {
        nd = parse_index(self, op, dimensions,
                        strides, &offset, maskna_strides, &maskna_offset);
    }
    else {
        nd = parse_index(self, op, dimensions,
                        strides, &offset, NULL, NULL);
    }
    if (nd == -1) {
        return NULL;
    }

    /* Create a view using the indexing result */
    Py_INCREF(PyArray_DESCR(self));
    ret = (PyArrayObject *)PyArray_NewFromDescr(Py_TYPE(self),
                                PyArray_DESCR(self),
                                nd, dimensions,
                                strides, PyArray_DATA(self) + offset,
                                PyArray_FLAGS(self),
                                (PyObject *)self);
    if (ret == NULL) {
        return NULL;
    }
    Py_INCREF(self);
    if (PyArray_SetBaseObject(ret, (PyObject *)self) < 0) {
        Py_DECREF(ret);
        return NULL;
    }
    PyArray_UpdateFlags(ret, NPY_ARRAY_UPDATE_ALL);

    if (PyArray_HASMASKNA(self)) {
        PyArrayObject_fieldaccess *fret = (PyArrayObject_fieldaccess *)ret;

        fret->maskna_dtype = PyArray_MASKNA_DTYPE(self);
        Py_INCREF(fret->maskna_dtype);

        fret->maskna_data = PyArray_MASKNA_DATA(self) + maskna_offset;

        if (nd > 0) {
            memcpy(fret->maskna_strides, maskna_strides,
                                            nd * sizeof(npy_intp));
        }

        /* This view doesn't own the mask */
        fret->flags |= NPY_ARRAY_MASKNA;
        fret->flags &= ~NPY_ARRAY_OWNMASKNA;
    }

    return (PyObject *)ret;
}

/*
 * Implements boolean indexing. This produces a one-dimensional
 * array which picks out all of the elements of 'self' for which
 * the corresponding element of 'op' is True.
 *
 * This operation is somewhat unfortunate, because to produce
 * a one-dimensional output array, it has to choose a particular
 * iteration order, in the case of NumPy that is always C order even
 * though this function allows different choices.
 */
NPY_NO_EXPORT PyArrayObject *
array_boolean_subscript(PyArrayObject *self,
                    PyArrayObject *bmask, NPY_ORDER order)
{
    npy_intp size, itemsize;
    char *ret_data, *ret_maskna_data = NULL;
    PyArray_Descr *dtype;
    PyArrayObject *ret;
    int self_has_maskna = PyArray_HASMASKNA(self), needs_api = 0;
    npy_intp bmask_size;

    if (PyArray_DESCR(bmask)->type_num != NPY_BOOL) {
        PyErr_SetString(PyExc_TypeError,
                "NumPy boolean array indexing requires a boolean index");
        return NULL;
    }

    /* See the Boolean Indexing section of the missing data NEP */
    if (PyArray_ContainsNA(bmask)) {
        PyErr_SetString(PyExc_ValueError,
                "The boolean mask indexing array "
                "may not contain any NA values");
        return NULL;
    }

    if (PyArray_NDIM(bmask) != PyArray_NDIM(self)) {
        PyErr_SetString(PyExc_ValueError,
                "The boolean mask assignment indexing array "
                "must have the same number of dimensions as "
                "the array being indexed");
        return NULL;
    }


    /*
     * Since we've checked that the mask contains no NAs, we
     * can do a straightforward count of the boolean True values
     * in the raw mask data array.
     */
    size = count_boolean_trues(PyArray_NDIM(bmask), PyArray_DATA(bmask),
                                PyArray_DIMS(bmask), PyArray_STRIDES(bmask));
    /* Correction factor for broadcasting 'bmask' to 'self' */
    bmask_size = PyArray_SIZE(bmask);
    if (bmask_size > 0) {
        size *= PyArray_SIZE(self) / bmask_size;
    }

    /* Allocate the output of the boolean indexing */
    dtype = PyArray_DESCR(self);
    Py_INCREF(dtype);
    ret = (PyArrayObject *)PyArray_NewFromDescr(Py_TYPE(self), dtype, 1, &size,
                                NULL, NULL, 0, (PyObject *)self);
    if (ret == NULL) {
        return NULL;
    }

    /* Allocate an NA mask for ret if required */
    if (self_has_maskna) {
        if (PyArray_AllocateMaskNA(ret, 1, 0, 1) < 0) {
            Py_DECREF(ret);
            return NULL;
        }
        ret_maskna_data = PyArray_MASKNA_DATA(ret);
    }

    itemsize = dtype->elsize;
    ret_data = PyArray_DATA(ret);

    /* Create an iterator for the data */
    if (size > 0) {
        NpyIter *iter;
        PyArrayObject *op[2] = {self, bmask};
        npy_uint32 flags, op_flags[2];
        npy_intp fixed_strides[3];
        PyArray_StridedTransferFn *stransfer = NULL;
        NpyAuxData *transferdata = NULL;

        NpyIter_IterNextFunc *iternext;
        npy_intp innersize, *innerstrides;
        char **dataptrs;

        /* Set up the iterator */
        flags = NPY_ITER_EXTERNAL_LOOP | NPY_ITER_REFS_OK;
        if (self_has_maskna) {
            op_flags[0] = NPY_ITER_READONLY |
                          NPY_ITER_NO_BROADCAST |
                          NPY_ITER_USE_MASKNA;
        }
        else {
            op_flags[0] = NPY_ITER_READONLY |
                          NPY_ITER_NO_BROADCAST;
        }
        /*
         * Since we already checked PyArray_ContainsNA(bmask), can
         * ignore any MASKNA of bmask.
         */
        op_flags[1] = NPY_ITER_READONLY | NPY_ITER_IGNORE_MASKNA;

        iter = NpyIter_MultiNew(2, op, flags, order, NPY_NO_CASTING,
                                op_flags, NULL);
        if (iter == NULL) {
            Py_DECREF(ret);
            return NULL;
        }

        /* Get a dtype transfer function */
        NpyIter_GetInnerFixedStrideArray(iter, fixed_strides);
        if (PyArray_GetDTypeTransferFunction(PyArray_ISALIGNED(self),
                        fixed_strides[0], itemsize,
                        dtype, dtype,
                        0,
                        &stransfer, &transferdata,
                        &needs_api) != NPY_SUCCEED) {
            Py_DECREF(ret);
            NpyIter_Deallocate(iter);
            return NULL;
        }

        /* Get the values needed for the inner loop */
        iternext = NpyIter_GetIterNext(iter, NULL);
        if (iternext == NULL) {
            Py_DECREF(ret);
            NpyIter_Deallocate(iter);
            NPY_AUXDATA_FREE(transferdata);
            return NULL;
        }
        innerstrides = NpyIter_GetInnerStrideArray(iter);
        dataptrs = NpyIter_GetDataPtrArray(iter);

        /* Regular inner loop */
        if (!self_has_maskna) {
            npy_intp self_stride = innerstrides[0];
            npy_intp bmask_stride = innerstrides[1];
            npy_intp subloopsize;
            do {
                innersize = *NpyIter_GetInnerLoopSizePtr(iter);
                char *self_data = dataptrs[0];
                char *bmask_data = dataptrs[1];

                while (innersize > 0) {
                    /* Skip masked values */
                    subloopsize = 0;
                    while (subloopsize < innersize && *bmask_data == 0) {
                        ++subloopsize;
                        bmask_data += bmask_stride;
                    }
                    innersize -= subloopsize;
                    self_data += subloopsize * self_stride;
                    /* Process unmasked values */
                    subloopsize = 0;
                    while (subloopsize < innersize && *bmask_data != 0) {
                        ++subloopsize;
                        bmask_data += bmask_stride;
                    }
                    stransfer(ret_data, itemsize, self_data, self_stride,
                                subloopsize, itemsize, transferdata);
                    innersize -= subloopsize;
                    self_data += subloopsize * self_stride;
                    ret_data += subloopsize * itemsize;
                }
            } while (iternext(iter));
        }
        /* NA masked inner loop */
        else {
            npy_intp i;
            npy_intp self_stride = innerstrides[0];
            npy_intp bmask_stride = innerstrides[1];
            npy_intp maskna_stride = innerstrides[2];
            npy_intp subloopsize;
            do {
                innersize = *NpyIter_GetInnerLoopSizePtr(iter);
                char *self_data = dataptrs[0];
                char *bmask_data = dataptrs[1];
                char *maskna_data = dataptrs[2];

                while (innersize > 0) {
                    /* Skip masked values */
                    subloopsize = 0;
                    while (subloopsize < innersize && *bmask_data == 0) {
                        ++subloopsize;
                        bmask_data += bmask_stride;
                    }
                    innersize -= subloopsize;
                    self_data += subloopsize * self_stride;
                    maskna_data += subloopsize * maskna_stride;
                    /* Process unmasked values */
                    subloopsize = 0;
                    while (subloopsize < innersize && *bmask_data != 0) {
                        ++subloopsize;
                        bmask_data += bmask_stride;
                    }
                    /*
                     * Because it's a newly allocated array, we
                     * don't have to be careful about not overwriting
                     * NA masked values. If 'ret' were an output parameter,
                     * we would have to avoid that.
                     */
                    stransfer(ret_data, itemsize, self_data, self_stride,
                                subloopsize, itemsize, transferdata);
                    /* Copy the mask as well */
                    for (i = 0; i < subloopsize; ++i) {
                        *ret_maskna_data = *maskna_data;
                        ++ret_maskna_data;
                        maskna_data += maskna_stride;
                    }
                    innersize -= subloopsize;
                    self_data += subloopsize * self_stride;
                    ret_data += subloopsize * itemsize;
                }
            } while (iternext(iter));
        }

        NpyIter_Deallocate(iter);
        NPY_AUXDATA_FREE(transferdata);
    }

    return ret;
}

/*
 * Implements boolean indexing assignment. This takes the one-dimensional
 * array 'v' and assigns its values to all of the elements of 'self' for which
 * the corresponding element of 'op' is True.
 *
 * This operation is somewhat unfortunate, because to match up with
 * a one-dimensional output array, it has to choose a particular
 * iteration order, in the case of NumPy that is always C order even
 * though this function allows different choices.
 *
 * Returns 0 on success, -1 on failure.
 */
NPY_NO_EXPORT int
array_ass_boolean_subscript(PyArrayObject *self,
                    PyArrayObject *bmask, PyArrayObject *v, NPY_ORDER order)
{
    npy_intp size, src_itemsize, v_stride, v_maskna_stride = 0;
    char *v_data, *v_maskna_data = NULL;
    int self_has_maskna = PyArray_HASMASKNA(self);
    int v_has_maskna = PyArray_HASMASKNA(v);
    int needs_api = 0;
    npy_intp bmask_size;
    char constant_valid_mask = 1;

    if (PyArray_DESCR(bmask)->type_num != NPY_BOOL) {
        PyErr_SetString(PyExc_TypeError,
                "NumPy boolean array indexing assignment "
                "requires a boolean index");
        return -1;
    }

    if (PyArray_NDIM(v) > 1) {
        PyErr_Format(PyExc_TypeError,
                "NumPy boolean array indexing assignment "
                "requires a 0 or 1-dimensional input, input "
                "has %d dimensions", PyArray_NDIM(v));
        return -1;
    }

    if (PyArray_NDIM(bmask) != PyArray_NDIM(self)) {
        PyErr_SetString(PyExc_ValueError,
                "The boolean mask assignment indexing array "
                "must have the same number of dimensions as "
                "the array being indexed");
        return -1;
    }

    /* See the Boolean Indexing section of the missing data NEP */
    if (PyArray_ContainsNA(bmask)) {
        PyErr_SetString(PyExc_ValueError,
                "The boolean mask assignment indexing array "
                "may not contain any NA values");
        return -1;
    }

    /* Can't assign an NA to an array which doesn't support it */
    if (v_has_maskna && !self_has_maskna) {
        if (PyArray_ContainsNA(v)) {
            PyErr_SetString(PyExc_ValueError,
                    "Cannot assign NA value to an array which "
                    "does not support NAs");
            return -1;
        }
        /* If there are no actual NAs, allow the assignment */
        else {
            v_has_maskna = 0;
        }
    }

    /*
     * Since we've checked that the mask contains no NAs, we
     * can do a straightforward count of the boolean True values
     * in the raw mask data array.
     */
    size = count_boolean_trues(PyArray_NDIM(bmask), PyArray_DATA(bmask),
                                PyArray_DIMS(bmask), PyArray_STRIDES(bmask));
    /* Correction factor for broadcasting 'bmask' to 'self' */
    bmask_size = PyArray_SIZE(bmask);
    if (bmask_size > 0) {
        size *= PyArray_SIZE(self) / bmask_size;
    }

    /* Tweak the strides for 0-dim and broadcasting cases */
    if (PyArray_NDIM(v) > 0 && PyArray_DIMS(v)[0] > 1) {
        v_stride = PyArray_STRIDES(v)[0];
        if (v_has_maskna) {
            v_maskna_stride = PyArray_MASKNA_STRIDES(v)[0];
        }
        else {
            v_maskna_stride = 0;
        }

        if (size != PyArray_DIMS(v)[0]) {
            PyErr_Format(PyExc_ValueError,
                    "NumPy boolean array indexing assignment "
                    "cannot assign %d input values to "
                    "the %d output values where the mask is true",
                    (int)PyArray_DIMS(v)[0], (int)size);
            return -1;
        }
    }
    else {
        v_stride = 0;
        v_maskna_stride = 0;
    }

    src_itemsize = PyArray_DESCR(v)->elsize;
    v_data = PyArray_DATA(v);
    if (v_has_maskna) {
        v_maskna_data = PyArray_MASKNA_DATA(v);
    }
    /* If assigning unmasked to masked, use a 0-stride all valid mask */
    else if (self_has_maskna) {
        v_maskna_data = &constant_valid_mask;
    }

    /* Create an iterator for the data */
    if (size > 0) {
        NpyIter *iter;
        PyArrayObject *op[2] = {self, bmask};
        npy_uint32 flags, op_flags[2];
        npy_intp fixed_strides[3];

        NpyIter_IterNextFunc *iternext;
        npy_intp innersize, *innerstrides;
        char **dataptrs;

        /* Set up the iterator */
        flags = NPY_ITER_EXTERNAL_LOOP | NPY_ITER_REFS_OK;
        if (self_has_maskna) {
            op_flags[0] = NPY_ITER_WRITEONLY |
                          NPY_ITER_NO_BROADCAST |
                          NPY_ITER_USE_MASKNA;
        }
        else {
            op_flags[0] = NPY_ITER_WRITEONLY |
                          NPY_ITER_NO_BROADCAST;
        }
        /*
         * Since we already checked PyArray_ContainsNA(bmask), can
         * ignore any MASKNA of bmask.
         */
        op_flags[1] = NPY_ITER_READONLY | NPY_ITER_IGNORE_MASKNA;

        iter = NpyIter_MultiNew(2, op, flags, order, NPY_NO_CASTING,
                                op_flags, NULL);
        if (iter == NULL) {
            return -1;
        }

        /* Get the values needed for the inner loop */
        iternext = NpyIter_GetIterNext(iter, NULL);
        if (iternext == NULL) {
            NpyIter_Deallocate(iter);
            return -1;
        }
        innerstrides = NpyIter_GetInnerStrideArray(iter);
        dataptrs = NpyIter_GetDataPtrArray(iter);

        /* Regular inner loop */
        if (!self_has_maskna) {
            PyArray_StridedTransferFn *stransfer = NULL;
            NpyAuxData *transferdata = NULL;
            npy_intp self_stride = innerstrides[0];
            npy_intp bmask_stride = innerstrides[1];
            npy_intp subloopsize;

            /* Get a dtype transfer function */
            NpyIter_GetInnerFixedStrideArray(iter, fixed_strides);
            if (PyArray_GetDTypeTransferFunction(
                            PyArray_ISALIGNED(self) && PyArray_ISALIGNED(v),
                            v_stride, fixed_strides[0],
                            PyArray_DESCR(v), PyArray_DESCR(self),
                            0,
                            &stransfer, &transferdata,
                            &needs_api) != NPY_SUCCEED) {
                NpyIter_Deallocate(iter);
                return -1;
            }

            do {
                innersize = *NpyIter_GetInnerLoopSizePtr(iter);
                char *self_data = dataptrs[0];
                char *bmask_data = dataptrs[1];

                while (innersize > 0) {
                    /* Skip masked values */
                    subloopsize = 0;
                    while (subloopsize < innersize && *bmask_data == 0) {
                        ++subloopsize;
                        bmask_data += bmask_stride;
                    }
                    innersize -= subloopsize;
                    self_data += subloopsize * self_stride;
                    /* Process unmasked values */
                    subloopsize = 0;
                    while (subloopsize < innersize && *bmask_data != 0) {
                        ++subloopsize;
                        bmask_data += bmask_stride;
                    }
                    stransfer(self_data, self_stride, v_data, v_stride,
                                subloopsize, src_itemsize, transferdata);
                    innersize -= subloopsize;
                    self_data += subloopsize * self_stride;
                    v_data += subloopsize * v_stride;
                }
            } while (iternext(iter));

            NPY_AUXDATA_FREE(transferdata);
        }
        /* NA masked inner loop */
        else {
            PyArray_MaskedStridedTransferFn *stransfer = NULL;
            NpyAuxData *transferdata = NULL;
            npy_intp i;
            npy_intp self_stride = innerstrides[0];
            npy_intp bmask_stride = innerstrides[1];
            npy_intp self_maskna_stride = innerstrides[2];
            npy_intp subloopsize;
            PyArray_Descr *v_maskna_dtype;

            if (PyArray_HASMASKNA(v)) {
                v_maskna_dtype = PyArray_MASKNA_DTYPE(v);
                Py_INCREF(v_maskna_dtype);
            }
            else {
                v_maskna_dtype = PyArray_DescrFromType(NPY_BOOL);
                if (v_maskna_dtype == NULL) {
                    NpyIter_Deallocate(iter);
                    return -1;
                }
            }

            /* Get a dtype transfer function */
            NpyIter_GetInnerFixedStrideArray(iter, fixed_strides);
            if (PyArray_GetMaskedDTypeTransferFunction(
                            PyArray_ISALIGNED(self) && PyArray_ISALIGNED(v),
                            v_stride, fixed_strides[0], v_maskna_stride,
                            PyArray_DESCR(v),
                            PyArray_DESCR(self),
                            v_maskna_dtype,
                            0,
                            &stransfer, &transferdata,
                            &needs_api) != NPY_SUCCEED) {
                Py_DECREF(v_maskna_dtype);
                NpyIter_Deallocate(iter);
                return -1;
            }
            Py_DECREF(v_maskna_dtype);

            do {
                innersize = *NpyIter_GetInnerLoopSizePtr(iter);
                char *self_data = dataptrs[0];
                char *bmask_data = dataptrs[1];
                char *self_maskna_data = dataptrs[2];

                while (innersize > 0) {
                    /* Skip masked values */
                    subloopsize = 0;
                    while (subloopsize < innersize && *bmask_data == 0) {
                        ++subloopsize;
                        bmask_data += bmask_stride;
                    }
                    innersize -= subloopsize;
                    self_data += subloopsize * self_stride;
                    self_maskna_data += subloopsize * self_maskna_stride;
                    /* Process unmasked values */
                    subloopsize = 0;
                    while (subloopsize < innersize && *bmask_data != 0) {
                        ++subloopsize;
                        bmask_data += bmask_stride;
                    }
                    /*
                     * Because we're assigning to an existing array,
                     * we have to be careful about not overwriting
                     * NA masked values.
                     */
                    stransfer(self_data, self_stride, v_data, v_stride,
                                (npy_mask *)v_maskna_data, v_maskna_stride,
                                subloopsize, src_itemsize, transferdata);
                    /* Copy the mask as well */
                    for (i = 0; i < subloopsize; ++i) {
                        *self_maskna_data = *v_maskna_data;
                        self_maskna_data += self_maskna_stride;
                        v_maskna_data += v_maskna_stride;
                    }
                    innersize -= subloopsize;
                    self_data += subloopsize * self_stride;
                    v_data += subloopsize * v_stride;
                }
            } while (iternext(iter));

            NPY_AUXDATA_FREE(transferdata);
        }

        NpyIter_Deallocate(iter);
    }

    return 0;
}

NPY_NO_EXPORT PyObject *
array_subscript(PyArrayObject *self, PyObject *op)
{
    int nd, fancy;
    PyArrayObject *other;
    PyArrayMapIterObject *mit;
    PyObject *obj;

    if (PyString_Check(op) || PyUnicode_Check(op)) {
        PyObject *temp;

        if (PyDataType_HASFIELDS(PyArray_DESCR(self))) {
            obj = PyDict_GetItem(PyArray_DESCR(self)->fields, op);
            if (obj != NULL) {
                PyArray_Descr *descr;
                int offset;
                PyObject *title;

                if (PyArg_ParseTuple(obj, "Oi|O", &descr, &offset, &title)) {
                    Py_INCREF(descr);
                    return PyArray_GetField(self, descr, offset);
                }
            }
        }

        temp = op;
        if (PyUnicode_Check(op)) {
            temp = PyUnicode_AsUnicodeEscapeString(op);
        }
        PyErr_Format(PyExc_ValueError,
                     "field named %s not found.",
                     PyBytes_AsString(temp));
        if (temp != op) {
            Py_DECREF(temp);
        }
        return NULL;
    }

    /* Check for multiple field access */
    if (PyDataType_HASFIELDS(PyArray_DESCR(self)) &&
                        PySequence_Check(op) &&
                        !PyTuple_Check(op)) {
        int seqlen, i;
        seqlen = PySequence_Size(op);
        for (i = 0; i < seqlen; i++) {
            obj = PySequence_GetItem(op, i);
            if (!PyString_Check(obj) && !PyUnicode_Check(obj)) {
                Py_DECREF(obj);
                break;
            }
            Py_DECREF(obj);
        }
        /*
         * extract multiple fields if all elements in sequence
         * are either string or unicode (i.e. no break occurred).
         */
        fancy = ((seqlen > 0) && (i == seqlen));
        if (fancy) {
            PyObject *_numpy_internal;
            _numpy_internal = PyImport_ImportModule("numpy.core._internal");
            if (_numpy_internal == NULL) {
                return NULL;
            }
            obj = PyObject_CallMethod(_numpy_internal,
                    "_index_fields", "OO", self, op);
            Py_DECREF(_numpy_internal);
            return obj;
        }
    }

    if (op == Py_Ellipsis) {
        Py_INCREF(self);
        return (PyObject *)self;
    }

    if (PyArray_NDIM(self) == 0) {
        if (op == Py_None) {
            return add_new_axes_0d(self, 1);
        }
        if (PyTuple_Check(op)) {
            if (0 == PyTuple_GET_SIZE(op))  {
                Py_INCREF(self);
                return (PyObject *)self;
            }
            if ((nd = count_new_axes_0d(op)) == -1) {
                return NULL;
            }
            return add_new_axes_0d(self, nd);
        }
        /* Allow Boolean mask selection also */
        if ((PyArray_Check(op) && (PyArray_DIMS((PyArrayObject *)op)==0)
                                && PyArray_ISBOOL((PyArrayObject *)op))) {
            if (PyObject_IsTrue(op)) {
                Py_INCREF(self);
                return (PyObject *)self;
            }
            else {
                npy_intp oned = 0;
                Py_INCREF(PyArray_DESCR(self));
                return PyArray_NewFromDescr(Py_TYPE(self),
                                            PyArray_DESCR(self),
                                            1, &oned,
                                            NULL, NULL,
                                            NPY_ARRAY_DEFAULT,
                                            NULL);
            }
        }
        PyErr_SetString(PyExc_IndexError, "0-d arrays can't be indexed.");
        return NULL;
    }

    /* Boolean indexing special case which supports mask NA */
    if (PyArray_Check(op) && (PyArray_TYPE((PyArrayObject *)op) == NPY_BOOL)
                && (PyArray_NDIM(self) == PyArray_NDIM((PyArrayObject *)op))) {
        return (PyObject *)array_boolean_subscript(self,
                                        (PyArrayObject *)op, NPY_CORDER);
    }

    fancy = fancy_indexing_check(op);
    if (fancy != SOBJ_NOTFANCY) {
        int oned;

        oned = ((PyArray_NDIM(self) == 1) &&
                !(PyTuple_Check(op) && PyTuple_GET_SIZE(op) > 1));

        /* wrap arguments into a mapiter object */
        mit = (PyArrayMapIterObject *) PyArray_MapIterNew(op, oned, fancy);
        if (mit == NULL) {
            return NULL;
        }
        if (oned) {
            PyArrayIterObject *it;
            PyObject *rval;
            it = (PyArrayIterObject *) PyArray_IterNew((PyObject *)self);
            if (it == NULL) {
                Py_DECREF(mit);
                return NULL;
            }
            rval = iter_subscript(it, mit->indexobj);
            Py_DECREF(it);
            Py_DECREF(mit);
            return rval;
        }
        PyArray_MapIterBind(mit, self);
        other = (PyArrayObject *)PyArray_GetMap(mit);
        Py_DECREF(mit);
        return (PyObject *)other;
    }

    return array_subscript_simple(self, op);
}


/*
 * Another assignment hacked by using CopyObject.
 * This only works if subscript returns a standard view.
 * Again there are two cases.  In the first case, PyArray_CopyObject
 * can be used.  In the second case, a new indexing function has to be
 * used.
 */

static int
array_ass_sub_simple(PyArrayObject *self, PyObject *index, PyObject *op)
{
    int ret;
    PyArrayObject *tmp;
    npy_intp value;

    value = PyArray_PyIntAsIntp(index);
    if (!error_converting(value)) {
        return array_ass_big_item(self, value, op);
    }
    PyErr_Clear();

    /* Rest of standard (view-based) indexing */

    if (PyArray_CheckExact(self)) {
        tmp = (PyArrayObject *)array_subscript_simple(self, index);
        if (tmp == NULL) {
            return -1;
        }
    }
    else {
        PyObject *tmp0;

        /*
         * Note: this code path should never be reached with an index that
         *       produces scalars -- those are handled earlier in array_ass_sub
         */

        tmp0 = PyObject_GetItem((PyObject *)self, index);
        if (tmp0 == NULL) {
            return -1;
        }
        if (!PyArray_Check(tmp0)) {
            PyErr_SetString(PyExc_RuntimeError,
                            "Getitem not returning array.");
            Py_DECREF(tmp0);
            return -1;
        }
        tmp = (PyArrayObject *)tmp0;
    }

    ret = PyArray_CopyObject(tmp, op);
    Py_DECREF(tmp);
    return ret;
}


/* return -1 if tuple-object seq is not a tuple of integers.
   otherwise fill vals with converted integers
*/
static int
_tuple_of_integers(PyObject *seq, npy_intp *vals, int maxvals)
{
    int i;
    PyObject *obj;
    npy_intp temp;

    for(i=0; i<maxvals; i++) {
        obj = PyTuple_GET_ITEM(seq, i);
        if ((PyArray_Check(obj) && PyArray_NDIM((PyArrayObject *)obj) > 0)
                || PyList_Check(obj)) {
            return -1;
        }
        temp = PyArray_PyIntAsIntp(obj);
        if (error_converting(temp)) {
            return -1;
        }
        vals[i] = temp;
    }
    return 0;
}


static int
array_ass_sub(PyArrayObject *self, PyObject *index, PyObject *op)
{
    int ret, oned, fancy;
    PyArrayMapIterObject *mit;
    npy_intp vals[NPY_MAXDIMS];

    if (op == NULL) {
        PyErr_SetString(PyExc_ValueError,
                        "cannot delete array elements");
        return -1;
    }
    if (!PyArray_ISWRITEABLE(self)) {
        PyErr_SetString(PyExc_RuntimeError,
                        "array is not writeable");
        return -1;
    }

    if (PyInt_Check(index) || PyArray_IsScalar(index, Integer) ||
        PyLong_Check(index) || (PyIndex_Check(index) &&
                                !PySequence_Check(index))) {
        npy_intp value;
        value = PyArray_PyIntAsIntp(index);
        if (PyErr_Occurred()) {
            PyErr_Clear();
        }
        else {
            return array_ass_big_item(self, value, op);
        }
    }

    if (PyString_Check(index) || PyUnicode_Check(index)) {
        if (PyDataType_HASFIELDS(PyArray_DESCR(self))) {
            PyObject *obj;

            obj = PyDict_GetItem(PyArray_DESCR(self)->fields, index);
            if (obj != NULL) {
                PyArray_Descr *descr;
                int offset;
                PyObject *title;

                if (PyArg_ParseTuple(obj, "Oi|O", &descr, &offset, &title)) {
                    Py_INCREF(descr);
                    return PyArray_SetField(self, descr, offset, op);
                }
            }
        }

#if defined(NPY_PY3K)
        PyErr_Format(PyExc_ValueError,
                     "field named %S not found.",
                     index);
#else
        PyErr_Format(PyExc_ValueError,
                     "field named %s not found.",
                     PyString_AsString(index));
#endif
        return -1;
    }

    if (index == Py_Ellipsis) {
        /*
         * Doing "a[...] += 1" triggers assigning an array to itself,
         * so this check is needed.
         */
        if ((PyObject *)self == op) {
            return 0;
        }
        else {
            return PyArray_CopyObject(self, op);
        }
    }

    if (PyArray_NDIM(self) == 0) {
        /*
         * Several different exceptions to the 0-d no-indexing rule
         *
         *  1) ellipses (handled above generally)
         *  2) empty tuple
         *  3) Using newaxis (None)
         *  4) Boolean mask indexing
         */
        if (index == Py_None || (PyTuple_Check(index) &&
                                        (0 == PyTuple_GET_SIZE(index) ||
                                         count_new_axes_0d(index) > 0))) {
            return PyArray_DESCR(self)->f->setitem(op, PyArray_DATA(self), self);
        }
        if (PyBool_Check(index) || PyArray_IsScalar(index, Bool) ||
                        (PyArray_Check(index) &&
                         (PyArray_DIMS((PyArrayObject *)index)==0) &&
                         PyArray_ISBOOL((PyArrayObject *)index))) {
            if (PyObject_IsTrue(index)) {
                return PyArray_CopyObject(self, op);
            }
            else { /* don't do anything */
                return 0;
            }
        }
        PyErr_SetString(PyExc_IndexError, "0-d arrays can't be indexed.");
        return -1;
    }

    /* Integer-tuple */
    if (PyTuple_Check(index) &&
                (PyTuple_GET_SIZE(index) == PyArray_NDIM(self)) &&
                (_tuple_of_integers(index, vals, PyArray_NDIM(self)) >= 0)) {
        int idim, ndim = PyArray_NDIM(self);
        npy_intp *shape = PyArray_DIMS(self);
        npy_intp *strides = PyArray_STRIDES(self);
        char *item = PyArray_DATA(self);

        if (!PyArray_HASMASKNA(self)) {
            for (idim = 0; idim < ndim; idim++) {
                npy_intp v = vals[idim];
                if (v < 0) {
                    v += shape[idim];
                }
                if (v < 0 || v >= shape[idim]) {
                    PyErr_Format(PyExc_IndexError,
                                 "index (%"INTP_FMT") out of range "\
                                 "(0<=index<%"INTP_FMT") in dimension %d",
                                 vals[idim], PyArray_DIMS(self)[idim], idim);
                    return -1;
                }
                else {
                    item += v * strides[idim];
                }
            }
            return PyArray_DESCR(self)->f->setitem(op, item, self);
        }
        else {
            char *maskna_item = PyArray_MASKNA_DATA(self);
            npy_intp *maskna_strides = PyArray_MASKNA_STRIDES(self);
            NpyNA *na;

            for (idim = 0; idim < ndim; idim++) {
                npy_intp v = vals[idim];
                if (v < 0) {
                    v += shape[idim];
                }
                if (v < 0 || v >= shape[idim]) {
                    PyErr_Format(PyExc_IndexError,
                                 "index (%"INTP_FMT") out of range "\
                                 "(0<=index<%"INTP_FMT") in dimension %d",
                                 vals[idim], PyArray_DIMS(self)[idim], idim);
                    return -1;
                }
                else {
                    item += v * strides[idim];
                    maskna_item += v * maskna_strides[idim];
                }
            }
            na = NpyNA_FromObject(op, 1);
            if (na == NULL) {
                *maskna_item = 1;
                return PyArray_DESCR(self)->f->setitem(op, item, self);
            }
            else {
                *maskna_item = NpyNA_AsMaskValue(na);
                Py_DECREF(na);
                return 0;
            }
        }
    }
    PyErr_Clear();

    /* Boolean indexing special case with NA mask support */
    if (PyArray_Check(index) &&
                (PyArray_TYPE((PyArrayObject *)index) == NPY_BOOL) &&
                (PyArray_NDIM(self) == PyArray_NDIM((PyArrayObject *)index))) {
        int retcode;
        PyArrayObject *op_arr;
        PyArray_Descr *dtype = NULL;

        /* If it's an NA with no dtype, specify the dtype explicitly */
        if (NpyNA_Check(op) && ((NpyNA_fields *)op)->dtype == NULL) {
            dtype = PyArray_DESCR(self);
            Py_INCREF(dtype);
        }
        op_arr = (PyArrayObject *)PyArray_FromAny(op, dtype, 0, 0,
                                                      NPY_ARRAY_ALLOWNA, NULL);
        if (op_arr == NULL) {
            return -1;
        }

        if (PyArray_NDIM(op_arr) < 2) {
            retcode = array_ass_boolean_subscript(self,
                            (PyArrayObject *)index,
                            op_arr, NPY_CORDER);
            Py_DECREF(op_arr);
            return retcode;
        }
        /*
         * Assigning from multi-dimensional 'op' in this case seems
         * inconsistent, so falling through to old code for backwards
         * compatibility.
         */
        Py_DECREF(op_arr);
    }

    fancy = fancy_indexing_check(index);
    if (fancy != SOBJ_NOTFANCY) {

        oned = ((PyArray_NDIM(self) == 1) &&
                !(PyTuple_Check(index) && PyTuple_GET_SIZE(index) > 1));
        mit = (PyArrayMapIterObject *) PyArray_MapIterNew(index, oned, fancy);
        if (mit == NULL) {
            return -1;
        }
        if (oned) {
            PyArrayIterObject *it;
            int rval;

            it = (PyArrayIterObject *)PyArray_IterNew((PyObject *)self);
            if (it == NULL) {
                Py_DECREF(mit);
                return -1;
            }
            rval = iter_ass_subscript(it, mit->indexobj, op);
            Py_DECREF(it);
            Py_DECREF(mit);
            return rval;
        }
        PyArray_MapIterBind(mit, self);
        ret = PyArray_SetMap(mit, op);
        Py_DECREF(mit);
        return ret;
    }

    return array_ass_sub_simple(self, index, op);
}


/*
 * There are places that require that array_subscript return a PyArrayObject
 * and not possibly a scalar.  Thus, this is the function exposed to
 * Python so that 0-dim arrays are passed as scalars
 */


static PyObject *
array_subscript_nice(PyArrayObject *self, PyObject *op)
{

    PyArrayObject *mp;
    npy_intp vals[NPY_MAXDIMS];

    if (PyInt_Check(op) || PyArray_IsScalar(op, Integer) ||
        PyLong_Check(op) || (PyIndex_Check(op) &&
                             !PySequence_Check(op))) {
        npy_intp value;
        value = PyArray_PyIntAsIntp(op);
        if (PyErr_Occurred()) {
            PyErr_Clear();
        }
        else {
            return array_item_nice(self, (Py_ssize_t) value);
        }
    }
    /* optimization for a tuple of integers */
    if (PyArray_NDIM(self) > 1 &&
                PyTuple_Check(op) &&
                (PyTuple_GET_SIZE(op) == PyArray_NDIM(self)) &&
                (_tuple_of_integers(op, vals, PyArray_NDIM(self)) >= 0)) {
        int idim, ndim = PyArray_NDIM(self);
        npy_intp *shape = PyArray_DIMS(self);
        npy_intp *strides = PyArray_STRIDES(self);
        char *item = PyArray_DATA(self);

        if (!PyArray_HASMASKNA(self)) {
            for (idim = 0; idim < ndim; idim++) {
                npy_intp v = vals[idim];
                if (v < 0) {
                    v += shape[idim];
                }
                if (v < 0 || v >= shape[idim]) {
                    PyErr_Format(PyExc_IndexError,
                                 "index (%"INTP_FMT") out of range "\
                                 "(0<=index<%"INTP_FMT") in dimension %d",
                                 vals[idim], PyArray_DIMS(self)[idim], idim);
                    return NULL;
                }
                else {
                    item += v * strides[idim];
                }
            }
            return PyArray_Scalar(item, PyArray_DESCR(self), (PyObject *)self);
        }
        else {
            char *maskna_item = PyArray_MASKNA_DATA(self);
            npy_intp *maskna_strides = PyArray_MASKNA_STRIDES(self);

            for (idim = 0; idim < ndim; idim++) {
                npy_intp v = vals[idim];
                if (v < 0) {
                    v += shape[idim];
                }
                if (v < 0 || v >= shape[idim]) {
                    PyErr_Format(PyExc_IndexError,
                                 "index (%"INTP_FMT") out of range "\
                                 "(0<=index<%"INTP_FMT") in dimension %d",
                                 vals[idim], PyArray_DIMS(self)[idim], idim);
                    return NULL;
                }
                else {
                    item += v * strides[idim];
                    maskna_item += v * maskna_strides[idim];
                }
            }
            if (NpyMaskValue_IsExposed((npy_mask)*maskna_item)) {
                return PyArray_Scalar(item, PyArray_DESCR(self),
                                                    (PyObject *)self);
            }
            else {
                return (PyObject *)NpyNA_FromDTypeAndMaskValue(
                            PyArray_DESCR(self), (npy_mask)*maskna_item, 0);
            }
        }
    }
    PyErr_Clear();

    mp = (PyArrayObject *)array_subscript(self, op);
    /*
     * mp could be a scalar if op is not an Int, Scalar, Long or other Index
     * object and still convertable to an integer (so that the code goes to
     * array_subscript_simple).  So, this cast is a bit dangerous..
     */

    /*
     * The following is just a copy of PyArray_Return with an
     * additional logic in the nd == 0 case.
     */

    if (mp == NULL) {
        return NULL;
    }
    if (PyErr_Occurred()) {
        Py_XDECREF(mp);
        return NULL;
    }
    if (PyArray_Check(mp) && PyArray_NDIM(mp) == 0) {
        npy_bool noellipses = TRUE;
        if ((op == Py_Ellipsis) || PyString_Check(op) || PyUnicode_Check(op)) {
            noellipses = FALSE;
        }
        else if (PyBool_Check(op) || PyArray_IsScalar(op, Bool) ||
                 (PyArray_Check(op) &&
                    (PyArray_DIMS((PyArrayObject *)op)==0) &&
                     PyArray_ISBOOL((PyArrayObject *)op))) {
            noellipses = FALSE;
        }
        else if (PySequence_Check(op)) {
            Py_ssize_t n, i;
            PyObject *temp;

            n = PySequence_Size(op);
            i = 0;
            while (i < n && noellipses) {
                temp = PySequence_GetItem(op, i);
                if (temp == Py_Ellipsis) {
                    noellipses = FALSE;
                }
                Py_DECREF(temp);
                i++;
            }
        }
        if (noellipses) {
            PyObject *ret;
            ret = PyArray_ToScalar(PyArray_DATA(mp), mp);
            Py_DECREF(mp);
            return ret;
        }
    }
    return (PyObject *)mp;
}


NPY_NO_EXPORT PyMappingMethods array_as_mapping = {
#if PY_VERSION_HEX >= 0x02050000
    (lenfunc)array_length,              /*mp_length*/
#else
    (inquiry)array_length,              /*mp_length*/
#endif
    (binaryfunc)array_subscript_nice,       /*mp_subscript*/
    (objobjargproc)array_ass_sub,       /*mp_ass_subscript*/
};

/****************** End of Mapping Protocol ******************************/

/*********************** Subscript Array Iterator *************************
 *                                                                        *
 * This object handles subscript behavior for array objects.              *
 *  It is an iterator object with a next method                           *
 *  It abstracts the n-dimensional mapping behavior to make the looping   *
 *     code more understandable (maybe)                                   *
 *     and so that indexing can be set up ahead of time                   *
 */

/*
 * This function takes a Boolean array and constructs index objects and
 * iterators as if nonzero(Bool) had been called
 */
static int
_nonzero_indices(PyObject *myBool, PyArrayIterObject **iters)
{
    PyArray_Descr *typecode;
    PyArrayObject *ba = NULL, *new = NULL;
    int nd, j;
    npy_intp size, i, count;
    npy_bool *ptr;
    npy_intp coords[NPY_MAXDIMS], dims_m1[NPY_MAXDIMS];
    npy_intp *dptr[NPY_MAXDIMS];

    typecode=PyArray_DescrFromType(NPY_BOOL);
    ba = (PyArrayObject *)PyArray_FromAny(myBool, typecode, 0, 0,
                                          NPY_ARRAY_CARRAY, NULL);
    if (ba == NULL) {
        return -1;
    }
    nd = PyArray_NDIM(ba);
    for (j = 0; j < nd; j++) {
        iters[j] = NULL;
    }
    size = PyArray_SIZE(ba);
    ptr = (Bool *)PyArray_DATA(ba);
    count = 0;

    /* pre-determine how many nonzero entries there are */
    for (i = 0; i < size; i++) {
        if (*(ptr++)) {
            count++;
        }
    }

    /* create count-sized index arrays for each dimension */
    for (j = 0; j < nd; j++) {
        new = (PyArrayObject *)PyArray_New(&PyArray_Type, 1, &count,
                                           PyArray_INTP, NULL, NULL,
                                           0, 0, NULL);
        if (new == NULL) {
            goto fail;
        }
        iters[j] = (PyArrayIterObject *)
            PyArray_IterNew((PyObject *)new);
        Py_DECREF(new);
        if (iters[j] == NULL) {
            goto fail;
        }
        dptr[j] = (npy_intp *)PyArray_DATA(iters[j]->ao);
        coords[j] = 0;
        dims_m1[j] = PyArray_DIMS(ba)[j]-1;
    }
    ptr = (npy_bool *)PyArray_DATA(ba);
    if (count == 0) {
        goto finish;
    }

    /*
     * Loop through the Boolean array  and copy coordinates
     * for non-zero entries
     */
    for (i = 0; i < size; i++) {
        if (*(ptr++)) {
            for (j = 0; j < nd; j++) {
                *(dptr[j]++) = coords[j];
            }
        }
        /* Borrowed from ITER_NEXT macro */
        for (j = nd - 1; j >= 0; j--) {
            if (coords[j] < dims_m1[j]) {
                coords[j]++;
                break;
            }
            else {
                coords[j] = 0;
            }
        }
    }

 finish:
    Py_DECREF(ba);
    return nd;

 fail:
    for (j = 0; j < nd; j++) {
        Py_XDECREF(iters[j]);
    }
    Py_XDECREF(ba);
    return -1;
}

/* convert an indexing object to an INTP indexing array iterator
   if possible -- otherwise, it is a Slice or Ellipsis object
   and has to be interpreted on bind to a particular
   array so leave it NULL for now.
*/
static int
_convert_obj(PyObject *obj, PyArrayIterObject **iter)
{
    PyArray_Descr *indtype;
    PyObject *arr;

    if (PySlice_Check(obj) || (obj == Py_Ellipsis)) {
        return 0;
    }
    else if (PyArray_Check(obj) && PyArray_ISBOOL((PyArrayObject *)obj)) {
        return _nonzero_indices(obj, iter);
    }
    else {
        indtype = PyArray_DescrFromType(PyArray_INTP);
        arr = PyArray_FromAny(obj, indtype, 0, 0, NPY_ARRAY_FORCECAST, NULL);
        if (arr == NULL) {
            return -1;
        }
        *iter = (PyArrayIterObject *)PyArray_IterNew(arr);
        Py_DECREF(arr);
        if (*iter == NULL) {
            return -1;
        }
    }
    return 1;
}

/* Reset the map iterator to the beginning */
NPY_NO_EXPORT void
PyArray_MapIterReset(PyArrayMapIterObject *mit)
{
    int i,j; npy_intp coord[NPY_MAXDIMS];
    PyArrayIterObject *it;
    PyArray_CopySwapFunc *copyswap;

    mit->index = 0;

    copyswap = PyArray_DESCR(mit->iters[0]->ao)->f->copyswap;

    if (mit->subspace != NULL) {
        memcpy(coord, mit->bscoord, sizeof(npy_intp)*PyArray_NDIM(mit->ait->ao));
        PyArray_ITER_RESET(mit->subspace);
        for (i = 0; i < mit->numiter; i++) {
            it = mit->iters[i];
            PyArray_ITER_RESET(it);
            j = mit->iteraxes[i];
            copyswap(coord+j,it->dataptr, !PyArray_ISNOTSWAPPED(it->ao),
                     it->ao);
        }
        PyArray_ITER_GOTO(mit->ait, coord);
        mit->subspace->dataptr = mit->ait->dataptr;
        mit->dataptr = mit->subspace->dataptr;
    }
    else {
        for (i = 0; i < mit->numiter; i++) {
            it = mit->iters[i];
            if (it->size != 0) {
                PyArray_ITER_RESET(it);
                copyswap(coord+i,it->dataptr, !PyArray_ISNOTSWAPPED(it->ao),
                         it->ao);
            }
            else {
                coord[i] = 0;
            }
        }
        PyArray_ITER_GOTO(mit->ait, coord);
        mit->dataptr = mit->ait->dataptr;
    }
    return;
}

/*
 * This function needs to update the state of the map iterator
 * and point mit->dataptr to the memory-location of the next object
 */
NPY_NO_EXPORT void
PyArray_MapIterNext(PyArrayMapIterObject *mit)
{
    int i, j;
    npy_intp coord[NPY_MAXDIMS];
    PyArrayIterObject *it;
    PyArray_CopySwapFunc *copyswap;

    mit->index += 1;
    if (mit->index >= mit->size) {
        return;
    }
    copyswap = PyArray_DESCR(mit->iters[0]->ao)->f->copyswap;
    /* Sub-space iteration */
    if (mit->subspace != NULL) {
        PyArray_ITER_NEXT(mit->subspace);
        if (mit->subspace->index >= mit->subspace->size) {
            /* reset coord to coordinates of beginning of the subspace */
            memcpy(coord, mit->bscoord,
                        sizeof(npy_intp)*PyArray_NDIM(mit->ait->ao));
            PyArray_ITER_RESET(mit->subspace);
            for (i = 0; i < mit->numiter; i++) {
                it = mit->iters[i];
                PyArray_ITER_NEXT(it);
                j = mit->iteraxes[i];
                copyswap(coord+j,it->dataptr, !PyArray_ISNOTSWAPPED(it->ao),
                         it->ao);
            }
            PyArray_ITER_GOTO(mit->ait, coord);
            mit->subspace->dataptr = mit->ait->dataptr;
        }
        mit->dataptr = mit->subspace->dataptr;
    }
    else {
        for (i = 0; i < mit->numiter; i++) {
            it = mit->iters[i];
            PyArray_ITER_NEXT(it);
            copyswap(coord+i,it->dataptr,
                     !PyArray_ISNOTSWAPPED(it->ao),
                     it->ao);
        }
        PyArray_ITER_GOTO(mit->ait, coord);
        mit->dataptr = mit->ait->dataptr;
    }
    return;
}

/*
 * Bind a mapiteration to a particular array
 *
 *  Determine if subspace iteration is necessary.  If so,
 *  1) Fill in mit->iteraxes
 *  2) Create subspace iterator
 *  3) Update nd, dimensions, and size.
 *
 *  Subspace iteration is necessary if:  PyArray_NDIM(arr) > mit->numiter
 *
 * Need to check for index-errors somewhere.
 *
 * Let's do it at bind time and also convert all <0 values to >0 here
 * as well.
 */
NPY_NO_EXPORT void
PyArray_MapIterBind(PyArrayMapIterObject *mit, PyArrayObject *arr)
{
    int subnd;
    PyObject *sub, *obj = NULL;
    int i, j, n, curraxis, ellipexp, noellip;
    PyArrayIterObject *it;
    npy_intp dimsize;
    npy_intp *indptr;

    subnd = PyArray_NDIM(arr) - mit->numiter;
    if (subnd < 0) {
        PyErr_SetString(PyExc_ValueError,
                        "too many indices for array");
        return;
    }

    mit->ait = (PyArrayIterObject *)PyArray_IterNew((PyObject *)arr);
    if (mit->ait == NULL) {
        return;
    }
    /* no subspace iteration needed.  Finish up and Return */
    if (subnd == 0) {
        n = PyArray_NDIM(arr);
        for (i = 0; i < n; i++) {
            mit->iteraxes[i] = i;
        }
        goto finish;
    }

    /*
     * all indexing arrays have been converted to 0
     * therefore we can extract the subspace with a simple
     * getitem call which will use view semantics
     *
     * But, be sure to do it with a true array.
     */
    if (PyArray_CheckExact(arr)) {
        sub = array_subscript_simple(arr, mit->indexobj);
    }
    else {
        Py_INCREF(arr);
        obj = PyArray_EnsureArray((PyObject *)arr);
        if (obj == NULL) {
            goto fail;
        }
        sub = array_subscript_simple((PyArrayObject *)obj, mit->indexobj);
        Py_DECREF(obj);
    }

    if (sub == NULL) {
        goto fail;
    }
    mit->subspace = (PyArrayIterObject *)PyArray_IterNew(sub);
    Py_DECREF(sub);
    if (mit->subspace == NULL) {
        goto fail;
    }
    /* Expand dimensions of result */
    n = PyArray_NDIM(mit->subspace->ao);
    for (i = 0; i < n; i++) {
        mit->dimensions[mit->nd+i] = PyArray_DIMS(mit->subspace->ao)[i];
    }
    mit->nd += n;

    /*
     * Now, we still need to interpret the ellipsis and slice objects
     * to determine which axes the indexing arrays are referring to
     */
    n = PyTuple_GET_SIZE(mit->indexobj);
    /* The number of dimensions an ellipsis takes up */
    ellipexp = PyArray_NDIM(arr) - n + 1;
    /*
     * Now fill in iteraxes -- remember indexing arrays have been
     * converted to 0's in mit->indexobj
     */
    curraxis = 0;
    j = 0;
    /* Only expand the first ellipsis */
    noellip = 1;
    memset(mit->bscoord, 0, sizeof(npy_intp)*PyArray_NDIM(arr));
    for (i = 0; i < n; i++) {
        /*
         * We need to fill in the starting coordinates for
         * the subspace
         */
        obj = PyTuple_GET_ITEM(mit->indexobj, i);
        if (PyInt_Check(obj) || PyLong_Check(obj)) {
            mit->iteraxes[j++] = curraxis++;
        }
        else if (noellip && obj == Py_Ellipsis) {
            curraxis += ellipexp;
            noellip = 0;
        }
        else {
            npy_intp start = 0;
            npy_intp stop, step;
            /* Should be slice object or another Ellipsis */
            if (obj == Py_Ellipsis) {
                mit->bscoord[curraxis] = 0;
            }
            else if (!PySlice_Check(obj) ||
                     (slice_GetIndices((PySliceObject *)obj,
                                       PyArray_DIMS(arr)[curraxis],
                                       &start, &stop, &step,
                                       &dimsize) < 0)) {
                PyErr_Format(PyExc_ValueError,
                             "unexpected object "       \
                             "(%s) in selection position %d",
                             Py_TYPE(obj)->tp_name, i);
                goto fail;
            }
            else {
                mit->bscoord[curraxis] = start;
            }
            curraxis += 1;
        }
    }

 finish:
    /* Here check the indexes (now that we have iteraxes) */
    mit->size = PyArray_OverflowMultiplyList(mit->dimensions, mit->nd);
    if (mit->size < 0) {
        PyErr_SetString(PyExc_ValueError,
                        "dimensions too large in fancy indexing");
        goto fail;
    }
    if (mit->ait->size == 0 && mit->size != 0) {
        PyErr_SetString(PyExc_ValueError,
                        "invalid index into a 0-size array");
        goto fail;
    }

    for (i = 0; i < mit->numiter; i++) {
        npy_intp indval;
        it = mit->iters[i];
        PyArray_ITER_RESET(it);
        dimsize = PyArray_DIMS(arr)[mit->iteraxes[i]];
        while (it->index < it->size) {
            indptr = ((npy_intp *)it->dataptr);
            indval = *indptr;
            if (indval < 0) {
                indval += dimsize;
            }
            if (indval < 0 || indval >= dimsize) {
                PyErr_Format(PyExc_IndexError,
                             "index (%"INTP_FMT") out of range "\
                             "(0<=index<%"INTP_FMT") in dimension %d",
                             indval, (dimsize-1), mit->iteraxes[i]);
                goto fail;
            }
            PyArray_ITER_NEXT(it);
        }
        PyArray_ITER_RESET(it);
    }
    return;

 fail:
    Py_XDECREF(mit->subspace);
    Py_XDECREF(mit->ait);
    mit->subspace = NULL;
    mit->ait = NULL;
    return;
}


NPY_NO_EXPORT PyObject *
PyArray_MapIterNew(PyObject *indexobj, int oned, int fancy)
{
    PyArrayMapIterObject *mit;
    PyArray_Descr *indtype;
    PyArrayObject *arr = NULL;
    int i, n, started, nonindex;

    if (fancy == SOBJ_BADARRAY) {
        PyErr_SetString(PyExc_IndexError,
                        "arrays used as indices must be of "
                        "integer (or boolean) type");
        return NULL;
    }
    if (fancy == SOBJ_TOOMANY) {
        PyErr_SetString(PyExc_IndexError, "too many indices");
        return NULL;
    }

    mit = (PyArrayMapIterObject *)PyArray_malloc(sizeof(PyArrayMapIterObject));
    PyObject_Init((PyObject *)mit, &PyArrayMapIter_Type);
    if (mit == NULL) {
        return NULL;
    }
    for (i = 0; i < NPY_MAXDIMS; i++) {
        mit->iters[i] = NULL;
    }
    mit->index = 0;
    mit->ait = NULL;
    mit->subspace = NULL;
    mit->numiter = 0;
    mit->consec = 1;
    Py_INCREF(indexobj);
    mit->indexobj = indexobj;

    if (fancy == SOBJ_LISTTUP) {
        PyObject *newobj;
        newobj = PySequence_Tuple(indexobj);
        if (newobj == NULL) {
            goto fail;
        }
        Py_DECREF(indexobj);
        indexobj = newobj;
        mit->indexobj = indexobj;
    }

#undef SOBJ_NOTFANCY
#undef SOBJ_ISFANCY
#undef SOBJ_BADARRAY
#undef SOBJ_TOOMANY
#undef SOBJ_LISTTUP

    if (oned) {
        return (PyObject *)mit;
    }
    /*
     * Must have some kind of fancy indexing if we are here
     * indexobj is either a list, an arrayobject, or a tuple
     * (with at least 1 list or arrayobject or Bool object)
     */

    /* convert all inputs to iterators */
    if (PyArray_Check(indexobj) &&
                    (PyArray_TYPE((PyArrayObject *)indexobj) == NPY_BOOL)) {
        mit->numiter = _nonzero_indices(indexobj, mit->iters);
        if (mit->numiter < 0) {
            goto fail;
        }
        mit->nd = 1;
        mit->dimensions[0] = mit->iters[0]->dims_m1[0]+1;
        Py_DECREF(mit->indexobj);
        mit->indexobj = PyTuple_New(mit->numiter);
        if (mit->indexobj == NULL) {
            goto fail;
        }
        for (i = 0; i < mit->numiter; i++) {
            PyTuple_SET_ITEM(mit->indexobj, i, PyInt_FromLong(0));
        }
    }
    else if (PyArray_Check(indexobj) || !PyTuple_Check(indexobj)) {
        mit->numiter = 1;
        indtype = PyArray_DescrFromType(NPY_INTP);
        arr = (PyArrayObject *)PyArray_FromAny(indexobj, indtype, 0, 0,
                                NPY_ARRAY_FORCECAST, NULL);
        if (arr == NULL) {
            goto fail;
        }
        mit->iters[0] = (PyArrayIterObject *)PyArray_IterNew((PyObject *)arr);
        if (mit->iters[0] == NULL) {
            Py_DECREF(arr);
            goto fail;
        }
        mit->nd = PyArray_NDIM(arr);
        memcpy(mit->dimensions, PyArray_DIMS(arr), mit->nd*sizeof(npy_intp));
        mit->size = PyArray_SIZE(arr);
        Py_DECREF(arr);
        Py_DECREF(mit->indexobj);
        mit->indexobj = Py_BuildValue("(N)", PyInt_FromLong(0));
    }
    else {
        /* must be a tuple */
        PyObject *obj;
        PyArrayIterObject **iterp;
        PyObject *new;
        int numiters, j, n2;
        /*
         * Make a copy of the tuple -- we will be replacing
         * index objects with 0's
         */
        n = PyTuple_GET_SIZE(indexobj);
        n2 = n;
        new = PyTuple_New(n2);
        if (new == NULL) {
            goto fail;
        }
        started = 0;
        nonindex = 0;
        j = 0;
        for (i = 0; i < n; i++) {
            obj = PyTuple_GET_ITEM(indexobj,i);
            iterp = mit->iters + mit->numiter;
            if ((numiters=_convert_obj(obj, iterp)) < 0) {
                Py_DECREF(new);
                goto fail;
            }
            if (numiters > 0) {
                started = 1;
                if (nonindex) {
                    mit->consec = 0;
                }
                mit->numiter += numiters;
                if (numiters == 1) {
                    PyTuple_SET_ITEM(new,j++, PyInt_FromLong(0));
                }
                else {
                    /*
                     * we need to grow the new indexing object and fill
                     * it with 0s for each of the iterators produced
                     */
                    int k;
                    n2 += numiters - 1;
                    if (_PyTuple_Resize(&new, n2) < 0) {
                        goto fail;
                    }
                    for (k = 0; k < numiters; k++) {
                        PyTuple_SET_ITEM(new, j++, PyInt_FromLong(0));
                    }
                }
            }
            else {
                if (started) {
                    nonindex = 1;
                }
                Py_INCREF(obj);
                PyTuple_SET_ITEM(new,j++,obj);
            }
        }
        Py_DECREF(mit->indexobj);
        mit->indexobj = new;
        /*
         * Store the number of iterators actually converted
         * These will be mapped to actual axes at bind time
         */
        if (PyArray_Broadcast((PyArrayMultiIterObject *)mit) < 0) {
            goto fail;
        }
    }

    return (PyObject *)mit;

 fail:
    Py_DECREF(mit);
    return NULL;
}


static void
arraymapiter_dealloc(PyArrayMapIterObject *mit)
{
    int i;
    Py_XDECREF(mit->indexobj);
    Py_XDECREF(mit->ait);
    Py_XDECREF(mit->subspace);
    for (i = 0; i < mit->numiter; i++) {
        Py_XDECREF(mit->iters[i]);
    }
    PyArray_free(mit);
}

/*
 * The mapiter object must be created new each time.  It does not work
 * to bind to a new array, and continue.
 *
 * This was the orginal intention, but currently that does not work.
 * Do not expose the MapIter_Type to Python.
 *
 * It's not very useful anyway, since mapiter(indexobj); mapiter.bind(a);
 * mapiter is equivalent to a[indexobj].flat but the latter gets to use
 * slice syntax.
 */
NPY_NO_EXPORT PyTypeObject PyArrayMapIter_Type = {
#if defined(NPY_PY3K)
    PyVarObject_HEAD_INIT(NULL, 0)
#else
    PyObject_HEAD_INIT(NULL)
    0,                                          /* ob_size */
#endif
    "numpy.mapiter",                            /* tp_name */
    sizeof(PyArrayIterObject),                  /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)arraymapiter_dealloc,           /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
#if defined(NPY_PY3K)
    0,                                          /* tp_reserved */
#else
    0,                                          /* tp_compare */
#endif
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    0,                                          /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,                         /* tp_flags */
    0,                                          /* tp_doc */
    0,                                          /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    0,                                          /* tp_iter */
    0,                                          /* tp_iternext */
    0,                                          /* tp_methods */
    0,                                          /* tp_members */
    0,                                          /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    0,                                          /* tp_init */
    0,                                          /* tp_alloc */
    0,                                          /* tp_new */
    0,                                          /* tp_free */
    0,                                          /* tp_is_gc */
    0,                                          /* tp_bases */
    0,                                          /* tp_mro */
    0,                                          /* tp_cache */
    0,                                          /* tp_subclasses */
    0,                                          /* tp_weaklist */
    0,                                          /* tp_del */
#if PY_VERSION_HEX >= 0x02060000
    0,                                          /* tp_version_tag */
#endif
};

/** END of Subscript Iterator **/
