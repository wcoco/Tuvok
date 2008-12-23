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
  \file    StdTuvokDefines.h
  \author    Jens Krueger
        SCI Institute
        University of Utah
  \date    December 2008
*/

#pragma once

#ifndef STDTUVOKDEFINES_H
#define STDTUVOKDEFINES_H

#include <limits>

#ifdef _WIN32
  typedef unsigned __int64 UINT64;
#else
  typedef unsigned long long UINT64;
#endif
typedef unsigned int UINT32;

#define UNUSED 0
#define UNUSED_FLOAT 0.0f
#define UNUSED_DOUBLE 0.0
#define TUVOK_VERSION "0.02 beta"

#ifdef _WIN32
  #ifdef _WIN64
    #ifdef USE_DIRECTX
      #define TUVOK_DETAILS "Windows 64bit build with DirectX extensions"
    #else
      #define TUVOK_DETAILS "Windows 64bit build"
    #endif
  #else
    #ifdef USE_DIRECTX
      #define TUVOK_DETAILS "Windows 32bit build with DirectX extensions"
    #else
      #define TUVOK_DETAILS "Windows 32bit build"
    #endif
  #endif
#else
#if defined(macintosh) || (defined(__MACH__) && defined(__APPLE__))
  #define TUVOK_DETAILS "OSX build"
#else
  #define TUVOK_DETAILS "Linux build"
#endif


#endif


#define UINT32_INVALID (std::numeric_limits<UINT>::max())
#define UINT64_INVALID (std::numeric_limits<UINT64>::max())


#endif // STDTUVOKDEFINES_H