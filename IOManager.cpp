/*
   For more information, please see: http://software.sci.utah.edu

   The MIT License

   Copyright (c) 2008 Scientific Computing and Imaging Institute,
   University of Utah.


   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included
   in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
   THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
   DEALINGS IN THE SOFTWARE.
*/

/**
  \file    IOManager.cpp
  \author  Jens Krueger
           SCI Institute
           University of Utah
  \date    August 2008
*/

#include <algorithm>
#include <fstream>
#include <float.h>
#include <iterator>
#include <set>
#include <sstream>
#include <map>
#include <memory>
#include "boost/cstdint.hpp"
#include "3rdParty/jpeglib/jconfig.h"

#include "IOManager.h"

#include "Basics/SysTools.h"
#include "Controller/Controller.h"
#include "DSFactory.h"
#include "exception/UnmergeableDatasets.h"
#include "expressions/parser.h"
#include "expressions/syntax.h"
#include "expressions/treenode.h"
#include "IO/DICOM/DICOMParser.h"
#include "IO/Images/ImageParser.h"
#include "Quantize.h"
#include "Renderer/GPUMemMan/GPUMemMan.h"
#include "TuvokJPEG.h"
#include "uvfDataset.h"
#include "UVF/UVF.h"
#include "UVF/GeometryDataBlock.h"
#include "UVF/Histogram1DDataBlock.h"
#include "UVF/Histogram2DDataBlock.h"

#include "AnalyzeConverter.h"
#include "BOVConverter.h"
#include "NRRDConverter.h"
#include "QVISConverter.h"
#include "KitwareConverter.h"
#include "REKConverter.h"
#include "IASSConverter.h"
#include "I3MConverter.h"
#include "InveonConverter.h"
#include "StkConverter.h"
#include "TiffVolumeConverter.h"
#include "VFFConverter.h"
#include "VGStudioConverter.h"

#include "Mesh.h"
#include "OBJGeoConverter.h"
#include "PLYGeoConverter.h"
#include "MobileGeoConverter.h"
#include "MedAlyVisGeoConverter.h"
#include "MedAlyVisFiberTractGeoConverter.h"

using namespace std;
using namespace tuvok;

static void read_first_block(const string& filename,
                             vector<int8_t>& block)
{
  ifstream ifs(filename.c_str(), ifstream::in |
                                      ifstream::binary);
  block.resize(512);
  ifs.read(reinterpret_cast<char*>(&block[0]), 512);
  ifs.close();
}

// Figure out the converters that can convert the given file.
// Multiple formats might think they can do as much; we return all of them and
// let the higher level figure it out.
namespace {
  template<typename ForwIter>
  std::set<AbstrConverter*>
  identify_converters(const std::string& filename,
                      ForwIter cbegin, ForwIter cend)
  {
    std::set<AbstrConverter*> converters;

    vector<int8_t> bytes(512);
    read_first_block(filename, bytes);

    while(cbegin != cend) {
      MESSAGE("Attempting converter '%s'", (*cbegin)->GetDesc().c_str());
      if((*cbegin)->CanRead(filename, bytes)) {
        MESSAGE("Converter '%s' can read '%s'!",
                (*cbegin)->GetDesc().c_str(), filename.c_str());
        converters.insert(*cbegin);
      }
      ++cbegin;
    }
    return converters;
  }
}

IOManager::IOManager() :
  m_pFinalConverter(NULL),
  m_dsFactory(new io::DSFactory()),
  m_iMaxBrickSize(DEFAULT_BRICKSIZE),
  m_iBrickOverlap(DEFAULT_BRICKOVERLAP),
  m_iIncoresize(m_iMaxBrickSize*m_iMaxBrickSize*m_iMaxBrickSize)
{
  m_vpGeoConverters.push_back(new PLYGeoConverter());
  m_vpGeoConverters.push_back(new OBJGeoConverter());
  m_vpGeoConverters.push_back(new MobileGeoConverter());
  m_vpGeoConverters.push_back(new MedAlyVisGeoConverter());
  m_vpGeoConverters.push_back(new MedAlyVisFiberTractGeoConverter());

  m_vpConverters.push_back(new VGStudioConverter());
  m_vpConverters.push_back(new QVISConverter());
  m_vpConverters.push_back(new NRRDConverter());
  m_vpConverters.push_back(new StkConverter());
  m_vpConverters.push_back(new TiffVolumeConverter());
  m_vpConverters.push_back(new VFFConverter());
  m_vpConverters.push_back(new BOVConverter());
  m_vpConverters.push_back(new REKConverter());
  m_vpConverters.push_back(new IASSConverter());
  m_vpConverters.push_back(new I3MConverter());
  m_vpConverters.push_back(new KitwareConverter());
  m_vpConverters.push_back(new InveonConverter());
  m_vpConverters.push_back(new AnalyzeConverter());
  m_dsFactory->AddReader(tr1::shared_ptr<UVFDataset>(new UVFDataset()));
}


void IOManager::RegisterExternalConverter(AbstrConverter* pConverter) {
  m_vpConverters.push_back(pConverter);
}

void IOManager::RegisterFinalConverter(AbstrConverter* pConverter) {
  if ( m_pFinalConverter ) delete m_pFinalConverter;
  m_pFinalConverter = pConverter;
}

namespace {
  template <typename T>
  void Delete(T *t) { delete t; }
}

IOManager::~IOManager()
{
  std::for_each(m_vpConverters.begin(), m_vpConverters.end(),
                Delete<AbstrConverter>);
  std::for_each(m_vpGeoConverters.begin(), m_vpGeoConverters.end(),
                Delete<AbstrGeoConverter>);
  m_vpConverters.clear();
  m_vpGeoConverters.clear();

  delete m_pFinalConverter;
}

vector<std::tr1::shared_ptr<FileStackInfo> >
IOManager::ScanDirectory(string strDirectory) const {
  MESSAGE("Scanning directory %s", strDirectory.c_str());

  vector<std::tr1::shared_ptr<FileStackInfo> > fileStacks;

  DICOMParser parseDICOM;
  parseDICOM.GetDirInfo(strDirectory);

  // Sort out DICOMs with embedded images that we can't read.
  for (size_t stack=0; stack < parseDICOM.m_FileStacks.size(); ++stack) {
    std::auto_ptr<DICOMStackInfo> f = std::auto_ptr<DICOMStackInfo>(
      new DICOMStackInfo((DICOMStackInfo*)parseDICOM.m_FileStacks[stack])
    );

    // if trying to load JPEG files. check if we can handle the JPEG payload
    if (f->m_bIsJPEGEncoded) {
      for(size_t i=0; i < f->m_Elements.size(); ++i) {
        if(!tuvok::JPEG(f->m_Elements[i]->m_strFileName,
                        dynamic_cast<SimpleDICOMFileInfo*>
                          (f->m_Elements[i])->GetOffsetToData()).valid()) {
          WARNING("Can't load JPEG in stack %u, element %u!",
                  static_cast<unsigned>(stack), static_cast<unsigned>(i));
          // should probably be using ptr container lib here instead of
          // trying to explicitly manage this.
          delete *(parseDICOM.m_FileStacks.begin()+stack);
          parseDICOM.m_FileStacks.erase(parseDICOM.m_FileStacks.begin()+stack);
          stack--;
          break;
        }
      }
    }
  }

  if (parseDICOM.m_FileStacks.size() == 1) {
    MESSAGE("  found a single DICOM stack");
  } else {
    MESSAGE("  found %u DICOM stacks",
            static_cast<unsigned>(parseDICOM.m_FileStacks.size()));
  }

  for (size_t stack=0; stack < parseDICOM.m_FileStacks.size(); ++stack) {
    std::tr1::shared_ptr<FileStackInfo> f =
      std::tr1::shared_ptr<FileStackInfo>(new DICOMStackInfo(
        static_cast<DICOMStackInfo*>(parseDICOM.m_FileStacks[stack])
      ));

    stringstream s;
    s << f->m_strFileType << " Stack: " << f->m_strDesc;
    f->m_strDesc = s.str();

    fileStacks.push_back(f);
  }

  ImageParser parseImages;
  parseImages.GetDirInfo(strDirectory);

  if (parseImages.m_FileStacks.size() == 1) {
    MESSAGE("  found a single image stack");
  } else {
    MESSAGE("  found %u image stacks",
            static_cast<unsigned>(parseImages.m_FileStacks.size()));
  }

  for (size_t stack=0; stack < parseImages.m_FileStacks.size(); ++stack) {
    std::tr1::shared_ptr<FileStackInfo> f =
      std::tr1::shared_ptr<FileStackInfo>(new ImageStackInfo(
        dynamic_cast<ImageStackInfo*>(parseImages.m_FileStacks[stack])
      ));

    stringstream s;
    s << f->m_strFileType << " Stack: " << f->m_strDesc;
    f->m_strDesc = s.str();

    fileStacks.push_back(f);
  }

   // add other image parsers here

  MESSAGE("  scan complete");

  return fileStacks;
}

#ifdef DETECTED_OS_WINDOWS
  #pragma warning(disable:4996)
#endif

bool IOManager::ConvertDataset(FileStackInfo* pStack,
                               const string& strTargetFilename,
                               const string& strTempDir,
                               UINT64 iMaxBrickSize,
                               UINT64 iBrickOverlap,
                               const bool bQuantizeTo8Bit) const {
  MESSAGE("Request to convert stack of %s files to %s received",
          pStack->m_strDesc.c_str(), strTargetFilename.c_str());

  if (pStack->m_strFileType == "DICOM") {
    MESSAGE("  Detected DICOM stack, starting DICOM conversion");

    DICOMStackInfo* pDICOMStack = ((DICOMStackInfo*)pStack);

    MESSAGE("  Stack contains %u files",
            static_cast<unsigned>(pDICOMStack->m_Elements.size()));
    MESSAGE("    Series: %u  Bits: %u (%u)", pDICOMStack->m_iSeries,
                                             pDICOMStack->m_iAllocated,
                                             pDICOMStack->m_iStored);
    MESSAGE("    Date: %s  Time: %s", pDICOMStack->m_strAcquDate.c_str(),
                                      pDICOMStack->m_strAcquTime.c_str());
    MESSAGE("    Modality: %s  Description: %s",
            pDICOMStack->m_strModality.c_str(),
            pDICOMStack->m_strDesc.c_str());
    MESSAGE("    Aspect Ratio: %g %g %g", pDICOMStack->m_fvfAspect.x,
            pDICOMStack->m_fvfAspect.y, pDICOMStack->m_fvfAspect.z);

    string strTempMergeFilename = strTempDir +
                                  SysTools::GetFilename(strTargetFilename) +
                                  "~";
    MESSAGE("Creating intermediate file %s", strTempMergeFilename.c_str());

    ofstream fs;
    fs.open(strTempMergeFilename.c_str(), fstream::binary);
    if (fs.fail()) {
      T_ERROR("Could not create temp file %s aborted conversion.",
              strTempMergeFilename.c_str());
      return false;
    }

    vector<char> vData;
    for (size_t j=0; j < pDICOMStack->m_Elements.size(); j++) {
      UINT32 iDataSize = pDICOMStack->m_Elements[j]->GetDataSize();
      vData.resize(iDataSize);

      if (pDICOMStack->m_bIsJPEGEncoded) {
        MESSAGE("JPEG is %d bytes, offset %d", iDataSize,
                dynamic_cast<SimpleDICOMFileInfo*>(pDICOMStack->m_Elements[j])
                  ->GetOffsetToData());
        tuvok::JPEG jpg(pDICOMStack->m_Elements[j]->m_strFileName,
                        dynamic_cast<SimpleDICOMFileInfo*>
                          (pDICOMStack->m_Elements[j])->GetOffsetToData());
        if(!jpg.valid()) {
          T_ERROR("'%s' reports an embedded JPEG, but the JPEG is invalid.",
                  pDICOMStack->m_Elements[j]->m_strFileName.c_str());
          return false;
        }
        MESSAGE("jpg is: %u bytes (%ux%u, %u components)", UINT32(jpg.size()),
                UINT32(jpg.width()), UINT32(jpg.height()),
                UINT32(jpg.components()));

        const char *jpeg_data = jpg.data();
        copy(jpeg_data, jpeg_data + jpg.size(), &vData[0]);
        pDICOMStack->m_iAllocated = BITS_IN_JSAMPLE;
      } else {
        pDICOMStack->m_Elements[j]->GetData(vData);
        MESSAGE("Creating intermediate file %s\n%u%%",
                strTempMergeFilename.c_str(),
                static_cast<unsigned>((100*j)/pDICOMStack->m_Elements.size()));
      }

      if (pDICOMStack->m_bIsBigEndian != EndianConvert::IsBigEndian()) {
        switch (pDICOMStack->m_iAllocated) {
          case  8 : break;
          case 16 : {
                short *pData = reinterpret_cast<short*>(&vData[0]);
                for (UINT32 k = 0;k<iDataSize/2;k++)
                  pData[k] = EndianConvert::Swap<short>(pData[k]);
                } break;
          case 32 : {
                float *pData = reinterpret_cast<float*>(&vData[0]);
                for (UINT32 k = 0;k<iDataSize/4;k++)
                  pData[k] = EndianConvert::Swap<float>(pData[k]);
                } break;
        }
      }

      // Create temporary file with the DICOM (image) data.  We pretend 3
      // component data is 4 component data to simplify processing later.
      /// @todo FIXME: this code assumes 3 component data is always 3*char
      if (pDICOMStack->m_iComponentCount == 3) {
        UINT32 iRGBADataSize = (iDataSize / 3 ) * 4;

        // Later we'll tell RAWConverter that this dataset has
        // m_iComponentCount components.  Since we're upping the number of
        // components here, we update the component count too.
        pDICOMStack->m_iComponentCount = 4;
        // Do note that the number of components in the data and the number of
        // components in our in-memory copy of the data now differ.

        unsigned char *pRGBAData = new unsigned char[ iRGBADataSize ];
        for (UINT32 k = 0;k<iDataSize/3;k++) {
          pRGBAData[k*4+0] = vData[k*3+0];
          pRGBAData[k*4+1] = vData[k*3+1];
          pRGBAData[k*4+2] = vData[k*3+2];
          pRGBAData[k*4+3] = 255;
        }
        fs.write((char*)pRGBAData, iRGBADataSize);
        delete [] pRGBAData;
      } else {
        fs.write(&vData[0], iDataSize);
      }
    }

    fs.close();
    MESSAGE("    done creating intermediate file %s", strTempMergeFilename.c_str());

    UINT64VECTOR3 iSize = UINT64VECTOR3(pDICOMStack->m_ivSize);
    iSize.z *= UINT32(pDICOMStack->m_Elements.size());

    /// \todo evaluate pDICOMStack->m_strModality

    /// \todo read sign property from DICOM file, instead of using the
    /// `m_iAllocated >= 32 heuristic.
    /// \todo read `is floating point' property from DICOM, instead of assuming
    /// false.
    const UINT64 timesteps = 1;
    bool result =
      RAWConverter::ConvertRAWDataset(strTempMergeFilename, strTargetFilename,
                                      strTempDir, 0, pDICOMStack->m_iAllocated,
                                      pDICOMStack->m_iComponentCount,
                                      timesteps,
                                      pDICOMStack->m_bIsBigEndian !=
                                        EndianConvert::IsBigEndian(),
                                      pDICOMStack->m_iAllocated >=32,
                                      false, iSize, pDICOMStack->m_fvfAspect,
                                      "DICOM stack",
                                      SysTools::GetFilename(
                                        pDICOMStack->m_Elements[0]->m_strFileName)
                                      + " to " +
                                      SysTools::GetFilename(
                                        pDICOMStack->m_Elements[
                                          pDICOMStack->m_Elements.size()-1
                                        ]->m_strFileName
                                      ),
                                      iMaxBrickSize, iBrickOverlap,
                                      UVFTables::ES_UNDEFINED,
                                      0, bQuantizeTo8Bit
                                     );

    if(remove(strTempMergeFilename.c_str()) != 0) {
      WARNING("Unable to remove temp file %s", strTempMergeFilename.c_str());
    }

    return result;
  } else if(pStack->m_strFileType == "IMAGE") {
    MESSAGE("  Detected Image stack, starting image conversion");
    MESSAGE("  Stack contains %u files",
            static_cast<unsigned>(pStack->m_Elements.size()));

    string strTempMergeFilename = strTempDir +
                                  SysTools::GetFilename(strTargetFilename) +
                                  "~";
    MESSAGE("Creating intermediate file %s", strTempMergeFilename.c_str());

    ofstream fs;
    fs.open(strTempMergeFilename.c_str(),fstream::binary);
    if (fs.fail())  {
      T_ERROR("Could not create temp file %s aborted conversion.",
              strTempMergeFilename.c_str());
      return false;
    }

    vector<char> vData;
    for (size_t j = 0;j<pStack->m_Elements.size();j++) {
      pStack->m_Elements[j]->GetData(vData);

      fs.write(&vData[0], vData.size());
      MESSAGE("Creating intermediate file %s\n%u%%",
              strTempMergeFilename.c_str(),
              static_cast<unsigned>((100*j)/pStack->m_Elements.size()));
    }

    fs.close();
    MESSAGE("    done creating intermediate file %s",
            strTempMergeFilename.c_str());

    UINT64VECTOR3 iSize = UINT64VECTOR3(pStack->m_ivSize);
    iSize.z *= UINT32(pStack->m_Elements.size());

    const string first_fn =
      SysTools::GetFilename(pStack->m_Elements[0]->m_strFileName);
    const size_t last_elem = pStack->m_Elements.size()-1;
    const string last_fn =
      SysTools::GetFilename(pStack->m_Elements[last_elem]->m_strFileName);

    const UINT64 timesteps = 1;

    // grab the number of components from the first file in the set.
    UINT64 components = pStack->m_Elements[0]->GetComponentCount();

    bool result =
      RAWConverter::ConvertRAWDataset(strTempMergeFilename, strTargetFilename,
                                      strTempDir, 0, pStack->m_iAllocated,
                                      components,
                                      timesteps,
                                      pStack->m_bIsBigEndian !=
                                        EndianConvert::IsBigEndian(),
                                      pStack->m_iComponentCount >= 32, false,
                                      iSize, pStack->m_fvfAspect,
                                      "Image stack",
                                      first_fn + " to " + last_fn,
                                      iMaxBrickSize, iBrickOverlap);

    if(remove(strTempMergeFilename.c_str()) != 0) {
      WARNING("Unable to remove temp file %s", strTempMergeFilename.c_str());
    }

    return result;
  } else {
    T_ERROR("Unknown source stack type %s", pStack->m_strFileType.c_str());
  }
  return false;
}

#ifdef DETECTED_OS_WINDOWS
  #pragma warning(default:4996)
#endif

bool IOManager::MergeDatasets(const vector <string>& strFilenames,
                              const vector <double>& vScales,
                              const vector<double>& vBiases,
                              const string& strTargetFilename,
                              const string& strTempDir,
                              bool bUseMaxMode, bool bNoUserInteraction) const {
  MESSAGE("Request to merge multiple data sets into %s received.",
          strTargetFilename.c_str());

  // convert the input files to RAW
  UINT64        iComponentSizeG=0;
  UINT64        iComponentCountG=0;
  bool          bConvertEndianessG=false;
  bool          bSignedG=false;
  bool          bIsFloatG=false;
  UINT64VECTOR3   vVolumeSizeG(0,0,0);
  FLOATVECTOR3  vVolumeAspectG(0,0,0);
  string strTitleG      = "Merged data from multiple files";
  stringstream  ss;
  for (size_t i = 0;i<strFilenames.size();i++) {
    ss << SysTools::GetFilename(strFilenames[i]);
    if (i<strFilenames.size()-1) ss << " ";
  }
  string        strSourceG = ss.str();

  bool bRAWCreated = false;
  vector<MergeDataset> vIntermediateFiles;
  for (size_t iInputData = 0;iInputData<strFilenames.size();iInputData++) {
    MESSAGE("Reading data sets %s...", strFilenames[iInputData].c_str());
    string strExt       = SysTools::ToUpperCase(SysTools::GetExt(strFilenames[iInputData]));

    MergeDataset IntermediateFile;
    IntermediateFile.fScale = vScales[iInputData];
    IntermediateFile.fBias = vBiases[iInputData];

    if (strExt == "UVF") {
      UVFDataset v(strFilenames[iInputData],m_iMaxBrickSize,false);
      if (!v.IsOpen()) {
        T_ERROR("Could not open '%s'!", strFilenames[iInputData].c_str());
        return false;
      }

      UINT64 iLODLevel = 0; // always extract the highest quality here

      IntermediateFile.iHeaderSkip = 0;

      if (iInputData == 0)  {
        iComponentSizeG = v.GetBitWidth();
        iComponentCountG = v.GetComponentCount();
        bConvertEndianessG = !v.IsSameEndianness();
        bSignedG = v.GetIsSigned();
        bIsFloatG = v.GetIsFloat();
        vVolumeSizeG = v.GetDomainSize(static_cast<size_t>(iLODLevel));
        vVolumeAspectG = FLOATVECTOR3(v.GetScale());
      } else {
#define DATA_TYPE_CHECK(a, b, errmsg) \
  do { \
    if(a != b) { \
      T_ERROR("%s", errmsg); \
      bRAWCreated = false; \
    } \
  } while(0)

        DATA_TYPE_CHECK(iComponentSizeG, v.GetBitWidth(),
                        "mismatched bit widths.");
        DATA_TYPE_CHECK(iComponentCountG, v.GetComponentCount(),
                        "different number of components.");
        DATA_TYPE_CHECK(bConvertEndianessG, !v.IsSameEndianness(),
                        "mismatched endianness.");
        DATA_TYPE_CHECK(bSignedG, v.GetIsSigned(),
                        "signedness differences");
        DATA_TYPE_CHECK(bIsFloatG, v.GetIsFloat(),
                        "some data float, other non-float.");
        DATA_TYPE_CHECK(vVolumeSizeG,
                        v.GetDomainSize(static_cast<size_t>(iLODLevel)),
                        "different volume sizes");
#undef DATA_TYPE_CHECK
        if(bRAWCreated == false) {
          T_ERROR("Incompatible data types.");
          break;
        }
        if (vVolumeAspectG != FLOATVECTOR3(v.GetScale()))
          WARNING("Different aspect ratios found.");
      }

      IntermediateFile.strFilename = strTempDir + SysTools::GetFilename(strFilenames[iInputData]) + SysTools::ToString(rand()) +".raw";
      IntermediateFile.bDelete = true;

      if (!v.Export(iLODLevel, IntermediateFile.strFilename, false)) {
        if (SysTools::FileExists(IntermediateFile.strFilename)) remove(IntermediateFile.strFilename.c_str());
        break;
      } else bRAWCreated = true;
      vIntermediateFiles.push_back(IntermediateFile);
    } else {
      UINT64        iComponentSize=0;
      UINT64        iComponentCount=0;
      bool          bConvertEndianess=false;
      bool          bSigned=false;
      bool          bIsFloat=false;
      UINT64VECTOR3 vVolumeSize(0,0,0);
      FLOATVECTOR3  vVolumeAspect(0,0,0);
      string        strTitle = "";
      string        strSource = "";
      UVFTables::ElementSemanticTable eType = UVFTables::ES_UNDEFINED;

      std::set<AbstrConverter*> converters =
        identify_converters(strFilenames[iInputData], m_vpConverters.begin(),
                            m_vpConverters.end());
      typedef std::set<AbstrConverter*>::const_iterator citer;
      for(citer conv = converters.begin(); conv != converters.end(); ++conv) {
        bRAWCreated = (*conv)->ConvertToRAW(
          strFilenames[iInputData], strTempDir, bNoUserInteraction,
          IntermediateFile.iHeaderSkip, iComponentSize, iComponentCount,
          bConvertEndianess, bSigned, bIsFloat, vVolumeSize, vVolumeAspect,
          strTitle, eType, IntermediateFile.strFilename,
          IntermediateFile.bDelete
        );
        strSource = SysTools::GetFilename(strFilenames[iInputData]);
        if(bRAWCreated) {
          MESSAGE("Conversion using '%s' succeeded!",
                  (*conv)->GetDesc().c_str());
          break;
        }
      }

      if (!bRAWCreated && m_pFinalConverter) {
        bRAWCreated = m_pFinalConverter->ConvertToRAW(
          strFilenames[iInputData], strTempDir, bNoUserInteraction,
          IntermediateFile.iHeaderSkip, iComponentSize, iComponentCount,
          bConvertEndianess, bSigned, bIsFloat, vVolumeSize, vVolumeAspect,
          strTitle, eType, IntermediateFile.strFilename,
          IntermediateFile.bDelete
        );
        strSource = SysTools::GetFilename(strFilenames[iInputData]);
      }

      if (!bRAWCreated) {
        break;
      }

      vIntermediateFiles.push_back(IntermediateFile);

      if (iInputData == 0)  {
        iComponentSizeG = iComponentSize;
        iComponentCountG = iComponentCount;
        bConvertEndianessG = bConvertEndianess;
        bSignedG = bSigned;
        bIsFloatG = bIsFloat;
        vVolumeSizeG = vVolumeSize;
        vVolumeAspectG = vVolumeAspect;
      } else {
        if (iComponentSizeG  != iComponentSize ||
            iComponentCountG != iComponentCount ||
            bConvertEndianessG != bConvertEndianess ||
            bSignedG != bSigned ||
            bIsFloatG != bIsFloat ||
            vVolumeSizeG != vVolumeSize) {
          T_ERROR("Incompatible data types.");
          bRAWCreated = false;
          break;
        }

        if (vVolumeAspectG != vVolumeAspect)
          WARNING("Different aspect ratios found.");
      }
    }
  }

  if (!bRAWCreated) {
    T_ERROR("No raw files.  Deleting temp files...");
    for (size_t i = 0;i<vIntermediateFiles.size();i++) {
      if (vIntermediateFiles[i].bDelete && SysTools::FileExists(vIntermediateFiles[i].strFilename))
        remove(vIntermediateFiles[i].strFilename.c_str());
    }
    T_ERROR("...  and bailing.");
    return false;
  }

  // merge the raw files into a single RAW file
  string strMergedFile = strTempDir + "merged.raw";

  bool bIsMerged = false;
  MasterController *MCtlr = &(Controller::Instance());
  if (bSignedG) {
    if (bIsFloatG) {
      switch (iComponentSizeG) {
        case 32 : {
                    DataMerger<float> d(vIntermediateFiles, strMergedFile, vVolumeSizeG.volume()*iComponentCountG, MCtlr, bUseMaxMode);
                    bIsMerged = d.IsOK();
                    break;
                  }
        case 64 : {
                    DataMerger<double> d(vIntermediateFiles, strMergedFile, vVolumeSizeG.volume()*iComponentCountG, MCtlr, bUseMaxMode);
                    bIsMerged = d.IsOK();
                    break;
                  }
      }
    } else {
      switch (iComponentSizeG) {
        case 8  : {
                    DataMerger<char> d(vIntermediateFiles, strMergedFile, vVolumeSizeG.volume()*iComponentCountG, MCtlr, bUseMaxMode);
                    bIsMerged = d.IsOK();
                    break;
                  }
        case 16 : {
                    DataMerger<short> d(vIntermediateFiles, strMergedFile, vVolumeSizeG.volume()*iComponentCountG, MCtlr, bUseMaxMode);
                    bIsMerged = d.IsOK();
                    break;
                  }
        case 32 : {
                    DataMerger<int> d(vIntermediateFiles, strMergedFile, vVolumeSizeG.volume()*iComponentCountG, MCtlr, bUseMaxMode);
                    bIsMerged = d.IsOK();
                    break;
                  }
        case 64 : {
                    DataMerger<int64_t> d(vIntermediateFiles, strMergedFile, vVolumeSizeG.volume()*iComponentCountG, MCtlr, bUseMaxMode);
                    bIsMerged = d.IsOK();
                    break;
                  }
      }
    }
  } else {
    if (bIsFloatG) {
      // unsigned float ??? :-)
      T_ERROR("Don't know how to handle unsigned float data.");
      return false;
    } else {
      switch (iComponentSizeG) {
        case 8  : {
                    DataMerger<unsigned char> d(vIntermediateFiles, strMergedFile, vVolumeSizeG.volume()*iComponentCountG, MCtlr, bUseMaxMode);
                    bIsMerged = d.IsOK();
                    break;
                  }
        case 16 : {
                    DataMerger<unsigned short> d(vIntermediateFiles, strMergedFile, vVolumeSizeG.volume()*iComponentCountG, MCtlr, bUseMaxMode);
                    bIsMerged = d.IsOK();
                    break;
                  }
        case 32 : {
                    DataMerger<unsigned int> d(vIntermediateFiles, strMergedFile, vVolumeSizeG.volume()*iComponentCountG, MCtlr, bUseMaxMode);
                    bIsMerged = d.IsOK();
                    break;
                  }
        case 64 : {
                    DataMerger<UINT64> d(vIntermediateFiles, strMergedFile, vVolumeSizeG.volume()*iComponentCountG, MCtlr, bUseMaxMode);
                    bIsMerged = d.IsOK();
                    break;
                  }
      }
    }
  }

  MESSAGE("Removing temporary files...");
  for (size_t i = 0;i<vIntermediateFiles.size();i++) {
    if (vIntermediateFiles[i].bDelete && SysTools::FileExists(vIntermediateFiles[i].strFilename))
      remove(vIntermediateFiles[i].strFilename.c_str());
  }
  if (!bIsMerged) {
    WARNING("Merged failed, see other debug messages.");
    return false;
  }

  // convert that single RAW file to the target data
  string strExtTarget = SysTools::ToUpperCase(SysTools::GetExt(strTargetFilename));
  bool bTargetCreated = false;
  if (strExtTarget == "UVF") {
    const UINT64 timesteps = 1;
    bTargetCreated = RAWConverter::ConvertRAWDataset(
        strMergedFile, strTargetFilename, strTempDir, 0,
        iComponentSizeG, iComponentCountG, timesteps, bConvertEndianessG,
        bSignedG, bIsFloatG, vVolumeSizeG, vVolumeAspectG, strTitleG,
        SysTools::GetFilename(strMergedFile), m_iMaxBrickSize,
        m_iBrickOverlap);
  } else {
    for (size_t k = 0;k<m_vpConverters.size();k++) {
      const vector<string>& vStrSupportedExtTarget =
        m_vpConverters[k]->SupportedExt();
      for (size_t l = 0;l<vStrSupportedExtTarget.size();l++) {
        if (vStrSupportedExtTarget[l] == strExtTarget) {
          bTargetCreated = m_vpConverters[k]->ConvertToNative(
            strMergedFile, strTargetFilename, 0, iComponentSizeG,
            iComponentCountG, bSignedG, bIsFloatG, vVolumeSizeG,
            vVolumeAspectG, bNoUserInteraction, false
          );

          if(!bTargetCreated) {
            WARNING("%s said it could convert to native, but failed!",
                    m_vpConverters[k]->GetDesc().c_str());
          } else {
            break;
          }
        }
      }
      if (bTargetCreated) break;
    }
  }
  remove(strMergedFile.c_str());
  return bTargetCreated;
}


bool IOManager::ConvertDataset(const string& strFilename,
                               const string& strTargetFilename,
                               const string& strTempDir,
                               const bool bNoUserInteraction,
                               const UINT64 iMaxBrickSize,
                               const UINT64 iBrickOverlap,
                               const bool bQuantizeTo8Bit) const {
  list<string> files;
  files.push_back(strFilename);
  return ConvertDataset(files, strTargetFilename, strTempDir,
                        bNoUserInteraction, iMaxBrickSize, iBrickOverlap,
                        bQuantizeTo8Bit);
}

bool IOManager::ConvertDataset(const list<string>& files,
                               const string& strTargetFilename,
                               const string& strTempDir,
                               const bool bNoUserInteraction,
                               UINT64 iMaxBrickSize,
                               UINT64 iBrickOverlap,
                               bool bQuantizeTo8Bit) const
{
  if(files.empty()) {
    T_ERROR("No files to convert?!");
    return false;
  }
  {
    ostringstream request;
    request << "Request to convert datasets ";
    copy(files.begin(), files.end(),
              ostream_iterator<string>(request, ", "));
    request << "to " << strTargetFilename << " received.";
    MESSAGE("%s", request.str().c_str());
  }

  // this might actually be a valid test case, if you want to compare
  // performance across brick sizes.  However it's completely ridiculous in
  // actual use, and catches a confusing bug if you forget an argument in the
  // API call (which still compiles due to default arguments!).
  assert(iMaxBrickSize >= 32 &&
         "Incredibly small bricks -- are you sure?");

  /// @todo verify the list of files is `compatible':
  ///   dimensions are the same
  ///   all from the same file format
  ///   all have equivalent bit depth, or at least something that'll convert to
  ///    the same depth
  string strExt = SysTools::ToUpperCase(SysTools::GetExt(*files.begin()));
  string strExtTarget = SysTools::ToUpperCase(SysTools::GetExt(strTargetFilename));

  if (strExtTarget == "UVF") {
    // Iterate through all our converters, stopping when one successfully
    // converts our data.
    std::set<AbstrConverter*> converters =
      identify_converters(*files.begin(), m_vpConverters.begin(),
                          m_vpConverters.end());
    typedef std::set<AbstrConverter*>::const_iterator citer;
    for(citer conv = converters.begin(); conv != converters.end(); ++conv) {
      if((*conv)->ConvertToUVF(files, strTargetFilename, strTempDir,
                               bNoUserInteraction, iMaxBrickSize, iBrickOverlap,
                               bQuantizeTo8Bit)) {
        return true;
      } else {
        WARNING("Converter %s can read files, but conversion failed!",
                (*conv)->GetDesc().c_str());
      }
    }

    MESSAGE("No suitable automatic converter found!");

    if (m_pFinalConverter) {
      MESSAGE("Attempting fallback converter.");
      return m_pFinalConverter->ConvertToUVF(files, strTargetFilename,
                                             strTempDir, bNoUserInteraction,
                                             iMaxBrickSize, iBrickOverlap,
                                             bQuantizeTo8Bit);
    } else {
      return false;
    }
  }

  if(files.size() > 1) {
    T_ERROR("Cannot convert multiple files to anything but UVF.");
    return false;
  }
  // Everything below is for exporting to non-UVF formats.

  string   strFilename = *files.begin();
  UINT64        iHeaderSkip=0;
  UINT64        iComponentSize=0;
  UINT64        iComponentCount=0;
  bool          bConvertEndianess=false;
  bool          bSigned=false;
  bool          bIsFloat=false;
  UINT64VECTOR3 vVolumeSize(0,0,0);
  FLOATVECTOR3  vVolumeAspect(0,0,0);
  string        strTitle = "";
  string        strSource = "";
  UVFTables::ElementSemanticTable eType = UVFTables::ES_UNDEFINED;
  string        strIntermediateFile = "";
  bool          bDeleteIntermediateFile = false;

  bool bRAWCreated = false;

  // source is UVF
  if (strExt == "UVF") {
    // max(): disable bricksize check
    UVFDataset v(strFilename,numeric_limits<UINT64>::max(),false,false);
    if (!v.IsOpen()) return false;

    UINT64 iLODLevel = 0; // always extract the highest quality here

    iHeaderSkip = 0;
    iComponentSize = v.GetBitWidth();
    iComponentCount = v.GetComponentCount();
    bConvertEndianess = !v.IsSameEndianness();
    bSigned = v.GetIsSigned();
    bIsFloat = v.GetIsFloat();
    vVolumeSize = v.GetDomainSize(static_cast<size_t>(iLODLevel));
    vVolumeAspect = FLOATVECTOR3(v.GetScale());
    eType             = UVFTables::ES_UNDEFINED;  /// \todo grab this data from the UVF file
    strTitle          = "UVF data";               /// \todo grab this data from the UVF file
    strSource         = SysTools::GetFilename(strFilename);

    strIntermediateFile = strTempDir + strSource +".raw";
    bDeleteIntermediateFile = true;

    if (!v.Export(iLODLevel, strIntermediateFile, false)) {
      if (SysTools::FileExists(strIntermediateFile)) {
        RAWConverter::Remove(strIntermediateFile.c_str(),
                         Controller::Debug::Out());
      }
      return false;
    } else bRAWCreated = true;
  } else { // for non-UVF source data
    vector<int8_t> bytes(512);
    read_first_block(strFilename, bytes);

    std::set<AbstrConverter*> converters =
      identify_converters(*files.begin(), m_vpConverters.begin(),
                          m_vpConverters.end());
    typedef std::set<AbstrConverter*>::const_iterator citer;
    for(citer conv = converters.begin(); conv != converters.end(); ++conv) {
      if((*conv)->ConvertToRAW(strFilename, strTempDir,
                               bNoUserInteraction,iHeaderSkip,
                               iComponentSize, iComponentCount,
                               bConvertEndianess, bSigned,
                               bIsFloat, vVolumeSize,
                               vVolumeAspect, strTitle, eType,
                               strIntermediateFile,
                               bDeleteIntermediateFile)) {
        bRAWCreated = true;
        break;
      }
    }

    if (!bRAWCreated && m_pFinalConverter) {
      MESSAGE("No converter can read the data.  Trying fallback converter.");
      bRAWCreated = m_pFinalConverter->ConvertToRAW(strFilename, strTempDir,
                                                    bNoUserInteraction,
                                                    iHeaderSkip,
                                                    iComponentSize,
                                                    iComponentCount,
                                                    bConvertEndianess, bSigned,
                                                    bIsFloat, vVolumeSize,
                                                    vVolumeAspect,
                                                    strTitle, eType,
                                                    strIntermediateFile,
                                                    bDeleteIntermediateFile);
    }
  }
  if (!bRAWCreated) { return false; }

  bool bTargetCreated = false;
  for (size_t k = 0;k<m_vpConverters.size();k++) {
    const vector<string>& vStrSupportedExtTarget =
      m_vpConverters[k]->SupportedExt();
    for (size_t l = 0;l<vStrSupportedExtTarget.size();l++) {
      if (vStrSupportedExtTarget[l] == strExtTarget) {
        bTargetCreated =
          m_vpConverters[k]->ConvertToNative(strIntermediateFile,
                                             strTargetFilename,
                                             iHeaderSkip, iComponentSize,
                                             iComponentCount, bSigned,
                                             bIsFloat, vVolumeSize,
                                             vVolumeAspect,
                                             bNoUserInteraction, bQuantizeTo8Bit);
        if (bTargetCreated) break;
      }
    }
    if (bTargetCreated) break;
  }
  if (bDeleteIntermediateFile) remove(strIntermediateFile.c_str());
  if (bTargetCreated) return true;

  return false;
}

UVFDataset* IOManager::ConvertDataset(FileStackInfo* pStack,
                                      const string& strTargetFilename,
                                      const string& strTempDir,
                                      AbstrRenderer* requester,
                                      UINT64 iMaxBrickSize,
                                      UINT64 iBrickOverlap,
                                      const bool bQuantizeTo8Bit) const {
  if (!ConvertDataset(pStack, strTargetFilename, strTempDir, iMaxBrickSize,
                      iBrickOverlap,bQuantizeTo8Bit)) {
    return NULL;
  }
  return dynamic_cast<UVFDataset*>(LoadDataset(strTargetFilename, requester));
}

UVFDataset* IOManager::ConvertDataset(const string& strFilename,
                                      const string& strTargetFilename,
                                      const string& strTempDir,
                                      AbstrRenderer* requester,
                                      UINT64 iMaxBrickSize,
                                      UINT64 iBrickOverlap,
                                      const bool bQuantizeTo8Bit) const {
  if (!ConvertDataset(strFilename, strTargetFilename, strTempDir, false,
                      iMaxBrickSize, iBrickOverlap,bQuantizeTo8Bit)) {
    return NULL;
  }
  return dynamic_cast<UVFDataset*>(LoadDataset(strTargetFilename, requester));
}

Dataset* IOManager::LoadDataset(const string& strFilename,
                                AbstrRenderer* requester) const {
  return Controller::Instance().MemMan()->LoadDataset(strFilename, requester);
}

Dataset* IOManager::CreateDataset(const string& filename,
                                  UINT64 max_brick_size, bool verify) const {
  MESSAGE("Searching for appropriate DS for '%s'", filename.c_str());
  return m_dsFactory->Create(filename, max_brick_size, verify);
}

void IOManager::AddReader(tr1::shared_ptr<FileBackedDataset> ds)
{
  m_dsFactory->AddReader(ds);
}

bool MCBrick(LargeRAWFile* pSourceFile, const vector<UINT64> vBrickSize,
             const vector<UINT64> vBrickOffset, void* pUserContext) {
    MCData* pMCData = (MCData*)pUserContext;
    return pMCData->PerformMC(pSourceFile, vBrickSize, vBrickOffset);
}

bool IOManager::ExtractIsosurface(const tuvok::UVFDataset* pSourceData,
                                  UINT64 iLODlevel, double fIsovalue,
                                  const FLOATVECTOR4& vfColor,
                                  const string& strTargetFilename,
                                  const string& strTempDir) const {
  if (pSourceData->GetComponentCount() != 1) {
    T_ERROR("Isosurface extraction only supported for scalar volumes.");
    return false;
  }

  string strTempFilename = strTempDir + SysTools::GetFilename(strTargetFilename)+".tmp_raw";
  MCData* pMCData = NULL;

  bool   bFloatingPoint  = pSourceData->GetIsFloat();
  bool   bSigned         = pSourceData->GetIsSigned();
  UINT64  iComponentSize = pSourceData->GetBitWidth();
  FLOATVECTOR3 vScale    = FLOATVECTOR3(pSourceData->GetScale());

  AbstrGeoConverter* conv = GetGeoConverterForExt(SysTools::ToLowerCase(SysTools::GetExt(strTargetFilename)),true);
  
  if (conv == NULL) {
    T_ERROR("Unknown Mesh Format.");
    return false;
  }

  UINT64VECTOR3 vDomainSize = pSourceData->GetDomainSize(size_t(iLODlevel));

  if (bFloatingPoint) {
    if (bSigned) {
      switch (iComponentSize) {
        case 32 : pMCData = new MCDataTemplate<float>(strTargetFilename, float(fIsovalue), vScale, vDomainSize, conv, vfColor); break;
        case 64 : pMCData = new MCDataTemplate<double>(strTargetFilename, double(fIsovalue), vScale, vDomainSize, conv, vfColor); break;
      }
    }
  } else {
    if (bSigned) {
      switch (iComponentSize) {
        case  8 : pMCData = new MCDataTemplate<char>(strTargetFilename, char(fIsovalue), vScale, vDomainSize, conv, vfColor); break;
        case 16 : pMCData = new MCDataTemplate<short>(strTargetFilename, short(fIsovalue), vScale, vDomainSize, conv, vfColor); break;
        case 32 : pMCData = new MCDataTemplate<int>(strTargetFilename, int(fIsovalue), vScale, vDomainSize, conv, vfColor); break;
        case 64 : pMCData = new MCDataTemplate<int64_t>(strTargetFilename, int64_t(fIsovalue), vScale, vDomainSize, conv, vfColor); break;
      }
    } else {
      switch (iComponentSize) {
        case  8 : pMCData = new MCDataTemplate<unsigned char>(strTargetFilename, (unsigned char)(fIsovalue), vScale, vDomainSize, conv, vfColor); break;
        case 16 : pMCData = new MCDataTemplate<unsigned short>(strTargetFilename, (unsigned short)(fIsovalue), vScale, vDomainSize, conv, vfColor); break;
        case 32 : pMCData = new MCDataTemplate<UINT32>(strTargetFilename, UINT32(fIsovalue), vScale, vDomainSize, conv, vfColor); break;
        case 64 : pMCData = new MCDataTemplate<UINT64>(strTargetFilename, UINT64(fIsovalue), vScale, vDomainSize, conv, vfColor); break;
      }
    }
  }

  if (!pMCData) {
    T_ERROR("Unsupported data format.");
    return false;
  }

  bool bResult = pSourceData->Export(iLODlevel, strTempFilename, false, &MCBrick, (void*)pMCData, 1);

  if (SysTools::FileExists(strTempFilename)) remove (strTempFilename.c_str());
  delete pMCData;

  if (bResult)
    return true;
  else {
    remove (strTargetFilename.c_str());
    T_ERROR("Export call failed.");
    return false;
  }
}

bool IOManager::ExportMesh(const Mesh* mesh, 
                           const std::string& strTargetFilename) {
  AbstrGeoConverter* conv = GetGeoConverterForExt(SysTools::ToLowerCase(SysTools::GetExt(strTargetFilename)),true);

  if (conv == NULL) {
    T_ERROR("Unknown Mesh Format.");
    return false;
  }

  return conv->ConvertToNative(*mesh, strTargetFilename);
}

bool IOManager::ExportDataset(const UVFDataset* pSourceData, UINT64 iLODlevel,
                              const string& strTargetFilename,
                              const string& strTempDir) const {
  // find the right converter to handle the output
  string strExt = SysTools::ToUpperCase(SysTools::GetExt(strTargetFilename));
  AbstrConverter* pExporter = NULL;
  for (size_t i = 0;i<m_vpConverters.size();i++) {
    const vector<string>& vStrSupportedExt = m_vpConverters[i]->SupportedExt();
    for (size_t j = 0;j<vStrSupportedExt.size();j++) {
      if (vStrSupportedExt[j] == strExt) {
        pExporter = m_vpConverters[i];
        break;
      }
    }
    if (pExporter) break;
  }

  if (!pExporter) {
    T_ERROR("Unknown file extension %s.", strExt.c_str());
    return false;
  }

  string strTempFilename = strTempDir + SysTools::GetFilename(strTargetFilename)+".tmp_raw";
  bool bRAWCreated = pSourceData->Export(iLODlevel, strTempFilename, false);

  if (!bRAWCreated) {
    T_ERROR("Unable to write temp file %s", strTempFilename.c_str());
    return false;
  }

  MESSAGE("Writing Target Dataset");

  bool bTargetCreated = pExporter->ConvertToNative(
                    strTempFilename, strTargetFilename, 0,
                    pSourceData->GetBitWidth(),
                    pSourceData->GetComponentCount(),
                    pSourceData->GetIsSigned(),
                    pSourceData->GetIsFloat(),
                    pSourceData->GetDomainSize(static_cast<size_t>(iLODlevel)),
                    FLOATVECTOR3(pSourceData->GetScale()),
                    false, false
                  );
  remove(strTempFilename.c_str());

  if (!bTargetCreated) {
    T_ERROR("Unable to write target file %s", strTargetFilename.c_str());
    return false;
  }

  MESSAGE("Done!");

  return bTargetCreated;
}


// Try to find the reader for the filename.  If we get back garbage, that must
// mean we can't read this.  If we can't read it, it needs to be converted.
// All your data are belong to us.
bool IOManager::NeedsConversion(const string& strFilename) const {
  const tr1::weak_ptr<Dataset> reader = m_dsFactory->Reader(strFilename);
  return reader.expired();
}

// Some readers checksum the data.  If they do, this is how the UI will access
// that verification method.
bool IOManager::Verify(const string& strFilename) const
{
  const tr1::weak_ptr<Dataset> reader = m_dsFactory->Reader(strFilename);

  // I swear I did not purposely choose words so that this text aligned.
  assert(!reader.expired() && "Impossible; we wouldn't have reached this code "
                              "unless we thought that the format doesn't need "
                              "conversion.  But we only think it doesn't need "
                              "conversion when there's a known reader for the "
                              "file.");

  // Upcast it.  Hard to verify a checksum on an abstract entity.
  const tr1::shared_ptr<FileBackedDataset> fileds =
    tr1::dynamic_pointer_cast<FileBackedDataset>(reader.lock());

  return fileds->Verify(strFilename);
}

using namespace tuvok;
using namespace tuvok::io;

string IOManager::GetLoadDialogString() const {
  string strDialog = "All known Files (";
  map<string,string> descPairs;

  // first create the show all text entry
  // native formats
  const DSFactory::DSList& readers = m_dsFactory->Readers();
  for(DSFactory::DSList::const_iterator rdr=readers.begin();
      rdr != readers.end(); ++rdr) {
    const tr1::shared_ptr<FileBackedDataset> fileds =
      tr1::dynamic_pointer_cast<FileBackedDataset>(*rdr);
    const list<string> extensions = fileds->Extensions();
    for(list<string>::const_iterator ext = extensions.begin();
        ext != extensions.end(); ++ext) {
      strDialog += "*." + SysTools::ToLowerCase(*ext) + " ";
      descPairs[*ext] = (*rdr)->Name();
    }
  }

  // converters
  for (size_t i = 0;i<m_vpConverters.size();i++) {
    for (size_t j = 0;j<m_vpConverters[i]->SupportedExt().size();j++) {
      string strExt = SysTools::ToLowerCase(m_vpConverters[i]->SupportedExt()[j]);
      if (descPairs.count(strExt) == 0) {
        strDialog = strDialog + "*." + strExt + " ";
        descPairs[strExt] = m_vpConverters[i]->GetDesc();
      }
    }
  }
  strDialog += ");;";

  // now create the separate entries, i.e. just UVFs, just TIFFs, etc.
  // native formats
  for(DSFactory::DSList::const_iterator rdr=readers.begin();
      rdr != readers.end(); ++rdr) {
    const tr1::shared_ptr<FileBackedDataset> fileds =
      tr1::dynamic_pointer_cast<FileBackedDataset>(*rdr);
    const list<string> extensions = fileds->Extensions();
    strDialog += string(fileds->Name()) + " (";
    for(list<string>::const_iterator ext = extensions.begin();
        ext != extensions.end(); ++ext) {
      strDialog += "*." + SysTools::ToLowerCase(*ext) + " ";
      descPairs[*ext] = (*rdr)->Name();
    }
    strDialog += ");;";
  }

  // converters
  for (size_t i=0; i < m_vpConverters.size(); i++) {
    strDialog += m_vpConverters[i]->GetDesc() + " (";
    for (size_t j=0; j < m_vpConverters[i]->SupportedExt().size(); j++) {
      string strExt = SysTools::ToLowerCase(m_vpConverters[i]->SupportedExt()[j]);
      strDialog += "*." + strExt;
      if (j<m_vpConverters[i]->SupportedExt().size()-1)
        strDialog += " ";
    }
    strDialog += ");;";
  }

  strDialog += "All Files (*)";

  return strDialog;
}

string IOManager::GetExportDialogString() const {
  string strDialog;
  // separate entries
  for (size_t i=0; i < m_vpConverters.size(); i++) {
    for (size_t j=0; j < m_vpConverters[i]->SupportedExt().size(); j++) {
      if (m_vpConverters[i]->CanExportData()) {
        string strExt = SysTools::ToLowerCase(m_vpConverters[i]->SupportedExt()[j]);
        strDialog += m_vpConverters[i]->GetDesc() + " (*." + strExt + ");;";
      }
    }
  }

  return strDialog;
}

vector< pair <string, string > > IOManager::GetExportFormatList() const {
  vector< pair <string, string > > v;
  v.push_back(make_pair("UVF", "Universal Volume Format"));
  for (size_t i = 0;i<m_vpConverters.size();i++) {
    for (size_t j = 0;j<m_vpConverters[i]->SupportedExt().size();j++) {
      if (m_vpConverters[i]->CanExportData()) {
        v.push_back(
          make_pair(SysTools::ToLowerCase(m_vpConverters[i]->SupportedExt()[j]),
                    m_vpConverters[i]->GetDesc()));
      }
    }
  }
  return v;
}

vector< pair <string, string > > IOManager::GetImportFormatList() const {
  vector< pair <string, string > > v;
  v.push_back(make_pair("UVF", "Universal Volume Format"));
  for (size_t i = 0;i<m_vpConverters.size();i++) {
    for (size_t j = 0;j<m_vpConverters[i]->SupportedExt().size();j++) {
      v.push_back(
        make_pair(SysTools::ToLowerCase(m_vpConverters[i]->SupportedExt()[j]),
                  m_vpConverters[i]->GetDesc()));
    }
  }
  return v;
}


vector< tConverterFormat > IOManager::GetFormatList() const {

  vector< tConverterFormat > v;
  v.push_back(tr1::make_tuple("UVF", "Universal Volume Format", true));
  for (size_t i = 0;i<m_vpConverters.size();i++) {
    for (size_t j = 0;j<m_vpConverters[i]->SupportedExt().size();j++) {
      v.push_back(tr1::make_tuple(
                      SysTools::ToLowerCase(
                        m_vpConverters[i]->SupportedExt()[j]
                      ),
                      m_vpConverters[i]->GetDesc(),
                      m_vpConverters[i]->CanExportData()));
    }
  }
  return v;
}

AbstrConverter* IOManager::GetConverterForExt(std::string ext,
                                              bool bMustSupportExport) const {
  for (size_t i = 0;i<m_vpConverters.size();i++) {
    if (!bMustSupportExport || m_vpConverters[i]->CanExportData()) {
      for (size_t j = 0;j<m_vpConverters[i]->SupportedExt().size();j++) {
        string convExt = SysTools::ToLowerCase(m_vpConverters[i]->SupportedExt()[j]);
        if (ext == convExt) return m_vpConverters[i];
      }
    }
  }
  return NULL;
}


AbstrGeoConverter* IOManager::GetGeoConverterForExt(std::string ext, bool bMustSupportExport) const {
  for (size_t i = 0;i<m_vpGeoConverters.size();i++) {
    if (!bMustSupportExport || m_vpGeoConverters[i]->CanExportData()) {
      for (size_t j = 0;j<m_vpGeoConverters[i]->SupportedExt().size();j++) {
        string convExt = SysTools::ToLowerCase(m_vpGeoConverters[i]->SupportedExt()[j]);
        if (ext == convExt) return m_vpGeoConverters[i];
      }
    }
  }
  return NULL;
}

string IOManager::GetLoadGeoDialogString() const {
  string strDialog = "All known Geometry Files (";
  map<string,string> descPairs;

  // converters
  for (size_t i = 0;i<m_vpGeoConverters.size();i++) {
    for (size_t j = 0;j<m_vpGeoConverters[i]->SupportedExt().size();j++) {
      string strExt = SysTools::ToLowerCase(m_vpGeoConverters[i]->SupportedExt()[j]);
      if (descPairs.count(strExt) == 0) {
        strDialog = strDialog + "*." + strExt + " ";
        descPairs[strExt] = m_vpGeoConverters[i]->GetDesc();
      }
    }
  }
  strDialog += ");;";

  // now create the separate entries, i.e. just OBJs, TRIs, etc.
  for (size_t i=0; i < m_vpGeoConverters.size(); i++) {
    strDialog += m_vpGeoConverters[i]->GetDesc() + " (";
    for (size_t j=0; j < m_vpGeoConverters[i]->SupportedExt().size(); j++) {
      string strExt = SysTools::ToLowerCase(m_vpGeoConverters[i]->SupportedExt()[j]);
      strDialog += "*." + strExt;
      if (j<m_vpGeoConverters[i]->SupportedExt().size()-1)
        strDialog += " ";
    }
    strDialog += ");;";
  }

  strDialog += "All Files (*)";

  return strDialog;
}

string IOManager::GetGeoExportDialogString() const {
  string strDialog;
  // separate entries
  for (size_t i=0; i < m_vpGeoConverters.size(); i++) {
    for (size_t j=0; j < m_vpGeoConverters[i]->SupportedExt().size(); j++) {
      if (m_vpGeoConverters[i]->CanExportData()) {
        string strExt = SysTools::ToLowerCase(m_vpGeoConverters[i]->SupportedExt()[j]);
        strDialog += m_vpGeoConverters[i]->GetDesc() + " (*." + strExt + ");;";
      }
    }
  }

  return strDialog;
}



vector< pair <string, string > > IOManager::GetGeoExportFormatList() const {
  vector< pair <string, string > > v;
  for (size_t i = 0;i<m_vpGeoConverters.size();i++) {
    for (size_t j = 0;j<m_vpGeoConverters[i]->SupportedExt().size();j++) {
      if (m_vpGeoConverters[i]->CanExportData()) {
        v.push_back(
          make_pair(SysTools::ToLowerCase(m_vpGeoConverters[i]->SupportedExt()[j]),
                    m_vpGeoConverters[i]->GetDesc()));
      }
    }
  }
  return v;
}

vector< pair <string, string > > IOManager::GetGeoImportFormatList() const {
  vector< pair <string, string > > v;
  for (size_t i = 0;i<m_vpGeoConverters.size();i++) {
    for (size_t j = 0;j<m_vpGeoConverters[i]->SupportedExt().size();j++) {
      v.push_back(
        make_pair(SysTools::ToLowerCase(m_vpGeoConverters[i]->SupportedExt()[j]),
                  m_vpGeoConverters[i]->GetDesc()));
    }
  }
  return v;
}


vector< tConverterFormat > IOManager::GetGeoFormatList() const {
  vector< tConverterFormat > v;
  for (size_t i = 0;i<m_vpGeoConverters.size();i++) {
    for (size_t j = 0;j<m_vpGeoConverters[i]->SupportedExt().size();j++) {
      v.push_back(tr1::make_tuple(
                      SysTools::ToLowerCase(
                        m_vpGeoConverters[i]->SupportedExt()[j]
                      ),
                      m_vpGeoConverters[i]->GetDesc(),
                      m_vpGeoConverters[i]->CanExportData()));
    }
  }
  return v;
}

bool IOManager::AnalyzeDataset(const string& strFilename, RangeInfo& info,
                               const string& strTempDir) const {
  // find the right converter to handle the dataset
  string strExt = SysTools::ToUpperCase(SysTools::GetExt(strFilename));

  if (strExt == "UVF") {
    UVFDataset v(strFilename,m_iMaxBrickSize,false);
    if (!v.IsOpen()) return false;

    UINT64 iComponentCount = v.GetComponentCount();
    bool bSigned = v.GetIsSigned();
    bool bIsFloat = v.GetIsFloat();

    if (iComponentCount != 1) return false;  // only scalar data supported at the moment

    info.m_fRange.first = v.GetRange().first;
    info.m_fRange.second = v.GetRange().second;

    // as our UVFs are always quantized to either 8bit or 16bit right now only the
    // nonfloat + unsigned path is taken, the others are for future extensions
    if (bIsFloat) {
      info.m_iValueType = 0;
    } else {
      if (bSigned) {
        info.m_iValueType = 1;
      } else {
        info.m_iValueType = 2;
      }
    }

    info.m_vAspect = FLOATVECTOR3(v.GetScale());
    info.m_vDomainSize = v.GetDomainSize();
    info.m_iComponentSize = v.GetBitWidth();

    return true;
  } else {
    bool bAnalyzed = false;
    for (size_t i = 0;i<m_vpConverters.size();i++) {
      const vector<string>& vStrSupportedExt = m_vpConverters[i]->SupportedExt();
      for (size_t j = 0;j<vStrSupportedExt.size();j++) {
        if (vStrSupportedExt[j] == strExt) {
          bAnalyzed = m_vpConverters[i]->Analyze(strFilename, strTempDir, false, info);
          if (bAnalyzed) break;
        }
      }
      if (bAnalyzed) break;
    }

    if (!bAnalyzed && m_pFinalConverter) {
      bAnalyzed = m_pFinalConverter->Analyze(strFilename, strTempDir, false, info);
    }

    return bAnalyzed;
  }
}

struct MergeableDatasets : public std::binary_function<Dataset, Dataset, bool> {
  bool operator()(const Dataset& a, const Dataset& b) const {
    if(a.GetComponentCount() != b.GetComponentCount() ||
       a.GetBrickOverlapSize() != b.GetBrickOverlapSize()) {
      return false;
    }

    const UINT64 timesteps = a.GetNumberOfTimesteps();
    if(timesteps != b.GetNumberOfTimesteps()) { return false; }

    const UINT64 LoDs = a.GetLODLevelCount();
    if(LoDs != b.GetLODLevelCount()) { return false; }

    for(UINT64 ts=0; ts < timesteps; ++ts) {
      for(UINT64 level=0; level < LoDs; ++level) {
        if(a.GetDomainSize() != b.GetDomainSize() ||
           a.GetBrickCount(level, ts) != b.GetBrickCount(level, ts)) {
          return false;
        }
      }
    }

    return true;
  }
};

namespace {
  // interpolate a chunk of data into a new range.
  template<typename IForwIter, typename OForwIter, typename U>
  void interpolate(IForwIter ibeg, IForwIter iend,
                   const std::pair<double,double>& src_range,
                   OForwIter obeg)
  {
    const U max_out = std::numeric_limits<U>::max();
    assert(src_range.second >= src_range.first);
    const double diff = src_range.second - src_range.first;
    const U ifactor = max_out / diff;
    while(ibeg != iend) {
      U value = *ibeg;
      *obeg = (value - src_range.first) * ifactor;
      ++obeg;
      ++ibeg;
    }
  }
}

const RasterDataBlock* GetFirstRDB(const UVF& uvf)
{
  for(UINT64 i=0; i < uvf.GetDataBlockCount(); ++i) {
    if(uvf.GetDataBlock(i)->GetBlockSemantic() == UVFTables::BS_REG_NDIM_GRID)
    {
      return dynamic_cast<const RasterDataBlock*>(uvf.GetDataBlock(i));
    }
  }
  return NULL;
}

namespace {
  // a minmax algorithm that doesn't suck.  Namely, it takes an input iterator
  // instead of a forward iterator, *as it should*.  Jesus.
  // Also it returns 'T's, so you don't have to deref the return value.
  template<typename T, typename InputIterator>
  std::pair<T,T> minmax_input(InputIterator begin, InputIterator end) {
    std::pair<T,T> retval = std::make_pair( std::numeric_limits<T>::max(),
                                           -std::numeric_limits<T>::max());
    while(begin != end) {
      retval.first  = std::min(*begin, retval.first);
      retval.second = std::max(*begin, retval.second);
      ++begin;
    }
    return retval;
  }
}

// converts 1D brick indices into RDB's indices.
std::vector<UINT64> NDBrickIndex(const RasterDataBlock* rdb,
                                 size_t LoD, size_t b) {
  UINT64 brick = static_cast<UINT64>(b);
  std::vector<UINT64> lod(1);
  lod[0] = LoD;
  const std::vector<UINT64>& counts = rdb->GetBrickCount(lod);

  UINT64 z = static_cast<UINT64>(brick / (counts[0] * counts[1]));
  brick = brick % (counts[0] * counts[1]);
  UINT64 y = static_cast<UINT64>(brick / counts[0]);
  brick = brick % counts[0];
  UINT64 x = brick;

  std::vector<UINT64> vec(3);
  vec[0] = x;
  vec[1] = y;
  vec[2] = z;
  return vec;
}

namespace {
  /// Computes the minimum and maximum for a single brick in a raster data block.
  template<typename T>
  DOUBLEVECTOR4 get_brick_minmax(const RasterDataBlock* rdb,
                                 const std::vector<UINT64>& vLOD,
                                 const std::vector<UINT64>& vBrick)
  {
    DOUBLEVECTOR4 mmv;
    // min/max of gradients not supported...
    mmv.z = -std::numeric_limits<double>::max();
    mmv.w =  std::numeric_limits<double>::max();

    std::vector<T> data;
    rdb->GetData(data, vLOD, vBrick);
    std::pair<T,T> mm = minmax_input<T>(data.begin(), data.end());
    mmv.x = mm.first;
    mmv.y = mm.second;

    return mmv;
  }
}

/// Calculates the min/max scalar and gradient for every brick in a data set.
std::vector<DOUBLEVECTOR4>
MaxMin(const RasterDataBlock* rdb)
{
  const bool is_float = rdb->ulElementMantissa[0][0];
  const bool is_signed = rdb->bSignedElement[0][0];
  const UINT64 bit_width = rdb->ulElementBitSize[0][0];
  std::vector<DOUBLEVECTOR4> mm;

  // We iterate over each LoD.  At each one, we iterate through the bricks.
  // When a GetData fails for that brick, we know we need to move on to the
  // next LoD.  When a GetData fails and we're at brick 0, we know we're done
  // with all of the LoDs.
  std::vector<UINT64> vLOD(1);
  size_t brick;
  vLOD[0] = 0;
  do {
    brick = 0;
    do {
      assert(rdb->ValidBrickIndex(vLOD, NDBrickIndex(rdb, vLOD[0], brick)));
      std::vector<UINT64> b_idx = NDBrickIndex(rdb, vLOD[0], brick);
      MESSAGE("%llu,%zu -> %llu,%llu,%llu", vLOD[0], brick,
              b_idx[0], b_idx[1], b_idx[2]);

      if(is_float && bit_width == 32) {
        assert(is_signed);
        mm.push_back(get_brick_minmax<float>(rdb, vLOD, b_idx));
      } else if(is_float && bit_width == 64) {
        assert(is_signed);
        mm.push_back(get_brick_minmax<double>(rdb, vLOD, b_idx));
      } else if( is_signed &&  8 == bit_width) {
        mm.push_back(get_brick_minmax<int8_t>(rdb, vLOD, b_idx));
      } else if(!is_signed &&  8 == bit_width) {
        mm.push_back(get_brick_minmax<uint8_t>(rdb, vLOD, b_idx));
      } else if( is_signed && 16 == bit_width) {
        mm.push_back(get_brick_minmax<int16_t>(rdb, vLOD, b_idx));
      } else if(!is_signed && 16 == bit_width) {
        mm.push_back(get_brick_minmax<uint16_t>(rdb, vLOD, b_idx));
      } else if( is_signed && 32 == bit_width) {
        mm.push_back(get_brick_minmax<int32_t>(rdb, vLOD, b_idx));
      } else if(!is_signed && 32 == bit_width) {
        mm.push_back(get_brick_minmax<uint32_t>(rdb, vLOD, b_idx));
      } else if( is_signed && 64 == bit_width) {
        T_ERROR("int64_t unsupported...");
        double mn = -std::numeric_limits<double>::max();
        double mx =  std::numeric_limits<double>::max();
        mm.push_back(DOUBLEVECTOR4(mn,mx, mn,mx));
        assert(1 == 0);
      } else if(!is_signed && 64 == bit_width) {
        T_ERROR("uint64_t unsupported...");
        double mn = -std::numeric_limits<double>::max();
        double mx =  std::numeric_limits<double>::max();
        mm.push_back(DOUBLEVECTOR4(mn,mx, mn,mx));
        assert(1 == 0);
      } else {
        T_ERROR("Unsupported data type!");
        assert(1 == 0);
      }
      MESSAGE("Finished lod,brick %u,%u", static_cast<unsigned>(vLOD[0]),
              static_cast<unsigned>(brick));
      ++brick;
    } while(rdb->ValidBrickIndex(vLOD, NDBrickIndex(rdb, vLOD[0], brick)));
    vLOD[0]++;
  } while(rdb->ValidLOD(vLOD));
  return mm;
}

void
CreateUVFFromRDB(const std::string& filename,
                 const RasterDataBlock* rdb)
{
  std::wstring wide_fn(filename.begin(), filename.end());
  UVF outuvf(wide_fn);
  outuvf.Create();

  GlobalHeader gh;
  gh.bIsBigEndian = EndianConvert::IsBigEndian();
  gh.ulChecksumSemanticsEntry = UVFTables::CS_MD5;
  outuvf.SetGlobalHeader(gh);

  outuvf.AddConstDataBlock(rdb);

  // create maxmin accel structures.  We'll need the maximum scalar
  // later, too, for computation of the 2D histogram.
  double max_val = DBL_MAX;
  {
    const size_t components = rdb->ulElementDimensionSize[0];
    MaxMinDataBlock mmdb(components);
    std::vector<DOUBLEVECTOR4> minmax = MaxMin(rdb);
    MESSAGE("found %u brick min/maxes...",
            static_cast<unsigned>(minmax.size()));
    for(std::vector<DOUBLEVECTOR4>::const_iterator i = minmax.begin();
        i != minmax.end(); ++i) {
      // get the maximum maximum (that makes sense, I swear ;)
      max_val = std::max(max_val, i->y);

      // merge in the current brick's minmax.
      mmdb.StartNewValue();
      std::vector<DOUBLEVECTOR4> tmp(1);
      tmp[0] = *i;
      mmdb.MergeData(tmp);
    }

    outuvf.AddDataBlock(&mmdb);
  }

  { // histograms
    Histogram1DDataBlock hist1d;
    hist1d.Compute(rdb);
    outuvf.AddDataBlock(&hist1d);
    {
      Histogram2DDataBlock hist2d;
      hist2d.Compute(rdb, hist1d.GetHistogram().size(), max_val);
      outuvf.AddDataBlock(&hist2d);
    }
  }

  outuvf.Close();
}


// Identifies the 'widest' type that is utilized in a series of UVFs.
// For example, if we've got FP data in one UVF and unsigned bytes in
// another, the 'widest' type is FP.
void IdentifyType(const std::vector<std::tr1::shared_ptr<UVFDataset> >& uvf,
                  size_t& bit_width, bool& is_float, bool &is_signed)
{
  bit_width = 0;
  is_float = false;
  is_signed = false;

  for(size_t i=0; i < uvf.size(); ++i) {
    bit_width = std::max(bit_width, static_cast<size_t>(uvf[i]->GetBitWidth()));
    assert(true > false);
    is_float = std::max(is_float, uvf[i]->GetIsFloat());
    is_signed = std::max(is_signed, uvf[i]->GetIsSigned());
  }
}

// Reads in data of the given type.  If data is not stored that way in
// the file, it will expand it out to the given type.  Assumes it will
// always be expanding data, never compressing it!
template<typename T>
void TypedRead(std::vector<T>& data,
               const Dataset& ds,
               const BrickKey& key)
{
  size_t width = ds.GetBitWidth();
  bool is_signed = ds.GetIsSigned();
  bool is_float = ds.GetIsFloat();

  size_t dest_width = sizeof(T) * 8;
  bool dest_signed = ctti<T>::is_signed;
  bool dest_float = std::tr1::is_floating_point<T>::value;

  // fp data implies signed data.
  assert(is_float ? is_signed : true);
  assert(dest_float ? dest_signed : true);

  MESSAGE(" [Source Data] Signed: %d  Float: %d  Width: %u",
          is_signed, is_float, static_cast<unsigned>(width));
  MESSAGE(" [Destination] Signed: %d  Float: %d  Width: %u",
          dest_signed, dest_float, static_cast<unsigned>(dest_width));

  // If we're lucky, we can just read the data and be done with it.
  if(dest_width == width && dest_signed == is_signed &&
     dest_float == is_float) {
    MESSAGE("Data is stored the way we need it!  Yay.");
    ds.GetBrick(key, data);
    return;
  }

  // Otherwise we'll need to read it into a temporary buffer and expand
  // it into the argument vector.
  std::pair<double, double> range = ds.GetRange();

  if(is_float && width == 32) {
    std::vector<float> tmpdata;
    ds.GetBrick(key, tmpdata);
    data.resize(tmpdata.size() / (width/8));
    interpolate<float*, typename std::vector<T>::iterator, T>(
      &tmpdata[0], (&tmpdata[0]) + tmpdata.size(), range, data.begin()
    );
  } else if(is_float && width == 64) {
    // Can this happen?  What would we expand double into?
    std::vector<double> tmpdata;
    ds.GetBrick(key, tmpdata);
    data.resize(tmpdata.size() / (width/8));
    interpolate<double*, typename std::vector<T>::iterator, T>(
      &tmpdata[0], (&tmpdata[0]) + tmpdata.size(), range, data.begin()
    );
  } else if( is_signed &&  8 == width) {
    std::vector<int8_t> tmpdata;
    ds.GetBrick(key, tmpdata);
    data.resize(tmpdata.size() / (width/8));
    interpolate<int8_t*, typename std::vector<T>::iterator, T>(
      &tmpdata[0], (&tmpdata[0]) + tmpdata.size(), range, data.begin()
    );
  } else if(!is_signed &&  8 == width) {
    std::vector<uint8_t> tmpdata;
    ds.GetBrick(key, tmpdata);
    data.resize(tmpdata.size() / (width/8));
    interpolate<uint8_t*, typename std::vector<T>::iterator, T>(
      &tmpdata[0], (&tmpdata[0]) + tmpdata.size(), range, data.begin()
    );
  } else if( is_signed && 16 == width) {
    std::vector<int16_t> tmpdata;
    ds.GetBrick(key, tmpdata);
    data.resize(tmpdata.size() / (width/8));
    interpolate<int16_t*, typename std::vector<T>::iterator, T>(
      &tmpdata[0], (&tmpdata[0]) + tmpdata.size(), range, data.begin()
    );
  } else if(!is_signed && 16 == width) {
    std::vector<uint16_t> tmpdata;
    ds.GetBrick(key, tmpdata);
    data.resize(tmpdata.size() / (width/8));
    interpolate<uint16_t*, typename std::vector<T>::iterator, T>(
      &tmpdata[0], (&tmpdata[0]) + tmpdata.size(), range, data.begin()
    );
  } else if( is_signed && 32 == width) {
    std::vector<int32_t> tmpdata;
    ds.GetBrick(key, tmpdata);
    data.resize(tmpdata.size() / (width/8));
    interpolate<int32_t*, typename std::vector<T>::iterator, T>(
      &tmpdata[0], (&tmpdata[0]) + tmpdata.size(), range, data.begin()
    );
  } else if(!is_signed && 32 == width) {
    std::vector<uint32_t> tmpdata;
    ds.GetBrick(key, tmpdata);
    data.resize(tmpdata.size() / (width/8));
    interpolate<uint32_t*, typename std::vector<T>::iterator, T>(
      &tmpdata[0], (&tmpdata[0]) + tmpdata.size(), range, data.begin()
    );
  } else {
    T_ERROR("Unhandled data type!  Width: %u, Signed: %d, Float: %d",
            static_cast<unsigned>(width), is_signed, is_float);
  }
}

namespace {
  template<typename T>
  void ReadAndEvalBrick(
    RasterDataBlock* rdb,
    const std::vector<std::tr1::shared_ptr<UVFDataset> >& uvfs,
    const std::vector<BrickTable::const_iterator>& iters,
    tuvok::expression::Node* tree
  ) {
    std::vector<std::vector<T> > involumes(uvfs.size());
    std::vector<T> output;
    for(size_t i=0; i < uvfs.size(); ++i) {
      MESSAGE("Reading brick from volume %u/%u...", static_cast<unsigned>(i+1),
              static_cast<unsigned>(uvfs.size()));
      TypedRead<T>(involumes[i], *uvfs[i], iters[i]->first);
    }
    MESSAGE("Evaluating expression ...");
    tuvok::expression::evaluate(*tree, involumes, output);

    MESSAGE("Writing ...");
    NDBrickKey nk = uvfs[0]->IndexToVectorKey(iters[0]->first);
    if(false == rdb->SetData(&output[0], nk.lod, nk.brick)) {
      T_ERROR("Write failed!");
    }
  }
}

struct cleanup_parser {
  ~cleanup_parser() { parser_free(); }
};

void
IOManager::EvaluateExpression(const char* expr,
                              const std::vector<std::string> volumes,
                              const std::string& out_fn) const
                              throw(tuvok::Exception)
{
  parser_set_string(expr);
  int parse_err = yyparse();
  std::auto_ptr<cleanup_parser> p(new cleanup_parser());
  assert(!volumes.empty());

  if(parse_err == 1) {
    throw tuvok::expression::SyntaxError("", 0, 2, __FILE__, __LINE__);
  }

  // open all of those files and get UVF datasets for each of them.
  const bool verify=false;
  std::vector<std::tr1::shared_ptr<UVFDataset> > uvf;
  typedef std::vector<std::string>::const_iterator citer;
  for(citer f = volumes.begin(); f != volumes.end(); ++f) {
    uvf.push_back(std::tr1::shared_ptr<UVFDataset>(
      dynamic_cast<UVFDataset*>(
        m_dsFactory->Create(*f, 256 /* hack! */, verify)
      )
    ));
    uvf[uvf.size()-1]->ComputeRange();
  }
  // ensure those UVFs are "equal" in some sense (same number of voxels, etc).
  MergeableDatasets mergeable;
  typedef std::vector<std::tr1::shared_ptr<UVFDataset> >::const_iterator uiter;
  for(uiter u = uvf.begin(); u != uvf.end(); ++u) {
    if(!mergeable(**uvf.begin(), **u)) {
      throw tuvok::io::UnmergeableDatasets("Incompatible input volumes",
                                           __FILE__, __LINE__);
    }
  }

  tuvok::expression::Node* tree = parser_tree_root();

  // volume iterators
  std::vector<BrickTable::const_iterator> viters(uvf.size());
  for(size_t i=0; i < viters.size(); ++i) { // initialize
    viters[i] = uvf[i]->BricksBegin();
  }

  RasterDataBlock* rdb = new RasterDataBlock();
  rdb->SetBlockSemantic(UVFTables::BS_REG_NDIM_GRID);
  rdb->SetIdentityTransformation();
  rdb->SetTypeToUShort(UVFTables::ES_RED);

  { // Copy the other basic info from the first input volume.
    std::wstring wide_vol(volumes[0].begin(), volumes[0].end());
    UVF firstvol(wide_vol);
    firstvol.Open(false, false, false);
    const RasterDataBlock* rdb1 = GetFirstRDB(firstvol);
    assert(rdb1);
    *rdb = *rdb1;
  }

  std::string tmp_fn = SysTools::RemoveExt(out_fn) + ".rdb";
  std::auto_ptr<LargeRAWFile> lout = std::auto_ptr<LargeRAWFile>
                                                  (new LargeRAWFile(tmp_fn));
  lout->Create();
  rdb->ResetFile(&*lout);

  // Figure out which what type our output data should be.
  size_t bit_width;
  bool is_float, is_signed;
  IdentifyType(uvf, bit_width, is_float, is_signed);

  //     foreach brick:
  //       load brick into 'involumes'
  //       evaluate(tree, input-bricks-in-a-vector, output)
  //       write output somewhere
  /// @todo FIXME: we should query bit_width, is_float, is_signed to create different
  /// 'involumes' based on the type we need...
  size_t brick = 0;
  while(viters[0] != uvf[0]->BricksEnd()) {
    for(size_t i=0; i < uvf.size(); ++i) {
      MESSAGE("Brick %u (file %03u/%03u)...", static_cast<unsigned>(brick),
              static_cast<unsigned>(i+1), static_cast<unsigned>(uvf.size()));
      // Read in the data we need.
      if(is_float && bit_width == 32) {
        ReadAndEvalBrick<float>(rdb, uvf, viters, tree);
      } else if(is_float && bit_width == 64) {
        // Not implemented in UVF...
        T_ERROR("double format data not supported!");
        continue;
      } else if( is_signed && bit_width ==  8) {
        ReadAndEvalBrick< int8_t>(rdb, uvf, viters, tree);
      } else if(!is_signed && bit_width ==  8) {
        ReadAndEvalBrick<uint8_t>(rdb, uvf, viters, tree);
      } else if( is_signed && bit_width == 16) {
        ReadAndEvalBrick< int16_t>(rdb, uvf, viters, tree);
      } else if(!is_signed && bit_width == 16) {
        ReadAndEvalBrick<uint16_t>(rdb, uvf, viters, tree);
      // These types aren't yet implemented in UVF/RasterDataBlock.
      } else if( is_signed && bit_width == 32) {
        T_ERROR("32bit signed int data not implemented!");
        //ReadAndEvalBrick< int32_t>(rdb, uvf, viters, tree);
      } else if(!is_signed && bit_width == 32) {
        T_ERROR("32bit unsigned data not implemented!");
        //ReadAndEvalBrick<uint32_t>(rdb, uvf, viters, tree);
      } else if( is_signed && bit_width == 64) {
        T_ERROR("64bit signed int data not implemented!");
        //ReadAndEvalBrick< int64_t>(rdb, uvf, viters, tree);
      } else if(!is_signed && bit_width == 64) {
        T_ERROR("64bit unsigned data not implemented!");
        //ReadAndEvalBrick<uint64_t>(rdb, uvf, viters, tree);
      } else {
        T_ERROR("Could not figure out destination data type!");
      }
    }
    MESSAGE("Brick %u (evaluation)...", static_cast<unsigned>(brick));

    using namespace std::tr1::placeholders;
    // advance each brick iterator by one.
    std::for_each(viters.begin(), viters.end(),
                  std::tr1::bind(std::advance<BrickTable::const_iterator,int>,
                                 _1, 1));
  }

  CreateUVFFromRDB(out_fn, rdb);

  delete rdb;
}

bool IOManager::ReBrickDataset(const string& strSourceFilename,
                               const string& strTargetFilename,
                               const string& strTempDir,
                               const UINT64 iMaxBrickSize,
                               const UINT64 iBrickOverlap,
                               bool bQuantizeTo8Bit) const {
  MESSAGE("Rebricking (Phase 1/2)...");

  string filenameOnly = SysTools::GetFilename(strSourceFilename);
  string tmpFile = strTempDir+SysTools::ChangeExt(filenameOnly,"nrrd"); /// use some simple format as intermediate file

  if (!ConvertDataset(strSourceFilename, tmpFile, strTempDir)) {
    T_ERROR("Unable to extract raw data from file %s to %s", strSourceFilename.c_str(),tmpFile.c_str());
    return false;
  }

  MESSAGE("Rebricking (Phase 2/2)...");

  if (!Controller::Instance().IOMan()->ConvertDataset(tmpFile, strTargetFilename, strTempDir, true, iMaxBrickSize, iBrickOverlap,bQuantizeTo8Bit)) {
    T_ERROR("Unable to convert raw data from file %s into new UVF file %s", tmpFile.c_str(),strTargetFilename.c_str());
    if(remove(tmpFile.c_str()) == -1) WARNING("Unable to delete temp file %s", tmpFile.c_str());
    return false;
  }
  if(remove(tmpFile.c_str()) == -1) WARNING("Unable to delete temp file %s", tmpFile.c_str());

  return true;
}


void IOManager::CopyToTSB(const Mesh* m, GeometryDataBlock* tsb) const {
  // source data
  const VertVec&      v = m->GetVertices();
  const NormVec&      n = m->GetNormals();
  const TexCoordVec&  t = m->GetTexCoords();
  const ColorVec&     c = m->GetColors();

  // target data
  vector<float> fVec;
  size_t iVerticesPerPoly = m->GetVerticesPerPoly();
  tsb->SetPolySize(iVerticesPerPoly );

  if (v.size()) {fVec.resize(v.size()*3); memcpy(&fVec[0],&v[0],v.size()*3*sizeof(float)); tsb->SetVertices(fVec);}
  if (n.size()) {fVec.resize(n.size()*3); memcpy(&fVec[0],&n[0],n.size()*3*sizeof(float)); tsb->SetNormals(fVec);}
  if (t.size()) {fVec.resize(t.size()*2); memcpy(&fVec[0],&t[0],t.size()*2*sizeof(float)); tsb->SetTexCoords(fVec);}
  if (c.size()) {fVec.resize(c.size()*4); memcpy(&fVec[0],&c[0],c.size()*4*sizeof(float)); tsb->SetColors(fVec);}

  tsb->SetVertexIndices(m->GetVertexIndices());
  tsb->SetNormalIndices(m->GetNormalIndices());
  tsb->SetTexCoordIndices(m->GetTexCoordIndices());
  tsb->SetColorIndices(m->GetColorIndices());

  tsb->m_Desc = m->Name();
}


Mesh* IOManager::LoadMesh(const string& meshfile) const
{
  MESSAGE("Opening Mesh File ...");

  // iterate through all our converters, stopping when one successfully
  // converts our data.
  Mesh* m = NULL;
  for(vector<AbstrGeoConverter*>::const_iterator conv =
      m_vpGeoConverters.begin(); conv != m_vpGeoConverters.end(); ++conv) {
    MESSAGE("Attempting converter '%s'", (*conv)->GetDesc().c_str());
    if((*conv)->CanRead(meshfile)) {
      MESSAGE("Converter '%s' can read '%s'!",
              (*conv)->GetDesc().c_str(), meshfile.c_str());
      try {
        m = (*conv)->ConvertToMesh(meshfile);
      } catch (const tuvok::io::DSOpenFailed& err) {
        WARNING("Converter %s can read files, but conversion failed! %s",
                (*conv)->GetDesc().c_str(), err.what());
        throw;
      }
      break;
    }
  }
  return m;
}

void IOManager::AddMesh(const UVF* sourceDataset,
                           const string& meshfile,
                           const string& uvf_fn) const
{
  Mesh* m = LoadMesh(meshfile);

  if (m == NULL) {
    WARNING("No converter for geometry file %s can be found",
            meshfile.c_str());
    throw tuvok::io::DSOpenFailed(meshfile.c_str(), __FILE__, __LINE__);
  }

  // make sure we have at least normals
  if (m->GetNormalIndices().empty()) m->RecomputeNormals();

  // now create a GeometryDataBlock ...
  GeometryDataBlock tsb;

  // ... and transfer the data from the mesh object
  CopyToTSB(m, &tsb);

  wstring wuvf(uvf_fn.begin(), uvf_fn.end());
  UVF uvfFile(wuvf);
  GlobalHeader uvfGlobalHeader;
  uvfGlobalHeader.bIsBigEndian = EndianConvert::IsBigEndian();
  uvfGlobalHeader.ulChecksumSemanticsEntry = UVFTables::CS_MD5;
  uvfFile.SetGlobalHeader(uvfGlobalHeader);

  for(UINT64 i = 0; i<sourceDataset->GetDataBlockCount(); i++) {
    uvfFile.AddConstDataBlock(sourceDataset->GetDataBlock(i));
  }

  MESSAGE("Adding triangle soup block...");
  uvfFile.AddDataBlock(&tsb, true);

  uvfFile.Create();
  MESSAGE("Computing checksum...");
  uvfFile.Close();
}

bool IOManager::SetMaxBrickSize(const UINT64 iMaxBrickSize) {
  if (iMaxBrickSize > m_iBrickOverlap) {
    m_iMaxBrickSize = iMaxBrickSize;
    return true;
  } else return false;
}

bool IOManager::SetBrickOverlap(const UINT64 iBrickOverlap) {
  if (m_iMaxBrickSize > iBrickOverlap) {
    m_iBrickOverlap = iBrickOverlap;
    return true;
  } else return false;
}
