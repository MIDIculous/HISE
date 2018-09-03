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

namespace hise { using namespace juce;

#define MAX_SAMPLE_NUMBER 2147483647

// ==================================================================================================== StreamingSamplerSound methods

StreamingSamplerSound::StreamingSamplerSound(const String &fileNameToLoad, StreamingSamplerSoundPool *pool) :
	fileReader(this, pool),
	sampleRate(-1.0),
	purged(false),
	monolithOffset(0),
	monolithLength(0),
	preloadSize(0),
	internalPreloadSize(0),
	entireSampleLoaded(false),
	sampleStart(0),
	sampleEnd(MAX_SAMPLE_NUMBER),
	sampleLength(MAX_SAMPLE_NUMBER),
	sampleStartMod(0),
	loopEnabled(false),
	loopStart(0),
	loopEnd(MAX_SAMPLE_NUMBER),
	loopLength(0),
	crossfadeLength(0),
	crossfadeArea(Range<int>())
{
	fileReader.setFile(fileNameToLoad);

	setPreloadSize(0);
}

StreamingSamplerSound::StreamingSamplerSound(MonolithInfoToUse *info, int channelIndex, int sampleIndex) :
	fileReader(this, nullptr),
	sampleRate(-1.0),
	purged(false),
	monolithOffset(0),
	monolithLength(0),
	preloadSize(0),
	internalPreloadSize(0),
	entireSampleLoaded(false),
	sampleStart(0),
	sampleEnd(MAX_SAMPLE_NUMBER),
	sampleLength(MAX_SAMPLE_NUMBER),
	sampleStartMod(0),
	loopEnabled(false),
	loopStart(0),
	loopEnd(MAX_SAMPLE_NUMBER),
	loopLength(0),
	crossfadeLength(0),
	crossfadeArea(Range<int>())
{
	fileReader.setMonolithicInfo(info, channelIndex, sampleIndex);

	setPreloadSize(0);
}

StreamingSamplerSound::~StreamingSamplerSound()
{
	masterReference.clear();
	fileReader.closeFileHandles();
}

void StreamingSamplerSound::setReversed(bool shouldBeReversed)
{
	if (reversed != shouldBeReversed)
	{
		if (shouldBeReversed)
		{
			loadEntireSample();
			preloadBuffer.reverse(0, preloadBuffer.getNumSamples());
			reversed = true;
		}
		else
		{
			reversed = false;
			setPreloadSize(preloadSize, true);
		}
	}
}

void StreamingSamplerSound::setBasicMappingData(const StreamingHelpers::BasicMappingData& data)
{
	rootNote = data.rootNote;

	midiNotes.clear();
	midiNotes.setRange(data.lowKey, (data.highKey - data.lowKey) + 1, 1);
	velocityRange.setRange(data.lowVelocity, (data.highVelocity - data.lowVelocity) + 1, 1);
}

void StreamingSamplerSound::setPreloadSize(int newPreloadSize, bool forceReload)
{
	if (reversed)
	{
		return;
	}

	const bool preloadSizeChanged = preloadSize == newPreloadSize;
	const bool streamingDeactivated = newPreloadSize == -1 && entireSampleLoaded;

	if (!forceReload && (preloadSizeChanged || streamingDeactivated)) return;

	ScopedLock sl(getSampleLock());

	const bool sampleDeactivated = !hasActiveState() || newPreloadSize == 0;



	if (sampleDeactivated)
	{
		internalPreloadSize = 0;
		preloadSize = 0;

		preloadBuffer = hlac::HiseSampleBuffer(!fileReader.isMonolithic(), fileReader.isStereo() ? 2 : 1, 0);

		return;
	}

	preloadSize = newPreloadSize;

	if (newPreloadSize == -1 || (preloadSize + sampleStartMod) > sampleLength)
	{
		internalPreloadSize = (int)sampleLength;
		entireSampleLoaded = true;
	}
	else
	{
		internalPreloadSize = preloadSize + (int)sampleStartMod;
		entireSampleLoaded = false;
	}

	internalPreloadSize = jmax(preloadSize, internalPreloadSize, 2048);

	fileReader.openFileHandles();

	preloadBuffer = hlac::HiseSampleBuffer(!fileReader.isMonolithic(), fileReader.isStereo() ? 2 : 1, 0);

	try
	{
		preloadBuffer.setSize(fileReader.isStereo() ? 2 : 1, internalPreloadSize);
	}
	catch (std::exception e)
	{
		preloadBuffer.setSize(fileReader.isStereo() ? 2 : 1, 0);

		throw StreamingSamplerSound::LoadingError(getFileName(), "Preload error (max memory exceeded).");
	}

	if (preloadBuffer.getNumSamples() == 0)
	{
		return;
	}

	preloadBuffer.clear();

	if (sampleRate <= 0.0)
	{
		if (AudioFormatReader *reader = fileReader.getReader())
		{
			sampleRate = reader->sampleRate;
			sampleEnd = jmin<int>(sampleEnd, (int)reader->lengthInSamples);
			sampleLength = jmax<int>(0, sampleEnd - sampleStart);
			loopEnd = jmin(loopEnd, sampleEnd);
		}
	}

	if (loopEnabled && (loopEnd - loopStart > 0) && loopEnd < internalPreloadSize)
	{
		entireSampleLoaded = false;
//        int samplesToFill = internalPreloadSize;
		
		fileReader.readFromDisk(preloadBuffer, 0, loopEnd, sampleStart + monolithOffset, true);
		const int samplesPerFillOp = (loopEnd - loopStart);

		int numTodo = internalPreloadSize - loopEnd;

		int pos = loopEnd;

		while (numTodo > 0)
		{
			int numThisTime = jmin<int>(numTodo, samplesPerFillOp);

			hlac::HiseSampleBuffer::copy(preloadBuffer, preloadBuffer, pos, loopStart, numThisTime);
			numTodo -= numThisTime;
			pos += numThisTime;
		}
	}
	else
	{
		auto samplesToRead = jmin<int>(sampleLength, internalPreloadSize);

		if(samplesToRead > 0)
			fileReader.readFromDisk(preloadBuffer, 0, samplesToRead, sampleStart + monolithOffset, true);
	}
}



size_t StreamingSamplerSound::getActualPreloadSize() const
{
	auto bytesPerSample = fileReader.isMonolithic() ? sizeof(int16) : sizeof(float);

	return hasActiveState() ? (size_t)(internalPreloadSize *preloadBuffer.getNumChannels()) * bytesPerSample + (size_t)(loopBuffer.getNumSamples() *loopBuffer.getNumChannels()) * bytesPerSample : 0;
}

void StreamingSamplerSound::loadEntireSample() { setPreloadSize(-1); }

void StreamingSamplerSound::increaseVoiceCount() const { fileReader.increaseVoiceCount(); }
void StreamingSamplerSound::decreaseVoiceCount() const { fileReader.decreaseVoiceCount(); }

void StreamingSamplerSound::closeFileHandle()
{
	fileReader.closeFileHandles();
}

void StreamingSamplerSound::openFileHandle()
{
	fileReader.openFileHandles();
}

bool StreamingSamplerSound::isOpened()
{
	return fileReader.isOpened();
}

bool StreamingSamplerSound::isMonolithic() const
{
	return fileReader.isMonolithic();
}

juce::AudioFormatReader* StreamingSamplerSound::createReaderForPreview()
{
	return fileReader.createMonolithicReaderForPreview();
}

AudioFormatReader* StreamingSamplerSound::createReaderForAnalysis()
{
	return fileReader.getReader();
}

String StreamingSamplerSound::getSampleStateAsString() const
{
	if (isMissing())
	{
		if (purged) return "Purged+Missing";
		else return "Missing";
	}
	else
	{
		if (purged) return "Purged";
		else return "Normal";
	}
}

String StreamingSamplerSound::getFileName(bool getFullPath /*= false*/) const { return fileReader.getFileName(getFullPath); }

int64 StreamingSamplerSound::getHashCode() { return fileReader.getHashCode(); }


void StreamingSamplerSound::checkFileReference()
{
	fileReader.checkFileReference();
}


void StreamingSamplerSound::replaceFileReference(const String &newFileName)
{
	fileReader.setFile(newFileName);

	if (isMissing()) return;

	fileReader.openFileHandles();

	AudioFormatReader *reader = fileReader.getReader();

	if (reader != nullptr)
	{
		monolithLength = (int)reader->lengthInSamples;
		sampleRate = reader->sampleRate;

		setPreloadSize(PRELOAD_SIZE, true);
	}
	else
	{
		throw LoadingError(fileReader.getFileName(true), "Error at normal reading");
	}

	fileReader.closeFileHandles();
}

bool StreamingSamplerSound::isMissing() const noexcept { return fileReader.isMissing(); }

bool StreamingSamplerSound::hasActiveState() const noexcept { return !isMissing() && !purged; }

double StreamingSamplerSound::getPitchFactor(int noteNumberToPitch, int rootNoteForPitchFactor) noexcept
{
	return pow(2.0, (noteNumberToPitch - rootNoteForPitchFactor) / 12.0);
}

void StreamingSamplerSound::setSampleStart(int newSampleStart)
{
	if (sampleStart != newSampleStart &&
		(!loopEnabled || (loopEnabled && loopStart > newSampleStart)))
	{
		sampleStart = newSampleStart;
		lengthChanged();
	}
}

void StreamingSamplerSound::setSampleStartModulation(int newModulationDelta)
{
	if (sampleStartMod != newModulationDelta)
	{
		ScopedLock sl(getSampleLock());

		sampleStartMod = newModulationDelta;
		lengthChanged();
	}
}

void StreamingSamplerSound::setLoopEnabled(bool shouldBeEnabled)
{
	if (loopEnabled != shouldBeEnabled)
	{
		loopEnabled = shouldBeEnabled;

		if (shouldBeEnabled && loopStart < sampleStart)
		{
			setLoopStart(sampleStart);
			return;
		}
		if (shouldBeEnabled && loopEnd > sampleEnd)
		{
			setLoopEnd(sampleEnd);
			return;
		}

		loopChanged();
	}
}

void StreamingSamplerSound::setLoopStart(int newLoopStart)
{
	if (loopStart != newLoopStart)
	{
		loopStart = jmax(sampleStart, newLoopStart);
		loopChanged();
	}
}

void StreamingSamplerSound::setLoopEnd(int newLoopEnd)
{
	if (loopEnd != newLoopEnd)
	{
		loopEnd = jmin(sampleEnd, newLoopEnd);
		crossfadeArea = Range<int>((int)(loopEnd - crossfadeLength), (int)loopEnd);
		loopChanged();
	}
}

void StreamingSamplerSound::setLoopCrossfade(int newCrossfadeLength)
{
	if (crossfadeLength != newCrossfadeLength)
	{
		crossfadeLength = newCrossfadeLength;
		crossfadeArea = Range<int>((int)(loopEnd - crossfadeLength), (int)loopEnd);
		loopChanged();
	}
}


void StreamingSamplerSound::setSampleEnd(int newSampleEnd)
{
	if (sampleEnd != newSampleEnd &&
		(!loopEnabled || (loopEnabled && loopEnd < newSampleEnd)))
	{
		sampleEnd = newSampleEnd;
		lengthChanged();
	}
}

void StreamingSamplerSound::lengthChanged()
{
	ScopedLock sl(getSampleLock());

	sampleLength = jmax<int>(0, sampleEnd - sampleStart);

	setPreloadSize(preloadSize, true);
}

void StreamingSamplerSound::loopChanged()
{
	ScopedLock sl(getSampleLock());

	loopStart = jmax<int>(loopStart, sampleStart);
	loopEnd = jmin<int>(loopEnd, sampleEnd);
	loopLength = jmax<int>(0, loopEnd - loopStart);

	if (loopEnabled)
	{
		if (loopEnd < preloadBuffer.getNumSamples())
		{
			useSmallLoopBuffer = false;
			smallLoopBuffer.setSize(1, 0);
			setPreloadSize(preloadSize, true);
		}
		else if (loopLength < 8192)
		{
			useSmallLoopBuffer = true;

			fileReader.openFileHandles();

			smallLoopBuffer = hlac::HiseSampleBuffer(!fileReader.isMonolithic(), fileReader.isStereo() ? 2 : 1, (int)loopLength);

			fileReader.readFromDisk(smallLoopBuffer, 0, loopLength, loopStart, false);

			closeFileHandle();

		}
		else
		{
			useSmallLoopBuffer = false;
			smallLoopBuffer.setSize(2, 0);
		}

		if (crossfadeLength != 0)
		{
			loopBuffer = hlac::HiseSampleBuffer(!fileReader.isMonolithic(), 2, (int)crossfadeLength);

			hlac::HiseSampleBuffer tempBuffer(!fileReader.isMonolithic(), 2, (int)crossfadeLength);

			// Calculate the fade in
			const int startCrossfade = loopStart - crossfadeLength;
			tempBuffer.clear();

			fileReader.openFileHandles();

			fileReader.readFromDisk(tempBuffer, 0, (int)crossfadeLength, startCrossfade + monolithOffset, false);

			tempBuffer.applyGainRamp(0, 0, (int)crossfadeLength, 0.0f, 1.0f);
			tempBuffer.applyGainRamp(1, 0, (int)crossfadeLength, 0.0f, 1.0f);

			hlac::HiseSampleBuffer::copy(loopBuffer, tempBuffer, 0, 0, (int)crossfadeLength);

			// Calculate the fade out
			tempBuffer.clear();

			const int endCrossfade = loopEnd - crossfadeLength;

			fileReader.readFromDisk(tempBuffer, 0, (int)crossfadeLength, endCrossfade + monolithOffset, false);

			tempBuffer.applyGainRamp(0, 0, (int)crossfadeLength, 1.0f, 0.0f);
			tempBuffer.applyGainRamp(1, 0, (int)crossfadeLength, 1.0f, 0.0f);

			hlac::HiseSampleBuffer::add(loopBuffer, tempBuffer, 0, 0, crossfadeLength);

			fileReader.closeFileHandles();
		}
	}
	else
	{
		if (loopEnd < internalPreloadSize)
		{
			useSmallLoopBuffer = false;
			smallLoopBuffer.setSize(1, 0);
			setPreloadSize(preloadSize, true);
		}
	}
}

void StreamingSamplerSound::wakeSound() const { fileReader.wakeSound(); }


bool StreamingSamplerSound::hasEnoughSamplesForBlock(int maxSampleIndexInFile) const
{
	return (loopEnabled && loopLength != 0) || maxSampleIndexInFile < sampleLength;
}

float StreamingSamplerSound::calculatePeakValue()
{
	return fileReader.calculatePeakValue();
}

void StreamingSamplerSound::fillSampleBuffer(hlac::HiseSampleBuffer &sampleBuffer, int samplesToCopy, int uptime) const
{
	ScopedLock sl(getSampleLock());

	if (!fileReader.isUsed()) return;

	const bool wrapLoop = (uptime + samplesToCopy + sampleStart) > loopEnd;

	if (loopEnabled && loopLength != 0 && wrapLoop)
	{
		const int indexInLoop = (uptime + sampleStart - loopStart) % loopLength;

		const int numSamplesInThisLoop = (int)(loopLength - indexInLoop);

		if (useSmallLoopBuffer)
		{
			int numSamplesBeforeFirstWrap = 0;

			if (indexInLoop < 0)
			{
				numSamplesBeforeFirstWrap = jmin<int>(samplesToCopy, loopStart - (uptime + sampleStart));

				fillInternal(sampleBuffer, numSamplesBeforeFirstWrap, uptime + (int)sampleStart, 0);
			}
			else
			{
				numSamplesBeforeFirstWrap = jmin<int>(samplesToCopy, numSamplesInThisLoop);
				int startSample = indexInLoop;

				hlac::HiseSampleBuffer::copy(sampleBuffer, smallLoopBuffer, 0, startSample, numSamplesBeforeFirstWrap);
			}

			int numSamples = samplesToCopy - numSamplesBeforeFirstWrap;
			int indexInSampleBuffer = numSamplesBeforeFirstWrap;

			if (numSamples < 0)
			{
				jassertfalse;
				return;
			}

			while (numSamples > (int)loopLength)
			{
				jassert(indexInSampleBuffer < sampleBuffer.getNumSamples());

				hlac::HiseSampleBuffer::copy(sampleBuffer, smallLoopBuffer, indexInSampleBuffer, 0, loopLength);

				numSamples -= (int)loopLength;
				indexInSampleBuffer += (int)loopLength;
			}

			hlac::HiseSampleBuffer::copy(sampleBuffer, smallLoopBuffer, indexInSampleBuffer, 0, numSamples);
		}

		// Loop is smaller than streaming buffers
		else if (loopLength < samplesToCopy)
		{
			const int numSamplesBeforeFirstWrap = numSamplesInThisLoop;

			int numSamples = samplesToCopy - numSamplesBeforeFirstWrap;
			int startSample = numSamplesBeforeFirstWrap;

			const int indexToUse = indexInLoop > 0 ? ((int)indexInLoop + (int)loopStart) : uptime + (int)sampleStart;
			fillInternal(sampleBuffer, numSamplesBeforeFirstWrap, indexToUse, 0);

			while (numSamples > (int)loopLength)
			{
				fillInternal(sampleBuffer, (int)loopLength, (int)loopStart, startSample);
				numSamples -= (int)loopLength;
				startSample += (int)loopLength;
			}

			fillInternal(sampleBuffer, numSamples, (int)loopStart, startSample);
		}

		// loop is bigger than streaming buffers and does not get wrapped
		else if (numSamplesInThisLoop > samplesToCopy)
		{
			fillInternal(sampleBuffer, samplesToCopy, (int)(loopStart + indexInLoop));
		}

		// loop is bigger than streaming buffers and needs some wrapping
		else
		{
			const int numSamplesBeforeWrap = numSamplesInThisLoop;
			const int numSamplesAfterWrap = samplesToCopy - numSamplesBeforeWrap;

			fillInternal(sampleBuffer, numSamplesBeforeWrap, (int)(loopStart + indexInLoop), 0);
			fillInternal(sampleBuffer, numSamplesAfterWrap, (int)loopStart, numSamplesBeforeWrap);
		}
	}
	else
	{
		jassert(((int)sampleStart + uptime + samplesToCopy) <= sampleEnd);

		fillInternal(sampleBuffer, samplesToCopy, uptime + (int)sampleStart);
	}
};

void StreamingSamplerSound::fillInternal(hlac::HiseSampleBuffer &sampleBuffer, int samplesToCopy, int uptime, int offsetInBuffer/*=0*/) const
{
	jassert(uptime + samplesToCopy <= sampleEnd);

	// Some samples from the loop crossfade buffer are required
	if (loopEnabled && Range<int>(uptime, uptime + samplesToCopy).intersects(crossfadeArea))
	{
		const int numSamplesBeforeCrossfade = jmax(0, crossfadeArea.getStart() - uptime);

		if (numSamplesBeforeCrossfade > 0)
		{
			fillInternal(sampleBuffer, numSamplesBeforeCrossfade, uptime, 0);
		}

		const int numSamplesInCrossfade = jmin(samplesToCopy - numSamplesBeforeCrossfade, (int)crossfadeLength);

		if (numSamplesInCrossfade > 0)
		{
			const int indexInLoopBuffer = jmax(0, uptime - crossfadeArea.getStart());

			hlac::HiseSampleBuffer::copy(sampleBuffer, loopBuffer, numSamplesBeforeCrossfade, indexInLoopBuffer, numSamplesInCrossfade);
		}

		// Should be taken care by higher logic (fillSampleBuffer should wrap the loop)
		jassert((samplesToCopy - numSamplesBeforeCrossfade - numSamplesInCrossfade) == 0);
	}

	// All samples can be fetched from the preload buffer
	else if (uptime + samplesToCopy < internalPreloadSize)
	{
		// the preload buffer has already the samplestart offset
		const int indexInPreloadBuffer = uptime - (int)sampleStart;

		jassert(indexInPreloadBuffer >= 0);

		if (indexInPreloadBuffer + samplesToCopy < preloadBuffer.getNumSamples())
		{
			hlac::HiseSampleBuffer::copy(sampleBuffer, preloadBuffer, offsetInBuffer, indexInPreloadBuffer, samplesToCopy);
		}
		else
		{
			jassertfalse;

			sampleBuffer.clear();
		}
	}

	// Read all samples from disk
	else
	{
		fileReader.readFromDisk(sampleBuffer, offsetInBuffer, samplesToCopy, uptime + monolithOffset, true);
	}
}

// =============================================================================================================================================== StreamingSamplerSound::FileReader methods


StreamingSamplerSound::FileReader::FileReader(StreamingSamplerSound *soundForReader, StreamingSamplerSoundPool *pool_) :
	pool(pool_),
	sound(soundForReader),
	missing(true),
	hashCode(0),
	voiceCount(0),
	fileHandlesOpen(false)
{}

StreamingSamplerSound::FileReader::~FileReader()
{
	ScopedWriteLock sl(fileAccessLock);

	memoryReader = nullptr;
	normalReader = nullptr;
}

void StreamingSamplerSound::FileReader::setFile(const String &fileName)
{
	monolithicInfo = nullptr;

	if (File::isAbsolutePath(fileName))
	{
		loadedFile = File(fileName);

		const String fileExtension = loadedFile.getFileExtension();

		fileFormatSupportsMemoryReading = fileExtension.contains("wav") || fileExtension.contains("aif");// || fileExtension.contains("hlac");

		hashCode = loadedFile.hashCode64();
	}
	else
	{
		faultyFileName = fileName;
		loadedFile = File();
	}
}

String StreamingSamplerSound::FileReader::getFileName(bool getFullPath)
{
	if (monolithicInfo != nullptr)
	{
		return monolithicName;
	}

	if (faultyFileName.isNotEmpty())
	{
		if (getFullPath)
		{
			return faultyFileName;
		}
		else
		{
#if JUCE_WINDOWS
			return faultyFileName.fromLastOccurrenceOf("/", false, false);
#else
			return faultyFileName.fromLastOccurrenceOf("\\", false, false);
#endif
		}
	}
	else return getFullPath ? loadedFile.getFullPathName() : loadedFile.getFileName();
}

void StreamingSamplerSound::FileReader::checkFileReference()
{
	if (monolithicInfo != nullptr) return;

	if (missing) missing = !loadedFile.existsAsFile();// && getReader() == nullptr;
}

void StreamingSamplerSound::FileReader::refreshFileInformation()
{
	checkFileReference();

	if (!missing)
	{
		faultyFileName = String();

		const String fileExtension = loadedFile.getFileExtension();

		fileFormatSupportsMemoryReading = fileExtension.compareIgnoreCase(".wav") || fileExtension.startsWithIgnoreCase(".aif");// || fileExtension.startsWithIgnoreCase("hlac");

		hashCode = loadedFile.hashCode64();
	}
}



AudioFormatReader * StreamingSamplerSound::FileReader::getReader()
{
	if (!fileHandlesOpen) openFileHandles();

	if (memoryReader != nullptr) return memoryReader;
	else if (normalReader != nullptr) return normalReader;
	else return nullptr;
}

void StreamingSamplerSound::FileReader::wakeSound()
{
	if (!fileFormatSupportsMemoryReading) return;

	if (memoryReader != nullptr && !memoryReader->getMappedSection().isEmpty()) memoryReader->touchSample(sound->sampleStart + sound->monolithOffset);
}


void StreamingSamplerSound::FileReader::openFileHandles(NotificationType notifyPool)
{


	if (fileHandlesOpen)
	{
		jassert(memoryReader != nullptr || normalReader != nullptr);
		return;
	}
	else
	{
		jassert(memoryReader == nullptr || normalReader == nullptr);

		ScopedWriteLock sl(fileAccessLock);

		fileHandlesOpen = true;

		memoryReader = nullptr;
		normalReader = nullptr;

		if (monolithicInfo != nullptr)
		{
#if USE_FALLBACK_READERS_FOR_MONOLITH
			normalReader = monolithicInfo->createFallbackReader(monolithicIndex, monolithicChannelIndex);
#else
			normalReader = monolithicInfo->createMonolithicReader(monolithicIndex, monolithicChannelIndex);
#endif

			if (normalReader != nullptr)
				stereo = normalReader->numChannels > 1;

			sampleLength = getMonolithLength();

		}
		else
		{
			if (fileFormatSupportsMemoryReading)
			{
				AudioFormat* format = pool->afm.findFormatForFileExtension(loadedFile.getFileExtension());

				if (format != nullptr)
				{
					memoryReader = format->createMemoryMappedReader(loadedFile);

					if (memoryReader != nullptr)
					{
						memoryReader->mapSectionOfFile(Range<int64>((int64)(sound->sampleStart) + (int64)(sound->monolithOffset), (int64)(sound->sampleEnd)));

						sampleLength = jmax<int64>(0, memoryReader->getMappedSection().getLength());

						stereo = memoryReader->numChannels > 1;
					}
				}
			}


			normalReader = pool->afm.createReaderFor(loadedFile);

			sampleLength = normalReader != nullptr ? normalReader->lengthInSamples : 0;
			stereo = (normalReader != nullptr) ? (normalReader->numChannels > 1) : false;

		}

#if USE_BACKEND
		if (monolithicInfo == nullptr && notifyPool == sendNotification) pool->increaseNumOpenFileHandles();
#else
		ignoreUnused(notifyPool);
#endif
	}
}


bool StreamingSamplerSound::FileReader::isStereo() const noexcept
{
	return stereo;
}

void StreamingSamplerSound::FileReader::closeFileHandles(NotificationType notifyPool)
{
	if (monolithicIndex != -1) return; // don't close the reader for monolithic files...

	if (voiceCount.get() == 0)
	{
		ScopedWriteLock sl(fileAccessLock);

		fileHandlesOpen = false;

		memoryReader = nullptr;
		normalReader = nullptr;

		if (monolithicInfo == nullptr && notifyPool == sendNotification) pool->decreaseNumOpenFileHandles();
	}
}




void StreamingSamplerSound::FileReader::readFromDisk(hlac::HiseSampleBuffer &buffer, int startSample, int numSamples, int readerPosition, bool useMemoryMappedReader)
{
	if (!fileHandlesOpen) openFileHandles(sendNotification);


#if USE_SAMPLE_DEBUG_COUNTER

	float* l = buffer.getWritePointer(0, startSample);
	float* r = buffer.getWritePointer(1, startSample);

	int v = readerPosition;

	for (int i = 0; i < numSamples; i++)
	{
		l[i] = (float)v;
		r[i] = (float)v;

		v++;
	}

	return;
#endif


	buffer.clear(startSample, numSamples);

	if (!isMonolithic() && useMemoryMappedReader)
	{
		if (memoryReader != nullptr && memoryReader->getMappedSection().contains(Range<int64>(readerPosition, readerPosition + numSamples)))
		{
			ScopedReadLock sl(fileAccessLock);

			if (buffer.isFloatingPoint())
				memoryReader->read(buffer.getFloatBufferForFileReader(), startSample, numSamples, readerPosition, true, true);
			else
				jassertfalse;

			return;
		}
	}

	if (normalReader != nullptr)
	{
		ScopedReadLock sl(fileAccessLock);

		if (buffer.isFloatingPoint())
			normalReader->read(buffer.getFloatBufferForFileReader(), startSample, numSamples, readerPosition, true, true);
		else
			dynamic_cast<hlac::HlacSubSectionReader*>(normalReader.get())->readIntoFixedBuffer(buffer, startSample, numSamples, readerPosition);


	}
	else
	{
		// Something is wrong so clear the buffer to be safe...
		buffer.clear(startSample, numSamples);
	}
}


float StreamingSamplerSound::FileReader::calculatePeakValue()
{
	float l1, l2, r1, r2;

	openFileHandles();

	AudioFormatReader *readerToUse = getReader();

	if (readerToUse != nullptr) readerToUse->readMaxLevels(sound->sampleStart + sound->monolithOffset, sound->sampleLength, l1, l2, r1, r2);
	else return 0.0f;

	closeFileHandles();

	const float maxLeft = jmax<float>(-l1, l2);
	const float maxRight = jmax<float>(-r1, r2);

	return jmax<float>(maxLeft, maxRight);
}


AudioFormatReader* StreamingSamplerSound::FileReader::createMonolithicReaderForPreview()
{
	if (monolithicInfo != nullptr)
	{
		return monolithicInfo->createThumbnailReader(monolithicIndex, monolithicChannelIndex);
	}
	else
	{
		return pool->afm.createReaderFor(loadedFile);
	}
}

void StreamingSamplerSound::FileReader::setMonolithicInfo(MonolithInfoToUse* info, int channelIndex, int sampleIndex)
{
	monolithicInfo = info;
	monolithicIndex = sampleIndex;
	missing = (sampleIndex == -1);
	monolithicName = info->getFileName(channelIndex, sampleIndex);

	hashCode = monolithicName.hashCode64();

	monolithicChannelIndex = channelIndex;
}

} // namespace hise
