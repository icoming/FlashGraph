import numpy as np
import ctypes
# "cimport" is used to import special compile-time information
# about the numpy module (this is stored in a file numpy.pxd which is
# currently part of the Cython distribution).
cimport numpy as np
from libc.stdlib cimport free, malloc
from libc.stdint cimport intptr_t
from libcpp.vector cimport vector
from libcpp.string cimport string
from libcpp cimport bool
from libc.string cimport memcpy

from cpython cimport array
import array

np.import_array()

cdef enum bulk_op_idx_t:
    OP_ADD, OP_SUB, OP_MUL, OP_DIV
    OP_MIN, OP_MAX,
    OP_POW,
    OP_EQ, OP_NEQ, OP_GT, OP_GE, OP_LT, OP_LE,
    OP_OR, OP_AND,
    OP_MOD, OP_IDIV

cdef enum bulk_uop_idx_t:
    UOP_NEG, UOP_SQRT, UOP_ABS, UOP_NOT, UOP_SQ,
    UOP_CEIL, UOP_FLOOR, UOP_ROUND,
    UOP_LOG, UOP_LOG2, UOP_LOG10

cdef extern from "MatrixWrapper.h" namespace "flashpy":
    cdef cppclass matrix_wrapper:
        matrix_wrapper()
        # create a vector with data from "data_addr".
        matrix_wrapper(intptr_t data_addr, size_t length,
                const string &t)
        # create a matrix with data from "data_addr".
        matrix_wrapper(intptr_t data_addr, size_t nrow, size_t ncol,
                const string &t, const string layout)
        # create an empty vector with the specified size
        matrix_wrapper(size_t length, string t)
        # create an empty matrix with the specified size
        matrix_wrapper(size_t nrow, size_t ncol, string t, string layout)

        void init_seq[T](T start, T stride, bool byrow)
        void init_const_float(double val)
        void init_const_int(long val)

        size_t get_num_rows() const
        size_t get_num_cols() const
        size_t get_entry_size() const
        string get_type_str() const
        np.NPY_TYPES get_type_py() const
        string get_layout() const
        bool is_in_mem() const
        bool is_virtual() const
        bool is_vector() const
        bool materialize_self() const
        matrix_wrapper get_cols(const vector[long] &idxs) const
        matrix_wrapper get_rows(const vector[long] &idxs) const
        matrix_wrapper get_cols(matrix_wrapper idxs) const
        matrix_wrapper get_rows(matrix_wrapper idxs) const
        matrix_wrapper get_cols(size_t start, size_t end) const
        matrix_wrapper get_rows(size_t start, size_t end) const
        matrix_wrapper set_cols(const vector[long] &idxs, matrix_wrapper cols)
        matrix_wrapper set_rows(const vector[long] &idxs, matrix_wrapper rows)
        const char *get_raw_arr() const
        matrix_wrapper transpose() const
        matrix_wrapper conv_store(bool in_mem, int num_nodes) const
        matrix_wrapper inner_prod(matrix_wrapper m, bulk_op_idx_t left_op,
                bulk_op_idx_t right_op) const
        matrix_wrapper multiply(matrix_wrapper m) const
        matrix_wrapper aggregate(bulk_op_idx_t op)
        matrix_wrapper agg_row(bulk_op_idx_t op) const
        matrix_wrapper agg_col(bulk_op_idx_t op) const
        matrix_wrapper groupby_row(matrix_wrapper labels, bulk_op_idx_t op) const
        matrix_wrapper groupby_row(matrix_wrapper labels, bulk_op_idx_t op) const
        matrix_wrapper mapply_cols(matrix_wrapper vals, bulk_op_idx_t op) const
        matrix_wrapper mapply_rows(matrix_wrapper vals, bulk_op_idx_t op) const
        matrix_wrapper mapply2(matrix_wrapper m, bulk_op_idx_t op) const
        matrix_wrapper sapply(bulk_uop_idx_t op) const

class flagsobj:
    def __init__(self):
        self.c_contiguous = False
        self.f_contiguous = False
        self.owndata = True
        self.writable = False
        self.aligned = True
        self.updateifcopy = False

    def set_layout(self, layout):
        if (layout == "C"):
            self.c_contiguous = True
            self.f_contiguous = False
        elif (layout == "F"):
            self.c_contiguous = False
            self.f_contiguous = True
        else:
            raise ValueError("Invalid layout")

cdef class PyMatrix:
    cdef matrix_wrapper mat      # hold a C++ instance which we're wrapping
    cdef readonly int ndim
    cdef readonly object shape
    cdef readonly string dtype
    cdef readonly object flags

    def __cinit__(self):
        self.mat = matrix_wrapper()

    def __init__(self):
        self.ndim = 0
        self.shape = (0, 0)
        self.flags = flagsobj()

    def __array__(self):
        cdef char *src = self.mat.get_raw_arr()
        if (src == NULL):
            return None
        cdef np.npy_intp shape[2]
        shape[0] = self.shape[0]
        shape[1] = self.shape[1]
        return np.PyArray_SimpleNewFromData(self.ndim, shape,
                self.mat.get_type_py(), src)

    # Special Methods Table
    # http://cython.readthedocs.io/en/latest/src/reference/special_methods_table.html

    def __richcmp__(PyMatrix x, PyMatrix y, int op):
        cdef PyMatrix ret = PyMatrix()
        # Rich comparisons:
        # http://cython.readthedocs.io/en/latest/src/userguide/special_methods.html#rich-comparisons
        # <   0
        # ==  2
        # >   4
        # <=  1
        # !=  3
        # >=  5
        if (op == 0):
            ret.mat = x.mat.mapply2(y.mat, OP_LT)
        elif (op == 2):
            ret.mat = x.mat.mapply2(y.mat, OP_EQ)
        elif (op == 4):
            ret.mat = x.mat.mapply2(y.mat, OP_GT)
        elif (op == 1):
            ret.mat = x.mat.mapply2(y.mat, OP_LE)
        elif (op == 3):
            ret.mat = x.mat.mapply2(y.mat, OP_NEQ)
        elif (op == 5):
            ret.mat = x.mat.mapply2(y.mat, OP_GE)
        else:
            print("invalid argument")
        return ret

    def __add__(PyMatrix x, PyMatrix y):
        return x.mapply2(y, OP_ADD)

    def __sub__(PyMatrix x, PyMatrix y):
        return x.mapply2(y, OP_SUB)

    def __mul__(PyMatrix x, PyMatrix y):
        return x.mapply2(y, OP_MUL)

    def __div__(PyMatrix x, PyMatrix y):
        return x.mapply2(y, OP_DIV)

    def __floordiv__(PyMatrix x, PyMatrix y):
        return x.mapply2(y, OP_IDIV)

    def __mod__(PyMatrix x, PyMatrix y):
        return x.mapply2(y, OP_MOD)

    def __and__(PyMatrix x, PyMatrix y):
        return x.mapply2(y, OP_AND)

    def __or__(PyMatrix x, PyMatrix y):
        return x.mapply2(y, OP_OR)

    def __neg__(self):
        return self.sapply(UOP_NEG)

    def __abs__(self):
        return self.sapply(UOP_ABS)

    def __len__(self):
        return self.mat.get_num_rows()

    def init_attr(self):
        self.shape = (self.mat.get_num_rows(), self.mat.get_num_cols())
        if (self.mat.is_vector()):
            self.ndim = 1
        else:
            self.ndim = 2
        self.dtype = self.mat.get_type_str()
        self.flags.set_layout(self.mat.get_layout())

    # These are specific for FlashMatrix.

    def is_in_mem(self):
        return self.mat.is_in_mem()

    def is_virtual(self):
        return self.mat.is_virtual()

    def materialize_self(self):
        return self.mat.materialize_self()

    def get_cols(self, array.array idxs):
        cdef vector[long] cidxs
        cdef long *p = idxs.data.as_longs
        cidxs.assign(p, p + len(idxs))

        cdef PyMatrix ret = PyMatrix()
        ret.mat = self.mat.get_cols(cidxs)
        ret.init_attr()
        return ret

    def transpose(self):
        cdef PyMatrix ret = PyMatrix()
        ret.mat = self.mat.transpose()
        ret.init_attr()
        return ret

    def conv_store(self, bool in_mem, int num_nodes):
        cdef PyMatrix ret = PyMatrix()
        ret.mat = self.mat.conv_store(in_mem, num_nodes)
        ret.init_attr()
        return ret

    # These are generalized functions.

    def inner_prod(self, PyMatrix mat, left_op, right_op):
        cdef PyMatrix ret = PyMatrix()
        ret.mat = self.mat.inner_prod(mat.mat, left_op, right_op)
        ret.init_attr()
        return ret

    def aggregate(self, op):
        cdef PyMatrix ret = PyMatrix()
        ret.mat = self.mat.aggregate(op)
        ret.init_attr()
        return ret

    def agg_row(self, op):
        cdef PyMatrix ret = PyMatrix()
        ret.mat = self.mat.agg_row(op)
        ret.init_attr()
        return ret

    def agg_col(self, op):
        cdef PyMatrix ret = PyMatrix()
        ret.mat = self.mat.agg_col(op)
        ret.init_attr()
        return ret

    def mapply_rows(self, PyMatrix vec, op):
        cdef PyMatrix ret = PyMatrix()
        ret.mat = self.mat.mapply_rows(vec.mat, op)
        ret.init_attr()
        return ret

    def mapply_cols(self, PyMatrix vec, op):
        cdef PyMatrix ret = PyMatrix()
        ret.mat = self.mat.mapply_cols(vec.mat, op)
        ret.init_attr()
        return ret

    def mapply2(self, PyMatrix mat, op):
        cdef PyMatrix ret = PyMatrix()
        ret.mat = self.mat.mapply2(mat.mat, op)
        ret.init_attr()
        return ret

    def sapply(self, op):
        cdef PyMatrix ret = PyMatrix()
        ret.mat = self.mat.sapply(op)
        ret.init_attr()
        return ret

#        matrix_wrapper groupby_row(matrix_wrapper labels, bulk_op_idx_t op) const
#        matrix_wrapper groupby_row(matrix_wrapper labels, bulk_op_idx_t op) const

# TODO this function should have the same interface as numpy.array.
def array(np.ndarray arr, string dtype):
    cdef PyMatrix ret = PyMatrix()
    # TODO this is a bit too hacky. Is there a better way?
    cdef intptr_t addr = ctypes.c_void_p(arr.ctypes.data).value
    if (arr.ndim == 1):
        ret.mat = matrix_wrapper(addr, arr.shape[0], dtype)
    elif (arr.ndim == 2):
        ret.mat = matrix_wrapper(addr, arr.shape[0], arr.shape[1], dtype, "C")
    else:
        raise ValueError("don't support more than 2 dimensions")
    ret.init_attr()
    return ret

def empty_like(a, dtype=None, order='K', subok=True):
    cdef PyMatrix ret = PyMatrix()
    shape = a.shape
    if (dtype is None):
        dtype = a.dtype

    # TODO what is the input array isn't contiguous.
    if (order == 'K' and a.flags.c_contiguous):
        order = 'C'
    elif (order == 'K' and a.flags.f_contiguous):
        order = 'F'

    if (len(shape) == 1):
        ret.mat = matrix_wrapper(shape[0], dtype)
    elif (len(shape) == 2):
        ret.mat = matrix_wrapper(shape[0], shape[1], dtype, order)
    else:
        raise ValueError("don't support more than 2 dimensions")
    ret.init_attr()
    return ret

def empty(shape, dtype='f', order='C'):
    cdef PyMatrix ret = PyMatrix()
    if (len(shape) == 1):
        ret.mat = matrix_wrapper(shape[0], dtype)
    elif (len(shape) == 2):
        ret.mat = matrix_wrapper(shape[0], shape[1], dtype, order)
    else:
        raise ValueError("don't support more than 2 dimensions")
    ret.init_attr()
    return ret

def init_val(PyMatrix data, dtype, val):
    if (dtype == 'f' or dtype == 'd' or dtype == 'g'):
        data.mat.init_const_float(val)
    else:
        data.mat.init_const_int(val)

def ones(shape, dtype='f', order='C'):
    cdef PyMatrix ret = empty(shape, dtype, order)
    init_val(ret, dtype, 1)
    ret.init_attr()
    return ret

def zeros(shape, dtype='f', order='C'):
    cdef PyMatrix ret = empty(shape, dtype, order)
    init_val(ret, dtype, 0)
    ret.init_attr()
    return ret