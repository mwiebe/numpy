#ifndef _NPY_HASHDESCR_H_
#define _NPY_HASHDESCR_H_

#ifdef __cplusplus
extern "C" {
#endif

NPY_NO_EXPORT long
PyArray_DescrHash(PyObject* odescr);

#ifdef __cplusplus
}
#endif

#endif
