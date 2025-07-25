/******************************************************************************
 *
 * Name:     gdalmultidim.cpp
 * Project:  GDAL Core
 * Purpose:  GDAL Core C++/Private implementation for multidimensional support
 * Author:   Even Rouault <even.rouault at spatialys.com>
 *
 ******************************************************************************
 * Copyright (c) 2019, Even Rouault <even.rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include <assert.h>
#include <algorithm>
#include <limits>
#include <list>
#include <queue>
#include <set>
#include <utility>
#include <time.h>

#include <cmath>
#include <ctype.h>  // isalnum

#include "cpl_error_internal.h"
#include "cpl_float.h"
#include "gdal_priv.h"
#include "gdal_pam.h"
#include "gdal_rat.h"
#include "gdal_utils.h"
#include "cpl_safemaths.hpp"
#include "memmultidim.h"
#include "ogrsf_frmts.h"
#include "gdalmultidim_priv.h"

#if defined(__clang__) || defined(_MSC_VER)
#define COMPILER_WARNS_ABOUT_ABSTRACT_VBASE_INIT
#endif

/************************************************************************/
/*                       GDALMDArrayUnscaled                            */
/************************************************************************/

class GDALMDArrayUnscaled final : public GDALPamMDArray
{
  private:
    std::shared_ptr<GDALMDArray> m_poParent{};
    const GDALExtendedDataType m_dt;
    bool m_bHasNoData;
    const double m_dfScale;
    const double m_dfOffset;
    std::vector<GByte> m_abyRawNoData{};

  protected:
    explicit GDALMDArrayUnscaled(const std::shared_ptr<GDALMDArray> &poParent,
                                 double dfScale, double dfOffset,
                                 double dfOverriddenDstNodata, GDALDataType eDT)
        : GDALAbstractMDArray(std::string(),
                              "Unscaled view of " + poParent->GetFullName()),
          GDALPamMDArray(
              std::string(), "Unscaled view of " + poParent->GetFullName(),
              GDALPamMultiDim::GetPAM(poParent), poParent->GetContext()),
          m_poParent(std::move(poParent)),
          m_dt(GDALExtendedDataType::Create(eDT)),
          m_bHasNoData(m_poParent->GetRawNoDataValue() != nullptr),
          m_dfScale(dfScale), m_dfOffset(dfOffset)
    {
        m_abyRawNoData.resize(m_dt.GetSize());
        const auto eNonComplexDT =
            GDALGetNonComplexDataType(m_dt.GetNumericDataType());
        GDALCopyWords64(
            &dfOverriddenDstNodata, GDT_Float64, 0, m_abyRawNoData.data(),
            eNonComplexDT, GDALGetDataTypeSizeBytes(eNonComplexDT),
            GDALDataTypeIsComplex(m_dt.GetNumericDataType()) ? 2 : 1);
    }

    bool IRead(const GUInt64 *arrayStartIdx, const size_t *count,
               const GInt64 *arrayStep, const GPtrDiff_t *bufferStride,
               const GDALExtendedDataType &bufferDataType,
               void *pDstBuffer) const override;

    bool IWrite(const GUInt64 *arrayStartIdx, const size_t *count,
                const GInt64 *arrayStep, const GPtrDiff_t *bufferStride,
                const GDALExtendedDataType &bufferDataType,
                const void *pSrcBuffer) override;

    bool IAdviseRead(const GUInt64 *arrayStartIdx, const size_t *count,
                     CSLConstList papszOptions) const override
    {
        return m_poParent->AdviseRead(arrayStartIdx, count, papszOptions);
    }

  public:
    static std::shared_ptr<GDALMDArrayUnscaled>
    Create(const std::shared_ptr<GDALMDArray> &poParent, double dfScale,
           double dfOffset, double dfDstNodata, GDALDataType eDT)
    {
        auto newAr(std::shared_ptr<GDALMDArrayUnscaled>(new GDALMDArrayUnscaled(
            poParent, dfScale, dfOffset, dfDstNodata, eDT)));
        newAr->SetSelf(newAr);
        return newAr;
    }

    bool IsWritable() const override
    {
        return m_poParent->IsWritable();
    }

    const std::string &GetFilename() const override
    {
        return m_poParent->GetFilename();
    }

    const std::vector<std::shared_ptr<GDALDimension>> &
    GetDimensions() const override
    {
        return m_poParent->GetDimensions();
    }

    const GDALExtendedDataType &GetDataType() const override
    {
        return m_dt;
    }

    const std::string &GetUnit() const override
    {
        return m_poParent->GetUnit();
    }

    std::shared_ptr<OGRSpatialReference> GetSpatialRef() const override
    {
        return m_poParent->GetSpatialRef();
    }

    const void *GetRawNoDataValue() const override
    {
        return m_bHasNoData ? m_abyRawNoData.data() : nullptr;
    }

    bool SetRawNoDataValue(const void *pRawNoData) override
    {
        m_bHasNoData = true;
        memcpy(m_abyRawNoData.data(), pRawNoData, m_dt.GetSize());
        return true;
    }

    std::vector<GUInt64> GetBlockSize() const override
    {
        return m_poParent->GetBlockSize();
    }

    std::shared_ptr<GDALAttribute>
    GetAttribute(const std::string &osName) const override
    {
        return m_poParent->GetAttribute(osName);
    }

    std::vector<std::shared_ptr<GDALAttribute>>
    GetAttributes(CSLConstList papszOptions = nullptr) const override
    {
        return m_poParent->GetAttributes(papszOptions);
    }

    bool SetUnit(const std::string &osUnit) override
    {
        return m_poParent->SetUnit(osUnit);
    }

    bool SetSpatialRef(const OGRSpatialReference *poSRS) override
    {
        return m_poParent->SetSpatialRef(poSRS);
    }

    std::shared_ptr<GDALAttribute>
    CreateAttribute(const std::string &osName,
                    const std::vector<GUInt64> &anDimensions,
                    const GDALExtendedDataType &oDataType,
                    CSLConstList papszOptions = nullptr) override
    {
        return m_poParent->CreateAttribute(osName, anDimensions, oDataType,
                                           papszOptions);
    }
};

/************************************************************************/
/*                         ~GDALIHasAttribute()                         */
/************************************************************************/

GDALIHasAttribute::~GDALIHasAttribute() = default;

/************************************************************************/
/*                            GetAttribute()                            */
/************************************************************************/

/** Return an attribute by its name.
 *
 * If the attribute does not exist, nullptr should be silently returned.
 *
 * @note Driver implementation: this method will fallback to
 * GetAttributeFromAttributes() is not explicitly implemented
 *
 * Drivers known to implement it for groups and arrays: MEM, netCDF.
 *
 * This is the same as the C function GDALGroupGetAttribute() or
 * GDALMDArrayGetAttribute().
 *
 * @param osName Attribute name
 * @return the attribute, or nullptr if it does not exist or an error occurred.
 */
std::shared_ptr<GDALAttribute>
GDALIHasAttribute::GetAttribute(const std::string &osName) const
{
    return GetAttributeFromAttributes(osName);
}

/************************************************************************/
/*                       GetAttributeFromAttributes()                   */
/************************************************************************/

/** Possible fallback implementation for GetAttribute() using GetAttributes().
 */
std::shared_ptr<GDALAttribute>
GDALIHasAttribute::GetAttributeFromAttributes(const std::string &osName) const
{
    auto attrs(GetAttributes());
    for (const auto &attr : attrs)
    {
        if (attr->GetName() == osName)
            return attr;
    }
    return nullptr;
}

/************************************************************************/
/*                           GetAttributes()                            */
/************************************************************************/

/** Return the list of attributes contained in a GDALMDArray or GDALGroup.
 *
 * If the attribute does not exist, nullptr should be silently returned.
 *
 * @note Driver implementation: optionally implemented. If implemented,
 * GetAttribute() should also be implemented.
 *
 * Drivers known to implement it for groups and arrays: MEM, netCDF.
 *
 * This is the same as the C function GDALGroupGetAttributes() or
 * GDALMDArrayGetAttributes().

 * @param papszOptions Driver specific options determining how attributes
 * should be retrieved. Pass nullptr for default behavior.
 *
 * @return the attributes.
 */
std::vector<std::shared_ptr<GDALAttribute>>
GDALIHasAttribute::GetAttributes(CPL_UNUSED CSLConstList papszOptions) const
{
    return {};
}

/************************************************************************/
/*                             CreateAttribute()                         */
/************************************************************************/

/** Create an attribute within a GDALMDArray or GDALGroup.
 *
 * The attribute might not be "physically" created until a value is written
 * into it.
 *
 * Optionally implemented.
 *
 * Drivers known to implement it: MEM, netCDF
 *
 * This is the same as the C function GDALGroupCreateAttribute() or
 * GDALMDArrayCreateAttribute()
 *
 * @param osName Attribute name.
 * @param anDimensions List of dimension sizes, ordered from the slowest varying
 *                     dimension first to the fastest varying dimension last.
 *                     Empty for a scalar attribute (common case)
 * @param oDataType  Attribute data type.
 * @param papszOptions Driver specific options determining how the attribute.
 * should be created.
 *
 * @return the new attribute, or nullptr if case of error
 */
std::shared_ptr<GDALAttribute> GDALIHasAttribute::CreateAttribute(
    CPL_UNUSED const std::string &osName,
    CPL_UNUSED const std::vector<GUInt64> &anDimensions,
    CPL_UNUSED const GDALExtendedDataType &oDataType,
    CPL_UNUSED CSLConstList papszOptions)
{
    CPLError(CE_Failure, CPLE_NotSupported,
             "CreateAttribute() not implemented");
    return nullptr;
}

/************************************************************************/
/*                          DeleteAttribute()                           */
/************************************************************************/

/** Delete an attribute from a GDALMDArray or GDALGroup.
 *
 * Optionally implemented.
 *
 * After this call, if a previously obtained instance of the deleted object
 * is still alive, no method other than for freeing it should be invoked.
 *
 * Drivers known to implement it: MEM, netCDF
 *
 * This is the same as the C function GDALGroupDeleteAttribute() or
 * GDALMDArrayDeleteAttribute()
 *
 * @param osName Attribute name.
 * @param papszOptions Driver specific options determining how the attribute.
 * should be deleted.
 *
 * @return true in case of success
 * @since GDAL 3.8
 */
bool GDALIHasAttribute::DeleteAttribute(CPL_UNUSED const std::string &osName,
                                        CPL_UNUSED CSLConstList papszOptions)
{
    CPLError(CE_Failure, CPLE_NotSupported,
             "DeleteAttribute() not implemented");
    return false;
}

/************************************************************************/
/*                            GDALGroup()                               */
/************************************************************************/

//! @cond Doxygen_Suppress
GDALGroup::GDALGroup(const std::string &osParentName, const std::string &osName,
                     const std::string &osContext)
    : m_osName(osParentName.empty() ? "/" : osName),
      m_osFullName(
          !osParentName.empty()
              ? ((osParentName == "/" ? "/" : osParentName + "/") + osName)
              : "/"),
      m_osContext(osContext)
{
}

//! @endcond

/************************************************************************/
/*                            ~GDALGroup()                              */
/************************************************************************/

GDALGroup::~GDALGroup() = default;

/************************************************************************/
/*                          GetMDArrayNames()                           */
/************************************************************************/

/** Return the list of multidimensional array names contained in this group.
 *
 * @note Driver implementation: optionally implemented. If implemented,
 * OpenMDArray() should also be implemented.
 *
 * Drivers known to implement it: MEM, netCDF.
 *
 * This is the same as the C function GDALGroupGetMDArrayNames().
 *
 * @param papszOptions Driver specific options determining how arrays
 * should be retrieved. Pass nullptr for default behavior.
 *
 * @return the array names.
 */
std::vector<std::string>
GDALGroup::GetMDArrayNames(CPL_UNUSED CSLConstList papszOptions) const
{
    return {};
}

/************************************************************************/
/*                     GetMDArrayFullNamesRecursive()                   */
/************************************************************************/

/** Return the list of multidimensional array full names contained in this
 * group and its subgroups.
 *
 * This is the same as the C function GDALGroupGetMDArrayFullNamesRecursive().
 *
 * @param papszGroupOptions Driver specific options determining how groups
 * should be retrieved. Pass nullptr for default behavior.
 * @param papszArrayOptions Driver specific options determining how arrays
 * should be retrieved. Pass nullptr for default behavior.
 *
 * @return the array full names.
 *
 * @since 3.11
 */
std::vector<std::string>
GDALGroup::GetMDArrayFullNamesRecursive(CSLConstList papszGroupOptions,
                                        CSLConstList papszArrayOptions) const
{
    std::vector<std::string> ret;
    std::list<std::shared_ptr<GDALGroup>> stackGroups;
    stackGroups.push_back(nullptr);  // nullptr means this
    while (!stackGroups.empty())
    {
        std::shared_ptr<GDALGroup> groupPtr = std::move(stackGroups.front());
        stackGroups.erase(stackGroups.begin());
        const GDALGroup *poCurGroup = groupPtr ? groupPtr.get() : this;
        for (const std::string &arrayName :
             poCurGroup->GetMDArrayNames(papszArrayOptions))
        {
            std::string osFullName = poCurGroup->GetFullName();
            if (!osFullName.empty() && osFullName.back() != '/')
                osFullName += '/';
            osFullName += arrayName;
            ret.push_back(std::move(osFullName));
        }
        auto insertionPoint = stackGroups.begin();
        for (const auto &osSubGroup :
             poCurGroup->GetGroupNames(papszGroupOptions))
        {
            auto poSubGroup = poCurGroup->OpenGroup(osSubGroup);
            if (poSubGroup)
                stackGroups.insert(insertionPoint, std::move(poSubGroup));
        }
    }

    return ret;
}

/************************************************************************/
/*                            OpenMDArray()                             */
/************************************************************************/

/** Open and return a multidimensional array.
 *
 * @note Driver implementation: optionally implemented. If implemented,
 * GetMDArrayNames() should also be implemented.
 *
 * Drivers known to implement it: MEM, netCDF.
 *
 * This is the same as the C function GDALGroupOpenMDArray().
 *
 * @param osName Array name.
 * @param papszOptions Driver specific options determining how the array should
 * be opened.  Pass nullptr for default behavior.
 *
 * @return the array, or nullptr.
 */
std::shared_ptr<GDALMDArray>
GDALGroup::OpenMDArray(CPL_UNUSED const std::string &osName,
                       CPL_UNUSED CSLConstList papszOptions) const
{
    return nullptr;
}

/************************************************************************/
/*                           GetGroupNames()                            */
/************************************************************************/

/** Return the list of sub-groups contained in this group.
 *
 * @note Driver implementation: optionally implemented. If implemented,
 * OpenGroup() should also be implemented.
 *
 * Drivers known to implement it: MEM, netCDF.
 *
 * This is the same as the C function GDALGroupGetGroupNames().
 *
 * @param papszOptions Driver specific options determining how groups
 * should be retrieved. Pass nullptr for default behavior.
 *
 * @return the group names.
 */
std::vector<std::string>
GDALGroup::GetGroupNames(CPL_UNUSED CSLConstList papszOptions) const
{
    return {};
}

/************************************************************************/
/*                             OpenGroup()                              */
/************************************************************************/

/** Open and return a sub-group.
 *
 * @note Driver implementation: optionally implemented. If implemented,
 * GetGroupNames() should also be implemented.
 *
 * Drivers known to implement it: MEM, netCDF.
 *
 * This is the same as the C function GDALGroupOpenGroup().
 *
 * @param osName Sub-group name.
 * @param papszOptions Driver specific options determining how the sub-group
 * should be opened.  Pass nullptr for default behavior.
 *
 * @return the group, or nullptr.
 */
std::shared_ptr<GDALGroup>
GDALGroup::OpenGroup(CPL_UNUSED const std::string &osName,
                     CPL_UNUSED CSLConstList papszOptions) const
{
    return nullptr;
}

/************************************************************************/
/*                        GetVectorLayerNames()                         */
/************************************************************************/

/** Return the list of layer names contained in this group.
 *
 * @note Driver implementation: optionally implemented. If implemented,
 * OpenVectorLayer() should also be implemented.
 *
 * Drivers known to implement it: OpenFileGDB, FileGDB
 *
 * Other drivers will return an empty list. GDALDataset::GetLayerCount() and
 * GDALDataset::GetLayer() should then be used.
 *
 * This is the same as the C function GDALGroupGetVectorLayerNames().
 *
 * @param papszOptions Driver specific options determining how layers
 * should be retrieved. Pass nullptr for default behavior.
 *
 * @return the vector layer names.
 * @since GDAL 3.4
 */
std::vector<std::string>
GDALGroup::GetVectorLayerNames(CPL_UNUSED CSLConstList papszOptions) const
{
    return {};
}

/************************************************************************/
/*                           OpenVectorLayer()                          */
/************************************************************************/

/** Open and return a vector layer.
 *
 * Due to the historical ownership of OGRLayer* by GDALDataset*, the
 * lifetime of the returned OGRLayer* is linked to the one of the owner
 * dataset (contrary to the general design of this class where objects can be
 * used independently of the object that returned them)
 *
 * @note Driver implementation: optionally implemented. If implemented,
 * GetVectorLayerNames() should also be implemented.
 *
 * Drivers known to implement it: MEM, netCDF.
 *
 * This is the same as the C function GDALGroupOpenVectorLayer().
 *
 * @param osName Vector layer name.
 * @param papszOptions Driver specific options determining how the layer should
 * be opened.  Pass nullptr for default behavior.
 *
 * @return the group, or nullptr.
 */
OGRLayer *GDALGroup::OpenVectorLayer(CPL_UNUSED const std::string &osName,
                                     CPL_UNUSED CSLConstList papszOptions) const
{
    return nullptr;
}

/************************************************************************/
/*                             GetDimensions()                          */
/************************************************************************/

/** Return the list of dimensions contained in this group and used by its
 * arrays.
 *
 * This is for dimensions that can potentially be used by several arrays.
 * Not all drivers might implement this. To retrieve the dimensions used by
 * a specific array, use GDALMDArray::GetDimensions().
 *
 * Drivers known to implement it: MEM, netCDF
 *
 * This is the same as the C function GDALGroupGetDimensions().
 *
 * @param papszOptions Driver specific options determining how groups
 * should be retrieved. Pass nullptr for default behavior.
 *
 * @return the dimensions.
 */
std::vector<std::shared_ptr<GDALDimension>>
GDALGroup::GetDimensions(CPL_UNUSED CSLConstList papszOptions) const
{
    return {};
}

/************************************************************************/
/*                         GetStructuralInfo()                          */
/************************************************************************/

/** Return structural information on the group.
 *
 * This may be the compression, etc..
 *
 * The return value should not be freed and is valid until GDALGroup is
 * released or this function called again.
 *
 * This is the same as the C function GDALGroupGetStructuralInfo().
 */
CSLConstList GDALGroup::GetStructuralInfo() const
{
    return nullptr;
}

/************************************************************************/
/*                              CreateGroup()                           */
/************************************************************************/

/** Create a sub-group within a group.
 *
 * Optionally implemented by drivers.
 *
 * Drivers known to implement it: MEM, netCDF
 *
 * This is the same as the C function GDALGroupCreateGroup().
 *
 * @param osName Sub-group name.
 * @param papszOptions Driver specific options determining how the sub-group
 * should be created.
 *
 * @return the new sub-group, or nullptr in case of error.
 */
std::shared_ptr<GDALGroup>
GDALGroup::CreateGroup(CPL_UNUSED const std::string &osName,
                       CPL_UNUSED CSLConstList papszOptions)
{
    CPLError(CE_Failure, CPLE_NotSupported, "CreateGroup() not implemented");
    return nullptr;
}

/************************************************************************/
/*                          DeleteGroup()                               */
/************************************************************************/

/** Delete a sub-group from a group.
 *
 * Optionally implemented.
 *
 * After this call, if a previously obtained instance of the deleted object
 * is still alive, no method other than for freeing it should be invoked.
 *
 * Drivers known to implement it: MEM, Zarr
 *
 * This is the same as the C function GDALGroupDeleteGroup().
 *
 * @param osName Sub-group name.
 * @param papszOptions Driver specific options determining how the group.
 * should be deleted.
 *
 * @return true in case of success
 * @since GDAL 3.8
 */
bool GDALGroup::DeleteGroup(CPL_UNUSED const std::string &osName,
                            CPL_UNUSED CSLConstList papszOptions)
{
    CPLError(CE_Failure, CPLE_NotSupported, "DeleteGroup() not implemented");
    return false;
}

/************************************************************************/
/*                            CreateDimension()                         */
/************************************************************************/

/** Create a dimension within a group.
 *
 * @note Driver implementation: drivers supporting CreateDimension() should
 * implement this method, but do not have necessarily to implement
 * GDALGroup::GetDimensions().
 *
 * Drivers known to implement it: MEM, netCDF
 *
 * This is the same as the C function GDALGroupCreateDimension().
 *
 * @param osName Dimension name.
 * @param osType Dimension type (might be empty, and ignored by drivers)
 * @param osDirection Dimension direction (might be empty, and ignored by
 * drivers)
 * @param nSize  Number of values indexed by this dimension. Should be > 0.
 * @param papszOptions Driver specific options determining how the dimension
 * should be created.
 *
 * @return the new dimension, or nullptr if case of error
 */
std::shared_ptr<GDALDimension> GDALGroup::CreateDimension(
    CPL_UNUSED const std::string &osName, CPL_UNUSED const std::string &osType,
    CPL_UNUSED const std::string &osDirection, CPL_UNUSED GUInt64 nSize,
    CPL_UNUSED CSLConstList papszOptions)
{
    CPLError(CE_Failure, CPLE_NotSupported,
             "CreateDimension() not implemented");
    return nullptr;
}

/************************************************************************/
/*                             CreateMDArray()                          */
/************************************************************************/

/** Create a multidimensional array within a group.
 *
 * It is recommended that the GDALDimension objects passed in aoDimensions
 * belong to this group, either by retrieving them with GetDimensions()
 * or creating a new one with CreateDimension().
 *
 * Optionally implemented.
 *
 * Drivers known to implement it: MEM, netCDF
 *
 * This is the same as the C function GDALGroupCreateMDArray().
 *
 * @note Driver implementation: drivers should take into account the possibility
 * that GDALDimension object passed in aoDimensions might belong to a different
 * group / dataset / driver and act accordingly.
 *
 * @param osName Array name.
 * @param aoDimensions List of dimensions, ordered from the slowest varying
 *                     dimension first to the fastest varying dimension last.
 *                     Might be empty for a scalar array (if supported by
 * driver)
 * @param oDataType  Array data type.
 * @param papszOptions Driver specific options determining how the array
 * should be created.
 *
 * @return the new array, or nullptr in case of error
 */
std::shared_ptr<GDALMDArray> GDALGroup::CreateMDArray(
    CPL_UNUSED const std::string &osName,
    CPL_UNUSED const std::vector<std::shared_ptr<GDALDimension>> &aoDimensions,
    CPL_UNUSED const GDALExtendedDataType &oDataType,
    CPL_UNUSED CSLConstList papszOptions)
{
    CPLError(CE_Failure, CPLE_NotSupported, "CreateMDArray() not implemented");
    return nullptr;
}

/************************************************************************/
/*                          DeleteMDArray()                             */
/************************************************************************/

/** Delete an array from a group.
 *
 * Optionally implemented.
 *
 * After this call, if a previously obtained instance of the deleted object
 * is still alive, no method other than for freeing it should be invoked.
 *
 * Drivers known to implement it: MEM, Zarr
 *
 * This is the same as the C function GDALGroupDeleteMDArray().
 *
 * @param osName Arrayname.
 * @param papszOptions Driver specific options determining how the array.
 * should be deleted.
 *
 * @return true in case of success
 * @since GDAL 3.8
 */
bool GDALGroup::DeleteMDArray(CPL_UNUSED const std::string &osName,
                              CPL_UNUSED CSLConstList papszOptions)
{
    CPLError(CE_Failure, CPLE_NotSupported, "DeleteMDArray() not implemented");
    return false;
}

/************************************************************************/
/*                           GetTotalCopyCost()                         */
/************************************************************************/

/** Return a total "cost" to copy the group.
 *
 * Used as a parameter for CopFrom()
 */
GUInt64 GDALGroup::GetTotalCopyCost() const
{
    GUInt64 nCost = COPY_COST;
    nCost += GetAttributes().size() * GDALAttribute::COPY_COST;

    auto groupNames = GetGroupNames();
    for (const auto &name : groupNames)
    {
        auto subGroup = OpenGroup(name);
        if (subGroup)
        {
            nCost += subGroup->GetTotalCopyCost();
        }
    }

    auto arrayNames = GetMDArrayNames();
    for (const auto &name : arrayNames)
    {
        auto array = OpenMDArray(name);
        if (array)
        {
            nCost += array->GetTotalCopyCost();
        }
    }
    return nCost;
}

/************************************************************************/
/*                               CopyFrom()                             */
/************************************************************************/

/** Copy the content of a group into a new (generally empty) group.
 *
 * @param poDstRootGroup Destination root group. Must NOT be nullptr.
 * @param poSrcDS    Source dataset. Might be nullptr (but for correct behavior
 *                   of some output drivers this is not recommended)
 * @param poSrcGroup Source group. Must NOT be nullptr.
 * @param bStrict Whether to enable strict mode. In strict mode, any error will
 *                stop the copy. In relaxed mode, the copy will be attempted to
 *                be pursued.
 * @param nCurCost  Should be provided as a variable initially set to 0.
 * @param nTotalCost Total cost from GetTotalCopyCost().
 * @param pfnProgress Progress callback, or nullptr.
 * @param pProgressData Progress user data, or nulptr.
 * @param papszOptions Creation options. Currently, only array creation
 *                     options are supported. They must be prefixed with
 * "ARRAY:" . The scope may be further restricted to arrays of a certain
 *                     dimension by adding "IF(DIM={ndims}):" after "ARRAY:".
 *                     For example, "ARRAY:IF(DIM=2):BLOCKSIZE=256,256" will
 *                     restrict BLOCKSIZE=256,256 to arrays of dimension 2.
 *                     Restriction to arrays of a given name is done with adding
 *                     "IF(NAME={name}):" after "ARRAY:". {name} can also be
 *                     a full qualified name.
 *                     A non-driver specific ARRAY option, "AUTOSCALE=YES" can
 * be used to ask (non indexing) variables of type Float32 or Float64 to be
 * scaled to UInt16 with scale and offset values being computed from the minimum
 * and maximum of the source array. The integer data type used can be set with
 *                     AUTOSCALE_DATA_TYPE=Byte/UInt16/Int16/UInt32/Int32.
 *
 * @return true in case of success (or partial success if bStrict == false).
 */
bool GDALGroup::CopyFrom(const std::shared_ptr<GDALGroup> &poDstRootGroup,
                         GDALDataset *poSrcDS,
                         const std::shared_ptr<GDALGroup> &poSrcGroup,
                         bool bStrict, GUInt64 &nCurCost,
                         const GUInt64 nTotalCost, GDALProgressFunc pfnProgress,
                         void *pProgressData, CSLConstList papszOptions)
{
    if (pfnProgress == nullptr)
        pfnProgress = GDALDummyProgress;

#define EXIT_OR_CONTINUE_IF_NULL(x)                                            \
    if (!(x))                                                                  \
    {                                                                          \
        if (bStrict)                                                           \
            return false;                                                      \
        continue;                                                              \
    }                                                                          \
    (void)0

    try
    {
        nCurCost += GDALGroup::COPY_COST;

        const auto srcDims = poSrcGroup->GetDimensions();
        std::map<std::string, std::shared_ptr<GDALDimension>>
            mapExistingDstDims;
        std::map<std::string, std::string> mapSrcVariableNameToIndexedDimName;
        for (const auto &dim : srcDims)
        {
            auto dstDim = CreateDimension(dim->GetName(), dim->GetType(),
                                          dim->GetDirection(), dim->GetSize());
            EXIT_OR_CONTINUE_IF_NULL(dstDim);
            mapExistingDstDims[dim->GetName()] = std::move(dstDim);
            auto poIndexingVarSrc(dim->GetIndexingVariable());
            if (poIndexingVarSrc)
            {
                mapSrcVariableNameToIndexedDimName[poIndexingVarSrc
                                                       ->GetName()] =
                    dim->GetName();
            }
        }

        auto attrs = poSrcGroup->GetAttributes();
        for (const auto &attr : attrs)
        {
            auto dstAttr =
                CreateAttribute(attr->GetName(), attr->GetDimensionsSize(),
                                attr->GetDataType());
            EXIT_OR_CONTINUE_IF_NULL(dstAttr);
            auto raw(attr->ReadAsRaw());
            if (!dstAttr->Write(raw.data(), raw.size()) && bStrict)
                return false;
        }
        if (!attrs.empty())
        {
            nCurCost += attrs.size() * GDALAttribute::COPY_COST;
            if (!pfnProgress(double(nCurCost) / nTotalCost, "", pProgressData))
                return false;
        }

        const auto CopyArray =
            [this, &poSrcDS, &poDstRootGroup, &mapExistingDstDims,
             &mapSrcVariableNameToIndexedDimName, pfnProgress, pProgressData,
             papszOptions, bStrict, &nCurCost,
             nTotalCost](const std::shared_ptr<GDALMDArray> &srcArray)
        {
            // Map source dimensions to target dimensions
            std::vector<std::shared_ptr<GDALDimension>> dstArrayDims;
            const auto &srcArrayDims(srcArray->GetDimensions());
            for (const auto &dim : srcArrayDims)
            {
                auto dstDim = poDstRootGroup->OpenDimensionFromFullname(
                    dim->GetFullName());
                if (dstDim && dstDim->GetSize() == dim->GetSize())
                {
                    dstArrayDims.emplace_back(dstDim);
                }
                else
                {
                    auto oIter = mapExistingDstDims.find(dim->GetName());
                    if (oIter != mapExistingDstDims.end() &&
                        oIter->second->GetSize() == dim->GetSize())
                    {
                        dstArrayDims.emplace_back(oIter->second);
                    }
                    else
                    {
                        std::string newDimName;
                        if (oIter == mapExistingDstDims.end())
                        {
                            newDimName = dim->GetName();
                        }
                        else
                        {
                            std::string newDimNamePrefix(srcArray->GetName() +
                                                         '_' + dim->GetName());
                            newDimName = newDimNamePrefix;
                            int nIterCount = 2;
                            while (
                                cpl::contains(mapExistingDstDims, newDimName))
                            {
                                newDimName = newDimNamePrefix +
                                             CPLSPrintf("_%d", nIterCount);
                                nIterCount++;
                            }
                        }
                        dstDim = CreateDimension(newDimName, dim->GetType(),
                                                 dim->GetDirection(),
                                                 dim->GetSize());
                        if (!dstDim)
                            return false;
                        mapExistingDstDims[newDimName] = dstDim;
                        dstArrayDims.emplace_back(dstDim);
                    }
                }
            }

            CPLStringList aosArrayCO;
            bool bAutoScale = false;
            GDALDataType eAutoScaleType = GDT_UInt16;
            for (const char *pszItem : cpl::Iterate(papszOptions))
            {
                if (STARTS_WITH_CI(pszItem, "ARRAY:"))
                {
                    const char *pszOption = pszItem + strlen("ARRAY:");
                    if (STARTS_WITH_CI(pszOption, "IF(DIM="))
                    {
                        const char *pszNext = strchr(pszOption, ':');
                        if (pszNext != nullptr)
                        {
                            int nDim = atoi(pszOption + strlen("IF(DIM="));
                            if (static_cast<size_t>(nDim) ==
                                dstArrayDims.size())
                            {
                                pszOption = pszNext + 1;
                            }
                            else
                            {
                                pszOption = nullptr;
                            }
                        }
                    }
                    else if (STARTS_WITH_CI(pszOption, "IF(NAME="))
                    {
                        const char *pszName = pszOption + strlen("IF(NAME=");
                        const char *pszNext = strchr(pszName, ':');
                        if (pszNext != nullptr && pszNext > pszName &&
                            pszNext[-1] == ')')
                        {
                            CPLString osName;
                            osName.assign(pszName, pszNext - pszName - 1);
                            if (osName == srcArray->GetName() ||
                                osName == srcArray->GetFullName())
                            {
                                pszOption = pszNext + 1;
                            }
                            else
                            {
                                pszOption = nullptr;
                            }
                        }
                    }
                    if (pszOption)
                    {
                        if (STARTS_WITH_CI(pszOption, "AUTOSCALE="))
                        {
                            bAutoScale =
                                CPLTestBool(pszOption + strlen("AUTOSCALE="));
                        }
                        else if (STARTS_WITH_CI(pszOption,
                                                "AUTOSCALE_DATA_TYPE="))
                        {
                            const char *pszDataType =
                                pszOption + strlen("AUTOSCALE_DATA_TYPE=");
                            eAutoScaleType = GDALGetDataTypeByName(pszDataType);
                            if (GDALDataTypeIsComplex(eAutoScaleType) ||
                                GDALDataTypeIsFloating(eAutoScaleType))
                            {
                                CPLError(CE_Failure, CPLE_NotSupported,
                                         "Unsupported value for "
                                         "AUTOSCALE_DATA_TYPE");
                                return false;
                            }
                        }
                        else
                        {
                            aosArrayCO.AddString(pszOption);
                        }
                    }
                }
            }

            auto oIterDimName =
                mapSrcVariableNameToIndexedDimName.find(srcArray->GetName());
            const auto &srcArrayType = srcArray->GetDataType();

            std::shared_ptr<GDALMDArray> dstArray;

            // Only autoscale non-indexing variables
            bool bHasOffset = false;
            bool bHasScale = false;
            if (bAutoScale && srcArrayType.GetClass() == GEDTC_NUMERIC &&
                (srcArrayType.GetNumericDataType() == GDT_Float16 ||
                 srcArrayType.GetNumericDataType() == GDT_Float32 ||
                 srcArrayType.GetNumericDataType() == GDT_Float64) &&
                srcArray->GetOffset(&bHasOffset) == 0.0 && !bHasOffset &&
                srcArray->GetScale(&bHasScale) == 1.0 && !bHasScale &&
                oIterDimName == mapSrcVariableNameToIndexedDimName.end())
            {
                constexpr bool bApproxOK = false;
                constexpr bool bForce = true;
                double dfMin = 0.0;
                double dfMax = 0.0;
                if (srcArray->GetStatistics(bApproxOK, bForce, &dfMin, &dfMax,
                                            nullptr, nullptr, nullptr, nullptr,
                                            nullptr) != CE_None)
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Could not retrieve statistics for array %s",
                             srcArray->GetName().c_str());
                    return false;
                }
                double dfDTMin = 0;
                double dfDTMax = 0;
#define setDTMinMax(ctype)                                                     \
    do                                                                         \
    {                                                                          \
        dfDTMin = static_cast<double>(cpl::NumericLimits<ctype>::lowest());    \
        dfDTMax = static_cast<double>(cpl::NumericLimits<ctype>::max());       \
    } while (0)

                switch (eAutoScaleType)
                {
                    case GDT_Byte:
                        setDTMinMax(GByte);
                        break;
                    case GDT_Int8:
                        setDTMinMax(GInt8);
                        break;
                    case GDT_UInt16:
                        setDTMinMax(GUInt16);
                        break;
                    case GDT_Int16:
                        setDTMinMax(GInt16);
                        break;
                    case GDT_UInt32:
                        setDTMinMax(GUInt32);
                        break;
                    case GDT_Int32:
                        setDTMinMax(GInt32);
                        break;
                    case GDT_UInt64:
                        setDTMinMax(std::uint64_t);
                        break;
                    case GDT_Int64:
                        setDTMinMax(std::int64_t);
                        break;
                    case GDT_Float16:
                    case GDT_Float32:
                    case GDT_Float64:
                    case GDT_Unknown:
                    case GDT_CInt16:
                    case GDT_CInt32:
                    case GDT_CFloat16:
                    case GDT_CFloat32:
                    case GDT_CFloat64:
                    case GDT_TypeCount:
                        CPLAssert(false);
                }

                dstArray =
                    CreateMDArray(srcArray->GetName(), dstArrayDims,
                                  GDALExtendedDataType::Create(eAutoScaleType),
                                  aosArrayCO.List());
                if (!dstArray)
                    return !bStrict;

                if (srcArray->GetRawNoDataValue() != nullptr)
                {
                    // If there's a nodata value in the source array, reserve
                    // DTMax for that purpose in the target scaled array
                    if (!dstArray->SetNoDataValue(dfDTMax))
                    {
                        CPLError(CE_Failure, CPLE_AppDefined,
                                 "Cannot set nodata value");
                        return false;
                    }
                    dfDTMax--;
                }
                const double dfScale =
                    dfMax > dfMin ? (dfMax - dfMin) / (dfDTMax - dfDTMin) : 1.0;
                const double dfOffset = dfMin - dfDTMin * dfScale;

                if (!dstArray->SetOffset(dfOffset) ||
                    !dstArray->SetScale(dfScale))
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Cannot set scale/offset");
                    return false;
                }

                auto poUnscaled = dstArray->GetUnscaled();
                if (srcArray->GetRawNoDataValue() != nullptr)
                {
                    poUnscaled->SetNoDataValue(
                        srcArray->GetNoDataValueAsDouble());
                }

                // Copy source array into unscaled array
                if (!poUnscaled->CopyFrom(poSrcDS, srcArray.get(), bStrict,
                                          nCurCost, nTotalCost, pfnProgress,
                                          pProgressData))
                {
                    return false;
                }
            }
            else
            {
                dstArray = CreateMDArray(srcArray->GetName(), dstArrayDims,
                                         srcArrayType, aosArrayCO.List());
                if (!dstArray)
                    return !bStrict;

                if (!dstArray->CopyFrom(poSrcDS, srcArray.get(), bStrict,
                                        nCurCost, nTotalCost, pfnProgress,
                                        pProgressData))
                {
                    return false;
                }
            }

            // If this array is the indexing variable of a dimension, link them
            // together.
            if (oIterDimName != mapSrcVariableNameToIndexedDimName.end())
            {
                auto oCorrespondingDimIter =
                    mapExistingDstDims.find(oIterDimName->second);
                if (oCorrespondingDimIter != mapExistingDstDims.end())
                {
                    CPLErrorStateBackuper oErrorStateBackuper(
                        CPLQuietErrorHandler);
                    oCorrespondingDimIter->second->SetIndexingVariable(
                        std::move(dstArray));
                }
            }

            return true;
        };

        const auto arrayNames = poSrcGroup->GetMDArrayNames();

        // Start by copying arrays that are indexing variables of dimensions
        for (const auto &name : arrayNames)
        {
            auto srcArray = poSrcGroup->OpenMDArray(name);
            EXIT_OR_CONTINUE_IF_NULL(srcArray);

            if (cpl::contains(mapSrcVariableNameToIndexedDimName,
                              srcArray->GetName()))
            {
                if (!CopyArray(srcArray))
                    return false;
            }
        }

        // Then copy regular arrays
        for (const auto &name : arrayNames)
        {
            auto srcArray = poSrcGroup->OpenMDArray(name);
            EXIT_OR_CONTINUE_IF_NULL(srcArray);

            if (!cpl::contains(mapSrcVariableNameToIndexedDimName,
                               srcArray->GetName()))
            {
                if (!CopyArray(srcArray))
                    return false;
            }
        }

        const auto groupNames = poSrcGroup->GetGroupNames();
        for (const auto &name : groupNames)
        {
            auto srcSubGroup = poSrcGroup->OpenGroup(name);
            EXIT_OR_CONTINUE_IF_NULL(srcSubGroup);
            auto dstSubGroup = CreateGroup(name);
            EXIT_OR_CONTINUE_IF_NULL(dstSubGroup);
            if (!dstSubGroup->CopyFrom(
                    poDstRootGroup, poSrcDS, srcSubGroup, bStrict, nCurCost,
                    nTotalCost, pfnProgress, pProgressData, papszOptions))
                return false;
        }

        if (!pfnProgress(double(nCurCost) / nTotalCost, "", pProgressData))
            return false;

        return true;
    }
    catch (const std::exception &e)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "%s", e.what());
        return false;
    }
}

/************************************************************************/
/*                         GetInnerMostGroup()                          */
/************************************************************************/

//! @cond Doxygen_Suppress
const GDALGroup *
GDALGroup::GetInnerMostGroup(const std::string &osPathOrArrayOrDim,
                             std::shared_ptr<GDALGroup> &curGroupHolder,
                             std::string &osLastPart) const
{
    if (osPathOrArrayOrDim.empty() || osPathOrArrayOrDim[0] != '/')
        return nullptr;
    const GDALGroup *poCurGroup = this;
    CPLStringList aosTokens(
        CSLTokenizeString2(osPathOrArrayOrDim.c_str(), "/", 0));
    if (aosTokens.size() == 0)
    {
        return nullptr;
    }

    for (int i = 0; i < aosTokens.size() - 1; i++)
    {
        curGroupHolder = poCurGroup->OpenGroup(aosTokens[i], nullptr);
        if (!curGroupHolder)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Cannot find group %s",
                     aosTokens[i]);
            return nullptr;
        }
        poCurGroup = curGroupHolder.get();
    }
    osLastPart = aosTokens[aosTokens.size() - 1];
    return poCurGroup;
}

//! @endcond

/************************************************************************/
/*                      OpenMDArrayFromFullname()                       */
/************************************************************************/

/** Get an array from its fully qualified name */
std::shared_ptr<GDALMDArray>
GDALGroup::OpenMDArrayFromFullname(const std::string &osFullName,
                                   CSLConstList papszOptions) const
{
    std::string osName;
    std::shared_ptr<GDALGroup> curGroupHolder;
    auto poGroup(GetInnerMostGroup(osFullName, curGroupHolder, osName));
    if (poGroup == nullptr)
        return nullptr;
    return poGroup->OpenMDArray(osName, papszOptions);
}

/************************************************************************/
/*                      OpenAttributeFromFullname()                     */
/************************************************************************/

/** Get an attribute from its fully qualified name */
std::shared_ptr<GDALAttribute>
GDALGroup::OpenAttributeFromFullname(const std::string &osFullName,
                                     CSLConstList papszOptions) const
{
    const auto pos = osFullName.rfind('/');
    if (pos == std::string::npos)
        return nullptr;
    const std::string attrName = osFullName.substr(pos + 1);
    if (pos == 0)
        return GetAttribute(attrName);
    const std::string container = osFullName.substr(0, pos);
    auto poArray = OpenMDArrayFromFullname(container, papszOptions);
    if (poArray)
        return poArray->GetAttribute(attrName);
    auto poGroup = OpenGroupFromFullname(container, papszOptions);
    if (poGroup)
        return poGroup->GetAttribute(attrName);
    return nullptr;
}

/************************************************************************/
/*                          ResolveMDArray()                            */
/************************************************************************/

/** Locate an array in a group and its subgroups by name.
 *
 * If osName is a fully qualified name, then OpenMDArrayFromFullname() is first
 * used
 * Otherwise the search will start from the group identified by osStartingPath,
 * and an array whose name is osName will be looked for in this group (if
 * osStartingPath is empty or "/", then the current group is used). If there
 * is no match, then a recursive descendent search will be made in its
 * subgroups. If there is no match in the subgroups, then the parent (if
 * existing) of the group pointed by osStartingPath will be used as the new
 * starting point for the search.
 *
 * @param osName name, qualified or not
 * @param osStartingPath fully qualified name of the (sub-)group from which
 *                       the search should be started. If this is a non-empty
 *                       string, the group on which this method is called should
 *                       nominally be the root group (otherwise the path will
 *                       be interpreted as from the current group)
 * @param papszOptions options to pass to OpenMDArray()
 * @since GDAL 3.2
 */
std::shared_ptr<GDALMDArray>
GDALGroup::ResolveMDArray(const std::string &osName,
                          const std::string &osStartingPath,
                          CSLConstList papszOptions) const
{
    if (!osName.empty() && osName[0] == '/')
    {
        auto poArray = OpenMDArrayFromFullname(osName, papszOptions);
        if (poArray)
            return poArray;
    }
    std::string osPath(osStartingPath);
    std::set<std::string> oSetAlreadyVisited;

    while (true)
    {
        std::shared_ptr<GDALGroup> curGroupHolder;
        std::shared_ptr<GDALGroup> poGroup;

        std::queue<std::shared_ptr<GDALGroup>> oQueue;
        bool goOn = false;
        if (osPath.empty() || osPath == "/")
        {
            goOn = true;
        }
        else
        {
            std::string osLastPart;
            const GDALGroup *poGroupPtr =
                GetInnerMostGroup(osPath, curGroupHolder, osLastPart);
            if (poGroupPtr)
                poGroup = poGroupPtr->OpenGroup(osLastPart);
            if (poGroup &&
                !cpl::contains(oSetAlreadyVisited, poGroup->GetFullName()))
            {
                oQueue.push(poGroup);
                goOn = true;
            }
        }

        if (goOn)
        {
            do
            {
                const GDALGroup *groupPtr;
                if (!oQueue.empty())
                {
                    poGroup = oQueue.front();
                    oQueue.pop();
                    groupPtr = poGroup.get();
                }
                else
                {
                    groupPtr = this;
                }

                auto poArray = groupPtr->OpenMDArray(osName, papszOptions);
                if (poArray)
                    return poArray;

                const auto aosGroupNames = groupPtr->GetGroupNames();
                for (const auto &osGroupName : aosGroupNames)
                {
                    auto poSubGroup = groupPtr->OpenGroup(osGroupName);
                    if (poSubGroup && !cpl::contains(oSetAlreadyVisited,
                                                     poSubGroup->GetFullName()))
                    {
                        oQueue.push(poSubGroup);
                        oSetAlreadyVisited.insert(poSubGroup->GetFullName());
                    }
                }
            } while (!oQueue.empty());
        }

        if (osPath.empty() || osPath == "/")
            break;

        const auto nPos = osPath.rfind('/');
        if (nPos == 0)
            osPath = "/";
        else
        {
            if (nPos == std::string::npos)
                break;
            osPath.resize(nPos);
        }
    }
    return nullptr;
}

/************************************************************************/
/*                       OpenGroupFromFullname()                        */
/************************************************************************/

/** Get a group from its fully qualified name.
 * @since GDAL 3.2
 */
std::shared_ptr<GDALGroup>
GDALGroup::OpenGroupFromFullname(const std::string &osFullName,
                                 CSLConstList papszOptions) const
{
    std::string osName;
    std::shared_ptr<GDALGroup> curGroupHolder;
    auto poGroup(GetInnerMostGroup(osFullName, curGroupHolder, osName));
    if (poGroup == nullptr)
        return nullptr;
    return poGroup->OpenGroup(osName, papszOptions);
}

/************************************************************************/
/*                      OpenDimensionFromFullname()                     */
/************************************************************************/

/** Get a dimension from its fully qualified name */
std::shared_ptr<GDALDimension>
GDALGroup::OpenDimensionFromFullname(const std::string &osFullName) const
{
    std::string osName;
    std::shared_ptr<GDALGroup> curGroupHolder;
    auto poGroup(GetInnerMostGroup(osFullName, curGroupHolder, osName));
    if (poGroup == nullptr)
        return nullptr;
    auto dims(poGroup->GetDimensions());
    for (auto &dim : dims)
    {
        if (dim->GetName() == osName)
            return dim;
    }
    return nullptr;
}

/************************************************************************/
/*                           ClearStatistics()                          */
/************************************************************************/

/**
 * \brief Clear statistics.
 *
 * @since GDAL 3.4
 */
void GDALGroup::ClearStatistics()
{
    auto groupNames = GetGroupNames();
    for (const auto &name : groupNames)
    {
        auto subGroup = OpenGroup(name);
        if (subGroup)
        {
            subGroup->ClearStatistics();
        }
    }

    auto arrayNames = GetMDArrayNames();
    for (const auto &name : arrayNames)
    {
        auto array = OpenMDArray(name);
        if (array)
        {
            array->ClearStatistics();
        }
    }
}

/************************************************************************/
/*                            Rename()                                  */
/************************************************************************/

/** Rename the group.
 *
 * This is not implemented by all drivers.
 *
 * Drivers known to implement it: MEM, netCDF, ZARR.
 *
 * This is the same as the C function GDALGroupRename().
 *
 * @param osNewName New name.
 *
 * @return true in case of success
 * @since GDAL 3.8
 */
bool GDALGroup::Rename(CPL_UNUSED const std::string &osNewName)
{
    CPLError(CE_Failure, CPLE_NotSupported, "Rename() not implemented");
    return false;
}

/************************************************************************/
/*                         BaseRename()                                 */
/************************************************************************/

//! @cond Doxygen_Suppress
void GDALGroup::BaseRename(const std::string &osNewName)
{
    m_osFullName.resize(m_osFullName.size() - m_osName.size());
    m_osFullName += osNewName;
    m_osName = osNewName;

    NotifyChildrenOfRenaming();
}

//! @endcond

/************************************************************************/
/*                        ParentRenamed()                               */
/************************************************************************/

//! @cond Doxygen_Suppress
void GDALGroup::ParentRenamed(const std::string &osNewParentFullName)
{
    m_osFullName = osNewParentFullName;
    m_osFullName += "/";
    m_osFullName += m_osName;

    NotifyChildrenOfRenaming();
}

//! @endcond

/************************************************************************/
/*                             Deleted()                                */
/************************************************************************/

//! @cond Doxygen_Suppress
void GDALGroup::Deleted()
{
    m_bValid = false;

    NotifyChildrenOfDeletion();
}

//! @endcond

/************************************************************************/
/*                        ParentDeleted()                               */
/************************************************************************/

//! @cond Doxygen_Suppress
void GDALGroup::ParentDeleted()
{
    Deleted();
}

//! @endcond

/************************************************************************/
/*                     CheckValidAndErrorOutIfNot()                     */
/************************************************************************/

//! @cond Doxygen_Suppress
bool GDALGroup::CheckValidAndErrorOutIfNot() const
{
    if (!m_bValid)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "This object has been deleted. No action on it is possible");
    }
    return m_bValid;
}

//! @endcond

/************************************************************************/
/*                       ~GDALAbstractMDArray()                         */
/************************************************************************/

GDALAbstractMDArray::~GDALAbstractMDArray() = default;

/************************************************************************/
/*                        GDALAbstractMDArray()                         */
/************************************************************************/

//! @cond Doxygen_Suppress
GDALAbstractMDArray::GDALAbstractMDArray(const std::string &osParentName,
                                         const std::string &osName)
    : m_osName(osName),
      m_osFullName(
          !osParentName.empty()
              ? ((osParentName == "/" ? "/" : osParentName + "/") + osName)
              : osName)
{
}

//! @endcond

/************************************************************************/
/*                           GetDimensions()                            */
/************************************************************************/

/** \fn GDALAbstractMDArray::GetDimensions() const
 * \brief Return the dimensions of an attribute/array.
 *
 * This is the same as the C functions GDALMDArrayGetDimensions() and
 * similar to GDALAttributeGetDimensionsSize().
 */

/************************************************************************/
/*                           GetDataType()                              */
/************************************************************************/

/** \fn GDALAbstractMDArray::GetDataType() const
 * \brief Return the data type of an attribute/array.
 *
 * This is the same as the C functions GDALMDArrayGetDataType() and
 * GDALAttributeGetDataType()
 */

/************************************************************************/
/*                        GetDimensionCount()                           */
/************************************************************************/

/** Return the number of dimensions.
 *
 * Default implementation is GetDimensions().size(), and may be overridden by
 * drivers if they have a faster / less expensive implementations.
 *
 * This is the same as the C function GDALMDArrayGetDimensionCount() or
 * GDALAttributeGetDimensionCount().
 *
 */
size_t GDALAbstractMDArray::GetDimensionCount() const
{
    return GetDimensions().size();
}

/************************************************************************/
/*                            Rename()                                  */
/************************************************************************/

/** Rename the attribute/array.
 *
 * This is not implemented by all drivers.
 *
 * Drivers known to implement it: MEM, netCDF, Zarr.
 *
 * This is the same as the C functions GDALMDArrayRename() or
 * GDALAttributeRename().
 *
 * @param osNewName New name.
 *
 * @return true in case of success
 * @since GDAL 3.8
 */
bool GDALAbstractMDArray::Rename(CPL_UNUSED const std::string &osNewName)
{
    CPLError(CE_Failure, CPLE_NotSupported, "Rename() not implemented");
    return false;
}

/************************************************************************/
/*                             CopyValue()                              */
/************************************************************************/

/** Convert a value from a source type to a destination type.
 *
 * If dstType is GEDTC_STRING, the written value will be a pointer to a char*,
 * that must be freed with CPLFree().
 */
bool GDALExtendedDataType::CopyValue(const void *pSrc,
                                     const GDALExtendedDataType &srcType,
                                     void *pDst,
                                     const GDALExtendedDataType &dstType)
{
    if (srcType.GetClass() == GEDTC_NUMERIC &&
        dstType.GetClass() == GEDTC_NUMERIC)
    {
        GDALCopyWords64(pSrc, srcType.GetNumericDataType(), 0, pDst,
                        dstType.GetNumericDataType(), 0, 1);
        return true;
    }
    if (srcType.GetClass() == GEDTC_STRING &&
        dstType.GetClass() == GEDTC_STRING)
    {
        const char *srcStrPtr;
        memcpy(&srcStrPtr, pSrc, sizeof(const char *));
        char *pszDup = srcStrPtr ? CPLStrdup(srcStrPtr) : nullptr;
        *reinterpret_cast<void **>(pDst) = pszDup;
        return true;
    }
    if (srcType.GetClass() == GEDTC_NUMERIC &&
        dstType.GetClass() == GEDTC_STRING)
    {
        const char *str = nullptr;
        switch (srcType.GetNumericDataType())
        {
            case GDT_Unknown:
                break;
            case GDT_Byte:
                str = CPLSPrintf("%d", *static_cast<const GByte *>(pSrc));
                break;
            case GDT_Int8:
                str = CPLSPrintf("%d", *static_cast<const GInt8 *>(pSrc));
                break;
            case GDT_UInt16:
                str = CPLSPrintf("%d", *static_cast<const GUInt16 *>(pSrc));
                break;
            case GDT_Int16:
                str = CPLSPrintf("%d", *static_cast<const GInt16 *>(pSrc));
                break;
            case GDT_UInt32:
                str = CPLSPrintf("%u", *static_cast<const GUInt32 *>(pSrc));
                break;
            case GDT_Int32:
                str = CPLSPrintf("%d", *static_cast<const GInt32 *>(pSrc));
                break;
            case GDT_UInt64:
                str =
                    CPLSPrintf(CPL_FRMT_GUIB,
                               static_cast<GUIntBig>(
                                   *static_cast<const std::uint64_t *>(pSrc)));
                break;
            case GDT_Int64:
                str = CPLSPrintf(CPL_FRMT_GIB,
                                 static_cast<GIntBig>(
                                     *static_cast<const std::int64_t *>(pSrc)));
                break;
            case GDT_Float16:
                str = CPLSPrintf("%.5g",
                                 double(*static_cast<const GFloat16 *>(pSrc)));
                break;
            case GDT_Float32:
                str = CPLSPrintf("%.9g", *static_cast<const float *>(pSrc));
                break;
            case GDT_Float64:
                str = CPLSPrintf("%.17g", *static_cast<const double *>(pSrc));
                break;
            case GDT_CInt16:
            {
                const GInt16 *src = static_cast<const GInt16 *>(pSrc);
                str = CPLSPrintf("%d+%dj", src[0], src[1]);
                break;
            }
            case GDT_CInt32:
            {
                const GInt32 *src = static_cast<const GInt32 *>(pSrc);
                str = CPLSPrintf("%d+%dj", src[0], src[1]);
                break;
            }
            case GDT_CFloat16:
            {
                const GFloat16 *src = static_cast<const GFloat16 *>(pSrc);
                str = CPLSPrintf("%.5g+%.5gj", double(src[0]), double(src[1]));
                break;
            }
            case GDT_CFloat32:
            {
                const float *src = static_cast<const float *>(pSrc);
                str = CPLSPrintf("%.9g+%.9gj", src[0], src[1]);
                break;
            }
            case GDT_CFloat64:
            {
                const double *src = static_cast<const double *>(pSrc);
                str = CPLSPrintf("%.17g+%.17gj", src[0], src[1]);
                break;
            }
            case GDT_TypeCount:
                CPLAssert(false);
                break;
        }
        char *pszDup = str ? CPLStrdup(str) : nullptr;
        *reinterpret_cast<void **>(pDst) = pszDup;
        return true;
    }
    if (srcType.GetClass() == GEDTC_STRING &&
        dstType.GetClass() == GEDTC_NUMERIC)
    {
        const char *srcStrPtr;
        memcpy(&srcStrPtr, pSrc, sizeof(const char *));
        if (dstType.GetNumericDataType() == GDT_Int64)
        {
            *(static_cast<int64_t *>(pDst)) =
                srcStrPtr == nullptr ? 0
                                     : static_cast<int64_t>(atoll(srcStrPtr));
        }
        else if (dstType.GetNumericDataType() == GDT_UInt64)
        {
            *(static_cast<uint64_t *>(pDst)) =
                srcStrPtr == nullptr
                    ? 0
                    : static_cast<uint64_t>(strtoull(srcStrPtr, nullptr, 10));
        }
        else
        {
            // FIXME GDT_UInt64
            const double dfVal = srcStrPtr == nullptr ? 0 : CPLAtof(srcStrPtr);
            GDALCopyWords64(&dfVal, GDT_Float64, 0, pDst,
                            dstType.GetNumericDataType(), 0, 1);
        }
        return true;
    }
    if (srcType.GetClass() == GEDTC_COMPOUND &&
        dstType.GetClass() == GEDTC_COMPOUND)
    {
        const auto &srcComponents = srcType.GetComponents();
        const auto &dstComponents = dstType.GetComponents();
        const GByte *pabySrc = static_cast<const GByte *>(pSrc);
        GByte *pabyDst = static_cast<GByte *>(pDst);

        std::map<std::string, const std::unique_ptr<GDALEDTComponent> *>
            srcComponentMap;
        for (const auto &srcComp : srcComponents)
        {
            srcComponentMap[srcComp->GetName()] = &srcComp;
        }
        for (const auto &dstComp : dstComponents)
        {
            auto oIter = srcComponentMap.find(dstComp->GetName());
            if (oIter == srcComponentMap.end())
                return false;
            const auto &srcComp = *(oIter->second);
            if (!GDALExtendedDataType::CopyValue(
                    pabySrc + srcComp->GetOffset(), srcComp->GetType(),
                    pabyDst + dstComp->GetOffset(), dstComp->GetType()))
            {
                return false;
            }
        }
        return true;
    }

    return false;
}

/************************************************************************/
/*                             CopyValues()                             */
/************************************************************************/

/** Convert severals value from a source type to a destination type.
 *
 * If dstType is GEDTC_STRING, the written value will be a pointer to a char*,
 * that must be freed with CPLFree().
 */
bool GDALExtendedDataType::CopyValues(const void *pSrc,
                                      const GDALExtendedDataType &srcType,
                                      GPtrDiff_t nSrcStrideInElts, void *pDst,
                                      const GDALExtendedDataType &dstType,
                                      GPtrDiff_t nDstStrideInElts,
                                      size_t nValues)
{
    const auto nSrcStrideInBytes =
        nSrcStrideInElts * static_cast<GPtrDiff_t>(srcType.GetSize());
    const auto nDstStrideInBytes =
        nDstStrideInElts * static_cast<GPtrDiff_t>(dstType.GetSize());
    if (srcType.GetClass() == GEDTC_NUMERIC &&
        dstType.GetClass() == GEDTC_NUMERIC &&
        nSrcStrideInBytes >= std::numeric_limits<int>::min() &&
        nSrcStrideInBytes <= std::numeric_limits<int>::max() &&
        nDstStrideInBytes >= std::numeric_limits<int>::min() &&
        nDstStrideInBytes <= std::numeric_limits<int>::max())
    {
        GDALCopyWords64(pSrc, srcType.GetNumericDataType(),
                        static_cast<int>(nSrcStrideInBytes), pDst,
                        dstType.GetNumericDataType(),
                        static_cast<int>(nDstStrideInBytes), nValues);
    }
    else
    {
        const GByte *pabySrc = static_cast<const GByte *>(pSrc);
        GByte *pabyDst = static_cast<GByte *>(pDst);
        for (size_t i = 0; i < nValues; ++i)
        {
            if (!CopyValue(pabySrc, srcType, pabyDst, dstType))
                return false;
            pabySrc += nSrcStrideInBytes;
            pabyDst += nDstStrideInBytes;
        }
    }
    return true;
}

/************************************************************************/
/*                       CheckReadWriteParams()                         */
/************************************************************************/
//! @cond Doxygen_Suppress
bool GDALAbstractMDArray::CheckReadWriteParams(
    const GUInt64 *arrayStartIdx, const size_t *count, const GInt64 *&arrayStep,
    const GPtrDiff_t *&bufferStride, const GDALExtendedDataType &bufferDataType,
    const void *buffer, const void *buffer_alloc_start,
    size_t buffer_alloc_size, std::vector<GInt64> &tmp_arrayStep,
    std::vector<GPtrDiff_t> &tmp_bufferStride) const
{
    const auto lamda_error = []()
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Not all elements pointed by buffer will fit in "
                 "[buffer_alloc_start, "
                 "buffer_alloc_start + buffer_alloc_size]");
    };

    const auto &dims = GetDimensions();
    if (dims.empty())
    {
        if (buffer_alloc_start)
        {
            const size_t elementSize = bufferDataType.GetSize();
            const GByte *paby_buffer = static_cast<const GByte *>(buffer);
            const GByte *paby_buffer_alloc_start =
                static_cast<const GByte *>(buffer_alloc_start);
            const GByte *paby_buffer_alloc_end =
                paby_buffer_alloc_start + buffer_alloc_size;

            if (paby_buffer < paby_buffer_alloc_start ||
                paby_buffer + elementSize > paby_buffer_alloc_end)
            {
                lamda_error();
                return false;
            }
        }
        return true;
    }

    if (arrayStep == nullptr)
    {
        tmp_arrayStep.resize(dims.size(), 1);
        arrayStep = tmp_arrayStep.data();
    }
    for (size_t i = 0; i < dims.size(); i++)
    {
        assert(count);
        if (count[i] == 0)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "count[%u] = 0 is invalid",
                     static_cast<unsigned>(i));
            return false;
        }
    }
    bool bufferStride_all_positive = true;
    if (bufferStride == nullptr)
    {
        GPtrDiff_t stride = 1;
        assert(dims.empty() || count != nullptr);
        // To compute strides we must proceed from the fastest varying dimension
        // (the last one), and then reverse the result
        for (size_t i = dims.size(); i != 0;)
        {
            --i;
            tmp_bufferStride.push_back(stride);
            GUInt64 newStride = 0;
            bool bOK;
            try
            {
                newStride = (CPLSM(static_cast<uint64_t>(stride)) *
                             CPLSM(static_cast<uint64_t>(count[i])))
                                .v();
                bOK = static_cast<size_t>(newStride) == newStride &&
                      newStride < std::numeric_limits<size_t>::max() / 2;
            }
            catch (...)
            {
                bOK = false;
            }
            if (!bOK)
            {
                CPLError(CE_Failure, CPLE_OutOfMemory, "Too big count values");
                return false;
            }
            stride = static_cast<GPtrDiff_t>(newStride);
        }
        std::reverse(tmp_bufferStride.begin(), tmp_bufferStride.end());
        bufferStride = tmp_bufferStride.data();
    }
    else
    {
        for (size_t i = 0; i < dims.size(); i++)
        {
            if (bufferStride[i] < 0)
            {
                bufferStride_all_positive = false;
                break;
            }
        }
    }
    for (size_t i = 0; i < dims.size(); i++)
    {
        assert(arrayStartIdx);
        assert(count);
        if (arrayStartIdx[i] >= dims[i]->GetSize())
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "arrayStartIdx[%u] = " CPL_FRMT_GUIB " >= " CPL_FRMT_GUIB,
                     static_cast<unsigned>(i),
                     static_cast<GUInt64>(arrayStartIdx[i]),
                     static_cast<GUInt64>(dims[i]->GetSize()));
            return false;
        }
        bool bOverflow;
        if (arrayStep[i] >= 0)
        {
            try
            {
                bOverflow = (CPLSM(static_cast<uint64_t>(arrayStartIdx[i])) +
                             CPLSM(static_cast<uint64_t>(count[i] - 1)) *
                                 CPLSM(static_cast<uint64_t>(arrayStep[i])))
                                .v() >= dims[i]->GetSize();
            }
            catch (...)
            {
                bOverflow = true;
            }
            if (bOverflow)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "arrayStartIdx[%u] + (count[%u]-1) * arrayStep[%u] "
                         ">= " CPL_FRMT_GUIB,
                         static_cast<unsigned>(i), static_cast<unsigned>(i),
                         static_cast<unsigned>(i),
                         static_cast<GUInt64>(dims[i]->GetSize()));
                return false;
            }
        }
        else
        {
            try
            {
                bOverflow =
                    arrayStartIdx[i] <
                    (CPLSM(static_cast<uint64_t>(count[i] - 1)) *
                     CPLSM(arrayStep[i] == std::numeric_limits<GInt64>::min()
                               ? (static_cast<uint64_t>(1) << 63)
                               : static_cast<uint64_t>(-arrayStep[i])))
                        .v();
            }
            catch (...)
            {
                bOverflow = true;
            }
            if (bOverflow)
            {
                CPLError(
                    CE_Failure, CPLE_AppDefined,
                    "arrayStartIdx[%u] + (count[%u]-1) * arrayStep[%u] < 0",
                    static_cast<unsigned>(i), static_cast<unsigned>(i),
                    static_cast<unsigned>(i));
                return false;
            }
        }
    }

    if (buffer_alloc_start)
    {
        const size_t elementSize = bufferDataType.GetSize();
        const GByte *paby_buffer = static_cast<const GByte *>(buffer);
        const GByte *paby_buffer_alloc_start =
            static_cast<const GByte *>(buffer_alloc_start);
        const GByte *paby_buffer_alloc_end =
            paby_buffer_alloc_start + buffer_alloc_size;
        if (bufferStride_all_positive)
        {
            if (paby_buffer < paby_buffer_alloc_start)
            {
                lamda_error();
                return false;
            }
            GUInt64 nOffset = elementSize;
            for (size_t i = 0; i < dims.size(); i++)
            {
                try
                {
                    nOffset = (CPLSM(static_cast<uint64_t>(nOffset)) +
                               CPLSM(static_cast<uint64_t>(bufferStride[i])) *
                                   CPLSM(static_cast<uint64_t>(count[i] - 1)) *
                                   CPLSM(static_cast<uint64_t>(elementSize)))
                                  .v();
                }
                catch (...)
                {
                    lamda_error();
                    return false;
                }
            }
#if SIZEOF_VOIDP == 4
            if (static_cast<size_t>(nOffset) != nOffset)
            {
                lamda_error();
                return false;
            }
#endif
            if (paby_buffer + nOffset > paby_buffer_alloc_end)
            {
                lamda_error();
                return false;
            }
        }
        else if (dims.size() < 31)
        {
            // Check all corners of the hypercube
            const unsigned nLoops = 1U << static_cast<unsigned>(dims.size());
            for (unsigned iCornerCode = 0; iCornerCode < nLoops; iCornerCode++)
            {
                const GByte *paby = paby_buffer;
                for (unsigned i = 0; i < static_cast<unsigned>(dims.size());
                     i++)
                {
                    if (iCornerCode & (1U << i))
                    {
                        // We should check for integer overflows
                        paby += bufferStride[i] * (count[i] - 1) * elementSize;
                    }
                }
                if (paby < paby_buffer_alloc_start ||
                    paby + elementSize > paby_buffer_alloc_end)
                {
                    lamda_error();
                    return false;
                }
            }
        }
    }

    return true;
}

//! @endcond

/************************************************************************/
/*                               Read()                                 */
/************************************************************************/

/** Read part or totality of a multidimensional array or attribute.
 *
 * This will extract the content of a hyper-rectangle from the array into
 * a user supplied buffer.
 *
 * If bufferDataType is of type string, the values written in pDstBuffer
 * will be char* pointers and the strings should be freed with CPLFree().
 *
 * This is the same as the C function GDALMDArrayRead().
 *
 * @param arrayStartIdx Values representing the starting index to read
 *                      in each dimension (in [0, aoDims[i].GetSize()-1] range).
 *                      Array of GetDimensionCount() values. Must not be
 *                      nullptr, unless for a zero-dimensional array.
 *
 * @param count         Values representing the number of values to extract in
 *                      each dimension.
 *                      Array of GetDimensionCount() values. Must not be
 *                      nullptr, unless for a zero-dimensional array.
 *
 * @param arrayStep     Spacing between values to extract in each dimension.
 *                      The spacing is in number of array elements, not bytes.
 *                      If provided, must contain GetDimensionCount() values.
 *                      If set to nullptr, [1, 1, ... 1] will be used as a
 * default to indicate consecutive elements.
 *
 * @param bufferStride  Spacing between values to store in pDstBuffer.
 *                      The spacing is in number of array elements, not bytes.
 *                      If provided, must contain GetDimensionCount() values.
 *                      Negative values are possible (for example to reorder
 *                      from bottom-to-top to top-to-bottom).
 *                      If set to nullptr, will be set so that pDstBuffer is
 *                      written in a compact way, with elements of the last /
 *                      fastest varying dimension being consecutive.
 *
 * @param bufferDataType Data type of values in pDstBuffer.
 *
 * @param pDstBuffer    User buffer to store the values read. Should be big
 *                      enough to store the number of values indicated by
 * count[] and with the spacing of bufferStride[].
 *
 * @param pDstBufferAllocStart Optional pointer that can be used to validate the
 *                             validity of pDstBuffer. pDstBufferAllocStart
 * should be the pointer returned by the malloc() or equivalent call used to
 * allocate the buffer. It will generally be equal to pDstBuffer (when
 * bufferStride[] values are all positive), but not necessarily. If specified,
 * nDstBufferAllocSize should be also set to the appropriate value. If no
 * validation is needed, nullptr can be passed.
 *
 * @param nDstBufferAllocSize  Optional buffer size, that can be used to
 * validate the validity of pDstBuffer. This is the size of the buffer starting
 * at pDstBufferAllocStart. If specified, pDstBufferAllocStart should be also
 *                             set to the appropriate value.
 *                             If no validation is needed, 0 can be passed.
 *
 * @return true in case of success.
 */
bool GDALAbstractMDArray::Read(
    const GUInt64 *arrayStartIdx, const size_t *count,
    const GInt64 *arrayStep,         // step in elements
    const GPtrDiff_t *bufferStride,  // stride in elements
    const GDALExtendedDataType &bufferDataType, void *pDstBuffer,
    const void *pDstBufferAllocStart, size_t nDstBufferAllocSize) const
{
    if (!GetDataType().CanConvertTo(bufferDataType))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Array data type is not convertible to buffer data type");
        return false;
    }

    std::vector<GInt64> tmp_arrayStep;
    std::vector<GPtrDiff_t> tmp_bufferStride;
    if (!CheckReadWriteParams(arrayStartIdx, count, arrayStep, bufferStride,
                              bufferDataType, pDstBuffer, pDstBufferAllocStart,
                              nDstBufferAllocSize, tmp_arrayStep,
                              tmp_bufferStride))
    {
        return false;
    }

    return IRead(arrayStartIdx, count, arrayStep, bufferStride, bufferDataType,
                 pDstBuffer);
}

/************************************************************************/
/*                                IWrite()                              */
/************************************************************************/

//! @cond Doxygen_Suppress
bool GDALAbstractMDArray::IWrite(const GUInt64 *, const size_t *,
                                 const GInt64 *, const GPtrDiff_t *,
                                 const GDALExtendedDataType &, const void *)
{
    CPLError(CE_Failure, CPLE_AppDefined, "IWrite() not implemented");
    return false;
}

//! @endcond

/************************************************************************/
/*                               Write()                                 */
/************************************************************************/

/** Write part or totality of a multidimensional array or attribute.
 *
 * This will set the content of a hyper-rectangle into the array from
 * a user supplied buffer.
 *
 * If bufferDataType is of type string, the values read from pSrcBuffer
 * will be char* pointers.
 *
 * This is the same as the C function GDALMDArrayWrite().
 *
 * @param arrayStartIdx Values representing the starting index to write
 *                      in each dimension (in [0, aoDims[i].GetSize()-1] range).
 *                      Array of GetDimensionCount() values. Must not be
 *                      nullptr, unless for a zero-dimensional array.
 *
 * @param count         Values representing the number of values to write in
 *                      each dimension.
 *                      Array of GetDimensionCount() values. Must not be
 *                      nullptr, unless for a zero-dimensional array.
 *
 * @param arrayStep     Spacing between values to write in each dimension.
 *                      The spacing is in number of array elements, not bytes.
 *                      If provided, must contain GetDimensionCount() values.
 *                      If set to nullptr, [1, 1, ... 1] will be used as a
 * default to indicate consecutive elements.
 *
 * @param bufferStride  Spacing between values to read from pSrcBuffer.
 *                      The spacing is in number of array elements, not bytes.
 *                      If provided, must contain GetDimensionCount() values.
 *                      Negative values are possible (for example to reorder
 *                      from bottom-to-top to top-to-bottom).
 *                      If set to nullptr, will be set so that pSrcBuffer is
 *                      written in a compact way, with elements of the last /
 *                      fastest varying dimension being consecutive.
 *
 * @param bufferDataType Data type of values in pSrcBuffer.
 *
 * @param pSrcBuffer    User buffer to read the values from. Should be big
 *                      enough to store the number of values indicated by
 * count[] and with the spacing of bufferStride[].
 *
 * @param pSrcBufferAllocStart Optional pointer that can be used to validate the
 *                             validity of pSrcBuffer. pSrcBufferAllocStart
 * should be the pointer returned by the malloc() or equivalent call used to
 * allocate the buffer. It will generally be equal to pSrcBuffer (when
 * bufferStride[] values are all positive), but not necessarily. If specified,
 * nSrcBufferAllocSize should be also set to the appropriate value. If no
 * validation is needed, nullptr can be passed.
 *
 * @param nSrcBufferAllocSize  Optional buffer size, that can be used to
 * validate the validity of pSrcBuffer. This is the size of the buffer starting
 * at pSrcBufferAllocStart. If specified, pDstBufferAllocStart should be also
 *                             set to the appropriate value.
 *                             If no validation is needed, 0 can be passed.
 *
 * @return true in case of success.
 */
bool GDALAbstractMDArray::Write(const GUInt64 *arrayStartIdx,
                                const size_t *count, const GInt64 *arrayStep,
                                const GPtrDiff_t *bufferStride,
                                const GDALExtendedDataType &bufferDataType,
                                const void *pSrcBuffer,
                                const void *pSrcBufferAllocStart,
                                size_t nSrcBufferAllocSize)
{
    if (!bufferDataType.CanConvertTo(GetDataType()))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Buffer data type is not convertible to array data type");
        return false;
    }

    std::vector<GInt64> tmp_arrayStep;
    std::vector<GPtrDiff_t> tmp_bufferStride;
    if (!CheckReadWriteParams(arrayStartIdx, count, arrayStep, bufferStride,
                              bufferDataType, pSrcBuffer, pSrcBufferAllocStart,
                              nSrcBufferAllocSize, tmp_arrayStep,
                              tmp_bufferStride))
    {
        return false;
    }

    return IWrite(arrayStartIdx, count, arrayStep, bufferStride, bufferDataType,
                  pSrcBuffer);
}

/************************************************************************/
/*                          GetTotalElementsCount()                     */
/************************************************************************/

/** Return the total number of values in the array.
 *
 * This is the same as the C functions GDALMDArrayGetTotalElementsCount()
 * and GDALAttributeGetTotalElementsCount().
 *
 */
GUInt64 GDALAbstractMDArray::GetTotalElementsCount() const
{
    const auto &dims = GetDimensions();
    if (dims.empty())
        return 1;
    GUInt64 nElts = 1;
    for (const auto &dim : dims)
    {
        try
        {
            nElts = (CPLSM(static_cast<uint64_t>(nElts)) *
                     CPLSM(static_cast<uint64_t>(dim->GetSize())))
                        .v();
        }
        catch (...)
        {
            return 0;
        }
    }
    return nElts;
}

/************************************************************************/
/*                           GetBlockSize()                             */
/************************************************************************/

/** Return the "natural" block size of the array along all dimensions.
 *
 * Some drivers might organize the array in tiles/blocks and reading/writing
 * aligned on those tile/block boundaries will be more efficient.
 *
 * The returned number of elements in the vector is the same as
 * GetDimensionCount(). A value of 0 should be interpreted as no hint regarding
 * the natural block size along the considered dimension.
 * "Flat" arrays will typically return a vector of values set to 0.
 *
 * The default implementation will return a vector of values set to 0.
 *
 * This method is used by GetProcessingChunkSize().
 *
 * Pedantic note: the returned type is GUInt64, so in the highly unlikeley
 * theoretical case of a 32-bit platform, this might exceed its size_t
 * allocation capabilities.
 *
 * This is the same as the C function GDALMDArrayGetBlockSize().
 *
 * @return the block size, in number of elements along each dimension.
 */
std::vector<GUInt64> GDALAbstractMDArray::GetBlockSize() const
{
    return std::vector<GUInt64>(GetDimensionCount());
}

/************************************************************************/
/*                       GetProcessingChunkSize()                       */
/************************************************************************/

/** \brief Return an optimal chunk size for read/write operations, given the
 * natural block size and memory constraints specified.
 *
 * This method will use GetBlockSize() to define a chunk whose dimensions are
 * multiple of those returned by GetBlockSize() (unless the block define by
 * GetBlockSize() is larger than nMaxChunkMemory, in which case it will be
 * returned by this method).
 *
 * This is the same as the C function GDALMDArrayGetProcessingChunkSize().
 *
 * @param nMaxChunkMemory Maximum amount of memory, in bytes, to use for the
 * chunk.
 *
 * @return the chunk size, in number of elements along each dimension.
 */
std::vector<size_t>
GDALAbstractMDArray::GetProcessingChunkSize(size_t nMaxChunkMemory) const
{
    const auto &dims = GetDimensions();
    const auto &nDTSize = GetDataType().GetSize();
    std::vector<size_t> anChunkSize;
    auto blockSize = GetBlockSize();
    CPLAssert(blockSize.size() == dims.size());
    size_t nChunkSize = nDTSize;
    bool bOverflow = false;
    constexpr auto kSIZE_T_MAX = std::numeric_limits<size_t>::max();
    // Initialize anChunkSize[i] with blockSize[i] by properly clamping in
    // [1, min(sizet_max, dim_size[i])]
    // Also make sure that the product of all anChunkSize[i]) fits on size_t
    for (size_t i = 0; i < dims.size(); i++)
    {
        const auto sizeDimI =
            std::max(static_cast<size_t>(1),
                     static_cast<size_t>(
                         std::min(static_cast<GUInt64>(kSIZE_T_MAX),
                                  std::min(blockSize[i], dims[i]->GetSize()))));
        anChunkSize.push_back(sizeDimI);
        if (nChunkSize > kSIZE_T_MAX / sizeDimI)
        {
            bOverflow = true;
        }
        else
        {
            nChunkSize *= sizeDimI;
        }
    }
    if (nChunkSize == 0)
        return anChunkSize;

    // If the product of all anChunkSize[i] does not fit on size_t, then
    // set lowest anChunkSize[i] to 1.
    if (bOverflow)
    {
        nChunkSize = nDTSize;
        bOverflow = false;
        for (size_t i = dims.size(); i > 0;)
        {
            --i;
            if (bOverflow || nChunkSize > kSIZE_T_MAX / anChunkSize[i])
            {
                bOverflow = true;
                anChunkSize[i] = 1;
            }
            else
            {
                nChunkSize *= anChunkSize[i];
            }
        }
    }

    nChunkSize = nDTSize;
    std::vector<size_t> anAccBlockSizeFromStart;
    for (size_t i = 0; i < dims.size(); i++)
    {
        nChunkSize *= anChunkSize[i];
        anAccBlockSizeFromStart.push_back(nChunkSize);
    }
    if (nChunkSize <= nMaxChunkMemory / 2)
    {
        size_t nVoxelsFromEnd = 1;
        for (size_t i = dims.size(); i > 0;)
        {
            --i;
            const auto nCurBlockSize =
                anAccBlockSizeFromStart[i] * nVoxelsFromEnd;
            const auto nMul = nMaxChunkMemory / nCurBlockSize;
            if (nMul >= 2)
            {
                const auto nSizeThisDim(dims[i]->GetSize());
                const auto nBlocksThisDim =
                    DIV_ROUND_UP(nSizeThisDim, anChunkSize[i]);
                anChunkSize[i] = static_cast<size_t>(std::min(
                    anChunkSize[i] *
                        std::min(static_cast<GUInt64>(nMul), nBlocksThisDim),
                    nSizeThisDim));
            }
            nVoxelsFromEnd *= anChunkSize[i];
        }
    }
    return anChunkSize;
}

/************************************************************************/
/*                         BaseRename()                                 */
/************************************************************************/

//! @cond Doxygen_Suppress
void GDALAbstractMDArray::BaseRename(const std::string &osNewName)
{
    m_osFullName.resize(m_osFullName.size() - m_osName.size());
    m_osFullName += osNewName;
    m_osName = osNewName;

    NotifyChildrenOfRenaming();
}

//! @endcond

//! @cond Doxygen_Suppress
/************************************************************************/
/*                          ParentRenamed()                             */
/************************************************************************/

void GDALAbstractMDArray::ParentRenamed(const std::string &osNewParentFullName)
{
    m_osFullName = osNewParentFullName;
    m_osFullName += "/";
    m_osFullName += m_osName;

    NotifyChildrenOfRenaming();
}

//! @endcond

/************************************************************************/
/*                             Deleted()                                */
/************************************************************************/

//! @cond Doxygen_Suppress
void GDALAbstractMDArray::Deleted()
{
    m_bValid = false;

    NotifyChildrenOfDeletion();
}

//! @endcond

/************************************************************************/
/*                        ParentDeleted()                               */
/************************************************************************/

//! @cond Doxygen_Suppress
void GDALAbstractMDArray::ParentDeleted()
{
    Deleted();
}

//! @endcond

/************************************************************************/
/*                     CheckValidAndErrorOutIfNot()                     */
/************************************************************************/

//! @cond Doxygen_Suppress
bool GDALAbstractMDArray::CheckValidAndErrorOutIfNot() const
{
    if (!m_bValid)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "This object has been deleted. No action on it is possible");
    }
    return m_bValid;
}

//! @endcond

/************************************************************************/
/*                             SetUnit()                                */
/************************************************************************/

/** Set the variable unit.
 *
 * Values should conform as much as possible with those allowed by
 * the NetCDF CF conventions:
 * http://cfconventions.org/Data/cf-conventions/cf-conventions-1.7/cf-conventions.html#units
 * but others might be returned.
 *
 * Few examples are "meter", "degrees", "second", ...
 * Empty value means unknown.
 *
 * This is the same as the C function GDALMDArraySetUnit()
 *
 * @note Driver implementation: optionally implemented.
 *
 * @param osUnit unit name.
 * @return true in case of success.
 */
bool GDALMDArray::SetUnit(CPL_UNUSED const std::string &osUnit)
{
    CPLError(CE_Failure, CPLE_NotSupported, "SetUnit() not implemented");
    return false;
}

/************************************************************************/
/*                             GetUnit()                                */
/************************************************************************/

/** Return the array unit.
 *
 * Values should conform as much as possible with those allowed by
 * the NetCDF CF conventions:
 * http://cfconventions.org/Data/cf-conventions/cf-conventions-1.7/cf-conventions.html#units
 * but others might be returned.
 *
 * Few examples are "meter", "degrees", "second", ...
 * Empty value means unknown.
 *
 * This is the same as the C function GDALMDArrayGetUnit()
 */
const std::string &GDALMDArray::GetUnit() const
{
    static const std::string emptyString;
    return emptyString;
}

/************************************************************************/
/*                          SetSpatialRef()                             */
/************************************************************************/

/** Assign a spatial reference system object to the array.
 *
 * This is the same as the C function GDALMDArraySetSpatialRef().
 */
bool GDALMDArray::SetSpatialRef(CPL_UNUSED const OGRSpatialReference *poSRS)
{
    CPLError(CE_Failure, CPLE_NotSupported, "SetSpatialRef() not implemented");
    return false;
}

/************************************************************************/
/*                          GetSpatialRef()                             */
/************************************************************************/

/** Return the spatial reference system object associated with the array.
 *
 * This is the same as the C function GDALMDArrayGetSpatialRef().
 */
std::shared_ptr<OGRSpatialReference> GDALMDArray::GetSpatialRef() const
{
    return nullptr;
}

/************************************************************************/
/*                        GetRawNoDataValue()                           */
/************************************************************************/

/** Return the nodata value as a "raw" value.
 *
 * The value returned might be nullptr in case of no nodata value. When
 * a nodata value is registered, a non-nullptr will be returned whose size in
 * bytes is GetDataType().GetSize().
 *
 * The returned value should not be modified or freed. It is valid until
 * the array is destroyed, or the next call to GetRawNoDataValue() or
 * SetRawNoDataValue(), or any similar methods.
 *
 * @note Driver implementation: this method shall be implemented if nodata
 * is supported.
 *
 * This is the same as the C function GDALMDArrayGetRawNoDataValue().
 *
 * @return nullptr or a pointer to GetDataType().GetSize() bytes.
 */
const void *GDALMDArray::GetRawNoDataValue() const
{
    return nullptr;
}

/************************************************************************/
/*                        GetNoDataValueAsDouble()                      */
/************************************************************************/

/** Return the nodata value as a double.
 *
 * This is the same as the C function GDALMDArrayGetNoDataValueAsDouble().
 *
 * @param pbHasNoData Pointer to a output boolean that will be set to true if
 * a nodata value exists and can be converted to double. Might be nullptr.
 *
 * @return the nodata value as a double. A 0.0 value might also indicate the
 * absence of a nodata value or an error in the conversion (*pbHasNoData will be
 * set to false then).
 */
double GDALMDArray::GetNoDataValueAsDouble(bool *pbHasNoData) const
{
    const void *pNoData = GetRawNoDataValue();
    double dfNoData = 0.0;
    const auto &eDT = GetDataType();
    const bool ok = pNoData != nullptr && eDT.GetClass() == GEDTC_NUMERIC;
    if (ok)
    {
        GDALCopyWords64(pNoData, eDT.GetNumericDataType(), 0, &dfNoData,
                        GDT_Float64, 0, 1);
    }
    if (pbHasNoData)
        *pbHasNoData = ok;
    return dfNoData;
}

/************************************************************************/
/*                        GetNoDataValueAsInt64()                       */
/************************************************************************/

/** Return the nodata value as a Int64.
 *
 * @param pbHasNoData Pointer to a output boolean that will be set to true if
 * a nodata value exists and can be converted to Int64. Might be nullptr.
 *
 * This is the same as the C function GDALMDArrayGetNoDataValueAsInt64().
 *
 * @return the nodata value as a Int64
 *
 * @since GDAL 3.5
 */
int64_t GDALMDArray::GetNoDataValueAsInt64(bool *pbHasNoData) const
{
    const void *pNoData = GetRawNoDataValue();
    int64_t nNoData = GDAL_PAM_DEFAULT_NODATA_VALUE_INT64;
    const auto &eDT = GetDataType();
    const bool ok = pNoData != nullptr && eDT.GetClass() == GEDTC_NUMERIC;
    if (ok)
    {
        GDALCopyWords64(pNoData, eDT.GetNumericDataType(), 0, &nNoData,
                        GDT_Int64, 0, 1);
    }
    if (pbHasNoData)
        *pbHasNoData = ok;
    return nNoData;
}

/************************************************************************/
/*                       GetNoDataValueAsUInt64()                       */
/************************************************************************/

/** Return the nodata value as a UInt64.
 *
 * This is the same as the C function GDALMDArrayGetNoDataValueAsUInt64().

 * @param pbHasNoData Pointer to a output boolean that will be set to true if
 * a nodata value exists and can be converted to UInt64. Might be nullptr.
 *
 * @return the nodata value as a UInt64
 *
 * @since GDAL 3.5
 */
uint64_t GDALMDArray::GetNoDataValueAsUInt64(bool *pbHasNoData) const
{
    const void *pNoData = GetRawNoDataValue();
    uint64_t nNoData = GDAL_PAM_DEFAULT_NODATA_VALUE_UINT64;
    const auto &eDT = GetDataType();
    const bool ok = pNoData != nullptr && eDT.GetClass() == GEDTC_NUMERIC;
    if (ok)
    {
        GDALCopyWords64(pNoData, eDT.GetNumericDataType(), 0, &nNoData,
                        GDT_UInt64, 0, 1);
    }
    if (pbHasNoData)
        *pbHasNoData = ok;
    return nNoData;
}

/************************************************************************/
/*                        SetRawNoDataValue()                           */
/************************************************************************/

/** Set the nodata value as a "raw" value.
 *
 * The value passed might be nullptr in case of no nodata value. When
 * a nodata value is registered, a non-nullptr whose size in
 * bytes is GetDataType().GetSize() must be passed.
 *
 * This is the same as the C function GDALMDArraySetRawNoDataValue().
 *
 * @note Driver implementation: this method shall be implemented if setting
 nodata
 * is supported.

 * @return true in case of success.
 */
bool GDALMDArray::SetRawNoDataValue(CPL_UNUSED const void *pRawNoData)
{
    CPLError(CE_Failure, CPLE_NotSupported,
             "SetRawNoDataValue() not implemented");
    return false;
}

/************************************************************************/
/*                           SetNoDataValue()                           */
/************************************************************************/

/** Set the nodata value as a double.
 *
 * If the natural data type of the attribute/array is not double, type
 * conversion will occur to the type returned by GetDataType().
 *
 * This is the same as the C function GDALMDArraySetNoDataValueAsDouble().
 *
 * @return true in case of success.
 */
bool GDALMDArray::SetNoDataValue(double dfNoData)
{
    void *pRawNoData = CPLMalloc(GetDataType().GetSize());
    bool bRet = false;
    if (GDALExtendedDataType::CopyValue(
            &dfNoData, GDALExtendedDataType::Create(GDT_Float64), pRawNoData,
            GetDataType()))
    {
        bRet = SetRawNoDataValue(pRawNoData);
    }
    CPLFree(pRawNoData);
    return bRet;
}

/************************************************************************/
/*                           SetNoDataValue()                           */
/************************************************************************/

/** Set the nodata value as a Int64.
 *
 * If the natural data type of the attribute/array is not Int64, type conversion
 * will occur to the type returned by GetDataType().
 *
 * This is the same as the C function GDALMDArraySetNoDataValueAsInt64().
 *
 * @return true in case of success.
 *
 * @since GDAL 3.5
 */
bool GDALMDArray::SetNoDataValue(int64_t nNoData)
{
    void *pRawNoData = CPLMalloc(GetDataType().GetSize());
    bool bRet = false;
    if (GDALExtendedDataType::CopyValue(&nNoData,
                                        GDALExtendedDataType::Create(GDT_Int64),
                                        pRawNoData, GetDataType()))
    {
        bRet = SetRawNoDataValue(pRawNoData);
    }
    CPLFree(pRawNoData);
    return bRet;
}

/************************************************************************/
/*                           SetNoDataValue()                           */
/************************************************************************/

/** Set the nodata value as a Int64.
 *
 * If the natural data type of the attribute/array is not Int64, type conversion
 * will occur to the type returned by GetDataType().
 *
 * This is the same as the C function GDALMDArraySetNoDataValueAsUInt64().
 *
 * @return true in case of success.
 *
 * @since GDAL 3.5
 */
bool GDALMDArray::SetNoDataValue(uint64_t nNoData)
{
    void *pRawNoData = CPLMalloc(GetDataType().GetSize());
    bool bRet = false;
    if (GDALExtendedDataType::CopyValue(
            &nNoData, GDALExtendedDataType::Create(GDT_UInt64), pRawNoData,
            GetDataType()))
    {
        bRet = SetRawNoDataValue(pRawNoData);
    }
    CPLFree(pRawNoData);
    return bRet;
}

/************************************************************************/
/*                            Resize()                                  */
/************************************************************************/

/** Resize an array to new dimensions.
 *
 * Not all drivers may allow this operation, and with restrictions (e.g.
 * for netCDF, this is limited to growing of "unlimited" dimensions)
 *
 * Resizing a dimension used in other arrays will cause those other arrays
 * to be resized.
 *
 * This is the same as the C function GDALMDArrayResize().
 *
 * @param anNewDimSizes Array of GetDimensionCount() values containing the
 *                      new size of each indexing dimension.
 * @param papszOptions Options. (Driver specific)
 * @return true in case of success.
 * @since GDAL 3.7
 */
bool GDALMDArray::Resize(CPL_UNUSED const std::vector<GUInt64> &anNewDimSizes,
                         CPL_UNUSED CSLConstList papszOptions)
{
    CPLError(CE_Failure, CPLE_NotSupported,
             "Resize() is not supported for this array");
    return false;
}

/************************************************************************/
/*                               SetScale()                             */
/************************************************************************/

/** Set the scale value to apply to raw values.
 *
 * unscaled_value = raw_value * GetScale() + GetOffset()
 *
 * This is the same as the C function GDALMDArraySetScale() /
 * GDALMDArraySetScaleEx().
 *
 * @note Driver implementation: this method shall be implemented if setting
 * scale is supported.
 *
 * @param dfScale scale
 * @param eStorageType Data type to which create the potential attribute that
 * will store the scale. Added in GDAL 3.3 If let to its GDT_Unknown value, the
 * implementation will decide automatically the data type. Note that changing
 * the data type after initial setting might not be supported.
 * @return true in case of success.
 */
bool GDALMDArray::SetScale(CPL_UNUSED double dfScale,
                           CPL_UNUSED GDALDataType eStorageType)
{
    CPLError(CE_Failure, CPLE_NotSupported, "SetScale() not implemented");
    return false;
}

/************************************************************************/
/*                               SetOffset)                             */
/************************************************************************/

/** Set the offset value to apply to raw values.
 *
 * unscaled_value = raw_value * GetScale() + GetOffset()
 *
 * This is the same as the C function GDALMDArraySetOffset() /
 * GDALMDArraySetOffsetEx().
 *
 * @note Driver implementation: this method shall be implemented if setting
 * offset is supported.
 *
 * @param dfOffset Offset
 * @param eStorageType Data type to which create the potential attribute that
 * will store the offset. Added in GDAL 3.3 If let to its GDT_Unknown value, the
 * implementation will decide automatically the data type. Note that changing
 * the data type after initial setting might not be supported.
 * @return true in case of success.
 */
bool GDALMDArray::SetOffset(CPL_UNUSED double dfOffset,
                            CPL_UNUSED GDALDataType eStorageType)
{
    CPLError(CE_Failure, CPLE_NotSupported, "SetOffset() not implemented");
    return false;
}

/************************************************************************/
/*                               GetScale()                             */
/************************************************************************/

/** Get the scale value to apply to raw values.
 *
 * unscaled_value = raw_value * GetScale() + GetOffset()
 *
 * This is the same as the C function GDALMDArrayGetScale().
 *
 * @note Driver implementation: this method shall be implemented if gettings
 * scale is supported.
 *
 * @param pbHasScale Pointer to a output boolean that will be set to true if
 * a scale value exists. Might be nullptr.
 * @param peStorageType Pointer to a output GDALDataType that will be set to
 * the storage type of the scale value, when known/relevant. Otherwise will be
 * set to GDT_Unknown. Might be nullptr. Since GDAL 3.3
 *
 * @return the scale value. A 1.0 value might also indicate the
 * absence of a scale value.
 */
double GDALMDArray::GetScale(CPL_UNUSED bool *pbHasScale,
                             CPL_UNUSED GDALDataType *peStorageType) const
{
    if (pbHasScale)
        *pbHasScale = false;
    return 1.0;
}

/************************************************************************/
/*                               GetOffset()                            */
/************************************************************************/

/** Get the offset value to apply to raw values.
 *
 * unscaled_value = raw_value * GetScale() + GetOffset()
 *
 * This is the same as the C function GDALMDArrayGetOffset().
 *
 * @note Driver implementation: this method shall be implemented if gettings
 * offset is supported.
 *
 * @param pbHasOffset Pointer to a output boolean that will be set to true if
 * a offset value exists. Might be nullptr.
 * @param peStorageType Pointer to a output GDALDataType that will be set to
 * the storage type of the offset value, when known/relevant. Otherwise will be
 * set to GDT_Unknown. Might be nullptr. Since GDAL 3.3
 *
 * @return the offset value. A 0.0 value might also indicate the
 * absence of a offset value.
 */
double GDALMDArray::GetOffset(CPL_UNUSED bool *pbHasOffset,
                              CPL_UNUSED GDALDataType *peStorageType) const
{
    if (pbHasOffset)
        *pbHasOffset = false;
    return 0.0;
}

/************************************************************************/
/*                         ProcessPerChunk()                            */
/************************************************************************/

namespace
{
enum class Caller
{
    CALLER_END_OF_LOOP,
    CALLER_IN_LOOP,
};
}

/** \brief Call a user-provided function to operate on an array chunk by chunk.
 *
 * This method is to be used when doing operations on an array, or a subset of
 * it, in a chunk by chunk way.
 *
 * @param arrayStartIdx Values representing the starting index to use
 *                      in each dimension (in [0, aoDims[i].GetSize()-1] range).
 *                      Array of GetDimensionCount() values. Must not be
 *                      nullptr, unless for a zero-dimensional array.
 *
 * @param count         Values representing the number of values to use in
 *                      each dimension.
 *                      Array of GetDimensionCount() values. Must not be
 *                      nullptr, unless for a zero-dimensional array.
 *
 * @param chunkSize     Values representing the chunk size in each dimension.
 *                      Might typically the output of GetProcessingChunkSize().
 *                      Array of GetDimensionCount() values. Must not be
 *                      nullptr, unless for a zero-dimensional array.
 *
 * @param pfnFunc       User-provided function of type FuncProcessPerChunkType.
 *                      Must NOT be nullptr.
 *
 * @param pUserData     Pointer to pass as the value of the pUserData argument
 * of FuncProcessPerChunkType. Might be nullptr (depends on pfnFunc.
 *
 * @return true in case of success.
 */
bool GDALAbstractMDArray::ProcessPerChunk(const GUInt64 *arrayStartIdx,
                                          const GUInt64 *count,
                                          const size_t *chunkSize,
                                          FuncProcessPerChunkType pfnFunc,
                                          void *pUserData)
{
    const auto &dims = GetDimensions();
    if (dims.empty())
    {
        return pfnFunc(this, nullptr, nullptr, 1, 1, pUserData);
    }

    // Sanity check
    size_t nTotalChunkSize = 1;
    for (size_t i = 0; i < dims.size(); i++)
    {
        const auto nSizeThisDim(dims[i]->GetSize());
        if (count[i] == 0 || count[i] > nSizeThisDim ||
            arrayStartIdx[i] > nSizeThisDim - count[i])
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Inconsistent arrayStartIdx[] / count[] values "
                     "regarding array size");
            return false;
        }
        if (chunkSize[i] == 0 || chunkSize[i] > nSizeThisDim ||
            chunkSize[i] > std::numeric_limits<size_t>::max() / nTotalChunkSize)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Inconsistent chunkSize[] values");
            return false;
        }
        nTotalChunkSize *= chunkSize[i];
    }

    size_t dimIdx = 0;
    std::vector<GUInt64> chunkArrayStartIdx(dims.size());
    std::vector<size_t> chunkCount(dims.size());

    struct Stack
    {
        GUInt64 nBlockCounter = 0;
        GUInt64 nBlocksMinusOne = 0;
        size_t first_count = 0;  // only used if nBlocks > 1
        Caller return_point = Caller::CALLER_END_OF_LOOP;
    };

    std::vector<Stack> stack(dims.size());
    GUInt64 iCurChunk = 0;
    GUInt64 nChunkCount = 1;
    for (size_t i = 0; i < dims.size(); i++)
    {
        const auto nStartBlock = arrayStartIdx[i] / chunkSize[i];
        const auto nEndBlock = (arrayStartIdx[i] + count[i] - 1) / chunkSize[i];
        stack[i].nBlocksMinusOne = nEndBlock - nStartBlock;
        nChunkCount *= 1 + stack[i].nBlocksMinusOne;
        if (stack[i].nBlocksMinusOne == 0)
        {
            chunkArrayStartIdx[i] = arrayStartIdx[i];
            chunkCount[i] = static_cast<size_t>(count[i]);
        }
        else
        {
            stack[i].first_count = static_cast<size_t>(
                (nStartBlock + 1) * chunkSize[i] - arrayStartIdx[i]);
        }
    }

lbl_next_depth:
    if (dimIdx == dims.size())
    {
        ++iCurChunk;
        if (!pfnFunc(this, chunkArrayStartIdx.data(), chunkCount.data(),
                     iCurChunk, nChunkCount, pUserData))
        {
            return false;
        }
    }
    else
    {
        if (stack[dimIdx].nBlocksMinusOne != 0)
        {
            stack[dimIdx].nBlockCounter = stack[dimIdx].nBlocksMinusOne;
            chunkArrayStartIdx[dimIdx] = arrayStartIdx[dimIdx];
            chunkCount[dimIdx] = stack[dimIdx].first_count;
            stack[dimIdx].return_point = Caller::CALLER_IN_LOOP;
            while (true)
            {
                dimIdx++;
                goto lbl_next_depth;
            lbl_return_to_caller_in_loop:
                --stack[dimIdx].nBlockCounter;
                if (stack[dimIdx].nBlockCounter == 0)
                    break;
                chunkArrayStartIdx[dimIdx] += chunkCount[dimIdx];
                chunkCount[dimIdx] = chunkSize[dimIdx];
            }

            chunkArrayStartIdx[dimIdx] += chunkCount[dimIdx];
            chunkCount[dimIdx] =
                static_cast<size_t>(arrayStartIdx[dimIdx] + count[dimIdx] -
                                    chunkArrayStartIdx[dimIdx]);
            stack[dimIdx].return_point = Caller::CALLER_END_OF_LOOP;
        }
        dimIdx++;
        goto lbl_next_depth;
    lbl_return_to_caller_end_of_loop:
        if (dimIdx == 0)
            goto end;
    }

    assert(dimIdx > 0);
    dimIdx--;
    // cppcheck-suppress negativeContainerIndex
    switch (stack[dimIdx].return_point)
    {
        case Caller::CALLER_END_OF_LOOP:
            goto lbl_return_to_caller_end_of_loop;
        case Caller::CALLER_IN_LOOP:
            goto lbl_return_to_caller_in_loop;
    }
end:
    return true;
}

/************************************************************************/
/*                          GDALAttribute()                             */
/************************************************************************/

//! @cond Doxygen_Suppress
GDALAttribute::GDALAttribute(CPL_UNUSED const std::string &osParentName,
                             CPL_UNUSED const std::string &osName)
#if !defined(COMPILER_WARNS_ABOUT_ABSTRACT_VBASE_INIT)
    : GDALAbstractMDArray(osParentName, osName)
#endif
{
}

GDALAttribute::~GDALAttribute() = default;

//! @endcond

/************************************************************************/
/*                        GetDimensionSize()                            */
/************************************************************************/

/** Return the size of the dimensions of the attribute.
 *
 * This will be an empty array for a scalar (single value) attribute.
 *
 * This is the same as the C function GDALAttributeGetDimensionsSize().
 */
std::vector<GUInt64> GDALAttribute::GetDimensionsSize() const
{
    const auto &dims = GetDimensions();
    std::vector<GUInt64> ret;
    ret.reserve(dims.size());
    for (const auto &dim : dims)
        ret.push_back(dim->GetSize());
    return ret;
}

/************************************************************************/
/*                            GDALRawResult()                           */
/************************************************************************/

//! @cond Doxygen_Suppress
GDALRawResult::GDALRawResult(GByte *raw, const GDALExtendedDataType &dt,
                             size_t nEltCount)
    : m_dt(dt), m_nEltCount(nEltCount), m_nSize(nEltCount * dt.GetSize()),
      m_raw(raw)
{
}

//! @endcond

/************************************************************************/
/*                            GDALRawResult()                           */
/************************************************************************/

/** Move constructor. */
GDALRawResult::GDALRawResult(GDALRawResult &&other)
    : m_dt(std::move(other.m_dt)), m_nEltCount(other.m_nEltCount),
      m_nSize(other.m_nSize), m_raw(other.m_raw)
{
    other.m_nEltCount = 0;
    other.m_nSize = 0;
    other.m_raw = nullptr;
}

/************************************************************************/
/*                               FreeMe()                               */
/************************************************************************/

void GDALRawResult::FreeMe()
{
    if (m_raw && m_dt.NeedsFreeDynamicMemory())
    {
        GByte *pabyPtr = m_raw;
        const auto nDTSize(m_dt.GetSize());
        for (size_t i = 0; i < m_nEltCount; ++i)
        {
            m_dt.FreeDynamicMemory(pabyPtr);
            pabyPtr += nDTSize;
        }
    }
    VSIFree(m_raw);
}

/************************************************************************/
/*                             operator=()                              */
/************************************************************************/

/** Move assignment. */
GDALRawResult &GDALRawResult::operator=(GDALRawResult &&other)
{
    FreeMe();
    m_dt = std::move(other.m_dt);
    m_nEltCount = other.m_nEltCount;
    m_nSize = other.m_nSize;
    m_raw = other.m_raw;
    other.m_nEltCount = 0;
    other.m_nSize = 0;
    other.m_raw = nullptr;
    return *this;
}

/************************************************************************/
/*                         ~GDALRawResult()                             */
/************************************************************************/

/** Destructor. */
GDALRawResult::~GDALRawResult()
{
    FreeMe();
}

/************************************************************************/
/*                            StealData()                               */
/************************************************************************/

//! @cond Doxygen_Suppress
/** Return buffer to caller which becomes owner of it.
 * Only to be used by GDALAttributeReadAsRaw().
 */
GByte *GDALRawResult::StealData()
{
    GByte *ret = m_raw;
    m_raw = nullptr;
    m_nEltCount = 0;
    m_nSize = 0;
    return ret;
}

//! @endcond

/************************************************************************/
/*                             ReadAsRaw()                              */
/************************************************************************/

/** Return the raw value of an attribute.
 *
 *
 * This is the same as the C function GDALAttributeReadAsRaw().
 */
GDALRawResult GDALAttribute::ReadAsRaw() const
{
    const auto nEltCount(GetTotalElementsCount());
    const auto &dt(GetDataType());
    const auto nDTSize(dt.GetSize());
    GByte *res = static_cast<GByte *>(
        VSI_MALLOC2_VERBOSE(static_cast<size_t>(nEltCount), nDTSize));
    if (!res)
        return GDALRawResult(nullptr, dt, 0);
    const auto &dims = GetDimensions();
    const auto nDims = GetDimensionCount();
    std::vector<GUInt64> startIdx(1 + nDims, 0);
    std::vector<size_t> count(1 + nDims);
    for (size_t i = 0; i < nDims; i++)
    {
        count[i] = static_cast<size_t>(dims[i]->GetSize());
    }
    if (!Read(startIdx.data(), count.data(), nullptr, nullptr, dt, &res[0],
              &res[0], static_cast<size_t>(nEltCount * nDTSize)))
    {
        VSIFree(res);
        return GDALRawResult(nullptr, dt, 0);
    }
    return GDALRawResult(res, dt, static_cast<size_t>(nEltCount));
}

/************************************************************************/
/*                            ReadAsString()                            */
/************************************************************************/

/** Return the value of an attribute as a string.
 *
 * The returned string should not be freed, and its lifetime does not
 * excess a next call to ReadAsString() on the same object, or the deletion
 * of the object itself.
 *
 * This function will only return the first element if there are several.
 *
 * This is the same as the C function GDALAttributeReadAsString()
 *
 * @return a string, or nullptr.
 */
const char *GDALAttribute::ReadAsString() const
{
    const auto nDims = GetDimensionCount();
    std::vector<GUInt64> startIdx(1 + nDims, 0);
    std::vector<size_t> count(1 + nDims, 1);
    char *szRet = nullptr;
    if (!Read(startIdx.data(), count.data(), nullptr, nullptr,
              GDALExtendedDataType::CreateString(), &szRet, &szRet,
              sizeof(szRet)) ||
        szRet == nullptr)
    {
        return nullptr;
    }
    m_osCachedVal = szRet;
    CPLFree(szRet);
    return m_osCachedVal.c_str();
}

/************************************************************************/
/*                            ReadAsInt()                               */
/************************************************************************/

/** Return the value of an attribute as a integer.
 *
 * This function will only return the first element if there are several.
 *
 * It can fail if its value can not be converted to integer.
 *
 * This is the same as the C function GDALAttributeReadAsInt()
 *
 * @return a integer, or INT_MIN in case of error.
 */
int GDALAttribute::ReadAsInt() const
{
    const auto nDims = GetDimensionCount();
    std::vector<GUInt64> startIdx(1 + nDims, 0);
    std::vector<size_t> count(1 + nDims, 1);
    int nRet = INT_MIN;
    Read(startIdx.data(), count.data(), nullptr, nullptr,
         GDALExtendedDataType::Create(GDT_Int32), &nRet, &nRet, sizeof(nRet));
    return nRet;
}

/************************************************************************/
/*                            ReadAsInt64()                             */
/************************************************************************/

/** Return the value of an attribute as an int64_t.
 *
 * This function will only return the first element if there are several.
 *
 * It can fail if its value can not be converted to long.
 *
 * This is the same as the C function GDALAttributeReadAsInt64()
 *
 * @return an int64_t, or INT64_MIN in case of error.
 */
int64_t GDALAttribute::ReadAsInt64() const
{
    const auto nDims = GetDimensionCount();
    std::vector<GUInt64> startIdx(1 + nDims, 0);
    std::vector<size_t> count(1 + nDims, 1);
    int64_t nRet = INT64_MIN;
    Read(startIdx.data(), count.data(), nullptr, nullptr,
         GDALExtendedDataType::Create(GDT_Int64), &nRet, &nRet, sizeof(nRet));
    return nRet;
}

/************************************************************************/
/*                            ReadAsDouble()                            */
/************************************************************************/

/** Return the value of an attribute as a double.
 *
 * This function will only return the first element if there are several.
 *
 * It can fail if its value can not be converted to double.
 *
 * This is the same as the C function GDALAttributeReadAsInt()
 *
 * @return a double value.
 */
double GDALAttribute::ReadAsDouble() const
{
    const auto nDims = GetDimensionCount();
    std::vector<GUInt64> startIdx(1 + nDims, 0);
    std::vector<size_t> count(1 + nDims, 1);
    double dfRet = 0;
    Read(startIdx.data(), count.data(), nullptr, nullptr,
         GDALExtendedDataType::Create(GDT_Float64), &dfRet, &dfRet,
         sizeof(dfRet));
    return dfRet;
}

/************************************************************************/
/*                          ReadAsStringArray()                         */
/************************************************************************/

/** Return the value of an attribute as an array of strings.
 *
 * This is the same as the C function GDALAttributeReadAsStringArray()
 */
CPLStringList GDALAttribute::ReadAsStringArray() const
{
    const auto nElts = GetTotalElementsCount();
    if (nElts > static_cast<unsigned>(std::numeric_limits<int>::max() - 1))
        return CPLStringList();
    char **papszList = static_cast<char **>(
        VSI_CALLOC_VERBOSE(static_cast<int>(nElts) + 1, sizeof(char *)));
    const auto &dims = GetDimensions();
    const auto nDims = GetDimensionCount();
    std::vector<GUInt64> startIdx(1 + nDims, 0);
    std::vector<size_t> count(1 + nDims);
    for (size_t i = 0; i < nDims; i++)
    {
        count[i] = static_cast<size_t>(dims[i]->GetSize());
    }
    Read(startIdx.data(), count.data(), nullptr, nullptr,
         GDALExtendedDataType::CreateString(), papszList, papszList,
         sizeof(char *) * static_cast<int>(nElts));
    for (int i = 0; i < static_cast<int>(nElts); i++)
    {
        if (papszList[i] == nullptr)
            papszList[i] = CPLStrdup("");
    }
    return CPLStringList(papszList);
}

/************************************************************************/
/*                          ReadAsIntArray()                            */
/************************************************************************/

/** Return the value of an attribute as an array of integers.
 *
 * This is the same as the C function GDALAttributeReadAsIntArray().
 */
std::vector<int> GDALAttribute::ReadAsIntArray() const
{
    const auto nElts = GetTotalElementsCount();
#if SIZEOF_VOIDP == 4
    if (nElts > static_cast<size_t>(nElts))
        return {};
#endif
    std::vector<int> res(static_cast<size_t>(nElts));
    const auto &dims = GetDimensions();
    const auto nDims = GetDimensionCount();
    std::vector<GUInt64> startIdx(1 + nDims, 0);
    std::vector<size_t> count(1 + nDims);
    for (size_t i = 0; i < nDims; i++)
    {
        count[i] = static_cast<size_t>(dims[i]->GetSize());
    }
    Read(startIdx.data(), count.data(), nullptr, nullptr,
         GDALExtendedDataType::Create(GDT_Int32), &res[0], res.data(),
         res.size() * sizeof(res[0]));
    return res;
}

/************************************************************************/
/*                          ReadAsInt64Array()                          */
/************************************************************************/

/** Return the value of an attribute as an array of int64_t.
 *
 * This is the same as the C function GDALAttributeReadAsInt64Array().
 */
std::vector<int64_t> GDALAttribute::ReadAsInt64Array() const
{
    const auto nElts = GetTotalElementsCount();
#if SIZEOF_VOIDP == 4
    if (nElts > static_cast<size_t>(nElts))
        return {};
#endif
    std::vector<int64_t> res(static_cast<size_t>(nElts));
    const auto &dims = GetDimensions();
    const auto nDims = GetDimensionCount();
    std::vector<GUInt64> startIdx(1 + nDims, 0);
    std::vector<size_t> count(1 + nDims);
    for (size_t i = 0; i < nDims; i++)
    {
        count[i] = static_cast<size_t>(dims[i]->GetSize());
    }
    Read(startIdx.data(), count.data(), nullptr, nullptr,
         GDALExtendedDataType::Create(GDT_Int64), &res[0], res.data(),
         res.size() * sizeof(res[0]));
    return res;
}

/************************************************************************/
/*                         ReadAsDoubleArray()                          */
/************************************************************************/

/** Return the value of an attribute as an array of double.
 *
 * This is the same as the C function GDALAttributeReadAsDoubleArray().
 */
std::vector<double> GDALAttribute::ReadAsDoubleArray() const
{
    const auto nElts = GetTotalElementsCount();
#if SIZEOF_VOIDP == 4
    if (nElts > static_cast<size_t>(nElts))
        return {};
#endif
    std::vector<double> res(static_cast<size_t>(nElts));
    const auto &dims = GetDimensions();
    const auto nDims = GetDimensionCount();
    std::vector<GUInt64> startIdx(1 + nDims, 0);
    std::vector<size_t> count(1 + nDims);
    for (size_t i = 0; i < nDims; i++)
    {
        count[i] = static_cast<size_t>(dims[i]->GetSize());
    }
    Read(startIdx.data(), count.data(), nullptr, nullptr,
         GDALExtendedDataType::Create(GDT_Float64), &res[0], res.data(),
         res.size() * sizeof(res[0]));
    return res;
}

/************************************************************************/
/*                               Write()                                */
/************************************************************************/

/** Write an attribute from raw values expressed in GetDataType()
 *
 * The values should be provided in the type of GetDataType() and there should
 * be exactly GetTotalElementsCount() of them.
 * If GetDataType() is a string, each value should be a char* pointer.
 *
 * This is the same as the C function GDALAttributeWriteRaw().
 *
 * @param pabyValue Buffer of nLen bytes.
 * @param nLen Size of pabyValue in bytes. Should be equal to
 *             GetTotalElementsCount() * GetDataType().GetSize()
 * @return true in case of success.
 */
bool GDALAttribute::Write(const void *pabyValue, size_t nLen)
{
    if (nLen != GetTotalElementsCount() * GetDataType().GetSize())
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Length is not of expected value");
        return false;
    }
    const auto &dims = GetDimensions();
    const auto nDims = GetDimensionCount();
    std::vector<GUInt64> startIdx(1 + nDims, 0);
    std::vector<size_t> count(1 + nDims);
    for (size_t i = 0; i < nDims; i++)
    {
        count[i] = static_cast<size_t>(dims[i]->GetSize());
    }
    return Write(startIdx.data(), count.data(), nullptr, nullptr, GetDataType(),
                 pabyValue, pabyValue, nLen);
}

/************************************************************************/
/*                               Write()                                */
/************************************************************************/

/** Write an attribute from a string value.
 *
 * Type conversion will be performed if needed. If the attribute contains
 * multiple values, only the first one will be updated.
 *
 * This is the same as the C function GDALAttributeWriteString().
 *
 * @param pszValue Pointer to a string.
 * @return true in case of success.
 */
bool GDALAttribute::Write(const char *pszValue)
{
    const auto nDims = GetDimensionCount();
    std::vector<GUInt64> startIdx(1 + nDims, 0);
    std::vector<size_t> count(1 + nDims, 1);
    return Write(startIdx.data(), count.data(), nullptr, nullptr,
                 GDALExtendedDataType::CreateString(), &pszValue, &pszValue,
                 sizeof(pszValue));
}

/************************************************************************/
/*                              WriteInt()                              */
/************************************************************************/

/** Write an attribute from a integer value.
 *
 * Type conversion will be performed if needed. If the attribute contains
 * multiple values, only the first one will be updated.
 *
 * This is the same as the C function GDALAttributeWriteInt().
 *
 * @param nVal Value.
 * @return true in case of success.
 */
bool GDALAttribute::WriteInt(int nVal)
{
    const auto nDims = GetDimensionCount();
    std::vector<GUInt64> startIdx(1 + nDims, 0);
    std::vector<size_t> count(1 + nDims, 1);
    return Write(startIdx.data(), count.data(), nullptr, nullptr,
                 GDALExtendedDataType::Create(GDT_Int32), &nVal, &nVal,
                 sizeof(nVal));
}

/************************************************************************/
/*                              WriteInt64()                             */
/************************************************************************/

/** Write an attribute from an int64_t value.
 *
 * Type conversion will be performed if needed. If the attribute contains
 * multiple values, only the first one will be updated.
 *
 * This is the same as the C function GDALAttributeWriteInt().
 *
 * @param nVal Value.
 * @return true in case of success.
 */
bool GDALAttribute::WriteInt64(int64_t nVal)
{
    const auto nDims = GetDimensionCount();
    std::vector<GUInt64> startIdx(1 + nDims, 0);
    std::vector<size_t> count(1 + nDims, 1);
    return Write(startIdx.data(), count.data(), nullptr, nullptr,
                 GDALExtendedDataType::Create(GDT_Int64), &nVal, &nVal,
                 sizeof(nVal));
}

/************************************************************************/
/*                                Write()                               */
/************************************************************************/

/** Write an attribute from a double value.
 *
 * Type conversion will be performed if needed. If the attribute contains
 * multiple values, only the first one will be updated.
 *
 * This is the same as the C function GDALAttributeWriteDouble().
 *
 * @param dfVal Value.
 * @return true in case of success.
 */
bool GDALAttribute::Write(double dfVal)
{
    const auto nDims = GetDimensionCount();
    std::vector<GUInt64> startIdx(1 + nDims, 0);
    std::vector<size_t> count(1 + nDims, 1);
    return Write(startIdx.data(), count.data(), nullptr, nullptr,
                 GDALExtendedDataType::Create(GDT_Float64), &dfVal, &dfVal,
                 sizeof(dfVal));
}

/************************************************************************/
/*                                Write()                               */
/************************************************************************/

/** Write an attribute from an array of strings.
 *
 * Type conversion will be performed if needed.
 *
 * Exactly GetTotalElementsCount() strings must be provided
 *
 * This is the same as the C function GDALAttributeWriteStringArray().
 *
 * @param vals Array of strings.
 * @return true in case of success.
 */
bool GDALAttribute::Write(CSLConstList vals)
{
    if (static_cast<size_t>(CSLCount(vals)) != GetTotalElementsCount())
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Invalid number of input values");
        return false;
    }
    const auto nDims = GetDimensionCount();
    std::vector<GUInt64> startIdx(1 + nDims, 0);
    std::vector<size_t> count(1 + nDims);
    const auto &dims = GetDimensions();
    for (size_t i = 0; i < nDims; i++)
        count[i] = static_cast<size_t>(dims[i]->GetSize());
    return Write(startIdx.data(), count.data(), nullptr, nullptr,
                 GDALExtendedDataType::CreateString(), vals, vals,
                 static_cast<size_t>(GetTotalElementsCount()) * sizeof(char *));
}

/************************************************************************/
/*                                Write()                               */
/************************************************************************/

/** Write an attribute from an array of int.
 *
 * Type conversion will be performed if needed.
 *
 * Exactly GetTotalElementsCount() strings must be provided
 *
 * This is the same as the C function GDALAttributeWriteIntArray()
 *
 * @param vals Array of int.
 * @param nVals Should be equal to GetTotalElementsCount().
 * @return true in case of success.
 */
bool GDALAttribute::Write(const int *vals, size_t nVals)
{
    if (nVals != GetTotalElementsCount())
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Invalid number of input values");
        return false;
    }
    const auto nDims = GetDimensionCount();
    std::vector<GUInt64> startIdx(1 + nDims, 0);
    std::vector<size_t> count(1 + nDims);
    const auto &dims = GetDimensions();
    for (size_t i = 0; i < nDims; i++)
        count[i] = static_cast<size_t>(dims[i]->GetSize());
    return Write(startIdx.data(), count.data(), nullptr, nullptr,
                 GDALExtendedDataType::Create(GDT_Int32), vals, vals,
                 static_cast<size_t>(GetTotalElementsCount()) * sizeof(GInt32));
}

/************************************************************************/
/*                                Write()                               */
/************************************************************************/

/** Write an attribute from an array of int64_t.
 *
 * Type conversion will be performed if needed.
 *
 * Exactly GetTotalElementsCount() strings must be provided
 *
 * This is the same as the C function GDALAttributeWriteLongArray()
 *
 * @param vals Array of int64_t.
 * @param nVals Should be equal to GetTotalElementsCount().
 * @return true in case of success.
 */
bool GDALAttribute::Write(const int64_t *vals, size_t nVals)
{
    if (nVals != GetTotalElementsCount())
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Invalid number of input values");
        return false;
    }
    const auto nDims = GetDimensionCount();
    std::vector<GUInt64> startIdx(1 + nDims, 0);
    std::vector<size_t> count(1 + nDims);
    const auto &dims = GetDimensions();
    for (size_t i = 0; i < nDims; i++)
        count[i] = static_cast<size_t>(dims[i]->GetSize());
    return Write(startIdx.data(), count.data(), nullptr, nullptr,
                 GDALExtendedDataType::Create(GDT_Int64), vals, vals,
                 static_cast<size_t>(GetTotalElementsCount()) *
                     sizeof(int64_t));
}

/************************************************************************/
/*                                Write()                               */
/************************************************************************/

/** Write an attribute from an array of double.
 *
 * Type conversion will be performed if needed.
 *
 * Exactly GetTotalElementsCount() strings must be provided
 *
 * This is the same as the C function GDALAttributeWriteDoubleArray()
 *
 * @param vals Array of double.
 * @param nVals Should be equal to GetTotalElementsCount().
 * @return true in case of success.
 */
bool GDALAttribute::Write(const double *vals, size_t nVals)
{
    if (nVals != GetTotalElementsCount())
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Invalid number of input values");
        return false;
    }
    const auto nDims = GetDimensionCount();
    std::vector<GUInt64> startIdx(1 + nDims, 0);
    std::vector<size_t> count(1 + nDims);
    const auto &dims = GetDimensions();
    for (size_t i = 0; i < nDims; i++)
        count[i] = static_cast<size_t>(dims[i]->GetSize());
    return Write(startIdx.data(), count.data(), nullptr, nullptr,
                 GDALExtendedDataType::Create(GDT_Float64), vals, vals,
                 static_cast<size_t>(GetTotalElementsCount()) * sizeof(double));
}

/************************************************************************/
/*                           GDALMDArray()                              */
/************************************************************************/

//! @cond Doxygen_Suppress
GDALMDArray::GDALMDArray(CPL_UNUSED const std::string &osParentName,
                         CPL_UNUSED const std::string &osName,
                         const std::string &osContext)
    :
#if !defined(COMPILER_WARNS_ABOUT_ABSTRACT_VBASE_INIT)
      GDALAbstractMDArray(osParentName, osName),
#endif
      m_osContext(osContext)
{
}

//! @endcond

/************************************************************************/
/*                           GetTotalCopyCost()                         */
/************************************************************************/

/** Return a total "cost" to copy the array.
 *
 * Used as a parameter for CopyFrom()
 */
GUInt64 GDALMDArray::GetTotalCopyCost() const
{
    return COPY_COST + GetAttributes().size() * GDALAttribute::COPY_COST +
           GetTotalElementsCount() * GetDataType().GetSize();
}

/************************************************************************/
/*                       CopyFromAllExceptValues()                      */
/************************************************************************/

//! @cond Doxygen_Suppress

bool GDALMDArray::CopyFromAllExceptValues(const GDALMDArray *poSrcArray,
                                          bool bStrict, GUInt64 &nCurCost,
                                          const GUInt64 nTotalCost,
                                          GDALProgressFunc pfnProgress,
                                          void *pProgressData)
{
    // Nodata setting must be one of the first things done for TileDB
    const void *pNoData = poSrcArray->GetRawNoDataValue();
    if (pNoData && poSrcArray->GetDataType() == GetDataType())
    {
        SetRawNoDataValue(pNoData);
    }

    const bool bThisIsUnscaledArray =
        dynamic_cast<GDALMDArrayUnscaled *>(this) != nullptr;
    auto attrs = poSrcArray->GetAttributes();
    for (const auto &attr : attrs)
    {
        const auto &osAttrName = attr->GetName();
        if (bThisIsUnscaledArray)
        {
            if (osAttrName == "missing_value" || osAttrName == "_FillValue" ||
                osAttrName == "valid_min" || osAttrName == "valid_max" ||
                osAttrName == "valid_range")
            {
                continue;
            }
        }

        auto dstAttr = CreateAttribute(osAttrName, attr->GetDimensionsSize(),
                                       attr->GetDataType());
        if (!dstAttr)
        {
            if (bStrict)
                return false;
            continue;
        }
        auto raw = attr->ReadAsRaw();
        if (!dstAttr->Write(raw.data(), raw.size()) && bStrict)
            return false;
    }
    if (!attrs.empty())
    {
        nCurCost += attrs.size() * GDALAttribute::COPY_COST;
        if (pfnProgress &&
            !pfnProgress(double(nCurCost) / nTotalCost, "", pProgressData))
            return false;
    }

    auto srcSRS = poSrcArray->GetSpatialRef();
    if (srcSRS)
    {
        SetSpatialRef(srcSRS.get());
    }

    const std::string &osUnit(poSrcArray->GetUnit());
    if (!osUnit.empty())
    {
        SetUnit(osUnit);
    }

    bool bGotValue = false;
    GDALDataType eOffsetStorageType = GDT_Unknown;
    const double dfOffset =
        poSrcArray->GetOffset(&bGotValue, &eOffsetStorageType);
    if (bGotValue)
    {
        SetOffset(dfOffset, eOffsetStorageType);
    }

    bGotValue = false;
    GDALDataType eScaleStorageType = GDT_Unknown;
    const double dfScale = poSrcArray->GetScale(&bGotValue, &eScaleStorageType);
    if (bGotValue)
    {
        SetScale(dfScale, eScaleStorageType);
    }

    return true;
}

//! @endcond

/************************************************************************/
/*                               CopyFrom()                             */
/************************************************************************/

/** Copy the content of an array into a new (generally empty) array.
 *
 * @param poSrcDS    Source dataset. Might be nullptr (but for correct behavior
 *                   of some output drivers this is not recommended)
 * @param poSrcArray Source array. Should NOT be nullptr.
 * @param bStrict Whether to enable strict mode. In strict mode, any error will
 *                stop the copy. In relaxed mode, the copy will be attempted to
 *                be pursued.
 * @param nCurCost  Should be provided as a variable initially set to 0.
 * @param nTotalCost Total cost from GetTotalCopyCost().
 * @param pfnProgress Progress callback, or nullptr.
 * @param pProgressData Progress user data, or nulptr.
 *
 * @return true in case of success (or partial success if bStrict == false).
 */
bool GDALMDArray::CopyFrom(CPL_UNUSED GDALDataset *poSrcDS,
                           const GDALMDArray *poSrcArray, bool bStrict,
                           GUInt64 &nCurCost, const GUInt64 nTotalCost,
                           GDALProgressFunc pfnProgress, void *pProgressData)
{
    if (pfnProgress == nullptr)
        pfnProgress = GDALDummyProgress;

    nCurCost += GDALMDArray::COPY_COST;

    if (!CopyFromAllExceptValues(poSrcArray, bStrict, nCurCost, nTotalCost,
                                 pfnProgress, pProgressData))
    {
        return false;
    }

    const auto &dims = poSrcArray->GetDimensions();
    const auto nDTSize = poSrcArray->GetDataType().GetSize();
    if (dims.empty())
    {
        std::vector<GByte> abyTmp(nDTSize);
        if (!(poSrcArray->Read(nullptr, nullptr, nullptr, nullptr,
                               GetDataType(), &abyTmp[0]) &&
              Write(nullptr, nullptr, nullptr, nullptr, GetDataType(),
                    &abyTmp[0])) &&
            bStrict)
        {
            return false;
        }
        nCurCost += GetTotalElementsCount() * GetDataType().GetSize();
        if (!pfnProgress(double(nCurCost) / nTotalCost, "", pProgressData))
            return false;
    }
    else
    {
        std::vector<GUInt64> arrayStartIdx(dims.size());
        std::vector<GUInt64> count(dims.size());
        for (size_t i = 0; i < dims.size(); i++)
        {
            count[i] = static_cast<size_t>(dims[i]->GetSize());
        }

        struct CopyFunc
        {
            GDALMDArray *poDstArray = nullptr;
            std::vector<GByte> abyTmp{};
            GDALProgressFunc pfnProgress = nullptr;
            void *pProgressData = nullptr;
            GUInt64 nCurCost = 0;
            GUInt64 nTotalCost = 0;
            GUInt64 nTotalBytesThisArray = 0;
            bool bStop = false;

            static bool f(GDALAbstractMDArray *l_poSrcArray,
                          const GUInt64 *chunkArrayStartIdx,
                          const size_t *chunkCount, GUInt64 iCurChunk,
                          GUInt64 nChunkCount, void *pUserData)
            {
                const auto &dt(l_poSrcArray->GetDataType());
                auto data = static_cast<CopyFunc *>(pUserData);
                auto poDstArray = data->poDstArray;
                if (!l_poSrcArray->Read(chunkArrayStartIdx, chunkCount, nullptr,
                                        nullptr, dt, &data->abyTmp[0]))
                {
                    return false;
                }
                bool bRet =
                    poDstArray->Write(chunkArrayStartIdx, chunkCount, nullptr,
                                      nullptr, dt, &data->abyTmp[0]);
                if (dt.NeedsFreeDynamicMemory())
                {
                    const auto l_nDTSize = dt.GetSize();
                    GByte *ptr = &data->abyTmp[0];
                    const size_t l_nDims(l_poSrcArray->GetDimensionCount());
                    size_t nEltCount = 1;
                    for (size_t i = 0; i < l_nDims; ++i)
                    {
                        nEltCount *= chunkCount[i];
                    }
                    for (size_t i = 0; i < nEltCount; i++)
                    {
                        dt.FreeDynamicMemory(ptr);
                        ptr += l_nDTSize;
                    }
                }
                if (!bRet)
                {
                    return false;
                }

                double dfCurCost =
                    double(data->nCurCost) + double(iCurChunk) / nChunkCount *
                                                 data->nTotalBytesThisArray;
                if (!data->pfnProgress(dfCurCost / data->nTotalCost, "",
                                       data->pProgressData))
                {
                    data->bStop = true;
                    return false;
                }

                return true;
            }
        };

        CopyFunc copyFunc;
        copyFunc.poDstArray = this;
        copyFunc.nCurCost = nCurCost;
        copyFunc.nTotalCost = nTotalCost;
        copyFunc.nTotalBytesThisArray = GetTotalElementsCount() * nDTSize;
        copyFunc.pfnProgress = pfnProgress;
        copyFunc.pProgressData = pProgressData;
        const char *pszSwathSize =
            CPLGetConfigOption("GDAL_SWATH_SIZE", nullptr);
        const size_t nMaxChunkSize =
            pszSwathSize
                ? static_cast<size_t>(
                      std::min(GIntBig(std::numeric_limits<size_t>::max() / 2),
                               CPLAtoGIntBig(pszSwathSize)))
                : static_cast<size_t>(
                      std::min(GIntBig(std::numeric_limits<size_t>::max() / 2),
                               GDALGetCacheMax64() / 4));
        const auto anChunkSizes(GetProcessingChunkSize(nMaxChunkSize));
        size_t nRealChunkSize = nDTSize;
        for (const auto &nChunkSize : anChunkSizes)
        {
            nRealChunkSize *= nChunkSize;
        }
        try
        {
            copyFunc.abyTmp.resize(nRealChunkSize);
        }
        catch (const std::exception &)
        {
            CPLError(CE_Failure, CPLE_OutOfMemory,
                     "Cannot allocate temporary buffer");
            nCurCost += copyFunc.nTotalBytesThisArray;
            return false;
        }
        if (copyFunc.nTotalBytesThisArray != 0 &&
            !const_cast<GDALMDArray *>(poSrcArray)
                 ->ProcessPerChunk(arrayStartIdx.data(), count.data(),
                                   anChunkSizes.data(), CopyFunc::f,
                                   &copyFunc) &&
            (bStrict || copyFunc.bStop))
        {
            nCurCost += copyFunc.nTotalBytesThisArray;
            return false;
        }
        nCurCost += copyFunc.nTotalBytesThisArray;
    }

    return true;
}

/************************************************************************/
/*                         GetStructuralInfo()                          */
/************************************************************************/

/** Return structural information on the array.
 *
 * This may be the compression, etc..
 *
 * The return value should not be freed and is valid until GDALMDArray is
 * released or this function called again.
 *
 * This is the same as the C function GDALMDArrayGetStructuralInfo().
 */
CSLConstList GDALMDArray::GetStructuralInfo() const
{
    return nullptr;
}

/************************************************************************/
/*                          AdviseRead()                                */
/************************************************************************/

/** Advise driver of upcoming read requests.
 *
 * Some GDAL drivers operate more efficiently if they know in advance what
 * set of upcoming read requests will be made.  The AdviseRead() method allows
 * an application to notify the driver of the region of interest.
 *
 * Many drivers just ignore the AdviseRead() call, but it can dramatically
 * accelerate access via some drivers. One such case is when reading through
 * a DAP dataset with the netCDF driver (a in-memory cache array is then created
 * with the region of interest defined by AdviseRead())
 *
 * This is the same as the C function GDALMDArrayAdviseRead().
 *
 * @param arrayStartIdx Values representing the starting index to read
 *                      in each dimension (in [0, aoDims[i].GetSize()-1] range).
 *                      Array of GetDimensionCount() values.
 *                      Can be nullptr as a synonymous for [0 for i in
 * range(GetDimensionCount() ]
 *
 * @param count         Values representing the number of values to extract in
 *                      each dimension.
 *                      Array of GetDimensionCount() values.
 *                      Can be nullptr as a synonymous for
 *                      [ aoDims[i].GetSize() - arrayStartIdx[i] for i in
 * range(GetDimensionCount() ]
 *
 * @param papszOptions Driver specific options, or nullptr. Consult driver
 * documentation.
 *
 * @return true in case of success (ignoring the advice is a success)
 *
 * @since GDAL 3.2
 */
bool GDALMDArray::AdviseRead(const GUInt64 *arrayStartIdx, const size_t *count,
                             CSLConstList papszOptions) const
{
    const auto nDimCount = GetDimensionCount();
    if (nDimCount == 0)
        return true;

    std::vector<GUInt64> tmp_arrayStartIdx;
    if (arrayStartIdx == nullptr)
    {
        tmp_arrayStartIdx.resize(nDimCount);
        arrayStartIdx = tmp_arrayStartIdx.data();
    }

    std::vector<size_t> tmp_count;
    if (count == nullptr)
    {
        tmp_count.resize(nDimCount);
        const auto &dims = GetDimensions();
        for (size_t i = 0; i < nDimCount; i++)
        {
            const GUInt64 nSize = dims[i]->GetSize() - arrayStartIdx[i];
#if SIZEOF_VOIDP < 8
            if (nSize != static_cast<size_t>(nSize))
            {
                CPLError(CE_Failure, CPLE_AppDefined, "Integer overflow");
                return false;
            }
#endif
            tmp_count[i] = static_cast<size_t>(nSize);
        }
        count = tmp_count.data();
    }

    std::vector<GInt64> tmp_arrayStep;
    std::vector<GPtrDiff_t> tmp_bufferStride;
    const GInt64 *arrayStep = nullptr;
    const GPtrDiff_t *bufferStride = nullptr;
    if (!CheckReadWriteParams(arrayStartIdx, count, arrayStep, bufferStride,
                              GDALExtendedDataType::Create(GDT_Unknown),
                              nullptr, nullptr, 0, tmp_arrayStep,
                              tmp_bufferStride))
    {
        return false;
    }

    return IAdviseRead(arrayStartIdx, count, papszOptions);
}

/************************************************************************/
/*                             IAdviseRead()                            */
/************************************************************************/

//! @cond Doxygen_Suppress
bool GDALMDArray::IAdviseRead(const GUInt64 *, const size_t *,
                              CSLConstList /* papszOptions*/) const
{
    return true;
}

//! @endcond

/************************************************************************/
/*                            MassageName()                             */
/************************************************************************/

//! @cond Doxygen_Suppress
/*static*/ std::string GDALMDArray::MassageName(const std::string &inputName)
{
    std::string ret;
    for (const char ch : inputName)
    {
        if (!isalnum(static_cast<unsigned char>(ch)))
            ret += '_';
        else
            ret += ch;
    }
    return ret;
}

//! @endcond

/************************************************************************/
/*                         GetCacheRootGroup()                          */
/************************************************************************/

//! @cond Doxygen_Suppress
std::shared_ptr<GDALGroup>
GDALMDArray::GetCacheRootGroup(bool bCanCreate,
                               std::string &osCacheFilenameOut) const
{
    const auto &osFilename = GetFilename();
    if (osFilename.empty())
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Cannot cache an array with an empty filename");
        return nullptr;
    }

    osCacheFilenameOut = osFilename + ".gmac";
    if (STARTS_WITH(osFilename.c_str(), "/vsicurl/http"))
    {
        const auto nPosQuestionMark = osFilename.find('?');
        if (nPosQuestionMark != std::string::npos)
        {
            osCacheFilenameOut =
                osFilename.substr(0, nPosQuestionMark)
                    .append(".gmac")
                    .append(osFilename.substr(nPosQuestionMark));
        }
    }
    const char *pszProxy = PamGetProxy(osCacheFilenameOut.c_str());
    if (pszProxy != nullptr)
        osCacheFilenameOut = pszProxy;

    std::unique_ptr<GDALDataset> poDS;
    VSIStatBufL sStat;
    if (VSIStatL(osCacheFilenameOut.c_str(), &sStat) == 0)
    {
        poDS.reset(GDALDataset::Open(osCacheFilenameOut.c_str(),
                                     GDAL_OF_MULTIDIM_RASTER | GDAL_OF_UPDATE,
                                     nullptr, nullptr, nullptr));
    }
    if (poDS)
    {
        CPLDebug("GDAL", "Opening cache %s", osCacheFilenameOut.c_str());
        return poDS->GetRootGroup();
    }

    if (bCanCreate)
    {
        const char *pszDrvName = "netCDF";
        GDALDriver *poDrv = GetGDALDriverManager()->GetDriverByName(pszDrvName);
        if (poDrv == nullptr)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Cannot get driver %s",
                     pszDrvName);
            return nullptr;
        }
        {
            CPLErrorHandlerPusher oHandlerPusher(CPLQuietErrorHandler);
            CPLErrorStateBackuper oErrorStateBackuper;
            poDS.reset(poDrv->CreateMultiDimensional(osCacheFilenameOut.c_str(),
                                                     nullptr, nullptr));
        }
        if (!poDS)
        {
            pszProxy = PamAllocateProxy(osCacheFilenameOut.c_str());
            if (pszProxy)
            {
                osCacheFilenameOut = pszProxy;
                poDS.reset(poDrv->CreateMultiDimensional(
                    osCacheFilenameOut.c_str(), nullptr, nullptr));
            }
        }
        if (poDS)
        {
            CPLDebug("GDAL", "Creating cache %s", osCacheFilenameOut.c_str());
            return poDS->GetRootGroup();
        }
        else
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Cannot create %s. Set the GDAL_PAM_PROXY_DIR "
                     "configuration option to write the cache in "
                     "another directory",
                     osCacheFilenameOut.c_str());
        }
    }

    return nullptr;
}

//! @endcond

/************************************************************************/
/*                              Cache()                                 */
/************************************************************************/

/** Cache the content of the array into an auxiliary filename.
 *
 * The main purpose of this method is to be able to cache views that are
 * expensive to compute, such as transposed arrays.
 *
 * The array will be stored in a file whose name is the one of
 * GetFilename(), with an extra .gmac extension (stands for GDAL
 * Multidimensional Array Cache). The cache is a netCDF dataset.
 *
 * If the .gmac file cannot be written next to the dataset, the
 * GDAL_PAM_PROXY_DIR will be used, if set, to write the cache file into that
 * directory.
 *
 * The GDALMDArray::Read() method will automatically use the cache when it
 * exists. There is no timestamp checks between the source array and the cached
 * array. If the source arrays changes, the cache must be manually deleted.
 *
 * This is the same as the C function GDALMDArrayCache()
 *
 * @note Driver implementation: optionally implemented.
 *
 * @param papszOptions List of options, null terminated, or NULL. Currently
 *                     the only option supported is BLOCKSIZE=bs0,bs1,...,bsN
 *                     to specify the block size of the cached array.
 * @return true in case of success.
 */
bool GDALMDArray::Cache(CSLConstList papszOptions) const
{
    std::string osCacheFilename;
    auto poRG = GetCacheRootGroup(true, osCacheFilename);
    if (!poRG)
        return false;

    const std::string osCachedArrayName(MassageName(GetFullName()));
    if (poRG->OpenMDArray(osCachedArrayName))
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "An array with same name %s already exists in %s",
                 osCachedArrayName.c_str(), osCacheFilename.c_str());
        return false;
    }

    CPLStringList aosOptions;
    aosOptions.SetNameValue("COMPRESS", "DEFLATE");
    const auto &aoDims = GetDimensions();
    std::vector<std::shared_ptr<GDALDimension>> aoNewDims;
    if (!aoDims.empty())
    {
        std::string osBlockSize(
            CSLFetchNameValueDef(papszOptions, "BLOCKSIZE", ""));
        if (osBlockSize.empty())
        {
            const auto anBlockSize = GetBlockSize();
            int idxDim = 0;
            for (auto nBlockSize : anBlockSize)
            {
                if (idxDim > 0)
                    osBlockSize += ',';
                if (nBlockSize == 0)
                    nBlockSize = 256;
                nBlockSize = std::min(nBlockSize, aoDims[idxDim]->GetSize());
                osBlockSize +=
                    std::to_string(static_cast<uint64_t>(nBlockSize));
                idxDim++;
            }
        }
        aosOptions.SetNameValue("BLOCKSIZE", osBlockSize.c_str());

        int idxDim = 0;
        for (const auto &poDim : aoDims)
        {
            auto poNewDim = poRG->CreateDimension(
                osCachedArrayName + '_' + std::to_string(idxDim),
                poDim->GetType(), poDim->GetDirection(), poDim->GetSize());
            if (!poNewDim)
                return false;
            aoNewDims.emplace_back(poNewDim);
            idxDim++;
        }
    }

    auto poCachedArray = poRG->CreateMDArray(osCachedArrayName, aoNewDims,
                                             GetDataType(), aosOptions.List());
    if (!poCachedArray)
    {
        CPLError(CE_Failure, CPLE_NotSupported, "Cannot create %s in %s",
                 osCachedArrayName.c_str(), osCacheFilename.c_str());
        return false;
    }

    GUInt64 nCost = 0;
    return poCachedArray->CopyFrom(nullptr, this,
                                   false,  // strict
                                   nCost, GetTotalCopyCost(), nullptr, nullptr);
}

/************************************************************************/
/*                               Read()                                 */
/************************************************************************/

bool GDALMDArray::Read(const GUInt64 *arrayStartIdx, const size_t *count,
                       const GInt64 *arrayStep,         // step in elements
                       const GPtrDiff_t *bufferStride,  // stride in elements
                       const GDALExtendedDataType &bufferDataType,
                       void *pDstBuffer, const void *pDstBufferAllocStart,
                       size_t nDstBufferAllocSize) const
{
    if (!m_bHasTriedCachedArray)
    {
        m_bHasTriedCachedArray = true;
        if (IsCacheable())
        {
            const auto &osFilename = GetFilename();
            if (!osFilename.empty() &&
                !EQUAL(CPLGetExtensionSafe(osFilename.c_str()).c_str(), "gmac"))
            {
                std::string osCacheFilename;
                auto poRG = GetCacheRootGroup(false, osCacheFilename);
                if (poRG)
                {
                    const std::string osCachedArrayName(
                        MassageName(GetFullName()));
                    m_poCachedArray = poRG->OpenMDArray(osCachedArrayName);
                    if (m_poCachedArray)
                    {
                        const auto &dims = GetDimensions();
                        const auto &cachedDims =
                            m_poCachedArray->GetDimensions();
                        const size_t nDims = dims.size();
                        bool ok =
                            m_poCachedArray->GetDataType() == GetDataType() &&
                            cachedDims.size() == nDims;
                        for (size_t i = 0; ok && i < nDims; ++i)
                        {
                            ok = dims[i]->GetSize() == cachedDims[i]->GetSize();
                        }
                        if (ok)
                        {
                            CPLDebug("GDAL", "Cached array for %s found in %s",
                                     osCachedArrayName.c_str(),
                                     osCacheFilename.c_str());
                        }
                        else
                        {
                            CPLError(CE_Warning, CPLE_AppDefined,
                                     "Cached array %s in %s has incompatible "
                                     "characteristics with current array.",
                                     osCachedArrayName.c_str(),
                                     osCacheFilename.c_str());
                            m_poCachedArray.reset();
                        }
                    }
                }
            }
        }
    }

    const auto array = m_poCachedArray ? m_poCachedArray.get() : this;
    if (!array->GetDataType().CanConvertTo(bufferDataType))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Array data type is not convertible to buffer data type");
        return false;
    }

    std::vector<GInt64> tmp_arrayStep;
    std::vector<GPtrDiff_t> tmp_bufferStride;
    if (!array->CheckReadWriteParams(arrayStartIdx, count, arrayStep,
                                     bufferStride, bufferDataType, pDstBuffer,
                                     pDstBufferAllocStart, nDstBufferAllocSize,
                                     tmp_arrayStep, tmp_bufferStride))
    {
        return false;
    }

    return array->IRead(arrayStartIdx, count, arrayStep, bufferStride,
                        bufferDataType, pDstBuffer);
}

/************************************************************************/
/*                          GetRootGroup()                              */
/************************************************************************/

/** Return the root group to which this arrays belongs too.
 *
 * Note that arrays may be free standing and some drivers may not implement
 * this method, hence nullptr may be returned.
 *
 * It is used internally by the GetResampled() method to detect if GLT
 * orthorectification is available.
 *
 * @return the root group, or nullptr.
 * @since GDAL 3.8
 */
std::shared_ptr<GDALGroup> GDALMDArray::GetRootGroup() const
{
    return nullptr;
}

//! @cond Doxygen_Suppress

/************************************************************************/
/*                       IsTransposedRequest()                          */
/************************************************************************/

bool GDALMDArray::IsTransposedRequest(
    const size_t *count,
    const GPtrDiff_t *bufferStride) const  // stride in elements
{
    /*
    For example:
    count = [2,3,4]
    strides = [12, 4, 1]            (2-1)*12+(3-1)*4+(4-1)*1=23   ==> row major
    stride [12, 1, 3]            (2-1)*12+(3-1)*1+(4-1)*3=23   ==>
    (axis[0],axis[2],axis[1]) transposition [1, 8, 2]             (2-1)*1+
    (3-1)*8+(4-1)*2=23   ==> (axis[2],axis[1],axis[0]) transposition
    */
    const size_t nDims(GetDimensionCount());
    size_t nCurStrideForRowMajorStrides = 1;
    bool bRowMajorStrides = true;
    size_t nElts = 1;
    size_t nLastIdx = 0;
    for (size_t i = nDims; i > 0;)
    {
        --i;
        if (bufferStride[i] < 0)
            return false;
        if (static_cast<size_t>(bufferStride[i]) !=
            nCurStrideForRowMajorStrides)
        {
            bRowMajorStrides = false;
        }
        // Integer overflows have already been checked in CheckReadWriteParams()
        nCurStrideForRowMajorStrides *= count[i];
        nElts *= count[i];
        nLastIdx += static_cast<size_t>(bufferStride[i]) * (count[i] - 1);
    }
    if (bRowMajorStrides)
        return false;
    return nLastIdx == nElts - 1;
}

/************************************************************************/
/*                   CopyToFinalBufferSameDataType()                    */
/************************************************************************/

template <size_t N>
void CopyToFinalBufferSameDataType(const void *pSrcBuffer, void *pDstBuffer,
                                   size_t nDims, const size_t *count,
                                   const GPtrDiff_t *bufferStride)
{
    std::vector<size_t> anStackCount(nDims);
    std::vector<GByte *> pabyDstBufferStack(nDims + 1);
    const GByte *pabySrcBuffer = static_cast<const GByte *>(pSrcBuffer);
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnull-dereference"
#endif
    pabyDstBufferStack[0] = static_cast<GByte *>(pDstBuffer);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    size_t iDim = 0;

lbl_next_depth:
    if (iDim == nDims - 1)
    {
        size_t n = count[iDim];
        GByte *pabyDstBuffer = pabyDstBufferStack[iDim];
        const auto bufferStrideLastDim = bufferStride[iDim] * N;
        while (n > 0)
        {
            --n;
            memcpy(pabyDstBuffer, pabySrcBuffer, N);
            pabyDstBuffer += bufferStrideLastDim;
            pabySrcBuffer += N;
        }
    }
    else
    {
        anStackCount[iDim] = count[iDim];
        while (true)
        {
            ++iDim;
            pabyDstBufferStack[iDim] = pabyDstBufferStack[iDim - 1];
            goto lbl_next_depth;
        lbl_return_to_caller_in_loop:
            --iDim;
            --anStackCount[iDim];
            if (anStackCount[iDim] == 0)
                break;
            pabyDstBufferStack[iDim] += bufferStride[iDim] * N;
        }
    }
    if (iDim > 0)
        goto lbl_return_to_caller_in_loop;
}

/************************************************************************/
/*                        CopyToFinalBuffer()                           */
/************************************************************************/

static void CopyToFinalBuffer(const void *pSrcBuffer,
                              const GDALExtendedDataType &eSrcDataType,
                              void *pDstBuffer,
                              const GDALExtendedDataType &eDstDataType,
                              size_t nDims, const size_t *count,
                              const GPtrDiff_t *bufferStride)
{
    const size_t nSrcDataTypeSize(eSrcDataType.GetSize());
    // Use specialized implementation for well-known data types when no
    // type conversion is needed
    if (eSrcDataType == eDstDataType)
    {
        if (nSrcDataTypeSize == 1)
        {
            CopyToFinalBufferSameDataType<1>(pSrcBuffer, pDstBuffer, nDims,
                                             count, bufferStride);
            return;
        }
        else if (nSrcDataTypeSize == 2)
        {
            CopyToFinalBufferSameDataType<2>(pSrcBuffer, pDstBuffer, nDims,
                                             count, bufferStride);
            return;
        }
        else if (nSrcDataTypeSize == 4)
        {
            CopyToFinalBufferSameDataType<4>(pSrcBuffer, pDstBuffer, nDims,
                                             count, bufferStride);
            return;
        }
        else if (nSrcDataTypeSize == 8)
        {
            CopyToFinalBufferSameDataType<8>(pSrcBuffer, pDstBuffer, nDims,
                                             count, bufferStride);
            return;
        }
    }

    const size_t nDstDataTypeSize(eDstDataType.GetSize());
    std::vector<size_t> anStackCount(nDims);
    std::vector<GByte *> pabyDstBufferStack(nDims + 1);
    const GByte *pabySrcBuffer = static_cast<const GByte *>(pSrcBuffer);
    pabyDstBufferStack[0] = static_cast<GByte *>(pDstBuffer);
    size_t iDim = 0;

lbl_next_depth:
    if (iDim == nDims - 1)
    {
        GDALExtendedDataType::CopyValues(pabySrcBuffer, eSrcDataType, 1,
                                         pabyDstBufferStack[iDim], eDstDataType,
                                         bufferStride[iDim], count[iDim]);
        pabySrcBuffer += count[iDim] * nSrcDataTypeSize;
    }
    else
    {
        anStackCount[iDim] = count[iDim];
        while (true)
        {
            ++iDim;
            pabyDstBufferStack[iDim] = pabyDstBufferStack[iDim - 1];
            goto lbl_next_depth;
        lbl_return_to_caller_in_loop:
            --iDim;
            --anStackCount[iDim];
            if (anStackCount[iDim] == 0)
                break;
            pabyDstBufferStack[iDim] += bufferStride[iDim] * nDstDataTypeSize;
        }
    }
    if (iDim > 0)
        goto lbl_return_to_caller_in_loop;
}

/************************************************************************/
/*                      TransposeLast2Dims()                            */
/************************************************************************/

static bool TransposeLast2Dims(void *pDstBuffer,
                               const GDALExtendedDataType &eDT,
                               const size_t nDims, const size_t *count,
                               const size_t nEltsNonLast2Dims)
{
    const size_t nEltsLast2Dims = count[nDims - 2] * count[nDims - 1];
    const auto nDTSize = eDT.GetSize();
    void *pTempBufferForLast2DimsTranspose =
        VSI_MALLOC2_VERBOSE(nEltsLast2Dims, nDTSize);
    if (pTempBufferForLast2DimsTranspose == nullptr)
        return false;

    GByte *pabyDstBuffer = static_cast<GByte *>(pDstBuffer);
    for (size_t i = 0; i < nEltsNonLast2Dims; ++i)
    {
        GDALTranspose2D(pabyDstBuffer, eDT.GetNumericDataType(),
                        pTempBufferForLast2DimsTranspose,
                        eDT.GetNumericDataType(), count[nDims - 1],
                        count[nDims - 2]);
        memcpy(pabyDstBuffer, pTempBufferForLast2DimsTranspose,
               nDTSize * nEltsLast2Dims);
        pabyDstBuffer += nDTSize * nEltsLast2Dims;
    }

    VSIFree(pTempBufferForLast2DimsTranspose);

    return true;
}

/************************************************************************/
/*                      ReadForTransposedRequest()                      */
/************************************************************************/

// Using the netCDF/HDF5 APIs to read a slice with strides that express a
// transposed view yield to extremely poor/unusable performance. This fixes
// this by using temporary memory to read in a contiguous buffer in a
// row-major order, and then do the transposition to the final buffer.

bool GDALMDArray::ReadForTransposedRequest(
    const GUInt64 *arrayStartIdx, const size_t *count, const GInt64 *arrayStep,
    const GPtrDiff_t *bufferStride, const GDALExtendedDataType &bufferDataType,
    void *pDstBuffer) const
{
    const size_t nDims(GetDimensionCount());
    if (nDims == 0)
    {
        CPLAssert(false);
        return false;  // shouldn't happen
    }
    size_t nElts = 1;
    for (size_t i = 0; i < nDims; ++i)
        nElts *= count[i];

    std::vector<GPtrDiff_t> tmpBufferStrides(nDims);
    tmpBufferStrides.back() = 1;
    for (size_t i = nDims - 1; i > 0;)
    {
        --i;
        tmpBufferStrides[i] = tmpBufferStrides[i + 1] * count[i + 1];
    }

    const auto &eDT = GetDataType();
    const auto nDTSize = eDT.GetSize();
    if (bufferDataType == eDT && nDims >= 2 && bufferStride[nDims - 2] == 1 &&
        static_cast<size_t>(bufferStride[nDims - 1]) == count[nDims - 2] &&
        (nDTSize == 1 || nDTSize == 2 || nDTSize == 4 || nDTSize == 8))
    {
        // Optimization of the optimization if only the last 2 dims are
        // transposed that saves on temporary buffer allocation
        const size_t nEltsLast2Dims = count[nDims - 2] * count[nDims - 1];
        size_t nCurStrideForRowMajorStrides = nEltsLast2Dims;
        bool bRowMajorStridesForNonLast2Dims = true;
        size_t nEltsNonLast2Dims = 1;
        for (size_t i = nDims - 2; i > 0;)
        {
            --i;
            if (static_cast<size_t>(bufferStride[i]) !=
                nCurStrideForRowMajorStrides)
            {
                bRowMajorStridesForNonLast2Dims = false;
            }
            // Integer overflows have already been checked in
            // CheckReadWriteParams()
            nCurStrideForRowMajorStrides *= count[i];
            nEltsNonLast2Dims *= count[i];
        }
        if (bRowMajorStridesForNonLast2Dims)
        {
            // We read in the final buffer!
            if (!IRead(arrayStartIdx, count, arrayStep, tmpBufferStrides.data(),
                       eDT, pDstBuffer))
            {
                return false;
            }

            return TransposeLast2Dims(pDstBuffer, eDT, nDims, count,
                                      nEltsNonLast2Dims);
        }
    }

    void *pTempBuffer = VSI_MALLOC2_VERBOSE(nElts, eDT.GetSize());
    if (pTempBuffer == nullptr)
        return false;

    if (!IRead(arrayStartIdx, count, arrayStep, tmpBufferStrides.data(), eDT,
               pTempBuffer))
    {
        VSIFree(pTempBuffer);
        return false;
    }
    CopyToFinalBuffer(pTempBuffer, eDT, pDstBuffer, bufferDataType, nDims,
                      count, bufferStride);

    if (eDT.NeedsFreeDynamicMemory())
    {
        GByte *pabyPtr = static_cast<GByte *>(pTempBuffer);
        for (size_t i = 0; i < nElts; ++i)
        {
            eDT.FreeDynamicMemory(pabyPtr);
            pabyPtr += nDTSize;
        }
    }

    VSIFree(pTempBuffer);
    return true;
}

/************************************************************************/
/*               IsStepOneContiguousRowMajorOrderedSameDataType()       */
/************************************************************************/

// Returns true if at all following conditions are met:
// arrayStep[] == 1, bufferDataType == GetDataType() and bufferStride[]
// defines a row-major ordered contiguous buffer.
bool GDALMDArray::IsStepOneContiguousRowMajorOrderedSameDataType(
    const size_t *count, const GInt64 *arrayStep,
    const GPtrDiff_t *bufferStride,
    const GDALExtendedDataType &bufferDataType) const
{
    if (bufferDataType != GetDataType())
        return false;
    size_t nExpectedStride = 1;
    for (size_t i = GetDimensionCount(); i > 0;)
    {
        --i;
        if (arrayStep[i] != 1 || bufferStride[i] < 0 ||
            static_cast<size_t>(bufferStride[i]) != nExpectedStride)
        {
            return false;
        }
        nExpectedStride *= count[i];
    }
    return true;
}

/************************************************************************/
/*                      ReadUsingContiguousIRead()                      */
/************************************************************************/

// Used for example by the TileDB driver when requesting it with
// arrayStep[] != 1, bufferDataType != GetDataType() or bufferStride[]
// not defining a row-major ordered contiguous buffer.
// Should only be called when at least one of the above conditions are true,
// which can be tested with IsStepOneContiguousRowMajorOrderedSameDataType()
// returning none.
// This method will call IRead() again with arrayStep[] == 1,
// bufferDataType == GetDataType() and bufferStride[] defining a row-major
// ordered contiguous buffer, on a temporary buffer. And it will rearrange the
// content of that temporary buffer onto pDstBuffer.
bool GDALMDArray::ReadUsingContiguousIRead(
    const GUInt64 *arrayStartIdx, const size_t *count, const GInt64 *arrayStep,
    const GPtrDiff_t *bufferStride, const GDALExtendedDataType &bufferDataType,
    void *pDstBuffer) const
{
    const size_t nDims(GetDimensionCount());
    std::vector<GUInt64> anTmpStartIdx(nDims);
    std::vector<size_t> anTmpCount(nDims);
    const auto &oType = GetDataType();
    size_t nMemArraySize = oType.GetSize();
    std::vector<GPtrDiff_t> anTmpStride(nDims);
    GPtrDiff_t nStride = 1;
    for (size_t i = nDims; i > 0;)
    {
        --i;
        if (arrayStep[i] > 0)
            anTmpStartIdx[i] = arrayStartIdx[i];
        else
            anTmpStartIdx[i] =
                arrayStartIdx[i] - (count[i] - 1) * (-arrayStep[i]);
        const uint64_t nCount =
            (count[i] - 1) * static_cast<uint64_t>(std::abs(arrayStep[i])) + 1;
        if (nCount > std::numeric_limits<size_t>::max() / nMemArraySize)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Read() failed due to too large memory requirement");
            return false;
        }
        anTmpCount[i] = static_cast<size_t>(nCount);
        nMemArraySize *= anTmpCount[i];
        anTmpStride[i] = nStride;
        nStride *= anTmpCount[i];
    }
    std::unique_ptr<void, decltype(&VSIFree)> pTmpBuffer(
        VSI_MALLOC_VERBOSE(nMemArraySize), VSIFree);
    if (!pTmpBuffer)
        return false;
    if (!IRead(anTmpStartIdx.data(), anTmpCount.data(),
               std::vector<GInt64>(nDims, 1).data(),  // steps
               anTmpStride.data(), oType, pTmpBuffer.get()))
    {
        return false;
    }
    std::vector<std::shared_ptr<GDALDimension>> apoTmpDims(nDims);
    for (size_t i = 0; i < nDims; ++i)
    {
        if (arrayStep[i] > 0)
            anTmpStartIdx[i] = 0;
        else
            anTmpStartIdx[i] = anTmpCount[i] - 1;
        apoTmpDims[i] = std::make_shared<GDALDimension>(
            std::string(), std::string(), std::string(), std::string(),
            anTmpCount[i]);
    }
    auto poMEMArray =
        MEMMDArray::Create(std::string(), std::string(), apoTmpDims, oType);
    return poMEMArray->Init(static_cast<GByte *>(pTmpBuffer.get())) &&
           poMEMArray->Read(anTmpStartIdx.data(), count, arrayStep,
                            bufferStride, bufferDataType, pDstBuffer);
}

//! @endcond

/************************************************************************/
/*                       GDALSlicedMDArray                              */
/************************************************************************/

class GDALSlicedMDArray final : public GDALPamMDArray
{
  private:
    std::shared_ptr<GDALMDArray> m_poParent{};
    std::vector<std::shared_ptr<GDALDimension>> m_dims{};
    std::vector<size_t> m_mapDimIdxToParentDimIdx{};  // of size m_dims.size()
    std::vector<Range>
        m_parentRanges{};  // of size m_poParent->GetDimensionCount()

    mutable std::vector<GUInt64> m_parentStart;
    mutable std::vector<size_t> m_parentCount;
    mutable std::vector<GInt64> m_parentStep;
    mutable std::vector<GPtrDiff_t> m_parentStride;

    void PrepareParentArrays(const GUInt64 *arrayStartIdx, const size_t *count,
                             const GInt64 *arrayStep,
                             const GPtrDiff_t *bufferStride) const;

  protected:
    explicit GDALSlicedMDArray(
        const std::shared_ptr<GDALMDArray> &poParent,
        const std::string &viewExpr,
        std::vector<std::shared_ptr<GDALDimension>> &&dims,
        std::vector<size_t> &&mapDimIdxToParentDimIdx,
        std::vector<Range> &&parentRanges)
        : GDALAbstractMDArray(std::string(), "Sliced view of " +
                                                 poParent->GetFullName() +
                                                 " (" + viewExpr + ")"),
          GDALPamMDArray(std::string(),
                         "Sliced view of " + poParent->GetFullName() + " (" +
                             viewExpr + ")",
                         GDALPamMultiDim::GetPAM(poParent),
                         poParent->GetContext()),
          m_poParent(std::move(poParent)), m_dims(std::move(dims)),
          m_mapDimIdxToParentDimIdx(std::move(mapDimIdxToParentDimIdx)),
          m_parentRanges(std::move(parentRanges)),
          m_parentStart(m_poParent->GetDimensionCount()),
          m_parentCount(m_poParent->GetDimensionCount(), 1),
          m_parentStep(m_poParent->GetDimensionCount()),
          m_parentStride(m_poParent->GetDimensionCount())
    {
    }

    bool IRead(const GUInt64 *arrayStartIdx, const size_t *count,
               const GInt64 *arrayStep, const GPtrDiff_t *bufferStride,
               const GDALExtendedDataType &bufferDataType,
               void *pDstBuffer) const override;

    bool IWrite(const GUInt64 *arrayStartIdx, const size_t *count,
                const GInt64 *arrayStep, const GPtrDiff_t *bufferStride,
                const GDALExtendedDataType &bufferDataType,
                const void *pSrcBuffer) override;

    bool IAdviseRead(const GUInt64 *arrayStartIdx, const size_t *count,
                     CSLConstList papszOptions) const override;

  public:
    static std::shared_ptr<GDALSlicedMDArray>
    Create(const std::shared_ptr<GDALMDArray> &poParent,
           const std::string &viewExpr,
           std::vector<std::shared_ptr<GDALDimension>> &&dims,
           std::vector<size_t> &&mapDimIdxToParentDimIdx,
           std::vector<Range> &&parentRanges)
    {
        CPLAssert(dims.size() == mapDimIdxToParentDimIdx.size());
        CPLAssert(parentRanges.size() == poParent->GetDimensionCount());

        auto newAr(std::shared_ptr<GDALSlicedMDArray>(new GDALSlicedMDArray(
            poParent, viewExpr, std::move(dims),
            std::move(mapDimIdxToParentDimIdx), std::move(parentRanges))));
        newAr->SetSelf(newAr);
        return newAr;
    }

    bool IsWritable() const override
    {
        return m_poParent->IsWritable();
    }

    const std::string &GetFilename() const override
    {
        return m_poParent->GetFilename();
    }

    const std::vector<std::shared_ptr<GDALDimension>> &
    GetDimensions() const override
    {
        return m_dims;
    }

    const GDALExtendedDataType &GetDataType() const override
    {
        return m_poParent->GetDataType();
    }

    const std::string &GetUnit() const override
    {
        return m_poParent->GetUnit();
    }

    // bool SetUnit(const std::string& osUnit) override  { return
    // m_poParent->SetUnit(osUnit); }

    std::shared_ptr<OGRSpatialReference> GetSpatialRef() const override
    {
        auto poSrcSRS = m_poParent->GetSpatialRef();
        if (!poSrcSRS)
            return nullptr;
        auto srcMapping = poSrcSRS->GetDataAxisToSRSAxisMapping();
        std::vector<int> dstMapping;
        for (int srcAxis : srcMapping)
        {
            bool bFound = false;
            for (size_t i = 0; i < m_mapDimIdxToParentDimIdx.size(); i++)
            {
                if (static_cast<int>(m_mapDimIdxToParentDimIdx[i]) ==
                    srcAxis - 1)
                {
                    dstMapping.push_back(static_cast<int>(i) + 1);
                    bFound = true;
                    break;
                }
            }
            if (!bFound)
            {
                dstMapping.push_back(0);
            }
        }
        auto poClone(std::shared_ptr<OGRSpatialReference>(poSrcSRS->Clone()));
        poClone->SetDataAxisToSRSAxisMapping(dstMapping);
        return poClone;
    }

    const void *GetRawNoDataValue() const override
    {
        return m_poParent->GetRawNoDataValue();
    }

    // bool SetRawNoDataValue(const void* pRawNoData) override { return
    // m_poParent->SetRawNoDataValue(pRawNoData); }

    double GetOffset(bool *pbHasOffset,
                     GDALDataType *peStorageType) const override
    {
        return m_poParent->GetOffset(pbHasOffset, peStorageType);
    }

    double GetScale(bool *pbHasScale,
                    GDALDataType *peStorageType) const override
    {
        return m_poParent->GetScale(pbHasScale, peStorageType);
    }

    // bool SetOffset(double dfOffset) override { return
    // m_poParent->SetOffset(dfOffset); }

    // bool SetScale(double dfScale) override { return
    // m_poParent->SetScale(dfScale); }

    std::vector<GUInt64> GetBlockSize() const override
    {
        std::vector<GUInt64> ret(GetDimensionCount());
        const auto parentBlockSize(m_poParent->GetBlockSize());
        for (size_t i = 0; i < m_mapDimIdxToParentDimIdx.size(); ++i)
        {
            const auto iOldAxis = m_mapDimIdxToParentDimIdx[i];
            if (iOldAxis != static_cast<size_t>(-1))
            {
                ret[i] = parentBlockSize[iOldAxis];
            }
        }
        return ret;
    }

    std::shared_ptr<GDALAttribute>
    GetAttribute(const std::string &osName) const override
    {
        return m_poParent->GetAttribute(osName);
    }

    std::vector<std::shared_ptr<GDALAttribute>>
    GetAttributes(CSLConstList papszOptions = nullptr) const override
    {
        return m_poParent->GetAttributes(papszOptions);
    }
};

/************************************************************************/
/*                        PrepareParentArrays()                         */
/************************************************************************/

void GDALSlicedMDArray::PrepareParentArrays(
    const GUInt64 *arrayStartIdx, const size_t *count, const GInt64 *arrayStep,
    const GPtrDiff_t *bufferStride) const
{
    const size_t nParentDimCount = m_parentRanges.size();
    for (size_t i = 0; i < nParentDimCount; i++)
    {
        // For dimensions in parent that have no existence in sliced array
        m_parentStart[i] = m_parentRanges[i].m_nStartIdx;
    }

    for (size_t i = 0; i < m_dims.size(); i++)
    {
        const auto iParent = m_mapDimIdxToParentDimIdx[i];
        if (iParent != static_cast<size_t>(-1))
        {
            m_parentStart[iParent] =
                m_parentRanges[iParent].m_nIncr >= 0
                    ? m_parentRanges[iParent].m_nStartIdx +
                          arrayStartIdx[i] * m_parentRanges[iParent].m_nIncr
                    : m_parentRanges[iParent].m_nStartIdx -
                          arrayStartIdx[i] *
                              static_cast<GUInt64>(
                                  -m_parentRanges[iParent].m_nIncr);
            m_parentCount[iParent] = count[i];
            if (arrayStep)
            {
                m_parentStep[iParent] =
                    count[i] == 1 ? 1 :
                                  // other checks should have ensured this does
                        // not overflow
                        arrayStep[i] * m_parentRanges[iParent].m_nIncr;
            }
            if (bufferStride)
            {
                m_parentStride[iParent] = bufferStride[i];
            }
        }
    }
}

/************************************************************************/
/*                             IRead()                                  */
/************************************************************************/

bool GDALSlicedMDArray::IRead(const GUInt64 *arrayStartIdx, const size_t *count,
                              const GInt64 *arrayStep,
                              const GPtrDiff_t *bufferStride,
                              const GDALExtendedDataType &bufferDataType,
                              void *pDstBuffer) const
{
    PrepareParentArrays(arrayStartIdx, count, arrayStep, bufferStride);
    return m_poParent->Read(m_parentStart.data(), m_parentCount.data(),
                            m_parentStep.data(), m_parentStride.data(),
                            bufferDataType, pDstBuffer);
}

/************************************************************************/
/*                             IWrite()                                  */
/************************************************************************/

bool GDALSlicedMDArray::IWrite(const GUInt64 *arrayStartIdx,
                               const size_t *count, const GInt64 *arrayStep,
                               const GPtrDiff_t *bufferStride,
                               const GDALExtendedDataType &bufferDataType,
                               const void *pSrcBuffer)
{
    PrepareParentArrays(arrayStartIdx, count, arrayStep, bufferStride);
    return m_poParent->Write(m_parentStart.data(), m_parentCount.data(),
                             m_parentStep.data(), m_parentStride.data(),
                             bufferDataType, pSrcBuffer);
}

/************************************************************************/
/*                             IAdviseRead()                            */
/************************************************************************/

bool GDALSlicedMDArray::IAdviseRead(const GUInt64 *arrayStartIdx,
                                    const size_t *count,
                                    CSLConstList papszOptions) const
{
    PrepareParentArrays(arrayStartIdx, count, nullptr, nullptr);
    return m_poParent->AdviseRead(m_parentStart.data(), m_parentCount.data(),
                                  papszOptions);
}

/************************************************************************/
/*                        CreateSlicedArray()                           */
/************************************************************************/

static std::shared_ptr<GDALMDArray>
CreateSlicedArray(const std::shared_ptr<GDALMDArray> &self,
                  const std::string &viewExpr, const std::string &activeSlice,
                  bool bRenameDimensions,
                  std::vector<GDALMDArray::ViewSpec> &viewSpecs)
{
    const auto &srcDims(self->GetDimensions());
    if (srcDims.empty())
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Cannot slice a 0-d array");
        return nullptr;
    }

    CPLStringList aosTokens(CSLTokenizeString2(activeSlice.c_str(), ",", 0));
    const auto nTokens = static_cast<size_t>(aosTokens.size());

    std::vector<std::shared_ptr<GDALDimension>> newDims;
    std::vector<size_t> mapDimIdxToParentDimIdx;
    std::vector<GDALSlicedMDArray::Range> parentRanges;
    newDims.reserve(nTokens);
    mapDimIdxToParentDimIdx.reserve(nTokens);
    parentRanges.reserve(nTokens);

    bool bGotEllipsis = false;
    size_t nCurSrcDim = 0;
    for (size_t i = 0; i < nTokens; i++)
    {
        const char *pszIdxSpec = aosTokens[i];
        if (EQUAL(pszIdxSpec, "..."))
        {
            if (bGotEllipsis)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Only one single ellipsis is supported");
                return nullptr;
            }
            bGotEllipsis = true;
            const auto nSubstitutionCount = srcDims.size() - (nTokens - 1);
            for (size_t j = 0; j < nSubstitutionCount; j++, nCurSrcDim++)
            {
                parentRanges.emplace_back(0, 1);
                newDims.push_back(srcDims[nCurSrcDim]);
                mapDimIdxToParentDimIdx.push_back(nCurSrcDim);
            }
            continue;
        }
        else if (EQUAL(pszIdxSpec, "newaxis") ||
                 EQUAL(pszIdxSpec, "np.newaxis"))
        {
            newDims.push_back(std::make_shared<GDALDimension>(
                std::string(), "newaxis", std::string(), std::string(), 1));
            mapDimIdxToParentDimIdx.push_back(static_cast<size_t>(-1));
            continue;
        }
        else if (CPLGetValueType(pszIdxSpec) == CPL_VALUE_INTEGER)
        {
            if (nCurSrcDim >= srcDims.size())
            {
                CPLError(CE_Failure, CPLE_AppDefined, "Too many values in %s",
                         activeSlice.c_str());
                return nullptr;
            }

            auto nVal = CPLAtoGIntBig(pszIdxSpec);
            GUInt64 nDimSize = srcDims[nCurSrcDim]->GetSize();
            if ((nVal >= 0 && static_cast<GUInt64>(nVal) >= nDimSize) ||
                (nVal < 0 && nDimSize < static_cast<GUInt64>(-nVal)))
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Index " CPL_FRMT_GIB " is out of bounds", nVal);
                return nullptr;
            }
            if (nVal < 0)
                nVal += nDimSize;
            parentRanges.emplace_back(nVal, 0);
        }
        else
        {
            if (nCurSrcDim >= srcDims.size())
            {
                CPLError(CE_Failure, CPLE_AppDefined, "Too many values in %s",
                         activeSlice.c_str());
                return nullptr;
            }

            CPLStringList aosRangeTokens(
                CSLTokenizeString2(pszIdxSpec, ":", CSLT_ALLOWEMPTYTOKENS));
            int nRangeTokens = aosRangeTokens.size();
            if (nRangeTokens > 3)
            {
                CPLError(CE_Failure, CPLE_AppDefined, "Too many : in %s",
                         pszIdxSpec);
                return nullptr;
            }
            if (nRangeTokens <= 1)
            {
                CPLError(CE_Failure, CPLE_AppDefined, "Invalid value %s",
                         pszIdxSpec);
                return nullptr;
            }
            const char *pszStart = aosRangeTokens[0];
            const char *pszEnd = aosRangeTokens[1];
            const char *pszInc = (nRangeTokens == 3) ? aosRangeTokens[2] : "";
            GDALSlicedMDArray::Range range;
            const GUInt64 nDimSize(srcDims[nCurSrcDim]->GetSize());
            range.m_nIncr = EQUAL(pszInc, "") ? 1 : CPLAtoGIntBig(pszInc);
            if (range.m_nIncr == 0 ||
                range.m_nIncr == std::numeric_limits<GInt64>::min())
            {
                CPLError(CE_Failure, CPLE_AppDefined, "Invalid increment");
                return nullptr;
            }
            auto startIdx(CPLAtoGIntBig(pszStart));
            if (startIdx < 0)
            {
                if (nDimSize < static_cast<GUInt64>(-startIdx))
                    startIdx = 0;
                else
                    startIdx = nDimSize + startIdx;
            }
            const bool bPosIncr = range.m_nIncr > 0;
            range.m_nStartIdx = startIdx;
            range.m_nStartIdx = EQUAL(pszStart, "")
                                    ? (bPosIncr ? 0 : nDimSize - 1)
                                    : range.m_nStartIdx;
            if (range.m_nStartIdx >= nDimSize - 1)
                range.m_nStartIdx = nDimSize - 1;
            auto endIdx(CPLAtoGIntBig(pszEnd));
            if (endIdx < 0)
            {
                const auto positiveEndIdx = static_cast<GUInt64>(-endIdx);
                if (nDimSize < positiveEndIdx)
                    endIdx = 0;
                else
                    endIdx = nDimSize - positiveEndIdx;
            }
            GUInt64 nEndIdx = endIdx;
            nEndIdx = EQUAL(pszEnd, "") ? (!bPosIncr ? 0 : nDimSize) : nEndIdx;
            if ((bPosIncr && range.m_nStartIdx >= nEndIdx) ||
                (!bPosIncr && range.m_nStartIdx <= nEndIdx))
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Output dimension of size 0 is not allowed");
                return nullptr;
            }
            int inc = (EQUAL(pszEnd, "") && !bPosIncr) ? 1 : 0;
            const auto nAbsIncr = std::abs(range.m_nIncr);
            const GUInt64 newSize =
                bPosIncr
                    ? DIV_ROUND_UP(nEndIdx - range.m_nStartIdx, nAbsIncr)
                    : DIV_ROUND_UP(inc + range.m_nStartIdx - nEndIdx, nAbsIncr);
            if (range.m_nStartIdx == 0 && range.m_nIncr == 1 &&
                newSize == srcDims[nCurSrcDim]->GetSize())
            {
                newDims.push_back(srcDims[nCurSrcDim]);
            }
            else
            {
                std::string osNewDimName(srcDims[nCurSrcDim]->GetName());
                if (bRenameDimensions)
                {
                    osNewDimName =
                        "subset_" + srcDims[nCurSrcDim]->GetName() +
                        CPLSPrintf("_" CPL_FRMT_GUIB "_" CPL_FRMT_GIB
                                   "_" CPL_FRMT_GUIB,
                                   static_cast<GUIntBig>(range.m_nStartIdx),
                                   static_cast<GIntBig>(range.m_nIncr),
                                   static_cast<GUIntBig>(newSize));
                }
                newDims.push_back(std::make_shared<GDALDimension>(
                    std::string(), osNewDimName, srcDims[nCurSrcDim]->GetType(),
                    range.m_nIncr > 0 ? srcDims[nCurSrcDim]->GetDirection()
                                      : std::string(),
                    newSize));
            }
            mapDimIdxToParentDimIdx.push_back(nCurSrcDim);
            parentRanges.emplace_back(range);
        }

        nCurSrcDim++;
    }
    for (; nCurSrcDim < srcDims.size(); nCurSrcDim++)
    {
        parentRanges.emplace_back(0, 1);
        newDims.push_back(srcDims[nCurSrcDim]);
        mapDimIdxToParentDimIdx.push_back(nCurSrcDim);
    }

    GDALMDArray::ViewSpec viewSpec;
    viewSpec.m_mapDimIdxToParentDimIdx = mapDimIdxToParentDimIdx;
    viewSpec.m_parentRanges = parentRanges;
    viewSpecs.emplace_back(std::move(viewSpec));

    return GDALSlicedMDArray::Create(self, viewExpr, std::move(newDims),
                                     std::move(mapDimIdxToParentDimIdx),
                                     std::move(parentRanges));
}

/************************************************************************/
/*                       GDALExtractFieldMDArray                        */
/************************************************************************/

class GDALExtractFieldMDArray final : public GDALPamMDArray
{
  private:
    std::shared_ptr<GDALMDArray> m_poParent{};
    GDALExtendedDataType m_dt;
    std::string m_srcCompName;
    mutable std::vector<GByte> m_pabyNoData{};

  protected:
    GDALExtractFieldMDArray(const std::shared_ptr<GDALMDArray> &poParent,
                            const std::string &fieldName,
                            const std::unique_ptr<GDALEDTComponent> &srcComp)
        : GDALAbstractMDArray(std::string(), "Extract field " + fieldName +
                                                 " of " +
                                                 poParent->GetFullName()),
          GDALPamMDArray(
              std::string(),
              "Extract field " + fieldName + " of " + poParent->GetFullName(),
              GDALPamMultiDim::GetPAM(poParent), poParent->GetContext()),
          m_poParent(poParent), m_dt(srcComp->GetType()),
          m_srcCompName(srcComp->GetName())
    {
        m_pabyNoData.resize(m_dt.GetSize());
    }

    bool IRead(const GUInt64 *arrayStartIdx, const size_t *count,
               const GInt64 *arrayStep, const GPtrDiff_t *bufferStride,
               const GDALExtendedDataType &bufferDataType,
               void *pDstBuffer) const override;

    bool IAdviseRead(const GUInt64 *arrayStartIdx, const size_t *count,
                     CSLConstList papszOptions) const override
    {
        return m_poParent->AdviseRead(arrayStartIdx, count, papszOptions);
    }

  public:
    static std::shared_ptr<GDALExtractFieldMDArray>
    Create(const std::shared_ptr<GDALMDArray> &poParent,
           const std::string &fieldName,
           const std::unique_ptr<GDALEDTComponent> &srcComp)
    {
        auto newAr(std::shared_ptr<GDALExtractFieldMDArray>(
            new GDALExtractFieldMDArray(poParent, fieldName, srcComp)));
        newAr->SetSelf(newAr);
        return newAr;
    }

    ~GDALExtractFieldMDArray()
    {
        m_dt.FreeDynamicMemory(&m_pabyNoData[0]);
    }

    bool IsWritable() const override
    {
        return m_poParent->IsWritable();
    }

    const std::string &GetFilename() const override
    {
        return m_poParent->GetFilename();
    }

    const std::vector<std::shared_ptr<GDALDimension>> &
    GetDimensions() const override
    {
        return m_poParent->GetDimensions();
    }

    const GDALExtendedDataType &GetDataType() const override
    {
        return m_dt;
    }

    const std::string &GetUnit() const override
    {
        return m_poParent->GetUnit();
    }

    std::shared_ptr<OGRSpatialReference> GetSpatialRef() const override
    {
        return m_poParent->GetSpatialRef();
    }

    const void *GetRawNoDataValue() const override
    {
        const void *parentNoData = m_poParent->GetRawNoDataValue();
        if (parentNoData == nullptr)
            return nullptr;

        m_dt.FreeDynamicMemory(&m_pabyNoData[0]);
        memset(&m_pabyNoData[0], 0, m_dt.GetSize());

        std::vector<std::unique_ptr<GDALEDTComponent>> comps;
        comps.emplace_back(std::unique_ptr<GDALEDTComponent>(
            new GDALEDTComponent(m_srcCompName, 0, m_dt)));
        auto tmpDT(GDALExtendedDataType::Create(std::string(), m_dt.GetSize(),
                                                std::move(comps)));

        GDALExtendedDataType::CopyValue(parentNoData, m_poParent->GetDataType(),
                                        &m_pabyNoData[0], tmpDT);

        return &m_pabyNoData[0];
    }

    double GetOffset(bool *pbHasOffset,
                     GDALDataType *peStorageType) const override
    {
        return m_poParent->GetOffset(pbHasOffset, peStorageType);
    }

    double GetScale(bool *pbHasScale,
                    GDALDataType *peStorageType) const override
    {
        return m_poParent->GetScale(pbHasScale, peStorageType);
    }

    std::vector<GUInt64> GetBlockSize() const override
    {
        return m_poParent->GetBlockSize();
    }
};

/************************************************************************/
/*                             IRead()                                  */
/************************************************************************/

bool GDALExtractFieldMDArray::IRead(const GUInt64 *arrayStartIdx,
                                    const size_t *count,
                                    const GInt64 *arrayStep,
                                    const GPtrDiff_t *bufferStride,
                                    const GDALExtendedDataType &bufferDataType,
                                    void *pDstBuffer) const
{
    std::vector<std::unique_ptr<GDALEDTComponent>> comps;
    comps.emplace_back(std::unique_ptr<GDALEDTComponent>(
        new GDALEDTComponent(m_srcCompName, 0, bufferDataType)));
    auto tmpDT(GDALExtendedDataType::Create(
        std::string(), bufferDataType.GetSize(), std::move(comps)));

    return m_poParent->Read(arrayStartIdx, count, arrayStep, bufferStride,
                            tmpDT, pDstBuffer);
}

/************************************************************************/
/*                      CreateFieldNameExtractArray()                   */
/************************************************************************/

static std::shared_ptr<GDALMDArray>
CreateFieldNameExtractArray(const std::shared_ptr<GDALMDArray> &self,
                            const std::string &fieldName)
{
    CPLAssert(self->GetDataType().GetClass() == GEDTC_COMPOUND);
    const std::unique_ptr<GDALEDTComponent> *srcComp = nullptr;
    for (const auto &comp : self->GetDataType().GetComponents())
    {
        if (comp->GetName() == fieldName)
        {
            srcComp = &comp;
            break;
        }
    }
    if (srcComp == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Cannot find field %s",
                 fieldName.c_str());
        return nullptr;
    }
    return GDALExtractFieldMDArray::Create(self, fieldName, *srcComp);
}

/************************************************************************/
/*                             GetView()                                */
/************************************************************************/

// clang-format off
/** Return a view of the array using slicing or field access.
 *
 * The slice expression uses the same syntax as NumPy basic slicing and
 * indexing. See
 * https://www.numpy.org/devdocs/reference/arrays.indexing.html#basic-slicing-and-indexing
 * Or it can use field access by name. See
 * https://www.numpy.org/devdocs/reference/arrays.indexing.html#field-access
 *
 * Multiple [] bracket elements can be concatenated, with a slice expression
 * or field name inside each.
 *
 * For basic slicing and indexing, inside each [] bracket element, a list of
 * indexes that apply to successive source dimensions, can be specified, using
 * integer indexing (e.g. 1), range indexing (start:stop:step), ellipsis (...)
 * or newaxis, using a comma separator.
 *
 * Examples with a 2-dimensional array whose content is [[0,1,2,3],[4,5,6,7]].
 * <ul>
 * <li>GetView("[1][2]"): returns a 0-dimensional/scalar array with the value
 *     at index 1 in the first dimension, and index 2 in the second dimension
 *     from the source array. That is 5</li>
 * <li>GetView("[1]")->GetView("[2]"): same as above. Above is actually
 * implemented internally doing this intermediate slicing approach.</li>
 * <li>GetView("[1,2]"): same as above, but a bit more performant.</li>
 * <li>GetView("[1]"): returns a 1-dimensional array, sliced at index 1 in the
 *     first dimension. That is [4,5,6,7].</li>
 * <li>GetView("[:,2]"): returns a 1-dimensional array, sliced at index 2 in the
 *     second dimension. That is [2,6].</li>
 * <li>GetView("[:,2:3:]"): returns a 2-dimensional array, sliced at index 2 in
 * the second dimension. That is [[2],[6]].</li>
 * <li>GetView("[::,2]"): Same as
 * above.</li> <li>GetView("[...,2]"): same as above, in that case, since the
 * ellipsis only expands to one dimension here.</li>
 * <li>GetView("[:,::2]"):
 * returns a 2-dimensional array, with even-indexed elements of the second
 * dimension. That is [[0,2],[4,6]].</li>
 * <li>GetView("[:,1::2]"): returns a
 * 2-dimensional array, with odd-indexed elements of the second dimension. That
 * is [[1,3],[5,7]].</li>
 * <li>GetView("[:,1:3:]"): returns a 2-dimensional
 * array, with elements of the second dimension with index in the range [1,3[.
 * That is [[1,2],[5,6]].</li>
 * <li>GetView("[::-1,:]"): returns a 2-dimensional
 * array, with the values in first dimension reversed. That is
 * [[4,5,6,7],[0,1,2,3]].</li>
 * <li>GetView("[newaxis,...]"): returns a
 * 3-dimensional array, with an additional dimension of size 1 put at the
 * beginning. That is [[[0,1,2,3],[4,5,6,7]]].</li>
 * </ul>
 *
 * One difference with NumPy behavior is that ranges that would result in
 * zero elements are not allowed (dimensions of size 0 not being allowed in the
 * GDAL multidimensional model).
 *
 * For field access, the syntax to use is ["field_name"] or ['field_name'].
 * Multiple field specification is not supported currently.
 *
 * Both type of access can be combined, e.g. GetView("[1]['field_name']")
 *
 * \note When using the GDAL Python bindings, natural Python syntax can be
 * used. That is ar[0,::,1]["foo"] will be internally translated to
 * ar.GetView("[0,::,1]['foo']")
 * \note When using the C++ API and integer indexing only, you may use the
 * at(idx0, idx1, ...) method.
 *
 * The returned array holds a reference to the original one, and thus is
 * a view of it (not a copy). If the content of the original array changes,
 * the content of the view array too. When using basic slicing and indexing,
 * the view can be written if the underlying array is writable.
 *
 * This is the same as the C function GDALMDArrayGetView()
 *
 * @param viewExpr Expression expressing basic slicing and indexing, or field
 * access.
 * @return a new array, that holds a reference to the original one, and thus is
 * a view of it (not a copy), or nullptr in case of error.
 */
// clang-format on

std::shared_ptr<GDALMDArray>
GDALMDArray::GetView(const std::string &viewExpr) const
{
    std::vector<ViewSpec> viewSpecs;
    return GetView(viewExpr, true, viewSpecs);
}

//! @cond Doxygen_Suppress
std::shared_ptr<GDALMDArray>
GDALMDArray::GetView(const std::string &viewExpr, bool bRenameDimensions,
                     std::vector<ViewSpec> &viewSpecs) const
{
    auto self = std::dynamic_pointer_cast<GDALMDArray>(m_pSelf.lock());
    if (!self)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Driver implementation issue: m_pSelf not set !");
        return nullptr;
    }
    std::string curExpr(viewExpr);
    while (true)
    {
        if (curExpr.empty() || curExpr[0] != '[')
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Slice string should start with ['");
            return nullptr;
        }

        std::string fieldName;
        size_t endExpr;
        if (curExpr.size() > 2 && (curExpr[1] == '"' || curExpr[1] == '\''))
        {
            if (self->GetDataType().GetClass() != GEDTC_COMPOUND)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Field access not allowed on non-compound data type");
                return nullptr;
            }
            size_t idx = 2;
            for (; idx < curExpr.size(); idx++)
            {
                const char ch = curExpr[idx];
                if (ch == curExpr[1])
                    break;
                if (ch == '\\' && idx + 1 < curExpr.size())
                {
                    fieldName += curExpr[idx + 1];
                    idx++;
                }
                else
                {
                    fieldName += ch;
                }
            }
            if (idx + 1 >= curExpr.size() || curExpr[idx + 1] != ']')
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Invalid field access specification");
                return nullptr;
            }
            endExpr = idx + 1;
        }
        else
        {
            endExpr = curExpr.find(']');
        }
        if (endExpr == std::string::npos)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Missing ]'");
            return nullptr;
        }
        if (endExpr == 1)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "[] not allowed");
            return nullptr;
        }
        std::string activeSlice(curExpr.substr(1, endExpr - 1));

        if (!fieldName.empty())
        {
            ViewSpec viewSpec;
            viewSpec.m_osFieldName = fieldName;
            viewSpecs.emplace_back(std::move(viewSpec));
        }

        auto newArray = !fieldName.empty()
                            ? CreateFieldNameExtractArray(self, fieldName)
                            : CreateSlicedArray(self, viewExpr, activeSlice,
                                                bRenameDimensions, viewSpecs);

        if (endExpr == curExpr.size() - 1)
        {
            return newArray;
        }
        self = std::move(newArray);
        curExpr = curExpr.substr(endExpr + 1);
    }
}

//! @endcond

std::shared_ptr<GDALMDArray>
GDALMDArray::GetView(const std::vector<GUInt64> &indices) const
{
    std::string osExpr("[");
    bool bFirst = true;
    for (const auto &idx : indices)
    {
        if (!bFirst)
            osExpr += ',';
        bFirst = false;
        osExpr += CPLSPrintf(CPL_FRMT_GUIB, static_cast<GUIntBig>(idx));
    }
    return GetView(osExpr + ']');
}

/************************************************************************/
/*                            operator[]                                */
/************************************************************************/

/** Return a view of the array using field access
 *
 * Equivalent of GetView("['fieldName']")
 *
 * \note When operationg on a shared_ptr, use (*array)["fieldName"] syntax.
 */
std::shared_ptr<GDALMDArray>
GDALMDArray::operator[](const std::string &fieldName) const
{
    return GetView(CPLSPrintf("['%s']", CPLString(fieldName)
                                            .replaceAll('\\', "\\\\")
                                            .replaceAll('\'', "\\\'")
                                            .c_str()));
}

/************************************************************************/
/*                      GDALMDArrayTransposed                           */
/************************************************************************/

class GDALMDArrayTransposed final : public GDALPamMDArray
{
  private:
    std::shared_ptr<GDALMDArray> m_poParent{};
    std::vector<int> m_anMapNewAxisToOldAxis{};
    std::vector<std::shared_ptr<GDALDimension>> m_dims{};

    mutable std::vector<GUInt64> m_parentStart;
    mutable std::vector<size_t> m_parentCount;
    mutable std::vector<GInt64> m_parentStep;
    mutable std::vector<GPtrDiff_t> m_parentStride;

    void PrepareParentArrays(const GUInt64 *arrayStartIdx, const size_t *count,
                             const GInt64 *arrayStep,
                             const GPtrDiff_t *bufferStride) const;

    static std::string
    MappingToStr(const std::vector<int> &anMapNewAxisToOldAxis)
    {
        std::string ret;
        ret += '[';
        for (size_t i = 0; i < anMapNewAxisToOldAxis.size(); ++i)
        {
            if (i > 0)
                ret += ',';
            ret += CPLSPrintf("%d", anMapNewAxisToOldAxis[i]);
        }
        ret += ']';
        return ret;
    }

  protected:
    GDALMDArrayTransposed(const std::shared_ptr<GDALMDArray> &poParent,
                          const std::vector<int> &anMapNewAxisToOldAxis,
                          std::vector<std::shared_ptr<GDALDimension>> &&dims)
        : GDALAbstractMDArray(std::string(),
                              "Transposed view of " + poParent->GetFullName() +
                                  " along " +
                                  MappingToStr(anMapNewAxisToOldAxis)),
          GDALPamMDArray(std::string(),
                         "Transposed view of " + poParent->GetFullName() +
                             " along " + MappingToStr(anMapNewAxisToOldAxis),
                         GDALPamMultiDim::GetPAM(poParent),
                         poParent->GetContext()),
          m_poParent(std::move(poParent)),
          m_anMapNewAxisToOldAxis(anMapNewAxisToOldAxis),
          m_dims(std::move(dims)),
          m_parentStart(m_poParent->GetDimensionCount()),
          m_parentCount(m_poParent->GetDimensionCount()),
          m_parentStep(m_poParent->GetDimensionCount()),
          m_parentStride(m_poParent->GetDimensionCount())
    {
    }

    bool IRead(const GUInt64 *arrayStartIdx, const size_t *count,
               const GInt64 *arrayStep, const GPtrDiff_t *bufferStride,
               const GDALExtendedDataType &bufferDataType,
               void *pDstBuffer) const override;

    bool IWrite(const GUInt64 *arrayStartIdx, const size_t *count,
                const GInt64 *arrayStep, const GPtrDiff_t *bufferStride,
                const GDALExtendedDataType &bufferDataType,
                const void *pSrcBuffer) override;

    bool IAdviseRead(const GUInt64 *arrayStartIdx, const size_t *count,
                     CSLConstList papszOptions) const override;

  public:
    static std::shared_ptr<GDALMDArrayTransposed>
    Create(const std::shared_ptr<GDALMDArray> &poParent,
           const std::vector<int> &anMapNewAxisToOldAxis)
    {
        const auto &parentDims(poParent->GetDimensions());
        std::vector<std::shared_ptr<GDALDimension>> dims;
        for (const auto iOldAxis : anMapNewAxisToOldAxis)
        {
            if (iOldAxis < 0)
            {
                dims.push_back(std::make_shared<GDALDimension>(
                    std::string(), "newaxis", std::string(), std::string(), 1));
            }
            else
            {
                dims.emplace_back(parentDims[iOldAxis]);
            }
        }

        auto newAr(
            std::shared_ptr<GDALMDArrayTransposed>(new GDALMDArrayTransposed(
                poParent, anMapNewAxisToOldAxis, std::move(dims))));
        newAr->SetSelf(newAr);
        return newAr;
    }

    bool IsWritable() const override
    {
        return m_poParent->IsWritable();
    }

    const std::string &GetFilename() const override
    {
        return m_poParent->GetFilename();
    }

    const std::vector<std::shared_ptr<GDALDimension>> &
    GetDimensions() const override
    {
        return m_dims;
    }

    const GDALExtendedDataType &GetDataType() const override
    {
        return m_poParent->GetDataType();
    }

    const std::string &GetUnit() const override
    {
        return m_poParent->GetUnit();
    }

    std::shared_ptr<OGRSpatialReference> GetSpatialRef() const override
    {
        auto poSrcSRS = m_poParent->GetSpatialRef();
        if (!poSrcSRS)
            return nullptr;
        auto srcMapping = poSrcSRS->GetDataAxisToSRSAxisMapping();
        std::vector<int> dstMapping;
        for (int srcAxis : srcMapping)
        {
            bool bFound = false;
            for (size_t i = 0; i < m_anMapNewAxisToOldAxis.size(); i++)
            {
                if (m_anMapNewAxisToOldAxis[i] == srcAxis - 1)
                {
                    dstMapping.push_back(static_cast<int>(i) + 1);
                    bFound = true;
                    break;
                }
            }
            if (!bFound)
            {
                dstMapping.push_back(0);
            }
        }
        auto poClone(std::shared_ptr<OGRSpatialReference>(poSrcSRS->Clone()));
        poClone->SetDataAxisToSRSAxisMapping(dstMapping);
        return poClone;
    }

    const void *GetRawNoDataValue() const override
    {
        return m_poParent->GetRawNoDataValue();
    }

    // bool SetRawNoDataValue(const void* pRawNoData) override { return
    // m_poParent->SetRawNoDataValue(pRawNoData); }

    double GetOffset(bool *pbHasOffset,
                     GDALDataType *peStorageType) const override
    {
        return m_poParent->GetOffset(pbHasOffset, peStorageType);
    }

    double GetScale(bool *pbHasScale,
                    GDALDataType *peStorageType) const override
    {
        return m_poParent->GetScale(pbHasScale, peStorageType);
    }

    // bool SetOffset(double dfOffset) override { return
    // m_poParent->SetOffset(dfOffset); }

    // bool SetScale(double dfScale) override { return
    // m_poParent->SetScale(dfScale); }

    std::vector<GUInt64> GetBlockSize() const override
    {
        std::vector<GUInt64> ret(GetDimensionCount());
        const auto parentBlockSize(m_poParent->GetBlockSize());
        for (size_t i = 0; i < m_anMapNewAxisToOldAxis.size(); ++i)
        {
            const auto iOldAxis = m_anMapNewAxisToOldAxis[i];
            if (iOldAxis >= 0)
            {
                ret[i] = parentBlockSize[iOldAxis];
            }
        }
        return ret;
    }

    std::shared_ptr<GDALAttribute>
    GetAttribute(const std::string &osName) const override
    {
        return m_poParent->GetAttribute(osName);
    }

    std::vector<std::shared_ptr<GDALAttribute>>
    GetAttributes(CSLConstList papszOptions = nullptr) const override
    {
        return m_poParent->GetAttributes(papszOptions);
    }
};

/************************************************************************/
/*                         PrepareParentArrays()                        */
/************************************************************************/

void GDALMDArrayTransposed::PrepareParentArrays(
    const GUInt64 *arrayStartIdx, const size_t *count, const GInt64 *arrayStep,
    const GPtrDiff_t *bufferStride) const
{
    for (size_t i = 0; i < m_anMapNewAxisToOldAxis.size(); ++i)
    {
        const auto iOldAxis = m_anMapNewAxisToOldAxis[i];
        if (iOldAxis >= 0)
        {
            m_parentStart[iOldAxis] = arrayStartIdx[i];
            m_parentCount[iOldAxis] = count[i];
            if (arrayStep)  // only null when called from IAdviseRead()
            {
                m_parentStep[iOldAxis] = arrayStep[i];
            }
            if (bufferStride)  // only null when called from IAdviseRead()
            {
                m_parentStride[iOldAxis] = bufferStride[i];
            }
        }
    }
}

/************************************************************************/
/*                             IRead()                                  */
/************************************************************************/

bool GDALMDArrayTransposed::IRead(const GUInt64 *arrayStartIdx,
                                  const size_t *count, const GInt64 *arrayStep,
                                  const GPtrDiff_t *bufferStride,
                                  const GDALExtendedDataType &bufferDataType,
                                  void *pDstBuffer) const
{
    PrepareParentArrays(arrayStartIdx, count, arrayStep, bufferStride);
    return m_poParent->Read(m_parentStart.data(), m_parentCount.data(),
                            m_parentStep.data(), m_parentStride.data(),
                            bufferDataType, pDstBuffer);
}

/************************************************************************/
/*                            IWrite()                                  */
/************************************************************************/

bool GDALMDArrayTransposed::IWrite(const GUInt64 *arrayStartIdx,
                                   const size_t *count, const GInt64 *arrayStep,
                                   const GPtrDiff_t *bufferStride,
                                   const GDALExtendedDataType &bufferDataType,
                                   const void *pSrcBuffer)
{
    PrepareParentArrays(arrayStartIdx, count, arrayStep, bufferStride);
    return m_poParent->Write(m_parentStart.data(), m_parentCount.data(),
                             m_parentStep.data(), m_parentStride.data(),
                             bufferDataType, pSrcBuffer);
}

/************************************************************************/
/*                             IAdviseRead()                            */
/************************************************************************/

bool GDALMDArrayTransposed::IAdviseRead(const GUInt64 *arrayStartIdx,
                                        const size_t *count,
                                        CSLConstList papszOptions) const
{
    PrepareParentArrays(arrayStartIdx, count, nullptr, nullptr);
    return m_poParent->AdviseRead(m_parentStart.data(), m_parentCount.data(),
                                  papszOptions);
}

/************************************************************************/
/*                           Transpose()                                */
/************************************************************************/

/** Return a view of the array whose axis have been reordered.
 *
 * The anMapNewAxisToOldAxis parameter should contain all the values between 0
 * and GetDimensionCount() - 1, and each only once.
 * -1 can be used as a special index value to ask for the insertion of a new
 * axis of size 1.
 * The new array will have anMapNewAxisToOldAxis.size() axis, and if i is the
 * index of one of its dimension, it corresponds to the axis of index
 * anMapNewAxisToOldAxis[i] from the current array.
 *
 * This is similar to the numpy.transpose() method
 *
 * The returned array holds a reference to the original one, and thus is
 * a view of it (not a copy). If the content of the original array changes,
 * the content of the view array too. The view can be written if the underlying
 * array is writable.
 *
 * Note that I/O performance in such a transposed view might be poor.
 *
 * This is the same as the C function GDALMDArrayTranspose().
 *
 * @return a new array, that holds a reference to the original one, and thus is
 * a view of it (not a copy), or nullptr in case of error.
 */
std::shared_ptr<GDALMDArray>
GDALMDArray::Transpose(const std::vector<int> &anMapNewAxisToOldAxis) const
{
    auto self = std::dynamic_pointer_cast<GDALMDArray>(m_pSelf.lock());
    if (!self)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Driver implementation issue: m_pSelf not set !");
        return nullptr;
    }
    const int nDims = static_cast<int>(GetDimensionCount());
    std::vector<bool> alreadyUsedOldAxis(nDims, false);
    int nCountOldAxis = 0;
    for (const auto iOldAxis : anMapNewAxisToOldAxis)
    {
        if (iOldAxis < -1 || iOldAxis >= nDims)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Invalid axis number");
            return nullptr;
        }
        if (iOldAxis >= 0)
        {
            if (alreadyUsedOldAxis[iOldAxis])
            {
                CPLError(CE_Failure, CPLE_AppDefined, "Axis %d is repeated",
                         iOldAxis);
                return nullptr;
            }
            alreadyUsedOldAxis[iOldAxis] = true;
            nCountOldAxis++;
        }
    }
    if (nCountOldAxis != nDims)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "One or several original axis missing");
        return nullptr;
    }
    return GDALMDArrayTransposed::Create(self, anMapNewAxisToOldAxis);
}

/************************************************************************/
/*                             IRead()                                  */
/************************************************************************/

bool GDALMDArrayUnscaled::IRead(const GUInt64 *arrayStartIdx,
                                const size_t *count, const GInt64 *arrayStep,
                                const GPtrDiff_t *bufferStride,
                                const GDALExtendedDataType &bufferDataType,
                                void *pDstBuffer) const
{
    const double dfScale = m_dfScale;
    const double dfOffset = m_dfOffset;
    const bool bDTIsComplex = GDALDataTypeIsComplex(m_dt.GetNumericDataType());
    const auto dtDouble =
        GDALExtendedDataType::Create(bDTIsComplex ? GDT_CFloat64 : GDT_Float64);
    const size_t nDTSize = dtDouble.GetSize();
    const bool bTempBufferNeeded = (dtDouble != bufferDataType);

    double adfSrcNoData[2] = {0, 0};
    if (m_bHasNoData)
    {
        GDALExtendedDataType::CopyValue(m_poParent->GetRawNoDataValue(),
                                        m_poParent->GetDataType(),
                                        &adfSrcNoData[0], dtDouble);
    }

    const auto nDims = GetDimensions().size();
    if (nDims == 0)
    {
        double adfVal[2];
        if (!m_poParent->Read(arrayStartIdx, count, arrayStep, bufferStride,
                              dtDouble, &adfVal[0]))
        {
            return false;
        }
        if (!m_bHasNoData || adfVal[0] != adfSrcNoData[0])
        {
            adfVal[0] = adfVal[0] * dfScale + dfOffset;
            if (bDTIsComplex)
            {
                adfVal[1] = adfVal[1] * dfScale + dfOffset;
            }
            GDALExtendedDataType::CopyValue(&adfVal[0], dtDouble, pDstBuffer,
                                            bufferDataType);
        }
        else
        {
            GDALExtendedDataType::CopyValue(m_abyRawNoData.data(), m_dt,
                                            pDstBuffer, bufferDataType);
        }
        return true;
    }

    std::vector<GPtrDiff_t> actualBufferStrideVector;
    const GPtrDiff_t *actualBufferStridePtr = bufferStride;
    void *pTempBuffer = pDstBuffer;
    if (bTempBufferNeeded)
    {
        size_t nElts = 1;
        actualBufferStrideVector.resize(nDims);
        for (size_t i = 0; i < nDims; i++)
            nElts *= count[i];
        actualBufferStrideVector.back() = 1;
        for (size_t i = nDims - 1; i > 0;)
        {
            --i;
            actualBufferStrideVector[i] =
                actualBufferStrideVector[i + 1] * count[i + 1];
        }
        actualBufferStridePtr = actualBufferStrideVector.data();
        pTempBuffer = VSI_MALLOC2_VERBOSE(nDTSize, nElts);
        if (!pTempBuffer)
            return false;
    }
    if (!m_poParent->Read(arrayStartIdx, count, arrayStep,
                          actualBufferStridePtr, dtDouble, pTempBuffer))
    {
        if (bTempBufferNeeded)
            VSIFree(pTempBuffer);
        return false;
    }

    struct Stack
    {
        size_t nIters = 0;
        double *src_ptr = nullptr;
        GByte *dst_ptr = nullptr;
        GPtrDiff_t src_inc_offset = 0;
        GPtrDiff_t dst_inc_offset = 0;
    };

    std::vector<Stack> stack(nDims);
    const size_t nBufferDTSize = bufferDataType.GetSize();
    for (size_t i = 0; i < nDims; i++)
    {
        stack[i].src_inc_offset =
            actualBufferStridePtr[i] * (bDTIsComplex ? 2 : 1);
        stack[i].dst_inc_offset =
            static_cast<GPtrDiff_t>(bufferStride[i] * nBufferDTSize);
    }
    stack[0].src_ptr = static_cast<double *>(pTempBuffer);
    stack[0].dst_ptr = static_cast<GByte *>(pDstBuffer);

    size_t dimIdx = 0;
    const size_t nDimsMinus1 = nDims - 1;
    GByte abyDstNoData[16];
    CPLAssert(nBufferDTSize <= sizeof(abyDstNoData));
    GDALExtendedDataType::CopyValue(m_abyRawNoData.data(), m_dt, abyDstNoData,
                                    bufferDataType);

lbl_next_depth:
    if (dimIdx == nDimsMinus1)
    {
        auto nIters = count[dimIdx];
        double *padfVal = stack[dimIdx].src_ptr;
        GByte *dst_ptr = stack[dimIdx].dst_ptr;
        while (true)
        {
            if (!m_bHasNoData || padfVal[0] != adfSrcNoData[0])
            {
                padfVal[0] = padfVal[0] * dfScale + dfOffset;
                if (bDTIsComplex)
                {
                    padfVal[1] = padfVal[1] * dfScale + dfOffset;
                }
                if (bTempBufferNeeded)
                {
                    GDALExtendedDataType::CopyValue(&padfVal[0], dtDouble,
                                                    dst_ptr, bufferDataType);
                }
            }
            else
            {
                memcpy(dst_ptr, abyDstNoData, nBufferDTSize);
            }

            if ((--nIters) == 0)
                break;
            padfVal += stack[dimIdx].src_inc_offset;
            dst_ptr += stack[dimIdx].dst_inc_offset;
        }
    }
    else
    {
        stack[dimIdx].nIters = count[dimIdx];
        while (true)
        {
            dimIdx++;
            stack[dimIdx].src_ptr = stack[dimIdx - 1].src_ptr;
            stack[dimIdx].dst_ptr = stack[dimIdx - 1].dst_ptr;
            goto lbl_next_depth;
        lbl_return_to_caller:
            dimIdx--;
            if ((--stack[dimIdx].nIters) == 0)
                break;
            stack[dimIdx].src_ptr += stack[dimIdx].src_inc_offset;
            stack[dimIdx].dst_ptr += stack[dimIdx].dst_inc_offset;
        }
    }
    if (dimIdx > 0)
        goto lbl_return_to_caller;

    if (bTempBufferNeeded)
        VSIFree(pTempBuffer);
    return true;
}

/************************************************************************/
/*                             IWrite()                                 */
/************************************************************************/

bool GDALMDArrayUnscaled::IWrite(const GUInt64 *arrayStartIdx,
                                 const size_t *count, const GInt64 *arrayStep,
                                 const GPtrDiff_t *bufferStride,
                                 const GDALExtendedDataType &bufferDataType,
                                 const void *pSrcBuffer)
{
    const double dfScale = m_dfScale;
    const double dfOffset = m_dfOffset;
    const bool bDTIsComplex = GDALDataTypeIsComplex(m_dt.GetNumericDataType());
    const auto dtDouble =
        GDALExtendedDataType::Create(bDTIsComplex ? GDT_CFloat64 : GDT_Float64);
    const size_t nDTSize = dtDouble.GetSize();
    const bool bIsBufferDataTypeNativeDataType = (dtDouble == bufferDataType);
    const bool bSelfAndParentHaveNoData =
        m_bHasNoData && m_poParent->GetRawNoDataValue() != nullptr;
    double dfNoData = 0;
    if (m_bHasNoData)
    {
        GDALCopyWords64(m_abyRawNoData.data(), m_dt.GetNumericDataType(), 0,
                        &dfNoData, GDT_Float64, 0, 1);
    }

    double adfSrcNoData[2] = {0, 0};
    if (bSelfAndParentHaveNoData)
    {
        GDALExtendedDataType::CopyValue(m_poParent->GetRawNoDataValue(),
                                        m_poParent->GetDataType(),
                                        &adfSrcNoData[0], dtDouble);
    }

    const auto nDims = GetDimensions().size();
    if (nDims == 0)
    {
        double adfVal[2];
        GDALExtendedDataType::CopyValue(pSrcBuffer, bufferDataType, &adfVal[0],
                                        dtDouble);
        if (bSelfAndParentHaveNoData &&
            (std::isnan(adfVal[0]) || adfVal[0] == dfNoData))
        {
            return m_poParent->Write(arrayStartIdx, count, arrayStep,
                                     bufferStride, m_poParent->GetDataType(),
                                     m_poParent->GetRawNoDataValue());
        }
        else
        {
            adfVal[0] = (adfVal[0] - dfOffset) / dfScale;
            if (bDTIsComplex)
            {
                adfVal[1] = (adfVal[1] - dfOffset) / dfScale;
            }
            return m_poParent->Write(arrayStartIdx, count, arrayStep,
                                     bufferStride, dtDouble, &adfVal[0]);
        }
    }

    std::vector<GPtrDiff_t> tmpBufferStrideVector;
    size_t nElts = 1;
    tmpBufferStrideVector.resize(nDims);
    for (size_t i = 0; i < nDims; i++)
        nElts *= count[i];
    tmpBufferStrideVector.back() = 1;
    for (size_t i = nDims - 1; i > 0;)
    {
        --i;
        tmpBufferStrideVector[i] = tmpBufferStrideVector[i + 1] * count[i + 1];
    }
    const GPtrDiff_t *tmpBufferStridePtr = tmpBufferStrideVector.data();
    void *pTempBuffer = VSI_MALLOC2_VERBOSE(nDTSize, nElts);
    if (!pTempBuffer)
        return false;

    struct Stack
    {
        size_t nIters = 0;
        double *dst_ptr = nullptr;
        const GByte *src_ptr = nullptr;
        GPtrDiff_t src_inc_offset = 0;
        GPtrDiff_t dst_inc_offset = 0;
    };

    std::vector<Stack> stack(nDims);
    const size_t nBufferDTSize = bufferDataType.GetSize();
    for (size_t i = 0; i < nDims; i++)
    {
        stack[i].dst_inc_offset =
            tmpBufferStridePtr[i] * (bDTIsComplex ? 2 : 1);
        stack[i].src_inc_offset =
            static_cast<GPtrDiff_t>(bufferStride[i] * nBufferDTSize);
    }
    stack[0].dst_ptr = static_cast<double *>(pTempBuffer);
    stack[0].src_ptr = static_cast<const GByte *>(pSrcBuffer);

    size_t dimIdx = 0;
    const size_t nDimsMinus1 = nDims - 1;

lbl_next_depth:
    if (dimIdx == nDimsMinus1)
    {
        auto nIters = count[dimIdx];
        double *dst_ptr = stack[dimIdx].dst_ptr;
        const GByte *src_ptr = stack[dimIdx].src_ptr;
        while (true)
        {
            double adfVal[2];
            const double *padfSrcVal;
            if (bIsBufferDataTypeNativeDataType)
            {
                padfSrcVal = reinterpret_cast<const double *>(src_ptr);
            }
            else
            {
                GDALExtendedDataType::CopyValue(src_ptr, bufferDataType,
                                                &adfVal[0], dtDouble);
                padfSrcVal = adfVal;
            }

            if (bSelfAndParentHaveNoData &&
                (std::isnan(padfSrcVal[0]) || padfSrcVal[0] == dfNoData))
            {
                dst_ptr[0] = adfSrcNoData[0];
                if (bDTIsComplex)
                {
                    dst_ptr[1] = adfSrcNoData[1];
                }
            }
            else
            {
                dst_ptr[0] = (padfSrcVal[0] - dfOffset) / dfScale;
                if (bDTIsComplex)
                {
                    dst_ptr[1] = (padfSrcVal[1] - dfOffset) / dfScale;
                }
            }

            if ((--nIters) == 0)
                break;
            dst_ptr += stack[dimIdx].dst_inc_offset;
            src_ptr += stack[dimIdx].src_inc_offset;
        }
    }
    else
    {
        stack[dimIdx].nIters = count[dimIdx];
        while (true)
        {
            dimIdx++;
            stack[dimIdx].src_ptr = stack[dimIdx - 1].src_ptr;
            stack[dimIdx].dst_ptr = stack[dimIdx - 1].dst_ptr;
            goto lbl_next_depth;
        lbl_return_to_caller:
            dimIdx--;
            if ((--stack[dimIdx].nIters) == 0)
                break;
            stack[dimIdx].src_ptr += stack[dimIdx].src_inc_offset;
            stack[dimIdx].dst_ptr += stack[dimIdx].dst_inc_offset;
        }
    }
    if (dimIdx > 0)
        goto lbl_return_to_caller;

    // If the parent array is not double/complex-double, then convert the
    // values to it, before calling Write(), as some implementations can be
    // very slow when doing the type conversion.
    const auto &eParentDT = m_poParent->GetDataType();
    const size_t nParentDTSize = eParentDT.GetSize();
    if (nParentDTSize <= nDTSize / 2)
    {
        // Copy in-place by making sure that source and target do not overlap
        const auto eNumericDT = dtDouble.GetNumericDataType();
        const auto eParentNumericDT = eParentDT.GetNumericDataType();

        // Copy first element
        {
            std::vector<GByte> abyTemp(nParentDTSize);
            GDALCopyWords64(static_cast<GByte *>(pTempBuffer), eNumericDT,
                            static_cast<int>(nDTSize), &abyTemp[0],
                            eParentNumericDT, static_cast<int>(nParentDTSize),
                            1);
            memcpy(pTempBuffer, abyTemp.data(), abyTemp.size());
        }
        // Remaining elements
        for (size_t i = 1; i < nElts; ++i)
        {
            GDALCopyWords64(
                static_cast<GByte *>(pTempBuffer) + i * nDTSize, eNumericDT, 0,
                static_cast<GByte *>(pTempBuffer) + i * nParentDTSize,
                eParentNumericDT, 0, 1);
        }
    }

    const bool ret =
        m_poParent->Write(arrayStartIdx, count, arrayStep, tmpBufferStridePtr,
                          eParentDT, pTempBuffer);

    VSIFree(pTempBuffer);
    return ret;
}

/************************************************************************/
/*                           GetUnscaled()                              */
/************************************************************************/

/** Return an array that is the unscaled version of the current one.
 *
 * That is each value of the unscaled array will be
 * unscaled_value = raw_value * GetScale() + GetOffset()
 *
 * Starting with GDAL 3.3, the Write() method is implemented and will convert
 * from unscaled values to raw values.
 *
 * This is the same as the C function GDALMDArrayGetUnscaled().
 *
 * @param dfOverriddenScale Custom scale value instead of GetScale()
 * @param dfOverriddenOffset Custom offset value instead of GetOffset()
 * @param dfOverriddenDstNodata Custom target nodata value instead of NaN
 * @return a new array, that holds a reference to the original one, and thus is
 * a view of it (not a copy), or nullptr in case of error.
 */
std::shared_ptr<GDALMDArray>
GDALMDArray::GetUnscaled(double dfOverriddenScale, double dfOverriddenOffset,
                         double dfOverriddenDstNodata) const
{
    auto self = std::dynamic_pointer_cast<GDALMDArray>(m_pSelf.lock());
    if (!self)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Driver implementation issue: m_pSelf not set !");
        return nullptr;
    }
    if (GetDataType().GetClass() != GEDTC_NUMERIC)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "GetUnscaled() only supports numeric data type");
        return nullptr;
    }
    const double dfScale =
        std::isnan(dfOverriddenScale) ? GetScale() : dfOverriddenScale;
    const double dfOffset =
        std::isnan(dfOverriddenOffset) ? GetOffset() : dfOverriddenOffset;
    if (dfScale == 1.0 && dfOffset == 0.0)
        return self;

    GDALDataType eDT = GDALDataTypeIsComplex(GetDataType().GetNumericDataType())
                           ? GDT_CFloat64
                           : GDT_Float64;
    if (dfOverriddenScale == -1 && dfOverriddenOffset == 0)
    {
        if (GetDataType().GetNumericDataType() == GDT_Float16)
            eDT = GDT_Float16;
        if (GetDataType().GetNumericDataType() == GDT_Float32)
            eDT = GDT_Float32;
    }

    return GDALMDArrayUnscaled::Create(self, dfScale, dfOffset,
                                       dfOverriddenDstNodata, eDT);
}

/************************************************************************/
/*                         GDALMDArrayMask                              */
/************************************************************************/

class GDALMDArrayMask final : public GDALPamMDArray
{
  private:
    std::shared_ptr<GDALMDArray> m_poParent{};
    GDALExtendedDataType m_dt{GDALExtendedDataType::Create(GDT_Byte)};
    double m_dfMissingValue = 0.0;
    bool m_bHasMissingValue = false;
    double m_dfFillValue = 0.0;
    bool m_bHasFillValue = false;
    double m_dfValidMin = 0.0;
    bool m_bHasValidMin = false;
    double m_dfValidMax = 0.0;
    bool m_bHasValidMax = false;
    std::vector<uint32_t> m_anValidFlagMasks{};
    std::vector<uint32_t> m_anValidFlagValues{};

    bool Init(CSLConstList papszOptions);

    template <typename Type>
    void
    ReadInternal(const size_t *count, const GPtrDiff_t *bufferStride,
                 const GDALExtendedDataType &bufferDataType, void *pDstBuffer,
                 const void *pTempBuffer,
                 const GDALExtendedDataType &oTmpBufferDT,
                 const std::vector<GPtrDiff_t> &tmpBufferStrideVector) const;

  protected:
    explicit GDALMDArrayMask(const std::shared_ptr<GDALMDArray> &poParent)
        : GDALAbstractMDArray(std::string(),
                              "Mask of " + poParent->GetFullName()),
          GDALPamMDArray(std::string(), "Mask of " + poParent->GetFullName(),
                         GDALPamMultiDim::GetPAM(poParent),
                         poParent->GetContext()),
          m_poParent(std::move(poParent))
    {
    }

    bool IRead(const GUInt64 *arrayStartIdx, const size_t *count,
               const GInt64 *arrayStep, const GPtrDiff_t *bufferStride,
               const GDALExtendedDataType &bufferDataType,
               void *pDstBuffer) const override;

    bool IAdviseRead(const GUInt64 *arrayStartIdx, const size_t *count,
                     CSLConstList papszOptions) const override
    {
        return m_poParent->AdviseRead(arrayStartIdx, count, papszOptions);
    }

  public:
    static std::shared_ptr<GDALMDArrayMask>
    Create(const std::shared_ptr<GDALMDArray> &poParent,
           CSLConstList papszOptions);

    bool IsWritable() const override
    {
        return false;
    }

    const std::string &GetFilename() const override
    {
        return m_poParent->GetFilename();
    }

    const std::vector<std::shared_ptr<GDALDimension>> &
    GetDimensions() const override
    {
        return m_poParent->GetDimensions();
    }

    const GDALExtendedDataType &GetDataType() const override
    {
        return m_dt;
    }

    std::shared_ptr<OGRSpatialReference> GetSpatialRef() const override
    {
        return m_poParent->GetSpatialRef();
    }

    std::vector<GUInt64> GetBlockSize() const override
    {
        return m_poParent->GetBlockSize();
    }
};

/************************************************************************/
/*                    GDALMDArrayMask::Create()                         */
/************************************************************************/

/* static */ std::shared_ptr<GDALMDArrayMask>
GDALMDArrayMask::Create(const std::shared_ptr<GDALMDArray> &poParent,
                        CSLConstList papszOptions)
{
    auto newAr(std::shared_ptr<GDALMDArrayMask>(new GDALMDArrayMask(poParent)));
    newAr->SetSelf(newAr);
    if (!newAr->Init(papszOptions))
        return nullptr;
    return newAr;
}

/************************************************************************/
/*                    GDALMDArrayMask::Init()                           */
/************************************************************************/

bool GDALMDArrayMask::Init(CSLConstList papszOptions)
{
    const auto GetSingleValNumericAttr =
        [this](const char *pszAttrName, bool &bHasVal, double &dfVal)
    {
        auto poAttr = m_poParent->GetAttribute(pszAttrName);
        if (poAttr && poAttr->GetDataType().GetClass() == GEDTC_NUMERIC)
        {
            const auto anDimSizes = poAttr->GetDimensionsSize();
            if (anDimSizes.empty() ||
                (anDimSizes.size() == 1 && anDimSizes[0] == 1))
            {
                bHasVal = true;
                dfVal = poAttr->ReadAsDouble();
            }
        }
    };

    GetSingleValNumericAttr("missing_value", m_bHasMissingValue,
                            m_dfMissingValue);
    GetSingleValNumericAttr("_FillValue", m_bHasFillValue, m_dfFillValue);
    GetSingleValNumericAttr("valid_min", m_bHasValidMin, m_dfValidMin);
    GetSingleValNumericAttr("valid_max", m_bHasValidMax, m_dfValidMax);

    {
        auto poValidRange = m_poParent->GetAttribute("valid_range");
        if (poValidRange && poValidRange->GetDimensionsSize().size() == 1 &&
            poValidRange->GetDimensionsSize()[0] == 2 &&
            poValidRange->GetDataType().GetClass() == GEDTC_NUMERIC)
        {
            m_bHasValidMin = true;
            m_bHasValidMax = true;
            auto vals = poValidRange->ReadAsDoubleArray();
            CPLAssert(vals.size() == 2);
            m_dfValidMin = vals[0];
            m_dfValidMax = vals[1];
        }
    }

    // Take into account
    // https://cfconventions.org/cf-conventions/cf-conventions.html#flags
    // Cf GDALMDArray::GetMask() for semantics of UNMASK_FLAGS
    const char *pszUnmaskFlags =
        CSLFetchNameValue(papszOptions, "UNMASK_FLAGS");
    if (pszUnmaskFlags)
    {
        const auto IsScalarStringAttr =
            [](const std::shared_ptr<GDALAttribute> &poAttr)
        {
            return poAttr->GetDataType().GetClass() == GEDTC_STRING &&
                   (poAttr->GetDimensionsSize().empty() ||
                    (poAttr->GetDimensionsSize().size() == 1 &&
                     poAttr->GetDimensionsSize()[0] == 1));
        };

        auto poFlagMeanings = m_poParent->GetAttribute("flag_meanings");
        if (!(poFlagMeanings && IsScalarStringAttr(poFlagMeanings)))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "UNMASK_FLAGS option specified but array has no "
                     "flag_meanings attribute");
            return false;
        }
        const char *pszFlagMeanings = poFlagMeanings->ReadAsString();
        if (!pszFlagMeanings)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Cannot read flag_meanings attribute");
            return false;
        }

        const auto IsSingleDimNumericAttr =
            [](const std::shared_ptr<GDALAttribute> &poAttr)
        {
            return poAttr->GetDataType().GetClass() == GEDTC_NUMERIC &&
                   poAttr->GetDimensionsSize().size() == 1;
        };

        auto poFlagValues = m_poParent->GetAttribute("flag_values");
        const bool bHasFlagValues =
            poFlagValues && IsSingleDimNumericAttr(poFlagValues);

        auto poFlagMasks = m_poParent->GetAttribute("flag_masks");
        const bool bHasFlagMasks =
            poFlagMasks && IsSingleDimNumericAttr(poFlagMasks);

        if (!bHasFlagValues && !bHasFlagMasks)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Cannot find flag_values and/or flag_masks attribute");
            return false;
        }

        const CPLStringList aosUnmaskFlags(
            CSLTokenizeString2(pszUnmaskFlags, ",", 0));
        const CPLStringList aosFlagMeanings(
            CSLTokenizeString2(pszFlagMeanings, " ", 0));

        if (bHasFlagValues)
        {
            const auto eType = poFlagValues->GetDataType().GetNumericDataType();
            // We could support Int64 or UInt64, but more work...
            if (eType != GDT_Byte && eType != GDT_Int8 && eType != GDT_UInt16 &&
                eType != GDT_Int16 && eType != GDT_UInt32 && eType != GDT_Int32)
            {
                CPLError(CE_Failure, CPLE_NotSupported,
                         "Unsupported data type for flag_values attribute: %s",
                         GDALGetDataTypeName(eType));
                return false;
            }
        }

        if (bHasFlagMasks)
        {
            const auto eType = poFlagMasks->GetDataType().GetNumericDataType();
            // We could support Int64 or UInt64, but more work...
            if (eType != GDT_Byte && eType != GDT_Int8 && eType != GDT_UInt16 &&
                eType != GDT_Int16 && eType != GDT_UInt32 && eType != GDT_Int32)
            {
                CPLError(CE_Failure, CPLE_NotSupported,
                         "Unsupported data type for flag_masks attribute: %s",
                         GDALGetDataTypeName(eType));
                return false;
            }
        }

        const std::vector<double> adfValues(
            bHasFlagValues ? poFlagValues->ReadAsDoubleArray()
                           : std::vector<double>());
        const std::vector<double> adfMasks(
            bHasFlagMasks ? poFlagMasks->ReadAsDoubleArray()
                          : std::vector<double>());

        if (bHasFlagValues &&
            adfValues.size() != static_cast<size_t>(aosFlagMeanings.size()))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Number of values in flag_values attribute is different "
                     "from the one in flag_meanings");
            return false;
        }

        if (bHasFlagMasks &&
            adfMasks.size() != static_cast<size_t>(aosFlagMeanings.size()))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Number of values in flag_masks attribute is different "
                     "from the one in flag_meanings");
            return false;
        }

        for (int i = 0; i < aosUnmaskFlags.size(); ++i)
        {
            const int nIdxFlag = aosFlagMeanings.FindString(aosUnmaskFlags[i]);
            if (nIdxFlag < 0)
            {
                CPLError(
                    CE_Failure, CPLE_AppDefined,
                    "Cannot fing flag %s in flag_meanings = '%s' attribute",
                    aosUnmaskFlags[i], pszFlagMeanings);
                return false;
            }

            if (bHasFlagValues && adfValues[nIdxFlag] < 0)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Invalid value in flag_values[%d] = %f", nIdxFlag,
                         adfValues[nIdxFlag]);
                return false;
            }

            if (bHasFlagMasks && adfMasks[nIdxFlag] < 0)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Invalid value in flag_masks[%d] = %f", nIdxFlag,
                         adfMasks[nIdxFlag]);
                return false;
            }

            if (bHasFlagValues)
            {
                m_anValidFlagValues.push_back(
                    static_cast<uint32_t>(adfValues[nIdxFlag]));
            }

            if (bHasFlagMasks)
            {
                m_anValidFlagMasks.push_back(
                    static_cast<uint32_t>(adfMasks[nIdxFlag]));
            }
        }
    }

    return true;
}

/************************************************************************/
/*                             IRead()                                  */
/************************************************************************/

bool GDALMDArrayMask::IRead(const GUInt64 *arrayStartIdx, const size_t *count,
                            const GInt64 *arrayStep,
                            const GPtrDiff_t *bufferStride,
                            const GDALExtendedDataType &bufferDataType,
                            void *pDstBuffer) const
{
    if (bufferDataType.GetClass() != GEDTC_NUMERIC)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "%s: only reading to a numeric data type is supported",
                 __func__);
        return false;
    }
    size_t nElts = 1;
    const size_t nDims = GetDimensionCount();
    std::vector<GPtrDiff_t> tmpBufferStrideVector(nDims);
    for (size_t i = 0; i < nDims; i++)
        nElts *= count[i];
    if (nDims > 0)
    {
        tmpBufferStrideVector.back() = 1;
        for (size_t i = nDims - 1; i > 0;)
        {
            --i;
            tmpBufferStrideVector[i] =
                tmpBufferStrideVector[i + 1] * count[i + 1];
        }
    }

    /* Optimized case: if we are an integer data type and that there is no */
    /* attribute that can be used to set mask = 0, then fill the mask buffer */
    /* directly */
    if (!m_bHasMissingValue && !m_bHasFillValue && !m_bHasValidMin &&
        !m_bHasValidMax && m_anValidFlagValues.empty() &&
        m_anValidFlagMasks.empty() &&
        m_poParent->GetRawNoDataValue() == nullptr &&
        GDALDataTypeIsInteger(m_poParent->GetDataType().GetNumericDataType()))
    {
        const bool bBufferDataTypeIsByte = bufferDataType == m_dt;
        if (bBufferDataTypeIsByte)  // Byte case
        {
            bool bContiguous = true;
            for (size_t i = 0; i < nDims; i++)
            {
                if (bufferStride[i] != tmpBufferStrideVector[i])
                {
                    bContiguous = false;
                    break;
                }
            }
            if (bContiguous)
            {
                // CPLDebug("GDAL", "GetMask(): contiguous case");
                memset(pDstBuffer, 1, nElts);
                return true;
            }
        }

        struct Stack
        {
            size_t nIters = 0;
            GByte *dst_ptr = nullptr;
            GPtrDiff_t dst_inc_offset = 0;
        };

        std::vector<Stack> stack(std::max(static_cast<size_t>(1), nDims));
        const size_t nBufferDTSize = bufferDataType.GetSize();
        for (size_t i = 0; i < nDims; i++)
        {
            stack[i].dst_inc_offset =
                static_cast<GPtrDiff_t>(bufferStride[i] * nBufferDTSize);
        }
        stack[0].dst_ptr = static_cast<GByte *>(pDstBuffer);

        size_t dimIdx = 0;
        const size_t nDimsMinus1 = nDims > 0 ? nDims - 1 : 0;
        GByte abyOne[16];  // 16 is sizeof GDT_CFloat64
        CPLAssert(nBufferDTSize <= 16);
        const GByte flag = 1;
        GDALCopyWords64(&flag, GDT_Byte, 0, abyOne,
                        bufferDataType.GetNumericDataType(), 0, 1);

    lbl_next_depth:
        if (dimIdx == nDimsMinus1)
        {
            auto nIters = nDims > 0 ? count[dimIdx] : 1;
            GByte *dst_ptr = stack[dimIdx].dst_ptr;

            while (true)
            {
                // cppcheck-suppress knownConditionTrueFalse
                if (bBufferDataTypeIsByte)
                {
                    *dst_ptr = flag;
                }
                else
                {
                    memcpy(dst_ptr, abyOne, nBufferDTSize);
                }

                if ((--nIters) == 0)
                    break;
                dst_ptr += stack[dimIdx].dst_inc_offset;
            }
        }
        else
        {
            stack[dimIdx].nIters = count[dimIdx];
            while (true)
            {
                dimIdx++;
                stack[dimIdx].dst_ptr = stack[dimIdx - 1].dst_ptr;
                goto lbl_next_depth;
            lbl_return_to_caller:
                dimIdx--;
                if ((--stack[dimIdx].nIters) == 0)
                    break;
                stack[dimIdx].dst_ptr += stack[dimIdx].dst_inc_offset;
            }
        }
        if (dimIdx > 0)
            goto lbl_return_to_caller;

        return true;
    }

    const auto oTmpBufferDT =
        GDALDataTypeIsComplex(m_poParent->GetDataType().GetNumericDataType())
            ? GDALExtendedDataType::Create(GDT_Float64)
            : m_poParent->GetDataType();
    const size_t nTmpBufferDTSize = oTmpBufferDT.GetSize();
    void *pTempBuffer = VSI_MALLOC2_VERBOSE(nTmpBufferDTSize, nElts);
    if (!pTempBuffer)
        return false;
    if (!m_poParent->Read(arrayStartIdx, count, arrayStep,
                          tmpBufferStrideVector.data(), oTmpBufferDT,
                          pTempBuffer))
    {
        VSIFree(pTempBuffer);
        return false;
    }

    switch (oTmpBufferDT.GetNumericDataType())
    {
        case GDT_Byte:
            ReadInternal<GByte>(count, bufferStride, bufferDataType, pDstBuffer,
                                pTempBuffer, oTmpBufferDT,
                                tmpBufferStrideVector);
            break;

        case GDT_Int8:
            ReadInternal<GInt8>(count, bufferStride, bufferDataType, pDstBuffer,
                                pTempBuffer, oTmpBufferDT,
                                tmpBufferStrideVector);
            break;

        case GDT_UInt16:
            ReadInternal<GUInt16>(count, bufferStride, bufferDataType,
                                  pDstBuffer, pTempBuffer, oTmpBufferDT,
                                  tmpBufferStrideVector);
            break;

        case GDT_Int16:
            ReadInternal<GInt16>(count, bufferStride, bufferDataType,
                                 pDstBuffer, pTempBuffer, oTmpBufferDT,
                                 tmpBufferStrideVector);
            break;

        case GDT_UInt32:
            ReadInternal<GUInt32>(count, bufferStride, bufferDataType,
                                  pDstBuffer, pTempBuffer, oTmpBufferDT,
                                  tmpBufferStrideVector);
            break;

        case GDT_Int32:
            ReadInternal<GInt32>(count, bufferStride, bufferDataType,
                                 pDstBuffer, pTempBuffer, oTmpBufferDT,
                                 tmpBufferStrideVector);
            break;

        case GDT_UInt64:
            ReadInternal<std::uint64_t>(count, bufferStride, bufferDataType,
                                        pDstBuffer, pTempBuffer, oTmpBufferDT,
                                        tmpBufferStrideVector);
            break;

        case GDT_Int64:
            ReadInternal<std::int64_t>(count, bufferStride, bufferDataType,
                                       pDstBuffer, pTempBuffer, oTmpBufferDT,
                                       tmpBufferStrideVector);
            break;

        case GDT_Float16:
            ReadInternal<GFloat16>(count, bufferStride, bufferDataType,
                                   pDstBuffer, pTempBuffer, oTmpBufferDT,
                                   tmpBufferStrideVector);
            break;

        case GDT_Float32:
            ReadInternal<float>(count, bufferStride, bufferDataType, pDstBuffer,
                                pTempBuffer, oTmpBufferDT,
                                tmpBufferStrideVector);
            break;

        case GDT_Float64:
            ReadInternal<double>(count, bufferStride, bufferDataType,
                                 pDstBuffer, pTempBuffer, oTmpBufferDT,
                                 tmpBufferStrideVector);
            break;
        case GDT_Unknown:
        case GDT_CInt16:
        case GDT_CInt32:
        case GDT_CFloat16:
        case GDT_CFloat32:
        case GDT_CFloat64:
        case GDT_TypeCount:
            CPLAssert(false);
            break;
    }

    VSIFree(pTempBuffer);

    return true;
}

/************************************************************************/
/*                          IsValidForDT()                              */
/************************************************************************/

template <typename Type> static bool IsValidForDT(double dfVal)
{
    if (std::isnan(dfVal))
        return false;
    if (dfVal < static_cast<double>(cpl::NumericLimits<Type>::lowest()))
        return false;
    if (dfVal > static_cast<double>(cpl::NumericLimits<Type>::max()))
        return false;
    return static_cast<double>(static_cast<Type>(dfVal)) == dfVal;
}

template <> bool IsValidForDT<double>(double)
{
    return true;
}

/************************************************************************/
/*                              IsNan()                                 */
/************************************************************************/

template <typename Type> inline bool IsNan(Type)
{
    return false;
}

template <> bool IsNan<double>(double val)
{
    return std::isnan(val);
}

template <> bool IsNan<float>(float val)
{
    return std::isnan(val);
}

/************************************************************************/
/*                         ReadInternal()                               */
/************************************************************************/

template <typename Type>
void GDALMDArrayMask::ReadInternal(
    const size_t *count, const GPtrDiff_t *bufferStride,
    const GDALExtendedDataType &bufferDataType, void *pDstBuffer,
    const void *pTempBuffer, const GDALExtendedDataType &oTmpBufferDT,
    const std::vector<GPtrDiff_t> &tmpBufferStrideVector) const
{
    const size_t nDims = GetDimensionCount();

    const auto castValue = [](bool &bHasVal, double dfVal) -> Type
    {
        if (bHasVal)
        {
            if (IsValidForDT<Type>(dfVal))
            {
                return static_cast<Type>(dfVal);
            }
            else
            {
                bHasVal = false;
            }
        }
        return 0;
    };

    const void *pSrcRawNoDataValue = m_poParent->GetRawNoDataValue();
    bool bHasNodataValue = pSrcRawNoDataValue != nullptr;
    const Type nNoDataValue =
        castValue(bHasNodataValue, m_poParent->GetNoDataValueAsDouble());
    bool bHasMissingValue = m_bHasMissingValue;
    const Type nMissingValue = castValue(bHasMissingValue, m_dfMissingValue);
    bool bHasFillValue = m_bHasFillValue;
    const Type nFillValue = castValue(bHasFillValue, m_dfFillValue);
    bool bHasValidMin = m_bHasValidMin;
    const Type nValidMin = castValue(bHasValidMin, m_dfValidMin);
    bool bHasValidMax = m_bHasValidMax;
    const Type nValidMax = castValue(bHasValidMax, m_dfValidMax);
    const bool bHasValidFlags =
        !m_anValidFlagValues.empty() || !m_anValidFlagMasks.empty();

    const auto IsValidFlag = [this](Type v)
    {
        if (!m_anValidFlagValues.empty() && !m_anValidFlagMasks.empty())
        {
            for (size_t i = 0; i < m_anValidFlagValues.size(); ++i)
            {
                if ((static_cast<uint32_t>(v) & m_anValidFlagMasks[i]) ==
                    m_anValidFlagValues[i])
                {
                    return true;
                }
            }
        }
        else if (!m_anValidFlagValues.empty())
        {
            for (size_t i = 0; i < m_anValidFlagValues.size(); ++i)
            {
                if (static_cast<uint32_t>(v) == m_anValidFlagValues[i])
                {
                    return true;
                }
            }
        }
        else /* if( !m_anValidFlagMasks.empty() ) */
        {
            for (size_t i = 0; i < m_anValidFlagMasks.size(); ++i)
            {
                if ((static_cast<uint32_t>(v) & m_anValidFlagMasks[i]) != 0)
                {
                    return true;
                }
            }
        }
        return false;
    };

#define GET_MASK_FOR_SAMPLE(v)                                                 \
    static_cast<GByte>(!IsNan(v) && !(bHasNodataValue && v == nNoDataValue) && \
                       !(bHasMissingValue && v == nMissingValue) &&            \
                       !(bHasFillValue && v == nFillValue) &&                  \
                       !(bHasValidMin && v < nValidMin) &&                     \
                       !(bHasValidMax && v > nValidMax) &&                     \
                       (!bHasValidFlags || IsValidFlag(v)));

    const bool bBufferDataTypeIsByte = bufferDataType == m_dt;
    /* Optimized case: Byte output and output buffer is contiguous */
    if (bBufferDataTypeIsByte)
    {
        bool bContiguous = true;
        for (size_t i = 0; i < nDims; i++)
        {
            if (bufferStride[i] != tmpBufferStrideVector[i])
            {
                bContiguous = false;
                break;
            }
        }
        if (bContiguous)
        {
            size_t nElts = 1;
            for (size_t i = 0; i < nDims; i++)
                nElts *= count[i];

            for (size_t i = 0; i < nElts; i++)
            {
                const Type *pSrc = static_cast<const Type *>(pTempBuffer) + i;
                static_cast<GByte *>(pDstBuffer)[i] =
                    GET_MASK_FOR_SAMPLE(*pSrc);
            }
            return;
        }
    }

    const size_t nTmpBufferDTSize = oTmpBufferDT.GetSize();

    struct Stack
    {
        size_t nIters = 0;
        const GByte *src_ptr = nullptr;
        GByte *dst_ptr = nullptr;
        GPtrDiff_t src_inc_offset = 0;
        GPtrDiff_t dst_inc_offset = 0;
    };

    std::vector<Stack> stack(std::max(static_cast<size_t>(1), nDims));
    const size_t nBufferDTSize = bufferDataType.GetSize();
    for (size_t i = 0; i < nDims; i++)
    {
        stack[i].src_inc_offset = static_cast<GPtrDiff_t>(
            tmpBufferStrideVector[i] * nTmpBufferDTSize);
        stack[i].dst_inc_offset =
            static_cast<GPtrDiff_t>(bufferStride[i] * nBufferDTSize);
    }
    stack[0].src_ptr = static_cast<const GByte *>(pTempBuffer);
    stack[0].dst_ptr = static_cast<GByte *>(pDstBuffer);

    size_t dimIdx = 0;
    const size_t nDimsMinus1 = nDims > 0 ? nDims - 1 : 0;
    GByte abyZeroOrOne[2][16];  // 16 is sizeof GDT_CFloat64
    CPLAssert(nBufferDTSize <= 16);
    for (GByte flag = 0; flag <= 1; flag++)
    {
        GDALCopyWords64(&flag, m_dt.GetNumericDataType(), 0, abyZeroOrOne[flag],
                        bufferDataType.GetNumericDataType(), 0, 1);
    }

lbl_next_depth:
    if (dimIdx == nDimsMinus1)
    {
        auto nIters = nDims > 0 ? count[dimIdx] : 1;
        const GByte *src_ptr = stack[dimIdx].src_ptr;
        GByte *dst_ptr = stack[dimIdx].dst_ptr;

        while (true)
        {
            const Type *pSrc = reinterpret_cast<const Type *>(src_ptr);
            const GByte flag = GET_MASK_FOR_SAMPLE(*pSrc);

            if (bBufferDataTypeIsByte)
            {
                *dst_ptr = flag;
            }
            else
            {
                memcpy(dst_ptr, abyZeroOrOne[flag], nBufferDTSize);
            }

            if ((--nIters) == 0)
                break;
            src_ptr += stack[dimIdx].src_inc_offset;
            dst_ptr += stack[dimIdx].dst_inc_offset;
        }
    }
    else
    {
        stack[dimIdx].nIters = count[dimIdx];
        while (true)
        {
            dimIdx++;
            stack[dimIdx].src_ptr = stack[dimIdx - 1].src_ptr;
            stack[dimIdx].dst_ptr = stack[dimIdx - 1].dst_ptr;
            goto lbl_next_depth;
        lbl_return_to_caller:
            dimIdx--;
            if ((--stack[dimIdx].nIters) == 0)
                break;
            stack[dimIdx].src_ptr += stack[dimIdx].src_inc_offset;
            stack[dimIdx].dst_ptr += stack[dimIdx].dst_inc_offset;
        }
    }
    if (dimIdx > 0)
        goto lbl_return_to_caller;
}

/************************************************************************/
/*                            GetMask()                                 */
/************************************************************************/

/** Return an array that is a mask for the current array

 This array will be of type Byte, with values set to 0 to indicate invalid
 pixels of the current array, and values set to 1 to indicate valid pixels.

 The generic implementation honours the NoDataValue, as well as various
 netCDF CF attributes: missing_value, _FillValue, valid_min, valid_max
 and valid_range.

 Starting with GDAL 3.8, option UNMASK_FLAGS=flag_meaning_1[,flag_meaning_2,...]
 can be used to specify strings of the "flag_meanings" attribute
 (cf https://cfconventions.org/cf-conventions/cf-conventions.html#flags)
 for which pixels matching any of those flags will be set at 1 in the mask array,
 and pixels matching none of those flags will be set at 0.
 For example, let's consider the following netCDF variable defined with:
 \verbatim
 l2p_flags:valid_min = 0s ;
 l2p_flags:valid_max = 256s ;
 l2p_flags:flag_meanings = "microwave land ice lake river reserved_for_future_use unused_currently unused_currently unused_currently" ;
 l2p_flags:flag_masks = 1s, 2s, 4s, 8s, 16s, 32s, 64s, 128s, 256s ;
 \endverbatim

 GetMask(["UNMASK_FLAGS=microwave,land"]) will return an array such that:
 - for pixel values *outside* valid_range [0,256], the mask value will be 0.
 - for a pixel value with bit 0 or bit 1 at 1 within [0,256], the mask value
   will be 1.
 - for a pixel value with bit 0 and bit 1 at 0 within [0,256], the mask value
   will be 0.

 This is the same as the C function GDALMDArrayGetMask().

 @param papszOptions NULL-terminated list of options, or NULL.

 @return a new array, that holds a reference to the original one, and thus is
 a view of it (not a copy), or nullptr in case of error.
*/
std::shared_ptr<GDALMDArray>
GDALMDArray::GetMask(CSLConstList papszOptions) const
{
    auto self = std::dynamic_pointer_cast<GDALMDArray>(m_pSelf.lock());
    if (!self)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Driver implementation issue: m_pSelf not set !");
        return nullptr;
    }
    if (GetDataType().GetClass() != GEDTC_NUMERIC)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "GetMask() only supports numeric data type");
        return nullptr;
    }
    return GDALMDArrayMask::Create(self, papszOptions);
}

/************************************************************************/
/*                         IsRegularlySpaced()                          */
/************************************************************************/

/** Returns whether an array is a 1D regularly spaced array.
 *
 * @param[out] dfStart     First value in the array
 * @param[out] dfIncrement Increment/spacing between consecutive values.
 * @return true if the array is regularly spaced.
 */
bool GDALMDArray::IsRegularlySpaced(double &dfStart, double &dfIncrement) const
{
    dfStart = 0;
    dfIncrement = 0;
    if (GetDimensionCount() != 1 || GetDataType().GetClass() != GEDTC_NUMERIC)
        return false;
    const auto nSize = GetDimensions()[0]->GetSize();
    if (nSize <= 1 || nSize > 10 * 1000 * 1000)
        return false;

    size_t nCount = static_cast<size_t>(nSize);
    std::vector<double> adfTmp;
    try
    {
        adfTmp.resize(nCount);
    }
    catch (const std::exception &)
    {
        return false;
    }

    GUInt64 anStart[1] = {0};
    size_t anCount[1] = {nCount};

    const auto IsRegularlySpacedInternal =
        [&dfStart, &dfIncrement, &anCount, &adfTmp]()
    {
        dfStart = adfTmp[0];
        dfIncrement = (adfTmp[anCount[0] - 1] - adfTmp[0]) / (anCount[0] - 1);
        if (dfIncrement == 0)
        {
            return false;
        }
        for (size_t i = 1; i < anCount[0]; i++)
        {
            if (fabs((adfTmp[i] - adfTmp[i - 1]) - dfIncrement) >
                1e-3 * fabs(dfIncrement))
            {
                return false;
            }
        }
        return true;
    };

    // First try with the first block(s). This can avoid excessive processing
    // time, for example with Zarr datasets.
    // https://bugs.chromium.org/p/oss-fuzz/issues/detail?id=37636 and
    // https://bugs.chromium.org/p/oss-fuzz/issues/detail?id=39273
    const auto nBlockSize = GetBlockSize()[0];
    if (nCount >= 5 && nBlockSize <= nCount / 2)
    {
        size_t nReducedCount =
            std::max<size_t>(3, static_cast<size_t>(nBlockSize));
        while (nReducedCount < 256 && nReducedCount <= (nCount - 2) / 2)
            nReducedCount *= 2;
        anCount[0] = nReducedCount;
        if (!Read(anStart, anCount, nullptr, nullptr,
                  GDALExtendedDataType::Create(GDT_Float64), &adfTmp[0]))
        {
            return false;
        }
        if (!IsRegularlySpacedInternal())
        {
            return false;
        }

        // Get next values
        anStart[0] = nReducedCount;
        anCount[0] = nCount - nReducedCount;
    }

    if (!Read(anStart, anCount, nullptr, nullptr,
              GDALExtendedDataType::Create(GDT_Float64),
              &adfTmp[static_cast<size_t>(anStart[0])]))
    {
        return false;
    }

    return IsRegularlySpacedInternal();
}

/************************************************************************/
/*                         GuessGeoTransform()                          */
/************************************************************************/

/** Returns whether 2 specified dimensions form a geotransform
 *
 * @param nDimX                Index of the X axis.
 * @param nDimY                Index of the Y axis.
 * @param bPixelIsPoint        Whether the geotransform should be returned
 *                             with the pixel-is-point (pixel-center) convention
 *                             (bPixelIsPoint = true), or with the pixel-is-area
 *                             (top left corner convention)
 *                             (bPixelIsPoint = false)
 * @param[out] adfGeoTransform Computed geotransform
 * @return true if a geotransform could be computed.
 */
bool GDALMDArray::GuessGeoTransform(size_t nDimX, size_t nDimY,
                                    bool bPixelIsPoint,
                                    double adfGeoTransform[6]) const
{
    const auto &dims(GetDimensions());
    auto poVarX = dims[nDimX]->GetIndexingVariable();
    auto poVarY = dims[nDimY]->GetIndexingVariable();
    double dfXStart = 0.0;
    double dfXSpacing = 0.0;
    double dfYStart = 0.0;
    double dfYSpacing = 0.0;
    if (poVarX && poVarX->GetDimensionCount() == 1 &&
        poVarX->GetDimensions()[0]->GetSize() == dims[nDimX]->GetSize() &&
        poVarY && poVarY->GetDimensionCount() == 1 &&
        poVarY->GetDimensions()[0]->GetSize() == dims[nDimY]->GetSize() &&
        poVarX->IsRegularlySpaced(dfXStart, dfXSpacing) &&
        poVarY->IsRegularlySpaced(dfYStart, dfYSpacing))
    {
        adfGeoTransform[0] = dfXStart - (bPixelIsPoint ? 0 : dfXSpacing / 2);
        adfGeoTransform[1] = dfXSpacing;
        adfGeoTransform[2] = 0;
        adfGeoTransform[3] = dfYStart - (bPixelIsPoint ? 0 : dfYSpacing / 2);
        adfGeoTransform[4] = 0;
        adfGeoTransform[5] = dfYSpacing;
        return true;
    }
    return false;
}

/************************************************************************/
/*                       GDALMDArrayResampled                           */
/************************************************************************/

class GDALMDArrayResampledDataset;

class GDALMDArrayResampledDatasetRasterBand final : public GDALRasterBand
{
  protected:
    CPLErr IReadBlock(int, int, void *) override;
    CPLErr IRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize,
                     int nYSize, void *pData, int nBufXSize, int nBufYSize,
                     GDALDataType eBufType, GSpacing nPixelSpaceBuf,
                     GSpacing nLineSpaceBuf,
                     GDALRasterIOExtraArg *psExtraArg) override;

  public:
    explicit GDALMDArrayResampledDatasetRasterBand(
        GDALMDArrayResampledDataset *poDSIn);

    double GetNoDataValue(int *pbHasNoData) override;
};

class GDALMDArrayResampledDataset final : public GDALPamDataset
{
    friend class GDALMDArrayResampled;
    friend class GDALMDArrayResampledDatasetRasterBand;

    std::shared_ptr<GDALMDArray> m_poArray;
    const size_t m_iXDim;
    const size_t m_iYDim;
    double m_adfGeoTransform[6]{0, 1, 0, 0, 0, 1};
    bool m_bHasGT = false;
    mutable std::shared_ptr<OGRSpatialReference> m_poSRS{};

    std::vector<GUInt64> m_anOffset{};
    std::vector<size_t> m_anCount{};
    std::vector<GPtrDiff_t> m_anStride{};

    std::string m_osFilenameLong{};
    std::string m_osFilenameLat{};

  public:
    GDALMDArrayResampledDataset(const std::shared_ptr<GDALMDArray> &array,
                                size_t iXDim, size_t iYDim)
        : m_poArray(array), m_iXDim(iXDim), m_iYDim(iYDim),
          m_anOffset(m_poArray->GetDimensionCount(), 0),
          m_anCount(m_poArray->GetDimensionCount(), 1),
          m_anStride(m_poArray->GetDimensionCount(), 0)
    {
        const auto &dims(m_poArray->GetDimensions());

        nRasterYSize = static_cast<int>(
            std::min(static_cast<GUInt64>(INT_MAX), dims[iYDim]->GetSize()));
        nRasterXSize = static_cast<int>(
            std::min(static_cast<GUInt64>(INT_MAX), dims[iXDim]->GetSize()));

        m_bHasGT = m_poArray->GuessGeoTransform(m_iXDim, m_iYDim, false,
                                                m_adfGeoTransform);

        SetBand(1, new GDALMDArrayResampledDatasetRasterBand(this));
    }

    ~GDALMDArrayResampledDataset() override;

    CPLErr GetGeoTransform(double *padfGeoTransform) override
    {
        memcpy(padfGeoTransform, m_adfGeoTransform, 6 * sizeof(double));
        return m_bHasGT ? CE_None : CE_Failure;
    }

    const OGRSpatialReference *GetSpatialRef() const override
    {
        m_poSRS = m_poArray->GetSpatialRef();
        if (m_poSRS)
        {
            m_poSRS.reset(m_poSRS->Clone());
            auto axisMapping = m_poSRS->GetDataAxisToSRSAxisMapping();
            for (auto &m : axisMapping)
            {
                if (m == static_cast<int>(m_iXDim) + 1)
                    m = 1;
                else if (m == static_cast<int>(m_iYDim) + 1)
                    m = 2;
            }
            m_poSRS->SetDataAxisToSRSAxisMapping(axisMapping);
        }
        return m_poSRS.get();
    }

    void SetGeolocationArray(const std::string &osFilenameLong,
                             const std::string &osFilenameLat)
    {
        m_osFilenameLong = osFilenameLong;
        m_osFilenameLat = osFilenameLat;
        CPLStringList aosGeoLoc;
        aosGeoLoc.SetNameValue("LINE_OFFSET", "0");
        aosGeoLoc.SetNameValue("LINE_STEP", "1");
        aosGeoLoc.SetNameValue("PIXEL_OFFSET", "0");
        aosGeoLoc.SetNameValue("PIXEL_STEP", "1");
        aosGeoLoc.SetNameValue("SRS", SRS_WKT_WGS84_LAT_LONG);  // FIXME?
        aosGeoLoc.SetNameValue("X_BAND", "1");
        aosGeoLoc.SetNameValue("X_DATASET", m_osFilenameLong.c_str());
        aosGeoLoc.SetNameValue("Y_BAND", "1");
        aosGeoLoc.SetNameValue("Y_DATASET", m_osFilenameLat.c_str());
        aosGeoLoc.SetNameValue("GEOREFERENCING_CONVENTION", "PIXEL_CENTER");
        SetMetadata(aosGeoLoc.List(), "GEOLOCATION");
    }
};

GDALMDArrayResampledDataset::~GDALMDArrayResampledDataset()
{
    if (!m_osFilenameLong.empty())
        VSIUnlink(m_osFilenameLong.c_str());
    if (!m_osFilenameLat.empty())
        VSIUnlink(m_osFilenameLat.c_str());
}

/************************************************************************/
/*                   GDALMDArrayResampledDatasetRasterBand()            */
/************************************************************************/

GDALMDArrayResampledDatasetRasterBand::GDALMDArrayResampledDatasetRasterBand(
    GDALMDArrayResampledDataset *poDSIn)
{
    const auto &poArray(poDSIn->m_poArray);
    const auto blockSize(poArray->GetBlockSize());
    nBlockYSize = (blockSize[poDSIn->m_iYDim])
                      ? static_cast<int>(std::min(static_cast<GUInt64>(INT_MAX),
                                                  blockSize[poDSIn->m_iYDim]))
                      : 1;
    nBlockXSize = blockSize[poDSIn->m_iXDim]
                      ? static_cast<int>(std::min(static_cast<GUInt64>(INT_MAX),
                                                  blockSize[poDSIn->m_iXDim]))
                      : poDSIn->GetRasterXSize();
    eDataType = poArray->GetDataType().GetNumericDataType();
    eAccess = poDSIn->eAccess;
}

/************************************************************************/
/*                           GetNoDataValue()                           */
/************************************************************************/

double GDALMDArrayResampledDatasetRasterBand::GetNoDataValue(int *pbHasNoData)
{
    auto l_poDS(cpl::down_cast<GDALMDArrayResampledDataset *>(poDS));
    const auto &poArray(l_poDS->m_poArray);
    bool bHasNodata = false;
    double dfRes = poArray->GetNoDataValueAsDouble(&bHasNodata);
    if (pbHasNoData)
        *pbHasNoData = bHasNodata;
    return dfRes;
}

/************************************************************************/
/*                            IReadBlock()                              */
/************************************************************************/

CPLErr GDALMDArrayResampledDatasetRasterBand::IReadBlock(int nBlockXOff,
                                                         int nBlockYOff,
                                                         void *pImage)
{
    const int nDTSize(GDALGetDataTypeSizeBytes(eDataType));
    const int nXOff = nBlockXOff * nBlockXSize;
    const int nYOff = nBlockYOff * nBlockYSize;
    const int nReqXSize = std::min(nRasterXSize - nXOff, nBlockXSize);
    const int nReqYSize = std::min(nRasterYSize - nYOff, nBlockYSize);
    GDALRasterIOExtraArg sExtraArg;
    INIT_RASTERIO_EXTRA_ARG(sExtraArg);
    return IRasterIO(GF_Read, nXOff, nYOff, nReqXSize, nReqYSize, pImage,
                     nReqXSize, nReqYSize, eDataType, nDTSize,
                     static_cast<GSpacing>(nDTSize) * nBlockXSize, &sExtraArg);
}

/************************************************************************/
/*                            IRasterIO()                               */
/************************************************************************/

CPLErr GDALMDArrayResampledDatasetRasterBand::IRasterIO(
    GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize, int nYSize,
    void *pData, int nBufXSize, int nBufYSize, GDALDataType eBufType,
    GSpacing nPixelSpaceBuf, GSpacing nLineSpaceBuf,
    GDALRasterIOExtraArg *psExtraArg)
{
    auto l_poDS(cpl::down_cast<GDALMDArrayResampledDataset *>(poDS));
    const auto &poArray(l_poDS->m_poArray);
    const int nBufferDTSize(GDALGetDataTypeSizeBytes(eBufType));
    if (eRWFlag == GF_Read && nXSize == nBufXSize && nYSize == nBufYSize &&
        nBufferDTSize > 0 && (nPixelSpaceBuf % nBufferDTSize) == 0 &&
        (nLineSpaceBuf % nBufferDTSize) == 0)
    {
        l_poDS->m_anOffset[l_poDS->m_iXDim] = static_cast<GUInt64>(nXOff);
        l_poDS->m_anCount[l_poDS->m_iXDim] = static_cast<size_t>(nXSize);
        l_poDS->m_anStride[l_poDS->m_iXDim] =
            static_cast<GPtrDiff_t>(nPixelSpaceBuf / nBufferDTSize);

        l_poDS->m_anOffset[l_poDS->m_iYDim] = static_cast<GUInt64>(nYOff);
        l_poDS->m_anCount[l_poDS->m_iYDim] = static_cast<size_t>(nYSize);
        l_poDS->m_anStride[l_poDS->m_iYDim] =
            static_cast<GPtrDiff_t>(nLineSpaceBuf / nBufferDTSize);

        return poArray->Read(l_poDS->m_anOffset.data(),
                             l_poDS->m_anCount.data(), nullptr,
                             l_poDS->m_anStride.data(),
                             GDALExtendedDataType::Create(eBufType), pData)
                   ? CE_None
                   : CE_Failure;
    }
    return GDALRasterBand::IRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize,
                                     pData, nBufXSize, nBufYSize, eBufType,
                                     nPixelSpaceBuf, nLineSpaceBuf, psExtraArg);
}

class GDALMDArrayResampled final : public GDALPamMDArray
{
  private:
    std::shared_ptr<GDALMDArray> m_poParent{};
    std::vector<std::shared_ptr<GDALDimension>> m_apoDims;
    std::vector<GUInt64> m_anBlockSize;
    GDALExtendedDataType m_dt;
    std::shared_ptr<OGRSpatialReference> m_poSRS{};
    std::shared_ptr<GDALMDArray> m_poVarX{};
    std::shared_ptr<GDALMDArray> m_poVarY{};
    std::unique_ptr<GDALMDArrayResampledDataset> m_poParentDS{};
    std::unique_ptr<GDALDataset> m_poReprojectedDS{};

  protected:
    GDALMDArrayResampled(
        const std::shared_ptr<GDALMDArray> &poParent,
        const std::vector<std::shared_ptr<GDALDimension>> &apoDims,
        const std::vector<GUInt64> &anBlockSize)
        : GDALAbstractMDArray(std::string(),
                              "Resampled view of " + poParent->GetFullName()),
          GDALPamMDArray(
              std::string(), "Resampled view of " + poParent->GetFullName(),
              GDALPamMultiDim::GetPAM(poParent), poParent->GetContext()),
          m_poParent(std::move(poParent)), m_apoDims(apoDims),
          m_anBlockSize(anBlockSize), m_dt(m_poParent->GetDataType())
    {
        CPLAssert(apoDims.size() == m_poParent->GetDimensionCount());
        CPLAssert(anBlockSize.size() == m_poParent->GetDimensionCount());
    }

    bool IRead(const GUInt64 *arrayStartIdx, const size_t *count,
               const GInt64 *arrayStep, const GPtrDiff_t *bufferStride,
               const GDALExtendedDataType &bufferDataType,
               void *pDstBuffer) const override;

  public:
    static std::shared_ptr<GDALMDArray>
    Create(const std::shared_ptr<GDALMDArray> &poParent,
           const std::vector<std::shared_ptr<GDALDimension>> &apoNewDims,
           GDALRIOResampleAlg resampleAlg,
           const OGRSpatialReference *poTargetSRS, CSLConstList papszOptions);

    ~GDALMDArrayResampled()
    {
        // First close the warped VRT
        m_poReprojectedDS.reset();
        m_poParentDS.reset();
    }

    bool IsWritable() const override
    {
        return false;
    }

    const std::string &GetFilename() const override
    {
        return m_poParent->GetFilename();
    }

    const std::vector<std::shared_ptr<GDALDimension>> &
    GetDimensions() const override
    {
        return m_apoDims;
    }

    const GDALExtendedDataType &GetDataType() const override
    {
        return m_dt;
    }

    std::shared_ptr<OGRSpatialReference> GetSpatialRef() const override
    {
        return m_poSRS;
    }

    std::vector<GUInt64> GetBlockSize() const override
    {
        return m_anBlockSize;
    }

    std::shared_ptr<GDALAttribute>
    GetAttribute(const std::string &osName) const override
    {
        return m_poParent->GetAttribute(osName);
    }

    std::vector<std::shared_ptr<GDALAttribute>>
    GetAttributes(CSLConstList papszOptions = nullptr) const override
    {
        return m_poParent->GetAttributes(papszOptions);
    }

    const std::string &GetUnit() const override
    {
        return m_poParent->GetUnit();
    }

    const void *GetRawNoDataValue() const override
    {
        return m_poParent->GetRawNoDataValue();
    }

    double GetOffset(bool *pbHasOffset,
                     GDALDataType *peStorageType) const override
    {
        return m_poParent->GetOffset(pbHasOffset, peStorageType);
    }

    double GetScale(bool *pbHasScale,
                    GDALDataType *peStorageType) const override
    {
        return m_poParent->GetScale(pbHasScale, peStorageType);
    }
};

/************************************************************************/
/*                   GDALMDArrayResampled::Create()                     */
/************************************************************************/

std::shared_ptr<GDALMDArray> GDALMDArrayResampled::Create(
    const std::shared_ptr<GDALMDArray> &poParent,
    const std::vector<std::shared_ptr<GDALDimension>> &apoNewDimsIn,
    GDALRIOResampleAlg resampleAlg, const OGRSpatialReference *poTargetSRS,
    CSLConstList /* papszOptions */)
{
    const char *pszResampleAlg = "nearest";
    bool unsupported = false;
    switch (resampleAlg)
    {
        case GRIORA_NearestNeighbour:
            pszResampleAlg = "nearest";
            break;
        case GRIORA_Bilinear:
            pszResampleAlg = "bilinear";
            break;
        case GRIORA_Cubic:
            pszResampleAlg = "cubic";
            break;
        case GRIORA_CubicSpline:
            pszResampleAlg = "cubicspline";
            break;
        case GRIORA_Lanczos:
            pszResampleAlg = "lanczos";
            break;
        case GRIORA_Average:
            pszResampleAlg = "average";
            break;
        case GRIORA_Mode:
            pszResampleAlg = "mode";
            break;
        case GRIORA_Gauss:
            unsupported = true;
            break;
        case GRIORA_RESERVED_START:
            unsupported = true;
            break;
        case GRIORA_RESERVED_END:
            unsupported = true;
            break;
        case GRIORA_RMS:
            pszResampleAlg = "rms";
            break;
    }
    if (unsupported)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Unsupported resample method for GetResampled()");
        return nullptr;
    }

    if (poParent->GetDimensionCount() < 2)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "GetResampled() only supports 2 dimensions or more");
        return nullptr;
    }

    const auto &aoParentDims = poParent->GetDimensions();
    if (apoNewDimsIn.size() != aoParentDims.size())
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "GetResampled(): apoNewDims size should be the same as "
                 "GetDimensionCount()");
        return nullptr;
    }

    std::vector<std::shared_ptr<GDALDimension>> apoNewDims;
    apoNewDims.reserve(apoNewDimsIn.size());

    std::vector<GUInt64> anBlockSize;
    anBlockSize.reserve(apoNewDimsIn.size());
    const auto &anParentBlockSize = poParent->GetBlockSize();

    auto apoParentDims = poParent->GetDimensions();
    // Special case for NASA EMIT datasets
    const bool bYXBandOrder = (apoParentDims.size() == 3 &&
                               apoParentDims[0]->GetName() == "downtrack" &&
                               apoParentDims[1]->GetName() == "crosstrack" &&
                               apoParentDims[2]->GetName() == "bands");

    const size_t iYDimParent =
        bYXBandOrder ? 0 : poParent->GetDimensionCount() - 2;
    const size_t iXDimParent =
        bYXBandOrder ? 1 : poParent->GetDimensionCount() - 1;

    for (unsigned i = 0; i < apoNewDimsIn.size(); ++i)
    {
        if (i == iYDimParent || i == iXDimParent)
            continue;
        if (apoNewDimsIn[i] == nullptr)
        {
            apoNewDims.emplace_back(aoParentDims[i]);
        }
        else if (apoNewDimsIn[i]->GetSize() != aoParentDims[i]->GetSize() ||
                 apoNewDimsIn[i]->GetName() != aoParentDims[i]->GetName())
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "GetResampled(): apoNewDims[%u] should be the same "
                     "as its parent",
                     i);
            return nullptr;
        }
        else
        {
            apoNewDims.emplace_back(aoParentDims[i]);
        }
        anBlockSize.emplace_back(anParentBlockSize[i]);
    }

    std::unique_ptr<GDALMDArrayResampledDataset> poParentDS(
        new GDALMDArrayResampledDataset(poParent, iXDimParent, iYDimParent));

    double dfXStart = 0.0;
    double dfXSpacing = 0.0;
    bool gotXSpacing = false;
    auto poNewDimX = apoNewDimsIn[iXDimParent];
    if (poNewDimX)
    {
        if (poNewDimX->GetSize() > static_cast<GUInt64>(INT_MAX))
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "Too big size for X dimension");
            return nullptr;
        }
        auto var = poNewDimX->GetIndexingVariable();
        if (var)
        {
            if (var->GetDimensionCount() != 1 ||
                var->GetDimensions()[0]->GetSize() != poNewDimX->GetSize() ||
                !var->IsRegularlySpaced(dfXStart, dfXSpacing))
            {
                CPLError(CE_Failure, CPLE_NotSupported,
                         "New X dimension should be indexed by a regularly "
                         "spaced variable");
                return nullptr;
            }
            gotXSpacing = true;
        }
    }

    double dfYStart = 0.0;
    double dfYSpacing = 0.0;
    auto poNewDimY = apoNewDimsIn[iYDimParent];
    bool gotYSpacing = false;
    if (poNewDimY)
    {
        if (poNewDimY->GetSize() > static_cast<GUInt64>(INT_MAX))
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "Too big size for Y dimension");
            return nullptr;
        }
        auto var = poNewDimY->GetIndexingVariable();
        if (var)
        {
            if (var->GetDimensionCount() != 1 ||
                var->GetDimensions()[0]->GetSize() != poNewDimY->GetSize() ||
                !var->IsRegularlySpaced(dfYStart, dfYSpacing))
            {
                CPLError(CE_Failure, CPLE_NotSupported,
                         "New Y dimension should be indexed by a regularly "
                         "spaced variable");
                return nullptr;
            }
            gotYSpacing = true;
        }
    }

    // This limitation could probably be removed
    if ((gotXSpacing && !gotYSpacing) || (!gotXSpacing && gotYSpacing))
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Either none of new X or Y dimension should have an indexing "
                 "variable, or both should both should have one.");
        return nullptr;
    }

    std::string osDstWKT;
    if (poTargetSRS)
    {
        char *pszDstWKT = nullptr;
        if (poTargetSRS->exportToWkt(&pszDstWKT) != OGRERR_NONE)
        {
            CPLFree(pszDstWKT);
            return nullptr;
        }
        osDstWKT = pszDstWKT;
        CPLFree(pszDstWKT);
    }

    // Use coordinate variables for geolocation array
    const auto apoCoordinateVars = poParent->GetCoordinateVariables();
    bool useGeolocationArray = false;
    if (apoCoordinateVars.size() >= 2)
    {
        std::shared_ptr<GDALMDArray> poLongVar;
        std::shared_ptr<GDALMDArray> poLatVar;
        for (const auto &poCoordVar : apoCoordinateVars)
        {
            const auto &osName = poCoordVar->GetName();
            const auto poAttr = poCoordVar->GetAttribute("standard_name");
            std::string osStandardName;
            if (poAttr && poAttr->GetDataType().GetClass() == GEDTC_STRING &&
                poAttr->GetDimensionCount() == 0)
            {
                const char *pszStandardName = poAttr->ReadAsString();
                if (pszStandardName)
                    osStandardName = pszStandardName;
            }
            if (osName == "lon" || osName == "longitude" ||
                osName == "Longitude" || osStandardName == "longitude")
            {
                poLongVar = poCoordVar;
            }
            else if (osName == "lat" || osName == "latitude" ||
                     osName == "Latitude" || osStandardName == "latitude")
            {
                poLatVar = poCoordVar;
            }
        }
        if (poLatVar != nullptr && poLongVar != nullptr)
        {
            const auto longDimCount = poLongVar->GetDimensionCount();
            const auto &longDims = poLongVar->GetDimensions();
            const auto latDimCount = poLatVar->GetDimensionCount();
            const auto &latDims = poLatVar->GetDimensions();
            const auto xDimSize = aoParentDims[iXDimParent]->GetSize();
            const auto yDimSize = aoParentDims[iYDimParent]->GetSize();
            if (longDimCount == 1 && longDims[0]->GetSize() == xDimSize &&
                latDimCount == 1 && latDims[0]->GetSize() == yDimSize)
            {
                // Geolocation arrays are 1D, and of consistent size with
                // the variable
                useGeolocationArray = true;
            }
            else if ((longDimCount == 2 ||
                      (longDimCount == 3 && longDims[0]->GetSize() == 1)) &&
                     longDims[longDimCount - 2]->GetSize() == yDimSize &&
                     longDims[longDimCount - 1]->GetSize() == xDimSize &&
                     (latDimCount == 2 ||
                      (latDimCount == 3 && latDims[0]->GetSize() == 1)) &&
                     latDims[latDimCount - 2]->GetSize() == yDimSize &&
                     latDims[latDimCount - 1]->GetSize() == xDimSize)

            {
                // Geolocation arrays are 2D (or 3D with first dimension of
                // size 1, as found in Sentinel 5P products), and of consistent
                // size with the variable
                useGeolocationArray = true;
            }
            else
            {
                CPLDebug(
                    "GDAL",
                    "Longitude and latitude coordinate variables found, "
                    "but their characteristics are not compatible of using "
                    "them as geolocation arrays");
            }
            if (useGeolocationArray)
            {
                CPLDebug("GDAL",
                         "Setting geolocation array from variables %s and %s",
                         poLongVar->GetName().c_str(),
                         poLatVar->GetName().c_str());
                const std::string osFilenameLong =
                    VSIMemGenerateHiddenFilename("longitude.tif");
                const std::string osFilenameLat =
                    VSIMemGenerateHiddenFilename("latitude.tif");
                std::unique_ptr<GDALDataset> poTmpLongDS(
                    longDimCount == 1
                        ? poLongVar->AsClassicDataset(0, 0)
                        : poLongVar->AsClassicDataset(longDimCount - 1,
                                                      longDimCount - 2));
                auto hTIFFLongDS = GDALTranslate(
                    osFilenameLong.c_str(),
                    GDALDataset::ToHandle(poTmpLongDS.get()), nullptr, nullptr);
                std::unique_ptr<GDALDataset> poTmpLatDS(
                    latDimCount == 1 ? poLatVar->AsClassicDataset(0, 0)
                                     : poLatVar->AsClassicDataset(
                                           latDimCount - 1, latDimCount - 2));
                auto hTIFFLatDS = GDALTranslate(
                    osFilenameLat.c_str(),
                    GDALDataset::ToHandle(poTmpLatDS.get()), nullptr, nullptr);
                const bool bError =
                    (hTIFFLatDS == nullptr || hTIFFLongDS == nullptr);
                GDALClose(hTIFFLongDS);
                GDALClose(hTIFFLatDS);
                if (bError)
                {
                    VSIUnlink(osFilenameLong.c_str());
                    VSIUnlink(osFilenameLat.c_str());
                    return nullptr;
                }

                poParentDS->SetGeolocationArray(osFilenameLong, osFilenameLat);
            }
        }
        else
        {
            CPLDebug("GDAL",
                     "Coordinate variables available for %s, but "
                     "longitude and/or latitude variables were not identified",
                     poParent->GetName().c_str());
        }
    }

    // Build gdalwarp arguments
    CPLStringList aosArgv;

    aosArgv.AddString("-of");
    aosArgv.AddString("VRT");

    aosArgv.AddString("-r");
    aosArgv.AddString(pszResampleAlg);

    if (!osDstWKT.empty())
    {
        aosArgv.AddString("-t_srs");
        aosArgv.AddString(osDstWKT.c_str());
    }

    if (useGeolocationArray)
        aosArgv.AddString("-geoloc");

    if (gotXSpacing && gotYSpacing)
    {
        const double dfXMin = dfXStart - dfXSpacing / 2;
        const double dfXMax =
            dfXMin + dfXSpacing * static_cast<double>(poNewDimX->GetSize());
        const double dfYMax = dfYStart - dfYSpacing / 2;
        const double dfYMin =
            dfYMax + dfYSpacing * static_cast<double>(poNewDimY->GetSize());
        aosArgv.AddString("-te");
        aosArgv.AddString(CPLSPrintf("%.17g", dfXMin));
        aosArgv.AddString(CPLSPrintf("%.17g", dfYMin));
        aosArgv.AddString(CPLSPrintf("%.17g", dfXMax));
        aosArgv.AddString(CPLSPrintf("%.17g", dfYMax));
    }

    if (poNewDimX && poNewDimY)
    {
        aosArgv.AddString("-ts");
        aosArgv.AddString(
            CPLSPrintf("%d", static_cast<int>(poNewDimX->GetSize())));
        aosArgv.AddString(
            CPLSPrintf("%d", static_cast<int>(poNewDimY->GetSize())));
    }
    else if (poNewDimX)
    {
        aosArgv.AddString("-ts");
        aosArgv.AddString(
            CPLSPrintf("%d", static_cast<int>(poNewDimX->GetSize())));
        aosArgv.AddString("0");
    }
    else if (poNewDimY)
    {
        aosArgv.AddString("-ts");
        aosArgv.AddString("0");
        aosArgv.AddString(
            CPLSPrintf("%d", static_cast<int>(poNewDimY->GetSize())));
    }

    // Create a warped VRT dataset
    GDALWarpAppOptions *psOptions =
        GDALWarpAppOptionsNew(aosArgv.List(), nullptr);
    GDALDatasetH hSrcDS = GDALDataset::ToHandle(poParentDS.get());
    std::unique_ptr<GDALDataset> poReprojectedDS(GDALDataset::FromHandle(
        GDALWarp("", nullptr, 1, &hSrcDS, psOptions, nullptr)));
    GDALWarpAppOptionsFree(psOptions);
    if (poReprojectedDS == nullptr)
        return nullptr;

    int nBlockXSize;
    int nBlockYSize;
    poReprojectedDS->GetRasterBand(1)->GetBlockSize(&nBlockXSize, &nBlockYSize);
    anBlockSize.emplace_back(nBlockYSize);
    anBlockSize.emplace_back(nBlockXSize);

    double adfGeoTransform[6] = {0, 0, 0, 0, 0, 0};
    CPLErr eErr = poReprojectedDS->GetGeoTransform(adfGeoTransform);
    CPLAssert(eErr == CE_None);
    CPL_IGNORE_RET_VAL(eErr);

    auto poDimY = std::make_shared<GDALDimensionWeakIndexingVar>(
        std::string(), "dimY", GDAL_DIM_TYPE_HORIZONTAL_Y, "NORTH",
        poReprojectedDS->GetRasterYSize());
    auto varY = GDALMDArrayRegularlySpaced::Create(
        std::string(), poDimY->GetName(), poDimY,
        adfGeoTransform[3] + adfGeoTransform[5] / 2, adfGeoTransform[5], 0);
    poDimY->SetIndexingVariable(varY);

    auto poDimX = std::make_shared<GDALDimensionWeakIndexingVar>(
        std::string(), "dimX", GDAL_DIM_TYPE_HORIZONTAL_X, "EAST",
        poReprojectedDS->GetRasterXSize());
    auto varX = GDALMDArrayRegularlySpaced::Create(
        std::string(), poDimX->GetName(), poDimX,
        adfGeoTransform[0] + adfGeoTransform[1] / 2, adfGeoTransform[1], 0);
    poDimX->SetIndexingVariable(varX);

    apoNewDims.emplace_back(poDimY);
    apoNewDims.emplace_back(poDimX);
    auto newAr(std::shared_ptr<GDALMDArrayResampled>(
        new GDALMDArrayResampled(poParent, apoNewDims, anBlockSize)));
    newAr->SetSelf(newAr);
    if (poTargetSRS)
    {
        newAr->m_poSRS.reset(poTargetSRS->Clone());
    }
    else
    {
        newAr->m_poSRS = poParent->GetSpatialRef();
    }
    newAr->m_poVarX = varX;
    newAr->m_poVarY = varY;
    newAr->m_poReprojectedDS = std::move(poReprojectedDS);
    newAr->m_poParentDS = std::move(poParentDS);

    // If the input array is y,x,band ordered, the above newAr is
    // actually band,y,x ordered as it is more convenient for
    // GDALMDArrayResampled::IRead() implementation. But transpose that
    // array to the order of the input array
    if (bYXBandOrder)
        return newAr->Transpose(std::vector<int>{1, 2, 0});

    return newAr;
}

/************************************************************************/
/*                   GDALMDArrayResampled::IRead()                      */
/************************************************************************/

bool GDALMDArrayResampled::IRead(const GUInt64 *arrayStartIdx,
                                 const size_t *count, const GInt64 *arrayStep,
                                 const GPtrDiff_t *bufferStride,
                                 const GDALExtendedDataType &bufferDataType,
                                 void *pDstBuffer) const
{
    if (bufferDataType.GetClass() != GEDTC_NUMERIC)
        return false;

    struct Stack
    {
        size_t nIters = 0;
        GByte *dst_ptr = nullptr;
        GPtrDiff_t dst_inc_offset = 0;
    };

    const auto nDims = GetDimensionCount();
    std::vector<Stack> stack(nDims + 1);  // +1 to avoid -Wnull-dereference
    const size_t nBufferDTSize = bufferDataType.GetSize();
    for (size_t i = 0; i < nDims; i++)
    {
        stack[i].dst_inc_offset =
            static_cast<GPtrDiff_t>(bufferStride[i] * nBufferDTSize);
    }
    stack[0].dst_ptr = static_cast<GByte *>(pDstBuffer);

    size_t dimIdx = 0;
    const size_t iDimY = nDims - 2;
    const size_t iDimX = nDims - 1;
    // Use an array to avoid a false positive warning from CLang Static
    // Analyzer about flushCaches being never read
    bool flushCaches[] = {false};
    const bool bYXBandOrder =
        m_poParentDS->m_iYDim == 0 && m_poParentDS->m_iXDim == 1;

lbl_next_depth:
    if (dimIdx == iDimY)
    {
        if (flushCaches[0])
        {
            flushCaches[0] = false;
            // When changing of 2D slice, flush GDAL 2D buffers
            m_poParentDS->FlushCache(false);
            m_poReprojectedDS->FlushCache(false);
        }

        if (!GDALMDRasterIOFromBand(m_poReprojectedDS->GetRasterBand(1),
                                    GF_Read, iDimX, iDimY, arrayStartIdx, count,
                                    arrayStep, bufferStride, bufferDataType,
                                    stack[dimIdx].dst_ptr))
        {
            return false;
        }
    }
    else
    {
        stack[dimIdx].nIters = count[dimIdx];
        if (m_poParentDS->m_anOffset[bYXBandOrder ? 2 : dimIdx] !=
            arrayStartIdx[dimIdx])
        {
            flushCaches[0] = true;
        }
        m_poParentDS->m_anOffset[bYXBandOrder ? 2 : dimIdx] =
            arrayStartIdx[dimIdx];
        while (true)
        {
            dimIdx++;
            stack[dimIdx].dst_ptr = stack[dimIdx - 1].dst_ptr;
            goto lbl_next_depth;
        lbl_return_to_caller:
            dimIdx--;
            if ((--stack[dimIdx].nIters) == 0)
                break;
            flushCaches[0] = true;
            ++m_poParentDS->m_anOffset[bYXBandOrder ? 2 : dimIdx];
            stack[dimIdx].dst_ptr += stack[dimIdx].dst_inc_offset;
        }
    }
    if (dimIdx > 0)
        goto lbl_return_to_caller;

    return true;
}

/************************************************************************/
/*                           GetResampled()                             */
/************************************************************************/

/** Return an array that is a resampled / reprojected view of the current array
 *
 * This is the same as the C function GDALMDArrayGetResampled().
 *
 * Currently this method can only resample along the last 2 dimensions, unless
 * orthorectifying a NASA EMIT dataset.
 *
 * For NASA EMIT datasets, if apoNewDims[] and poTargetSRS is NULL, the
 * geometry lookup table (GLT) is used by default for fast orthorectification.
 *
 * Options available are:
 * <ul>
 * <li>EMIT_ORTHORECTIFICATION=YES/NO: defaults to YES for a NASA EMIT dataset.
 * Can be set to NO to use generic reprojection method.
 * </li>
 * <li>USE_GOOD_WAVELENGTHS=YES/NO: defaults to YES. Only used for EMIT
 * orthorectification to take into account the value of the
 * /sensor_band_parameters/good_wavelengths array to decide if slices of the
 * current array along the band dimension are valid.</li>
 * </ul>
 *
 * @param apoNewDims New dimensions. Its size should be GetDimensionCount().
 *                   apoNewDims[i] can be NULL to let the method automatically
 *                   determine it.
 * @param resampleAlg Resampling algorithm
 * @param poTargetSRS Target SRS, or nullptr
 * @param papszOptions NULL-terminated list of options, or NULL.
 *
 * @return a new array, that holds a reference to the original one, and thus is
 * a view of it (not a copy), or nullptr in case of error.
 *
 * @since 3.4
 */
std::shared_ptr<GDALMDArray> GDALMDArray::GetResampled(
    const std::vector<std::shared_ptr<GDALDimension>> &apoNewDims,
    GDALRIOResampleAlg resampleAlg, const OGRSpatialReference *poTargetSRS,
    CSLConstList papszOptions) const
{
    auto self = std::dynamic_pointer_cast<GDALMDArray>(m_pSelf.lock());
    if (!self)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Driver implementation issue: m_pSelf not set !");
        return nullptr;
    }
    if (GetDataType().GetClass() != GEDTC_NUMERIC)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "GetResampled() only supports numeric data type");
        return nullptr;
    }

    // Special case for NASA EMIT datasets
    auto apoDims = GetDimensions();
    if (poTargetSRS == nullptr &&
        ((apoDims.size() == 3 && apoDims[0]->GetName() == "downtrack" &&
          apoDims[1]->GetName() == "crosstrack" &&
          apoDims[2]->GetName() == "bands" &&
          (apoNewDims == std::vector<std::shared_ptr<GDALDimension>>(3) ||
           apoNewDims ==
               std::vector<std::shared_ptr<GDALDimension>>{nullptr, nullptr,
                                                           apoDims[2]})) ||
         (apoDims.size() == 2 && apoDims[0]->GetName() == "downtrack" &&
          apoDims[1]->GetName() == "crosstrack" &&
          apoNewDims == std::vector<std::shared_ptr<GDALDimension>>(2))) &&
        CPLTestBool(CSLFetchNameValueDef(papszOptions,
                                         "EMIT_ORTHORECTIFICATION", "YES")))
    {
        auto poRootGroup = GetRootGroup();
        if (poRootGroup)
        {
            auto poAttrGeotransform = poRootGroup->GetAttribute("geotransform");
            auto poLocationGroup = poRootGroup->OpenGroup("location");
            if (poAttrGeotransform &&
                poAttrGeotransform->GetDataType().GetClass() == GEDTC_NUMERIC &&
                poAttrGeotransform->GetDimensionCount() == 1 &&
                poAttrGeotransform->GetDimensionsSize()[0] == 6 &&
                poLocationGroup)
            {
                auto poGLT_X = poLocationGroup->OpenMDArray("glt_x");
                auto poGLT_Y = poLocationGroup->OpenMDArray("glt_y");
                if (poGLT_X && poGLT_X->GetDimensionCount() == 2 &&
                    poGLT_X->GetDimensions()[0]->GetName() == "ortho_y" &&
                    poGLT_X->GetDimensions()[1]->GetName() == "ortho_x" &&
                    poGLT_Y && poGLT_Y->GetDimensionCount() == 2 &&
                    poGLT_Y->GetDimensions()[0]->GetName() == "ortho_y" &&
                    poGLT_Y->GetDimensions()[1]->GetName() == "ortho_x")
                {
                    return CreateGLTOrthorectified(
                        self, poRootGroup, poGLT_X, poGLT_Y,
                        /* nGLTIndexOffset = */ -1,
                        poAttrGeotransform->ReadAsDoubleArray(), papszOptions);
                }
            }
        }
    }

    if (CPLTestBool(CSLFetchNameValueDef(papszOptions,
                                         "EMIT_ORTHORECTIFICATION", "NO")))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "EMIT_ORTHORECTIFICATION required, but dataset and/or "
                 "parameters are not compatible with it");
        return nullptr;
    }

    return GDALMDArrayResampled::Create(self, apoNewDims, resampleAlg,
                                        poTargetSRS, papszOptions);
}

/************************************************************************/
/*                         GDALDatasetFromArray()                       */
/************************************************************************/

class GDALDatasetFromArray;

namespace
{
struct MetadataItem
{
    std::shared_ptr<GDALAbstractMDArray> poArray{};
    std::string osName{};
    std::string osDefinition{};
    bool bDefinitionUsesPctForG = false;
};

struct BandImageryMetadata
{
    std::shared_ptr<GDALAbstractMDArray> poCentralWavelengthArray{};
    double dfCentralWavelengthToMicrometer = 1.0;
    std::shared_ptr<GDALAbstractMDArray> poFWHMArray{};
    double dfFWHMToMicrometer = 1.0;
};

}  // namespace

class GDALRasterBandFromArray final : public GDALPamRasterBand
{
    std::vector<GUInt64> m_anOffset{};
    std::vector<size_t> m_anCount{};
    std::vector<GPtrDiff_t> m_anStride{};

  protected:
    CPLErr IReadBlock(int, int, void *) override;
    CPLErr IWriteBlock(int, int, void *) override;
    CPLErr IRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize,
                     int nYSize, void *pData, int nBufXSize, int nBufYSize,
                     GDALDataType eBufType, GSpacing nPixelSpaceBuf,
                     GSpacing nLineSpaceBuf,
                     GDALRasterIOExtraArg *psExtraArg) override;

  public:
    explicit GDALRasterBandFromArray(
        GDALDatasetFromArray *poDSIn,
        const std::vector<GUInt64> &anOtherDimCoord,
        const std::vector<std::vector<MetadataItem>>
            &aoBandParameterMetadataItems,
        const std::vector<BandImageryMetadata> &aoBandImageryMetadata,
        double dfDelay, time_t nStartTime, bool &bHasWarned);

    double GetNoDataValue(int *pbHasNoData) override;
    int64_t GetNoDataValueAsInt64(int *pbHasNoData) override;
    uint64_t GetNoDataValueAsUInt64(int *pbHasNoData) override;
    double GetOffset(int *pbHasOffset) override;
    double GetScale(int *pbHasScale) override;
    const char *GetUnitType() override;
    GDALColorInterp GetColorInterpretation() override;
};

class GDALDatasetFromArray final : public GDALPamDataset
{
    friend class GDALRasterBandFromArray;

    std::shared_ptr<GDALMDArray> m_poArray;
    size_t m_iXDim;
    size_t m_iYDim;
    double m_adfGeoTransform[6]{0, 1, 0, 0, 0, 1};
    bool m_bHasGT = false;
    mutable std::shared_ptr<OGRSpatialReference> m_poSRS{};
    GDALMultiDomainMetadata m_oMDD{};
    std::string m_osOvrFilename{};

  public:
    GDALDatasetFromArray(const std::shared_ptr<GDALMDArray> &array,
                         size_t iXDim, size_t iYDim)
        : m_poArray(array), m_iXDim(iXDim), m_iYDim(iYDim)
    {
        // Initialize an overview filename from the filename of the array
        // and its name.
        const std::string &osFilename = m_poArray->GetFilename();
        if (!osFilename.empty())
        {
            m_osOvrFilename = osFilename;
            m_osOvrFilename += '.';
            for (char ch : m_poArray->GetName())
            {
                if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
                    (ch >= 'a' && ch <= 'z') || ch == '_')
                {
                    m_osOvrFilename += ch;
                }
                else
                {
                    m_osOvrFilename += '_';
                }
            }
            m_osOvrFilename += ".ovr";
            oOvManager.Initialize(this);
        }
    }

    static GDALDatasetFromArray *
    Create(const std::shared_ptr<GDALMDArray> &array, size_t iXDim,
           size_t iYDim, const std::shared_ptr<GDALGroup> &poRootGroup,
           CSLConstList papszOptions);

    ~GDALDatasetFromArray() override;

    CPLErr Close() override
    {
        CPLErr eErr = CE_None;
        if (nOpenFlags != OPEN_FLAGS_CLOSED)
        {
            if (GDALDatasetFromArray::FlushCache(/*bAtClosing=*/true) !=
                CE_None)
                eErr = CE_Failure;
            m_poArray.reset();
        }
        return eErr;
    }

    CPLErr GetGeoTransform(double *padfGeoTransform) override
    {
        memcpy(padfGeoTransform, m_adfGeoTransform, 6 * sizeof(double));
        return m_bHasGT ? CE_None : CE_Failure;
    }

    const OGRSpatialReference *GetSpatialRef() const override
    {
        if (m_poArray->GetDimensionCount() < 2)
            return nullptr;
        m_poSRS = m_poArray->GetSpatialRef();
        if (m_poSRS)
        {
            m_poSRS.reset(m_poSRS->Clone());
            auto axisMapping = m_poSRS->GetDataAxisToSRSAxisMapping();
            for (auto &m : axisMapping)
            {
                if (m == static_cast<int>(m_iXDim) + 1)
                    m = 1;
                else if (m == static_cast<int>(m_iYDim) + 1)
                    m = 2;
            }
            m_poSRS->SetDataAxisToSRSAxisMapping(axisMapping);
        }
        return m_poSRS.get();
    }

    CPLErr SetMetadata(char **papszMetadata, const char *pszDomain) override
    {
        return m_oMDD.SetMetadata(papszMetadata, pszDomain);
    }

    char **GetMetadata(const char *pszDomain) override
    {
        return m_oMDD.GetMetadata(pszDomain);
    }

    const char *GetMetadataItem(const char *pszName,
                                const char *pszDomain) override
    {
        if (!m_osOvrFilename.empty() && pszName &&
            EQUAL(pszName, "OVERVIEW_FILE") && pszDomain &&
            EQUAL(pszDomain, "OVERVIEWS"))
        {
            return m_osOvrFilename.c_str();
        }
        return m_oMDD.GetMetadataItem(pszName, pszDomain);
    }
};

GDALDatasetFromArray::~GDALDatasetFromArray()
{
    GDALDatasetFromArray::Close();
}

/************************************************************************/
/*                      GDALRasterBandFromArray()                       */
/************************************************************************/

GDALRasterBandFromArray::GDALRasterBandFromArray(
    GDALDatasetFromArray *poDSIn, const std::vector<GUInt64> &anOtherDimCoord,
    const std::vector<std::vector<MetadataItem>> &aoBandParameterMetadataItems,
    const std::vector<BandImageryMetadata> &aoBandImageryMetadata,
    double dfDelay, time_t nStartTime, bool &bHasWarned)
{
    const auto &poArray(poDSIn->m_poArray);
    const auto &dims(poArray->GetDimensions());
    const auto nDimCount(dims.size());
    const auto blockSize(poArray->GetBlockSize());
    nBlockYSize = (nDimCount >= 2 && blockSize[poDSIn->m_iYDim])
                      ? static_cast<int>(std::min(static_cast<GUInt64>(INT_MAX),
                                                  blockSize[poDSIn->m_iYDim]))
                      : 1;
    nBlockXSize = blockSize[poDSIn->m_iXDim]
                      ? static_cast<int>(std::min(static_cast<GUInt64>(INT_MAX),
                                                  blockSize[poDSIn->m_iXDim]))
                      : poDSIn->GetRasterXSize();
    eDataType = poArray->GetDataType().GetNumericDataType();
    eAccess = poDSIn->eAccess;
    m_anOffset.resize(nDimCount);
    m_anCount.resize(nDimCount, 1);
    m_anStride.resize(nDimCount);
    for (size_t i = 0, j = 0; i < nDimCount; ++i)
    {
        if (i != poDSIn->m_iXDim && !(nDimCount >= 2 && i == poDSIn->m_iYDim))
        {
            std::string dimName(dims[i]->GetName());
            GUInt64 nIndex = anOtherDimCoord[j];
            // Detect subset_{orig_dim_name}_{start}_{incr}_{size} names of
            // subsetted dimensions as generated by GetView()
            if (STARTS_WITH(dimName.c_str(), "subset_"))
            {
                CPLStringList aosTokens(
                    CSLTokenizeString2(dimName.c_str(), "_", 0));
                if (aosTokens.size() == 5)
                {
                    dimName = aosTokens[1];
                    const auto nStartDim = static_cast<GUInt64>(CPLScanUIntBig(
                        aosTokens[2], static_cast<int>(strlen(aosTokens[2]))));
                    const auto nIncrDim = CPLAtoGIntBig(aosTokens[3]);
                    nIndex = nIncrDim > 0 ? nStartDim + nIndex * nIncrDim
                                          : nStartDim - (nIndex * -nIncrDim);
                }
            }
            if (nDimCount != 3 || dimName != "Band")
            {
                SetMetadataItem(
                    CPLSPrintf("DIM_%s_INDEX", dimName.c_str()),
                    CPLSPrintf(CPL_FRMT_GUIB, static_cast<GUIntBig>(nIndex)));
            }

            auto indexingVar = dims[i]->GetIndexingVariable();

            // If the indexing variable is also listed in band parameter arrays,
            // then don't use our default formatting
            if (indexingVar)
            {
                for (const auto &oItem : aoBandParameterMetadataItems[j])
                {
                    if (oItem.poArray->GetFullName() ==
                        indexingVar->GetFullName())
                    {
                        indexingVar.reset();
                        break;
                    }
                }
            }

            if (indexingVar && indexingVar->GetDimensionCount() == 1 &&
                indexingVar->GetDimensions()[0]->GetSize() ==
                    dims[i]->GetSize())
            {
                if (dfDelay >= 0 && time(nullptr) - nStartTime > dfDelay)
                {
                    if (!bHasWarned)
                    {
                        CPLError(
                            CE_Warning, CPLE_AppDefined,
                            "Maximum delay to load band metadata from "
                            "dimension indexing variables has expired. "
                            "Increase the value of the "
                            "LOAD_EXTRA_DIM_METADATA_DELAY "
                            "option of GDALMDArray::AsClassicDataset() "
                            "(also accessible as the "
                            "GDAL_LOAD_EXTRA_DIM_METADATA_DELAY "
                            "configuration option), "
                            "or set it to 'unlimited' for unlimited delay. ");
                        bHasWarned = true;
                    }
                }
                else
                {
                    size_t nCount = 1;
                    const auto &dt(indexingVar->GetDataType());
                    std::vector<GByte> abyTmp(dt.GetSize());
                    if (indexingVar->Read(&(anOtherDimCoord[j]), &nCount,
                                          nullptr, nullptr, dt, &abyTmp[0]))
                    {
                        char *pszTmp = nullptr;
                        GDALExtendedDataType::CopyValue(
                            &abyTmp[0], dt, &pszTmp,
                            GDALExtendedDataType::CreateString());
                        if (pszTmp)
                        {
                            SetMetadataItem(
                                CPLSPrintf("DIM_%s_VALUE", dimName.c_str()),
                                pszTmp);
                            CPLFree(pszTmp);
                        }

                        const auto &unit(indexingVar->GetUnit());
                        if (!unit.empty())
                        {
                            SetMetadataItem(
                                CPLSPrintf("DIM_%s_UNIT", dimName.c_str()),
                                unit.c_str());
                        }
                    }
                }
            }

            for (const auto &oItem : aoBandParameterMetadataItems[j])
            {
                CPLString osVal;

                size_t nCount = 1;
                const auto &dt(oItem.poArray->GetDataType());
                if (oItem.bDefinitionUsesPctForG)
                {
                    // There is one and only one %[x][.y]f|g in osDefinition
                    std::vector<GByte> abyTmp(dt.GetSize());
                    if (oItem.poArray->Read(&(anOtherDimCoord[j]), &nCount,
                                            nullptr, nullptr, dt, &abyTmp[0]))
                    {
                        double dfVal = 0;
                        GDALExtendedDataType::CopyValue(
                            &abyTmp[0], dt, &dfVal,
                            GDALExtendedDataType::Create(GDT_Float64));
                        osVal.Printf(oItem.osDefinition.c_str(), dfVal);
                    }
                }
                else
                {
                    // There should be zero or one %s in osDefinition
                    char *pszValue = nullptr;
                    if (dt.GetClass() == GEDTC_STRING)
                    {
                        CPL_IGNORE_RET_VAL(oItem.poArray->Read(
                            &(anOtherDimCoord[j]), &nCount, nullptr, nullptr,
                            dt, &pszValue));
                    }
                    else
                    {
                        std::vector<GByte> abyTmp(dt.GetSize());
                        if (oItem.poArray->Read(&(anOtherDimCoord[j]), &nCount,
                                                nullptr, nullptr, dt,
                                                &abyTmp[0]))
                        {
                            GDALExtendedDataType::CopyValue(
                                &abyTmp[0], dt, &pszValue,
                                GDALExtendedDataType::CreateString());
                        }
                    }

                    if (pszValue)
                    {
                        osVal.Printf(oItem.osDefinition.c_str(), pszValue);
                        CPLFree(pszValue);
                    }
                }
                if (!osVal.empty())
                    SetMetadataItem(oItem.osName.c_str(), osVal);
            }

            if (aoBandImageryMetadata[j].poCentralWavelengthArray)
            {
                auto &poCentralWavelengthArray =
                    aoBandImageryMetadata[j].poCentralWavelengthArray;
                size_t nCount = 1;
                const auto &dt(poCentralWavelengthArray->GetDataType());
                std::vector<GByte> abyTmp(dt.GetSize());
                if (poCentralWavelengthArray->Read(&(anOtherDimCoord[j]),
                                                   &nCount, nullptr, nullptr,
                                                   dt, &abyTmp[0]))
                {
                    double dfVal = 0;
                    GDALExtendedDataType::CopyValue(
                        &abyTmp[0], dt, &dfVal,
                        GDALExtendedDataType::Create(GDT_Float64));
                    SetMetadataItem(
                        "CENTRAL_WAVELENGTH_UM",
                        CPLSPrintf(
                            "%g", dfVal * aoBandImageryMetadata[j]
                                              .dfCentralWavelengthToMicrometer),
                        "IMAGERY");
                }
            }

            if (aoBandImageryMetadata[j].poFWHMArray)
            {
                auto &poFWHMArray = aoBandImageryMetadata[j].poFWHMArray;
                size_t nCount = 1;
                const auto &dt(poFWHMArray->GetDataType());
                std::vector<GByte> abyTmp(dt.GetSize());
                if (poFWHMArray->Read(&(anOtherDimCoord[j]), &nCount, nullptr,
                                      nullptr, dt, &abyTmp[0]))
                {
                    double dfVal = 0;
                    GDALExtendedDataType::CopyValue(
                        &abyTmp[0], dt, &dfVal,
                        GDALExtendedDataType::Create(GDT_Float64));
                    SetMetadataItem(
                        "FWHM_UM",
                        CPLSPrintf("%g", dfVal * aoBandImageryMetadata[j]
                                                     .dfFWHMToMicrometer),
                        "IMAGERY");
                }
            }

            m_anOffset[i] = anOtherDimCoord[j];
            j++;
        }
    }
}

/************************************************************************/
/*                           GetNoDataValue()                           */
/************************************************************************/

double GDALRasterBandFromArray::GetNoDataValue(int *pbHasNoData)
{
    auto l_poDS(cpl::down_cast<GDALDatasetFromArray *>(poDS));
    const auto &poArray(l_poDS->m_poArray);
    bool bHasNodata = false;
    const auto res = poArray->GetNoDataValueAsDouble(&bHasNodata);
    if (pbHasNoData)
        *pbHasNoData = bHasNodata;
    return res;
}

/************************************************************************/
/*                       GetNoDataValueAsInt64()                        */
/************************************************************************/

int64_t GDALRasterBandFromArray::GetNoDataValueAsInt64(int *pbHasNoData)
{
    auto l_poDS(cpl::down_cast<GDALDatasetFromArray *>(poDS));
    const auto &poArray(l_poDS->m_poArray);
    bool bHasNodata = false;
    const auto nodata = poArray->GetNoDataValueAsInt64(&bHasNodata);
    if (pbHasNoData)
        *pbHasNoData = bHasNodata;
    return nodata;
}

/************************************************************************/
/*                      GetNoDataValueAsUInt64()                        */
/************************************************************************/

uint64_t GDALRasterBandFromArray::GetNoDataValueAsUInt64(int *pbHasNoData)
{
    auto l_poDS(cpl::down_cast<GDALDatasetFromArray *>(poDS));
    const auto &poArray(l_poDS->m_poArray);
    bool bHasNodata = false;
    const auto nodata = poArray->GetNoDataValueAsUInt64(&bHasNodata);
    if (pbHasNoData)
        *pbHasNoData = bHasNodata;
    return nodata;
}

/************************************************************************/
/*                             GetOffset()                              */
/************************************************************************/

double GDALRasterBandFromArray::GetOffset(int *pbHasOffset)
{
    auto l_poDS(cpl::down_cast<GDALDatasetFromArray *>(poDS));
    const auto &poArray(l_poDS->m_poArray);
    bool bHasValue = false;
    double dfRes = poArray->GetOffset(&bHasValue);
    if (pbHasOffset)
        *pbHasOffset = bHasValue;
    return dfRes;
}

/************************************************************************/
/*                           GetUnitType()                              */
/************************************************************************/

const char *GDALRasterBandFromArray::GetUnitType()
{
    auto l_poDS(cpl::down_cast<GDALDatasetFromArray *>(poDS));
    const auto &poArray(l_poDS->m_poArray);
    return poArray->GetUnit().c_str();
}

/************************************************************************/
/*                             GetScale()                              */
/************************************************************************/

double GDALRasterBandFromArray::GetScale(int *pbHasScale)
{
    auto l_poDS(cpl::down_cast<GDALDatasetFromArray *>(poDS));
    const auto &poArray(l_poDS->m_poArray);
    bool bHasValue = false;
    double dfRes = poArray->GetScale(&bHasValue);
    if (pbHasScale)
        *pbHasScale = bHasValue;
    return dfRes;
}

/************************************************************************/
/*                            IReadBlock()                              */
/************************************************************************/

CPLErr GDALRasterBandFromArray::IReadBlock(int nBlockXOff, int nBlockYOff,
                                           void *pImage)
{
    const int nDTSize(GDALGetDataTypeSizeBytes(eDataType));
    const int nXOff = nBlockXOff * nBlockXSize;
    const int nYOff = nBlockYOff * nBlockYSize;
    const int nReqXSize = std::min(nRasterXSize - nXOff, nBlockXSize);
    const int nReqYSize = std::min(nRasterYSize - nYOff, nBlockYSize);
    GDALRasterIOExtraArg sExtraArg;
    INIT_RASTERIO_EXTRA_ARG(sExtraArg);
    return IRasterIO(GF_Read, nXOff, nYOff, nReqXSize, nReqYSize, pImage,
                     nReqXSize, nReqYSize, eDataType, nDTSize,
                     static_cast<GSpacing>(nDTSize) * nBlockXSize, &sExtraArg);
}

/************************************************************************/
/*                            IWriteBlock()                             */
/************************************************************************/

CPLErr GDALRasterBandFromArray::IWriteBlock(int nBlockXOff, int nBlockYOff,
                                            void *pImage)
{
    const int nDTSize(GDALGetDataTypeSizeBytes(eDataType));
    const int nXOff = nBlockXOff * nBlockXSize;
    const int nYOff = nBlockYOff * nBlockYSize;
    const int nReqXSize = std::min(nRasterXSize - nXOff, nBlockXSize);
    const int nReqYSize = std::min(nRasterYSize - nYOff, nBlockYSize);
    GDALRasterIOExtraArg sExtraArg;
    INIT_RASTERIO_EXTRA_ARG(sExtraArg);
    return IRasterIO(GF_Write, nXOff, nYOff, nReqXSize, nReqYSize, pImage,
                     nReqXSize, nReqYSize, eDataType, nDTSize,
                     static_cast<GSpacing>(nDTSize) * nBlockXSize, &sExtraArg);
}

/************************************************************************/
/*                            IRasterIO()                               */
/************************************************************************/

CPLErr GDALRasterBandFromArray::IRasterIO(GDALRWFlag eRWFlag, int nXOff,
                                          int nYOff, int nXSize, int nYSize,
                                          void *pData, int nBufXSize,
                                          int nBufYSize, GDALDataType eBufType,
                                          GSpacing nPixelSpaceBuf,
                                          GSpacing nLineSpaceBuf,
                                          GDALRasterIOExtraArg *psExtraArg)
{
    auto l_poDS(cpl::down_cast<GDALDatasetFromArray *>(poDS));
    const auto &poArray(l_poDS->m_poArray);
    const int nBufferDTSize(GDALGetDataTypeSizeBytes(eBufType));
    if (nXSize == nBufXSize && nYSize == nBufYSize && nBufferDTSize > 0 &&
        (nPixelSpaceBuf % nBufferDTSize) == 0 &&
        (nLineSpaceBuf % nBufferDTSize) == 0)
    {
        m_anOffset[l_poDS->m_iXDim] = static_cast<GUInt64>(nXOff);
        m_anCount[l_poDS->m_iXDim] = static_cast<size_t>(nXSize);
        m_anStride[l_poDS->m_iXDim] =
            static_cast<GPtrDiff_t>(nPixelSpaceBuf / nBufferDTSize);
        if (poArray->GetDimensionCount() >= 2)
        {
            m_anOffset[l_poDS->m_iYDim] = static_cast<GUInt64>(nYOff);
            m_anCount[l_poDS->m_iYDim] = static_cast<size_t>(nYSize);
            m_anStride[l_poDS->m_iYDim] =
                static_cast<GPtrDiff_t>(nLineSpaceBuf / nBufferDTSize);
        }
        if (eRWFlag == GF_Read)
        {
            return poArray->Read(m_anOffset.data(), m_anCount.data(), nullptr,
                                 m_anStride.data(),
                                 GDALExtendedDataType::Create(eBufType), pData)
                       ? CE_None
                       : CE_Failure;
        }
        else
        {
            return poArray->Write(m_anOffset.data(), m_anCount.data(), nullptr,
                                  m_anStride.data(),
                                  GDALExtendedDataType::Create(eBufType), pData)
                       ? CE_None
                       : CE_Failure;
        }
    }
    return GDALRasterBand::IRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize,
                                     pData, nBufXSize, nBufYSize, eBufType,
                                     nPixelSpaceBuf, nLineSpaceBuf, psExtraArg);
}

/************************************************************************/
/*                      GetColorInterpretation()                        */
/************************************************************************/

GDALColorInterp GDALRasterBandFromArray::GetColorInterpretation()
{
    auto l_poDS(cpl::down_cast<GDALDatasetFromArray *>(poDS));
    const auto &poArray(l_poDS->m_poArray);
    auto poAttr = poArray->GetAttribute("COLOR_INTERPRETATION");
    if (poAttr && poAttr->GetDataType().GetClass() == GEDTC_STRING)
    {
        bool bOK = false;
        GUInt64 nStartIndex = 0;
        if (poArray->GetDimensionCount() == 2 &&
            poAttr->GetDimensionCount() == 0)
        {
            bOK = true;
        }
        else if (poArray->GetDimensionCount() == 3)
        {
            uint64_t nExtraDimSamples = 1;
            const auto &apoDims = poArray->GetDimensions();
            for (size_t i = 0; i < apoDims.size(); ++i)
            {
                if (i != l_poDS->m_iXDim && i != l_poDS->m_iYDim)
                    nExtraDimSamples *= apoDims[i]->GetSize();
            }
            if (poAttr->GetDimensionsSize() ==
                std::vector<GUInt64>{static_cast<GUInt64>(nExtraDimSamples)})
            {
                bOK = true;
            }
            nStartIndex = nBand - 1;
        }
        if (bOK)
        {
            const auto oStringDT = GDALExtendedDataType::CreateString();
            const size_t nCount = 1;
            const GInt64 arrayStep = 1;
            const GPtrDiff_t bufferStride = 1;
            char *pszValue = nullptr;
            poAttr->Read(&nStartIndex, &nCount, &arrayStep, &bufferStride,
                         oStringDT, &pszValue);
            if (pszValue)
            {
                const auto eColorInterp =
                    GDALGetColorInterpretationByName(pszValue);
                CPLFree(pszValue);
                return eColorInterp;
            }
        }
    }
    return GCI_Undefined;
}

/************************************************************************/
/*                    GDALDatasetFromArray::Create()                    */
/************************************************************************/

GDALDatasetFromArray *GDALDatasetFromArray::Create(
    const std::shared_ptr<GDALMDArray> &array, size_t iXDim, size_t iYDim,
    const std::shared_ptr<GDALGroup> &poRootGroup, CSLConstList papszOptions)

{
    const auto nDimCount(array->GetDimensionCount());
    if (nDimCount == 0)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Unsupported number of dimensions");
        return nullptr;
    }
    if (array->GetDataType().GetClass() != GEDTC_NUMERIC ||
        array->GetDataType().GetNumericDataType() == GDT_Unknown)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Only arrays with numeric data types "
                 "can be exposed as classic GDALDataset");
        return nullptr;
    }
    if (iXDim >= nDimCount ||
        (nDimCount >= 2 && (iYDim >= nDimCount || iXDim == iYDim)))
    {
        CPLError(CE_Failure, CPLE_NotSupported, "Invalid iXDim and/or iYDim");
        return nullptr;
    }
    GUInt64 nTotalBands = 1;
    const auto &dims(array->GetDimensions());
    for (size_t i = 0; i < nDimCount; ++i)
    {
        if (i != iXDim && !(nDimCount >= 2 && i == iYDim))
        {
            if (dims[i]->GetSize() > 65536 / nTotalBands)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Too many bands. Operate on a sliced view");
                return nullptr;
            }
            nTotalBands *= dims[i]->GetSize();
        }
    }

    std::map<std::string, size_t> oMapArrayDimNameToExtraDimIdx;
    std::vector<size_t> oMapArrayExtraDimIdxToOriginalIdx;
    for (size_t i = 0, j = 0; i < nDimCount; ++i)
    {
        if (i != iXDim && !(nDimCount >= 2 && i == iYDim))
        {
            oMapArrayDimNameToExtraDimIdx[dims[i]->GetName()] = j;
            oMapArrayExtraDimIdxToOriginalIdx.push_back(i);
            ++j;
        }
    }

    const size_t nNewDimCount = nDimCount >= 2 ? nDimCount - 2 : 0;

    const char *pszBandMetadata =
        CSLFetchNameValue(papszOptions, "BAND_METADATA");
    std::vector<std::vector<MetadataItem>> aoBandParameterMetadataItems(
        nNewDimCount);
    if (pszBandMetadata)
    {
        if (!poRootGroup)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Root group should be provided when BAND_METADATA is set");
            return nullptr;
        }
        CPLJSONDocument oDoc;
        if (!oDoc.LoadMemory(pszBandMetadata))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Invalid JSON content for BAND_METADATA");
            return nullptr;
        }
        auto oRoot = oDoc.GetRoot();
        if (oRoot.GetType() != CPLJSONObject::Type::Array)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Value of BAND_METADATA should be an array");
            return nullptr;
        }

        auto oArray = oRoot.ToArray();
        for (int j = 0; j < oArray.Size(); ++j)
        {
            const auto oJsonItem = oArray[j];
            MetadataItem oItem;
            size_t iExtraDimIdx = 0;

            const auto osBandArrayFullname = oJsonItem.GetString("array");
            const auto osBandAttributeName = oJsonItem.GetString("attribute");
            std::shared_ptr<GDALMDArray> poArray;
            std::shared_ptr<GDALAttribute> poAttribute;
            if (osBandArrayFullname.empty() && osBandAttributeName.empty())
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "BAND_METADATA[%d][\"array\"] or "
                         "BAND_METADATA[%d][\"attribute\"] is missing",
                         j, j);
                return nullptr;
            }
            else if (!osBandArrayFullname.empty() &&
                     !osBandAttributeName.empty())
            {
                CPLError(
                    CE_Failure, CPLE_AppDefined,
                    "BAND_METADATA[%d][\"array\"] and "
                    "BAND_METADATA[%d][\"attribute\"] are mutually exclusive",
                    j, j);
                return nullptr;
            }
            else if (!osBandArrayFullname.empty())
            {
                poArray =
                    poRootGroup->OpenMDArrayFromFullname(osBandArrayFullname);
                if (!poArray)
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Array %s cannot be found",
                             osBandArrayFullname.c_str());
                    return nullptr;
                }
                if (poArray->GetDimensionCount() != 1)
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Array %s is not a 1D array",
                             osBandArrayFullname.c_str());
                    return nullptr;
                }
                const auto &osAuxArrayDimName =
                    poArray->GetDimensions()[0]->GetName();
                auto oIter =
                    oMapArrayDimNameToExtraDimIdx.find(osAuxArrayDimName);
                if (oIter == oMapArrayDimNameToExtraDimIdx.end())
                {
                    CPLError(
                        CE_Failure, CPLE_AppDefined,
                        "Dimension %s of array %s is not a non-X/Y dimension "
                        "of array %s",
                        osAuxArrayDimName.c_str(), osBandArrayFullname.c_str(),
                        array->GetName().c_str());
                    return nullptr;
                }
                iExtraDimIdx = oIter->second;
                CPLAssert(iExtraDimIdx < nNewDimCount);
            }
            else
            {
                CPLAssert(!osBandAttributeName.empty());
                poAttribute = !osBandAttributeName.empty() &&
                                      osBandAttributeName[0] == '/'
                                  ? poRootGroup->OpenAttributeFromFullname(
                                        osBandAttributeName)
                                  : array->GetAttribute(osBandAttributeName);
                if (!poAttribute)
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Attribute %s cannot be found",
                             osBandAttributeName.c_str());
                    return nullptr;
                }
                const auto aoAttrDims = poAttribute->GetDimensionsSize();
                if (aoAttrDims.size() != 1)
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Attribute %s is not a 1D array",
                             osBandAttributeName.c_str());
                    return nullptr;
                }
                bool found = false;
                for (const auto &iter : oMapArrayDimNameToExtraDimIdx)
                {
                    if (dims[oMapArrayExtraDimIdxToOriginalIdx[iter.second]]
                            ->GetSize() == aoAttrDims[0])
                    {
                        if (found)
                        {
                            CPLError(CE_Failure, CPLE_AppDefined,
                                     "Several dimensions of %s have the same "
                                     "size as attribute %s. Cannot infer which "
                                     "one to bind to!",
                                     array->GetName().c_str(),
                                     osBandAttributeName.c_str());
                            return nullptr;
                        }
                        found = true;
                        iExtraDimIdx = iter.second;
                    }
                }
                if (!found)
                {
                    CPLError(
                        CE_Failure, CPLE_AppDefined,
                        "No dimension of %s has the same size as attribute %s",
                        array->GetName().c_str(), osBandAttributeName.c_str());
                    return nullptr;
                }
            }

            oItem.osName = oJsonItem.GetString("item_name");
            if (oItem.osName.empty())
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "BAND_METADATA[%d][\"item_name\"] is missing", j);
                return nullptr;
            }

            const auto osDefinition = oJsonItem.GetString("item_value", "%s");

            // Check correctness of definition
            bool bFirstNumericFormatter = true;
            std::string osModDefinition;
            bool bDefinitionUsesPctForG = false;
            for (size_t k = 0; k < osDefinition.size(); ++k)
            {
                if (osDefinition[k] == '%')
                {
                    osModDefinition += osDefinition[k];
                    if (k + 1 == osDefinition.size())
                    {
                        CPLError(CE_Failure, CPLE_AppDefined,
                                 "Value of "
                                 "BAND_METADATA[%d][\"item_value\"] = "
                                 "%s is invalid at offset %d",
                                 j, osDefinition.c_str(), int(k));
                        return nullptr;
                    }
                    ++k;
                    if (osDefinition[k] == '%')
                    {
                        osModDefinition += osDefinition[k];
                        continue;
                    }
                    if (!bFirstNumericFormatter)
                    {
                        CPLError(CE_Failure, CPLE_AppDefined,
                                 "Value of "
                                 "BAND_METADATA[%d][\"item_value\"] = %s is "
                                 "invalid at offset %d: %%[x][.y]f|g or %%s "
                                 "formatters should be specified at most once",
                                 j, osDefinition.c_str(), int(k));
                        return nullptr;
                    }
                    bFirstNumericFormatter = false;
                    for (; k < osDefinition.size(); ++k)
                    {
                        osModDefinition += osDefinition[k];
                        if (!((osDefinition[k] >= '0' &&
                               osDefinition[k] <= '9') ||
                              osDefinition[k] == '.'))
                            break;
                    }
                    if (k == osDefinition.size() ||
                        (osDefinition[k] != 'f' && osDefinition[k] != 'g' &&
                         osDefinition[k] != 's'))
                    {
                        CPLError(CE_Failure, CPLE_AppDefined,
                                 "Value of "
                                 "BAND_METADATA[%d][\"item_value\"] = "
                                 "%s is invalid at offset %d: only "
                                 "%%[x][.y]f|g or %%s formatters are accepted",
                                 j, osDefinition.c_str(), int(k));
                        return nullptr;
                    }
                    bDefinitionUsesPctForG =
                        (osDefinition[k] == 'f' || osDefinition[k] == 'g');
                    if (bDefinitionUsesPctForG)
                    {
                        if (poArray &&
                            poArray->GetDataType().GetClass() != GEDTC_NUMERIC)
                        {
                            CPLError(CE_Failure, CPLE_AppDefined,
                                     "Data type of %s array is not numeric",
                                     poArray->GetName().c_str());
                            return nullptr;
                        }
                        else if (poAttribute &&
                                 poAttribute->GetDataType().GetClass() !=
                                     GEDTC_NUMERIC)
                        {
                            CPLError(CE_Failure, CPLE_AppDefined,
                                     "Data type of %s attribute is not numeric",
                                     poAttribute->GetFullName().c_str());
                            return nullptr;
                        }
                    }
                }
                else if (osDefinition[k] == '$' &&
                         k + 1 < osDefinition.size() &&
                         osDefinition[k + 1] == '{')
                {
                    const auto nPos = osDefinition.find('}', k);
                    if (nPos == std::string::npos)
                    {
                        CPLError(CE_Failure, CPLE_AppDefined,
                                 "Value of "
                                 "BAND_METADATA[%d][\"item_value\"] = "
                                 "%s is invalid at offset %d",
                                 j, osDefinition.c_str(), int(k));
                        return nullptr;
                    }
                    const auto osAttrName =
                        osDefinition.substr(k + 2, nPos - (k + 2));
                    std::shared_ptr<GDALAttribute> poAttr;
                    if (poArray && !osAttrName.empty() && osAttrName[0] != '/')
                    {
                        poAttr = poArray->GetAttribute(osAttrName);
                        if (!poAttr)
                        {
                            CPLError(
                                CE_Failure, CPLE_AppDefined,
                                "Value of "
                                "BAND_METADATA[%d][\"item_value\"] = "
                                "%s is invalid: %s is not an attribute of %s",
                                j, osDefinition.c_str(), osAttrName.c_str(),
                                poArray->GetName().c_str());
                            return nullptr;
                        }
                    }
                    else
                    {
                        poAttr =
                            poRootGroup->OpenAttributeFromFullname(osAttrName);
                        if (!poAttr)
                        {
                            CPLError(CE_Failure, CPLE_AppDefined,
                                     "Value of "
                                     "BAND_METADATA[%d][\"item_value\"] = "
                                     "%s is invalid: %s is not an attribute",
                                     j, osDefinition.c_str(),
                                     osAttrName.c_str());
                            return nullptr;
                        }
                    }
                    k = nPos;
                    const char *pszValue = poAttr->ReadAsString();
                    if (!pszValue)
                    {
                        CPLError(CE_Failure, CPLE_AppDefined,
                                 "Cannot get value of attribute %s as a "
                                 "string",
                                 osAttrName.c_str());
                        return nullptr;
                    }
                    osModDefinition += pszValue;
                }
                else
                {
                    osModDefinition += osDefinition[k];
                }
            }

            if (poArray)
                oItem.poArray = std::move(poArray);
            else
                oItem.poArray = std::move(poAttribute);
            oItem.osDefinition = std::move(osModDefinition);
            oItem.bDefinitionUsesPctForG = bDefinitionUsesPctForG;

            aoBandParameterMetadataItems[iExtraDimIdx].emplace_back(
                std::move(oItem));
        }
    }

    std::vector<BandImageryMetadata> aoBandImageryMetadata(nNewDimCount);
    const char *pszBandImageryMetadata =
        CSLFetchNameValue(papszOptions, "BAND_IMAGERY_METADATA");
    if (pszBandImageryMetadata)
    {
        if (!poRootGroup)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Root group should be provided when BAND_IMAGERY_METADATA "
                     "is set");
            return nullptr;
        }
        CPLJSONDocument oDoc;
        if (!oDoc.LoadMemory(pszBandImageryMetadata))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Invalid JSON content for BAND_IMAGERY_METADATA");
            return nullptr;
        }
        auto oRoot = oDoc.GetRoot();
        if (oRoot.GetType() != CPLJSONObject::Type::Object)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Value of BAND_IMAGERY_METADATA should be an object");
            return nullptr;
        }
        for (const auto &oJsonItem : oRoot.GetChildren())
        {
            if (oJsonItem.GetName() == "CENTRAL_WAVELENGTH_UM" ||
                oJsonItem.GetName() == "FWHM_UM")
            {
                const auto osBandArrayFullname = oJsonItem.GetString("array");
                const auto osBandAttributeName =
                    oJsonItem.GetString("attribute");
                std::shared_ptr<GDALMDArray> poArray;
                std::shared_ptr<GDALAttribute> poAttribute;
                size_t iExtraDimIdx = 0;
                if (osBandArrayFullname.empty() && osBandAttributeName.empty())
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "BAND_IMAGERY_METADATA[\"%s\"][\"array\"] or "
                             "BAND_IMAGERY_METADATA[\"%s\"][\"attribute\"] is "
                             "missing",
                             oJsonItem.GetName().c_str(),
                             oJsonItem.GetName().c_str());
                    return nullptr;
                }
                else if (!osBandArrayFullname.empty() &&
                         !osBandAttributeName.empty())
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "BAND_IMAGERY_METADATA[\"%s\"][\"array\"] and "
                             "BAND_IMAGERY_METADATA[\"%s\"][\"attribute\"] are "
                             "mutually exclusive",
                             oJsonItem.GetName().c_str(),
                             oJsonItem.GetName().c_str());
                    return nullptr;
                }
                else if (!osBandArrayFullname.empty())
                {
                    poArray = poRootGroup->OpenMDArrayFromFullname(
                        osBandArrayFullname);
                    if (!poArray)
                    {
                        CPLError(CE_Failure, CPLE_AppDefined,
                                 "Array %s cannot be found",
                                 osBandArrayFullname.c_str());
                        return nullptr;
                    }
                    if (poArray->GetDimensionCount() != 1)
                    {
                        CPLError(CE_Failure, CPLE_AppDefined,
                                 "Array %s is not a 1D array",
                                 osBandArrayFullname.c_str());
                        return nullptr;
                    }
                    const auto &osAuxArrayDimName =
                        poArray->GetDimensions()[0]->GetName();
                    auto oIter =
                        oMapArrayDimNameToExtraDimIdx.find(osAuxArrayDimName);
                    if (oIter == oMapArrayDimNameToExtraDimIdx.end())
                    {
                        CPLError(CE_Failure, CPLE_AppDefined,
                                 "Dimension \"%s\" of array \"%s\" is not a "
                                 "non-X/Y dimension of array \"%s\"",
                                 osAuxArrayDimName.c_str(),
                                 osBandArrayFullname.c_str(),
                                 array->GetName().c_str());
                        return nullptr;
                    }
                    iExtraDimIdx = oIter->second;
                    CPLAssert(iExtraDimIdx < nNewDimCount);
                }
                else
                {
                    poAttribute =
                        !osBandAttributeName.empty() &&
                                osBandAttributeName[0] == '/'
                            ? poRootGroup->OpenAttributeFromFullname(
                                  osBandAttributeName)
                            : array->GetAttribute(osBandAttributeName);
                    if (!poAttribute)
                    {
                        CPLError(CE_Failure, CPLE_AppDefined,
                                 "Attribute %s cannot be found",
                                 osBandAttributeName.c_str());
                        return nullptr;
                    }
                    const auto aoAttrDims = poAttribute->GetDimensionsSize();
                    if (aoAttrDims.size() != 1)
                    {
                        CPLError(CE_Failure, CPLE_AppDefined,
                                 "Attribute %s is not a 1D array",
                                 osBandAttributeName.c_str());
                        return nullptr;
                    }
                    bool found = false;
                    for (const auto &iter : oMapArrayDimNameToExtraDimIdx)
                    {
                        if (dims[oMapArrayExtraDimIdxToOriginalIdx[iter.second]]
                                ->GetSize() == aoAttrDims[0])
                        {
                            if (found)
                            {
                                CPLError(CE_Failure, CPLE_AppDefined,
                                         "Several dimensions of %s have the "
                                         "same size as attribute %s. Cannot "
                                         "infer which one to bind to!",
                                         array->GetName().c_str(),
                                         osBandAttributeName.c_str());
                                return nullptr;
                            }
                            found = true;
                            iExtraDimIdx = iter.second;
                        }
                    }
                    if (!found)
                    {
                        CPLError(CE_Failure, CPLE_AppDefined,
                                 "No dimension of %s has the same size as "
                                 "attribute %s",
                                 array->GetName().c_str(),
                                 osBandAttributeName.c_str());
                        return nullptr;
                    }
                }

                std::string osUnit = oJsonItem.GetString("unit", "um");
                if (STARTS_WITH(osUnit.c_str(), "${"))
                {
                    if (osUnit.back() != '}')
                    {
                        CPLError(CE_Failure, CPLE_AppDefined,
                                 "Value of "
                                 "BAND_IMAGERY_METADATA[\"%s\"][\"unit\"] = "
                                 "%s is invalid",
                                 oJsonItem.GetName().c_str(), osUnit.c_str());
                        return nullptr;
                    }
                    const auto osAttrName = osUnit.substr(2, osUnit.size() - 3);
                    std::shared_ptr<GDALAttribute> poAttr;
                    if (poArray && !osAttrName.empty() && osAttrName[0] != '/')
                    {
                        poAttr = poArray->GetAttribute(osAttrName);
                        if (!poAttr)
                        {
                            CPLError(
                                CE_Failure, CPLE_AppDefined,
                                "Value of "
                                "BAND_IMAGERY_METADATA[\"%s\"][\"unit\"] = "
                                "%s is invalid: %s is not an attribute of %s",
                                oJsonItem.GetName().c_str(), osUnit.c_str(),
                                osAttrName.c_str(),
                                osBandArrayFullname.c_str());
                            return nullptr;
                        }
                    }
                    else
                    {
                        poAttr =
                            poRootGroup->OpenAttributeFromFullname(osAttrName);
                        if (!poAttr)
                        {
                            CPLError(
                                CE_Failure, CPLE_AppDefined,
                                "Value of "
                                "BAND_IMAGERY_METADATA[\"%s\"][\"unit\"] = "
                                "%s is invalid: %s is not an attribute",
                                oJsonItem.GetName().c_str(), osUnit.c_str(),
                                osAttrName.c_str());
                            return nullptr;
                        }
                    }

                    const char *pszValue = poAttr->ReadAsString();
                    if (!pszValue)
                    {
                        CPLError(CE_Failure, CPLE_AppDefined,
                                 "Cannot get value of attribute %s of %s as a "
                                 "string",
                                 osAttrName.c_str(),
                                 osBandArrayFullname.c_str());
                        return nullptr;
                    }
                    osUnit = pszValue;
                }
                double dfConvToUM = 1.0;
                if (osUnit == "nm" || osUnit == "nanometre" ||
                    osUnit == "nanometres" || osUnit == "nanometer" ||
                    osUnit == "nanometers")
                {
                    dfConvToUM = 1e-3;
                }
                else if (!(osUnit == "um" || osUnit == "micrometre" ||
                           osUnit == "micrometres" || osUnit == "micrometer" ||
                           osUnit == "micrometers"))
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Unhandled value for "
                             "BAND_IMAGERY_METADATA[\"%s\"][\"unit\"] = %s",
                             oJsonItem.GetName().c_str(), osUnit.c_str());
                    return nullptr;
                }

                BandImageryMetadata &item = aoBandImageryMetadata[iExtraDimIdx];

                std::shared_ptr<GDALAbstractMDArray> abstractArray;
                if (poArray)
                    abstractArray = std::move(poArray);
                else
                    abstractArray = std::move(poAttribute);
                if (oJsonItem.GetName() == "CENTRAL_WAVELENGTH_UM")
                {
                    item.poCentralWavelengthArray = std::move(abstractArray);
                    item.dfCentralWavelengthToMicrometer = dfConvToUM;
                }
                else
                {
                    item.poFWHMArray = std::move(abstractArray);
                    item.dfFWHMToMicrometer = dfConvToUM;
                }
            }
            else
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "Ignored member \"%s\" in BAND_IMAGERY_METADATA",
                         oJsonItem.GetName().c_str());
            }
        }
    }

    auto poDS = std::make_unique<GDALDatasetFromArray>(array, iXDim, iYDim);

    poDS->eAccess = array->IsWritable() ? GA_Update : GA_ReadOnly;

    poDS->nRasterYSize =
        nDimCount < 2 ? 1
                      : static_cast<int>(std::min(static_cast<GUInt64>(INT_MAX),
                                                  dims[iYDim]->GetSize()));
    poDS->nRasterXSize = static_cast<int>(
        std::min(static_cast<GUInt64>(INT_MAX), dims[iXDim]->GetSize()));

    std::vector<GUInt64> anOtherDimCoord(nNewDimCount);
    std::vector<GUInt64> anStackIters(nDimCount);
    std::vector<size_t> anMapNewToOld(nNewDimCount);
    for (size_t i = 0, j = 0; i < nDimCount; ++i)
    {
        if (i != iXDim && !(nDimCount >= 2 && i == iYDim))
        {
            anMapNewToOld[j] = i;
            j++;
        }
    }

    poDS->m_bHasGT =
        array->GuessGeoTransform(iXDim, iYDim, false, poDS->m_adfGeoTransform);

    const auto attrs(array->GetAttributes());
    for (const auto &attr : attrs)
    {
        if (attr->GetName() != "COLOR_INTERPRETATION")
        {
            auto stringArray = attr->ReadAsStringArray();
            std::string val;
            if (stringArray.size() > 1)
            {
                val += '{';
            }
            for (int i = 0; i < stringArray.size(); ++i)
            {
                if (i > 0)
                    val += ',';
                val += stringArray[i];
            }
            if (stringArray.size() > 1)
            {
                val += '}';
            }
            poDS->m_oMDD.SetMetadataItem(attr->GetName().c_str(), val.c_str());
        }
    }

    const char *pszDelay = CSLFetchNameValueDef(
        papszOptions, "LOAD_EXTRA_DIM_METADATA_DELAY",
        CPLGetConfigOption("GDAL_LOAD_EXTRA_DIM_METADATA_DELAY", "5"));
    const double dfDelay =
        EQUAL(pszDelay, "unlimited") ? -1 : CPLAtof(pszDelay);
    const auto nStartTime = time(nullptr);
    bool bHasWarned = false;
    // Instantiate bands by iterating over non-XY variables
    size_t iDim = 0;
    int nCurBand = 1;
lbl_next_depth:
    if (iDim < nNewDimCount)
    {
        anStackIters[iDim] = dims[anMapNewToOld[iDim]]->GetSize();
        anOtherDimCoord[iDim] = 0;
        while (true)
        {
            ++iDim;
            goto lbl_next_depth;
        lbl_return_to_caller:
            --iDim;
            --anStackIters[iDim];
            if (anStackIters[iDim] == 0)
                break;
            ++anOtherDimCoord[iDim];
        }
    }
    else
    {
        poDS->SetBand(nCurBand,
                      new GDALRasterBandFromArray(
                          poDS.get(), anOtherDimCoord,
                          aoBandParameterMetadataItems, aoBandImageryMetadata,
                          dfDelay, nStartTime, bHasWarned));
        ++nCurBand;
    }
    if (iDim > 0)
        goto lbl_return_to_caller;

    if (!array->GetFilename().empty())
    {
        poDS->SetPhysicalFilename(array->GetFilename().c_str());
        std::string osDerivedDatasetName(
            CPLSPrintf("AsClassicDataset(%d,%d) view of %s", int(iXDim),
                       int(iYDim), array->GetFullName().c_str()));
        if (!array->GetContext().empty())
        {
            osDerivedDatasetName += " with context ";
            osDerivedDatasetName += array->GetContext();
        }
        poDS->SetDerivedDatasetName(osDerivedDatasetName.c_str());
        poDS->TryLoadXML();

        for (const auto &[pszKey, pszValue] :
             cpl::IterateNameValue(static_cast<CSLConstList>(
                 poDS->GDALPamDataset::GetMetadata())))
        {
            poDS->m_oMDD.SetMetadataItem(pszKey, pszValue);
        }
    }

    return poDS.release();
}

/************************************************************************/
/*                          AsClassicDataset()                         */
/************************************************************************/

/** Return a view of this array as a "classic" GDALDataset (ie 2D)
 *
 * In the case of > 2D arrays, additional dimensions will be represented as
 * raster bands.
 *
 * The "reverse" method is GDALRasterBand::AsMDArray().
 *
 * This is the same as the C function GDALMDArrayAsClassicDataset().
 *
 * @param iXDim Index of the dimension that will be used as the X/width axis.
 * @param iYDim Index of the dimension that will be used as the Y/height axis.
 *              Ignored if the dimension count is 1.
 * @param poRootGroup (Added in GDAL 3.8) Root group. Used with the BAND_METADATA
 *                    and BAND_IMAGERY_METADATA option.
 * @param papszOptions (Added in GDAL 3.8) Null-terminated list of options, or
 *                     nullptr. Current supported options are:
 *                     <ul>
 *                     <li>BAND_METADATA: JSON serialized array defining which
 *                         arrays of the poRootGroup, indexed by non-X and Y
 *                         dimensions, should be mapped as band metadata items.
 *                         Each array item should be an object with the
 *                         following members:
 *                         - "array": full name of a band parameter array.
 *                           Such array must be a one
 *                           dimensional array, and its dimension must be one of
 *                           the dimensions of the array on which the method is
 *                           called (excluding the X and Y dimensons).
 *                         - "attribute": name relative to *this array or full
 *                           name of a single dimension numeric array whose size
 *                           must be one of the dimensions of *this array
 *                           (excluding the X and Y dimensons).
 *                           "array" and "attribute" are mutually exclusive.
 *                         - "item_name": band metadata item name
 *                         - "item_value": (optional) String, where "%[x][.y]f",
 *                           "%[x][.y]g" or "%s" printf-like formatting can be
 *                           used to format the corresponding value of the
 *                           parameter array. The percentage character should be
 *                           repeated: "%%"
 *                           "${attribute_name}" can also be used to include the
 *                           value of an attribute for "array" when set and if
 *                           not starting with '/'. Otherwise if starting with
 *                           '/', it is the full path to the attribute.
 *
 *                           If "item_value" is not provided, a default formatting
 *                           of the value will be applied.
 *
 *                         Example:
 *                         [
 *                            {
 *                              "array": "/sensor_band_parameters/wavelengths",
 *                              "item_name": "WAVELENGTH",
 *                              "item_value": "%.1f ${units}"
 *                            },
 *                            {
 *                              "array": "/sensor_band_parameters/fwhm",
 *                              "item_name": "FWHM"
 *                            },
 *                            {
 *                              "array": "/sensor_band_parameters/fwhm",
 *                              "item_name": "FWHM_UNIT",
 *                              "item_value": "${units}"
 *                            }
 *                         ]
 *
 *                         Example for Planet Labs Tanager radiance products:
 *                         [
 *                            {
 *                              "attribute": "center_wavelengths",
 *                              "item_name": "WAVELENGTH",
 *                              "item_value": "%.1f ${center_wavelengths_units}"
 *                            }
 *                         ]
 *
 *                     </li>
 *                     <li>BAND_IMAGERY_METADATA: (GDAL >= 3.11)
 *                         JSON serialized object defining which arrays of the
 *                         poRootGroup, indexed by non-X and Y dimensions,
 *                         should be mapped as band metadata items in the
 *                         band IMAGERY domain.
 *                         The object currently accepts 2 members:
 *                         - "CENTRAL_WAVELENGTH_UM": Central Wavelength in
 *                           micrometers.
 *                         - "FWHM_UM": Full-width half-maximum
 *                           in micrometers.
 *                         The value of each member should be an object with the
 *                         following members:
 *                         - "array": full name of a band parameter array.
 *                           Such array must be a one dimensional array, and its
 *                           dimension must be one of the dimensions of the
 *                           array on which the method is called
 *                           (excluding the X and Y dimensons).
 *                         - "attribute": name relative to *this array or full
 *                           name of a single dimension numeric array whose size
 *                           must be one of the dimensions of *this array
 *                           (excluding the X and Y dimensons).
 *                           "array" and "attribute" are mutually exclusive,
 *                           and one of them is required.
 *                         - "unit": (optional) unit of the values pointed in
 *                           the above array.
 *                           Can be a literal string or a string of the form
 *                           "${attribute_name}" to point to an attribute for
 *                           "array" when set and if no starting
 *                           with '/'. Otherwise if starting with '/', it is
 *                           the full path to the attribute.
 *                           Accepted values are "um", "micrometer"
 *                           (with UK vs US spelling, singular or plural), "nm",
 *                           "nanometer" (with UK vs US spelling, singular or
 *                           plural)
 *                           If not provided, micrometer is assumed.
 *
 *                         Example for EMIT datasets:
 *                         {
 *                            "CENTRAL_WAVELENGTH_UM": {
 *                                "array": "/sensor_band_parameters/wavelengths",
 *                                "unit": "${units}"
 *                            },
 *                            "FWHM_UM": {
 *                                "array": "/sensor_band_parameters/fwhm",
 *                                "unit": "${units}"
 *                            }
 *                         }
 *
 *                         Example for Planet Labs Tanager radiance products:
 *                         {
 *                            "CENTRAL_WAVELENGTH_UM": {
 *                              "attribute": "center_wavelengths",
 *                              "unit": "${center_wavelengths_units}"
 *                            },
 *                            "FWHM_UM": {
 *                              "attribute": "fwhm",
 *                              "unit": "${fwhm_units}"
 *                            }
 *                         }
 *
 *                     </li>
 *                     <li>LOAD_EXTRA_DIM_METADATA_DELAY: Maximum delay in
 *                         seconds allowed to set the DIM_{dimname}_VALUE band
 *                         metadata items from the indexing variable of the
 *                         dimensions.
 *                         Default value is 5. 'unlimited' can be used to mean
 *                         unlimited delay. Can also be defined globally with
 *                         the GDAL_LOAD_EXTRA_DIM_METADATA_DELAY configuration
 *                         option.</li>
 *                     </ul>
 * @return a new GDALDataset that must be freed with GDALClose(), or nullptr
 */
GDALDataset *
GDALMDArray::AsClassicDataset(size_t iXDim, size_t iYDim,
                              const std::shared_ptr<GDALGroup> &poRootGroup,
                              CSLConstList papszOptions) const
{
    auto self = std::dynamic_pointer_cast<GDALMDArray>(m_pSelf.lock());
    if (!self)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Driver implementation issue: m_pSelf not set !");
        return nullptr;
    }
    return GDALDatasetFromArray::Create(self, iXDim, iYDim, poRootGroup,
                                        papszOptions);
}

/************************************************************************/
/*                           GetStatistics()                            */
/************************************************************************/

/**
 * \brief Fetch statistics.
 *
 * Returns the minimum, maximum, mean and standard deviation of all
 * pixel values in this array.
 *
 * If bForce is FALSE results will only be returned if it can be done
 * quickly (i.e. without scanning the data).  If bForce is FALSE and
 * results cannot be returned efficiently, the method will return CE_Warning
 * but no warning will have been issued.   This is a non-standard use of
 * the CE_Warning return value to indicate "nothing done".
 *
 * When cached statistics are not available, and bForce is TRUE,
 * ComputeStatistics() is called.
 *
 * Note that file formats using PAM (Persistent Auxiliary Metadata) services
 * will generally cache statistics in the .aux.xml file allowing fast fetch
 * after the first request.
 *
 * Cached statistics can be cleared with GDALDataset::ClearStatistics().
 *
 * This method is the same as the C function GDALMDArrayGetStatistics().
 *
 * @param bApproxOK Currently ignored. In the future, should be set to true
 * if statistics on the whole array are wished, or to false if a subset of it
 * may be used.
 *
 * @param bForce If false statistics will only be returned if it can
 * be done without rescanning the image.
 *
 * @param pdfMin Location into which to load image minimum (may be NULL).
 *
 * @param pdfMax Location into which to load image maximum (may be NULL).-
 *
 * @param pdfMean Location into which to load image mean (may be NULL).
 *
 * @param pdfStdDev Location into which to load image standard deviation
 * (may be NULL).
 *
 * @param pnValidCount Number of samples whose value is different from the
 * nodata value. (may be NULL)
 *
 * @param pfnProgress a function to call to report progress, or NULL.
 *
 * @param pProgressData application data to pass to the progress function.
 *
 * @return CE_None on success, CE_Warning if no values returned,
 * CE_Failure if an error occurs.
 *
 * @since GDAL 3.2
 */

CPLErr GDALMDArray::GetStatistics(bool bApproxOK, bool bForce, double *pdfMin,
                                  double *pdfMax, double *pdfMean,
                                  double *pdfStdDev, GUInt64 *pnValidCount,
                                  GDALProgressFunc pfnProgress,
                                  void *pProgressData)
{
    if (!bForce)
        return CE_Warning;

    return ComputeStatistics(bApproxOK, pdfMin, pdfMax, pdfMean, pdfStdDev,
                             pnValidCount, pfnProgress, pProgressData, nullptr)
               ? CE_None
               : CE_Failure;
}

/************************************************************************/
/*                         ComputeStatistics()                          */
/************************************************************************/

/**
 * \brief Compute statistics.
 *
 * Returns the minimum, maximum, mean and standard deviation of all
 * pixel values in this array.
 *
 * Pixels taken into account in statistics are those whose mask value
 * (as determined by GetMask()) is non-zero.
 *
 * Once computed, the statistics will generally be "set" back on the
 * owing dataset.
 *
 * Cached statistics can be cleared with GDALDataset::ClearStatistics().
 *
 * This method is the same as the C functions GDALMDArrayComputeStatistics().
 * and GDALMDArrayComputeStatisticsEx().
 *
 * @param bApproxOK Currently ignored. In the future, should be set to true
 * if statistics on the whole array are wished, or to false if a subset of it
 * may be used.
 *
 * @param pdfMin Location into which to load image minimum (may be NULL).
 *
 * @param pdfMax Location into which to load image maximum (may be NULL).-
 *
 * @param pdfMean Location into which to load image mean (may be NULL).
 *
 * @param pdfStdDev Location into which to load image standard deviation
 * (may be NULL).
 *
 * @param pnValidCount Number of samples whose value is different from the
 * nodata value. (may be NULL)
 *
 * @param pfnProgress a function to call to report progress, or NULL.
 *
 * @param pProgressData application data to pass to the progress function.
 *
 * @param papszOptions NULL-terminated list of options, of NULL. Added in 3.8.
 *                     Options are driver specific. For now the netCDF and Zarr
 *                     drivers recognize UPDATE_METADATA=YES, whose effect is
 *                     to add or update the actual_range attribute with the
 *                     computed min/max, only if done on the full array, in non
 *                     approximate mode, and the dataset is opened in update
 *                     mode.
 *
 * @return true on success
 *
 * @since GDAL 3.2
 */

bool GDALMDArray::ComputeStatistics(bool bApproxOK, double *pdfMin,
                                    double *pdfMax, double *pdfMean,
                                    double *pdfStdDev, GUInt64 *pnValidCount,
                                    GDALProgressFunc pfnProgress,
                                    void *pProgressData,
                                    CSLConstList papszOptions)
{
    struct StatsPerChunkType
    {
        const GDALMDArray *array = nullptr;
        std::shared_ptr<GDALMDArray> poMask{};
        double dfMin = cpl::NumericLimits<double>::max();
        double dfMax = -cpl::NumericLimits<double>::max();
        double dfMean = 0.0;
        double dfM2 = 0.0;
        GUInt64 nValidCount = 0;
        std::vector<GByte> abyData{};
        std::vector<double> adfData{};
        std::vector<GByte> abyMaskData{};
        GDALProgressFunc pfnProgress = nullptr;
        void *pProgressData = nullptr;
    };

    const auto PerChunkFunc = [](GDALAbstractMDArray *,
                                 const GUInt64 *chunkArrayStartIdx,
                                 const size_t *chunkCount, GUInt64 iCurChunk,
                                 GUInt64 nChunkCount, void *pUserData)
    {
        StatsPerChunkType *data = static_cast<StatsPerChunkType *>(pUserData);
        const GDALMDArray *array = data->array;
        const GDALMDArray *poMask = data->poMask.get();
        const size_t nDims = array->GetDimensionCount();
        size_t nVals = 1;
        for (size_t i = 0; i < nDims; i++)
            nVals *= chunkCount[i];

        // Get mask
        data->abyMaskData.resize(nVals);
        if (!(poMask->Read(chunkArrayStartIdx, chunkCount, nullptr, nullptr,
                           poMask->GetDataType(), &data->abyMaskData[0])))
        {
            return false;
        }

        // Get data
        const auto &oType = array->GetDataType();
        if (oType.GetNumericDataType() == GDT_Float64)
        {
            data->adfData.resize(nVals);
            if (!array->Read(chunkArrayStartIdx, chunkCount, nullptr, nullptr,
                             oType, &data->adfData[0]))
            {
                return false;
            }
        }
        else
        {
            data->abyData.resize(nVals * oType.GetSize());
            if (!array->Read(chunkArrayStartIdx, chunkCount, nullptr, nullptr,
                             oType, &data->abyData[0]))
            {
                return false;
            }
            data->adfData.resize(nVals);
            GDALCopyWords64(&data->abyData[0], oType.GetNumericDataType(),
                            static_cast<int>(oType.GetSize()),
                            &data->adfData[0], GDT_Float64,
                            static_cast<int>(sizeof(double)),
                            static_cast<GPtrDiff_t>(nVals));
        }
        for (size_t i = 0; i < nVals; i++)
        {
            if (data->abyMaskData[i])
            {
                const double dfValue = data->adfData[i];
                data->dfMin = std::min(data->dfMin, dfValue);
                data->dfMax = std::max(data->dfMax, dfValue);
                data->nValidCount++;
                const double dfDelta = dfValue - data->dfMean;
                data->dfMean += dfDelta / data->nValidCount;
                data->dfM2 += dfDelta * (dfValue - data->dfMean);
            }
        }
        if (data->pfnProgress &&
            !data->pfnProgress(static_cast<double>(iCurChunk + 1) / nChunkCount,
                               "", data->pProgressData))
        {
            return false;
        }
        return true;
    };

    const auto &oType = GetDataType();
    if (oType.GetClass() != GEDTC_NUMERIC ||
        GDALDataTypeIsComplex(oType.GetNumericDataType()))
    {
        CPLError(
            CE_Failure, CPLE_NotSupported,
            "Statistics can only be computed on non-complex numeric data type");
        return false;
    }

    const size_t nDims = GetDimensionCount();
    std::vector<GUInt64> arrayStartIdx(nDims);
    std::vector<GUInt64> count(nDims);
    const auto &poDims = GetDimensions();
    for (size_t i = 0; i < nDims; i++)
    {
        count[i] = poDims[i]->GetSize();
    }
    const char *pszSwathSize = CPLGetConfigOption("GDAL_SWATH_SIZE", nullptr);
    const size_t nMaxChunkSize =
        pszSwathSize
            ? static_cast<size_t>(
                  std::min(GIntBig(std::numeric_limits<size_t>::max() / 2),
                           CPLAtoGIntBig(pszSwathSize)))
            : static_cast<size_t>(
                  std::min(GIntBig(std::numeric_limits<size_t>::max() / 2),
                           GDALGetCacheMax64() / 4));
    StatsPerChunkType sData;
    sData.array = this;
    sData.poMask = GetMask(nullptr);
    if (sData.poMask == nullptr)
    {
        return false;
    }
    sData.pfnProgress = pfnProgress;
    sData.pProgressData = pProgressData;
    if (!ProcessPerChunk(arrayStartIdx.data(), count.data(),
                         GetProcessingChunkSize(nMaxChunkSize).data(),
                         PerChunkFunc, &sData))
    {
        return false;
    }

    if (pdfMin)
        *pdfMin = sData.dfMin;

    if (pdfMax)
        *pdfMax = sData.dfMax;

    if (pdfMean)
        *pdfMean = sData.dfMean;

    const double dfStdDev =
        sData.nValidCount > 0 ? sqrt(sData.dfM2 / sData.nValidCount) : 0.0;
    if (pdfStdDev)
        *pdfStdDev = dfStdDev;

    if (pnValidCount)
        *pnValidCount = sData.nValidCount;

    SetStatistics(bApproxOK, sData.dfMin, sData.dfMax, sData.dfMean, dfStdDev,
                  sData.nValidCount, papszOptions);

    return true;
}

/************************************************************************/
/*                            SetStatistics()                           */
/************************************************************************/
//! @cond Doxygen_Suppress
bool GDALMDArray::SetStatistics(bool /* bApproxStats */, double /* dfMin */,
                                double /* dfMax */, double /* dfMean */,
                                double /* dfStdDev */,
                                GUInt64 /* nValidCount */,
                                CSLConstList /* papszOptions */)
{
    CPLDebug("GDAL", "Cannot save statistics on a non-PAM MDArray");
    return false;
}

//! @endcond

/************************************************************************/
/*                           ClearStatistics()                          */
/************************************************************************/

/**
 * \brief Clear statistics.
 *
 * @since GDAL 3.4
 */
void GDALMDArray::ClearStatistics()
{
}

/************************************************************************/
/*                      GetCoordinateVariables()                        */
/************************************************************************/

/**
 * \brief Return coordinate variables.
 *
 * Coordinate variables are an alternate way of indexing an array that can
 * be sometimes used. For example, an array collected through remote sensing
 * might be indexed by (scanline, pixel). But there can be
 * a longitude and latitude arrays alongside that are also both indexed by
 * (scanline, pixel), and are referenced from operational arrays for
 * reprojection purposes.
 *
 * For netCDF, this will return the arrays referenced by the "coordinates"
 * attribute.
 *
 * This method is the same as the C function
 * GDALMDArrayGetCoordinateVariables().
 *
 * @return a vector of arrays
 *
 * @since GDAL 3.4
 */

std::vector<std::shared_ptr<GDALMDArray>>
GDALMDArray::GetCoordinateVariables() const
{
    return {};
}

/************************************************************************/
/*                       ~GDALExtendedDataType()                        */
/************************************************************************/

GDALExtendedDataType::~GDALExtendedDataType() = default;

/************************************************************************/
/*                        GDALExtendedDataType()                        */
/************************************************************************/

GDALExtendedDataType::GDALExtendedDataType(size_t nMaxStringLength,
                                           GDALExtendedDataTypeSubType eSubType)
    : m_eClass(GEDTC_STRING), m_eSubType(eSubType), m_nSize(sizeof(char *)),
      m_nMaxStringLength(nMaxStringLength)
{
}

/************************************************************************/
/*                        GDALExtendedDataType()                        */
/************************************************************************/

GDALExtendedDataType::GDALExtendedDataType(GDALDataType eType)
    : m_eClass(GEDTC_NUMERIC), m_eNumericDT(eType),
      m_nSize(GDALGetDataTypeSizeBytes(eType))
{
}

/************************************************************************/
/*                        GDALExtendedDataType()                        */
/************************************************************************/

GDALExtendedDataType::GDALExtendedDataType(
    const std::string &osName, GDALDataType eBaseType,
    std::unique_ptr<GDALRasterAttributeTable> poRAT)
    : m_osName(osName), m_eClass(GEDTC_NUMERIC), m_eNumericDT(eBaseType),
      m_nSize(GDALGetDataTypeSizeBytes(eBaseType)), m_poRAT(std::move(poRAT))
{
}

/************************************************************************/
/*                        GDALExtendedDataType()                        */
/************************************************************************/

GDALExtendedDataType::GDALExtendedDataType(
    const std::string &osName, size_t nTotalSize,
    std::vector<std::unique_ptr<GDALEDTComponent>> &&components)
    : m_osName(osName), m_eClass(GEDTC_COMPOUND),
      m_aoComponents(std::move(components)), m_nSize(nTotalSize)
{
}

/************************************************************************/
/*                        GDALExtendedDataType()                        */
/************************************************************************/

/** Copy constructor. */
GDALExtendedDataType::GDALExtendedDataType(const GDALExtendedDataType &other)
    : m_osName(other.m_osName), m_eClass(other.m_eClass),
      m_eSubType(other.m_eSubType), m_eNumericDT(other.m_eNumericDT),
      m_nSize(other.m_nSize), m_nMaxStringLength(other.m_nMaxStringLength),
      m_poRAT(other.m_poRAT ? other.m_poRAT->Clone() : nullptr)
{
    if (m_eClass == GEDTC_COMPOUND)
    {
        for (const auto &elt : other.m_aoComponents)
        {
            m_aoComponents.emplace_back(new GDALEDTComponent(*elt));
        }
    }
}

/************************************************************************/
/*                            operator= ()                              */
/************************************************************************/

/** Copy assignment. */
GDALExtendedDataType &
GDALExtendedDataType::operator=(const GDALExtendedDataType &other)
{
    if (this != &other)
    {
        m_osName = other.m_osName;
        m_eClass = other.m_eClass;
        m_eSubType = other.m_eSubType;
        m_eNumericDT = other.m_eNumericDT;
        m_nSize = other.m_nSize;
        m_nMaxStringLength = other.m_nMaxStringLength;
        m_poRAT.reset(other.m_poRAT ? other.m_poRAT->Clone() : nullptr);
        m_aoComponents.clear();
        if (m_eClass == GEDTC_COMPOUND)
        {
            for (const auto &elt : other.m_aoComponents)
            {
                m_aoComponents.emplace_back(new GDALEDTComponent(*elt));
            }
        }
    }
    return *this;
}

/************************************************************************/
/*                            operator= ()                              */
/************************************************************************/

/** Move assignment. */
GDALExtendedDataType &
GDALExtendedDataType::operator=(GDALExtendedDataType &&other)
{
    m_osName = std::move(other.m_osName);
    m_eClass = other.m_eClass;
    m_eSubType = other.m_eSubType;
    m_eNumericDT = other.m_eNumericDT;
    m_nSize = other.m_nSize;
    m_nMaxStringLength = other.m_nMaxStringLength;
    m_aoComponents = std::move(other.m_aoComponents);
    m_poRAT = std::move(other.m_poRAT);
    other.m_eClass = GEDTC_NUMERIC;
    other.m_eNumericDT = GDT_Unknown;
    other.m_nSize = 0;
    other.m_nMaxStringLength = 0;
    return *this;
}

/************************************************************************/
/*                           Create()                                   */
/************************************************************************/

/** Return a new GDALExtendedDataType of class GEDTC_NUMERIC.
 *
 * This is the same as the C function GDALExtendedDataTypeCreate()
 *
 * @param eType Numeric data type. Must be different from GDT_Unknown and
 * GDT_TypeCount
 */
GDALExtendedDataType GDALExtendedDataType::Create(GDALDataType eType)
{
    return GDALExtendedDataType(eType);
}

/************************************************************************/
/*                           Create()                                   */
/************************************************************************/

/** Return a new GDALExtendedDataType from a raster attribute table.
 *
 * @param osName Type name
 * @param eBaseType Base integer data type.
 * @param poRAT Raster attribute table. Must not be NULL.
 * @since 3.12
 */
GDALExtendedDataType
GDALExtendedDataType::Create(const std::string &osName, GDALDataType eBaseType,
                             std::unique_ptr<GDALRasterAttributeTable> poRAT)
{
    return GDALExtendedDataType(osName, eBaseType, std::move(poRAT));
}

/************************************************************************/
/*                           Create()                                   */
/************************************************************************/

/** Return a new GDALExtendedDataType of class GEDTC_COMPOUND.
 *
 * This is the same as the C function GDALExtendedDataTypeCreateCompound()
 *
 * @param osName Type name.
 * @param nTotalSize Total size of the type in bytes.
 *                   Should be large enough to store all components.
 * @param components Components of the compound type.
 */
GDALExtendedDataType GDALExtendedDataType::Create(
    const std::string &osName, size_t nTotalSize,
    std::vector<std::unique_ptr<GDALEDTComponent>> &&components)
{
    size_t nLastOffset = 0;
    // Some arbitrary threshold to avoid potential integer overflows
    if (nTotalSize > static_cast<size_t>(std::numeric_limits<int>::max() / 2))
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Invalid offset/size");
        return GDALExtendedDataType(GDT_Unknown);
    }
    for (const auto &comp : components)
    {
        // Check alignment too ?
        if (comp->GetOffset() < nLastOffset)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Invalid offset/size");
            return GDALExtendedDataType(GDT_Unknown);
        }
        nLastOffset = comp->GetOffset() + comp->GetType().GetSize();
    }
    if (nTotalSize < nLastOffset)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Invalid offset/size");
        return GDALExtendedDataType(GDT_Unknown);
    }
    if (nTotalSize == 0 || components.empty())
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Empty compound not allowed");
        return GDALExtendedDataType(GDT_Unknown);
    }
    return GDALExtendedDataType(osName, nTotalSize, std::move(components));
}

/************************************************************************/
/*                           Create()                                   */
/************************************************************************/

/** Return a new GDALExtendedDataType of class GEDTC_STRING.
 *
 * This is the same as the C function GDALExtendedDataTypeCreateString().
 *
 * @param nMaxStringLength maximum length of a string in bytes. 0 if
 * unknown/unlimited
 * @param eSubType Subtype.
 */
GDALExtendedDataType
GDALExtendedDataType::CreateString(size_t nMaxStringLength,
                                   GDALExtendedDataTypeSubType eSubType)
{
    return GDALExtendedDataType(nMaxStringLength, eSubType);
}

/************************************************************************/
/*                           operator==()                               */
/************************************************************************/

/** Equality operator.
 *
 * This is the same as the C function GDALExtendedDataTypeEquals().
 */
bool GDALExtendedDataType::operator==(const GDALExtendedDataType &other) const
{
    if (m_eClass != other.m_eClass || m_eSubType != other.m_eSubType ||
        m_nSize != other.m_nSize || m_osName != other.m_osName)
    {
        return false;
    }
    if (m_eClass == GEDTC_NUMERIC)
    {
        return m_eNumericDT == other.m_eNumericDT;
    }
    if (m_eClass == GEDTC_STRING)
    {
        return true;
    }
    CPLAssert(m_eClass == GEDTC_COMPOUND);
    if (m_aoComponents.size() != other.m_aoComponents.size())
    {
        return false;
    }
    for (size_t i = 0; i < m_aoComponents.size(); i++)
    {
        if (!(*m_aoComponents[i] == *other.m_aoComponents[i]))
        {
            return false;
        }
    }
    return true;
}

/************************************************************************/
/*                        CanConvertTo()                                */
/************************************************************************/

/** Return whether this data type can be converted to the other one.
 *
 * This is the same as the C function GDALExtendedDataTypeCanConvertTo().
 *
 * @param other Target data type for the conversion being considered.
 */
bool GDALExtendedDataType::CanConvertTo(const GDALExtendedDataType &other) const
{
    if (m_eClass == GEDTC_NUMERIC)
    {
        if (m_eNumericDT == GDT_Unknown)
            return false;
        if (other.m_eClass == GEDTC_NUMERIC &&
            other.m_eNumericDT == GDT_Unknown)
            return false;
        return other.m_eClass == GEDTC_NUMERIC ||
               other.m_eClass == GEDTC_STRING;
    }
    if (m_eClass == GEDTC_STRING)
    {
        return other.m_eClass == m_eClass;
    }
    CPLAssert(m_eClass == GEDTC_COMPOUND);
    if (other.m_eClass != GEDTC_COMPOUND)
        return false;
    std::map<std::string, const std::unique_ptr<GDALEDTComponent> *>
        srcComponents;
    for (const auto &srcComp : m_aoComponents)
    {
        srcComponents[srcComp->GetName()] = &srcComp;
    }
    for (const auto &dstComp : other.m_aoComponents)
    {
        auto oIter = srcComponents.find(dstComp->GetName());
        if (oIter == srcComponents.end())
            return false;
        if (!(*(oIter->second))->GetType().CanConvertTo(dstComp->GetType()))
            return false;
    }
    return true;
}

/************************************************************************/
/*                     NeedsFreeDynamicMemory()                         */
/************************************************************************/

/** Return whether the data type holds dynamically allocated memory, that
 * needs to be freed with FreeDynamicMemory().
 *
 */
bool GDALExtendedDataType::NeedsFreeDynamicMemory() const
{
    switch (m_eClass)
    {
        case GEDTC_STRING:
            return true;

        case GEDTC_NUMERIC:
            return false;

        case GEDTC_COMPOUND:
        {
            for (const auto &comp : m_aoComponents)
            {
                if (comp->GetType().NeedsFreeDynamicMemory())
                    return true;
            }
        }
    }
    return false;
}

/************************************************************************/
/*                        FreeDynamicMemory()                           */
/************************************************************************/

/** Release the dynamic memory (strings typically) from a raw value.
 *
 * This is the same as the C function GDALExtendedDataTypeFreeDynamicMemory().
 *
 * @param pBuffer Raw buffer of a single element of an attribute or array value.
 */
void GDALExtendedDataType::FreeDynamicMemory(void *pBuffer) const
{
    switch (m_eClass)
    {
        case GEDTC_STRING:
        {
            char *pszStr;
            memcpy(&pszStr, pBuffer, sizeof(char *));
            if (pszStr)
            {
                VSIFree(pszStr);
            }
            break;
        }

        case GEDTC_NUMERIC:
        {
            break;
        }

        case GEDTC_COMPOUND:
        {
            GByte *pabyBuffer = static_cast<GByte *>(pBuffer);
            for (const auto &comp : m_aoComponents)
            {
                comp->GetType().FreeDynamicMemory(pabyBuffer +
                                                  comp->GetOffset());
            }
            break;
        }
    }
}

/************************************************************************/
/*                      ~GDALEDTComponent()                             */
/************************************************************************/

GDALEDTComponent::~GDALEDTComponent() = default;

/************************************************************************/
/*                      GDALEDTComponent()                              */
/************************************************************************/

/** constructor of a GDALEDTComponent
 *
 * This is the same as the C function GDALEDTComponendCreate()
 *
 * @param name Component name
 * @param offset Offset in byte of the component in the compound data type.
 *               In case of nesting of compound data type, this should be
 *               the offset to the immediate belonging data type, not to the
 *               higher level one.
 * @param type   Component data type.
 */
GDALEDTComponent::GDALEDTComponent(const std::string &name, size_t offset,
                                   const GDALExtendedDataType &type)
    : m_osName(name), m_nOffset(offset), m_oType(type)
{
}

/************************************************************************/
/*                      GDALEDTComponent()                              */
/************************************************************************/

/** Copy constructor. */
GDALEDTComponent::GDALEDTComponent(const GDALEDTComponent &) = default;

/************************************************************************/
/*                           operator==()                               */
/************************************************************************/

/** Equality operator.
 */
bool GDALEDTComponent::operator==(const GDALEDTComponent &other) const
{
    return m_osName == other.m_osName && m_nOffset == other.m_nOffset &&
           m_oType == other.m_oType;
}

/************************************************************************/
/*                        ~GDALDimension()                              */
/************************************************************************/

GDALDimension::~GDALDimension() = default;

/************************************************************************/
/*                         GDALDimension()                              */
/************************************************************************/

//! @cond Doxygen_Suppress
/** Constructor.
 *
 * @param osParentName Parent name
 * @param osName name
 * @param osType type. See GetType().
 * @param osDirection direction. See GetDirection().
 * @param nSize size.
 */
GDALDimension::GDALDimension(const std::string &osParentName,
                             const std::string &osName,
                             const std::string &osType,
                             const std::string &osDirection, GUInt64 nSize)
    : m_osName(osName),
      m_osFullName(
          !osParentName.empty()
              ? ((osParentName == "/" ? "/" : osParentName + "/") + osName)
              : osName),
      m_osType(osType), m_osDirection(osDirection), m_nSize(nSize)
{
}

//! @endcond

/************************************************************************/
/*                         GetIndexingVariable()                        */
/************************************************************************/

/** Return the variable that is used to index the dimension (if there is one).
 *
 * This is the array, typically one-dimensional, describing the values taken
 * by the dimension.
 */
std::shared_ptr<GDALMDArray> GDALDimension::GetIndexingVariable() const
{
    return nullptr;
}

/************************************************************************/
/*                         SetIndexingVariable()                        */
/************************************************************************/

/** Set the variable that is used to index the dimension.
 *
 * This is the array, typically one-dimensional, describing the values taken
 * by the dimension.
 *
 * Optionally implemented by drivers.
 *
 * Drivers known to implement it: MEM.
 *
 * @param poArray Variable to use to index the dimension.
 * @return true in case of success.
 */
bool GDALDimension::SetIndexingVariable(
    CPL_UNUSED std::shared_ptr<GDALMDArray> poArray)
{
    CPLError(CE_Failure, CPLE_NotSupported,
             "SetIndexingVariable() not implemented");
    return false;
}

/************************************************************************/
/*                            Rename()                                  */
/************************************************************************/

/** Rename the dimension.
 *
 * This is not implemented by all drivers.
 *
 * Drivers known to implement it: MEM, netCDF, ZARR.
 *
 * This is the same as the C function GDALDimensionRename().
 *
 * @param osNewName New name.
 *
 * @return true in case of success
 * @since GDAL 3.8
 */
bool GDALDimension::Rename(CPL_UNUSED const std::string &osNewName)
{
    CPLError(CE_Failure, CPLE_NotSupported, "Rename() not implemented");
    return false;
}

/************************************************************************/
/*                         BaseRename()                                 */
/************************************************************************/

//! @cond Doxygen_Suppress
void GDALDimension::BaseRename(const std::string &osNewName)
{
    m_osFullName.resize(m_osFullName.size() - m_osName.size());
    m_osFullName += osNewName;
    m_osName = osNewName;
}

//! @endcond

//! @cond Doxygen_Suppress
/************************************************************************/
/*                          ParentRenamed()                             */
/************************************************************************/

void GDALDimension::ParentRenamed(const std::string &osNewParentFullName)
{
    m_osFullName = osNewParentFullName;
    m_osFullName += "/";
    m_osFullName += m_osName;
}

//! @endcond

//! @cond Doxygen_Suppress
/************************************************************************/
/*                          ParentDeleted()                             */
/************************************************************************/

void GDALDimension::ParentDeleted()
{
}

//! @endcond

/************************************************************************/
/************************************************************************/
/************************************************************************/
/*                              C API                                   */
/************************************************************************/
/************************************************************************/
/************************************************************************/

/************************************************************************/
/*                      GDALExtendedDataTypeCreate()                    */
/************************************************************************/

/** Return a new GDALExtendedDataType of class GEDTC_NUMERIC.
 *
 * This is the same as the C++ method GDALExtendedDataType::Create()
 *
 * The returned handle should be freed with GDALExtendedDataTypeRelease().
 *
 * @param eType Numeric data type. Must be different from GDT_Unknown and
 * GDT_TypeCount
 *
 * @return a new GDALExtendedDataTypeH handle, or nullptr.
 */
GDALExtendedDataTypeH GDALExtendedDataTypeCreate(GDALDataType eType)
{
    if (CPL_UNLIKELY(eType == GDT_Unknown || eType == GDT_TypeCount))
    {
        CPLError(CE_Failure, CPLE_IllegalArg,
                 "Illegal GDT_Unknown/GDT_TypeCount argument");
        return nullptr;
    }
    return new GDALExtendedDataTypeHS(
        new GDALExtendedDataType(GDALExtendedDataType::Create(eType)));
}

/************************************************************************/
/*                    GDALExtendedDataTypeCreateString()                */
/************************************************************************/

/** Return a new GDALExtendedDataType of class GEDTC_STRING.
 *
 * This is the same as the C++ method GDALExtendedDataType::CreateString()
 *
 * The returned handle should be freed with GDALExtendedDataTypeRelease().
 *
 * @return a new GDALExtendedDataTypeH handle, or nullptr.
 */
GDALExtendedDataTypeH GDALExtendedDataTypeCreateString(size_t nMaxStringLength)
{
    return new GDALExtendedDataTypeHS(new GDALExtendedDataType(
        GDALExtendedDataType::CreateString(nMaxStringLength)));
}

/************************************************************************/
/*                   GDALExtendedDataTypeCreateStringEx()               */
/************************************************************************/

/** Return a new GDALExtendedDataType of class GEDTC_STRING.
 *
 * This is the same as the C++ method GDALExtendedDataType::CreateString()
 *
 * The returned handle should be freed with GDALExtendedDataTypeRelease().
 *
 * @return a new GDALExtendedDataTypeH handle, or nullptr.
 * @since GDAL 3.4
 */
GDALExtendedDataTypeH
GDALExtendedDataTypeCreateStringEx(size_t nMaxStringLength,
                                   GDALExtendedDataTypeSubType eSubType)
{
    return new GDALExtendedDataTypeHS(new GDALExtendedDataType(
        GDALExtendedDataType::CreateString(nMaxStringLength, eSubType)));
}

/************************************************************************/
/*                   GDALExtendedDataTypeCreateCompound()               */
/************************************************************************/

/** Return a new GDALExtendedDataType of class GEDTC_COMPOUND.
 *
 * This is the same as the C++ method GDALExtendedDataType::Create(const
 * std::string&, size_t, std::vector<std::unique_ptr<GDALEDTComponent>>&&)
 *
 * The returned handle should be freed with GDALExtendedDataTypeRelease().
 *
 * @param pszName Type name.
 * @param nTotalSize Total size of the type in bytes.
 *                   Should be large enough to store all components.
 * @param nComponents Number of components in comps array.
 * @param comps Components.
 * @return a new GDALExtendedDataTypeH handle, or nullptr.
 */
GDALExtendedDataTypeH
GDALExtendedDataTypeCreateCompound(const char *pszName, size_t nTotalSize,
                                   size_t nComponents,
                                   const GDALEDTComponentH *comps)
{
    std::vector<std::unique_ptr<GDALEDTComponent>> compsCpp;
    for (size_t i = 0; i < nComponents; i++)
    {
        compsCpp.emplace_back(std::unique_ptr<GDALEDTComponent>(
            new GDALEDTComponent(*(comps[i]->m_poImpl.get()))));
    }
    auto dt = GDALExtendedDataType::Create(pszName ? pszName : "", nTotalSize,
                                           std::move(compsCpp));
    if (dt.GetClass() != GEDTC_COMPOUND)
        return nullptr;
    return new GDALExtendedDataTypeHS(new GDALExtendedDataType(dt));
}

/************************************************************************/
/*                     GDALExtendedDataTypeRelease()                    */
/************************************************************************/

/** Release the GDAL in-memory object associated with a GDALExtendedDataTypeH.
 *
 * Note: when applied on a object coming from a driver, this does not
 * destroy the object in the file, database, etc...
 */
void GDALExtendedDataTypeRelease(GDALExtendedDataTypeH hEDT)
{
    delete hEDT;
}

/************************************************************************/
/*                     GDALExtendedDataTypeGetName()                    */
/************************************************************************/

/** Return type name.
 *
 * This is the same as the C++ method GDALExtendedDataType::GetName()
 */
const char *GDALExtendedDataTypeGetName(GDALExtendedDataTypeH hEDT)
{
    VALIDATE_POINTER1(hEDT, __func__, "");
    return hEDT->m_poImpl->GetName().c_str();
}

/************************************************************************/
/*                     GDALExtendedDataTypeGetClass()                    */
/************************************************************************/

/** Return type class.
 *
 * This is the same as the C++ method GDALExtendedDataType::GetClass()
 */
GDALExtendedDataTypeClass
GDALExtendedDataTypeGetClass(GDALExtendedDataTypeH hEDT)
{
    VALIDATE_POINTER1(hEDT, __func__, GEDTC_NUMERIC);
    return hEDT->m_poImpl->GetClass();
}

/************************************************************************/
/*               GDALExtendedDataTypeGetNumericDataType()               */
/************************************************************************/

/** Return numeric data type (only valid when GetClass() == GEDTC_NUMERIC)
 *
 * This is the same as the C++ method GDALExtendedDataType::GetNumericDataType()
 */
GDALDataType GDALExtendedDataTypeGetNumericDataType(GDALExtendedDataTypeH hEDT)
{
    VALIDATE_POINTER1(hEDT, __func__, GDT_Unknown);
    return hEDT->m_poImpl->GetNumericDataType();
}

/************************************************************************/
/*                   GDALExtendedDataTypeGetSize()                      */
/************************************************************************/

/** Return data type size in bytes.
 *
 * This is the same as the C++ method GDALExtendedDataType::GetSize()
 */
size_t GDALExtendedDataTypeGetSize(GDALExtendedDataTypeH hEDT)
{
    VALIDATE_POINTER1(hEDT, __func__, 0);
    return hEDT->m_poImpl->GetSize();
}

/************************************************************************/
/*              GDALExtendedDataTypeGetMaxStringLength()                */
/************************************************************************/

/** Return the maximum length of a string in bytes.
 *
 * 0 indicates unknown/unlimited string.
 *
 * This is the same as the C++ method GDALExtendedDataType::GetMaxStringLength()
 */
size_t GDALExtendedDataTypeGetMaxStringLength(GDALExtendedDataTypeH hEDT)
{
    VALIDATE_POINTER1(hEDT, __func__, 0);
    return hEDT->m_poImpl->GetMaxStringLength();
}

/************************************************************************/
/*                    GDALExtendedDataTypeCanConvertTo()                */
/************************************************************************/

/** Return whether this data type can be converted to the other one.
 *
 * This is the same as the C function GDALExtendedDataType::CanConvertTo()
 *
 * @param hSourceEDT Source data type for the conversion being considered.
 * @param hTargetEDT Target data type for the conversion being considered.
 * @return TRUE if hSourceEDT can be convert to hTargetEDT. FALSE otherwise.
 */
int GDALExtendedDataTypeCanConvertTo(GDALExtendedDataTypeH hSourceEDT,
                                     GDALExtendedDataTypeH hTargetEDT)
{
    VALIDATE_POINTER1(hSourceEDT, __func__, FALSE);
    VALIDATE_POINTER1(hTargetEDT, __func__, FALSE);
    return hSourceEDT->m_poImpl->CanConvertTo(*(hTargetEDT->m_poImpl));
}

/************************************************************************/
/*                        GDALExtendedDataTypeEquals()                  */
/************************************************************************/

/** Return whether this data type is equal to another one.
 *
 * This is the same as the C++ method GDALExtendedDataType::operator==()
 *
 * @param hFirstEDT First data type.
 * @param hSecondEDT Second data type.
 * @return TRUE if they are equal. FALSE otherwise.
 */
int GDALExtendedDataTypeEquals(GDALExtendedDataTypeH hFirstEDT,
                               GDALExtendedDataTypeH hSecondEDT)
{
    VALIDATE_POINTER1(hFirstEDT, __func__, FALSE);
    VALIDATE_POINTER1(hSecondEDT, __func__, FALSE);
    return *(hFirstEDT->m_poImpl) == *(hSecondEDT->m_poImpl);
}

/************************************************************************/
/*                    GDALExtendedDataTypeGetSubType()                  */
/************************************************************************/

/** Return the subtype of a type.
 *
 * This is the same as the C++ method GDALExtendedDataType::GetSubType()
 *
 * @param hEDT Data type.
 * @return subtype.
 * @since 3.4
 */
GDALExtendedDataTypeSubType
GDALExtendedDataTypeGetSubType(GDALExtendedDataTypeH hEDT)
{
    VALIDATE_POINTER1(hEDT, __func__, GEDTST_NONE);
    return hEDT->m_poImpl->GetSubType();
}

/************************************************************************/
/*                      GDALExtendedDataTypeGetRAT()                    */
/************************************************************************/

/** Return associated raster attribute table, when there is one.
 *
 * * For the netCDF driver, the RAT will capture enumerated types, with
 * a "value" column with an integer value and a "name" column with the
 * associated name.
 * This is the same as the C++ method GDALExtendedDataType::GetRAT()
 *
 * @param hEDT Data type.
 * @return raster attribute (owned by GDALExtendedDataTypeH), or NULL
 * @since 3.12
 */
GDALRasterAttributeTableH GDALExtendedDataTypeGetRAT(GDALExtendedDataTypeH hEDT)
{
    VALIDATE_POINTER1(hEDT, __func__, nullptr);
    return GDALRasterAttributeTable::ToHandle(
        const_cast<GDALRasterAttributeTable *>(hEDT->m_poImpl->GetRAT()));
}

/************************************************************************/
/*                     GDALExtendedDataTypeGetComponents()              */
/************************************************************************/

/** Return the components of the data type (only valid when GetClass() ==
 * GEDTC_COMPOUND)
 *
 * The returned array and its content must be freed with
 * GDALExtendedDataTypeFreeComponents(). If only the array itself needs to be
 * freed, CPLFree() should be called (and GDALExtendedDataTypeRelease() on
 * individual array members).
 *
 * This is the same as the C++ method GDALExtendedDataType::GetComponents()
 *
 * @param hEDT Data type
 * @param pnCount Pointer to the number of values returned. Must NOT be NULL.
 * @return an array of *pnCount components.
 */
GDALEDTComponentH *GDALExtendedDataTypeGetComponents(GDALExtendedDataTypeH hEDT,
                                                     size_t *pnCount)
{
    VALIDATE_POINTER1(hEDT, __func__, nullptr);
    VALIDATE_POINTER1(pnCount, __func__, nullptr);
    const auto &components = hEDT->m_poImpl->GetComponents();
    auto ret = static_cast<GDALEDTComponentH *>(
        CPLMalloc(sizeof(GDALEDTComponentH) * components.size()));
    for (size_t i = 0; i < components.size(); i++)
    {
        ret[i] = new GDALEDTComponentHS(*components[i].get());
    }
    *pnCount = components.size();
    return ret;
}

/************************************************************************/
/*                     GDALExtendedDataTypeFreeComponents()             */
/************************************************************************/

/** Free the return of GDALExtendedDataTypeGetComponents().
 *
 * @param components return value of GDALExtendedDataTypeGetComponents()
 * @param nCount *pnCount value returned by GDALExtendedDataTypeGetComponents()
 */
void GDALExtendedDataTypeFreeComponents(GDALEDTComponentH *components,
                                        size_t nCount)
{
    for (size_t i = 0; i < nCount; i++)
    {
        delete components[i];
    }
    CPLFree(components);
}

/************************************************************************/
/*                         GDALEDTComponentCreate()                     */
/************************************************************************/

/** Create a new GDALEDTComponent.
 *
 * The returned value must be freed with GDALEDTComponentRelease().
 *
 * This is the same as the C++ constructor GDALEDTComponent::GDALEDTComponent().
 */
GDALEDTComponentH GDALEDTComponentCreate(const char *pszName, size_t nOffset,
                                         GDALExtendedDataTypeH hType)
{
    VALIDATE_POINTER1(pszName, __func__, nullptr);
    VALIDATE_POINTER1(hType, __func__, nullptr);
    return new GDALEDTComponentHS(
        GDALEDTComponent(pszName, nOffset, *(hType->m_poImpl.get())));
}

/************************************************************************/
/*                         GDALEDTComponentRelease()                    */
/************************************************************************/

/** Release the GDAL in-memory object associated with a GDALEDTComponentH.
 *
 * Note: when applied on a object coming from a driver, this does not
 * destroy the object in the file, database, etc...
 */
void GDALEDTComponentRelease(GDALEDTComponentH hComp)
{
    delete hComp;
}

/************************************************************************/
/*                         GDALEDTComponentGetName()                    */
/************************************************************************/

/** Return the name.
 *
 * The returned pointer is valid until hComp is released.
 *
 * This is the same as the C++ method GDALEDTComponent::GetName().
 */
const char *GDALEDTComponentGetName(GDALEDTComponentH hComp)
{
    VALIDATE_POINTER1(hComp, __func__, nullptr);
    return hComp->m_poImpl->GetName().c_str();
}

/************************************************************************/
/*                       GDALEDTComponentGetOffset()                    */
/************************************************************************/

/** Return the offset (in bytes) of the component in the compound data type.
 *
 * This is the same as the C++ method GDALEDTComponent::GetOffset().
 */
size_t GDALEDTComponentGetOffset(GDALEDTComponentH hComp)
{
    VALIDATE_POINTER1(hComp, __func__, 0);
    return hComp->m_poImpl->GetOffset();
}

/************************************************************************/
/*                       GDALEDTComponentGetType()                      */
/************************************************************************/

/** Return the data type of the component.
 *
 * This is the same as the C++ method GDALEDTComponent::GetType().
 */
GDALExtendedDataTypeH GDALEDTComponentGetType(GDALEDTComponentH hComp)
{
    VALIDATE_POINTER1(hComp, __func__, nullptr);
    return new GDALExtendedDataTypeHS(
        new GDALExtendedDataType(hComp->m_poImpl->GetType()));
}

/************************************************************************/
/*                           GDALGroupRelease()                         */
/************************************************************************/

/** Release the GDAL in-memory object associated with a GDALGroupH.
 *
 * Note: when applied on a object coming from a driver, this does not
 * destroy the object in the file, database, etc...
 */
void GDALGroupRelease(GDALGroupH hGroup)
{
    delete hGroup;
}

/************************************************************************/
/*                           GDALGroupGetName()                         */
/************************************************************************/

/** Return the name of the group.
 *
 * The returned pointer is valid until hGroup is released.
 *
 * This is the same as the C++ method GDALGroup::GetName().
 */
const char *GDALGroupGetName(GDALGroupH hGroup)
{
    VALIDATE_POINTER1(hGroup, __func__, nullptr);
    return hGroup->m_poImpl->GetName().c_str();
}

/************************************************************************/
/*                         GDALGroupGetFullName()                       */
/************************************************************************/

/** Return the full name of the group.
 *
 * The returned pointer is valid until hGroup is released.
 *
 * This is the same as the C++ method GDALGroup::GetFullName().
 */
const char *GDALGroupGetFullName(GDALGroupH hGroup)
{
    VALIDATE_POINTER1(hGroup, __func__, nullptr);
    return hGroup->m_poImpl->GetFullName().c_str();
}

/************************************************************************/
/*                          GDALGroupGetMDArrayNames()                  */
/************************************************************************/

/** Return the list of multidimensional array names contained in this group.
 *
 * This is the same as the C++ method GDALGroup::GetGroupNames().
 *
 * @return the array names, to be freed with CSLDestroy()
 */
char **GDALGroupGetMDArrayNames(GDALGroupH hGroup, CSLConstList papszOptions)
{
    VALIDATE_POINTER1(hGroup, __func__, nullptr);
    auto names = hGroup->m_poImpl->GetMDArrayNames(papszOptions);
    CPLStringList res;
    for (const auto &name : names)
    {
        res.AddString(name.c_str());
    }
    return res.StealList();
}

/************************************************************************/
/*                  GDALGroupGetMDArrayFullNamesRecursive()             */
/************************************************************************/

/** Return the list of multidimensional array full names contained in this
 * group and its subgroups.
 *
 * This is the same as the C++ method GDALGroup::GetMDArrayFullNamesRecursive().
 *
 * @return the array names, to be freed with CSLDestroy()
 *
 * @since 3.11
 */
char **GDALGroupGetMDArrayFullNamesRecursive(GDALGroupH hGroup,
                                             CSLConstList papszGroupOptions,
                                             CSLConstList papszArrayOptions)
{
    VALIDATE_POINTER1(hGroup, __func__, nullptr);
    auto names = hGroup->m_poImpl->GetMDArrayFullNamesRecursive(
        papszGroupOptions, papszArrayOptions);
    CPLStringList res;
    for (const auto &name : names)
    {
        res.AddString(name.c_str());
    }
    return res.StealList();
}

/************************************************************************/
/*                          GDALGroupOpenMDArray()                      */
/************************************************************************/

/** Open and return a multidimensional array.
 *
 * This is the same as the C++ method GDALGroup::OpenMDArray().
 *
 * @return the array, to be freed with GDALMDArrayRelease(), or nullptr.
 */
GDALMDArrayH GDALGroupOpenMDArray(GDALGroupH hGroup, const char *pszMDArrayName,
                                  CSLConstList papszOptions)
{
    VALIDATE_POINTER1(hGroup, __func__, nullptr);
    VALIDATE_POINTER1(pszMDArrayName, __func__, nullptr);
    auto array = hGroup->m_poImpl->OpenMDArray(std::string(pszMDArrayName),
                                               papszOptions);
    if (!array)
        return nullptr;
    return new GDALMDArrayHS(array);
}

/************************************************************************/
/*                  GDALGroupOpenMDArrayFromFullname()                  */
/************************************************************************/

/** Open and return a multidimensional array from its fully qualified name.
 *
 * This is the same as the C++ method GDALGroup::OpenMDArrayFromFullname().
 *
 * @return the array, to be freed with GDALMDArrayRelease(), or nullptr.
 *
 * @since GDAL 3.2
 */
GDALMDArrayH GDALGroupOpenMDArrayFromFullname(GDALGroupH hGroup,
                                              const char *pszFullname,
                                              CSLConstList papszOptions)
{
    VALIDATE_POINTER1(hGroup, __func__, nullptr);
    VALIDATE_POINTER1(pszFullname, __func__, nullptr);
    auto array = hGroup->m_poImpl->OpenMDArrayFromFullname(
        std::string(pszFullname), papszOptions);
    if (!array)
        return nullptr;
    return new GDALMDArrayHS(array);
}

/************************************************************************/
/*                      GDALGroupResolveMDArray()                       */
/************************************************************************/

/** Locate an array in a group and its subgroups by name.
 *
 * See GDALGroup::ResolveMDArray() for description of the behavior.
 * @since GDAL 3.2
 */
GDALMDArrayH GDALGroupResolveMDArray(GDALGroupH hGroup, const char *pszName,
                                     const char *pszStartingPoint,
                                     CSLConstList papszOptions)
{
    VALIDATE_POINTER1(hGroup, __func__, nullptr);
    VALIDATE_POINTER1(pszName, __func__, nullptr);
    VALIDATE_POINTER1(pszStartingPoint, __func__, nullptr);
    auto array = hGroup->m_poImpl->ResolveMDArray(
        std::string(pszName), std::string(pszStartingPoint), papszOptions);
    if (!array)
        return nullptr;
    return new GDALMDArrayHS(array);
}

/************************************************************************/
/*                        GDALGroupGetGroupNames()                      */
/************************************************************************/

/** Return the list of sub-groups contained in this group.
 *
 * This is the same as the C++ method GDALGroup::GetGroupNames().
 *
 * @return the group names, to be freed with CSLDestroy()
 */
char **GDALGroupGetGroupNames(GDALGroupH hGroup, CSLConstList papszOptions)
{
    VALIDATE_POINTER1(hGroup, __func__, nullptr);
    auto names = hGroup->m_poImpl->GetGroupNames(papszOptions);
    CPLStringList res;
    for (const auto &name : names)
    {
        res.AddString(name.c_str());
    }
    return res.StealList();
}

/************************************************************************/
/*                           GDALGroupOpenGroup()                       */
/************************************************************************/

/** Open and return a sub-group.
 *
 * This is the same as the C++ method GDALGroup::OpenGroup().
 *
 * @return the sub-group, to be freed with GDALGroupRelease(), or nullptr.
 */
GDALGroupH GDALGroupOpenGroup(GDALGroupH hGroup, const char *pszSubGroupName,
                              CSLConstList papszOptions)
{
    VALIDATE_POINTER1(hGroup, __func__, nullptr);
    VALIDATE_POINTER1(pszSubGroupName, __func__, nullptr);
    auto subGroup =
        hGroup->m_poImpl->OpenGroup(std::string(pszSubGroupName), papszOptions);
    if (!subGroup)
        return nullptr;
    return new GDALGroupHS(subGroup);
}

/************************************************************************/
/*                   GDALGroupGetVectorLayerNames()                     */
/************************************************************************/

/** Return the list of layer names contained in this group.
 *
 * This is the same as the C++ method GDALGroup::GetVectorLayerNames().
 *
 * @return the group names, to be freed with CSLDestroy()
 * @since 3.4
 */
char **GDALGroupGetVectorLayerNames(GDALGroupH hGroup,
                                    CSLConstList papszOptions)
{
    VALIDATE_POINTER1(hGroup, __func__, nullptr);
    auto names = hGroup->m_poImpl->GetVectorLayerNames(papszOptions);
    CPLStringList res;
    for (const auto &name : names)
    {
        res.AddString(name.c_str());
    }
    return res.StealList();
}

/************************************************************************/
/*                      GDALGroupOpenVectorLayer()                      */
/************************************************************************/

/** Open and return a vector layer.
 *
 * This is the same as the C++ method GDALGroup::OpenVectorLayer().
 *
 * Note that the vector layer is owned by its parent GDALDatasetH, and thus
 * the returned handled if only valid while the parent GDALDatasetH is kept
 * opened.
 *
 * @return the vector layer, or nullptr.
 * @since 3.4
 */
OGRLayerH GDALGroupOpenVectorLayer(GDALGroupH hGroup,
                                   const char *pszVectorLayerName,
                                   CSLConstList papszOptions)
{
    VALIDATE_POINTER1(hGroup, __func__, nullptr);
    VALIDATE_POINTER1(pszVectorLayerName, __func__, nullptr);
    return OGRLayer::ToHandle(hGroup->m_poImpl->OpenVectorLayer(
        std::string(pszVectorLayerName), papszOptions));
}

/************************************************************************/
/*                       GDALGroupOpenMDArrayFromFullname()             */
/************************************************************************/

/** Open and return a sub-group from its fully qualified name.
 *
 * This is the same as the C++ method GDALGroup::OpenGroupFromFullname().
 *
 * @return the sub-group, to be freed with GDALGroupRelease(), or nullptr.
 *
 * @since GDAL 3.2
 */
GDALGroupH GDALGroupOpenGroupFromFullname(GDALGroupH hGroup,
                                          const char *pszFullname,
                                          CSLConstList papszOptions)
{
    VALIDATE_POINTER1(hGroup, __func__, nullptr);
    VALIDATE_POINTER1(pszFullname, __func__, nullptr);
    auto subGroup = hGroup->m_poImpl->OpenGroupFromFullname(
        std::string(pszFullname), papszOptions);
    if (!subGroup)
        return nullptr;
    return new GDALGroupHS(subGroup);
}

/************************************************************************/
/*                         GDALGroupGetDimensions()                     */
/************************************************************************/

/** Return the list of dimensions contained in this group and used by its
 * arrays.
 *
 * The returned array must be freed with GDALReleaseDimensions().  If only the
 * array itself needs to be freed, CPLFree() should be called (and
 * GDALDimensionRelease() on individual array members).
 *
 * This is the same as the C++ method GDALGroup::GetDimensions().
 *
 * @param hGroup Group.
 * @param pnCount Pointer to the number of values returned. Must NOT be NULL.
 * @param papszOptions Driver specific options determining how dimensions
 * should be retrieved. Pass nullptr for default behavior.
 *
 * @return an array of *pnCount dimensions.
 */
GDALDimensionH *GDALGroupGetDimensions(GDALGroupH hGroup, size_t *pnCount,
                                       CSLConstList papszOptions)
{
    VALIDATE_POINTER1(hGroup, __func__, nullptr);
    VALIDATE_POINTER1(pnCount, __func__, nullptr);
    auto dims = hGroup->m_poImpl->GetDimensions(papszOptions);
    auto ret = static_cast<GDALDimensionH *>(
        CPLMalloc(sizeof(GDALDimensionH) * dims.size()));
    for (size_t i = 0; i < dims.size(); i++)
    {
        ret[i] = new GDALDimensionHS(dims[i]);
    }
    *pnCount = dims.size();
    return ret;
}

/************************************************************************/
/*                          GDALGroupGetAttribute()                     */
/************************************************************************/

/** Return an attribute by its name.
 *
 * This is the same as the C++ method GDALIHasAttribute::GetAttribute()
 *
 * The returned attribute must be freed with GDALAttributeRelease().
 */
GDALAttributeH GDALGroupGetAttribute(GDALGroupH hGroup, const char *pszName)
{
    VALIDATE_POINTER1(hGroup, __func__, nullptr);
    VALIDATE_POINTER1(pszName, __func__, nullptr);
    auto attr = hGroup->m_poImpl->GetAttribute(std::string(pszName));
    if (attr)
        return new GDALAttributeHS(attr);
    return nullptr;
}

/************************************************************************/
/*                         GDALGroupGetAttributes()                     */
/************************************************************************/

/** Return the list of attributes contained in this group.
 *
 * The returned array must be freed with GDALReleaseAttributes(). If only the
 * array itself needs to be freed, CPLFree() should be called (and
 * GDALAttributeRelease() on individual array members).
 *
 * This is the same as the C++ method GDALGroup::GetAttributes().
 *
 * @param hGroup Group.
 * @param pnCount Pointer to the number of values returned. Must NOT be NULL.
 * @param papszOptions Driver specific options determining how attributes
 * should be retrieved. Pass nullptr for default behavior.
 *
 * @return an array of *pnCount attributes.
 */
GDALAttributeH *GDALGroupGetAttributes(GDALGroupH hGroup, size_t *pnCount,
                                       CSLConstList papszOptions)
{
    VALIDATE_POINTER1(hGroup, __func__, nullptr);
    VALIDATE_POINTER1(pnCount, __func__, nullptr);
    auto attrs = hGroup->m_poImpl->GetAttributes(papszOptions);
    auto ret = static_cast<GDALAttributeH *>(
        CPLMalloc(sizeof(GDALAttributeH) * attrs.size()));
    for (size_t i = 0; i < attrs.size(); i++)
    {
        ret[i] = new GDALAttributeHS(attrs[i]);
    }
    *pnCount = attrs.size();
    return ret;
}

/************************************************************************/
/*                     GDALGroupGetStructuralInfo()                     */
/************************************************************************/

/** Return structural information on the group.
 *
 * This may be the compression, etc..
 *
 * The return value should not be freed and is valid until GDALGroup is
 * released or this function called again.
 *
 * This is the same as the C++ method GDALGroup::GetStructuralInfo().
 */
CSLConstList GDALGroupGetStructuralInfo(GDALGroupH hGroup)
{
    VALIDATE_POINTER1(hGroup, __func__, nullptr);
    return hGroup->m_poImpl->GetStructuralInfo();
}

/************************************************************************/
/*                   GDALGroupGetDataTypeCount()                        */
/************************************************************************/

/** Return the number of data types associated with the group
 * (typically enumerations).
 *
 * This is the same as the C++ method GDALGroup::GetDataTypes().size().
 *
 * @since 3.12
 */
size_t GDALGroupGetDataTypeCount(GDALGroupH hGroup)
{
    VALIDATE_POINTER1(hGroup, __func__, 0);
    return hGroup->m_poImpl->GetDataTypes().size();
}

/************************************************************************/
/*                      GDALGroupGetDataType()                          */
/************************************************************************/

/** Return one of the data types associated with the group.
 *
 * This is the same as the C++ method GDALGroup::GetDataTypes()[].
 *
 * @return a type to release with GDALExtendedDataTypeRelease() once done,
 * or nullptr in case of error.
 * @since 3.12
 */
GDALExtendedDataTypeH GDALGroupGetDataType(GDALGroupH hGroup, size_t nIdx)
{
    VALIDATE_POINTER1(hGroup, __func__, nullptr);
    if (nIdx >= hGroup->m_poImpl->GetDataTypes().size())
        return nullptr;
    return new GDALExtendedDataTypeHS(new GDALExtendedDataType(
        *(hGroup->m_poImpl->GetDataTypes()[nIdx].get())));
}

/************************************************************************/
/*                         GDALReleaseAttributes()                      */
/************************************************************************/

/** Free the return of GDALGroupGetAttributes() or GDALMDArrayGetAttributes()
 *
 * @param attributes return pointer of above methods
 * @param nCount *pnCount value returned by above methods
 */
void GDALReleaseAttributes(GDALAttributeH *attributes, size_t nCount)
{
    for (size_t i = 0; i < nCount; i++)
    {
        delete attributes[i];
    }
    CPLFree(attributes);
}

/************************************************************************/
/*                         GDALGroupCreateGroup()                       */
/************************************************************************/

/** Create a sub-group within a group.
 *
 * This is the same as the C++ method GDALGroup::CreateGroup().
 *
 * @return the sub-group, to be freed with GDALGroupRelease(), or nullptr.
 */
GDALGroupH GDALGroupCreateGroup(GDALGroupH hGroup, const char *pszSubGroupName,
                                CSLConstList papszOptions)
{
    VALIDATE_POINTER1(hGroup, __func__, nullptr);
    VALIDATE_POINTER1(pszSubGroupName, __func__, nullptr);
    auto ret = hGroup->m_poImpl->CreateGroup(std::string(pszSubGroupName),
                                             papszOptions);
    if (!ret)
        return nullptr;
    return new GDALGroupHS(ret);
}

/************************************************************************/
/*                         GDALGroupDeleteGroup()                       */
/************************************************************************/

/** Delete a sub-group from a group.
 *
 * After this call, if a previously obtained instance of the deleted object
 * is still alive, no method other than for freeing it should be invoked.
 *
 * This is the same as the C++ method GDALGroup::DeleteGroup().
 *
 * @return true in case of success.
 * @since GDAL 3.8
 */
bool GDALGroupDeleteGroup(GDALGroupH hGroup, const char *pszSubGroupName,
                          CSLConstList papszOptions)
{
    VALIDATE_POINTER1(hGroup, __func__, false);
    VALIDATE_POINTER1(pszSubGroupName, __func__, false);
    return hGroup->m_poImpl->DeleteGroup(std::string(pszSubGroupName),
                                         papszOptions);
}

/************************************************************************/
/*                      GDALGroupCreateDimension()                      */
/************************************************************************/

/** Create a dimension within a group.
 *
 * This is the same as the C++ method GDALGroup::CreateDimension().
 *
 * @return the dimension, to be freed with GDALDimensionRelease(), or nullptr.
 */
GDALDimensionH GDALGroupCreateDimension(GDALGroupH hGroup, const char *pszName,
                                        const char *pszType,
                                        const char *pszDirection, GUInt64 nSize,
                                        CSLConstList papszOptions)
{
    VALIDATE_POINTER1(hGroup, __func__, nullptr);
    VALIDATE_POINTER1(pszName, __func__, nullptr);
    auto ret = hGroup->m_poImpl->CreateDimension(
        std::string(pszName), std::string(pszType ? pszType : ""),
        std::string(pszDirection ? pszDirection : ""), nSize, papszOptions);
    if (!ret)
        return nullptr;
    return new GDALDimensionHS(ret);
}

/************************************************************************/
/*                      GDALGroupCreateMDArray()                        */
/************************************************************************/

/** Create a multidimensional array within a group.
 *
 * This is the same as the C++ method GDALGroup::CreateMDArray().
 *
 * @return the array, to be freed with GDALMDArrayRelease(), or nullptr.
 */
GDALMDArrayH GDALGroupCreateMDArray(GDALGroupH hGroup, const char *pszName,
                                    size_t nDimensions,
                                    GDALDimensionH *pahDimensions,
                                    GDALExtendedDataTypeH hEDT,
                                    CSLConstList papszOptions)
{
    VALIDATE_POINTER1(hGroup, __func__, nullptr);
    VALIDATE_POINTER1(pszName, __func__, nullptr);
    VALIDATE_POINTER1(hEDT, __func__, nullptr);
    std::vector<std::shared_ptr<GDALDimension>> dims;
    dims.reserve(nDimensions);
    for (size_t i = 0; i < nDimensions; i++)
        dims.push_back(pahDimensions[i]->m_poImpl);
    auto ret = hGroup->m_poImpl->CreateMDArray(std::string(pszName), dims,
                                               *(hEDT->m_poImpl), papszOptions);
    if (!ret)
        return nullptr;
    return new GDALMDArrayHS(ret);
}

/************************************************************************/
/*                         GDALGroupDeleteMDArray()                     */
/************************************************************************/

/** Delete an array from a group.
 *
 * After this call, if a previously obtained instance of the deleted object
 * is still alive, no method other than for freeing it should be invoked.
 *
 * This is the same as the C++ method GDALGroup::DeleteMDArray().
 *
 * @return true in case of success.
 * @since GDAL 3.8
 */
bool GDALGroupDeleteMDArray(GDALGroupH hGroup, const char *pszName,
                            CSLConstList papszOptions)
{
    VALIDATE_POINTER1(hGroup, __func__, false);
    VALIDATE_POINTER1(pszName, __func__, false);
    return hGroup->m_poImpl->DeleteMDArray(std::string(pszName), papszOptions);
}

/************************************************************************/
/*                      GDALGroupCreateAttribute()                      */
/************************************************************************/

/** Create a attribute within a group.
 *
 * This is the same as the C++ method GDALGroup::CreateAttribute().
 *
 * @return the attribute, to be freed with GDALAttributeRelease(), or nullptr.
 */
GDALAttributeH GDALGroupCreateAttribute(GDALGroupH hGroup, const char *pszName,
                                        size_t nDimensions,
                                        const GUInt64 *panDimensions,
                                        GDALExtendedDataTypeH hEDT,
                                        CSLConstList papszOptions)
{
    VALIDATE_POINTER1(hGroup, __func__, nullptr);
    VALIDATE_POINTER1(hEDT, __func__, nullptr);
    std::vector<GUInt64> dims;
    dims.reserve(nDimensions);
    for (size_t i = 0; i < nDimensions; i++)
        dims.push_back(panDimensions[i]);
    auto ret = hGroup->m_poImpl->CreateAttribute(
        std::string(pszName), dims, *(hEDT->m_poImpl), papszOptions);
    if (!ret)
        return nullptr;
    return new GDALAttributeHS(ret);
}

/************************************************************************/
/*                         GDALGroupDeleteAttribute()                   */
/************************************************************************/

/** Delete an attribute from a group.
 *
 * After this call, if a previously obtained instance of the deleted object
 * is still alive, no method other than for freeing it should be invoked.
 *
 * This is the same as the C++ method GDALGroup::DeleteAttribute().
 *
 * @return true in case of success.
 * @since GDAL 3.8
 */
bool GDALGroupDeleteAttribute(GDALGroupH hGroup, const char *pszName,
                              CSLConstList papszOptions)
{
    VALIDATE_POINTER1(hGroup, __func__, false);
    VALIDATE_POINTER1(pszName, __func__, false);
    return hGroup->m_poImpl->DeleteAttribute(std::string(pszName),
                                             papszOptions);
}

/************************************************************************/
/*                          GDALGroupRename()                           */
/************************************************************************/

/** Rename the group.
 *
 * This is not implemented by all drivers.
 *
 * Drivers known to implement it: MEM, netCDF.
 *
 * This is the same as the C++ method GDALGroup::Rename()
 *
 * @return true in case of success
 * @since GDAL 3.8
 */
bool GDALGroupRename(GDALGroupH hGroup, const char *pszNewName)
{
    VALIDATE_POINTER1(hGroup, __func__, false);
    VALIDATE_POINTER1(pszNewName, __func__, false);
    return hGroup->m_poImpl->Rename(pszNewName);
}

/************************************************************************/
/*                 GDALGroupSubsetDimensionFromSelection()              */
/************************************************************************/

/** Return a virtual group whose one dimension has been subset according to a
 * selection.
 *
 * This is the same as the C++ method GDALGroup::SubsetDimensionFromSelection().
 *
 * @return a virtual group, to be freed with GDALGroupRelease(), or nullptr.
 */
GDALGroupH
GDALGroupSubsetDimensionFromSelection(GDALGroupH hGroup,
                                      const char *pszSelection,
                                      CPL_UNUSED CSLConstList papszOptions)
{
    VALIDATE_POINTER1(hGroup, __func__, nullptr);
    VALIDATE_POINTER1(pszSelection, __func__, nullptr);
    auto hNewGroup = hGroup->m_poImpl->SubsetDimensionFromSelection(
        std::string(pszSelection));
    if (!hNewGroup)
        return nullptr;
    return new GDALGroupHS(hNewGroup);
}

/************************************************************************/
/*                        GDALMDArrayRelease()                          */
/************************************************************************/

/** Release the GDAL in-memory object associated with a GDALMDArray.
 *
 * Note: when applied on a object coming from a driver, this does not
 * destroy the object in the file, database, etc...
 */
void GDALMDArrayRelease(GDALMDArrayH hMDArray)
{
    delete hMDArray;
}

/************************************************************************/
/*                        GDALMDArrayGetName()                          */
/************************************************************************/

/** Return array name.
 *
 * This is the same as the C++ method GDALMDArray::GetName()
 */
const char *GDALMDArrayGetName(GDALMDArrayH hArray)
{
    VALIDATE_POINTER1(hArray, __func__, nullptr);
    return hArray->m_poImpl->GetName().c_str();
}

/************************************************************************/
/*                    GDALMDArrayGetFullName()                          */
/************************************************************************/

/** Return array full name.
 *
 * This is the same as the C++ method GDALMDArray::GetFullName()
 */
const char *GDALMDArrayGetFullName(GDALMDArrayH hArray)
{
    VALIDATE_POINTER1(hArray, __func__, nullptr);
    return hArray->m_poImpl->GetFullName().c_str();
}

/************************************************************************/
/*                        GDALMDArrayGetName()                          */
/************************************************************************/

/** Return the total number of values in the array.
 *
 * This is the same as the C++ method
 * GDALAbstractMDArray::GetTotalElementsCount()
 */
GUInt64 GDALMDArrayGetTotalElementsCount(GDALMDArrayH hArray)
{
    VALIDATE_POINTER1(hArray, __func__, 0);
    return hArray->m_poImpl->GetTotalElementsCount();
}

/************************************************************************/
/*                        GDALMDArrayGetDimensionCount()                */
/************************************************************************/

/** Return the number of dimensions.
 *
 * This is the same as the C++ method GDALAbstractMDArray::GetDimensionCount()
 */
size_t GDALMDArrayGetDimensionCount(GDALMDArrayH hArray)
{
    VALIDATE_POINTER1(hArray, __func__, 0);
    return hArray->m_poImpl->GetDimensionCount();
}

/************************************************************************/
/*                        GDALMDArrayGetDimensions()                    */
/************************************************************************/

/** Return the dimensions of the array
 *
 * The returned array must be freed with GDALReleaseDimensions(). If only the
 * array itself needs to be freed, CPLFree() should be called (and
 * GDALDimensionRelease() on individual array members).
 *
 * This is the same as the C++ method GDALAbstractMDArray::GetDimensions()
 *
 * @param hArray Array.
 * @param pnCount Pointer to the number of values returned. Must NOT be NULL.
 *
 * @return an array of *pnCount dimensions.
 */
GDALDimensionH *GDALMDArrayGetDimensions(GDALMDArrayH hArray, size_t *pnCount)
{
    VALIDATE_POINTER1(hArray, __func__, nullptr);
    VALIDATE_POINTER1(pnCount, __func__, nullptr);
    const auto &dims(hArray->m_poImpl->GetDimensions());
    auto ret = static_cast<GDALDimensionH *>(
        CPLMalloc(sizeof(GDALDimensionH) * dims.size()));
    for (size_t i = 0; i < dims.size(); i++)
    {
        ret[i] = new GDALDimensionHS(dims[i]);
    }
    *pnCount = dims.size();
    return ret;
}

/************************************************************************/
/*                        GDALReleaseDimensions()                       */
/************************************************************************/

/** Free the return of GDALGroupGetDimensions() or GDALMDArrayGetDimensions()
 *
 * @param dims return pointer of above methods
 * @param nCount *pnCount value returned by above methods
 */
void GDALReleaseDimensions(GDALDimensionH *dims, size_t nCount)
{
    for (size_t i = 0; i < nCount; i++)
    {
        delete dims[i];
    }
    CPLFree(dims);
}

/************************************************************************/
/*                        GDALMDArrayGetDataType()                     */
/************************************************************************/

/** Return the data type
 *
 * The return must be freed with GDALExtendedDataTypeRelease().
 */
GDALExtendedDataTypeH GDALMDArrayGetDataType(GDALMDArrayH hArray)
{
    VALIDATE_POINTER1(hArray, __func__, nullptr);
    return new GDALExtendedDataTypeHS(
        new GDALExtendedDataType(hArray->m_poImpl->GetDataType()));
}

/************************************************************************/
/*                          GDALMDArrayRead()                           */
/************************************************************************/

/** Read part or totality of a multidimensional array.
 *
 * This is the same as the C++ method GDALAbstractMDArray::Read()
 *
 * @return TRUE in case of success.
 */
int GDALMDArrayRead(GDALMDArrayH hArray, const GUInt64 *arrayStartIdx,
                    const size_t *count, const GInt64 *arrayStep,
                    const GPtrDiff_t *bufferStride,
                    GDALExtendedDataTypeH bufferDataType, void *pDstBuffer,
                    const void *pDstBufferAllocStart,
                    size_t nDstBufferAllocSize)
{
    VALIDATE_POINTER1(hArray, __func__, FALSE);
    if ((arrayStartIdx == nullptr || count == nullptr) &&
        hArray->m_poImpl->GetDimensionCount() > 0)
    {
        VALIDATE_POINTER1(arrayStartIdx, __func__, FALSE);
        VALIDATE_POINTER1(count, __func__, FALSE);
    }
    VALIDATE_POINTER1(bufferDataType, __func__, FALSE);
    VALIDATE_POINTER1(pDstBuffer, __func__, FALSE);
    return hArray->m_poImpl->Read(arrayStartIdx, count, arrayStep, bufferStride,
                                  *(bufferDataType->m_poImpl), pDstBuffer,
                                  pDstBufferAllocStart, nDstBufferAllocSize);
}

/************************************************************************/
/*                          GDALMDArrayWrite()                           */
/************************************************************************/

/** Write part or totality of a multidimensional array.
 *
 * This is the same as the C++ method GDALAbstractMDArray::Write()
 *
 * @return TRUE in case of success.
 */
int GDALMDArrayWrite(GDALMDArrayH hArray, const GUInt64 *arrayStartIdx,
                     const size_t *count, const GInt64 *arrayStep,
                     const GPtrDiff_t *bufferStride,
                     GDALExtendedDataTypeH bufferDataType,
                     const void *pSrcBuffer, const void *pSrcBufferAllocStart,
                     size_t nSrcBufferAllocSize)
{
    VALIDATE_POINTER1(hArray, __func__, FALSE);
    if ((arrayStartIdx == nullptr || count == nullptr) &&
        hArray->m_poImpl->GetDimensionCount() > 0)
    {
        VALIDATE_POINTER1(arrayStartIdx, __func__, FALSE);
        VALIDATE_POINTER1(count, __func__, FALSE);
    }
    VALIDATE_POINTER1(bufferDataType, __func__, FALSE);
    VALIDATE_POINTER1(pSrcBuffer, __func__, FALSE);
    return hArray->m_poImpl->Write(arrayStartIdx, count, arrayStep,
                                   bufferStride, *(bufferDataType->m_poImpl),
                                   pSrcBuffer, pSrcBufferAllocStart,
                                   nSrcBufferAllocSize);
}

/************************************************************************/
/*                       GDALMDArrayAdviseRead()                        */
/************************************************************************/

/** Advise driver of upcoming read requests.
 *
 * This is the same as the C++ method GDALMDArray::AdviseRead()
 *
 * @return TRUE in case of success.
 *
 * @since GDAL 3.2
 */
int GDALMDArrayAdviseRead(GDALMDArrayH hArray, const GUInt64 *arrayStartIdx,
                          const size_t *count)
{
    return GDALMDArrayAdviseReadEx(hArray, arrayStartIdx, count, nullptr);
}

/************************************************************************/
/*                      GDALMDArrayAdviseReadEx()                       */
/************************************************************************/

/** Advise driver of upcoming read requests.
 *
 * This is the same as the C++ method GDALMDArray::AdviseRead()
 *
 * @return TRUE in case of success.
 *
 * @since GDAL 3.4
 */
int GDALMDArrayAdviseReadEx(GDALMDArrayH hArray, const GUInt64 *arrayStartIdx,
                            const size_t *count, CSLConstList papszOptions)
{
    VALIDATE_POINTER1(hArray, __func__, FALSE);
    return hArray->m_poImpl->AdviseRead(arrayStartIdx, count, papszOptions);
}

/************************************************************************/
/*                         GDALMDArrayGetAttribute()                    */
/************************************************************************/

/** Return an attribute by its name.
 *
 * This is the same as the C++ method GDALIHasAttribute::GetAttribute()
 *
 * The returned attribute must be freed with GDALAttributeRelease().
 */
GDALAttributeH GDALMDArrayGetAttribute(GDALMDArrayH hArray, const char *pszName)
{
    VALIDATE_POINTER1(hArray, __func__, nullptr);
    VALIDATE_POINTER1(pszName, __func__, nullptr);
    auto attr = hArray->m_poImpl->GetAttribute(std::string(pszName));
    if (attr)
        return new GDALAttributeHS(attr);
    return nullptr;
}

/************************************************************************/
/*                        GDALMDArrayGetAttributes()                    */
/************************************************************************/

/** Return the list of attributes contained in this array.
 *
 * The returned array must be freed with GDALReleaseAttributes(). If only the
 * array itself needs to be freed, CPLFree() should be called (and
 * GDALAttributeRelease() on individual array members).
 *
 * This is the same as the C++ method GDALMDArray::GetAttributes().
 *
 * @param hArray Array.
 * @param pnCount Pointer to the number of values returned. Must NOT be NULL.
 * @param papszOptions Driver specific options determining how attributes
 * should be retrieved. Pass nullptr for default behavior.
 *
 * @return an array of *pnCount attributes.
 */
GDALAttributeH *GDALMDArrayGetAttributes(GDALMDArrayH hArray, size_t *pnCount,
                                         CSLConstList papszOptions)
{
    VALIDATE_POINTER1(hArray, __func__, nullptr);
    VALIDATE_POINTER1(pnCount, __func__, nullptr);
    auto attrs = hArray->m_poImpl->GetAttributes(papszOptions);
    auto ret = static_cast<GDALAttributeH *>(
        CPLMalloc(sizeof(GDALAttributeH) * attrs.size()));
    for (size_t i = 0; i < attrs.size(); i++)
    {
        ret[i] = new GDALAttributeHS(attrs[i]);
    }
    *pnCount = attrs.size();
    return ret;
}

/************************************************************************/
/*                       GDALMDArrayCreateAttribute()                   */
/************************************************************************/

/** Create a attribute within an array.
 *
 * This is the same as the C++ method GDALMDArray::CreateAttribute().
 *
 * @return the attribute, to be freed with GDALAttributeRelease(), or nullptr.
 */
GDALAttributeH GDALMDArrayCreateAttribute(GDALMDArrayH hArray,
                                          const char *pszName,
                                          size_t nDimensions,
                                          const GUInt64 *panDimensions,
                                          GDALExtendedDataTypeH hEDT,
                                          CSLConstList papszOptions)
{
    VALIDATE_POINTER1(hArray, __func__, nullptr);
    VALIDATE_POINTER1(pszName, __func__, nullptr);
    VALIDATE_POINTER1(hEDT, __func__, nullptr);
    std::vector<GUInt64> dims;
    dims.reserve(nDimensions);
    for (size_t i = 0; i < nDimensions; i++)
        dims.push_back(panDimensions[i]);
    auto ret = hArray->m_poImpl->CreateAttribute(
        std::string(pszName), dims, *(hEDT->m_poImpl), papszOptions);
    if (!ret)
        return nullptr;
    return new GDALAttributeHS(ret);
}

/************************************************************************/
/*                       GDALMDArrayDeleteAttribute()                   */
/************************************************************************/

/** Delete an attribute from an array.
 *
 * After this call, if a previously obtained instance of the deleted object
 * is still alive, no method other than for freeing it should be invoked.
 *
 * This is the same as the C++ method GDALMDArray::DeleteAttribute().
 *
 * @return true in case of success.
 * @since GDAL 3.8
 */
bool GDALMDArrayDeleteAttribute(GDALMDArrayH hArray, const char *pszName,
                                CSLConstList papszOptions)
{
    VALIDATE_POINTER1(hArray, __func__, false);
    VALIDATE_POINTER1(pszName, __func__, false);
    return hArray->m_poImpl->DeleteAttribute(std::string(pszName),
                                             papszOptions);
}

/************************************************************************/
/*                       GDALMDArrayGetRawNoDataValue()                 */
/************************************************************************/

/** Return the nodata value as a "raw" value.
 *
 * The value returned might be nullptr in case of no nodata value. When
 * a nodata value is registered, a non-nullptr will be returned whose size in
 * bytes is GetDataType().GetSize().
 *
 * The returned value should not be modified or freed.
 *
 * This is the same as the ++ method GDALMDArray::GetRawNoDataValue().
 *
 * @return nullptr or a pointer to GetDataType().GetSize() bytes.
 */
const void *GDALMDArrayGetRawNoDataValue(GDALMDArrayH hArray)
{
    VALIDATE_POINTER1(hArray, __func__, nullptr);
    return hArray->m_poImpl->GetRawNoDataValue();
}

/************************************************************************/
/*                      GDALMDArrayGetNoDataValueAsDouble()             */
/************************************************************************/

/** Return the nodata value as a double.
 *
 * The value returned might be nullptr in case of no nodata value. When
 * a nodata value is registered, a non-nullptr will be returned whose size in
 * bytes is GetDataType().GetSize().
 *
 * This is the same as the C++ method GDALMDArray::GetNoDataValueAsDouble().
 *
 * @param hArray Array handle.
 * @param pbHasNoDataValue Pointer to a output boolean that will be set to true
 * if a nodata value exists and can be converted to double. Might be nullptr.
 *
 * @return the nodata value as a double. A 0.0 value might also indicate the
 * absence of a nodata value or an error in the conversion (*pbHasNoDataValue
 * will be set to false then).
 */
double GDALMDArrayGetNoDataValueAsDouble(GDALMDArrayH hArray,
                                         int *pbHasNoDataValue)
{
    VALIDATE_POINTER1(hArray, __func__, 0);
    bool bHasNodataValue = false;
    double ret = hArray->m_poImpl->GetNoDataValueAsDouble(&bHasNodataValue);
    if (pbHasNoDataValue)
        *pbHasNoDataValue = bHasNodataValue;
    return ret;
}

/************************************************************************/
/*                      GDALMDArrayGetNoDataValueAsInt64()              */
/************************************************************************/

/** Return the nodata value as a Int64.
 *
 * This is the same as the C++ method GDALMDArray::GetNoDataValueAsInt64().
 *
 * @param hArray Array handle.
 * @param pbHasNoDataValue Pointer to a output boolean that will be set to true
 * if a nodata value exists and can be converted to Int64. Might be nullptr.
 *
 * @return the nodata value as a Int64.
 * @since GDAL 3.5
 */
int64_t GDALMDArrayGetNoDataValueAsInt64(GDALMDArrayH hArray,
                                         int *pbHasNoDataValue)
{
    VALIDATE_POINTER1(hArray, __func__, 0);
    bool bHasNodataValue = false;
    const auto ret = hArray->m_poImpl->GetNoDataValueAsInt64(&bHasNodataValue);
    if (pbHasNoDataValue)
        *pbHasNoDataValue = bHasNodataValue;
    return ret;
}

/************************************************************************/
/*                      GDALMDArrayGetNoDataValueAsUInt64()              */
/************************************************************************/

/** Return the nodata value as a UInt64.
 *
 * This is the same as the C++ method GDALMDArray::GetNoDataValueAsInt64().
 *
 * @param hArray Array handle.
 * @param pbHasNoDataValue Pointer to a output boolean that will be set to true
 * if a nodata value exists and can be converted to UInt64. Might be nullptr.
 *
 * @return the nodata value as a UInt64.
 * @since GDAL 3.5
 */
uint64_t GDALMDArrayGetNoDataValueAsUInt64(GDALMDArrayH hArray,
                                           int *pbHasNoDataValue)
{
    VALIDATE_POINTER1(hArray, __func__, 0);
    bool bHasNodataValue = false;
    const auto ret = hArray->m_poImpl->GetNoDataValueAsUInt64(&bHasNodataValue);
    if (pbHasNoDataValue)
        *pbHasNoDataValue = bHasNodataValue;
    return ret;
}

/************************************************************************/
/*                     GDALMDArraySetRawNoDataValue()                   */
/************************************************************************/

/** Set the nodata value as a "raw" value.
 *
 * This is the same as the C++ method GDALMDArray::SetRawNoDataValue(const
 * void*).
 *
 * @return TRUE in case of success.
 */
int GDALMDArraySetRawNoDataValue(GDALMDArrayH hArray, const void *pNoData)
{
    VALIDATE_POINTER1(hArray, __func__, FALSE);
    return hArray->m_poImpl->SetRawNoDataValue(pNoData);
}

/************************************************************************/
/*                   GDALMDArraySetNoDataValueAsDouble()                */
/************************************************************************/

/** Set the nodata value as a double.
 *
 * If the natural data type of the attribute/array is not double, type
 * conversion will occur to the type returned by GetDataType().
 *
 * This is the same as the C++ method GDALMDArray::SetNoDataValue(double).
 *
 * @return TRUE in case of success.
 */
int GDALMDArraySetNoDataValueAsDouble(GDALMDArrayH hArray, double dfNoDataValue)
{
    VALIDATE_POINTER1(hArray, __func__, FALSE);
    return hArray->m_poImpl->SetNoDataValue(dfNoDataValue);
}

/************************************************************************/
/*                   GDALMDArraySetNoDataValueAsInt64()                 */
/************************************************************************/

/** Set the nodata value as a Int64.
 *
 * If the natural data type of the attribute/array is not Int64, type conversion
 * will occur to the type returned by GetDataType().
 *
 * This is the same as the C++ method GDALMDArray::SetNoDataValue(int64_t).
 *
 * @return TRUE in case of success.
 * @since GDAL 3.5
 */
int GDALMDArraySetNoDataValueAsInt64(GDALMDArrayH hArray, int64_t nNoDataValue)
{
    VALIDATE_POINTER1(hArray, __func__, FALSE);
    return hArray->m_poImpl->SetNoDataValue(nNoDataValue);
}

/************************************************************************/
/*                   GDALMDArraySetNoDataValueAsUInt64()                */
/************************************************************************/

/** Set the nodata value as a UInt64.
 *
 * If the natural data type of the attribute/array is not UInt64, type
 * conversion will occur to the type returned by GetDataType().
 *
 * This is the same as the C++ method GDALMDArray::SetNoDataValue(uint64_t).
 *
 * @return TRUE in case of success.
 * @since GDAL 3.5
 */
int GDALMDArraySetNoDataValueAsUInt64(GDALMDArrayH hArray,
                                      uint64_t nNoDataValue)
{
    VALIDATE_POINTER1(hArray, __func__, FALSE);
    return hArray->m_poImpl->SetNoDataValue(nNoDataValue);
}

/************************************************************************/
/*                        GDALMDArrayResize()                           */
/************************************************************************/

/** Resize an array to new dimensions.
 *
 * Not all drivers may allow this operation, and with restrictions (e.g.
 * for netCDF, this is limited to growing of "unlimited" dimensions)
 *
 * Resizing a dimension used in other arrays will cause those other arrays
 * to be resized.
 *
 * This is the same as the C++ method GDALMDArray::Resize().
 *
 * @param hArray Array.
 * @param panNewDimSizes Array of GetDimensionCount() values containing the
 *                       new size of each indexing dimension.
 * @param papszOptions Options. (Driver specific)
 * @return true in case of success.
 * @since GDAL 3.7
 */
bool GDALMDArrayResize(GDALMDArrayH hArray, const GUInt64 *panNewDimSizes,
                       CSLConstList papszOptions)
{
    VALIDATE_POINTER1(hArray, __func__, false);
    VALIDATE_POINTER1(panNewDimSizes, __func__, false);
    std::vector<GUInt64> anNewDimSizes(hArray->m_poImpl->GetDimensionCount());
    for (size_t i = 0; i < anNewDimSizes.size(); ++i)
    {
        anNewDimSizes[i] = panNewDimSizes[i];
    }
    return hArray->m_poImpl->Resize(anNewDimSizes, papszOptions);
}

/************************************************************************/
/*                          GDALMDArraySetScale()                       */
/************************************************************************/

/** Set the scale value to apply to raw values.
 *
 * unscaled_value = raw_value * GetScale() + GetOffset()
 *
 * This is the same as the C++ method GDALMDArray::SetScale().
 *
 * @return TRUE in case of success.
 */
int GDALMDArraySetScale(GDALMDArrayH hArray, double dfScale)
{
    VALIDATE_POINTER1(hArray, __func__, FALSE);
    return hArray->m_poImpl->SetScale(dfScale);
}

/************************************************************************/
/*                        GDALMDArraySetScaleEx()                       */
/************************************************************************/

/** Set the scale value to apply to raw values.
 *
 * unscaled_value = raw_value * GetScale() + GetOffset()
 *
 * This is the same as the C++ method GDALMDArray::SetScale().
 *
 * @return TRUE in case of success.
 * @since GDAL 3.3
 */
int GDALMDArraySetScaleEx(GDALMDArrayH hArray, double dfScale,
                          GDALDataType eStorageType)
{
    VALIDATE_POINTER1(hArray, __func__, FALSE);
    return hArray->m_poImpl->SetScale(dfScale, eStorageType);
}

/************************************************************************/
/*                          GDALMDArraySetOffset()                       */
/************************************************************************/

/** Set the scale value to apply to raw values.
 *
 * unscaled_value = raw_value * GetScale() + GetOffset()
 *
 * This is the same as the C++ method GDALMDArray::SetOffset().
 *
 * @return TRUE in case of success.
 */
int GDALMDArraySetOffset(GDALMDArrayH hArray, double dfOffset)
{
    VALIDATE_POINTER1(hArray, __func__, FALSE);
    return hArray->m_poImpl->SetOffset(dfOffset);
}

/************************************************************************/
/*                       GDALMDArraySetOffsetEx()                       */
/************************************************************************/

/** Set the scale value to apply to raw values.
 *
 * unscaled_value = raw_value * GetOffset() + GetOffset()
 *
 * This is the same as the C++ method GDALMDArray::SetOffset().
 *
 * @return TRUE in case of success.
 * @since GDAL 3.3
 */
int GDALMDArraySetOffsetEx(GDALMDArrayH hArray, double dfOffset,
                           GDALDataType eStorageType)
{
    VALIDATE_POINTER1(hArray, __func__, FALSE);
    return hArray->m_poImpl->SetOffset(dfOffset, eStorageType);
}

/************************************************************************/
/*                          GDALMDArrayGetScale()                       */
/************************************************************************/

/** Get the scale value to apply to raw values.
 *
 * unscaled_value = raw_value * GetScale() + GetOffset()
 *
 * This is the same as the C++ method GDALMDArray::GetScale().
 *
 * @return the scale value
 */
double GDALMDArrayGetScale(GDALMDArrayH hArray, int *pbHasValue)
{
    VALIDATE_POINTER1(hArray, __func__, 0.0);
    bool bHasValue = false;
    double dfRet = hArray->m_poImpl->GetScale(&bHasValue);
    if (pbHasValue)
        *pbHasValue = bHasValue;
    return dfRet;
}

/************************************************************************/
/*                        GDALMDArrayGetScaleEx()                       */
/************************************************************************/

/** Get the scale value to apply to raw values.
 *
 * unscaled_value = raw_value * GetScale() + GetScale()
 *
 * This is the same as the C++ method GDALMDArray::GetScale().
 *
 * @return the scale value
 * @since GDAL 3.3
 */
double GDALMDArrayGetScaleEx(GDALMDArrayH hArray, int *pbHasValue,
                             GDALDataType *peStorageType)
{
    VALIDATE_POINTER1(hArray, __func__, 0.0);
    bool bHasValue = false;
    double dfRet = hArray->m_poImpl->GetScale(&bHasValue, peStorageType);
    if (pbHasValue)
        *pbHasValue = bHasValue;
    return dfRet;
}

/************************************************************************/
/*                          GDALMDArrayGetOffset()                      */
/************************************************************************/

/** Get the scale value to apply to raw values.
 *
 * unscaled_value = raw_value * GetScale() + GetOffset()
 *
 * This is the same as the C++ method GDALMDArray::GetOffset().
 *
 * @return the scale value
 */
double GDALMDArrayGetOffset(GDALMDArrayH hArray, int *pbHasValue)
{
    VALIDATE_POINTER1(hArray, __func__, 0.0);
    bool bHasValue = false;
    double dfRet = hArray->m_poImpl->GetOffset(&bHasValue);
    if (pbHasValue)
        *pbHasValue = bHasValue;
    return dfRet;
}

/************************************************************************/
/*                        GDALMDArrayGetOffsetEx()                      */
/************************************************************************/

/** Get the scale value to apply to raw values.
 *
 * unscaled_value = raw_value * GetScale() + GetOffset()
 *
 * This is the same as the C++ method GDALMDArray::GetOffset().
 *
 * @return the scale value
 * @since GDAL 3.3
 */
double GDALMDArrayGetOffsetEx(GDALMDArrayH hArray, int *pbHasValue,
                              GDALDataType *peStorageType)
{
    VALIDATE_POINTER1(hArray, __func__, 0.0);
    bool bHasValue = false;
    double dfRet = hArray->m_poImpl->GetOffset(&bHasValue, peStorageType);
    if (pbHasValue)
        *pbHasValue = bHasValue;
    return dfRet;
}

/************************************************************************/
/*                      GDALMDArrayGetBlockSize()                       */
/************************************************************************/

/** Return the "natural" block size of the array along all dimensions.
 *
 * Some drivers might organize the array in tiles/blocks and reading/writing
 * aligned on those tile/block boundaries will be more efficient.
 *
 * The returned number of elements in the vector is the same as
 * GetDimensionCount(). A value of 0 should be interpreted as no hint regarding
 * the natural block size along the considered dimension.
 * "Flat" arrays will typically return a vector of values set to 0.
 *
 * The default implementation will return a vector of values set to 0.
 *
 * This method is used by GetProcessingChunkSize().
 *
 * Pedantic note: the returned type is GUInt64, so in the highly unlikeley
 * theoretical case of a 32-bit platform, this might exceed its size_t
 * allocation capabilities.
 *
 * This is the same as the C++ method GDALAbstractMDArray::GetBlockSize().
 *
 * @return the block size, in number of elements along each dimension.
 */
GUInt64 *GDALMDArrayGetBlockSize(GDALMDArrayH hArray, size_t *pnCount)
{
    VALIDATE_POINTER1(hArray, __func__, nullptr);
    VALIDATE_POINTER1(pnCount, __func__, nullptr);
    auto res = hArray->m_poImpl->GetBlockSize();
    auto ret = static_cast<GUInt64 *>(CPLMalloc(sizeof(GUInt64) * res.size()));
    for (size_t i = 0; i < res.size(); i++)
    {
        ret[i] = res[i];
    }
    *pnCount = res.size();
    return ret;
}

/***********************************************************************/
/*                   GDALMDArrayGetProcessingChunkSize()               */
/************************************************************************/

/** \brief Return an optimal chunk size for read/write operations, given the
 * natural block size and memory constraints specified.
 *
 * This method will use GetBlockSize() to define a chunk whose dimensions are
 * multiple of those returned by GetBlockSize() (unless the block define by
 * GetBlockSize() is larger than nMaxChunkMemory, in which case it will be
 * returned by this method).
 *
 * This is the same as the C++ method
 * GDALAbstractMDArray::GetProcessingChunkSize().
 *
 * @param hArray Array.
 * @param pnCount Pointer to the number of values returned. Must NOT be NULL.
 * @param nMaxChunkMemory Maximum amount of memory, in bytes, to use for the
 * chunk.
 *
 * @return the chunk size, in number of elements along each dimension.
 */

size_t *GDALMDArrayGetProcessingChunkSize(GDALMDArrayH hArray, size_t *pnCount,
                                          size_t nMaxChunkMemory)
{
    VALIDATE_POINTER1(hArray, __func__, nullptr);
    VALIDATE_POINTER1(pnCount, __func__, nullptr);
    auto res = hArray->m_poImpl->GetProcessingChunkSize(nMaxChunkMemory);
    auto ret = static_cast<size_t *>(CPLMalloc(sizeof(size_t) * res.size()));
    for (size_t i = 0; i < res.size(); i++)
    {
        ret[i] = res[i];
    }
    *pnCount = res.size();
    return ret;
}

/************************************************************************/
/*                     GDALMDArrayGetStructuralInfo()                   */
/************************************************************************/

/** Return structural information on the array.
 *
 * This may be the compression, etc..
 *
 * The return value should not be freed and is valid until GDALMDArray is
 * released or this function called again.
 *
 * This is the same as the C++ method GDALMDArray::GetStructuralInfo().
 */
CSLConstList GDALMDArrayGetStructuralInfo(GDALMDArrayH hArray)
{
    VALIDATE_POINTER1(hArray, __func__, nullptr);
    return hArray->m_poImpl->GetStructuralInfo();
}

/************************************************************************/
/*                        GDALMDArrayGetView()                          */
/************************************************************************/

/** Return a view of the array using slicing or field access.
 *
 * The returned object should be released with GDALMDArrayRelease().
 *
 * This is the same as the C++ method GDALMDArray::GetView().
 */
GDALMDArrayH GDALMDArrayGetView(GDALMDArrayH hArray, const char *pszViewExpr)
{
    VALIDATE_POINTER1(hArray, __func__, nullptr);
    VALIDATE_POINTER1(pszViewExpr, __func__, nullptr);
    auto sliced = hArray->m_poImpl->GetView(std::string(pszViewExpr));
    if (!sliced)
        return nullptr;
    return new GDALMDArrayHS(sliced);
}

/************************************************************************/
/*                       GDALMDArrayTranspose()                         */
/************************************************************************/

/** Return a view of the array whose axis have been reordered.
 *
 * The returned object should be released with GDALMDArrayRelease().
 *
 * This is the same as the C++ method GDALMDArray::Transpose().
 */
GDALMDArrayH GDALMDArrayTranspose(GDALMDArrayH hArray, size_t nNewAxisCount,
                                  const int *panMapNewAxisToOldAxis)
{
    VALIDATE_POINTER1(hArray, __func__, nullptr);
    std::vector<int> anMapNewAxisToOldAxis(nNewAxisCount);
    if (nNewAxisCount)
    {
        memcpy(&anMapNewAxisToOldAxis[0], panMapNewAxisToOldAxis,
               nNewAxisCount * sizeof(int));
    }
    auto reordered = hArray->m_poImpl->Transpose(anMapNewAxisToOldAxis);
    if (!reordered)
        return nullptr;
    return new GDALMDArrayHS(reordered);
}

/************************************************************************/
/*                      GDALMDArrayGetUnscaled()                        */
/************************************************************************/

/** Return an array that is the unscaled version of the current one.
 *
 * That is each value of the unscaled array will be
 * unscaled_value = raw_value * GetScale() + GetOffset()
 *
 * Starting with GDAL 3.3, the Write() method is implemented and will convert
 * from unscaled values to raw values.
 *
 * The returned object should be released with GDALMDArrayRelease().
 *
 * This is the same as the C++ method GDALMDArray::GetUnscaled().
 */
GDALMDArrayH GDALMDArrayGetUnscaled(GDALMDArrayH hArray)
{
    VALIDATE_POINTER1(hArray, __func__, nullptr);
    auto unscaled = hArray->m_poImpl->GetUnscaled();
    if (!unscaled)
        return nullptr;
    return new GDALMDArrayHS(unscaled);
}

/************************************************************************/
/*                          GDALMDArrayGetMask()                         */
/************************************************************************/

/** Return an array that is a mask for the current array
 *
 * This array will be of type Byte, with values set to 0 to indicate invalid
 * pixels of the current array, and values set to 1 to indicate valid pixels.
 *
 * The returned object should be released with GDALMDArrayRelease().
 *
 * This is the same as the C++ method GDALMDArray::GetMask().
 */
GDALMDArrayH GDALMDArrayGetMask(GDALMDArrayH hArray, CSLConstList papszOptions)
{
    VALIDATE_POINTER1(hArray, __func__, nullptr);
    auto unscaled = hArray->m_poImpl->GetMask(papszOptions);
    if (!unscaled)
        return nullptr;
    return new GDALMDArrayHS(unscaled);
}

/************************************************************************/
/*                   GDALMDArrayGetResampled()                          */
/************************************************************************/

/** Return an array that is a resampled / reprojected view of the current array
 *
 * This is the same as the C++ method GDALMDArray::GetResampled().
 *
 * Currently this method can only resample along the last 2 dimensions, unless
 * orthorectifying a NASA EMIT dataset.
 *
 * The returned object should be released with GDALMDArrayRelease().
 *
 * @since 3.4
 */
GDALMDArrayH GDALMDArrayGetResampled(GDALMDArrayH hArray, size_t nNewDimCount,
                                     const GDALDimensionH *pahNewDims,
                                     GDALRIOResampleAlg resampleAlg,
                                     OGRSpatialReferenceH hTargetSRS,
                                     CSLConstList papszOptions)
{
    VALIDATE_POINTER1(hArray, __func__, nullptr);
    VALIDATE_POINTER1(pahNewDims, __func__, nullptr);
    std::vector<std::shared_ptr<GDALDimension>> apoNewDims(nNewDimCount);
    for (size_t i = 0; i < nNewDimCount; ++i)
    {
        if (pahNewDims[i])
            apoNewDims[i] = pahNewDims[i]->m_poImpl;
    }
    auto poNewArray = hArray->m_poImpl->GetResampled(
        apoNewDims, resampleAlg, OGRSpatialReference::FromHandle(hTargetSRS),
        papszOptions);
    if (!poNewArray)
        return nullptr;
    return new GDALMDArrayHS(poNewArray);
}

/************************************************************************/
/*                      GDALMDArraySetUnit()                            */
/************************************************************************/

/** Set the variable unit.
 *
 * Values should conform as much as possible with those allowed by
 * the NetCDF CF conventions:
 * http://cfconventions.org/Data/cf-conventions/cf-conventions-1.7/cf-conventions.html#units
 * but others might be returned.
 *
 * Few examples are "meter", "degrees", "second", ...
 * Empty value means unknown.
 *
 * This is the same as the C function GDALMDArraySetUnit()
 *
 * @param hArray array.
 * @param pszUnit unit name.
 * @return TRUE in case of success.
 */
int GDALMDArraySetUnit(GDALMDArrayH hArray, const char *pszUnit)
{
    VALIDATE_POINTER1(hArray, __func__, FALSE);
    return hArray->m_poImpl->SetUnit(pszUnit ? pszUnit : "");
}

/************************************************************************/
/*                      GDALMDArrayGetUnit()                            */
/************************************************************************/

/** Return the array unit.
 *
 * Values should conform as much as possible with those allowed by
 * the NetCDF CF conventions:
 * http://cfconventions.org/Data/cf-conventions/cf-conventions-1.7/cf-conventions.html#units
 * but others might be returned.
 *
 * Few examples are "meter", "degrees", "second", ...
 * Empty value means unknown.
 *
 * The return value should not be freed and is valid until GDALMDArray is
 * released or this function called again.
 *
 * This is the same as the C++ method GDALMDArray::GetUnit().
 */
const char *GDALMDArrayGetUnit(GDALMDArrayH hArray)
{
    VALIDATE_POINTER1(hArray, __func__, nullptr);
    return hArray->m_poImpl->GetUnit().c_str();
}

/************************************************************************/
/*                      GDALMDArrayGetSpatialRef()                      */
/************************************************************************/

/** Assign a spatial reference system object to the array.
 *
 * This is the same as the C++ method GDALMDArray::SetSpatialRef().
 * @return TRUE in case of success.
 */
int GDALMDArraySetSpatialRef(GDALMDArrayH hArray, OGRSpatialReferenceH hSRS)
{
    VALIDATE_POINTER1(hArray, __func__, FALSE);
    return hArray->m_poImpl->SetSpatialRef(
        OGRSpatialReference::FromHandle(hSRS));
}

/************************************************************************/
/*                      GDALMDArrayGetSpatialRef()                      */
/************************************************************************/

/** Return the spatial reference system object associated with the array.
 *
 * This is the same as the C++ method GDALMDArray::GetSpatialRef().
 *
 * The returned object must be freed with OSRDestroySpatialReference().
 */
OGRSpatialReferenceH GDALMDArrayGetSpatialRef(GDALMDArrayH hArray)
{
    VALIDATE_POINTER1(hArray, __func__, nullptr);
    auto poSRS = hArray->m_poImpl->GetSpatialRef();
    return poSRS ? OGRSpatialReference::ToHandle(poSRS->Clone()) : nullptr;
}

/************************************************************************/
/*                      GDALMDArrayGetStatistics()                      */
/************************************************************************/

/**
 * \brief Fetch statistics.
 *
 * This is the same as the C++ method GDALMDArray::GetStatistics().
 *
 * @since GDAL 3.2
 */

CPLErr GDALMDArrayGetStatistics(GDALMDArrayH hArray, GDALDatasetH /*hDS*/,
                                int bApproxOK, int bForce, double *pdfMin,
                                double *pdfMax, double *pdfMean,
                                double *pdfStdDev, GUInt64 *pnValidCount,
                                GDALProgressFunc pfnProgress,
                                void *pProgressData)
{
    VALIDATE_POINTER1(hArray, __func__, CE_Failure);
    return hArray->m_poImpl->GetStatistics(
        CPL_TO_BOOL(bApproxOK), CPL_TO_BOOL(bForce), pdfMin, pdfMax, pdfMean,
        pdfStdDev, pnValidCount, pfnProgress, pProgressData);
}

/************************************************************************/
/*                      GDALMDArrayComputeStatistics()                  */
/************************************************************************/

/**
 * \brief Compute statistics.
 *
 * This is the same as the C++ method GDALMDArray::ComputeStatistics().
 *
 * @since GDAL 3.2
 * @see GDALMDArrayComputeStatisticsEx()
 */

int GDALMDArrayComputeStatistics(GDALMDArrayH hArray, GDALDatasetH /* hDS */,
                                 int bApproxOK, double *pdfMin, double *pdfMax,
                                 double *pdfMean, double *pdfStdDev,
                                 GUInt64 *pnValidCount,
                                 GDALProgressFunc pfnProgress,
                                 void *pProgressData)
{
    VALIDATE_POINTER1(hArray, __func__, FALSE);
    return hArray->m_poImpl->ComputeStatistics(
        CPL_TO_BOOL(bApproxOK), pdfMin, pdfMax, pdfMean, pdfStdDev,
        pnValidCount, pfnProgress, pProgressData, nullptr);
}

/************************************************************************/
/*                     GDALMDArrayComputeStatisticsEx()                 */
/************************************************************************/

/**
 * \brief Compute statistics.
 *
 * Same as GDALMDArrayComputeStatistics() with extra papszOptions argument.
 *
 * This is the same as the C++ method GDALMDArray::ComputeStatistics().
 *
 * @since GDAL 3.8
 */

int GDALMDArrayComputeStatisticsEx(GDALMDArrayH hArray, GDALDatasetH /* hDS */,
                                   int bApproxOK, double *pdfMin,
                                   double *pdfMax, double *pdfMean,
                                   double *pdfStdDev, GUInt64 *pnValidCount,
                                   GDALProgressFunc pfnProgress,
                                   void *pProgressData,
                                   CSLConstList papszOptions)
{
    VALIDATE_POINTER1(hArray, __func__, FALSE);
    return hArray->m_poImpl->ComputeStatistics(
        CPL_TO_BOOL(bApproxOK), pdfMin, pdfMax, pdfMean, pdfStdDev,
        pnValidCount, pfnProgress, pProgressData, papszOptions);
}

/************************************************************************/
/*                 GDALMDArrayGetCoordinateVariables()                  */
/************************************************************************/

/** Return coordinate variables.
 *
 * The returned array must be freed with GDALReleaseArrays(). If only the array
 * itself needs to be freed, CPLFree() should be called (and
 * GDALMDArrayRelease() on individual array members).
 *
 * This is the same as the C++ method GDALMDArray::GetCoordinateVariables()
 *
 * @param hArray Array.
 * @param pnCount Pointer to the number of values returned. Must NOT be NULL.
 *
 * @return an array of *pnCount arrays.
 * @since 3.4
 */
GDALMDArrayH *GDALMDArrayGetCoordinateVariables(GDALMDArrayH hArray,
                                                size_t *pnCount)
{
    VALIDATE_POINTER1(hArray, __func__, nullptr);
    VALIDATE_POINTER1(pnCount, __func__, nullptr);
    const auto coordinates(hArray->m_poImpl->GetCoordinateVariables());
    auto ret = static_cast<GDALMDArrayH *>(
        CPLMalloc(sizeof(GDALMDArrayH) * coordinates.size()));
    for (size_t i = 0; i < coordinates.size(); i++)
    {
        ret[i] = new GDALMDArrayHS(coordinates[i]);
    }
    *pnCount = coordinates.size();
    return ret;
}

/************************************************************************/
/*                     GDALMDArrayGetGridded()                          */
/************************************************************************/

/** Return a gridded array from scattered point data, that is from an array
 * whose last dimension is the indexing variable of X and Y arrays.
 *
 * The returned object should be released with GDALMDArrayRelease().
 *
 * This is the same as the C++ method GDALMDArray::GetGridded().
 *
 * @since GDAL 3.7
 */
GDALMDArrayH GDALMDArrayGetGridded(GDALMDArrayH hArray,
                                   const char *pszGridOptions,
                                   GDALMDArrayH hXArray, GDALMDArrayH hYArray,
                                   CSLConstList papszOptions)
{
    VALIDATE_POINTER1(hArray, __func__, nullptr);
    VALIDATE_POINTER1(pszGridOptions, __func__, nullptr);
    auto gridded = hArray->m_poImpl->GetGridded(
        pszGridOptions, hXArray ? hXArray->m_poImpl : nullptr,
        hYArray ? hYArray->m_poImpl : nullptr, papszOptions);
    if (!gridded)
        return nullptr;
    return new GDALMDArrayHS(gridded);
}

/************************************************************************/
/*                      GDALMDArrayGetMeshGrid()                        */
/************************************************************************/

/** Return a list of multidimensional arrays from a list of one-dimensional
 * arrays.
 *
 * This is typically used to transform one-dimensional longitude, latitude
 * arrays into 2D ones.
 *
 * More formally, for one-dimensional arrays x1, x2,..., xn with lengths
 * Ni=len(xi), returns (N1, N2, ..., Nn) shaped arrays if indexing="ij" or
 * (N2, N1, ..., Nn) shaped arrays if indexing="xy" with the elements of xi
 * repeated to fill the matrix along the first dimension for x1, the second
 * for x2 and so on.
 *
 * For example, if x = [1, 2], and y = [3, 4, 5],
 * GetMeshGrid([x, y], ["INDEXING=xy"]) will return [xm, ym] such that
 * xm=[[1, 2],[1, 2],[1, 2]] and ym=[[3, 3],[4, 4],[5, 5]],
 * or more generally xm[any index][i] = x[i] and ym[i][any index]=y[i]
 *
 * and
 * GetMeshGrid([x, y], ["INDEXING=ij"]) will return [xm, ym] such that
 * xm=[[1, 1, 1],[2, 2, 2]] and ym=[[3, 4, 5],[3, 4, 5]],
 * or more generally xm[i][any index] = x[i] and ym[any index][i]=y[i]
 *
 * The currently supported options are:
 * <ul>
 * <li>INDEXING=xy/ij: Cartesian ("xy", default) or matrix ("ij") indexing of
 * output.
 * </li>
 * </ul>
 *
 * This is the same as
 * <a href="https://numpy.org/doc/stable/reference/generated/numpy.meshgrid.html">numpy.meshgrid()</a>
 * function.
 *
 * The returned array (of arrays) must be freed with GDALReleaseArrays().
 * If only the array itself needs to be freed, CPLFree() should be called
 * (and GDALMDArrayRelease() on individual array members).
 *
 * This is the same as the C++ method GDALMDArray::GetMeshGrid()
 *
 * @param pahInputArrays Input arrays
 * @param nCountInputArrays Number of input arrays
 * @param pnCountOutputArrays Pointer to the number of values returned. Must NOT be NULL.
 * @param papszOptions NULL, or NULL terminated list of options.
 *
 * @return an array of *pnCountOutputArrays arrays.
 * @since 3.10
 */
GDALMDArrayH *GDALMDArrayGetMeshGrid(const GDALMDArrayH *pahInputArrays,
                                     size_t nCountInputArrays,
                                     size_t *pnCountOutputArrays,
                                     CSLConstList papszOptions)
{
    VALIDATE_POINTER1(pahInputArrays, __func__, nullptr);
    VALIDATE_POINTER1(pnCountOutputArrays, __func__, nullptr);

    std::vector<std::shared_ptr<GDALMDArray>> apoInputArrays;
    for (size_t i = 0; i < nCountInputArrays; ++i)
        apoInputArrays.push_back(pahInputArrays[i]->m_poImpl);

    const auto apoOutputArrays =
        GDALMDArray::GetMeshGrid(apoInputArrays, papszOptions);
    auto ret = static_cast<GDALMDArrayH *>(
        CPLMalloc(sizeof(GDALMDArrayH) * apoOutputArrays.size()));
    for (size_t i = 0; i < apoOutputArrays.size(); i++)
    {
        ret[i] = new GDALMDArrayHS(apoOutputArrays[i]);
    }
    *pnCountOutputArrays = apoOutputArrays.size();
    return ret;
}

/************************************************************************/
/*                        GDALReleaseArrays()                           */
/************************************************************************/

/** Free the return of GDALMDArrayGetCoordinateVariables()
 *
 * @param arrays return pointer of above methods
 * @param nCount *pnCount value returned by above methods
 */
void GDALReleaseArrays(GDALMDArrayH *arrays, size_t nCount)
{
    for (size_t i = 0; i < nCount; i++)
    {
        delete arrays[i];
    }
    CPLFree(arrays);
}

/************************************************************************/
/*                           GDALMDArrayCache()                         */
/************************************************************************/

/**
 * \brief Cache the content of the array into an auxiliary filename.
 *
 * This is the same as the C++ method GDALMDArray::Cache().
 *
 * @since GDAL 3.4
 */

int GDALMDArrayCache(GDALMDArrayH hArray, CSLConstList papszOptions)
{
    VALIDATE_POINTER1(hArray, __func__, FALSE);
    return hArray->m_poImpl->Cache(papszOptions);
}

/************************************************************************/
/*                       GDALMDArrayRename()                           */
/************************************************************************/

/** Rename the array.
 *
 * This is not implemented by all drivers.
 *
 * Drivers known to implement it: MEM, netCDF, Zarr.
 *
 * This is the same as the C++ method GDALAbstractMDArray::Rename()
 *
 * @return true in case of success
 * @since GDAL 3.8
 */
bool GDALMDArrayRename(GDALMDArrayH hArray, const char *pszNewName)
{
    VALIDATE_POINTER1(hArray, __func__, false);
    VALIDATE_POINTER1(pszNewName, __func__, false);
    return hArray->m_poImpl->Rename(pszNewName);
}

/************************************************************************/
/*                        GDALAttributeRelease()                        */
/************************************************************************/

/** Release the GDAL in-memory object associated with a GDALAttribute.
 *
 * Note: when applied on a object coming from a driver, this does not
 * destroy the object in the file, database, etc...
 */
void GDALAttributeRelease(GDALAttributeH hAttr)
{
    delete hAttr;
}

/************************************************************************/
/*                        GDALAttributeGetName()                        */
/************************************************************************/

/** Return the name of the attribute.
 *
 * The returned pointer is valid until hAttr is released.
 *
 * This is the same as the C++ method GDALAttribute::GetName().
 */
const char *GDALAttributeGetName(GDALAttributeH hAttr)
{
    VALIDATE_POINTER1(hAttr, __func__, nullptr);
    return hAttr->m_poImpl->GetName().c_str();
}

/************************************************************************/
/*                      GDALAttributeGetFullName()                      */
/************************************************************************/

/** Return the full name of the attribute.
 *
 * The returned pointer is valid until hAttr is released.
 *
 * This is the same as the C++ method GDALAttribute::GetFullName().
 */
const char *GDALAttributeGetFullName(GDALAttributeH hAttr)
{
    VALIDATE_POINTER1(hAttr, __func__, nullptr);
    return hAttr->m_poImpl->GetFullName().c_str();
}

/************************************************************************/
/*                   GDALAttributeGetTotalElementsCount()               */
/************************************************************************/

/** Return the total number of values in the attribute.
 *
 * This is the same as the C++ method
 * GDALAbstractMDArray::GetTotalElementsCount()
 */
GUInt64 GDALAttributeGetTotalElementsCount(GDALAttributeH hAttr)
{
    VALIDATE_POINTER1(hAttr, __func__, 0);
    return hAttr->m_poImpl->GetTotalElementsCount();
}

/************************************************************************/
/*                    GDALAttributeGetDimensionCount()                */
/************************************************************************/

/** Return the number of dimensions.
 *
 * This is the same as the C++ method GDALAbstractMDArray::GetDimensionCount()
 */
size_t GDALAttributeGetDimensionCount(GDALAttributeH hAttr)
{
    VALIDATE_POINTER1(hAttr, __func__, 0);
    return hAttr->m_poImpl->GetDimensionCount();
}

/************************************************************************/
/*                       GDALAttributeGetDimensionsSize()                */
/************************************************************************/

/** Return the dimension sizes of the attribute.
 *
 * The returned array must be freed with CPLFree()
 *
 * @param hAttr Attribute.
 * @param pnCount Pointer to the number of values returned. Must NOT be NULL.
 *
 * @return an array of *pnCount values.
 */
GUInt64 *GDALAttributeGetDimensionsSize(GDALAttributeH hAttr, size_t *pnCount)
{
    VALIDATE_POINTER1(hAttr, __func__, nullptr);
    VALIDATE_POINTER1(pnCount, __func__, nullptr);
    const auto &dims = hAttr->m_poImpl->GetDimensions();
    auto ret = static_cast<GUInt64 *>(CPLMalloc(sizeof(GUInt64) * dims.size()));
    for (size_t i = 0; i < dims.size(); i++)
    {
        ret[i] = dims[i]->GetSize();
    }
    *pnCount = dims.size();
    return ret;
}

/************************************************************************/
/*                       GDALAttributeGetDataType()                     */
/************************************************************************/

/** Return the data type
 *
 * The return must be freed with GDALExtendedDataTypeRelease().
 */
GDALExtendedDataTypeH GDALAttributeGetDataType(GDALAttributeH hAttr)
{
    VALIDATE_POINTER1(hAttr, __func__, nullptr);
    return new GDALExtendedDataTypeHS(
        new GDALExtendedDataType(hAttr->m_poImpl->GetDataType()));
}

/************************************************************************/
/*                       GDALAttributeReadAsRaw()                       */
/************************************************************************/

/** Return the raw value of an attribute.
 *
 * This is the same as the C++ method GDALAttribute::ReadAsRaw().
 *
 * The returned buffer must be freed with GDALAttributeFreeRawResult()
 *
 * @param hAttr Attribute.
 * @param pnSize Pointer to the number of bytes returned. Must NOT be NULL.
 *
 * @return a buffer of *pnSize bytes.
 */
GByte *GDALAttributeReadAsRaw(GDALAttributeH hAttr, size_t *pnSize)
{
    VALIDATE_POINTER1(hAttr, __func__, nullptr);
    VALIDATE_POINTER1(pnSize, __func__, nullptr);
    auto res(hAttr->m_poImpl->ReadAsRaw());
    *pnSize = res.size();
    auto ret = res.StealData();
    if (!ret)
    {
        *pnSize = 0;
        return nullptr;
    }
    return ret;
}

/************************************************************************/
/*                       GDALAttributeFreeRawResult()                   */
/************************************************************************/

/** Free the return of GDALAttributeAsRaw()
 */
void GDALAttributeFreeRawResult(GDALAttributeH hAttr, GByte *raw,
                                CPL_UNUSED size_t nSize)
{
    VALIDATE_POINTER0(hAttr, __func__);
    if (raw)
    {
        const auto &dt(hAttr->m_poImpl->GetDataType());
        const auto nDTSize(dt.GetSize());
        GByte *pabyPtr = raw;
        const auto nEltCount(hAttr->m_poImpl->GetTotalElementsCount());
        CPLAssert(nSize == nDTSize * nEltCount);
        for (size_t i = 0; i < nEltCount; ++i)
        {
            dt.FreeDynamicMemory(pabyPtr);
            pabyPtr += nDTSize;
        }
        CPLFree(raw);
    }
}

/************************************************************************/
/*                       GDALAttributeReadAsString()                    */
/************************************************************************/

/** Return the value of an attribute as a string.
 *
 * The returned string should not be freed, and its lifetime does not
 * excess a next call to ReadAsString() on the same object, or the deletion
 * of the object itself.
 *
 * This function will only return the first element if there are several.
 *
 * This is the same as the C++ method GDALAttribute::ReadAsString()
 *
 * @return a string, or nullptr.
 */
const char *GDALAttributeReadAsString(GDALAttributeH hAttr)
{
    VALIDATE_POINTER1(hAttr, __func__, nullptr);
    return hAttr->m_poImpl->ReadAsString();
}

/************************************************************************/
/*                      GDALAttributeReadAsInt()                        */
/************************************************************************/

/** Return the value of an attribute as a integer.
 *
 * This function will only return the first element if there are several.
 *
 * It can fail if its value can not be converted to integer.
 *
 * This is the same as the C++ method GDALAttribute::ReadAsInt()
 *
 * @return a integer, or INT_MIN in case of error.
 */
int GDALAttributeReadAsInt(GDALAttributeH hAttr)
{
    VALIDATE_POINTER1(hAttr, __func__, 0);
    return hAttr->m_poImpl->ReadAsInt();
}

/************************************************************************/
/*                      GDALAttributeReadAsInt64()                      */
/************************************************************************/

/** Return the value of an attribute as a int64_t.
 *
 * This function will only return the first element if there are several.
 *
 * It can fail if its value can not be converted to integer.
 *
 * This is the same as the C++ method GDALAttribute::ReadAsInt64()
 *
 * @return an int64_t, or INT64_MIN in case of error.
 */
int64_t GDALAttributeReadAsInt64(GDALAttributeH hAttr)
{
    VALIDATE_POINTER1(hAttr, __func__, 0);
    return hAttr->m_poImpl->ReadAsInt64();
}

/************************************************************************/
/*                       GDALAttributeReadAsDouble()                    */
/************************************************************************/

/** Return the value of an attribute as a double.
 *
 * This function will only return the first element if there are several.
 *
 * It can fail if its value can not be converted to double.
 *
 * This is the same as the C++ method GDALAttribute::ReadAsDouble()
 *
 * @return a double value.
 */
double GDALAttributeReadAsDouble(GDALAttributeH hAttr)
{
    VALIDATE_POINTER1(hAttr, __func__, 0);
    return hAttr->m_poImpl->ReadAsDouble();
}

/************************************************************************/
/*                     GDALAttributeReadAsStringArray()                 */
/************************************************************************/

/** Return the value of an attribute as an array of strings.
 *
 * This is the same as the C++ method GDALAttribute::ReadAsStringArray()
 *
 * The return value must be freed with CSLDestroy().
 */
char **GDALAttributeReadAsStringArray(GDALAttributeH hAttr)
{
    VALIDATE_POINTER1(hAttr, __func__, nullptr);
    return hAttr->m_poImpl->ReadAsStringArray().StealList();
}

/************************************************************************/
/*                     GDALAttributeReadAsIntArray()                    */
/************************************************************************/

/** Return the value of an attribute as an array of integers.
 *
 * This is the same as the C++ method GDALAttribute::ReadAsIntArray()
 *
 * @param hAttr Attribute
 * @param pnCount Pointer to the number of values returned. Must NOT be NULL.
 * @return array to be freed with CPLFree(), or nullptr.
 */
int *GDALAttributeReadAsIntArray(GDALAttributeH hAttr, size_t *pnCount)
{
    VALIDATE_POINTER1(hAttr, __func__, nullptr);
    VALIDATE_POINTER1(pnCount, __func__, nullptr);
    *pnCount = 0;
    auto tmp(hAttr->m_poImpl->ReadAsIntArray());
    if (tmp.empty())
        return nullptr;
    auto ret = static_cast<int *>(VSI_MALLOC2_VERBOSE(tmp.size(), sizeof(int)));
    if (!ret)
        return nullptr;
    memcpy(ret, tmp.data(), tmp.size() * sizeof(int));
    *pnCount = tmp.size();
    return ret;
}

/************************************************************************/
/*                     GDALAttributeReadAsInt64Array()                  */
/************************************************************************/

/** Return the value of an attribute as an array of int64_t.
 *
 * This is the same as the C++ method GDALAttribute::ReadAsInt64Array()
 *
 * @param hAttr Attribute
 * @param pnCount Pointer to the number of values returned. Must NOT be NULL.
 * @return array to be freed with CPLFree(), or nullptr.
 */
int64_t *GDALAttributeReadAsInt64Array(GDALAttributeH hAttr, size_t *pnCount)
{
    VALIDATE_POINTER1(hAttr, __func__, nullptr);
    VALIDATE_POINTER1(pnCount, __func__, nullptr);
    *pnCount = 0;
    auto tmp(hAttr->m_poImpl->ReadAsInt64Array());
    if (tmp.empty())
        return nullptr;
    auto ret = static_cast<int64_t *>(
        VSI_MALLOC2_VERBOSE(tmp.size(), sizeof(int64_t)));
    if (!ret)
        return nullptr;
    memcpy(ret, tmp.data(), tmp.size() * sizeof(int64_t));
    *pnCount = tmp.size();
    return ret;
}

/************************************************************************/
/*                     GDALAttributeReadAsDoubleArray()                 */
/************************************************************************/

/** Return the value of an attribute as an array of doubles.
 *
 * This is the same as the C++ method GDALAttribute::ReadAsDoubleArray()
 *
 * @param hAttr Attribute
 * @param pnCount Pointer to the number of values returned. Must NOT be NULL.
 * @return array to be freed with CPLFree(), or nullptr.
 */
double *GDALAttributeReadAsDoubleArray(GDALAttributeH hAttr, size_t *pnCount)
{
    VALIDATE_POINTER1(hAttr, __func__, nullptr);
    VALIDATE_POINTER1(pnCount, __func__, nullptr);
    *pnCount = 0;
    auto tmp(hAttr->m_poImpl->ReadAsDoubleArray());
    if (tmp.empty())
        return nullptr;
    auto ret =
        static_cast<double *>(VSI_MALLOC2_VERBOSE(tmp.size(), sizeof(double)));
    if (!ret)
        return nullptr;
    memcpy(ret, tmp.data(), tmp.size() * sizeof(double));
    *pnCount = tmp.size();
    return ret;
}

/************************************************************************/
/*                     GDALAttributeWriteRaw()                          */
/************************************************************************/

/** Write an attribute from raw values expressed in GetDataType()
 *
 * The values should be provided in the type of GetDataType() and there should
 * be exactly GetTotalElementsCount() of them.
 * If GetDataType() is a string, each value should be a char* pointer.
 *
 * This is the same as the C++ method GDALAttribute::Write(const void*, size_t).
 *
 * @param hAttr Attribute
 * @param pabyValue Buffer of nLen bytes.
 * @param nLength Size of pabyValue in bytes. Should be equal to
 *             GetTotalElementsCount() * GetDataType().GetSize()
 * @return TRUE in case of success.
 */
int GDALAttributeWriteRaw(GDALAttributeH hAttr, const void *pabyValue,
                          size_t nLength)
{
    VALIDATE_POINTER1(hAttr, __func__, FALSE);
    return hAttr->m_poImpl->Write(pabyValue, nLength);
}

/************************************************************************/
/*                     GDALAttributeWriteString()                       */
/************************************************************************/

/** Write an attribute from a string value.
 *
 * Type conversion will be performed if needed. If the attribute contains
 * multiple values, only the first one will be updated.
 *
 * This is the same as the C++ method GDALAttribute::Write(const char*)
 *
 * @param hAttr Attribute
 * @param pszVal Pointer to a string.
 * @return TRUE in case of success.
 */
int GDALAttributeWriteString(GDALAttributeH hAttr, const char *pszVal)
{
    VALIDATE_POINTER1(hAttr, __func__, FALSE);
    return hAttr->m_poImpl->Write(pszVal);
}

/************************************************************************/
/*                        GDALAttributeWriteInt()                       */
/************************************************************************/

/** Write an attribute from a integer value.
 *
 * Type conversion will be performed if needed. If the attribute contains
 * multiple values, only the first one will be updated.
 *
 * This is the same as the C++ method GDALAttribute::WriteInt()
 *
 * @param hAttr Attribute
 * @param nVal Value.
 * @return TRUE in case of success.
 */
int GDALAttributeWriteInt(GDALAttributeH hAttr, int nVal)
{
    VALIDATE_POINTER1(hAttr, __func__, FALSE);
    return hAttr->m_poImpl->WriteInt(nVal);
}

/************************************************************************/
/*                        GDALAttributeWriteInt64()                     */
/************************************************************************/

/** Write an attribute from an int64_t value.
 *
 * Type conversion will be performed if needed. If the attribute contains
 * multiple values, only the first one will be updated.
 *
 * This is the same as the C++ method GDALAttribute::WriteLong()
 *
 * @param hAttr Attribute
 * @param nVal Value.
 * @return TRUE in case of success.
 */
int GDALAttributeWriteInt64(GDALAttributeH hAttr, int64_t nVal)
{
    VALIDATE_POINTER1(hAttr, __func__, FALSE);
    return hAttr->m_poImpl->WriteInt64(nVal);
}

/************************************************************************/
/*                        GDALAttributeWriteDouble()                    */
/************************************************************************/

/** Write an attribute from a double value.
 *
 * Type conversion will be performed if needed. If the attribute contains
 * multiple values, only the first one will be updated.
 *
 * This is the same as the C++ method GDALAttribute::Write(double);
 *
 * @param hAttr Attribute
 * @param dfVal Value.
 *
 * @return TRUE in case of success.
 */
int GDALAttributeWriteDouble(GDALAttributeH hAttr, double dfVal)
{
    VALIDATE_POINTER1(hAttr, __func__, FALSE);
    return hAttr->m_poImpl->Write(dfVal);
}

/************************************************************************/
/*                       GDALAttributeWriteStringArray()                */
/************************************************************************/

/** Write an attribute from an array of strings.
 *
 * Type conversion will be performed if needed.
 *
 * Exactly GetTotalElementsCount() strings must be provided
 *
 * This is the same as the C++ method GDALAttribute::Write(CSLConstList)
 *
 * @param hAttr Attribute
 * @param papszValues Array of strings.
 * @return TRUE in case of success.
 */
int GDALAttributeWriteStringArray(GDALAttributeH hAttr,
                                  CSLConstList papszValues)
{
    VALIDATE_POINTER1(hAttr, __func__, FALSE);
    return hAttr->m_poImpl->Write(papszValues);
}

/************************************************************************/
/*                       GDALAttributeWriteIntArray()                */
/************************************************************************/

/** Write an attribute from an array of int.
 *
 * Type conversion will be performed if needed.
 *
 * Exactly GetTotalElementsCount() strings must be provided
 *
 * This is the same as the C++ method GDALAttribute::Write(const int *,
 * size_t)
 *
 * @param hAttr Attribute
 * @param panValues Array of int.
 * @param nCount Should be equal to GetTotalElementsCount().
 * @return TRUE in case of success.
 */
int GDALAttributeWriteIntArray(GDALAttributeH hAttr, const int *panValues,
                               size_t nCount)
{
    VALIDATE_POINTER1(hAttr, __func__, FALSE);
    return hAttr->m_poImpl->Write(panValues, nCount);
}

/************************************************************************/
/*                       GDALAttributeWriteInt64Array()                 */
/************************************************************************/

/** Write an attribute from an array of int64_t.
 *
 * Type conversion will be performed if needed.
 *
 * Exactly GetTotalElementsCount() strings must be provided
 *
 * This is the same as the C++ method GDALAttribute::Write(const int64_t *,
 * size_t)
 *
 * @param hAttr Attribute
 * @param panValues Array of int64_t.
 * @param nCount Should be equal to GetTotalElementsCount().
 * @return TRUE in case of success.
 */
int GDALAttributeWriteInt64Array(GDALAttributeH hAttr, const int64_t *panValues,
                                 size_t nCount)
{
    VALIDATE_POINTER1(hAttr, __func__, FALSE);
    return hAttr->m_poImpl->Write(panValues, nCount);
}

/************************************************************************/
/*                       GDALAttributeWriteDoubleArray()                */
/************************************************************************/

/** Write an attribute from an array of double.
 *
 * Type conversion will be performed if needed.
 *
 * Exactly GetTotalElementsCount() strings must be provided
 *
 * This is the same as the C++ method GDALAttribute::Write(const double *,
 * size_t)
 *
 * @param hAttr Attribute
 * @param padfValues Array of double.
 * @param nCount Should be equal to GetTotalElementsCount().
 * @return TRUE in case of success.
 */
int GDALAttributeWriteDoubleArray(GDALAttributeH hAttr,
                                  const double *padfValues, size_t nCount)
{
    VALIDATE_POINTER1(hAttr, __func__, FALSE);
    return hAttr->m_poImpl->Write(padfValues, nCount);
}

/************************************************************************/
/*                      GDALAttributeRename()                           */
/************************************************************************/

/** Rename the attribute.
 *
 * This is not implemented by all drivers.
 *
 * Drivers known to implement it: MEM, netCDF.
 *
 * This is the same as the C++ method GDALAbstractMDArray::Rename()
 *
 * @return true in case of success
 * @since GDAL 3.8
 */
bool GDALAttributeRename(GDALAttributeH hAttr, const char *pszNewName)
{
    VALIDATE_POINTER1(hAttr, __func__, false);
    VALIDATE_POINTER1(pszNewName, __func__, false);
    return hAttr->m_poImpl->Rename(pszNewName);
}

/************************************************************************/
/*                        GDALDimensionRelease()                        */
/************************************************************************/

/** Release the GDAL in-memory object associated with a GDALDimension.
 *
 * Note: when applied on a object coming from a driver, this does not
 * destroy the object in the file, database, etc...
 */
void GDALDimensionRelease(GDALDimensionH hDim)
{
    delete hDim;
}

/************************************************************************/
/*                        GDALDimensionGetName()                        */
/************************************************************************/

/** Return dimension name.
 *
 * This is the same as the C++ method GDALDimension::GetName()
 */
const char *GDALDimensionGetName(GDALDimensionH hDim)
{
    VALIDATE_POINTER1(hDim, __func__, nullptr);
    return hDim->m_poImpl->GetName().c_str();
}

/************************************************************************/
/*                      GDALDimensionGetFullName()                      */
/************************************************************************/

/** Return dimension full name.
 *
 * This is the same as the C++ method GDALDimension::GetFullName()
 */
const char *GDALDimensionGetFullName(GDALDimensionH hDim)
{
    VALIDATE_POINTER1(hDim, __func__, nullptr);
    return hDim->m_poImpl->GetFullName().c_str();
}

/************************************************************************/
/*                        GDALDimensionGetType()                        */
/************************************************************************/

/** Return dimension type.
 *
 * This is the same as the C++ method GDALDimension::GetType()
 */
const char *GDALDimensionGetType(GDALDimensionH hDim)
{
    VALIDATE_POINTER1(hDim, __func__, nullptr);
    return hDim->m_poImpl->GetType().c_str();
}

/************************************************************************/
/*                     GDALDimensionGetDirection()                      */
/************************************************************************/

/** Return dimension direction.
 *
 * This is the same as the C++ method GDALDimension::GetDirection()
 */
const char *GDALDimensionGetDirection(GDALDimensionH hDim)
{
    VALIDATE_POINTER1(hDim, __func__, nullptr);
    return hDim->m_poImpl->GetDirection().c_str();
}

/************************************************************************/
/*                        GDALDimensionGetSize()                        */
/************************************************************************/

/** Return the size, that is the number of values along the dimension.
 *
 * This is the same as the C++ method GDALDimension::GetSize()
 */
GUInt64 GDALDimensionGetSize(GDALDimensionH hDim)
{
    VALIDATE_POINTER1(hDim, __func__, 0);
    return hDim->m_poImpl->GetSize();
}

/************************************************************************/
/*                     GDALDimensionGetIndexingVariable()               */
/************************************************************************/

/** Return the variable that is used to index the dimension (if there is one).
 *
 * This is the array, typically one-dimensional, describing the values taken
 * by the dimension.
 *
 * The returned value should be freed with GDALMDArrayRelease().
 *
 * This is the same as the C++ method GDALDimension::GetIndexingVariable()
 */
GDALMDArrayH GDALDimensionGetIndexingVariable(GDALDimensionH hDim)
{
    VALIDATE_POINTER1(hDim, __func__, nullptr);
    auto var(hDim->m_poImpl->GetIndexingVariable());
    if (!var)
        return nullptr;
    return new GDALMDArrayHS(var);
}

/************************************************************************/
/*                      GDALDimensionSetIndexingVariable()              */
/************************************************************************/

/** Set the variable that is used to index the dimension.
 *
 * This is the array, typically one-dimensional, describing the values taken
 * by the dimension.
 *
 * This is the same as the C++ method GDALDimension::SetIndexingVariable()
 *
 * @return TRUE in case of success.
 */
int GDALDimensionSetIndexingVariable(GDALDimensionH hDim, GDALMDArrayH hArray)
{
    VALIDATE_POINTER1(hDim, __func__, FALSE);
    return hDim->m_poImpl->SetIndexingVariable(hArray ? hArray->m_poImpl
                                                      : nullptr);
}

/************************************************************************/
/*                      GDALDimensionRename()                           */
/************************************************************************/

/** Rename the dimension.
 *
 * This is not implemented by all drivers.
 *
 * Drivers known to implement it: MEM, netCDF.
 *
 * This is the same as the C++ method GDALDimension::Rename()
 *
 * @return true in case of success
 * @since GDAL 3.8
 */
bool GDALDimensionRename(GDALDimensionH hDim, const char *pszNewName)
{
    VALIDATE_POINTER1(hDim, __func__, false);
    VALIDATE_POINTER1(pszNewName, __func__, false);
    return hDim->m_poImpl->Rename(pszNewName);
}

/************************************************************************/
/*                       GDALDatasetGetRootGroup()                      */
/************************************************************************/

/** Return the root GDALGroup of this dataset.
 *
 * Only valid for multidimensional datasets.
 *
 * The returned value must be freed with GDALGroupRelease().
 *
 * This is the same as the C++ method GDALDataset::GetRootGroup().
 *
 * @since GDAL 3.1
 */
GDALGroupH GDALDatasetGetRootGroup(GDALDatasetH hDS)
{
    VALIDATE_POINTER1(hDS, __func__, nullptr);
    auto poGroup(GDALDataset::FromHandle(hDS)->GetRootGroup());
    return poGroup ? new GDALGroupHS(poGroup) : nullptr;
}

/************************************************************************/
/*                      GDALRasterBandAsMDArray()                        */
/************************************************************************/

/** Return a view of this raster band as a 2D multidimensional GDALMDArray.
 *
 * The band must be linked to a GDALDataset. If this dataset is not already
 * marked as shared, it will be, so that the returned array holds a reference
 * to it.
 *
 * If the dataset has a geotransform attached, the X and Y dimensions of the
 * returned array will have an associated indexing variable.
 *
 * The returned pointer must be released with GDALMDArrayRelease().
 *
 * This is the same as the C++ method GDALRasterBand::AsMDArray().
 *
 * @return a new array, or NULL.
 *
 * @since GDAL 3.1
 */
GDALMDArrayH GDALRasterBandAsMDArray(GDALRasterBandH hBand)
{
    VALIDATE_POINTER1(hBand, __func__, nullptr);
    auto poArray(GDALRasterBand::FromHandle(hBand)->AsMDArray());
    if (!poArray)
        return nullptr;
    return new GDALMDArrayHS(poArray);
}

/************************************************************************/
/*                       GDALMDArrayAsClassicDataset()                  */
/************************************************************************/

/** Return a view of this array as a "classic" GDALDataset (ie 2D)
 *
 * Only 2D or more arrays are supported.
 *
 * In the case of > 2D arrays, additional dimensions will be represented as
 * raster bands.
 *
 * The "reverse" method is GDALRasterBand::AsMDArray().
 *
 * This is the same as the C++ method GDALMDArray::AsClassicDataset().
 *
 * @param hArray Array.
 * @param iXDim Index of the dimension that will be used as the X/width axis.
 * @param iYDim Index of the dimension that will be used as the Y/height axis.
 * @return a new GDALDataset that must be freed with GDALClose(), or nullptr
 */
GDALDatasetH GDALMDArrayAsClassicDataset(GDALMDArrayH hArray, size_t iXDim,
                                         size_t iYDim)
{
    VALIDATE_POINTER1(hArray, __func__, nullptr);
    return GDALDataset::ToHandle(
        hArray->m_poImpl->AsClassicDataset(iXDim, iYDim));
}

/************************************************************************/
/*                     GDALMDArrayAsClassicDatasetEx()                  */
/************************************************************************/

/** Return a view of this array as a "classic" GDALDataset (ie 2D)
 *
 * Only 2D or more arrays are supported.
 *
 * In the case of > 2D arrays, additional dimensions will be represented as
 * raster bands.
 *
 * The "reverse" method is GDALRasterBand::AsMDArray().
 *
 * This is the same as the C++ method GDALMDArray::AsClassicDataset().
 * @param hArray Array.
 * @param iXDim Index of the dimension that will be used as the X/width axis.
 * @param iYDim Index of the dimension that will be used as the Y/height axis.
 *              Ignored if the dimension count is 1.
 * @param hRootGroup Root group, or NULL. Used with the BAND_METADATA and
 *                   BAND_IMAGERY_METADATA option.
 * @param papszOptions Cf GDALMDArray::AsClassicDataset()
 * @return a new GDALDataset that must be freed with GDALClose(), or nullptr
 * @since GDAL 3.8
 */
GDALDatasetH GDALMDArrayAsClassicDatasetEx(GDALMDArrayH hArray, size_t iXDim,
                                           size_t iYDim, GDALGroupH hRootGroup,
                                           CSLConstList papszOptions)
{
    VALIDATE_POINTER1(hArray, __func__, nullptr);
    return GDALDataset::ToHandle(hArray->m_poImpl->AsClassicDataset(
        iXDim, iYDim, hRootGroup ? hRootGroup->m_poImpl : nullptr,
        papszOptions));
}

//! @cond Doxygen_Suppress

GDALAttributeString::GDALAttributeString(const std::string &osParentName,
                                         const std::string &osName,
                                         const std::string &osValue,
                                         GDALExtendedDataTypeSubType eSubType)
    : GDALAbstractMDArray(osParentName, osName),
      GDALAttribute(osParentName, osName),
      m_dt(GDALExtendedDataType::CreateString(0, eSubType)), m_osValue(osValue)
{
}

const std::vector<std::shared_ptr<GDALDimension>> &
GDALAttributeString::GetDimensions() const
{
    return m_dims;
}

const GDALExtendedDataType &GDALAttributeString::GetDataType() const
{
    return m_dt;
}

bool GDALAttributeString::IRead(const GUInt64 *, const size_t *, const GInt64 *,
                                const GPtrDiff_t *,
                                const GDALExtendedDataType &bufferDataType,
                                void *pDstBuffer) const
{
    if (bufferDataType.GetClass() != GEDTC_STRING)
        return false;
    char *pszStr = static_cast<char *>(VSIMalloc(m_osValue.size() + 1));
    if (!pszStr)
        return false;
    memcpy(pszStr, m_osValue.c_str(), m_osValue.size() + 1);
    *static_cast<char **>(pDstBuffer) = pszStr;
    return true;
}

GDALAttributeNumeric::GDALAttributeNumeric(const std::string &osParentName,
                                           const std::string &osName,
                                           double dfValue)
    : GDALAbstractMDArray(osParentName, osName),
      GDALAttribute(osParentName, osName),
      m_dt(GDALExtendedDataType::Create(GDT_Float64)), m_dfValue(dfValue)
{
}

GDALAttributeNumeric::GDALAttributeNumeric(const std::string &osParentName,
                                           const std::string &osName,
                                           int nValue)
    : GDALAbstractMDArray(osParentName, osName),
      GDALAttribute(osParentName, osName),
      m_dt(GDALExtendedDataType::Create(GDT_Int32)), m_nValue(nValue)
{
}

GDALAttributeNumeric::GDALAttributeNumeric(const std::string &osParentName,
                                           const std::string &osName,
                                           const std::vector<GUInt32> &anValues)
    : GDALAbstractMDArray(osParentName, osName),
      GDALAttribute(osParentName, osName),
      m_dt(GDALExtendedDataType::Create(GDT_UInt32)), m_anValuesUInt32(anValues)
{
    m_dims.push_back(std::make_shared<GDALDimension>(
        std::string(), "dim0", std::string(), std::string(),
        m_anValuesUInt32.size()));
}

const std::vector<std::shared_ptr<GDALDimension>> &
GDALAttributeNumeric::GetDimensions() const
{
    return m_dims;
}

const GDALExtendedDataType &GDALAttributeNumeric::GetDataType() const
{
    return m_dt;
}

bool GDALAttributeNumeric::IRead(const GUInt64 *arrayStartIdx,
                                 const size_t *count, const GInt64 *arrayStep,
                                 const GPtrDiff_t *bufferStride,
                                 const GDALExtendedDataType &bufferDataType,
                                 void *pDstBuffer) const
{
    if (m_dims.empty())
    {
        if (m_dt.GetNumericDataType() == GDT_Float64)
            GDALExtendedDataType::CopyValue(&m_dfValue, m_dt, pDstBuffer,
                                            bufferDataType);
        else
        {
            CPLAssert(m_dt.GetNumericDataType() == GDT_Int32);
            GDALExtendedDataType::CopyValue(&m_nValue, m_dt, pDstBuffer,
                                            bufferDataType);
        }
    }
    else
    {
        CPLAssert(m_dt.GetNumericDataType() == GDT_UInt32);
        GByte *pabyDstBuffer = static_cast<GByte *>(pDstBuffer);
        for (size_t i = 0; i < count[0]; ++i)
        {
            GDALExtendedDataType::CopyValue(
                &m_anValuesUInt32[static_cast<size_t>(arrayStartIdx[0] +
                                                      i * arrayStep[0])],
                m_dt, pabyDstBuffer, bufferDataType);
            pabyDstBuffer += bufferDataType.GetSize() * bufferStride[0];
        }
    }
    return true;
}

GDALMDArrayRegularlySpaced::GDALMDArrayRegularlySpaced(
    const std::string &osParentName, const std::string &osName,
    const std::shared_ptr<GDALDimension> &poDim, double dfStart,
    double dfIncrement, double dfOffsetInIncrement)
    : GDALAbstractMDArray(osParentName, osName),
      GDALMDArray(osParentName, osName), m_dfStart(dfStart),
      m_dfIncrement(dfIncrement),
      m_dfOffsetInIncrement(dfOffsetInIncrement), m_dims{poDim}
{
}

std::shared_ptr<GDALMDArrayRegularlySpaced> GDALMDArrayRegularlySpaced::Create(
    const std::string &osParentName, const std::string &osName,
    const std::shared_ptr<GDALDimension> &poDim, double dfStart,
    double dfIncrement, double dfOffsetInIncrement)
{
    auto poArray = std::make_shared<GDALMDArrayRegularlySpaced>(
        osParentName, osName, poDim, dfStart, dfIncrement, dfOffsetInIncrement);
    poArray->SetSelf(poArray);
    return poArray;
}

const std::vector<std::shared_ptr<GDALDimension>> &
GDALMDArrayRegularlySpaced::GetDimensions() const
{
    return m_dims;
}

const GDALExtendedDataType &GDALMDArrayRegularlySpaced::GetDataType() const
{
    return m_dt;
}

std::vector<std::shared_ptr<GDALAttribute>>
GDALMDArrayRegularlySpaced::GetAttributes(CSLConstList) const
{
    return m_attributes;
}

void GDALMDArrayRegularlySpaced::AddAttribute(
    const std::shared_ptr<GDALAttribute> &poAttr)
{
    m_attributes.emplace_back(poAttr);
}

bool GDALMDArrayRegularlySpaced::IRead(
    const GUInt64 *arrayStartIdx, const size_t *count, const GInt64 *arrayStep,
    const GPtrDiff_t *bufferStride, const GDALExtendedDataType &bufferDataType,
    void *pDstBuffer) const
{
    GByte *pabyDstBuffer = static_cast<GByte *>(pDstBuffer);
    for (size_t i = 0; i < count[0]; i++)
    {
        const double dfVal = m_dfStart + (arrayStartIdx[0] + i * arrayStep[0] +
                                          m_dfOffsetInIncrement) *
                                             m_dfIncrement;
        GDALExtendedDataType::CopyValue(&dfVal, m_dt, pabyDstBuffer,
                                        bufferDataType);
        pabyDstBuffer += bufferStride[0] * bufferDataType.GetSize();
    }
    return true;
}

GDALDimensionWeakIndexingVar::GDALDimensionWeakIndexingVar(
    const std::string &osParentName, const std::string &osName,
    const std::string &osType, const std::string &osDirection, GUInt64 nSize)
    : GDALDimension(osParentName, osName, osType, osDirection, nSize)
{
}

std::shared_ptr<GDALMDArray>
GDALDimensionWeakIndexingVar::GetIndexingVariable() const
{
    return m_poIndexingVariable.lock();
}

// cppcheck-suppress passedByValue
bool GDALDimensionWeakIndexingVar::SetIndexingVariable(
    std::shared_ptr<GDALMDArray> poIndexingVariable)
{
    m_poIndexingVariable = poIndexingVariable;
    return true;
}

void GDALDimensionWeakIndexingVar::SetSize(GUInt64 nNewSize)
{
    m_nSize = nNewSize;
}

/************************************************************************/
/*                       GDALPamMultiDim::Private                       */
/************************************************************************/

struct GDALPamMultiDim::Private
{
    std::string m_osFilename{};
    std::string m_osPamFilename{};

    struct Statistics
    {
        bool bHasStats = false;
        bool bApproxStats = false;
        double dfMin = 0;
        double dfMax = 0;
        double dfMean = 0;
        double dfStdDev = 0;
        GUInt64 nValidCount = 0;
    };

    struct ArrayInfo
    {
        std::shared_ptr<OGRSpatialReference> poSRS{};
        // cppcheck-suppress unusedStructMember
        Statistics stats{};
    };

    typedef std::pair<std::string, std::string> NameContext;
    std::map<NameContext, ArrayInfo> m_oMapArray{};
    std::vector<CPLXMLTreeCloser> m_apoOtherNodes{};
    bool m_bDirty = false;
    bool m_bLoaded = false;
};

/************************************************************************/
/*                          GDALPamMultiDim                             */
/************************************************************************/

GDALPamMultiDim::GDALPamMultiDim(const std::string &osFilename)
    : d(new Private())
{
    d->m_osFilename = osFilename;
}

/************************************************************************/
/*                   GDALPamMultiDim::~GDALPamMultiDim()                */
/************************************************************************/

GDALPamMultiDim::~GDALPamMultiDim()
{
    if (d->m_bDirty)
        Save();
}

/************************************************************************/
/*                          GDALPamMultiDim::Load()                     */
/************************************************************************/

void GDALPamMultiDim::Load()
{
    if (d->m_bLoaded)
        return;
    d->m_bLoaded = true;

    const char *pszProxyPam = PamGetProxy(d->m_osFilename.c_str());
    d->m_osPamFilename =
        pszProxyPam ? std::string(pszProxyPam) : d->m_osFilename + ".aux.xml";
    CPLXMLTreeCloser oTree(nullptr);
    {
        CPLErrorStateBackuper oErrorStateBackuper(CPLQuietErrorHandler);
        oTree.reset(CPLParseXMLFile(d->m_osPamFilename.c_str()));
    }
    if (!oTree)
    {
        return;
    }
    const auto poPAMMultiDim = CPLGetXMLNode(oTree.get(), "=PAMDataset");
    if (!poPAMMultiDim)
        return;
    for (CPLXMLNode *psIter = poPAMMultiDim->psChild; psIter;
         psIter = psIter->psNext)
    {
        if (psIter->eType == CXT_Element &&
            strcmp(psIter->pszValue, "Array") == 0)
        {
            const char *pszName = CPLGetXMLValue(psIter, "name", nullptr);
            if (!pszName)
                continue;
            const char *pszContext = CPLGetXMLValue(psIter, "context", "");
            const auto oKey =
                std::pair<std::string, std::string>(pszName, pszContext);

            /* --------------------------------------------------------------------
             */
            /*      Check for an SRS node. */
            /* --------------------------------------------------------------------
             */
            const CPLXMLNode *psSRSNode = CPLGetXMLNode(psIter, "SRS");
            if (psSRSNode)
            {
                std::shared_ptr<OGRSpatialReference> poSRS =
                    std::make_shared<OGRSpatialReference>();
                poSRS->SetFromUserInput(
                    CPLGetXMLValue(psSRSNode, nullptr, ""),
                    OGRSpatialReference::SET_FROM_USER_INPUT_LIMITATIONS);
                const char *pszMapping = CPLGetXMLValue(
                    psSRSNode, "dataAxisToSRSAxisMapping", nullptr);
                if (pszMapping)
                {
                    char **papszTokens =
                        CSLTokenizeStringComplex(pszMapping, ",", FALSE, FALSE);
                    std::vector<int> anMapping;
                    for (int i = 0; papszTokens && papszTokens[i]; i++)
                    {
                        anMapping.push_back(atoi(papszTokens[i]));
                    }
                    CSLDestroy(papszTokens);
                    poSRS->SetDataAxisToSRSAxisMapping(anMapping);
                }
                else
                {
                    poSRS->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
                }

                const char *pszCoordinateEpoch =
                    CPLGetXMLValue(psSRSNode, "coordinateEpoch", nullptr);
                if (pszCoordinateEpoch)
                    poSRS->SetCoordinateEpoch(CPLAtof(pszCoordinateEpoch));

                d->m_oMapArray[oKey].poSRS = std::move(poSRS);
            }

            const CPLXMLNode *psStatistics =
                CPLGetXMLNode(psIter, "Statistics");
            if (psStatistics)
            {
                Private::Statistics sStats;
                sStats.bHasStats = true;
                sStats.bApproxStats = CPLTestBool(
                    CPLGetXMLValue(psStatistics, "ApproxStats", "false"));
                sStats.dfMin =
                    CPLAtofM(CPLGetXMLValue(psStatistics, "Minimum", "0"));
                sStats.dfMax =
                    CPLAtofM(CPLGetXMLValue(psStatistics, "Maximum", "0"));
                sStats.dfMean =
                    CPLAtofM(CPLGetXMLValue(psStatistics, "Mean", "0"));
                sStats.dfStdDev =
                    CPLAtofM(CPLGetXMLValue(psStatistics, "StdDev", "0"));
                sStats.nValidCount = static_cast<GUInt64>(CPLAtoGIntBig(
                    CPLGetXMLValue(psStatistics, "ValidSampleCount", "0")));
                d->m_oMapArray[oKey].stats = sStats;
            }
        }
        else
        {
            CPLXMLNode *psNextBackup = psIter->psNext;
            psIter->psNext = nullptr;
            d->m_apoOtherNodes.emplace_back(
                CPLXMLTreeCloser(CPLCloneXMLTree(psIter)));
            psIter->psNext = psNextBackup;
        }
    }
}

/************************************************************************/
/*                          GDALPamMultiDim::Save()                     */
/************************************************************************/

void GDALPamMultiDim::Save()
{
    CPLXMLTreeCloser oTree(
        CPLCreateXMLNode(nullptr, CXT_Element, "PAMDataset"));
    for (const auto &poOtherNode : d->m_apoOtherNodes)
    {
        CPLAddXMLChild(oTree.get(), CPLCloneXMLTree(poOtherNode.get()));
    }
    for (const auto &kv : d->m_oMapArray)
    {
        CPLXMLNode *psArrayNode =
            CPLCreateXMLNode(oTree.get(), CXT_Element, "Array");
        CPLAddXMLAttributeAndValue(psArrayNode, "name", kv.first.first.c_str());
        if (!kv.first.second.empty())
        {
            CPLAddXMLAttributeAndValue(psArrayNode, "context",
                                       kv.first.second.c_str());
        }
        if (kv.second.poSRS)
        {
            char *pszWKT = nullptr;
            {
                CPLErrorStateBackuper oErrorStateBackuper(CPLQuietErrorHandler);
                const char *const apszOptions[] = {"FORMAT=WKT2", nullptr};
                kv.second.poSRS->exportToWkt(&pszWKT, apszOptions);
            }
            CPLXMLNode *psSRSNode =
                CPLCreateXMLElementAndValue(psArrayNode, "SRS", pszWKT);
            CPLFree(pszWKT);
            const auto &mapping =
                kv.second.poSRS->GetDataAxisToSRSAxisMapping();
            CPLString osMapping;
            for (size_t i = 0; i < mapping.size(); ++i)
            {
                if (!osMapping.empty())
                    osMapping += ",";
                osMapping += CPLSPrintf("%d", mapping[i]);
            }
            CPLAddXMLAttributeAndValue(psSRSNode, "dataAxisToSRSAxisMapping",
                                       osMapping.c_str());

            const double dfCoordinateEpoch =
                kv.second.poSRS->GetCoordinateEpoch();
            if (dfCoordinateEpoch > 0)
            {
                std::string osCoordinateEpoch =
                    CPLSPrintf("%f", dfCoordinateEpoch);
                if (osCoordinateEpoch.find('.') != std::string::npos)
                {
                    while (osCoordinateEpoch.back() == '0')
                        osCoordinateEpoch.resize(osCoordinateEpoch.size() - 1);
                }
                CPLAddXMLAttributeAndValue(psSRSNode, "coordinateEpoch",
                                           osCoordinateEpoch.c_str());
            }
        }

        if (kv.second.stats.bHasStats)
        {
            CPLXMLNode *psMDArray =
                CPLCreateXMLNode(psArrayNode, CXT_Element, "Statistics");
            CPLCreateXMLElementAndValue(psMDArray, "ApproxStats",
                                        kv.second.stats.bApproxStats ? "1"
                                                                     : "0");
            CPLCreateXMLElementAndValue(
                psMDArray, "Minimum",
                CPLSPrintf("%.17g", kv.second.stats.dfMin));
            CPLCreateXMLElementAndValue(
                psMDArray, "Maximum",
                CPLSPrintf("%.17g", kv.second.stats.dfMax));
            CPLCreateXMLElementAndValue(
                psMDArray, "Mean", CPLSPrintf("%.17g", kv.second.stats.dfMean));
            CPLCreateXMLElementAndValue(
                psMDArray, "StdDev",
                CPLSPrintf("%.17g", kv.second.stats.dfStdDev));
            CPLCreateXMLElementAndValue(
                psMDArray, "ValidSampleCount",
                CPLSPrintf(CPL_FRMT_GUIB, kv.second.stats.nValidCount));
        }
    }

    int bSaved;
    CPLErrorAccumulator oErrorAccumulator;
    {
        auto oAccumulator = oErrorAccumulator.InstallForCurrentScope();
        CPL_IGNORE_RET_VAL(oAccumulator);
        bSaved =
            CPLSerializeXMLTreeToFile(oTree.get(), d->m_osPamFilename.c_str());
    }

    const char *pszNewPam = nullptr;
    if (!bSaved && PamGetProxy(d->m_osFilename.c_str()) == nullptr &&
        ((pszNewPam = PamAllocateProxy(d->m_osFilename.c_str())) != nullptr))
    {
        CPLErrorReset();
        CPLSerializeXMLTreeToFile(oTree.get(), pszNewPam);
    }
    else
    {
        oErrorAccumulator.ReplayErrors();
    }
}

/************************************************************************/
/*                    GDALPamMultiDim::GetSpatialRef()                  */
/************************************************************************/

std::shared_ptr<OGRSpatialReference>
GDALPamMultiDim::GetSpatialRef(const std::string &osArrayFullName,
                               const std::string &osContext)
{
    Load();
    auto oIter =
        d->m_oMapArray.find(std::make_pair(osArrayFullName, osContext));
    if (oIter != d->m_oMapArray.end())
        return oIter->second.poSRS;
    return nullptr;
}

/************************************************************************/
/*                    GDALPamMultiDim::SetSpatialRef()                  */
/************************************************************************/

void GDALPamMultiDim::SetSpatialRef(const std::string &osArrayFullName,
                                    const std::string &osContext,
                                    const OGRSpatialReference *poSRS)
{
    Load();
    d->m_bDirty = true;
    if (poSRS && !poSRS->IsEmpty())
        d->m_oMapArray[std::make_pair(osArrayFullName, osContext)].poSRS.reset(
            poSRS->Clone());
    else
        d->m_oMapArray[std::make_pair(osArrayFullName, osContext)]
            .poSRS.reset();
}

/************************************************************************/
/*                           GetStatistics()                            */
/************************************************************************/

CPLErr GDALPamMultiDim::GetStatistics(const std::string &osArrayFullName,
                                      const std::string &osContext,
                                      bool bApproxOK, double *pdfMin,
                                      double *pdfMax, double *pdfMean,
                                      double *pdfStdDev, GUInt64 *pnValidCount)
{
    Load();
    auto oIter =
        d->m_oMapArray.find(std::make_pair(osArrayFullName, osContext));
    if (oIter == d->m_oMapArray.end())
        return CE_Failure;
    const auto &stats = oIter->second.stats;
    if (!stats.bHasStats)
        return CE_Failure;
    if (!bApproxOK && stats.bApproxStats)
        return CE_Failure;
    if (pdfMin)
        *pdfMin = stats.dfMin;
    if (pdfMax)
        *pdfMax = stats.dfMax;
    if (pdfMean)
        *pdfMean = stats.dfMean;
    if (pdfStdDev)
        *pdfStdDev = stats.dfStdDev;
    if (pnValidCount)
        *pnValidCount = stats.nValidCount;
    return CE_None;
}

/************************************************************************/
/*                           SetStatistics()                            */
/************************************************************************/

void GDALPamMultiDim::SetStatistics(const std::string &osArrayFullName,
                                    const std::string &osContext,
                                    bool bApproxStats, double dfMin,
                                    double dfMax, double dfMean,
                                    double dfStdDev, GUInt64 nValidCount)
{
    Load();
    d->m_bDirty = true;
    auto &stats =
        d->m_oMapArray[std::make_pair(osArrayFullName, osContext)].stats;
    stats.bHasStats = true;
    stats.bApproxStats = bApproxStats;
    stats.dfMin = dfMin;
    stats.dfMax = dfMax;
    stats.dfMean = dfMean;
    stats.dfStdDev = dfStdDev;
    stats.nValidCount = nValidCount;
}

/************************************************************************/
/*                           ClearStatistics()                          */
/************************************************************************/

void GDALPamMultiDim::ClearStatistics(const std::string &osArrayFullName,
                                      const std::string &osContext)
{
    Load();
    d->m_bDirty = true;
    d->m_oMapArray[std::make_pair(osArrayFullName, osContext)].stats.bHasStats =
        false;
}

/************************************************************************/
/*                           ClearStatistics()                          */
/************************************************************************/

void GDALPamMultiDim::ClearStatistics()
{
    Load();
    d->m_bDirty = true;
    for (auto &kv : d->m_oMapArray)
        kv.second.stats.bHasStats = false;
}

/************************************************************************/
/*                             GetPAM()                                 */
/************************************************************************/

/*static*/ std::shared_ptr<GDALPamMultiDim>
GDALPamMultiDim::GetPAM(const std::shared_ptr<GDALMDArray> &poParent)
{
    auto poPamArray = dynamic_cast<GDALPamMDArray *>(poParent.get());
    if (poPamArray)
        return poPamArray->GetPAM();
    return nullptr;
}

/************************************************************************/
/*                           GDALPamMDArray                             */
/************************************************************************/

GDALPamMDArray::GDALPamMDArray(const std::string &osParentName,
                               const std::string &osName,
                               const std::shared_ptr<GDALPamMultiDim> &poPam,
                               const std::string &osContext)
    :
#if !defined(COMPILER_WARNS_ABOUT_ABSTRACT_VBASE_INIT)
      GDALAbstractMDArray(osParentName, osName),
#endif
      GDALMDArray(osParentName, osName, osContext), m_poPam(poPam)
{
}

/************************************************************************/
/*                    GDALPamMDArray::SetSpatialRef()                   */
/************************************************************************/

bool GDALPamMDArray::SetSpatialRef(const OGRSpatialReference *poSRS)
{
    if (!m_poPam)
        return false;
    m_poPam->SetSpatialRef(GetFullName(), GetContext(), poSRS);
    return true;
}

/************************************************************************/
/*                    GDALPamMDArray::GetSpatialRef()                   */
/************************************************************************/

std::shared_ptr<OGRSpatialReference> GDALPamMDArray::GetSpatialRef() const
{
    if (!m_poPam)
        return nullptr;
    return m_poPam->GetSpatialRef(GetFullName(), GetContext());
}

/************************************************************************/
/*                           GetStatistics()                            */
/************************************************************************/

CPLErr GDALPamMDArray::GetStatistics(bool bApproxOK, bool bForce,
                                     double *pdfMin, double *pdfMax,
                                     double *pdfMean, double *pdfStdDev,
                                     GUInt64 *pnValidCount,
                                     GDALProgressFunc pfnProgress,
                                     void *pProgressData)
{
    if (m_poPam && m_poPam->GetStatistics(GetFullName(), GetContext(),
                                          bApproxOK, pdfMin, pdfMax, pdfMean,
                                          pdfStdDev, pnValidCount) == CE_None)
    {
        return CE_None;
    }
    if (!bForce)
        return CE_Warning;

    return GDALMDArray::GetStatistics(bApproxOK, bForce, pdfMin, pdfMax,
                                      pdfMean, pdfStdDev, pnValidCount,
                                      pfnProgress, pProgressData);
}

/************************************************************************/
/*                           SetStatistics()                            */
/************************************************************************/

bool GDALPamMDArray::SetStatistics(bool bApproxStats, double dfMin,
                                   double dfMax, double dfMean, double dfStdDev,
                                   GUInt64 nValidCount,
                                   CSLConstList /* papszOptions */)
{
    if (!m_poPam)
        return false;
    m_poPam->SetStatistics(GetFullName(), GetContext(), bApproxStats, dfMin,
                           dfMax, dfMean, dfStdDev, nValidCount);
    return true;
}

/************************************************************************/
/*                           ClearStatistics()                          */
/************************************************************************/

void GDALPamMDArray::ClearStatistics()
{
    if (!m_poPam)
        return;
    m_poPam->ClearStatistics(GetFullName(), GetContext());
}

//! @endcond
