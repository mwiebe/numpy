:Title: Redefining the NumPy UFunc
:Author: Mark Wiebe <mwwiebe@gmail.com
:Content-Type: text/x-rst
:Created: 22-Jun-2011

*****************
Table of Contents
*****************

.. contents::

********
Abstract
********

This NEP proposes a redefinition of what a NumPy ufunc is, from an
instance of a specific class, to a Python function object which exposes
a particular interface for operating on arrays.

Matrix multiplication, for example, is an operation which consumes
and produces arrays, but is not element-wise, so does not fit within
the ufunc structure. By broadening the definition of a ufunc, we can
include this and others to use a common interface for array operations.

An important aspect of this redefinition is creating a mechanism for
dispatching alternative implementations of any ufunc specific to
array-like objects. An object which behaves like an array but tracks
physical units, for example, will want to override many ufuncs for which
the units should be manipulated for the operation. Being able to override
basic element-wide operations like add and multiply, as well as more
sophisticated operations like solve, are important to producing a seamless
interoperable class.

********************
New UFunc Definition
********************

We will call all of the current ufuncs by a new name, *element-wise ufuncs*.
From this point on, any usage of the undecorated term *ufunc* is about
the new definition.

A *ufunc* is an Python function object which has a particular signature
and exposes several properties and methods. In general, it takes one
or more inputs and produces one or more outputs, all of which are arrays
or array-like objects.

The call signature of a *ufunc* is at minimum as follows, but may
have more::

    ufunc(in1, ..., in<nin>, out=None,
                             casting='same_kind', 
                             order='K',
                             subok=True)

The *ufunc* must define several properties which describe its inputs
and outputs, and allow functions which override it to deduce what
it needs.::

    ufunc.name:
        The name of the ufunc.

    ufunc.nin:
        The number of input arguments.

    ufunc.nout:
        The number of output arguments.

    ufunc.result_type(in1, ..., in<nin>, out=None):
        A function which applies the ufunc's particular type promotion
        rules to produce either a single dtype if ufunc.nout == 1, or
        a tuple of dtypes if ufunc.nout > 1, representing the data types
        the output would have if the ufunc was called with those inputs.

    ufunc.result_shape(in1, ..., in<nin>, out=None):
        A function which applies the ufunc's particular output shape
        determination on the operands, to produce either a single
        shape tuple if ufunc.nout == 1, or a tuple of shape tuples is
        ufunc.nout > 1.

    ufunc.result_array(in1, ..., in<nin>, out=None, order='K', subok=True):
        A function which produces output arrays exactly as the ufunc would.
        The result of this may be passed to the out= parameter of
        the ufunc. This function follows the order= and subok= parameters,
        something that can't be done with just the return values of
        result_shape.

The *ufunc* may accept any additional optional keyword arguments, but when it
accepts the following arguments, the behavior must be as documented
here::

    out:
        If ufunc.nout == 1, this is the output array. If ufunc.nout > 1,
        this should accept a tuple of output arrays. For backwards
        compatibility with the previous ufunc definition, it may as
        another option accept the first output array here, and the other
        output parameters sequentially after it.

    casting:
        The casting rule to apply to the inputs and outputs. One of
        'unsafe', 'same_kind', 'safe', 'equiv', or 'no'.

    order:
        Indicates what the memory layout of the output should be. This
        should default to 'K', trying to preserve the memory layout of
        the inputs.

    dtype:
        When provided, gives the dtype desired for the calculation and
        for the outputs.

    subok:
        If subok is True, does all processing with base class ndarrays,
        avoiding all subclassing and overloading mechanisms.

