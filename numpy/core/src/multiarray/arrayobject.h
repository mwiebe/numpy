#ifndef _NPY_INTERNAL_ARRAYOBJECT_H_
#define _NPY_INTERNAL_ARRAYOBJECT_H_

#ifndef _MULTIARRAYMODULE
#error You should not include this
#endif

#ifdef __cplusplus
extern "C" {
#endif

NPY_NO_EXPORT PyObject *
_strings_richcompare(PyArrayObject *self, PyArrayObject *other, int cmp_op,
                     int rstrip);

NPY_NO_EXPORT PyObject *
array_richcompare(PyArrayObject *self, PyObject *other, int cmp_op);

#ifdef __cplusplus
}
#endif

#endif
