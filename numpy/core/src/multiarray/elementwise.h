#ifndef _NPY_PRIVATE__ELEMENTWISE_H_
#define _NPY_PRIVATE__ELEMENTWISE_H_

typedef int (PyArray_BinaryElementWiseLoopFunc)(NpyIter *iter,
                                            char **dataptr,
                                            npy_intp *strideptr,
                                            npy_intp *countptr,
                                            NpyIter_IterNextFunc *iternext,
                                            int needs_api,
                                            void *data);


#endif
