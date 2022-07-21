#include <exception>
#include <string>
#include <iostream>

#include "conversion.h"

//#define NUMPY_IMPORT_ARRAY_RETVAL NULL

using namespace std;
/*
 * The following conversion functions are taken/adapted from OpenCV's cv2.cpp file
 * inside modules/python/src2 folder.
 */

#if PY_MAJOR_VERSION >= 3
int init_numpy()
{
import_array();
}
#else
void init_numpy()
{
import_array();
}
#endif

static int failmsg(const char *fmt, ...)
{
    char str[1000];

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(str, sizeof(str), fmt, ap);
    va_end(ap);

    throw runtime_error(string(str));
    //PyErr_SetString(PyExc_TypeError, str);
    return 0;
}

class PyAllowThreads
{
public:
    PyAllowThreads() : _state(PyEval_SaveThread()) {}
    ~PyAllowThreads()
    {
        PyEval_RestoreThread(_state);
    }
private:
    PyThreadState* _state;
};


using namespace cv;

#if CV_MAJOR_VERSION == 3
class NumpyAllocator : public MatAllocator
{
public:
    NumpyAllocator() { stdAllocator = Mat::getStdAllocator(); }
    ~NumpyAllocator() {}

    UMatData* allocate(PyObject* o, int dims, const int* sizes, int type, size_t* step) const
    {
        UMatData* u = new UMatData(this);
        u->data = u->origdata = (uchar*)PyArray_DATA((PyArrayObject*) o);
        npy_intp* _strides = PyArray_STRIDES((PyArrayObject*) o);
        for( int i = 0; i < dims - 1; i++ )
            step[i] = (size_t)_strides[i];
        step[dims-1] = CV_ELEM_SIZE(type);
        u->size = sizes[0]*step[0];
        u->userdata = o;
        return u;
    }

    UMatData* allocate(int dims0, const int* sizes, int type, void* data, size_t* step, int flags, UMatUsageFlags usageFlags) const
    {
        if( data != 0 )
        {
            CV_Error(Error::StsAssert, "The data should normally be NULL!");
            // probably this is safe to do in such extreme case
            return stdAllocator->allocate(dims0, sizes, type, data, step, flags, usageFlags);
        }

        py::gil_scoped_acquire gil;

        int depth = CV_MAT_DEPTH(type);
        int cn = CV_MAT_CN(type);
        const int f = (int)(sizeof(size_t)/8);
        int typenum = depth == CV_8U ? NPY_UBYTE : depth == CV_8S ? NPY_BYTE :
        depth == CV_16U ? NPY_USHORT : depth == CV_16S ? NPY_SHORT :
        depth == CV_32S ? NPY_INT : depth == CV_32F ? NPY_FLOAT :
        depth == CV_64F ? NPY_DOUBLE : f*NPY_ULONGLONG + (f^1)*NPY_UINT;
        int i, dims = dims0;
        cv::AutoBuffer<npy_intp> _sizes(dims + 1);
        for( i = 0; i < dims; i++ )
            _sizes[i] = sizes[i];
        if( cn > 1 )
            _sizes[dims++] = cn;
        PyObject* o = PyArray_SimpleNew(dims, _sizes, typenum);
        if(!o)
            CV_Error_(Error::StsError, ("The numpy array of typenum=%d, ndims=%d can not be created", typenum, dims));
        return allocate(o, dims0, sizes, type, step);
    }

    bool allocate(UMatData* u, int accessFlags, UMatUsageFlags usageFlags) const
    {
        return stdAllocator->allocate(u, accessFlags, usageFlags);
    }

    void deallocate(UMatData* u) const
    {
        if(u)
        {
            py::gil_scoped_acquire gil;
            PyArrayObject* o = (PyArrayObject*)u->userdata;
            Py_XDECREF(o);
            delete u;
        }
    }
 
    const MatAllocator* stdAllocator;
};

NumpyAllocator g_numpyAllocator;

PyObject* NDArrayConverter::toNDArray(Mat const& m) {
    if (!m.data)
        Py_RETURN_NONE;
        Mat temp,
    *p = (Mat*) &m;
    if (!p->u || p->allocator != &g_numpyAllocator) {
        temp.allocator = &g_numpyAllocator;
        ERRWRAP2(m.copyTo(temp));
        p = &temp;
    }
    PyObject* o = (PyObject*) p->u->userdata;
    Py_INCREF(o);
    return o;
}

cv::Mat NDArrayConverter::toMat(PyObject *object)
{
    /*cv::Mat m;

    if(!object || object == Py_None)
    {
        if( !m.data )
            m.allocator = &g_numpyAllocator;
    }*/

    if( !PyArray_Check(object) )
    {
        failmsg("Object is not a numpy array");
    }

    PyArrayObject* oarr = (PyArrayObject*) object;

    bool needcopy = false, needcast = false;
    int typenum = PyArray_TYPE(oarr), new_typenum = typenum;
    int type = typenum == NPY_UBYTE ? CV_8U : typenum == NPY_BYTE ? CV_8S :
                typenum == NPY_USHORT ? CV_16U :
                typenum == NPY_SHORT ? CV_16S :
                typenum == NPY_INT ? CV_32S :
                typenum == NPY_INT32 ? CV_32S :
                typenum == NPY_FLOAT ? CV_32F :
                typenum == NPY_DOUBLE ? CV_64F : -1;

    if (type < 0) {
        needcopy = needcast = true;
        new_typenum = NPY_INT;
        type = CV_32S;
    }

#ifndef CV_MAX_DIM
    const int CV_MAX_DIM = 32;
#endif
    int ndims = PyArray_NDIM(oarr);

    int size[CV_MAX_DIM + 1];
    size_t step[CV_MAX_DIM + 1];
    size_t elemsize = CV_ELEM_SIZE1(type);
    const npy_intp* _sizes = PyArray_DIMS(oarr);
    const npy_intp* _strides = PyArray_STRIDES(oarr);
    bool ismultichannel = ndims == 3 && _sizes[2] <= CV_CN_MAX;

    for (int i = ndims - 1; i >= 0 && !needcopy; i--) {
        // these checks handle cases of
        //  a) multi-dimensional (ndims > 2) arrays, as well as simpler 1- and 2-dimensional cases
        //  b) transposed arrays, where _strides[] elements go in non-descending order
        //  c) flipped arrays, where some of _strides[] elements are negative
        if ((i == ndims - 1 && (size_t) _strides[i] != elemsize)
                || (i < ndims - 1 && _strides[i] < _strides[i + 1]))
            needcopy = true;
    }

    if (ismultichannel && _strides[1] != (npy_intp) elemsize * _sizes[2])
        needcopy = true;

    if (needcopy) {

        if (needcast) {
            object = PyArray_Cast(oarr, new_typenum);
            oarr = (PyArrayObject*) object;
        } else {
            oarr = PyArray_GETCONTIGUOUS(oarr);
            object = (PyObject*) oarr;
        }

        _strides = PyArray_STRIDES(oarr);
    }

    for (int i = 0; i < ndims; i++) {
        size[i] = (int) _sizes[i];
        step[i] = (size_t) _strides[i];
    }

    // handle degenerate case
    if (ndims == 0) {
        size[ndims] = 1;
        step[ndims] = elemsize;
        ndims++;
    }

    if (ismultichannel) {
        ndims--;
        type |= CV_MAKETYPE(0, size[2]);
    }
    if (!needcopy) {
        Py_INCREF(object);
    }

    Mat m = Mat(ndims, size, type, PyArray_DATA(oarr), step);
    m.u = g_numpyAllocator.allocate(object, ndims, size, type, step);
    m.allocator = &g_numpyAllocator;
    m.addref();
   
    return m;
}

#else

class PyEnsureGIL
{
public:
    PyEnsureGIL() : _state(PyGILState_Ensure()) {}
    ~PyEnsureGIL()
    {
        PyGILState_Release(_state);
    }
private:
    PyGILState_STATE _state;
};

class NumpyAllocator : public MatAllocator
{
public:
    NumpyAllocator() { stdAllocator = cv::Mat::getStdAllocator(); }
    ~NumpyAllocator() {}

    cv::UMatData* allocate(PyObject* o, int dims, const int* sizes, int type, size_t* step) const
    {
        UMatData* u = new UMatData(this);
        u->data = u->origdata = (uchar*)PyArray_DATA((PyArrayObject*) o);
        npy_intp* _strides = PyArray_STRIDES((PyArrayObject*) o);
        for( int i = 0; i < dims - 1; i++ )
            step[i] = (size_t)_strides[i];
        step[dims-1] = CV_ELEM_SIZE(type);
        u->size = sizes[0]*step[0];
        u->userdata = o;
        return u;
    }
    cv::UMatData* allocate(int dims0, const int* sizes, int type, void* data, size_t* step, cv::AccessFlag flags, cv::UMatUsageFlags usageFlags) const CV_OVERRIDE 
    {
        if( data != 0 )
        {
            // issue #6969: CV_Error(Error::StsAssert, "The data should normally be NULL!");
            // probably this is safe to do in such extreme case
            return stdAllocator->allocate(dims0, sizes, type, data, step, flags, usageFlags);
        }
        PyEnsureGIL gil;

        int depth = CV_MAT_DEPTH(type);
        int cn = CV_MAT_CN(type);
        const int f = (int)(sizeof(size_t)/8);
        int typenum = depth == CV_8U ? NPY_UBYTE : depth == CV_8S ? NPY_BYTE :
        depth == CV_16U ? NPY_USHORT : depth == CV_16S ? NPY_SHORT :
        depth == CV_32S ? NPY_INT : depth == CV_32F ? NPY_FLOAT :
        depth == CV_64F ? NPY_DOUBLE : f*NPY_ULONGLONG + (f^1)*NPY_UINT;
        int i, dims = dims0;
        cv::AutoBuffer<npy_intp> _sizes(dims + 1);
        for( i = 0; i < dims; i++ )
            _sizes[i] = sizes[i];
        if( cn > 1 )
            _sizes[dims++] = cn;
        PyObject* o = PyArray_SimpleNew(dims, _sizes.data(), typenum);
        if(!o)
            CV_Error_(Error::StsError, ("The numpy array of typenum=%d, ndims=%d can not be created", typenum, dims));
        return allocate(o, dims0, sizes, type, step);
    }
    bool allocate(cv::UMatData* u, cv::AccessFlag accessFlags, cv::UMatUsageFlags usageFlags) const CV_OVERRIDE
    {
        return stdAllocator->allocate(u, accessFlags, usageFlags);
    }
    void deallocate(cv::UMatData* u) const CV_OVERRIDE
    {
        if(!u)
            return;
        PyEnsureGIL gil;
        CV_Assert(u->urefcount >= 0);
        CV_Assert(u->refcount >= 0);
        if(u->refcount == 0)
        {
            PyObject* o = (PyObject*)u->userdata;
            Py_XDECREF(o);
            delete u;
        }
    }

    const cv::MatAllocator* stdAllocator;
};

NumpyAllocator g_numpyAllocator;

cv::Mat NDArrayConverter::toMat(PyObject *o)
{
    cv::Mat m;

    if(!o || o == Py_None)
    {
        if( !m.data )
            m.allocator = &g_numpyAllocator;
    }

    if( !PyArray_Check(o) )
    {
        failmsg("Object is not a numpy array");
    }

    int typenum = PyArray_TYPE((PyArrayObject*)o);
    int type = typenum == NPY_UBYTE ? CV_8U : typenum == NPY_BYTE ? CV_8S :
               typenum == NPY_USHORT ? CV_16U : typenum == NPY_SHORT ? CV_16S :
               typenum == NPY_INT || typenum == NPY_LONG ? CV_32S :
               typenum == NPY_FLOAT ? CV_32F :
               typenum == NPY_DOUBLE ? CV_64F : -1;

    if( type < 0 )
    {
        failmsg("Data type = %d is not supported", typenum);
    }

    int ndims = PyArray_NDIM((PyArrayObject*)o);

    if(ndims >= CV_MAX_DIM)
    {
        failmsg("Dimensionality (=%d) is too high", ndims);
    }

    int size[CV_MAX_DIM+1];
    size_t step[CV_MAX_DIM+1], elemsize = CV_ELEM_SIZE1(type);
    const npy_intp* _sizes = PyArray_DIMS((PyArrayObject*)o);
    const npy_intp* _strides = PyArray_STRIDES((PyArrayObject*)o);
    bool transposed = false;

    for(int i = 0; i < ndims; i++)
    {
        size[i] = (int)_sizes[i];
        step[i] = (size_t)_strides[i];
    }

    if( ndims == 0 || step[ndims-1] > elemsize ) {
        size[ndims] = 1;
        step[ndims] = elemsize;
        ndims++;
    }

    if( ndims >= 2 && step[0] < step[1] )
    {
        std::swap(size[0], size[1]);
        std::swap(step[0], step[1]);
        transposed = true;
    }

    if( ndims == 3 && size[2] <= CV_CN_MAX && step[1] == elemsize*size[2] )
    {
        ndims--;
        type |= CV_MAKETYPE(0, size[2]);
    }

    if( ndims > 2)
    {
        failmsg("Object has more than 2 dimensions");
    }

    m = Mat(ndims, size, type, PyArray_DATA((PyArrayObject*)o), step);

    if( m.data )
    {
        m.u->refcount = *refcountFromPyObject((PyObject*)o);
        m.addref(); // protect the original numpy array from deallocation
                    // (since Mat destructor will decrement the reference counter)
    };
    m.allocator = &g_numpyAllocator;

    if( transposed )
    {
        Mat tmp;
        tmp.allocator = &g_numpyAllocator;
        transpose(m, tmp);
        m = tmp;
    }
    return m;
}

PyObject* NDArrayConverter::toNDArray(const cv::Mat& m)
{
    if( !m.data )
        Py_RETURN_NONE;
    Mat temp, *p = (Mat*)&m;
    if(!p->u->refcount || p->allocator != &g_numpyAllocator) {
        temp.allocator = &g_numpyAllocator;
        m.copyTo(temp);
        p = &temp;
    }
    p->addref();
    return pyObjectFromRefcount(&p->u->refcount);

}


#endif

NDArrayConverter::NDArrayConverter() { init(); }

void NDArrayConverter::init()
{
    py::gil_scoped_acquire gil;
    //printf("PY: %d \n", Py_IsInitialized());
    init_numpy();
}

