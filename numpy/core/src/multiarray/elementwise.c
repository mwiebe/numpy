/*
 * This file implements generic methods for computing element-wise operations
 * on arrays.
 *
 * Written by Mark Wiebe (mwwiebe@gmail.com)
 * Copyright (c) 2011 by Enthought, Inc.
 *
 * See LICENSE.txt for the license.
 */

/*
 * This function executes all the standard NumPy element-wise binary
 * operation boilerplate code, just calling the appropriate loop 
 * function where necessary.
 *
 * a         : The first operand.
 * b         : The second operand.
 * out       : NULL, or the array into which to place the result.
 * a_dtype   : The data type 'a' should appear as for the inner loop.
 * b_dtype   : The data type 'b' should appear as for the inner loop.
 * result_dtype : The data type the result should appear as for the inner loop.
 * in_casting : The casting rule to use for the inputs.
 * out_casting : The casting rule to use for the output.
 * order     : The order the output should be (recommend NPY_KEEPORDER).
 * wheremask : A where mask indicating which elements should have the
 *             computation done.
 * subok     : If true, the result uses the subclass of operand, otherwise
 *              it is always a base class ndarray.
 * loop      : The function to execute the element-wise loop on regular
 *             aligned data. The 'iter' object this loop gets has three
 *             operands, corresponding to 'a', 'b', and 'result'.
 * wheremask_loop : The function to execute the element-wise loop on
 *             regular aligned data, with a where-mask selecting where
 *             to do the computation. The 'iter' object this loop gets
 *             has four operands, corresponding to 'a', 'b', 'result',
 *             and 'wheremask' The dtype of wheremask is NPY_BOOL.
 * maskna_loop : The function to execute the element-wise loop on
 *             NA-masked aligned data. The 'iter' object this loop gets
 *             has 6 operands, corresponding to 'a', 'b', 'result',
 *             followed by the NA masks in the same order.
 * maskna_wheremask_loop: The function to execute the element-wise loop
 *             on NA-masked aligned data, with a where-mask selection.
 *             The 'iter' object this loop gets has seven operands,
 *             corresponding to 'a', 'b', 'result', 'wheremask',
 *             followed by the NA masks of 'a', 'b', and 'result'.
 * data      : Data which is passed to the loop function.
 * buffersize: Buffer size for the iterator. For the default, pass in 0.
 * funcname  : The name of the element-wise function, for error messages.
 */
NPY_NO_EXPORT PyArrayObject *
PyArray_BinaryElementWiseWrapper(PyArrayObject *a, PyArrayObject *b,
                PyArrayObject *out,
                PyArray_Descr *a_dtype, PyArray_Descr *b_dtype,
                PyArray_Descr *result_dtype,
                NPY_CASTING in_casting, NPY_CASTING out_casting,
                NPY_ORDER order,
                PyArrayObject *wheremask, int subok,
                PyArray_BinaryElementWiseLoopFunc *loop,
                PyArray_BinaryElementWiseLoopFunc *wheremask_loop,
                PyArray_BinaryElementWiseLoopFunc *maskna_loop,
                PyArray_BinaryElementWiseLoopFunc *maskna_wheremask_loop,
                void *data, npy_intp buffersize, const char *funcname)
{
    int use_maskna;

    use_maskna = PyArray_HASMASKNA(a) ||
                 PyArray_HASMASKNA(b);

    /*
     * If the output array doesn't have an NA mask, make sure
     * neither of the inputs contain any NAs.
     */
    if (use_maskna && out != NULL && !PyArray_HASMASKNA(out)) {
        int containsna_a, containsna_b;
        containsna_a = PyArray_ContainsNA(a);
        if (containsna_a == -1) {
            return NULL;
        }
        containsna_b = PyArray_ContainsNA(b);
        if (containsna_b == -1) {
            return NULL;
        }
        if (containsna_a || containsna_b) {
            PyErr_SetString(PyExc_ValueError,
                            "Cannot assign NA value to an array which "
                            "does not support NAs");
                    return -1;
        }
        else {
            use_maskna = 0;
        }
    }
}
