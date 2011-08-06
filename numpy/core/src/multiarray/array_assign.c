/*
 * This file implements several array assignment routines.
 *
 * Written by Mark Wiebe (mwwiebe@gmail.com)
 * Copyright (c) 2011 by Enthought, Inc.
 *
 * See LICENSE.txt for the license.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#define NPY_NO_DEPRECATED_API
#define _MULTIARRAYMODULE
#include <numpy/ndarraytypes.h>

#include "npy_config.h"
#include "numpy/npy_3kcompat.h"

#include "convert_datatype.h"
#include "methods.h"
#include "lowlevel_strided_loops.h"
#include "array_assign.h"


/*
 * Assigns the scalar value to every element of the destination raw array.
 *
 * Returns 0 on success, -1 on failure.
 */
static int
raw_array_assign_scalar(int ndim, npy_intp *shape,
        PyArray_Descr *dst_dtype, char *dst_data, npy_intp *dst_strides,
        PyArray_Descr *src_dtype, char *src_data)
{
    int idim;
    npy_intp shape_it[NPY_MAXDIMS], dst_strides_it[NPY_MAXDIMS];
    npy_intp coord[NPY_MAXDIMS];
    NPY_BEGIN_THREADS_DEF;

    PyArray_StridedTransferFn *stransfer = NULL;
    NpyAuxData *transferdata = NULL;
    int aligned = 1, needs_api = 0;
    npy_intp src_itemsize = src_dtype->elsize;

    /* Check alignment */
    if (dst_dtype->alignment > 1) {
        npy_intp align_check = (npy_intp)dst_data;
        for (idim = 0; idim < ndim; ++idim) {
            align_check |= dst_strides[idim];
        }
        if ((align_check & (dst_dtype->alignment - 1)) != 0) {
            aligned = 0;
        }
    }
    if (((npy_intp)src_data & (src_dtype->alignment - 1)) != 0) {
        aligned = 0;
    }

    /* Use raw iteration with no heap allocation */
    if (PyArray_PrepareOneRawArrayIter(
                    ndim, shape,
                    dst_data, dst_strides,
                    &ndim, shape_it,
                    &dst_data, dst_strides_it) < 0) {
        return -1;
    }

    /* Get the function to do the casting */
    if (PyArray_GetDTypeTransferFunction(aligned,
                        0, dst_strides_it[0],
                        src_dtype, dst_dtype,
                        0,
                        &stransfer, &transferdata,
                        &needs_api) != NPY_SUCCEED) {
        return -1;
    }

    if (!needs_api) {
        NPY_BEGIN_THREADS;
    }

    NPY_RAW_ITER_START(idim, ndim, coord, shape_it) {
        /* Process the innermost dimension */
        stransfer(dst_data, dst_strides_it[0], src_data, 0,
                    shape_it[0], src_itemsize, transferdata);
    } NPY_RAW_ITER_ONE_NEXT(idim, ndim, coord,
                            shape_it, dst_data, dst_strides_it);

    if (!needs_api) {
        NPY_END_THREADS;
    }

    NPY_AUXDATA_FREE(transferdata);

    return (needs_api && PyErr_Occurred()) ? -1 : 0;
}

/* See array_assign.h for documentation */
NPY_NO_EXPORT int
array_assign_scalar(PyArrayObject *dst,
                    PyArray_Descr *src_dtype, char *src_data,
                    PyArrayObject *wheremask,
                    NPY_CASTING casting, npy_bool overwritena)
{
    int copied_src_data = 0, dst_has_maskna = PyArray_HASMASKNA(dst);

    /* Check the casting rule */
    if (!can_cast_scalar_to(src_dtype, src_data,
                            PyArray_DESCR(dst), casting)) {
        PyObject *errmsg;
        errmsg = PyUString_FromString("Cannot cast scalar from ");
        PyUString_ConcatAndDel(&errmsg,
                PyObject_Repr((PyObject *)src_dtype));
        PyUString_ConcatAndDel(&errmsg,
                PyUString_FromString(" to "));
        PyUString_ConcatAndDel(&errmsg,
                PyObject_Repr((PyObject *)PyArray_DESCR(dst)));
        PyUString_ConcatAndDel(&errmsg,
                PyUString_FromFormat(" according to the rule %s",
                        npy_casting_to_string(casting)));
        PyErr_SetObject(PyExc_TypeError, errmsg);
        return -1;
    }

    /*
     * Make a copy of the src data if it's a different dtype than 'dst'
     * or isn't aligned, and the destination we're copying to has
     * more than one element.
     */
    if ((!PyArray_EquivTypes(PyArray_DESCR(dst), src_dtype) ||
                ((npy_intp)src_data & (src_dtype->alignment - 1)) != 0) &&
                    PyArray_SIZE(dst) > 1) {
        char *tmp_src_data;

        /* Allocate a new buffer to store the copied src data */
        tmp_src_data = PyArray_malloc(PyArray_DESCR(dst)->elsize);
        copied_src_data = 1;
        if (PyArray_CastRawArrays(1, src_data, tmp_src_data, 0, 0,
                            src_dtype, PyArray_DESCR(dst), 0) != NPY_SUCCEED) {
            goto fail;
        }

        /* Replace src_data/src_dtype */
        src_data = tmp_src_data;
        src_dtype = PyArray_DESCR(dst);
    }

    if (wheremask == NULL) {
        /* This is the case of a straightforward value assignment */
        if (overwritena || !dst_has_maskna) {
            /* If we're assigning to an array with a mask, set to all exposed */
            if (dst_has_maskna) {
                if (PyArray_AssignMaskNA(dst, 1) < 0) {
                    goto fail;
                }
            }

            /* TODO: continuing here */
            if (raw_array_assign_scalar(PyArray_NDIM(dst), PyArray_DIMS(dst),
                    PyArray_DESCR(dst), PyArray_DATA(dst), PyArray_STRIDES(dst),
                    src_dtype, src_data) < 0) {
                goto fail;
            }
        }
        /* This is value assignment without overwriting NA values */
        else {
        }
    }
    else {
        /* This is the case of a straightforward masked assignment */
        if (overwritena || !dst_has_maskna) {
        }
        /* This is masked value assignment without overwriting NA values */
        else {
        }
    }

    if (copied_src_data) {
        PyArray_free(src_data);
    }

    return 0;

fail:
    if (copied_src_data) {
        PyArray_free(src_data);
    }

    return -1;
}

