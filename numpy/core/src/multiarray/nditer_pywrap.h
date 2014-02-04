#ifndef __NDITER_PYWRAP_H
#define __NDITER_PYWRAP_H

#ifdef __cplusplus
extern "C" {
#endif

NPY_NO_EXPORT PyObject *
NpyIter_NestedIters(PyObject *NPY_UNUSED(self),
                    PyObject *args, PyObject *kwds);

#ifdef __cplusplus
}
#endif

#endif
