/*
 * This file implements the missing value NA singleton object for NumPy.
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
#include <numpy/arrayobject.h>
#include <numpy/arrayscalars.h>

#include "npy_config.h"
#include "numpy/npy_3kcompat.h"

#include "descriptor.h"
#include "common.h"
#include "na_singleton.h"

static PyObject *
na_new(PyTypeObject *subtype, PyObject *args, PyObject *kwds)
{
    NpyNA_fieldaccess *self;

    self = (NpyNA_fieldaccess *)subtype->tp_alloc(subtype, 0);
    if (self != NULL) {
        /* 255 signals no payload */
        self->payload = 255;
        self->dtype = NULL;
        self->is_singleton = 0;
    }

    return (PyObject *)self;
}

static int
na_init(NpyNA_fieldaccess *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"payload", "dtype", NULL};
    int payload = NPY_MAX_INT;
    PyArray_Descr *dtype = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|iO&:NA", kwlist,
                        &payload,
                        &PyArray_DescrConverter, &dtype)) {
        Py_XDECREF(dtype);
        return -1;
    }

    /* Use 255 as the signal that no payload is set */
    if (payload == NPY_MAX_INT) {
        self->payload = 255;
    }
    else if (payload < 0 || payload > 127) {
        PyErr_Format(PyExc_ValueError,
                    "out of bounds payload for NumPy NA, "
                    "%d is not in the range [0,127]", payload);
        Py_XDECREF(dtype);
        return -1;
    }
    else {
        self->payload = (npy_uint8)payload;
    }

    Py_XDECREF(self->dtype);
    self->dtype = dtype;

    return 0;
}

/*
 * The call function proxies to the na_init function to handle
 * the payload and dtype parameters.
 */
static PyObject *
na_call(PyObject *NPY_UNUSED(self), PyObject *args, PyObject *kwds)
{
    NpyNA_fieldaccess *ret;
    
    ret = (NpyNA_fieldaccess *)na_new(&NpyNA_Type, NULL, NULL);
    if (ret != NULL) {
        if (na_init(ret, args, kwds) < 0) {
            Py_DECREF(ret);
            return NULL;
        }
    }

    return (PyObject *)ret;
}

static void
na_dealloc(NpyNA_fieldaccess *self)
{
    Py_XDECREF(self->dtype);
    self->dtype = NULL;
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
na_repr(NpyNA_fieldaccess *self)
{
    if (self->dtype == NULL) {
        if (self->payload == 255) {
            return PyUString_FromString("NA");
        }
        else {
            return PyUString_FromFormat("NA(%d)", (int)self->payload);
        }
    }
    else {
        PyObject *s;
        if (self->payload == 255) {
            s = PyUString_FromString("NA(dtype=");
        }
        else {
            s  = PyUString_FromFormat("NA(%d, dtype=", (int)self->payload);
        }
        PyUString_ConcatAndDel(&s,
                arraydescr_short_construction_repr(self->dtype));
        PyUString_ConcatAndDel(&s,
                PyUString_FromString(")"));
        return s;
    }
}

/*
 * The str function is the same as repr, except it throws away
 * the dtype. It is always either "NA" or "NA(payload)".
 */
static PyObject *
na_str(NpyNA_fieldaccess *self)
{
    if (self->payload == 255) {
        return PyUString_FromString("NA");
    }
    else {
        return PyUString_FromFormat("NA(%d)", (int)self->payload);
    }
}

/*
 * Any comparison with NA produces an NA.
 */
static PyObject *
na_richcompare(NpyNA_fieldaccess *self, PyObject *other, int cmp_op)
{
    /* If an ndarray is compared directly with NA, let the array handle it */
    if (PyArray_Check(other)) {
        Py_INCREF(Py_NotImplemented);
        return Py_NotImplemented;
    }
    /* Otherwise always return the NA singleton */
    else {
        Py_INCREF(Npy_NA);
        return Npy_NA;
    }
}

static PyObject *
na_payload_get(NpyNA_fieldaccess *self)
{
    /* If no payload is set, the value stored is 255 */
    if (self->payload == 255) {
        Py_INCREF(Py_None);
        return Py_None;
    }
    else {
        return PyInt_FromLong(self->payload);
    }
}

static int
na_payload_set(NpyNA_fieldaccess *self, PyObject *value)
{
    long payload;

    /* Don't allow changing the static singleton instance */
    if (self->is_singleton) {
        PyErr_SetString(PyExc_RuntimeError,
                    "cannot change the payload of the NumPy NA singleton, "
                    "make a new copy like 'numpy.NA(payload)'");
        return -1;
    }
    /* Deleting the payload sets it to 255, the signal for no payload */
    else if (value == NULL || value == Py_None) {
        self->payload = 255;
    }
    else {
        /* Use PyNumber_Index to ensure an integer in Python >= 2.5*/
#if PY_VERSION_HEX >= 0x02050000
        value = PyNumber_Index(value);
        if (value == NULL) {
            return -1;
        }
#else
        Py_INCREF(value);
#endif
        payload = PyInt_AsLong(value);
        Py_DECREF(value);
        if (payload == -1 && PyErr_Occurred()) {
            return -1;
        }
        else if (payload < 0 || payload > 127) {
            PyErr_Format(PyExc_ValueError,
                        "out of bounds payload for NumPy NA, "
                        "%ld is not in the range [0,127]", payload);
            return -1;
        }
        else {
            self->payload = (npy_uint8)payload;
        }
    }

    return 0;
}

static PyObject *
na_dtype_get(NpyNA_fieldaccess *self)
{
    if (self->dtype == NULL) {
        Py_INCREF(Py_None);
        return Py_None;
    }
    else {
        Py_INCREF(self->dtype);
        return (PyObject *)self->dtype;
    }
}

static int
na_dtype_set(NpyNA_fieldaccess *self, PyObject *value)
{
    PyArray_Descr *dtype = NULL;

    /* Don't allow changing the static singleton instance */
    if (self->is_singleton) {
        PyErr_SetString(PyExc_RuntimeError,
                    "cannot change the dtype of the NumPy NA singleton, "
                    "make a new copy like 'numpy.NA(dtype=val)'");
        return -1;
    }
    /* Convert the input into a dtype object */
    else if (!PyArray_DescrConverter(value, &dtype)) {
        return -1;
    }

    /* Replace the existing dtype in self */
    Py_XDECREF(self->dtype);
    self->dtype = dtype;

    return 0;
}

static PyGetSetDef na_getsets[] = {
    {"payload",
        (getter)na_payload_get,
        (setter)na_payload_set,
        NULL, NULL},
    {"dtype",
        (getter)na_dtype_get,
        (setter)na_dtype_set,
        NULL, NULL},

    {NULL, NULL, NULL, NULL, NULL}
};

/* Combines two NA values together, merging their payloads and dtypes. */
NPY_NO_EXPORT NpyNA *
NpyNA_CombineNA(NpyNA *na1, NpyNA *na2)
{
    NpyNA_fieldaccess *ret, *fna1, *fna2;

    fna1 = (NpyNA_fieldaccess *)na1;
    fna2 = (NpyNA_fieldaccess *)na2;

    ret = (NpyNA_fieldaccess *)na_new(&NpyNA_Type, NULL, NULL);
    if (ret == NULL) {
        return NULL;
    }

    /* Combine the payloads */
    ret->payload = NpyNA_CombinePayloads(fna1->payload, fna2->payload);

    /* Combine the dtypes */
    Py_XDECREF(ret->dtype);
    ret->dtype = NULL;
    if (fna1->dtype != NULL && fna2->dtype != NULL) {
        ret->dtype = PyArray_PromoteTypes(fna1->dtype, fna2->dtype);
        if (ret->dtype == NULL) {
            Py_DECREF(ret);
            return NULL;
        }
    }
    else if (fna1->dtype != NULL) {
        ret->dtype = fna1->dtype;
        Py_INCREF(ret->dtype);
    }
    else if (fna2->dtype != NULL) {
        ret->dtype = fna2->dtype;
        Py_INCREF(ret->dtype);
    }

    return (NpyNA *)ret;
}

/*
 * Combines an NA with an object, raising an error if the object has
 * no extractable NumPy dtype.
 */
NPY_NO_EXPORT NpyNA *
NpyNA_CombineNAWithObject(NpyNA *na, PyObject *obj)
{
    NpyNA_fieldaccess *ret, *fna;
    PyArray_Descr *dtype = NULL;

    fna = (NpyNA_fieldaccess *)na;

    /* If 'obj' is NA, handle it specially */
    if (NpyNA_Check(obj)) {
        return NpyNA_CombineNA(na, (NpyNA *)obj);
    }

    /* Extract a dtype from 'obj' */
    if (PyArray_IsScalar(obj, Generic)) {
        dtype = PyArray_DescrFromScalar(obj);
        if (dtype == NULL) {
            return NULL;
        }
    }
    else if (PyArray_Check(obj)) {
        /* TODO: This needs to be more complicated... */
        dtype = PyArray_DESCR((PyArrayObject *)obj);
        Py_INCREF(dtype);
    }
    else {
        dtype = _array_find_python_scalar_type(obj);
        if (dtype == NULL) {
            PyErr_SetString(PyExc_TypeError,
                    "numpy.NA only supports operations with scalars "
                    "and NumPy arrays");
            return NULL;
        }
    }

    ret = (NpyNA_fieldaccess *)na_new(&NpyNA_Type, NULL, NULL);
    if (ret == NULL) {
        return NULL;
    }

    /* Copy the payload */
    ret->payload = fna->payload;

    /* Combine the dtypes */
    Py_XDECREF(ret->dtype);
    if (fna->dtype == NULL) {
        ret->dtype = dtype;
    }
    else {
        ret->dtype = PyArray_PromoteTypes(fna->dtype, dtype);
        Py_DECREF(dtype);
        if (ret->dtype == NULL) {
            Py_DECREF(ret);
            return NULL;
        }
    }

    return (NpyNA *)ret;
}

/* An NA unary op simply passes along the same NA */
static PyObject *
na_unaryop(PyObject *self)
{
    Py_INCREF(self);
    return self;
}

static PyObject *
na_binaryop(PyObject *op1, PyObject *op2)
{
    /* If an ndarray is operated on with NA, let the array handle it */
    if (PyArray_Check(op1) || PyArray_Check(op2)) {
        Py_INCREF(Py_NotImplemented);
        return Py_NotImplemented;
    }
    /* Combine NAs according to standard rules */
    else {
        if (NpyNA_Check(op1)) {
            return (PyObject *)NpyNA_CombineNAWithObject((NpyNA *)op1, op2);
        }
        else if (NpyNA_Check(op2)) {
            return (PyObject *)NpyNA_CombineNAWithObject((NpyNA *)op2, op1);
        }
        else {
            Py_INCREF(Py_NotImplemented);
            return Py_NotImplemented;
        }
    }
}

static PyObject *
na_power(PyObject *op1, PyObject *op2, PyObject *NPY_UNUSED(op3))
{
    return na_binaryop(op1, op2);
}

/* Special case bitwise <and> with a boolean 'other' */
static PyObject *
na_and(PyObject *op1, PyObject *op2)
{
    NpyNA *na;
    PyObject *other;

    if (NpyNA_Check(op1)) {
        na = (NpyNA *)op1;
        other = op2;
    }
    else if (NpyNA_Check(op2)) {
        na = (NpyNA *)op2;
        other = op1;
    }
    else {
        Py_INCREF(Py_NotImplemented);
        return Py_NotImplemented;
    }

    /* If an ndarray is operated on with NA, let the array handle it */
    if (PyArray_Check(other)) {
        Py_INCREF(Py_NotImplemented);
        return Py_NotImplemented;
    }
    /* NA & False is False */
    else if (other == Py_False ||
                        ((Py_TYPE(other) == &PyBoolArrType_Type) &&
                         ((PyBoolScalarObject *)other)->obval == 0)) {
        Py_INCREF(Py_False);
        return Py_False;
    }
    /* Combine NAs according to standard rules */
    else {
        return (PyObject *)NpyNA_CombineNAWithObject(na, other);
    }
}

/* Special case bitwise <or> with a boolean 'other' */
static PyObject *
na_or(PyObject *op1, PyObject *op2)
{
    NpyNA *na;
    PyObject *other;

    if (NpyNA_Check(op1)) {
        na = (NpyNA *)op1;
        other = op2;
    }
    else if (NpyNA_Check(op2)) {
        na = (NpyNA *)op2;
        other = op1;
    }
    else {
        Py_INCREF(Py_NotImplemented);
        return Py_NotImplemented;
    }

    /* If an ndarray is operated on with NA, let the array handle it */
    if (PyArray_Check(other)) {
        Py_INCREF(Py_NotImplemented);
        return Py_NotImplemented;
    }
    /* NA & True is True */
    else if (other == Py_True ||
                        ((Py_TYPE(other) == &PyBoolArrType_Type) &&
                         ((PyBoolScalarObject *)other)->obval != 0)) {
        Py_INCREF(Py_True);
        return Py_True;
    }
    /* Combine NAs according to standard rules */
    else {
        return (PyObject *)NpyNA_CombineNAWithObject(na, other);
    }
}

/* Using NA in an if statement is always an error */
static int
na_nonzero(PyObject *NPY_UNUSED(self))
{
    PyErr_SetString(PyExc_ValueError,
            "numpy.NA represents an unknown missing value, "
            "so its truth value cannot be determined");
    return -1;
}

NPY_NO_EXPORT PyNumberMethods na_as_number = {
    (binaryfunc)na_binaryop,                    /*nb_add*/
    (binaryfunc)na_binaryop,                    /*nb_subtract*/
    (binaryfunc)na_binaryop,                    /*nb_multiply*/
#if defined(NPY_PY3K)
#else
    (binaryfunc)na_binaryop,                    /*nb_divide*/
#endif
    (binaryfunc)na_binaryop,                    /*nb_remainder*/
    (binaryfunc)na_binaryop,                    /*nb_divmod*/
    (ternaryfunc)na_power,                      /*nb_power*/
    (unaryfunc)na_unaryop,                      /*nb_neg*/
    (unaryfunc)na_unaryop,                      /*nb_pos*/
    (unaryfunc)na_unaryop,                      /*nb_abs,*/
    (inquiry)na_nonzero,                        /*nb_nonzero*/
    (unaryfunc)na_unaryop,                      /*nb_invert*/
    (binaryfunc)na_binaryop,                    /*nb_lshift*/
    (binaryfunc)na_binaryop,                    /*nb_rshift*/
    (binaryfunc)na_and,                         /*nb_and*/
    (binaryfunc)na_binaryop,                    /*nb_xor*/
    (binaryfunc)na_or,                          /*nb_or*/
#if defined(NPY_PY3K)
#else
    0,                                          /*nb_coerce*/
#endif
    0,                                          /*nb_int*/
#if defined(NPY_PY3K)
    0,                                          /*nb_reserved*/
#else
    0,                                          /*nb_long*/
#endif
    0,                                          /*nb_float*/
#if defined(NPY_PY3K)
#else
    0,                                          /*nb_oct*/
    0,                                          /*nb_hex*/
#endif
    0,                                          /*inplace_add*/
    0,                                          /*inplace_subtract*/
    0,                                          /*inplace_multiply*/
#if defined(NPY_PY3K)
#else
    0,                                          /*inplace_divide*/
#endif
    0,                                          /*inplace_remainder*/
    0,                                          /*inplace_power*/
    0,                                          /*inplace_lshift*/
    0,                                          /*inplace_rshift*/
    0,                                          /*inplace_and*/
    0,                                          /*inplace_xor*/
    0,                                          /*inplace_or*/
    (binaryfunc)na_binaryop,                    /*nb_floor_divide*/
    (binaryfunc)na_binaryop,                    /*nb_true_divide*/
    0,                                          /*nb_inplace_floor_divide*/
    0,                                          /*nb_inplace_true_divide*/
#if PY_VERSION_HEX >= 0x02050000
    0,                                          /*nb_index*/
#endif
};

NPY_NO_EXPORT PyTypeObject NpyNA_Type = {
#if defined(NPY_PY3K)
    PyVarObject_HEAD_INIT(NULL, 0)
#else
    PyObject_HEAD_INIT(NULL)
    0,                                          /* ob_size */
#endif
    "numpy.NAType",                             /* tp_name */
    sizeof(NpyNA_fieldaccess),                  /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)na_dealloc,                     /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
#if defined(NPY_PY3K)
    0,                                          /* tp_reserved */
#else
    0,                                          /* tp_compare */
#endif
    (reprfunc)na_repr,                          /* tp_repr */
    &na_as_number,                              /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    (ternaryfunc)na_call,                       /* tp_call */
    (reprfunc)na_str,                           /* tp_str */
    0,                                          /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT|Py_TPFLAGS_CHECKTYPES,   /* tp_flags */
    0,                                          /* tp_doc */
    0,                                          /* tp_traverse */
    0,                                          /* tp_clear */
    (richcmpfunc)na_richcompare,                /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    0,                                          /* tp_iter */
    0,                                          /* tp_iternext */
    0,                                          /* tp_methods */
    0,                                          /* tp_members */
    na_getsets,                                 /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    (initproc)na_init,                          /* tp_init */
    0,                                          /* tp_alloc */
    na_new,                                     /* tp_new */
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

NPY_NO_EXPORT NpyNA_fieldaccess _Npy_NASingleton = {
    PyObject_HEAD_INIT(&NpyNA_Type)
    255,  /* payload (255 means no payload) */
    NULL, /* dtype */
    1     /* is_singleton */
};

