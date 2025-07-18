/******************************************************************************
 *
 * Name:     gdal_array.i
 * Project:  GDAL Python Interface
 * Purpose:  GDAL / Numpy interface
 * Author:   Frank Warmerdam, warmerda@home.com
 *
 ******************************************************************************
 * Copyright (c) 2000, Frank Warmerdam
 *
 * SPDX-License-Identifier: MIT
 *****************************************************************************/

%feature("autodoc");

%module (package="osgeo") gdal_array

%include constraints.i

%import typemaps_python.i

%import MajorObject.i
%import Band.i
%import Dataset.i
%import RasterAttributeTable.i

%include "cplvirtualmem.i"

%init %{
  import_array();
  PyDateTime_IMPORT;
  GDALRegister_NUMPY();
%}

typedef int CPLErr;
typedef int GDALRIOResampleAlg;

%include "python_exceptions.i"
%include "python_strings.i"

%{
#include <vector>
#include "gdal_priv.h"
#include "ogr_recordbatch.h"

#ifdef _DEBUG
#undef _DEBUG
#include "Python.h"
#include "datetime.h"
#define _DEBUG
#else
#include "Python.h"
#include "datetime.h"
#endif
#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION
#include "numpy/arrayobject.h"

#ifdef DEBUG
typedef struct GDALRasterBandHS GDALRasterBandShadow;
typedef struct GDALDatasetHS GDALDatasetShadow;
typedef struct RasterAttributeTableHS GDALRasterAttributeTableShadow;
#else
typedef void GDALRasterBandShadow;
typedef void GDALDatasetShadow;
typedef void GDALRasterAttributeTableShadow;
#endif

// Declaration from memmultidim.h
std::shared_ptr<GDALMDArray> CPL_DLL MEMGroupCreateMDArray(GDALGroup* poGroup,
                                                   const std::string& osName,
                                                   const std::vector<std::shared_ptr<GDALDimension>>& aoDimensions,
                                                   const GDALExtendedDataType& oDataType,
                                                   void* pData,
                                                   CSLConstList papszOptions);


CPL_C_START

GDALRasterBandH CPL_DLL MEMCreateRasterBandEx( GDALDataset *, int, GByte *,
                                               GDALDataType, GSpacing, GSpacing, int );
CPL_C_END

typedef char retStringAndCPLFree;

class NUMPYDataset : public GDALDataset
{
    PyArrayObject *psArray;

    int           bValidGeoTransform;
    double	  adfGeoTransform[6];
    OGRSpatialReference m_oSRS{};

    int           nGCPCount;
    GDAL_GCP      *pasGCPList;
    OGRSpatialReference m_oGCPSRS{};;

  public:
                 NUMPYDataset();
                 ~NUMPYDataset();

    const OGRSpatialReference* GetSpatialRef() const override;
    CPLErr SetSpatialRef(const OGRSpatialReference* poSRS) override;

    virtual CPLErr GetGeoTransform( double * ) override;
    virtual CPLErr SetGeoTransform( double * ) override;

    virtual int    GetGCPCount() override;
    const OGRSpatialReference* GetGCPSpatialRef() const override;
    virtual const GDAL_GCP *GetGCPs() override;
    CPLErr SetGCPs( int nGCPCount, const GDAL_GCP *pasGCPList,
                    const OGRSpatialReference* poSRS ) override;

    static GDALDataset *Open( PyArrayObject *psArray, bool binterleave = true );
    static GDALDataset *Open( GDALOpenInfo * );
};


/************************************************************************/
/*                          GDALRegister_NUMPY()                        */
/************************************************************************/

static void GDALRegister_NUMPY(void)

{
    GDALDriver	*poDriver;
    if (! GDAL_CHECK_VERSION("NUMPY driver"))
        return;
    if( GDALGetDriverByName( "NUMPY" ) == NULL )
    {
        poDriver = static_cast<GDALDriver*>(GDALCreateDriver());

        poDriver->SetDescription( "NUMPY" );
        poDriver->SetMetadataItem( GDAL_DMD_LONGNAME,
                                   "Numeric Python Array" );
        poDriver->SetMetadataItem( GDAL_DCAP_RASTER, "YES" );

        poDriver->pfnOpen = NUMPYDataset::Open;

        GetGDALDriverManager()->RegisterDriver( poDriver );

    }
}

/************************************************************************/
/*                            NUMPYDataset()                            */
/************************************************************************/

NUMPYDataset::NUMPYDataset()

{
    psArray = NULL;
    bValidGeoTransform = FALSE;
    adfGeoTransform[0] = 0.0;
    adfGeoTransform[1] = 1.0;
    adfGeoTransform[2] = 0.0;
    adfGeoTransform[3] = 0.0;
    adfGeoTransform[4] = 0.0;
    adfGeoTransform[5] = 1.0;

    nGCPCount = 0;
    pasGCPList = NULL;
}

/************************************************************************/
/*                            ~NUMPYDataset()                            */
/************************************************************************/

NUMPYDataset::~NUMPYDataset()

{
    if( nGCPCount > 0 )
    {
        GDALDeinitGCPs( nGCPCount, pasGCPList );
        CPLFree( pasGCPList );
    }

    FlushCache(true);

    // Although the module has thread disabled, we go here from GDALClose()
    SWIG_PYTHON_THREAD_BEGIN_BLOCK;

    Py_DECREF( psArray );

    SWIG_PYTHON_THREAD_END_BLOCK;
}

/************************************************************************/
/*                          GetSpatialRef()                             */
/************************************************************************/

const OGRSpatialReference *NUMPYDataset::GetSpatialRef() const

{
    return m_oSRS.IsEmpty() ? nullptr:  &m_oSRS;
}

/************************************************************************/
/*                           SetSpatialRef()                            */
/************************************************************************/

CPLErr NUMPYDataset::SetSpatialRef( const OGRSpatialReference* poSRS )

{
    m_oSRS.Clear();
    if( poSRS )
        m_oSRS = *poSRS;

    return CE_None;
}

/************************************************************************/
/*                          GetGeoTransform()                           */
/************************************************************************/

CPLErr NUMPYDataset::GetGeoTransform( double * padfTransform )

{
    memcpy( padfTransform, adfGeoTransform, sizeof(double)*6 );
    if( bValidGeoTransform )
        return CE_None;
    else
        return CE_Failure;
}

/************************************************************************/
/*                          SetGeoTransform()                           */
/************************************************************************/

CPLErr NUMPYDataset::SetGeoTransform( double * padfTransform )

{
    bValidGeoTransform = TRUE;
    memcpy( adfGeoTransform, padfTransform, sizeof(double)*6 );
    return( CE_None );
}

/************************************************************************/
/*                            GetGCPCount()                             */
/************************************************************************/

int NUMPYDataset::GetGCPCount()

{
    return nGCPCount;
}

/************************************************************************/
/*                          GetGCPSpatialRef()                          */
/************************************************************************/

const OGRSpatialReference *NUMPYDataset::GetGCPSpatialRef() const

{
    return m_oGCPSRS.IsEmpty() ? nullptr:  &m_oGCPSRS;
}

/************************************************************************/
/*                               GetGCPs()                              */
/************************************************************************/

const GDAL_GCP *NUMPYDataset::GetGCPs()

{
    return pasGCPList;
}

/************************************************************************/
/*                              SetGCPs()                               */
/************************************************************************/

CPLErr NUMPYDataset::SetGCPs( int nGCPCount, const GDAL_GCP *pasGCPList,
                              const OGRSpatialReference* poSRS )

{
    m_oGCPSRS.Clear();
    if( poSRS )
        m_oGCPSRS = *poSRS;

    if( this->nGCPCount > 0 )
    {
        GDALDeinitGCPs( this->nGCPCount, this->pasGCPList );
        CPLFree( this->pasGCPList );
    }

    this->nGCPCount = nGCPCount;

    this->pasGCPList = GDALDuplicateGCPs( nGCPCount, pasGCPList );

    return CE_None;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset *NUMPYDataset::Open( GDALOpenInfo * poOpenInfo )

{
    PyArrayObject *psArray;

/* -------------------------------------------------------------------- */
/*      Is this a numpy dataset name?                                   */
/* -------------------------------------------------------------------- */
    if( !EQUALN(poOpenInfo->pszFilename,"NUMPY:::",8)
        || poOpenInfo->fpL != NULL )
        return NULL;

    psArray = NULL;
    sscanf( poOpenInfo->pszFilename+8, "%p", &(psArray) );
    if( psArray == NULL )
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                  "Failed to parse meaningful pointer value from NUMPY name\n"
                  "string: %s\n",
                  poOpenInfo->pszFilename );
        return NULL;
    }

    if( !CPLTestBool(CPLGetConfigOption("GDAL_ARRAY_OPEN_BY_FILENAME",
                                        "FALSE")) )
    {
        if( CPLGetConfigOption("GDAL_ARRAY_OPEN_BY_FILENAME", NULL) == NULL )
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                    "Opening a NumPy array through gdal.Open(gdal_array.GetArrayFilename()) "
                    "is no longer supported by default unless the GDAL_ARRAY_OPEN_BY_FILENAME "
                    "configuration option is set to TRUE. The recommended way is to use "
                    "gdal_array.OpenArray() instead.");
        }
        return NULL;
    }

    return Open(psArray);
}

static GDALDataType NumpyTypeToGDALType(PyArrayObject *psArray)
{
    switch( PyArray_DESCR(psArray)->type_num )
    {
      case NPY_CDOUBLE:
        return GDT_CFloat64;

      case NPY_CFLOAT:
        return GDT_CFloat32;

      // case NPY_CHALF
      //   return GDT_CFloat16;

      case NPY_DOUBLE:
        return GDT_Float64;

      case NPY_FLOAT:
        return GDT_Float32;

      case NPY_HALF:
        return GDT_Float16;

      case NPY_INT32:
        return GDT_Int32;

      case NPY_UINT32:
        return GDT_UInt32;

      case NPY_INT64:
        return GDT_Int64;

      case NPY_UINT64:
        return GDT_UInt64;

      case NPY_INT16:
        return GDT_Int16;

      case NPY_UINT16:
        return GDT_UInt16;

      case NPY_BYTE:
        return GDT_Int8;

      case NPY_UBYTE:
        return GDT_Byte;

      default:
        CPLError( CE_Failure, CPLE_AppDefined,
                  "Unable to access numpy arrays of typecode `%c'.",
                  PyArray_DESCR(psArray)->type );
        return GDT_Unknown;
    }
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset* NUMPYDataset::Open( PyArrayObject *psArray, bool binterleave )
{
    GDALDataType  eType;
    int     nBands;

/* -------------------------------------------------------------------- */
/*      Is this a directly mappable Python array?  Verify rank, and     */
/*      data type.                                                      */
/* -------------------------------------------------------------------- */

    if( PyArray_NDIM(psArray) < 2 || PyArray_NDIM(psArray) > 3 )
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                  "Illegal numpy array rank %d.",
                  PyArray_NDIM(psArray) );
        return NULL;
    }

    eType = NumpyTypeToGDALType(psArray);
    if( eType == GDT_Unknown )
    {
        return NULL;
    }

/* -------------------------------------------------------------------- */
/*      Create the new NUMPYDataset object.                             */
/* -------------------------------------------------------------------- */
    NUMPYDataset *poDS;

    poDS = new NUMPYDataset();
    poDS->poDriver = static_cast<GDALDriver*>(GDALGetDriverByName("NUMPY"));

    poDS->psArray = psArray;

    poDS->eAccess = (PyArray_FLAGS(psArray) & NPY_ARRAY_WRITEABLE) ? GA_Update : GA_ReadOnly;

/* -------------------------------------------------------------------- */
/*      Add a reference to the array.                                   */
/* -------------------------------------------------------------------- */
    Py_INCREF( psArray );

/* -------------------------------------------------------------------- */
/*      Workout the data layout.                                        */
/* -------------------------------------------------------------------- */
    npy_intp nBandOffset;
    npy_intp nPixelOffset;
    npy_intp nLineOffset;

    int xdim = binterleave ? 2 : 1;
    int ydim = binterleave ? 1 : 0;
    int bdim = binterleave ? 0 : 2;

    if( PyArray_NDIM(psArray) == 3 )
    {
        if( PyArray_DIMS(psArray)[0] > INT_MAX ||
            PyArray_DIMS(psArray)[1] > INT_MAX ||
            PyArray_DIMS(psArray)[2] > INT_MAX ||
            !GDALCheckBandCount(static_cast<int>(PyArray_DIMS(psArray)[bdim]), 0) )
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "Too big array dimensions");
            delete poDS;
            return NULL;
        }
        nBands = static_cast<int>(PyArray_DIMS(psArray)[bdim]);
        nBandOffset = PyArray_STRIDES(psArray)[bdim];
        poDS->nRasterXSize = static_cast<int>(PyArray_DIMS(psArray)[xdim]);
        nPixelOffset = PyArray_STRIDES(psArray)[xdim];
        poDS->nRasterYSize = static_cast<int>(PyArray_DIMS(psArray)[ydim]);
        nLineOffset = PyArray_STRIDES(psArray)[ydim];
    }
    else
    {
        if( PyArray_DIMS(psArray)[0] > INT_MAX ||
            PyArray_DIMS(psArray)[1] > INT_MAX )
        {
            delete poDS;
            return NULL;
        }
        nBands = 1;
        nBandOffset = 0;
        poDS->nRasterXSize = static_cast<int>(PyArray_DIMS(psArray)[1]);
        nPixelOffset = PyArray_STRIDES(psArray)[1];
        poDS->nRasterYSize = static_cast<int>(PyArray_DIMS(psArray)[0]);
        nLineOffset = PyArray_STRIDES(psArray)[0];
    }

/* -------------------------------------------------------------------- */
/*      Create band information objects.                                */
/* -------------------------------------------------------------------- */
    for( int iBand = 0; iBand < nBands; iBand++ )
    {
        poDS->SetBand( iBand+1,
                       (GDALRasterBand *)
                       MEMCreateRasterBandEx( poDS, iBand+1,
                                (GByte *) PyArray_DATA(psArray) + nBandOffset*iBand,
                                          eType, nPixelOffset, nLineOffset,
                                          FALSE ) );
    }

/* -------------------------------------------------------------------- */
/*      Try to return a regular handle on the file.                     */
/* -------------------------------------------------------------------- */
    return poDS;
}

/************************************************************************/
/*                       NUMPYMultiDimensionalDataset                   */
/************************************************************************/

class NUMPYMultiDimensionalDataset : public GDALDataset
{
    PyArrayObject *psArray = nullptr;
    std::unique_ptr<GDALDataset> poMEMDS{};

    NUMPYMultiDimensionalDataset();
    ~NUMPYMultiDimensionalDataset();

public:
    static GDALDataset *Open( PyArrayObject *psArray );

    std::shared_ptr<GDALGroup> GetRootGroup() const override { return poMEMDS->GetRootGroup(); }
};

/************************************************************************/
/*                     NUMPYMultiDimensionalDataset()                   */
/************************************************************************/

NUMPYMultiDimensionalDataset::NUMPYMultiDimensionalDataset()
{
}

/************************************************************************/
/*                    ~NUMPYMultiDimensionalDataset()                   */
/************************************************************************/

NUMPYMultiDimensionalDataset::~NUMPYMultiDimensionalDataset()
{
    // Although the module has thread disabled, we go here from GDALClose()
    SWIG_PYTHON_THREAD_BEGIN_BLOCK;

    Py_DECREF( psArray );

    SWIG_PYTHON_THREAD_END_BLOCK;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset* NUMPYMultiDimensionalDataset::Open( PyArrayObject *psArray )
{
    const auto eType = NumpyTypeToGDALType(psArray);
    if( eType == GDT_Unknown )
    {
        return nullptr;
    }
    auto poMemDriver = GDALDriver::FromHandle(GDALGetDriverByName("MEM"));
    if( !poMemDriver )
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "MEM driver not available");
        return nullptr;
    }

    auto poMEMDS = poMemDriver->CreateMultiDimensional("", nullptr, nullptr);
    assert(poMEMDS);
    auto poGroup = poMEMDS->GetRootGroup();
    assert(poGroup);
    std::vector<std::shared_ptr<GDALDimension>> apoDims;
    const auto ndims = PyArray_NDIM(psArray);
    CPLString strides;
    for( int i = 0; i < ndims; i++ )
    {
        auto poDim = poGroup->CreateDimension(std::string(CPLSPrintf("dim%d", i)),
                                              std::string(),
                                              std::string(),
                                              PyArray_DIMS(psArray)[i],
                                              nullptr);
        apoDims.push_back(poDim);
        if( i > 0 )
            strides += ',';
        strides += CPLSPrintf(CPL_FRMT_GIB,
                              static_cast<GIntBig>(PyArray_STRIDES(psArray)[i]));
    }
    CPLStringList aosOptions;
    aosOptions.SetNameValue("STRIDES", strides.c_str());
    auto mdArray = MEMGroupCreateMDArray(poGroup.get(), "array",
                                      apoDims,
                                      GDALExtendedDataType::Create(eType),
                                      PyArray_DATA(psArray),
                                      aosOptions.List());
    if( !mdArray )
    {
        delete poMEMDS;
        return nullptr;
    }

    auto poDS = new NUMPYMultiDimensionalDataset();
    poDS->poDriver = GDALDriver::FromHandle(GDALGetDriverByName("NUMPY"));
    poDS->psArray = psArray;
    Py_INCREF( psArray );
    poDS->eAccess = GA_ReadOnly;
    poDS->poMEMDS.reset(poMEMDS);
    return poDS;
}

%}


#ifdef SWIGPYTHON
%nothread;
#endif


// So that SWIGTYPE_p_f_double_p_q_const__char_p_void__int is declared...
/************************************************************************/
/*                            TermProgress()                            */
/************************************************************************/

%rename (TermProgress_nocb) GDALTermProgress_nocb;
%feature( "kwargs" ) GDALTermProgress_nocb;
%inline %{
int GDALTermProgress_nocb( double dfProgress, const char * pszMessage=NULL, void *pData=NULL ) {
  return GDALTermProgress( dfProgress, pszMessage, pData);
}
%}

%rename (TermProgress) GDALTermProgress;
%callback("%s");
int GDALTermProgress( double, const char *, void * );
%nocallback;

%include "callback.i"

%typemap(in,numinputs=1) (PyArrayObject *psArray)
{
  /* %typemap(in,numinputs=1) (PyArrayObject  *psArray) */
  if ($input != NULL && PyArray_Check($input))
  {
      $1 = (PyArrayObject*)($input);
  }
  else
  {
      PyErr_SetString(PyExc_TypeError, "not a numpy array");
      SWIG_fail;
  }
}

%feature("except") OpenNumPyArray {
    const int bLocalUseExceptions = GetUseExceptions();
    if ( bLocalUseExceptions ) {
        pushErrorHandler();
    }
    $action
    if ( bLocalUseExceptions ) {
        popErrorHandler();
    }
%#ifndef SED_HACKS
    if( result == NULL && bLocalUseExceptions ) {
      CPLErr eclass = CPLGetLastErrorType();
      if ( eclass == CE_Failure || eclass == CE_Fatal ) {
        SWIG_exception( SWIG_RuntimeError, CPLGetLastErrorMsg() );
      }
    }
%#endif
    if( result != NULL && bLocalUseExceptions ) {
%#ifdef SED_HACKS
        bLocalUseExceptionsCode = FALSE;
%#endif
    }
}

%newobject OpenNumPyArray;
%inline %{
GDALDatasetShadow* OpenNumPyArray(PyArrayObject *psArray, bool binterleave)
{
    return NUMPYDataset::Open( psArray, binterleave );
}
%}

%feature("except") OpenMultiDimensionalNumPyArray {
    const int bLocalUseExceptions = GetUseExceptions();
    if ( bLocalUseExceptions ) {
        pushErrorHandler();
    }
    $action
    if ( bLocalUseExceptions ) {
        popErrorHandler();
    }
%#ifndef SED_HACKS
    if( result == NULL && bLocalUseExceptions ) {
      CPLErr eclass = CPLGetLastErrorType();
      if ( eclass == CE_Failure || eclass == CE_Fatal ) {
        SWIG_exception( SWIG_RuntimeError, CPLGetLastErrorMsg() );
      }
    }
%#endif
    if( result != NULL && bLocalUseExceptions ) {
%#ifdef SED_HACKS
        bLocalUseExceptionsCode = FALSE;
%#endif
    }
}

%newobject OpenMultiDimensionalNumPyArray;
%inline %{
GDALDatasetShadow* OpenMultiDimensionalNumPyArray(PyArrayObject *psArray)
{
    return NUMPYMultiDimensionalDataset::Open( psArray );
}
%}

/* Deprecated */
%inline %{
retStringAndCPLFree* GetArrayFilename(PyArrayObject *psArray)
{
    char      szString[128];

    GDALRegister_NUMPY();

    /* I wish I had a safe way of checking the type */
    snprintf( szString, sizeof(szString), "NUMPY:::%p", psArray );
    return CPLStrdup(szString);
}
%}

#ifdef SWIGPYTHON
%thread;
#endif

%feature( "kwargs" ) BandRasterIONumPy;
%inline %{
  CPLErr BandRasterIONumPy( GDALRasterBandShadow* band, int bWrite, double xoff, double yoff, double xsize, double ysize,
                            PyArrayObject *psArray,
                            GDALDataType buf_type,
                            GDALRIOResampleAlg resample_alg,
                            GDALProgressFunc callback = NULL,
                            void* callback_data = NULL) {

    if( PyArray_NDIM(psArray) < 2 || PyArray_NDIM(psArray) > 3 )
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                  "Illegal numpy array rank %d.\n",
                  PyArray_NDIM(psArray) );
        return CE_Failure;
    }

    if( !bWrite && !(PyArray_FLAGS(psArray) & NPY_ARRAY_WRITEABLE) )
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                  "Cannot read in a non-writeable array." );
        return CE_Failure;
    }

    int xdim = ( PyArray_NDIM(psArray) == 2) ? 1 : 2;
    int ydim = ( PyArray_NDIM(psArray) == 2) ? 0 : 1;

    if( PyArray_DIMS(psArray)[xdim] > INT_MAX ||
        PyArray_DIMS(psArray)[ydim] > INT_MAX )
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                    "Too big array dimensions");
        return CE_Failure;
    }
    int nxsize, nysize;
    GSpacing pixel_space, line_space;
    nxsize = static_cast<int>(PyArray_DIMS(psArray)[xdim]);
    nysize = static_cast<int>(PyArray_DIMS(psArray)[ydim]);
    pixel_space = PyArray_STRIDES(psArray)[xdim];
    line_space = PyArray_STRIDES(psArray)[ydim];

    GDALRasterIOExtraArg sExtraArg;
    INIT_RASTERIO_EXTRA_ARG(sExtraArg);
    sExtraArg.eResampleAlg = resample_alg;
    sExtraArg.pfnProgress = callback;
    sExtraArg.pProgressData = callback_data;
    int nXOff = (int)(xoff + 0.5);
    int nYOff = (int)(yoff + 0.5);
    int nXSize = (int)(xsize + 0.5);
    int nYSize = (int)(ysize + 0.5);
    if( fabs(xoff-nXOff) > 1e-8 || fabs(yoff-nYOff) > 1e-8 ||
        fabs(xsize-nXSize) > 1e-8 || fabs(ysize-nYSize) > 1e-8 )
    {
        sExtraArg.bFloatingPointWindowValidity = TRUE;
        sExtraArg.dfXOff = xoff;
        sExtraArg.dfYOff = yoff;
        sExtraArg.dfXSize = xsize;
        sExtraArg.dfYSize = ysize;
    }

    return  GDALRasterIOEx( band, (bWrite) ? GF_Write : GF_Read, nXOff, nYOff, nXSize, nYSize,
                          PyArray_DATA(psArray), nxsize, nysize,
                          buf_type, pixel_space, line_space, &sExtraArg );
  }
%}

%feature( "kwargs" ) DatasetIONumPy;
%apply (int nList, int *pList ) { (int band_list, int *pband_list ) };
%inline %{
  CPLErr DatasetIONumPy( GDALDatasetShadow* ds, int bWrite, double xoff, double yoff, double xsize, double ysize,
                         PyArrayObject *psArray,
                         GDALDataType buf_type,
                         GDALRIOResampleAlg resample_alg,
                         GDALProgressFunc callback = NULL,
                         void* callback_data = NULL,
                         bool binterleave = true,
                         int band_list = 0, int *pband_list = 0 )
{
    if( PyArray_NDIM(psArray) != 3 )
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                  "Illegal numpy array rank %d.",
                  PyArray_NDIM(psArray) );
        return CE_Failure;
    }

    if( !bWrite && !(PyArray_FLAGS(psArray) & NPY_ARRAY_WRITEABLE) )
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                  "Cannot read in a non-writeable array." );
        return CE_Failure;
    }

    int xdim = binterleave ? 2 : 1;
    int ydim = binterleave ? 1 : 0;
    int bdim = binterleave ? 0 : 2;
    if( PyArray_DIMS(psArray)[xdim] > INT_MAX ||
        PyArray_DIMS(psArray)[ydim] > INT_MAX ||
        PyArray_DIMS(psArray)[bdim] > INT_MAX )
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                    "Too big array dimensions");
        return CE_Failure;
    }

    int bandsize, nxsize, nysize;
    GIntBig pixel_space, line_space, band_space;
    nxsize = static_cast<int>(PyArray_DIMS(psArray)[xdim]);
    nysize = static_cast<int>(PyArray_DIMS(psArray)[ydim]);
    bandsize = static_cast<int>(PyArray_DIMS(psArray)[bdim]);
    int bandcount = band_list ? band_list : GDALGetRasterCount(ds);
    if( bandsize != bandcount )
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                  "Illegal numpy array band dimension %d. Expected value: %d",
                  bandsize, bandcount );
        return CE_Failure;
    }
    pixel_space = PyArray_STRIDES(psArray)[xdim];
    line_space = PyArray_STRIDES(psArray)[ydim];
    band_space = PyArray_STRIDES(psArray)[bdim];

    GDALRasterIOExtraArg sExtraArg;
    INIT_RASTERIO_EXTRA_ARG(sExtraArg);
    sExtraArg.eResampleAlg = resample_alg;
    sExtraArg.pfnProgress = callback;
    sExtraArg.pProgressData = callback_data;
    int nXOff = (int)(xoff + 0.5);
    int nYOff = (int)(yoff + 0.5);
    int nXSize = (int)(xsize + 0.5);
    int nYSize = (int)(ysize + 0.5);
    if( fabs(xoff-nXOff) > 1e-8 || fabs(yoff-nYOff) > 1e-8 ||
        fabs(xsize-nXSize) > 1e-8 || fabs(ysize-nYSize) > 1e-8 )
    {
        sExtraArg.bFloatingPointWindowValidity = TRUE;
        sExtraArg.dfXOff = xoff;
        sExtraArg.dfYOff = yoff;
        sExtraArg.dfXSize = xsize;
        sExtraArg.dfYSize = ysize;
    }

    return  GDALDatasetRasterIOEx( ds, (bWrite) ? GF_Write : GF_Read, nXOff, nYOff, nXSize, nYSize,
                                   PyArray_DATA(psArray), nxsize, nysize,
                                   buf_type,
                                   bandcount, pband_list,
                                   pixel_space, line_space, band_space, &sExtraArg );
  }
%}
%clear (int band_list, int *pband_list );

%{
static bool CheckNumericDataType(GDALExtendedDataTypeHS* dt)
{
    auto klass = GDALExtendedDataTypeGetClass(dt);
    if( klass == GEDTC_NUMERIC )
        return true;
    if( klass == GEDTC_STRING )
        return false;
    CPLAssert( klass == GEDTC_COMPOUND );
    size_t nCount = 0;
    GDALEDTComponentH* comps = GDALExtendedDataTypeGetComponents(dt, &nCount);
    bool ret = true;
    for( size_t i = 0; i < nCount; i++ )
    {
        auto tmpType = GDALEDTComponentGetType(comps[i]);
        ret = CheckNumericDataType(tmpType);
        GDALExtendedDataTypeRelease(tmpType);
        if( !ret )
            break;
    }
    GDALExtendedDataTypeFreeComponents(comps, nCount);
    return ret;
}
%}

%apply (int nList, GUIntBig* pList) {(int nDims1, GUIntBig *array_start_idx)};
%apply (int nList, GIntBig* pList) {(int nDims3, GIntBig *array_step)};
%inline %{
  CPLErr MDArrayIONumPy( bool bWrite,
                          GDALMDArrayHS* mdarray,
                          PyArrayObject *psArray,
                          int nDims1, GUIntBig* array_start_idx,
                          int nDims3, GIntBig* array_step,
                          GDALExtendedDataTypeHS* buffer_datatype) {

    if( !CheckNumericDataType(buffer_datatype) )
    {
        CPLError(CE_Failure, CPLE_NotSupported,
            "String buffer data type not supported in SWIG bindings");
        return CE_Failure;
    }
    const int nExpectedDims = (int)GDALMDArrayGetDimensionCount(mdarray);
    if( PyArray_NDIM(psArray) != nExpectedDims )
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                  "Illegal numpy array rank %d.",
                  PyArray_NDIM(psArray) );
        return CE_Failure;
    }

    std::vector<size_t> count_internal(nExpectedDims+1);
    std::vector<GPtrDiff_t> buffer_stride_internal(nExpectedDims+1);
    const size_t nDTSize = GDALExtendedDataTypeGetSize(buffer_datatype);
    if( nDTSize == 0 )
    {
        return CE_Failure;
    }
    for( int i = 0; i < nExpectedDims; i++ )
    {
        count_internal[i] = PyArray_DIMS(psArray)[i];
        if( (PyArray_STRIDES(psArray)[i] % nDTSize) != 0 )
        {
            CPLError( CE_Failure, CPLE_AppDefined,
                      "Stride[%d] not a multiple of data type size",
                      i );
            return CE_Failure;
        }
        buffer_stride_internal[i] = PyArray_STRIDES(psArray)[i] / nDTSize;
    }

    if( bWrite )
    {
        return GDALMDArrayWrite( mdarray,
                                array_start_idx,
                                &count_internal[0],
                                array_step,
                                &buffer_stride_internal[0],
                                buffer_datatype,
                                PyArray_DATA(psArray),
                                NULL, 0) ? CE_None : CE_Failure;
    }
    else
    {
        return GDALMDArrayRead( mdarray,
                                array_start_idx,
                                &count_internal[0],
                                array_step,
                                &buffer_stride_internal[0],
                                buffer_datatype,
                                PyArray_DATA(psArray),
                                NULL, 0) ? CE_None : CE_Failure;
    }
  }
%}

#ifdef SWIGPYTHON
%nothread;
#endif

%newobject _RecordBatchAsNumpy;
%inline %{
typedef void* VoidPtrAsLong;

static bool AddNumpyArrayToDict(PyObject *dict,
                                const struct ArrowSchema* schemaField,
                                const struct ArrowArray* arrayField,
                                const std::string& osPrefix,
                                PyObject* pointerArrayKeeper)
{
    npy_intp dims = arrayField->length;
    const char* arrowType = schemaField->format;
    int typenum = -1;
    int sizeOfType = 0;
    const struct
    {
        char        arrowType;
        int         numpyType;
        int         sizeOfType;
    } MapArrowTypeToNumpyType[] = {
        { 'b', NPY_BOOL,    1 },
        { 'C', NPY_UINT8,   1 },
        { 'c', NPY_INT8,    1 },
        { 'S', NPY_UINT16,  2 },
        { 's', NPY_INT16,   2 },
        { 'I', NPY_UINT32,  4 },
        { 'i', NPY_INT32,   4 },
        { 'L', NPY_UINT64,  8 },
        { 'l', NPY_INT64,   8 },
        { 'e', NPY_FLOAT16, 2 },
        { 'f', NPY_FLOAT32, 4 },
        { 'g', NPY_FLOAT64, 8 },
        // { 'E', NPY_COMPLEX32, 4 },
        // { 'F', NPY_COMPLEX64, 8 },
        // { 'G', NPY_COMPLEX128, 16 },
    };
    const size_t nEltsInMapArrowTypeToNumpyType =
        sizeof(MapArrowTypeToNumpyType) / sizeof(MapArrowTypeToNumpyType[0]);
    const bool bIsLargeList = (arrowType[0] == '+' &&
                               arrowType[1] == 'L' &&
                               arrowType[2] == '\0' &&
                               schemaField->n_children == 1);
    const bool bIsList = (arrowType[0] == '+' &&
                          arrowType[1] == 'l' &&
                          arrowType[2] == '\0' &&
                          schemaField->n_children == 1);
    const bool bIsFixedSizeList = (arrowType[0] == '+' &&
                                   arrowType[1] == 'w' &&
                                   arrowType[2] == ':' &&
                                   schemaField->n_children == 1);
    for( size_t j = 0; j < nEltsInMapArrowTypeToNumpyType; ++j )
    {
        if( arrowType[0] == MapArrowTypeToNumpyType[j].arrowType &&
            arrowType[1] == '\0' )
        {
            typenum = MapArrowTypeToNumpyType[j].numpyType;
            sizeOfType = MapArrowTypeToNumpyType[j].sizeOfType;
            break;
        }
        else if( (bIsList || bIsLargeList || bIsFixedSizeList) &&
                 schemaField->children[0]->format[0] == MapArrowTypeToNumpyType[j].arrowType &&
                 schemaField->children[0]->format[1] == '\0' )
        {
            typenum = MapArrowTypeToNumpyType[j].numpyType;
            sizeOfType = MapArrowTypeToNumpyType[j].sizeOfType;
            break;
        }
    }

    PyObject* numpyArray = NULL;
    if( typenum != -1 && !bIsList && !bIsLargeList && schemaField->n_children == 0 )
    {
        if( arrayField->n_buffers != 2 )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Field %s: arrayField->n_buffers != 2",
                     schemaField->name);
            return false;
        }
        if( typenum == NPY_BOOL )
        {
            numpyArray = PyArray_SimpleNew(1, &dims, NPY_BOOL);
            for( npy_intp j = 0; j < dims; j++ )
            {
                size_t srcOffset = static_cast<size_t>(arrayField->offset + j);
                uint8_t val = (((uint8_t*)arrayField->buffers[1])[srcOffset/8] >> (srcOffset % 8)) & 1;
                *(uint8_t*)PyArray_GETPTR1((PyArrayObject *) numpyArray, j) = val;
            }
        }
        else
        {
            numpyArray = PyArray_SimpleNewFromData(1, &dims, typenum,
                                        (char*)arrayField->buffers[1] + static_cast<size_t>(arrayField->offset) * sizeOfType);

            /* Keep a reference to the owner object */
#if NPY_API_VERSION >= 0x00000007
            PyArray_SetBaseObject((PyArrayObject *) numpyArray, pointerArrayKeeper);
#else
            PyArray_BASE((PyArrayObject *) numpyArray) = pointerArrayKeeper;
#endif
            Py_INCREF(pointerArrayKeeper);
        }
    }
    else if( typenum != -1 && (bIsList || bIsLargeList) )
    {
        if( arrayField->n_buffers != 2 )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Field %s: arrayField->n_buffers != 2",
                     schemaField->name);
            return false;
        }
        if( arrayField->n_children != 1 )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Field %s: arrayField->n_children != 1",
                     schemaField->name);
            return false;
        }
        if( arrayField->children[0]->n_buffers != 2 )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Field %s: arrayField->children[0]->n_buffers != 2",
                     schemaField->name);
            return false;
        }
        const int32_t* offsets = bIsList ? (const int32_t*)arrayField->buffers[1] + arrayField->offset : NULL;
        const int64_t* largeOffsets = bIsLargeList ? (const int64_t*)arrayField->buffers[1] + arrayField->offset : NULL;
        if( arrayField->children[0]->length < (offsets ? offsets[arrayField->length] : largeOffsets[arrayField->length]) )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Field %s: arrayField->children[0]->length (=%d) < offsets[arrayField->length] (=%d)",
                     schemaField->name,
                     int(arrayField->children[0]->length),
                     int(offsets ? offsets[arrayField->length] : largeOffsets[arrayField->length]));
            return false;
        }
        numpyArray = PyArray_SimpleNew(1, &dims, NPY_OBJECT);
        for( npy_intp j = 0; j < dims; j++ )
        {
            npy_intp nvalues = offsets ? offsets[j+1] - offsets[j] : (npy_intp)(largeOffsets[j+1] - largeOffsets[j]);
            PyObject* subObj;
            if( typenum == NPY_BOOL )
            {
                subObj = PyArray_SimpleNew(1, &nvalues, NPY_BOOL);
                for( npy_intp k = 0; k < nvalues; k++ )
                {
                    size_t srcOffset = static_cast<size_t>(arrayField->children[0]->offset + (offsets ? offsets[j] : largeOffsets[j]) + k);
                    uint8_t val = (((uint8_t*)arrayField->children[0]->buffers[1])[srcOffset / 8]  >> (srcOffset % 8)) & 1;
                    *(uint8_t*)PyArray_GETPTR1((PyArrayObject *) subObj, k) = val;
                }
            }
            else
            {
                subObj = PyArray_SimpleNewFromData(
                    1, &nvalues, typenum,
                    (char*)arrayField->children[0]->buffers[1] + (static_cast<size_t>(arrayField->children[0]->offset) + (offsets ? offsets[j] : largeOffsets[j])) * sizeOfType);
                /* Keep a reference to the owner object */
#if NPY_API_VERSION >= 0x00000007
                PyArray_SetBaseObject((PyArrayObject *) subObj, pointerArrayKeeper);
#else
                PyArray_BASE((PyArrayObject *) subObj) = pointerArrayKeeper;
#endif
                Py_INCREF(pointerArrayKeeper);
            }
            memcpy(PyArray_GETPTR1((PyArrayObject *) numpyArray, j),
                   &subObj,
                   sizeof(PyObject*));
        }
    }
    else if( typenum != -1 && bIsFixedSizeList )
    {
        if( arrayField->n_buffers != 1 )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Field %s: arrayField->n_buffers != 1",
                     schemaField->name);
            return false;
        }
        if( arrayField->n_children != 1 )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Field %s: arrayField->n_children != 1",
                     schemaField->name);
            return false;
        }
        const struct ArrowArray* psChildArray = arrayField->children[0];
        if( psChildArray->n_buffers != 2 )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Field %s: psChildArray->n_buffers != 2",
                     schemaField->name);
            return false;
        }
        const int nLength = atoi(arrowType + strlen("+w:"));
        if( psChildArray->length < nLength * arrayField->length )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Field %s: psChildArray->length < nLength * arrayField->length",
                     schemaField->name);
            return false;
        }
        numpyArray = PyArray_SimpleNew(1, &dims, NPY_OBJECT);
        for( npy_intp j = 0; j < dims; j++ )
        {
            PyObject* subObj;
            npy_intp nvalues = nLength;
            if( typenum == NPY_BOOL )
            {
                subObj = PyArray_SimpleNew(1, &nvalues, NPY_BOOL);
                for( npy_intp k = 0; k < nvalues; k++ )
                {
                    size_t srcOffset = static_cast<size_t>(psChildArray->offset + (j + arrayField->offset) * nLength + k);
                    uint8_t val = (((uint8_t*)psChildArray->buffers[1])[srcOffset / 8]  >> (srcOffset % 8)) & 1;
                    *(uint8_t*)PyArray_GETPTR1((PyArrayObject *) subObj, k) = val;
                }
            }
            else
            {
                subObj = PyArray_SimpleNewFromData(
                    1, &nvalues, typenum,
                    (char*)psChildArray->buffers[1] + static_cast<size_t>((psChildArray->offset + (j + arrayField->offset) * nLength) * sizeOfType));
                /* Keep a reference to the owner object */
#if NPY_API_VERSION >= 0x00000007
                PyArray_SetBaseObject((PyArrayObject *) subObj, pointerArrayKeeper);
#else
                PyArray_BASE((PyArrayObject *) subObj) = pointerArrayKeeper;
#endif
                Py_INCREF(pointerArrayKeeper);
            }
            memcpy(PyArray_GETPTR1((PyArrayObject *) numpyArray, j),
                   &subObj,
                   sizeof(PyObject*));
        }
    }
    else if( (arrowType[0] == 'u' || /* string */
              arrowType[0] == 'z'    /* binary */) && arrowType[1] == '\0' &&
              schemaField->n_children == 0 )
    {
        if( arrayField->n_buffers != 3 )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Field %s: arrayField->n_buffers != 3",
                     schemaField->name);
            return false;
        }
        const int32_t* offsets = (const int32_t*)arrayField->buffers[1] + static_cast<size_t>(arrayField->offset);
        // numpy can't deal with zero length strings
        int32_t maxLength = 1;
        int32_t minLength = 0x7FFFFFFF;
        int64_t averageLength = 0;
        for( npy_intp j = 0; j < dims; j++ )
        {
            const int32_t nLength = offsets[j+1] - offsets[j];
            if( nLength < minLength )
                minLength = nLength;
            if( nLength > maxLength )
                maxLength = nLength;
            averageLength += nLength;
        }
        if( dims )
            averageLength /= dims;


        if( arrowType[0] == 'z' && (minLength == 0 || minLength != maxLength) )
        {
            const uint8_t* panNotNulls =
                 arrayField->null_count == 0 ? NULL :
                (const uint8_t*)arrayField->buffers[0];
            numpyArray = PyArray_SimpleNew(1, &dims, NPY_OBJECT);
            for( npy_intp j = 0; j < dims; j++ )
            {
                PyObject* subObj;
                size_t srcOffset = static_cast<size_t>(arrayField->offset + j);
                if( panNotNulls && (panNotNulls[srcOffset / 8] & (1 << (srcOffset%8))) == 0 )
                {
                    subObj = Py_None;
                    Py_INCREF(subObj);
                }
                else
                {
                    const int32_t nLength = offsets[j+1] - offsets[j];
                    subObj = PyBytes_FromStringAndSize(
                        ((const char*)arrayField->buffers[2]) + offsets[j],
                        nLength);
                }
                memcpy(PyArray_GETPTR1((PyArrayObject *) numpyArray, j),
                       &subObj,
                       sizeof(PyObject*));
            }
        }
        else if( arrowType[0] == 'u' && dims > 0 && maxLength > 32 &&
                 maxLength > 100 * 1000 / dims &&
                 maxLength > averageLength * 2 )
        {
            // If the maximum string size is significantly large, and
            // larger than the average one, then do not use fixed size
            // strings, but create an array of string objects to save memory
            const uint8_t* panNotNulls =
                 arrayField->null_count == 0 ? NULL :
                (const uint8_t*)arrayField->buffers[0];
            numpyArray = PyArray_SimpleNew(1, &dims, NPY_OBJECT);
            for( npy_intp j = 0; j < dims; j++ )
            {
                PyObject* subObj;
                size_t srcOffset = static_cast<size_t>(arrayField->offset + j);
                if( panNotNulls && (panNotNulls[srcOffset / 8] & (1 << (srcOffset%8))) == 0 )
                {
                    subObj = Py_None;
                    Py_INCREF(subObj);
                }
                else
                {
                    const int32_t nLength = offsets[j+1] - offsets[j];
                    subObj = PyUnicode_FromStringAndSize(
                        ((const char*)arrayField->buffers[2]) + offsets[j],
                        nLength);
                }
                memcpy(PyArray_GETPTR1((PyArrayObject *) numpyArray, j),
                       &subObj,
                       sizeof(PyObject*));
            }
        }
        else
        {
            // create the dtype string
            PyObject *pDTypeString = PyUnicode_FromFormat("%c%u",
                arrowType[0] == 'u' ? 'S' : 'V', maxLength);
            // out type description object
            PyArray_Descr *pDescr = NULL;
            PyArray_DescrConverter(pDTypeString, &pDescr);
            Py_DECREF(pDTypeString);

            if( minLength == maxLength )
            {
                numpyArray = PyArray_NewFromDescr(
                    &PyArray_Type, pDescr, 1, &dims, NULL,
                    (char*)arrayField->buffers[2] + offsets[0], 0, NULL);

                /* Keep a reference to the owner object */
#if NPY_API_VERSION >= 0x00000007
                PyArray_SetBaseObject((PyArrayObject *) numpyArray, pointerArrayKeeper);
#else
                PyArray_BASE((PyArrayObject *) numpyArray) = pointerArrayKeeper;
#endif
                Py_INCREF(pointerArrayKeeper);
            }
            else
            {
                // create array
                numpyArray = PyArray_SimpleNewFromDescr(1, &dims, pDescr);
                for( npy_intp j = 0; j < dims; j++ )
                {
                    const int32_t nLength = offsets[j+1] - offsets[j];
                    if( nLength > 0 )
                    {
                        memcpy(PyArray_GETPTR1((PyArrayObject *) numpyArray, j),
                               ((const char*)arrayField->buffers[2]) + offsets[j],
                               nLength);
                    }
                    if( nLength < maxLength )
                    {
                        memset(((char*)PyArray_GETPTR1((PyArrayObject *) numpyArray, j)) + nLength,
                               0,
                               maxLength - nLength);
                    }
                }
            }
        }
    }
    else if( (arrowType[0] == 'U' || /* string */
              arrowType[0] == 'Z'    /* binary */) && arrowType[1] == '\0' &&
              schemaField->n_children == 0 )
    {
        if( arrayField->n_buffers != 3 )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Field %s: arrayField->n_buffers != 3",
                     schemaField->name);
            return false;
        }
        const int64_t* offsets = (const int64_t*)arrayField->buffers[1] + static_cast<size_t>(arrayField->offset);
        // numpy can't deal with zero length strings
        int64_t maxLength = 1;
        int64_t minLength = ((int64_t)0x7FFFFFFF << 32) | 0xFFFFFFFF;
        int64_t averageLength = 0;
        for( npy_intp j = 0; j < dims; j++ )
        {
            const int64_t nLength = offsets[j+1] - offsets[j];
            if( nLength < minLength )
                minLength = nLength;
            if( nLength > maxLength )
                maxLength = nLength;
            averageLength += nLength;
        }
        if( dims )
            averageLength /= dims;

        if( arrowType[0] == 'Z' && (minLength == 0 || minLength != maxLength || maxLength > 0x7FFFFFFF) )
        {
            const uint8_t* panNotNulls =
                 arrayField->null_count == 0 ? NULL :
                (const uint8_t*)arrayField->buffers[0];
            numpyArray = PyArray_SimpleNew(1, &dims, NPY_OBJECT);
            for( npy_intp j = 0; j < dims; j++ )
            {
                PyObject* subObj;
                size_t srcOffset = static_cast<size_t>(arrayField->offset + j);
                if( panNotNulls && (panNotNulls[srcOffset / 8] & (1 << (srcOffset%8))) == 0 )
                {
                    subObj = Py_None;
                    Py_INCREF(subObj);
                }
                else
                {
                    const int64_t nLength = offsets[j+1] - offsets[j];
                    subObj = PyBytes_FromStringAndSize(
                        ((const char*)arrayField->buffers[2]) + offsets[j],
                        static_cast<size_t>(nLength));
                }
                memcpy(PyArray_GETPTR1((PyArrayObject *) numpyArray, j),
                       &subObj,
                       sizeof(PyObject*));
            }
        }
        else if( arrowType[0] == 'U' && dims > 0 && maxLength > 32 &&
                 maxLength <= 0x7FFFFFFF &&
                 maxLength > 100 * 1000 / dims &&
                 maxLength > averageLength * 2 )
        {
            // If the maximum string size is significantly large, and
            // larger than the average one, then do not use fixed size
            // strings, but create an array of string objects to save memory
            const uint8_t* panNotNulls =
                 arrayField->null_count == 0 ? NULL :
                (const uint8_t*)arrayField->buffers[0];
            numpyArray = PyArray_SimpleNew(1, &dims, NPY_OBJECT);
            for( npy_intp j = 0; j < dims; j++ )
            {
                PyObject* subObj;
                size_t srcOffset = static_cast<size_t>(arrayField->offset + j);
                if( panNotNulls && (panNotNulls[srcOffset / 8] & (1 << (srcOffset%8))) == 0 )
                {
                    subObj = Py_None;
                    Py_INCREF(subObj);
                }
                else
                {
                    const int32_t nLength = int(offsets[j+1] - offsets[j]);
                    subObj = PyUnicode_FromStringAndSize(
                        ((const char*)arrayField->buffers[2]) + offsets[j],
                        nLength);
                }
                memcpy(PyArray_GETPTR1((PyArrayObject *) numpyArray, j),
                       &subObj,
                       sizeof(PyObject*));
            }
        }
        else
        {
            // We could possibly handle this...
            if( maxLength > 0x7FFFFFFF )
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Field %s: too large value",
                         schemaField->name);
                return false;
            }

            // create the dtype string
            PyObject *pDTypeString = PyUnicode_FromFormat("%c%u",
                arrowType[0] == 'U' ? 'S' : 'V', static_cast<int32_t>(maxLength));
            // out type description object
            PyArray_Descr *pDescr = NULL;
            PyArray_DescrConverter(pDTypeString, &pDescr);
            Py_DECREF(pDTypeString);

            if( minLength == maxLength )
            {
                numpyArray = PyArray_NewFromDescr(
                    &PyArray_Type, pDescr, 1, &dims, NULL,
                    (char*)arrayField->buffers[2] + offsets[0], 0, NULL);

                /* Keep a reference to the owner object */
#if NPY_API_VERSION >= 0x00000007
                PyArray_SetBaseObject((PyArrayObject *) numpyArray, pointerArrayKeeper);
#else
                PyArray_BASE((PyArrayObject *) numpyArray) = pointerArrayKeeper;
#endif
                Py_INCREF(pointerArrayKeeper);
            }
            else
            {
                // create array
                numpyArray = PyArray_SimpleNewFromDescr(1, &dims, pDescr);
                for( npy_intp j = 0; j < dims; j++ )
                {
                    const int32_t nLength = static_cast<int32_t>(offsets[j+1] - offsets[j]);
                    if( nLength > 0 )
                    {
                        memcpy(PyArray_GETPTR1((PyArrayObject *) numpyArray, j),
                               ((const char*)arrayField->buffers[2]) + offsets[j],
                               nLength);
                    }
                    if( nLength < maxLength )
                    {
                        memset(((char*)PyArray_GETPTR1((PyArrayObject *) numpyArray, j)) + nLength,
                               0,
                               static_cast<int32_t>(maxLength) - nLength);
                    }
                }
            }
        }
    }
    else if( arrowType[0] == 'w' && arrowType[1] == ':' &&
             schemaField->n_children == 0 )
    {
        // Fixed width binary
        if( arrayField->n_buffers != 2 )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "field %s:arrayField->n_buffers != 2",
                     schemaField->name);
            return false;
        }
        const int nLength = atoi(arrowType + strlen("w:"));
        numpyArray = PyArray_SimpleNew(1, &dims, NPY_OBJECT);
        for( npy_intp j = 0; j < dims; j++ )
        {
            PyObject* subObj = PyBytes_FromStringAndSize(
                    ((const char*)arrayField->buffers[1]) + static_cast<size_t>(arrayField->offset + j) * nLength,
                    nLength);
            memcpy(PyArray_GETPTR1((PyArrayObject *) numpyArray, j),
                   &subObj,
                   sizeof(PyObject*));
        }
    }
    else if( bIsList &&
             schemaField->children[0]->format[0] == 'u' &&
             schemaField->children[0]->format[1] == '\0' )
    {
        // List of strings
        if( arrayField->n_buffers != 2 )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Field %s: arrayField->n_buffers != 2",
                     schemaField->name);
            return false;
        }
        if( arrayField->n_children != 1 )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Field %s: arrayField->n_children != 1",
                     schemaField->name);
            return false;
        }
        if( arrayField->children[0]->n_buffers != 3 )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Field %s: arrayField->children[0]->n_buffers != 3",
                     schemaField->name);
            return false;
        }
        const int32_t* offsets = (const int32_t*)arrayField->buffers[1] + static_cast<size_t>(arrayField->offset);
        if( arrayField->children[0]->length < offsets[arrayField->length] )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Field %s: arrayField->children[0]->length = (%d) < offsets[arrayField->length] (=%d)",
                     schemaField->name,
                     int(arrayField->children[0]->length),
                     int(offsets[arrayField->length]));
            return false;
        }
        const int32_t* offsetsToBytes = (const int32_t*)arrayField->children[0]->buffers[1] + static_cast<size_t>(arrayField->children[0]->offset);
        const char* bytes = (const char*)arrayField->children[0]->buffers[2];
        numpyArray = PyArray_SimpleNew(1, &dims, NPY_OBJECT);
        for( npy_intp j = 0; j < dims; j++ )
        {
            npy_intp nStrings = offsets[j+1] - offsets[j];
            int32_t maxLength = 1;
            for( npy_intp k = 0; k < nStrings; k++ )
            {
                const int32_t nLength = offsetsToBytes[offsets[j] + k + 1] - offsetsToBytes[offsets[j] + k];
                if( nLength > maxLength )
                    maxLength = nLength;
            }

            // create the dtype string
            PyObject *pDTypeString = PyUnicode_FromFormat("S%d", maxLength);
            // out type description object
            PyArray_Descr *pDescr = NULL;
            PyArray_DescrConverter(pDTypeString, &pDescr);
            Py_DECREF(pDTypeString);

            PyObject* subArray = PyArray_SimpleNewFromDescr(1, &nStrings, pDescr);
            for( npy_intp k = 0; k < nStrings; k++ )
            {
                const int32_t nLength = offsetsToBytes[offsets[j] + k + 1] - offsetsToBytes[offsets[j] + k];
                if( nLength > 0 )
                {
                    memcpy(PyArray_GETPTR1((PyArrayObject *) subArray, k),
                           bytes + offsetsToBytes[offsets[j] + k],
                           nLength);
                }
                if( nLength < maxLength )
                {
                    memset(((char*)PyArray_GETPTR1((PyArrayObject *) subArray, k)) + nLength,
                           0,
                           maxLength - nLength);
                }
            }

            memcpy(PyArray_GETPTR1((PyArrayObject *) numpyArray, j),
                   &subArray,
                   sizeof(PyObject*));
        }
    }
    else if( bIsLargeList &&
             schemaField->children[0]->format[0] == 'U' &&
             schemaField->children[0]->format[1] == '\0' )
    {
        // List of strings
        if( arrayField->n_buffers != 2 )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Field %s: arrayField->n_buffers != 2",
                     schemaField->name);
            return false;
        }
        if( arrayField->n_children != 1 )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Field %s: arrayField->n_children != 1",
                     schemaField->name);
            return false;
        }
        if( arrayField->children[0]->n_buffers != 3 )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Field %s: arrayField->children[0]->n_buffers != 3",
                     schemaField->name);
            return false;
        }
        const int64_t* offsets = (const int64_t*)arrayField->buffers[1] + static_cast<size_t>(arrayField->offset);
        if( arrayField->children[0]->length < offsets[arrayField->length] )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Field %s: arrayField->children[0]->length = (%d) < offsets[arrayField->length] (=%d)",
                     schemaField->name,
                     int(arrayField->children[0]->length),
                     int(offsets[arrayField->length]));
            return false;
        }
        const int64_t* offsetsToBytes = (const int64_t*)arrayField->children[0]->buffers[1] + static_cast<size_t>(arrayField->children[0]->offset);
        const char* bytes = (const char*)arrayField->children[0]->buffers[2];
        numpyArray = PyArray_SimpleNew(1, &dims, NPY_OBJECT);
        for( npy_intp j = 0; j < dims; j++ )
        {
            npy_intp nStrings = offsets[j+1] - offsets[j];
            int64_t maxLength = 1;
            for( npy_intp k = 0; k < nStrings; k++ )
            {
                const int64_t nLength = offsetsToBytes[offsets[j] + k + 1] - offsetsToBytes[offsets[j] + k];
                if( nLength > maxLength )
                    maxLength = nLength;
            }
            if( maxLength >= INT_MAX )
            {
                CPLError(CE_Failure, CPLE_AppDefined, "Too large string");
                return false;
            }

            // create the dtype string
            PyObject *pDTypeString = PyUnicode_FromFormat("S%d", int(maxLength));
            // out type description object
            PyArray_Descr *pDescr = NULL;
            PyArray_DescrConverter(pDTypeString, &pDescr);
            Py_DECREF(pDTypeString);

            PyObject* subArray = PyArray_SimpleNewFromDescr(1, &nStrings, pDescr);
            for( npy_intp k = 0; k < nStrings; k++ )
            {
                const int64_t nLength = offsetsToBytes[offsets[j] + k + 1] - offsetsToBytes[offsets[j] + k];
                if( nLength > 0 )
                {
                    memcpy(PyArray_GETPTR1((PyArrayObject *) subArray, k),
                           bytes + offsetsToBytes[offsets[j] + k],
                           int(nLength));
                }
                if( nLength < maxLength )
                {
                    memset(((char*)PyArray_GETPTR1((PyArrayObject *) subArray, k)) + nLength,
                           0,
                           int(maxLength - nLength));
                }
            }

            memcpy(PyArray_GETPTR1((PyArrayObject *) numpyArray, j),
                   &subArray,
                   sizeof(PyObject*));
        }
    }
    else if( bIsFixedSizeList &&
             schemaField->children[0]->format[0] == 'u' &&
             schemaField->children[0]->format[1] == '\0' )
    {
        // Fixed size list of strings
        const int nStrings = atoi(arrowType + strlen("+w:"));
        if( arrayField->n_buffers != 1 )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Field %s: arrayField->n_buffers != 1",
                     schemaField->name);
            return false;
        }
        if( arrayField->n_children != 1 )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Field %s: arrayField->n_children != 1",
                     schemaField->name);
            return false;
        }
        if( arrayField->children[0]->n_buffers != 3 )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Field %s: arrayField->children[0]->n_buffers != 3",
                     schemaField->name);
            return false;
        }
        if( arrayField->children[0]->length < nStrings * arrayField->length )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Field %s: arrayField->children[0]->length < nStrings * arrayField->length",
                     schemaField->name);
            return false;
        }
        const int32_t* offsetsToBytes = (const int32_t*)arrayField->children[0]->buffers[1] + arrayField->offset * nStrings + static_cast<size_t>(arrayField->children[0]->offset);
        const char* bytes = (const char*)arrayField->children[0]->buffers[2];
        numpyArray = PyArray_SimpleNew(1, &dims, NPY_OBJECT);
        for( npy_intp j = 0; j < dims; j++ )
        {
            int32_t maxLength = 1;
            for( int k = 0; k < nStrings; k++ )
            {
                const int32_t nLength = offsetsToBytes[j * nStrings + k + 1] - offsetsToBytes[j * nStrings + k];
                if( nLength > maxLength )
                    maxLength = nLength;
            }

            // create the dtype string
            PyObject *pDTypeString = PyUnicode_FromFormat("S%u", maxLength);
            // out type description object
            PyArray_Descr *pDescr = NULL;
            PyArray_DescrConverter(pDTypeString, &pDescr);
            Py_DECREF(pDTypeString);

            npy_intp nStringsNpyIntp = nStrings;
            PyObject* subArray = PyArray_SimpleNewFromDescr(1, &nStringsNpyIntp, pDescr);
            for( int k = 0; k < nStrings; k++ )
            {
                const int32_t nLength = offsetsToBytes[j * nStrings + k + 1] - offsetsToBytes[j * nStrings + k];
                if( nLength > 0 )
                {
                    memcpy(PyArray_GETPTR1((PyArrayObject *) subArray, k),
                           bytes + offsetsToBytes[j * nStrings + k],
                           nLength);
                }
                if( nLength < maxLength )
                {
                    memset(((char*)PyArray_GETPTR1((PyArrayObject *) subArray, k)) + nLength,
                           0,
                           maxLength - nLength);
                }
            }

            memcpy(PyArray_GETPTR1((PyArrayObject *) numpyArray, j),
                   &subArray,
                   sizeof(PyObject*));
        }
    }
    else if( strcmp(arrowType, "tdD") == 0 &&
             schemaField->n_children == 0 )
    {
        // Date(32) in days since Epoch
        if( arrayField->n_buffers != 2 )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Field %s: arrayField->n_buffers != 2",
                     schemaField->name);
            return false;
        }

        // create the dtype string
        PyObject *pDTypeString = PyUnicode_FromString("datetime64[D]");
        // out type description object
        PyArray_Descr *pDescr = NULL;
        PyArray_DescrConverter(pDTypeString, &pDescr);
        Py_DECREF(pDTypeString);
        CPLAssert(pDescr);

        // create array
        numpyArray = PyArray_SimpleNewFromDescr(1, &dims, pDescr);
        for( npy_intp j = 0; j < dims; j++ )
        {
            *(int64_t*)PyArray_GETPTR1((PyArrayObject *) numpyArray, j) =
                ((int*)arrayField->buffers[1])[j + static_cast<size_t>(arrayField->offset)];
        }
    }
    else if( strcmp(arrowType, "ttm") == 0 &&
             schemaField->n_children == 0 )
    {
        // Time(32) in milliseconds
        if( arrayField->n_buffers != 2 )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Field %s: arrayField->n_buffers != 2",
                     schemaField->name);
            return false;
        }
#if 0
        // create the dtype string
        PyObject *pDTypeString = PyUnicode_FromString("datetime64[ms]");
        // out type description object
        PyArray_Descr *pDescr = NULL;
        PyArray_DescrConverter(pDTypeString, &pDescr);
        Py_DECREF(pDTypeString);
        CPLAssert(pDescr);

        // create array
        numpyArray = PyArray_SimpleNewFromDescr(1, &dims, pDescr);
        for( npy_intp j = 0; j < dims; j++ )
        {
            *(int64_t*)PyArray_GETPTR1((PyArrayObject *) numpyArray, j) =
                ((int*)arrayField->buffers[1])[j + static_cast<size_t>(arrayField->offset)];
        }
#else
        // create array
        numpyArray = PyArray_SimpleNew(1, &dims, NPY_OBJECT);
        for( npy_intp j = 0; j < dims; j++ )
        {
            int timeMs = ((int*)arrayField->buffers[1])[j + static_cast<size_t>(arrayField->offset)];
            PyObject* subObj = PyTime_FromTime((timeMs / 1000) / 3600,
                                               ((timeMs / 1000) % 3600) / 60,
                                               ((timeMs / 1000) % 3600) % 60,
                                               (timeMs % 1000) * 1000);
            memcpy(PyArray_GETPTR1((PyArrayObject *) numpyArray, j),
                   &subObj,
                   sizeof(PyObject*));
        }
#endif
    }
    else if( strcmp(arrowType, "ttu") == 0 &&
             schemaField->n_children == 0 )
    {
        // Time(64) in microseconds
        if( arrayField->n_buffers != 2 )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Field %s: arrayField->n_buffers != 2",
                     schemaField->name);
            return false;
        }

        // create array
        numpyArray = PyArray_SimpleNew(1, &dims, NPY_OBJECT);
        for( npy_intp j = 0; j < dims; j++ )
        {
            const int64_t timeUs = ((int64_t*)arrayField->buffers[1])[j + static_cast<size_t>(arrayField->offset)];
            PyObject* subObj = PyTime_FromTime(static_cast<int>((timeUs / 1000000) / 3600),
                                               static_cast<int>(((timeUs / 1000000) % 3600) / 60),
                                               static_cast<int>(((timeUs / 1000000) % 3600) % 60),
                                               static_cast<int>(timeUs % 1000000));
            memcpy(PyArray_GETPTR1((PyArrayObject *) numpyArray, j),
                   &subObj,
                   sizeof(PyObject*));
        }
    }
    else if( (strncmp(arrowType, "tsm:", 4) == 0 || // DateTime in milliseconds
              strncmp(arrowType, "tsu:", 4) == 0 || // DateTime in microseconds
              strncmp(arrowType, "tsn:", 4) == 0) &&  // DateTime in nanoseconds
             schemaField->n_children == 0 )
    {
        // DateTime(64)
        if( arrayField->n_buffers != 2 )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Field %s: arrayField->n_buffers != 2",
                     schemaField->name);
            return false;
        }

        // create the dtype string
        PyObject *pDTypeString = PyUnicode_FromString(
            strncmp(arrowType, "tsm:", 4) == 0 ? "datetime64[ms]" :
            strncmp(arrowType, "tsu:", 4) == 0 ? "datetime64[us]" :
                                                 "datetime64[ns]");
        // out type description object
        PyArray_Descr *pDescr = NULL;
        PyArray_DescrConverter(pDTypeString, &pDescr);
        Py_DECREF(pDTypeString);
        CPLAssert(pDescr);

        // create array
        numpyArray = PyArray_NewFromDescr(
                    &PyArray_Type, pDescr, 1, &dims, NULL,
                    (int64_t*)arrayField->buffers[1] + static_cast<size_t>(arrayField->offset), 0, NULL);

        /* Keep a reference to the owner object */
#if NPY_API_VERSION >= 0x00000007
        PyArray_SetBaseObject((PyArrayObject *) numpyArray, pointerArrayKeeper);
#else
        PyArray_BASE((PyArrayObject *) numpyArray) = pointerArrayKeeper;
#endif
        Py_INCREF(pointerArrayKeeper);
    }
    else if( strcmp(arrowType, "+s") == 0 )
    {
        for( int iField = 0; iField < arrayField->n_children; iField++ )
        {
            const struct ArrowArray* childArray = arrayField ->children[iField];
            const struct ArrowSchema* childSchema = schemaField->children[iField];

            if( !AddNumpyArrayToDict(dict, childSchema, childArray,
                                     osPrefix + schemaField->name + ".",
                                     pointerArrayKeeper) )
            {
                return false;
            }
        }
    }
    else
    {
        if( strcmp(arrowType, "+l") == 0 || strcmp(arrowType, "+L") == 0 )
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "Field %s: Unhandled arrow type: %s %s",
                     (osPrefix + schemaField->name).c_str(),
                     arrowType, schemaField->children[0]->format);
        }
        else
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "Field %s: Unhandled arrow type: %s",
                     (osPrefix + schemaField->name).c_str(),
                     arrowType);
        }
    }

    if( numpyArray )
    {
        const uint8_t* panNotNulls = (const uint8_t*)arrayField->buffers[0];
        if( panNotNulls && arrayField->null_count != 0 )
        {
            PyObject* maskArray = PyArray_SimpleNew(1, &dims, NPY_BOOL);
            for( npy_intp j = 0; j < dims; j++ )
            {
                size_t srcOffset = static_cast<size_t>(arrayField->offset + j);
                // Inverse convention between arrow not-null bitmap, where
                // 1 means valid, and numpy masks where 1 means invalid
                *(char*)PyArray_GETPTR1((PyArrayObject *) maskArray, j) =
                    ((panNotNulls[srcOffset / 8] & (1 << (srcOffset%8))) == 0) ? 1 : 0;
            }
            PyObject *subdict = PyDict_New();
            PyDict_SetItemString( subdict, "mask", maskArray );
            PyDict_SetItemString( subdict, "data", numpyArray );
            PyDict_SetItemString( dict, (osPrefix + schemaField->name).c_str(), subdict);
            Py_DECREF(maskArray);
            Py_DECREF(subdict);
        }
        else
        {
            PyDict_SetItemString( dict, (osPrefix + schemaField->name).c_str(), numpyArray );
        }
        Py_DECREF(numpyArray);
    }

    return true;
}

/* Internal method used by ogr.Layer.GetNextRecordBatchAsNumpy() */
PyObject* _RecordBatchAsNumpy(VoidPtrAsLong recordBatchPtr,
                              VoidPtrAsLong schemaPtr,
                              PyObject* pointerArrayKeeper)
{
    const struct ArrowSchema* schema = (const struct ArrowSchema* )schemaPtr;
    const struct ArrowArray* array = (const struct ArrowArray* )recordBatchPtr;
    if( strcmp(schema->format, "+s") != 0 )
    {
      CPLError(CE_Failure, CPLE_AppDefined, "schema->format != '+s'");
      Py_RETURN_NONE;
    }
    if( schema->n_children != array->n_children )
    {
      CPLError(CE_Failure, CPLE_AppDefined,
               "schema->n_children(=%d) != array->n_children(=%d)",
               static_cast<int>(schema->n_children),
               static_cast<int>(array->n_children));
      Py_RETURN_NONE;
    }
    PyObject *dict = PyDict_New();
    for( int iField = 0; iField < array->n_children; iField++ )
    {
        const struct ArrowArray* childArray = array->children[iField];
        const struct ArrowSchema* childSchema = schema->children[iField];

        if( !AddNumpyArrayToDict(dict, childSchema, childArray, std::string(), pointerArrayKeeper) )
        {
            Py_DECREF(dict);
            Py_RETURN_NONE;
        }
    }
    return dict;
}

%}

%typemap(in,numinputs=0) (CPLVirtualMemShadow** pvirtualmem, int numpytypemap) (CPLVirtualMemShadow* virtualmem)
{
  $1 = &virtualmem;
  $2 = 0;
}

%typemap(argout) (CPLVirtualMemShadow** pvirtualmem, int numpytypemap)
{
    CPLVirtualMemShadow* virtualmem = *($1);
    void* ptr = CPLVirtualMemGetAddr( virtualmem->vmem );
    /*size_t nsize = CPLVirtualMemGetSize( virtualmem->vmem );*/
    GDALDataType datatype = virtualmem->eBufType;
    int readonly = virtualmem->bReadOnly;
    GIntBig nBufXSize = virtualmem->nBufXSize;
    GIntBig nBufYSize = virtualmem->nBufYSize;
    int nBandCount = virtualmem->nBandCount;
    int bIsBandSequential = virtualmem->bIsBandSequential;
    GDALTileOrganization eTileOrganization = virtualmem->eTileOrganization;
    int nTileXSize = virtualmem->nTileXSize;
    int nTileYSize = virtualmem->nTileYSize;
    int bAuto = virtualmem->bAuto;
    int            nPixelSpace = virtualmem->nPixelSpace; /* if bAuto == TRUE */
    GIntBig        nLineSpace = virtualmem->nLineSpace; /* if bAuto == TRUE */
    int numpytype;

    if( datatype == GDT_CInt16 || datatype == GDT_CInt32 || datatype == GDT_CFloat16 )
    {
        PyErr_SetString( PyExc_RuntimeError, "GDT_CInt16, GDT_CInt32, and GDT_CFloat16 not supported for now" );
        SWIG_fail;
    }

    switch(datatype)
    {
        case GDT_Byte: numpytype = NPY_UBYTE; break;
        case GDT_Int8: numpytype = NPY_INT8; break;
        case GDT_Int16: numpytype = NPY_INT16; break;
        case GDT_UInt16: numpytype = NPY_UINT16; break;
        case GDT_Int32: numpytype = NPY_INT32; break;
        case GDT_UInt32: numpytype = NPY_UINT32; break;
        case GDT_Int64: numpytype = NPY_INT64; break;
        case GDT_UInt64: numpytype = NPY_UINT64; break;
        case GDT_Float16: numpytype = NPY_FLOAT16; break;
        case GDT_Float32: numpytype = NPY_FLOAT32; break;
        case GDT_Float64: numpytype = NPY_FLOAT64; break;
        //case GDT_CInt16: numpytype = NPY_INT16; break;
        //case GDT_CInt32: numpytype = NPY_INT32; break;
        //case GDT_CFloat16: numpytype = NPY_CHALF; break;
        case GDT_CFloat32: numpytype = NPY_CFLOAT; break;
        case GDT_CFloat64: numpytype = NPY_CDOUBLE; break;
        default: numpytype = NPY_UBYTE; break;
    }
    PyArrayObject* ar;
    int flags = (readonly) ? 0x1 : 0x1 | 0x0400;
    int nDataTypeSize = GDALGetDataTypeSize(datatype) / 8;
    if( bAuto )
    {
        if( nBandCount == 1 )
        {
            npy_intp shape[2], stride[2];
            shape[0] = nBufYSize;
            shape[1] = nBufXSize;
            stride[1] = nPixelSpace;
            stride[0] = nLineSpace;
            ar = (PyArrayObject*) PyArray_New(&PyArray_Type, 2, shape,
                    numpytype, stride, ptr, 0, flags , NULL);
        }
        else
        {
            PyErr_SetString( PyExc_RuntimeError, "Code update needed for bAuto and nBandCount > 1 !" );
            SWIG_fail;
        }
    }
    else if( bIsBandSequential >= 0 )
    {
        if( nBandCount == 1 )
        {
            npy_intp shape[2], stride[2];
            shape[0] = nBufYSize;
            shape[1] = nBufXSize;
            stride[1] = nDataTypeSize;
            stride[0] = stride[1] * nBufXSize;
            ar = (PyArrayObject*) PyArray_New(&PyArray_Type, 2, shape,
                    numpytype, stride, ptr, 0, flags , NULL);
        }
        else
        {
            npy_intp shape[3], stride[3];
            if( bIsBandSequential )
            {
                shape[0] = nBandCount;
                shape[1] = nBufYSize;
                shape[2] = nBufXSize;
                stride[2] = nDataTypeSize;
                stride[1] = stride[2] * nBufXSize;
                stride[0] = stride[1] * nBufYSize;
            }
            else
            {
                shape[0] = nBufYSize;
                shape[1] = nBufXSize;
                shape[2] = nBandCount;
                stride[2] = nDataTypeSize;
                stride[1] = stride[2] * nBandCount;
                stride[0] = stride[1] * nBufXSize;
            }
            ar = (PyArrayObject*) PyArray_New(&PyArray_Type, 3, shape,
                    numpytype, stride, ptr, 0, flags , NULL);
        }
    }
    else
    {
        npy_intp nTilesPerRow = static_cast<npy_intp>(DIV_ROUND_UP(nBufXSize, nTileXSize));
        npy_intp nTilesPerCol = static_cast<npy_intp>(DIV_ROUND_UP(nBufYSize, nTileYSize));
        npy_intp shape[5], stride[5];
        if( nBandCount == 1 )
        {
            shape[0] = nTilesPerCol;
            shape[1] = nTilesPerRow;
            shape[2] = nTileYSize;
            shape[3] = nTileXSize;
            stride[3] = nDataTypeSize;
            stride[2] = stride[3] * nTileXSize;
            stride[1] = stride[2] * nTileYSize;
            stride[0] = stride[1] * nTilesPerRow;
            ar = (PyArrayObject*) PyArray_New(&PyArray_Type, 4, shape,
                    numpytype, stride, ptr, 0, flags , NULL);
        }
        else if( eTileOrganization == GTO_TIP )
        {
            shape[0] = nTilesPerCol;
            shape[1] = nTilesPerRow;
            shape[2] = nTileYSize;
            shape[3] = nTileXSize;
            shape[4] = nBandCount;
            stride[4] = nDataTypeSize;
            stride[3] = stride[4] * nBandCount;
            stride[2] = stride[3] * nTileXSize;
            stride[1] = stride[2] * nTileYSize;
            stride[0] = stride[1] * nTilesPerRow;
            ar = (PyArrayObject*) PyArray_New(&PyArray_Type, 5, shape,
                    numpytype, stride, ptr, 0, flags , NULL);
        }
        else if( eTileOrganization == GTO_BIT )
        {
            shape[0] = nTilesPerCol;
            shape[1] = nTilesPerRow;
            shape[2] = nBandCount;
            shape[3] = nTileYSize;
            shape[4] = nTileXSize;
            stride[4] = nDataTypeSize;
            stride[3] = stride[4] * nTileXSize;
            stride[2] = stride[3] * nTileYSize;
            stride[1] = stride[2] * nBandCount;
            stride[0] = stride[1] * nTilesPerRow;
            ar = (PyArrayObject*) PyArray_New(&PyArray_Type, 5, shape,
                    numpytype, stride, ptr, 0, flags , NULL);
        }
        else /* GTO_BSQ */
        {
            shape[0] = nBandCount;
            shape[1] = nTilesPerCol;
            shape[2] = nTilesPerRow;
            shape[3] = nTileYSize;
            shape[4] = nTileXSize;
            stride[4] = nDataTypeSize;
            stride[3] = stride[4] * nTileXSize;
            stride[2] = stride[3] * nTileYSize;
            stride[1] = stride[2] * nTilesPerRow;
            stride[0] = stride[1] * nTilesPerCol;
            ar = (PyArrayObject*) PyArray_New(&PyArray_Type, 5, shape,
                    numpytype, stride, ptr, 0, flags , NULL);
        }
    }

    /* Keep a reference to the VirtualMem object */
%#if NPY_API_VERSION >= 0x00000007
    PyArray_SetBaseObject(ar, $self);
%#else
    PyArray_BASE(ar) = $self;
%#endif
    Py_INCREF($self);
    Py_DECREF($result);
    $result = (PyObject*) ar;
}

%apply Pointer NONNULL {CPLVirtualMemShadow* virtualmem};
%inline %{
    void VirtualMemGetArray(CPLVirtualMemShadow* virtualmem, CPLVirtualMemShadow** pvirtualmem, int numpytypemap)
    {
        *pvirtualmem = virtualmem;
    }
%}
%clear CPLVirtualMemShadow* virtualmem;

%feature( "kwargs" ) RATValuesIONumPyWrite;
%inline %{
  // need different functions for read and write
  // since reading strings requires us to know the
  // length of the longest string before creating array
  CPLErr RATValuesIONumPyWrite( GDALRasterAttributeTableShadow* poRAT, int nField, int nStart,
                       PyArrayObject *psArray) {

    if( PyArray_NDIM(psArray) != 1 )
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                  "Illegal numpy array rank %d.\n",
                  PyArray_NDIM(psArray) );
        return CE_Failure;
    }
    if( PyArray_DIM(psArray, 0) > INT_MAX )
    {
        CPLError( CE_Failure, CPLE_NotSupported,
                  "Too big array dimension");
        return CE_Failure;
    }

    int nLength = static_cast<int>(PyArray_DIM(psArray, 0));
    int nType = PyArray_TYPE(psArray);
    CPLErr retval = CE_None;

    if( nType == NPY_INT32 )
    {
        retval = GDALRATValuesIOAsInteger(poRAT, GF_Write, nField, nStart, nLength,
                        (int*)PyArray_DATA(psArray) );
    }
    else if( nType == NPY_DOUBLE )
    {
        retval = GDALRATValuesIOAsDouble(poRAT, GF_Write, nField, nStart, nLength,
                        (double*)PyArray_DATA(psArray) );
    }
    else if( nType == NPY_STRING )
    {
        // max size of string
        const size_t nMaxLen = PyArray_ITEMSIZE(psArray);
        char *pszBuffer = (char*)VSIMalloc(nMaxLen+1);
        if (!pszBuffer)
        {
            CPLError( CE_Failure, CPLE_OutOfMemory,
                      "Out of memory in RATValuesIONumPyWrite()" );
            return CE_Failure;
        }
        // make sure there is a null char on the end
        // as there won't be if this string is the maximum size
        pszBuffer[nMaxLen] = '\0';

        // have to convert array of strings to a char **
        char **papszStringData = (char**)CPLCalloc(sizeof(char*), nLength);

        // we can't just use the memory location in the array
        // since long strings won't be null terminated
        for( int i = 0; i < nLength; i++ )
        {
            strncpy(pszBuffer, (char*)PyArray_GETPTR1(psArray, i), nMaxLen);
            papszStringData[i] = CPLStrdup(pszBuffer);
        }
        CPLFree(pszBuffer);

        retval = GDALRATValuesIOAsString(poRAT, GF_Write, nField, nStart, nLength,
                                            papszStringData);

        for( int i = 0; i < nLength; i++ )
        {
            CPLFree(papszStringData[i]);
        }
        CPLFree(papszStringData);
    }
    else
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                  "Illegal numpy array type %d.\n",
                  nType );
        return CE_Failure;
    }
    return retval;
  }
%}

%feature( "kwargs" ) RATValuesIONumPyRead;
%inline %{
  // need different functions for read and write
  // since reading strings requires us to know the
  // length of the longest string before creating array
  PyObject *RATValuesIONumPyRead( GDALRasterAttributeTableShadow* poRAT, int nField, int nStart,
                       int nLength) {

    GDALRATFieldType colType = GDALRATGetTypeOfCol(poRAT, nField);
    npy_intp dims = nLength;
    PyObject *pOutArray = NULL;
    if( colType == GFT_Integer )
    {
        pOutArray = PyArray_SimpleNew(1, &dims, NPY_INT32);
        if( GDALRATValuesIOAsInteger(poRAT, GF_Read, nField, nStart, nLength,
                        (int*)PyArray_DATA((PyArrayObject *) pOutArray)) != CE_None)
        {
            Py_DECREF(pOutArray);
            Py_RETURN_NONE;
        }
    }
    else if( colType == GFT_Real )
    {
        pOutArray = PyArray_SimpleNew(1, &dims, NPY_DOUBLE);
        if( GDALRATValuesIOAsDouble(poRAT, GF_Read, nField, nStart, nLength,
                        (double*)PyArray_DATA((PyArrayObject *) pOutArray)) != CE_None)
        {
            Py_DECREF(pOutArray);
            Py_RETURN_NONE;
        }
    }
    else if( colType == GFT_String )
    {
        // must read the data first to work out max size
        // of strings to create array
        int n;
        char **papszStringList = (char**)CPLCalloc(sizeof(char*), nLength);
        if( GDALRATValuesIOAsString(poRAT, GF_Read, nField, nStart, nLength, papszStringList) != CE_None )
        {
            CPLFree(papszStringList);
            Py_RETURN_NONE;
        }
        int nMaxLen = 0, nLen;
        for( n = 0; n < nLength; n++ )
        {
            // note strlen doesn't include null char
            // but that is what numpy expects so all good
            nLen = static_cast<int>(strlen(papszStringList[n]));
            if( nLen > nMaxLen )
                nMaxLen = nLen;
        }
        int bZeroLength = FALSE;
        // numpy can't deal with zero length strings
        if( nMaxLen == 0 )
        {
            nMaxLen = 1;
            bZeroLength = TRUE;
        }

        // create the dtype string
        PyObject *pDTypeString = PyUnicode_FromFormat("S%d", nMaxLen);
        // out type description object
        PyArray_Descr *pDescr;
        PyArray_DescrConverter(pDTypeString, &pDescr);
        Py_DECREF(pDTypeString);

        // create array
        pOutArray = PyArray_SimpleNewFromDescr(1, &dims, pDescr);

        // copy data in
        if( !bZeroLength )
        {
            for( n = 0; n < nLength; n++ )
            {
                // we use strncpy so that we don't go over nMaxLen
                // which we would if the null char is copied
                // (which we don't want as numpy 'knows' to interpret the string as nMaxLen long)
                strncpy((char*)PyArray_GETPTR1((PyArrayObject *) pOutArray, n), papszStringList[n], nMaxLen);
            }
        }
        else
        {
            // so there isn't rubbish in the 1 char strings
            PyArray_FILLWBYTE((PyArrayObject *) pOutArray, 0);
        }

        // free strings
        for( n = 0; n < nLength; n++ )
        {
            CPLFree(papszStringList[n]);
        }
        CPLFree(papszStringList);
    }
    return pOutArray;
  }
%}

%pythoncode %{
import numpy

from osgeo import gdalconst
from osgeo import gdal
gdal.AllRegister()

codes = {gdalconst.GDT_Byte: numpy.uint8,
         gdalconst.GDT_Int8: numpy.int8,
         gdalconst.GDT_UInt16: numpy.uint16,
         gdalconst.GDT_Int16: numpy.int16,
         gdalconst.GDT_UInt32: numpy.uint32,
         gdalconst.GDT_Int32: numpy.int32,
         gdalconst.GDT_UInt64: numpy.uint64,
         gdalconst.GDT_Int64: numpy.int64,
         gdalconst.GDT_Float16: numpy.float16,
         gdalconst.GDT_Float32: numpy.float32,
         gdalconst.GDT_Float64: numpy.float64,
         gdalconst.GDT_CInt16: numpy.complex64,
         gdalconst.GDT_CInt32: numpy.complex64,
         gdalconst.GDT_CFloat16: numpy.complex64,
         gdalconst.GDT_CFloat32: numpy.complex64,
         gdalconst.GDT_CFloat64: numpy.complex128}

np_class_to_gdal_code = { v : k for k, v in codes.items() }
# since several things map to complex64 we must carefully select
# the opposite that is an exact match (ticket 1518)
np_class_to_gdal_code[numpy.complex64] = gdalconst.GDT_CFloat32
# also recognize numpy bool arrays
np_class_to_gdal_code[numpy.bool_] = gdalconst.GDT_Byte
np_dtype_to_gdal_code = { numpy.dtype(k) : v for k, v in np_class_to_gdal_code.items() }

def OpenArray(array, prototype_ds=None, interleave='band'):

    interleave = interleave.lower()
    if interleave == 'band':
        interleave = True
    elif interleave == 'pixel':
        interleave = False
    else:
        raise ValueError('Interleave should be band or pixel')

    ds = OpenNumPyArray(array, interleave)

    if ds is not None and prototype_ds is not None:
        if type(prototype_ds).__name__ == 'str':
            prototype_ds = gdal.Open(prototype_ds)
        if prototype_ds is not None:
            CopyDatasetInfo(prototype_ds, ds)

    return ds

def flip_code(code):
    try:
        return NumericTypeCodeToGDALTypeCode(code)
    except TypeError:
        return GDALTypeCodeToNumericTypeCode(code)

def NumericTypeCodeToGDALTypeCode(numeric_type):
    if isinstance(numeric_type, type):
        return np_class_to_gdal_code.get(numeric_type, None)
    elif isinstance(numeric_type, numpy.dtype):
        return np_dtype_to_gdal_code.get(numeric_type, None)
    raise TypeError("Input must be a type")

def GDALTypeCodeToNumericTypeCode(gdal_code):
    return codes.get(gdal_code, None)

def _RaiseException():
    if gdal.GetUseExceptions():
        raise RuntimeError(gdal.GetLastErrorMsg())

def LoadFile(filename, xoff=0, yoff=0, xsize=None, ysize=None,
             buf_xsize=None, buf_ysize=None, buf_type=None,
             resample_alg=gdal.GRIORA_NearestNeighbour,
             callback=None, callback_data=None, interleave='band',
             band_list=None):
    ds = gdal.Open(filename)
    if ds is None:
        raise ValueError("Can't open "+filename+"\n\n"+gdal.GetLastErrorMsg())

    return DatasetReadAsArray(ds, xoff, yoff, xsize, ysize,
                              buf_xsize=buf_xsize, buf_ysize=buf_ysize, buf_type=buf_type,
                              resample_alg=resample_alg,
                              callback=callback, callback_data=callback_data,
                              interleave=interleave,
                              band_list=band_list)

def SaveArray(src_array, filename, format="GTiff", prototype=None, interleave='band'):
    driver = gdal.GetDriverByName(format)
    if driver is None:
        raise ValueError("Can't find driver "+format)

    return driver.CreateCopy(filename, OpenArray(src_array, prototype, interleave))

def _to_primitive_type(x):
    """Converts an object with a __int__ or __float__ method to the
       corresponding primitive type, or return x."""
    if x is None:
        return x
    if hasattr(x, "__int__"):
        if hasattr(x, "is_integer") and x.is_integer():
            return int(x)
        elif not hasattr(x, "__float__"):
            return int(x)
        else:
            ret = float(x)
            if ret == int(ret):
                ret = int(ret)
            return ret
    elif hasattr(x, "__float__"):
        return float(x)
    return x

def DatasetReadAsArray(ds, xoff=0, yoff=0, win_xsize=None, win_ysize=None, buf_obj=None,
                       buf_xsize=None, buf_ysize=None, buf_type=None,
                       resample_alg=gdal.GRIORA_NearestNeighbour,
                       callback=None, callback_data=None, interleave='band',
                       band_list=None):
    """Pure python implementation of reading a chunk of a GDAL file
    into a numpy array.  Used by the gdal.Dataset.ReadAsArray method."""

    if win_xsize is None:
        win_xsize = ds.RasterXSize
    if win_ysize is None:
        win_ysize = ds.RasterYSize

    xoff = _to_primitive_type(xoff)
    yoff = _to_primitive_type(yoff)
    win_xsize = _to_primitive_type(win_xsize)
    win_ysize = _to_primitive_type(win_ysize)
    buf_xsize = _to_primitive_type(buf_xsize)
    buf_ysize = _to_primitive_type(buf_ysize)

    if band_list is None:
        band_list = list(range(1, ds.RasterCount + 1))

    interleave = interleave.lower()
    if interleave == 'band':
        interleave = True
        xdim = 2
        ydim = 1
        banddim = 0
    elif interleave == 'pixel':
        interleave = False
        xdim = 1
        ydim = 0
        banddim = 2
    else:
        raise ValueError('Interleave should be band or pixel')

    nbands = len(band_list)
    if nbands == 0:
        return None

    if nbands == 1:
        return BandReadAsArray(ds.GetRasterBand(band_list[0]), xoff, yoff, win_xsize, win_ysize,
                               buf_xsize=buf_xsize, buf_ysize=buf_ysize, buf_type=buf_type,
                               buf_obj=buf_obj,
                               resample_alg=resample_alg,
                               callback=callback,
                               callback_data=callback_data)

    if buf_obj is None:
        if buf_xsize is None:
            buf_xsize = win_xsize
        if buf_ysize is None:
            buf_ysize = win_ysize
        if buf_type is None:
            buf_type = ds.GetRasterBand(band_list[0]).DataType
            for idx in range(1, nbands):
                band_index = band_list[idx]
                if buf_type != ds.GetRasterBand(band_index).DataType:
                    buf_type = gdalconst.GDT_Float32

        typecode = GDALTypeCodeToNumericTypeCode(buf_type)
        if typecode is None:
            buf_type = gdalconst.GDT_Float32
            typecode = numpy.float32
        else:
            buf_type = NumericTypeCodeToGDALTypeCode(typecode)

        if buf_type == gdalconst.GDT_Byte:
            band = ds.GetRasterBand(1)
            band._EnablePixelTypeSignedByteWarning(False)
            if band.GetMetadataItem('PIXELTYPE', 'IMAGE_STRUCTURE') == 'SIGNEDBYTE':
                typecode = numpy.int8
            band._EnablePixelTypeSignedByteWarning(True)
        buf_shape = (nbands, buf_ysize, buf_xsize) if interleave else (buf_ysize, buf_xsize, nbands)
        buf_obj = numpy.empty(buf_shape, dtype=typecode)

    else:
        if len(buf_obj.shape) != 3:
            raise ValueError('Array should have 3 dimensions')

        shape_buf_xsize = buf_obj.shape[xdim]
        shape_buf_ysize = buf_obj.shape[ydim]
        if buf_xsize is not None and buf_xsize != shape_buf_xsize:
            raise ValueError('Specified buf_xsize not consistent with array shape')
        if buf_ysize is not None and buf_ysize != shape_buf_ysize:
            raise ValueError('Specified buf_ysize not consistent with array shape')
        if buf_obj.shape[banddim] != nbands:
            raise ValueError('Dimension %d of array should have size %d to store bands)' % (banddim, nbands))

        datatype = NumericTypeCodeToGDALTypeCode(buf_obj.dtype.type)
        if not datatype:
            raise ValueError("array does not have corresponding GDAL data type")
        if buf_type is not None and buf_type != datatype:
            raise ValueError("Specified buf_type not consistent with array type")
        buf_type = datatype

    if DatasetIONumPy(ds, 0, xoff, yoff, win_xsize, win_ysize,
                      buf_obj, buf_type, resample_alg, callback, callback_data,
                      interleave, band_list) != 0:
        _RaiseException()
        return None

    return buf_obj


def DatasetWriteArray(ds, array, xoff=0, yoff=0,
                      band_list=None,
                      interleave='band',
                      resample_alg=gdal.GRIORA_NearestNeighbour,
                      callback=None, callback_data=None):
    """Pure python implementation of writing a chunk of a GDAL file
    from a numpy array.  Used by the gdal.Dataset.WriteArray method."""

    xoff = _to_primitive_type(xoff)
    yoff = _to_primitive_type(yoff)

    if band_list is None:
        band_list = list(range(1, ds.RasterCount + 1))

    interleave = interleave.lower()
    if interleave == 'band':
        interleave = True
        xdim = 2
        ydim = 1
        banddim = 0
    elif interleave == 'pixel':
        interleave = False
        xdim = 1
        ydim = 0
        banddim = 2
    else:
        raise ValueError('Interleave should be band or pixel')

    if len(band_list) == 1:
        if array is None or (len(array.shape) != 2 and len(array.shape) != 3):
            raise ValueError("expected array of dim 2 or 3")
        if len(array.shape) == 3:
            if array.shape[banddim] != 1:
                raise ValueError("expected size of dimension %d should be 1" % banddim)
            array = array[banddim]

        return BandWriteArray(ds.GetRasterBand(band_list[0]),
                              array,
                              xoff=xoff, yoff=yoff, resample_alg=resample_alg,
                              callback=callback, callback_data=callback_data)

    if array is None or len(array.shape) != 3:
        raise ValueError("expected array of dim 3")

    xsize = array.shape[xdim]
    ysize = array.shape[ydim]

    if xsize + xoff > ds.RasterXSize or ysize + yoff > ds.RasterYSize:
        raise ValueError("array larger than output file, or offset off edge")
    if array.shape[banddim] != len(band_list):
        raise ValueError('Dimension %d of array should have size %d to store bands)' % (banddim, len(band_list)))

    datatype = NumericTypeCodeToGDALTypeCode(array.dtype.type)

    # if we receive some odd type, like int64, try casting to a very
    # generic type we do support (#2285)
    if not datatype:
        gdal.Debug('gdal_array', 'force array to float64')
        array = array.astype(numpy.float64)
        datatype = NumericTypeCodeToGDALTypeCode(array.dtype.type)

    if not datatype:
        raise ValueError("array does not have corresponding GDAL data type")

    ret = DatasetIONumPy(ds, 1, xoff, yoff, xsize, ysize,
                         array, datatype, resample_alg, callback, callback_data,
                         interleave, band_list)
    if ret != 0:
        _RaiseException()
    return ret


def BandReadAsArray(band, xoff=0, yoff=0, win_xsize=None, win_ysize=None,
                    buf_xsize=None, buf_ysize=None, buf_type=None, buf_obj=None,
                    resample_alg=gdal.GRIORA_NearestNeighbour,
                    callback=None, callback_data=None):
    """Pure python implementation of reading a chunk of a GDAL file
    into a numpy array.  Used by the gdal.Band.ReadAsArray method."""

    if win_xsize is None:
        win_xsize = band.XSize
    if win_ysize is None:
        win_ysize = band.YSize

    xoff = _to_primitive_type(xoff)
    yoff = _to_primitive_type(yoff)
    win_xsize = _to_primitive_type(win_xsize)
    win_ysize = _to_primitive_type(win_ysize)
    buf_xsize = _to_primitive_type(buf_xsize)
    buf_ysize = _to_primitive_type(buf_ysize)

    if buf_obj is None:
        if buf_xsize is None:
            buf_xsize = win_xsize
        if buf_ysize is None:
            buf_ysize = win_ysize
        if buf_type is None:
            buf_type = band.DataType

        typecode = GDALTypeCodeToNumericTypeCode(buf_type)
        if typecode is None:
            buf_type = gdalconst.GDT_Float32
            typecode = numpy.float32
        else:
            buf_type = NumericTypeCodeToGDALTypeCode(typecode)

        if buf_type == gdalconst.GDT_Byte:
            band._EnablePixelTypeSignedByteWarning(False)
            if band.GetMetadataItem('PIXELTYPE', 'IMAGE_STRUCTURE') == 'SIGNEDBYTE':
                typecode = numpy.int8
            band._EnablePixelTypeSignedByteWarning(True)
        buf_obj = numpy.empty([buf_ysize, buf_xsize], dtype=typecode)

    else:
        if len(buf_obj.shape) not in (2, 3):
            raise ValueError("expected array of dimension 2 or 3")

        if len(buf_obj.shape) == 2:
            shape_buf_xsize = buf_obj.shape[1]
            shape_buf_ysize = buf_obj.shape[0]
        else:
            if buf_obj.shape[0] != 1:
                raise ValueError("expected size of first dimension should be 0")
            shape_buf_xsize = buf_obj.shape[2]
            shape_buf_ysize = buf_obj.shape[1]
        if buf_xsize is not None and buf_xsize != shape_buf_xsize:
            raise ValueError('Specified buf_xsize not consistent with array shape')
        if buf_ysize is not None and buf_ysize != shape_buf_ysize:
            raise ValueError('Specified buf_ysize not consistent with array shape')

        datatype = NumericTypeCodeToGDALTypeCode(buf_obj.dtype.type)
        if not datatype:
            raise ValueError("array does not have corresponding GDAL data type")
        if buf_type is not None and buf_type != datatype:
            raise ValueError("Specified buf_type not consistent with array type")
        buf_type = datatype

    if BandRasterIONumPy(band, 0, xoff, yoff, win_xsize, win_ysize,
                         buf_obj, buf_type, resample_alg, callback, callback_data) != 0:
        _RaiseException()
        return None

    return buf_obj

def BandWriteArray(band, array, xoff=0, yoff=0,
                   resample_alg=gdal.GRIORA_NearestNeighbour,
                   callback=None, callback_data=None):
    """Pure python implementation of writing a chunk of a GDAL file
    from a numpy array.  Used by the gdal.Band.WriteArray method."""

    if array is None or len(array.shape) != 2:
        raise ValueError("expected array of dim 2")

    xoff = _to_primitive_type(xoff)
    yoff = _to_primitive_type(yoff)

    xsize = array.shape[1]
    ysize = array.shape[0]

    if xsize + xoff > band.XSize or ysize + yoff > band.YSize:
        raise ValueError("array larger than output file, or offset off edge")

    datatype = NumericTypeCodeToGDALTypeCode(array.dtype.type)

    # if we receive some odd type, like int64, try casting to a very
    # generic type we do support (#2285)
    if not datatype:
        gdal.Debug('gdal_array', 'force array to float64')
        array = array.astype(numpy.float64)
        datatype = NumericTypeCodeToGDALTypeCode(array.dtype.type)

    if not datatype:
        raise ValueError("array does not have corresponding GDAL data type")

    ret = BandRasterIONumPy(band, 1, xoff, yoff, xsize, ysize,
                             array, datatype, resample_alg, callback, callback_data)
    if ret != 0:
        _RaiseException()
    return ret

def _ExtendedDataTypeToNumPyDataType(dt):
    klass = dt.GetClass()

    if klass == gdal.GEDTC_STRING:
        return numpy.bytes_, dt

    if klass == gdal.GEDTC_NUMERIC:
        buf_type = dt.GetNumericDataType()
        typecode = GDALTypeCodeToNumericTypeCode(buf_type)
        if typecode is None:
            typecode = numpy.float32
            dt = gdal.ExtendedDataType.Create(gdal.GDT_Float32)
        else:
            dt = gdal.ExtendedDataType.Create(NumericTypeCodeToGDALTypeCode(typecode))
        return typecode, dt

    assert klass == gdal.GEDTC_COMPOUND
    names = []
    formats = []
    offsets = []
    for comp in dt.GetComponents():
        names.append(comp.GetName())
        typecode, subdt = _ExtendedDataTypeToNumPyDataType(comp.GetType())
        if subdt != comp.GetType():
            raise Exception("Incompatible datatype")
        formats.append(typecode)
        offsets.append(comp.GetOffset())

    return numpy.dtype({'names': names,
                        'formats': formats,
                        'offsets': offsets,
                        'itemsize': dt.GetSize()}), dt

def ExtendedDataTypeToNumPyDataType(dt):
    typecode, _ = _ExtendedDataTypeToNumPyDataType(dt)
    return typecode

def MDArrayReadAsArray(mdarray,
                        array_start_idx = None,
                        count = None,
                        array_step = None,
                        buffer_datatype = None,
                        buf_obj = None):
    if not array_start_idx:
        array_start_idx = [0] * mdarray.GetDimensionCount()
    if not count:
        count = [ dim.GetSize() for dim in mdarray.GetDimensions() ]
    if not array_step:
        array_step = [1] * mdarray.GetDimensionCount()

    if buf_obj is None:
        if not buffer_datatype:
            buffer_datatype = mdarray.GetDataType()
        typecode, buffer_datatype = _ExtendedDataTypeToNumPyDataType(buffer_datatype)
        buf_obj = numpy.empty(count, dtype=typecode)
    else:
        datatype = NumericTypeCodeToGDALTypeCode(buf_obj.dtype.type)
        if not datatype:
            raise ValueError("array does not have corresponding GDAL data type")

        buffer_datatype = gdal.ExtendedDataType.Create(datatype)

    ret = MDArrayIONumPy(False, mdarray, buf_obj, array_start_idx, array_step, buffer_datatype)
    if ret != 0:
        _RaiseException()
    return buf_obj

def MDArrayWriteArray(mdarray, array,
                        array_start_idx = None,
                        array_step = None):
    if not array_start_idx:
        array_start_idx = [0] * mdarray.GetDimensionCount()
    if not array_step:
        array_step = [1] * mdarray.GetDimensionCount()

    buffer_datatype = mdarray.GetDataType()
    typecode = ExtendedDataTypeToNumPyDataType(buffer_datatype)
    if array.dtype != typecode:
        datatype = NumericTypeCodeToGDALTypeCode(array.dtype.type)

        # if we receive some odd type, like int64, try casting to a very
        # generic type we do support (#2285)
        if not datatype:
            gdal.Debug('gdal_array', 'force array to float64')
            array = array.astype(numpy.float64)
            datatype = NumericTypeCodeToGDALTypeCode(array.dtype.type)

        if not datatype:
            raise ValueError("array does not have corresponding GDAL data type")

        buffer_datatype = gdal.ExtendedDataType.Create(datatype)

    ret = MDArrayIONumPy(True, mdarray, array, array_start_idx, array_step, buffer_datatype)
    if ret != 0:
        _RaiseException()
    return ret

def RATWriteArray(rat, array, field, start=0):
    """
    Pure Python implementation of writing a chunk of the RAT
    from a numpy array. Type of array is coerced to one of the types
    (int, double, string) supported. Called from RasterAttributeTable.WriteArray
    """
    if array is None:
        raise ValueError("Expected array of dim 1")

    # if not the array type convert it to handle lists etc
    if not isinstance(array, numpy.ndarray):
        array = numpy.array(array)

    if array.ndim != 1:
        raise ValueError("Expected array of dim 1")

    if (start + array.size) > rat.GetRowCount():
        raise ValueError("Array too big to fit into RAT from start position")

    if numpy.issubdtype(array.dtype, numpy.integer):
        # is some type of integer - coerce to standard int
        # TODO: must check this is fine on all platforms
        # confusingly numpy.int 64 bit even if native type 32 bit
        array = array.astype(numpy.int32)
    elif numpy.issubdtype(array.dtype, numpy.floating):
        # is some type of floating point - coerce to double
        array = array.astype(numpy.double)
    elif numpy.issubdtype(array.dtype, numpy.character):
        # cast away any kind of Unicode etc
        array = array.astype(bytes)
    else:
        raise ValueError("Array not of a supported type (integer, double or string)")

    ret = RATValuesIONumPyWrite(rat, field, start, array)
    if ret != 0:
        _RaiseException()
    return ret

def RATReadArray(rat, field, start=0, length=None):
    """
    Pure Python implementation of reading a chunk of the RAT
    into a numpy array. Called from RasterAttributeTable.ReadAsArray
    """
    if length is None:
        length = rat.GetRowCount() - start

    ret = RATValuesIONumPyRead(rat, field, start, length)
    if ret is None:
        _RaiseException()
    return ret

def CopyDatasetInfo(src, dst, xoff=0, yoff=0):
    """
    Copy georeferencing information and metadata from one dataset to another.
    src: input dataset
    dst: output dataset - It can be a ROI -
    xoff, yoff:  dst's offset with respect to src in pixel/line.

    Notes: Destination dataset must have update access.  Certain formats
           do not support creation of geotransforms and/or gcps.

    """

    dst.SetMetadata(src.GetMetadata())



    #Check for geo transform
    gt = src.GetGeoTransform()
    if gt != (0, 1, 0, 0, 0, 1):
        dst.SetProjection(src.GetProjectionRef())

        if xoff == 0 and yoff == 0:
            dst.SetGeoTransform(gt)
        else:
            ngt = [gt[0], gt[1], gt[2], gt[3], gt[4], gt[5]]
            ngt[0] = gt[0] + xoff*gt[1] + yoff*gt[2]
            ngt[3] = gt[3] + xoff*gt[4] + yoff*gt[5]
            dst.SetGeoTransform((ngt[0], ngt[1], ngt[2], ngt[3], ngt[4], ngt[5]))

    #Check for GCPs
    elif src.GetGCPCount() > 0:

        if (xoff == 0) and (yoff == 0):
            dst.SetGCPs(src.GetGCPs(), src.GetGCPProjection())
        else:
            gcps = src.GetGCPs()
            #Shift gcps
            new_gcps = []
            for gcp in gcps:
                ngcp = gdal.GCP()
                ngcp.GCPX = gcp.GCPX
                ngcp.GCPY = gcp.GCPY
                ngcp.GCPZ = gcp.GCPZ
                ngcp.GCPPixel = gcp.GCPPixel - xoff
                ngcp.GCPLine = gcp.GCPLine - yoff
                ngcp.Info = gcp.Info
                ngcp.Id = gcp.Id
                new_gcps.append(ngcp)

            try:
                dst.SetGCPs(new_gcps, src.GetGCPProjection())
            except:
                print("Failed to set GCPs")
                return

    return
%}

#ifdef SWIGPYTHON
%thread;
#endif
