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

#ifndef STREAMINGSAMPLER_H_INCLUDED
#define STREAMINGSAMPLER_H_INCLUDED

namespace hise { using namespace juce;

class ModulatorSampler;

class StreamingSamplerSound;

struct StreamingHelpers
{
	/** This contains the minimal MIDI information that can be extracted from a SampleMap. */
	struct BasicMappingData
	{
		int8 lowKey = -1;
		int8 highKey = -1;
		int8 lowVelocity = -1;
		int8 highVelocity = -1;
		int8 rootNote = -1;
	};

	static void increaseBufferIfNeeded(hlac::HiseSampleBuffer& b, int numSamplesNeeded);

	static bool preloadSample(StreamingSamplerSound * s, const int preloadSize, String& errorMessage);

	/** Creates a BasicMappingData object from the given samplemap entry. */
	static BasicMappingData getBasicMappingDataFromSample(const ValueTree& sampleData);
};





class StreamingSamplerSoundPool
{
public:

	StreamingSamplerSoundPool()
	{
		afm.registerBasicFormats();
		afm.registerFormat(new hlac::HiseLosslessAudioFormat(), false);
	}

	virtual ~StreamingSamplerSoundPool()
	{

	}

	virtual void increaseNumOpenFileHandles()
	{
		++numOpenFileHandles;
	}

	virtual void decreaseNumOpenFileHandles()
	{
		--numOpenFileHandles;
		if (numOpenFileHandles < 0) numOpenFileHandles = 0;
	}

	AudioFormatManager afm;

	int getNumOpenFileHandles() const { return numOpenFileHandles; }

private:

	int numOpenFileHandles = 0;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StreamingSamplerSoundPool);
};



class DebugLogger;

// ==================================================================================================================================================

/** A simple data struct for a stereo channel.
*
*	This merges channels from an AudioSampleBuffer into one data structure which is filled by the sample rendering.
*/
struct StereoChannelData
{
	const void *leftChannel;
	const void *rightChannel;
	bool isFloatingPoint = false;
};

// ==================================================================================================================================================

// This class is a spin off of my upcoming sampler framework, so in order to use it in another project, leave this at '1'
#define STANDALONE 0


#if HISE_IOS
// This is the maximum value for sample pitch manipulation (this means 3 octaves, which should be more than enough
#define MAX_SAMPLER_PITCH 8
#else
#define MAX_SAMPLER_PITCH 16
#endif

// This is the default preload size. I defined a pretty random value here, but you can change this dynamically.
#define PRELOAD_SIZE 8192

// Same as the preload size.
#define BUFFER_SIZE_FOR_STREAM_BUFFERS 8192

// Deactivate this to use one rounded pitch value for one a buffer (crucial for other interpolation methods than linear interpolation)
#define USE_SAMPLE_ACCURATE_RESAMPLING 0

// You can set this to 0, if you want to disable background threaded reading. The files will then be read directly in the audio thread,
// which is not the smartest thing to do, but it comes to good use for debugging.
#define USE_BACKGROUND_THREAD 1

// If the streaming background thread is blocked, it will kill the voice to exit gracefully.
#define KILL_VOICES_WHEN_STREAMING_IS_BLOCKED 0

// By default, every voice adds its output to the supplied buffer. Depending on your architecture, it could be more practical to
// set (overwrite) the buffer. In this case, set this to 1.
#if STANDALONE
#define OVERWRITE_BUFFER_WITH_VOICE_DATA 0
#else
#define OVERWRITE_BUFFER_WITH_VOICE_DATA 1
#endif


#if JUCE_32BIT
#define NUM_UNMAPPERS 8
#else
#define NUM_UNMAPPERS 8
#endif



} // namespace hise
#endif  // STREAMINGSAMPLER_H_INCLUDED
