/*  ===========================================================================
*
*   This file is part of HISE.
*   Copyright 2016 Christoph Hart
*
*   HISE is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, either version 3 of the License, or
*   (at your option) any later version.
*
*   HISE is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with HISE.  If not, see <http://www.gnu.org/licenses/>.
*
*   Commercial licenses for using HISE in an closed source project are
*   available on request. Please visit the project's website to get more
*   information about commercial licensing:
*
*   http://www.hise.audio/
*
*   HISE is based on the JUCE library,
*   which must be separately licensed for closed source applications:
*
*   http://www.juce.com
*
*   ===========================================================================
*/

/******************************************************************************

BEGIN_JUCE_MODULE_DECLARATION

  ID:               hi_core
  vendor:           Hart Instruments
  version:          1.6.0
  name:             HISE Core module
  description:      The core classes for HISE
  website:          http://hise.audio
  license:          GPL / Commercial

  dependencies:      juce_audio_basics, juce_audio_devices, juce_audio_formats, juce_audio_processors, juce_core, juce_cryptography, juce_data_structures, juce_events, juce_graphics, juce_gui_basics, juce_gui_extra, juce_opengl, hi_lac
  OSXFrameworks:    Accelerate
  iOSFrameworks:    Accelerate

END_JUCE_MODULE_DECLARATION

******************************************************************************/

#ifndef HI_CORE_INCLUDED
#define HI_CORE_INCLUDED



#include "AppConfig.h"

#ifndef DOUBLE_TO_STRING_DIGITS
#define DOUBLE_TO_STRING_DIGITS 8
#endif


#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_product_unlocking/juce_product_unlocking.h>
#include <juce_opengl/juce_opengl.h>
#include <juce_dsp/juce_dsp.h>
#include <hi_streaming/hi_streaming.h>


#include <complex>


#ifndef HISE_VERSION
#define HISE_VERSION "1.6.0"
#endif


//=============================================================================
/** Config: USE_BACKEND

If true, then the plugin uses the backend system including IDE editor & stuff.
*/
#ifndef USE_BACKEND
#define USE_BACKEND 0
#endif

/** Config: USE_FRONTEND

If true, this project uses the frontend module and some special file reference handling.
*/
#ifndef USE_FRONTEND
#define USE_FRONTEND 1
#endif

/** Config: IS_STANDALONE_APP

If true, then this will use some additional features for the standalone app (popup out windows, audio device settings etc.)
*/
#ifndef IS_STANDALONE_APP
#define IS_STANDALONE_APP 0
#endif

/** Config: USE_COPY_PROTECTION

If true, then the copy protection will be used
*/
#ifndef USE_COPY_PROTECTION
#define USE_COPY_PROTECTION 0
#endif

/** Config: USE_IPP

Use the Intel Performance Primitives Library for the convolution reverb.
*/
#ifndef USE_IPP
#define USE_IPP 1
#endif

/** Config: USE_VDSP_FFT
*
* Use the vDsp FFT on Apple devices.
*/
#ifndef USE_VDSP_FFT
#define USE_VDSP_FFT 0
#endif

/** Config: FRONTEND_IS_PLUGIN

If set to 1, the compiled plugin will be a effect (stereo in / out). */
#ifndef FRONTEND_IS_PLUGIN
#define FRONTEND_IS_PLUGIN 0
#endif

/** Config: USE_CUSTOM_FRONTEND_TOOLBAR

If set to 1, you can specify a customized toolbar class which will be used instead of the default one. 
*/
#ifndef USE_CUSTOM_FRONTEND_TOOLBAR
#define USE_CUSTOM_FRONTEND_TOOLBAR 0
#endif

/** Config: IS_STANDALONE_FRONTEND

If set to 1, you can specify a customized toolbar class which will be used instead of the default one. 
*/
#ifndef IS_STANDALONE_FRONTEND
#define IS_STANDALONE_FRONTEND 0
#endif

/** Config: USE_GLITCH_DETECTION

Enable this to add a glitch detector to some performance crititcal functions
*/
#ifndef USE_GLITCH_DETECTION
#define USE_GLITCH_DETECTION 0
#endif

/** Config: ENABLE_PLOTTER

Set this to 0 to deactivate the plotter data collection
*/
#ifndef ENABLE_PLOTTER
#define ENABLE_PLOTTER 1
#endif



/** Config: ENABLE_SCRIPTING_SAFE_CHECKS

Set this to 0 to deactivate the safe checks for scripting
*/
#ifndef ENABLE_SCRIPTING_SAFE_CHECKS
#define ENABLE_SCRIPTING_SAFE_CHECKS 1
#endif

/** Config: CRASH_ON_GLITCH
 
If this is set to 1, the application will crash instantly if there is a drop out or a burst in the signal (values above 32dB = +36dB ). Use this to get a crash dump with the location.
*/
#ifndef CRASH_ON_GLITCH
#define CRASH_ON_GLITCH 0
#endif


/** Config: ENABLE_SCRIPTING_BREAKPOINTS

*/
#ifndef ENABLE_SCRIPTING_BREAKPOINTS
#define ENABLE_SCRIPTING_BREAKPOINTS 0
#endif

/** Config: ENABLE_ALL_PEAK_METERS

Set this to 0 to deactivate peak collection for any other processor than the main synth chain
*/
#ifndef ENABLE_ALL_PEAK_METERS
#define ENABLE_ALL_PEAK_METERS 1
#endif

/** Config: ENABLE_CONSOLE_OUTPUT

Set this to 0 to deactivate the console output
*/
#ifndef ENABLE_CONSOLE_OUTPUT
#define ENABLE_CONSOLE_OUTPUT 1
#endif

/** Config: ENABLE_HOST_INFO

Set this to 0 to disable host information like tempo, playing position etc...
*/
#ifndef ENABLE_HOST_INFO
#define ENABLE_HOST_INFO 1
#endif


/** Config: ENABLE_STARTUP_LOGGER

If this is enabled, compiled plugins will write a startup log to the desktop for debugging purposes
*/
#ifndef ENABLE_STARTUP_LOG
#define ENABLE_STARTUP_LOG 0
#endif



/** Config: ENABLE_CPU_MEASUREMENT

Set this to 0 to deactivate the CPU peak meter.
*/
#ifndef ENABLE_CPU_MEASUREMENT
#define ENABLE_CPU_MEASUREMENT 1
#endif


#ifndef ENABLE_APPLE_SANDBOX
#define ENABLE_APPLE_SANDBOX 0
#endif

/** Config: USE_HARD_CLIPPER

Set this to 1 to enable hard clipping of the output (brickwall everything over 1.0)
*/
#ifndef USE_HARD_CLIPPER
#define USE_HARD_CLIPPER 0
#endif

/** Config: USE_SPLASH_SCREEN

If your project contains a SplashScreen.png image file, it will use this as splash screen while loading the instrument in the background.
*/
#ifndef USE_SPLASH_SCREEN
#define USE_SPLASH_SCREEN 0
#endif

// for iOS apps, the external files don't need to be embedded. Enable this to simulate this behaviour on desktop projects (not recommended for production)
//#define DONT_EMBED_FILES_IN_FRONTEND 1

#if JUCE_IOS
#ifndef DONT_EMBED_FILES_IN_FRONTEND
#define DONT_EMBED_FILES_IN_FRONTEND 1
#endif

#ifndef HISE_IOS
#define HISE_IOS 1
#endif
#endif



/**Appconfig file

Use this file to enable the modules that are needed

For all defined variables:

- 1 if the module is used
- 0 if the module should not be used

*/


/** Add new subgroups here and in hi_module.cpp
*
*	New files must be added in the specific subfolder header / .cpp file.
*/


#if USE_IPP
#include "ipp.h"
#endif



#include "hi_binary_data/hi_binary_data.h"

#include "LibConfig.h"
#include "Macros.h"

#include "additional_libraries/additional_libraries.h"

#include "hi_core/hi_core.h" // has its own namespace definition







#endif   // HI_CORE_INCLUDED
