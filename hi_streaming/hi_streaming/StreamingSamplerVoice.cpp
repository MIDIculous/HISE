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

namespace hise {
using namespace juce;

#if LOG_SAMPLE_RENDERING
namespace {
    static bool checkSampleData(bool isLeftChannel, const float *data, int numSamples)
    {
        //if (location <= locationForErrorInCurrentCallback) // poor man's stack trace
        //    return true;

        auto range = FloatVectorOperations::findMinAndMax(data, numSamples);

        const float maxValue = 32.0f;

        double errorValue = 0.0;
        bool isError = false;
        int numFaultySamples = 0;

        if (range.getEnd() > maxValue) {
            isError = true;

            errorValue = range.getEnd();

            for (int i = 0; i < numSamples; i++) {
                if (data[i] > maxValue)
                    numFaultySamples++;
            }
        }

        if (range.getStart() < -maxValue) {
            isError = true;

            errorValue = range.getStart();

            for (int i = 0; i < numSamples; i++) {
                if (data[i] < -maxValue)
                    numFaultySamples++;
            }
        }

        if (isError) {
            String failureType;
            if (numFaultySamples == 1) {
                failureType = isLeftChannel ? "ClickLeft" : "ClickRight";
            }
            else {
                failureType = isLeftChannel ? "BurstLeft" : "BurstRight";
            }
            Logger::writeToLog("checkSampleData Error: " + failureType);

            return false;
        }

        return true;
    }
}
#endif

// =============================================================================================================================================== SampleLoader methods

SampleLoader::SampleLoader(SampleThreadPool *pool_)
: backgroundPool(pool_),
  unmapper(std::make_shared<Unmapper>()),
  writeBufferIsBeingFilled(false),
  sound(0),
  readIndex(0),
  readIndexDouble(0.0),
  idealBufferSize(0),
  minimumBufferSizeForSamplesPerBlock(0),
  positionInSampleFile(0),
  isReadingFromPreloadBuffer(true),
  sampleStartModValue(0),
  readBuffer(nullptr),
  writeBuffer(nullptr),
  diskUsage(0.0),
  lastCallToRequestData(0.0),
  b1(true, 2, 0),
  b2(true, 2, 0)
{
    unmapper->setLoader(this);

    setBufferSize(BUFFER_SIZE_FOR_STREAM_BUFFERS);
}

SampleLoader::~SampleLoader()
{
    unmapper = nullptr;
    b1.setSize(2, 0);
    b2.setSize(2, 0);
}

/** Sets the buffer size in samples. */
void SampleLoader::setBufferSize(int newBufferSize)
{
    ScopedLock sl(getLock());

#if HISE_IOS

    // because of memory
    idealBufferSize = 4096;
#else
    idealBufferSize = newBufferSize;
#endif

    refreshBufferSizes();
}

bool SampleLoader::assertBufferSize(int minimumBufferSize)
{
    minimumBufferSizeForSamplesPerBlock = minimumBufferSize;

    refreshBufferSizes();

    return true;
}

void SampleLoader::startNote(StreamingSamplerSound const *s, int startTime)
{
    diskUsage = 0.0;

    sound = s;

    s->wakeSound();

    sampleStartModValue = (int)startTime;

    auto localReadBuffer = &s->getPreloadBuffer();
    auto localWriteBuffer = &b1;

    // the read pointer will be pointing directly to the preload buffer of the sample sound
    readBuffer = localReadBuffer;
    writeBuffer = localWriteBuffer;

    lastSwapPosition = 0.0;

    readIndex = startTime;
    readIndexDouble = (double)startTime;

    isReadingFromPreloadBuffer = true;

    // Set the sampleposition to (1 * bufferSize) because the first buffer is the preload buffer
    positionInSampleFile = (int)localReadBuffer->getNumSamples();

    voiceCounterWasIncreased = false;

    entireSampleIsLoaded = s->isEntireSampleLoaded();

    if (!entireSampleIsLoaded) {
        // The other buffer will be filled on the next free thread pool slot
        requestNewData();
    }
};

void SampleLoader::reset()
{
    const StreamingSamplerSound *currentSound = sound.get();

    if (currentSound != nullptr) {
        const bool isMonolith = currentSound->isMonolithic();

        if (isMonolith) {
            currentSound->decreaseVoiceCount();
            clearLoader();
        }
        else {
            // If the samples are not monolithic, we'll need to close the
            // file handles on the background thread.

            unmapper->setSoundToUnmap(currentSound);

            backgroundPool->addJob(unmapper, false);

            clearLoader();
        }
    }
}

void SampleLoader::clearLoader()
{
    sound = nullptr;
    diskUsage = 0.0f;
    cancelled = false;
}

double SampleLoader::getDiskUsage() noexcept
{
    const double returnValue = (double)diskUsage.get();
    diskUsage = 0.0f;
    return returnValue;
}

void SampleLoader::setStreamingBufferDataType(bool shouldBeFloat)
{
    ScopedLock sl(getLock());

    b1 = hlac::HiseSampleBuffer(shouldBeFloat, 2, 0);
    b2 = hlac::HiseSampleBuffer(shouldBeFloat, 2, 0);

    refreshBufferSizes();
}

StereoChannelData SampleLoader::fillVoiceBuffer(hlac::HiseSampleBuffer &voiceBuffer, double numSamples) const
{
    auto localReadBuffer = readBuffer.get();
    auto localWriteBuffer = writeBuffer.get();

    const int numSamplesInBuffer = localReadBuffer->getNumSamples();
    const int maxSampleIndexForFillOperation = (int)(readIndexDouble + numSamples) + 1; // Round up the samples

    if (maxSampleIndexForFillOperation >= numSamplesInBuffer) // Check because of preloadbuffer style
    {
        const int indexBeforeWrap = jmax<int>(0, (int)(readIndexDouble));
        const int numSamplesInFirstBuffer = localReadBuffer->getNumSamples() - indexBeforeWrap;

        jassert(numSamplesInFirstBuffer >= 0);

        if (numSamplesInFirstBuffer > 0) {
            hlac::HiseSampleBuffer::copy(voiceBuffer, *localReadBuffer, 0, indexBeforeWrap, numSamplesInFirstBuffer);
        }

        const int offset = numSamplesInFirstBuffer;
        const int numSamplesAvailableInSecondBuffer = localWriteBuffer->getNumSamples() - offset;

        if ((numSamplesAvailableInSecondBuffer > 0) && (numSamplesAvailableInSecondBuffer <= localWriteBuffer->getNumSamples())) {
            const int numSamplesToCopyFromSecondBuffer = jmin<int>(numSamplesAvailableInSecondBuffer, voiceBuffer.getNumSamples() - offset);

            if (writeBufferIsBeingFilled || entireSampleIsLoaded) {
                voiceBuffer.clear(offset, numSamplesToCopyFromSecondBuffer);
            }
            else {
                hlac::HiseSampleBuffer::copy(voiceBuffer, *localWriteBuffer, offset, 0, numSamplesToCopyFromSecondBuffer);
            }
        }
        else {
            // The streaming buffers must be greater than the block size!
            jassertfalse;

            voiceBuffer.clear();
        }

        StereoChannelData returnData;

        returnData.isFloatingPoint = localReadBuffer->isFloatingPoint();
        returnData.leftChannel = voiceBuffer.getReadPointer(0);
        returnData.rightChannel = voiceBuffer.getReadPointer(1);

#if USE_SAMPLE_DEBUG_COUNTER

        const float *l = voiceBuffer.getReadPointer(0, 0);
        const float *r = voiceBuffer.getReadPointer(1, 0);

        float ll = l[0];
        float lr = r[0];

        for (int i = 1; i < voiceBuffer.getNumSamples(); i++) {
            const float tl = l[i];
            const float tr = r[i];

            jassert(tl == tr);
            jassert(tl - ll == 1.0f);
            ll = tl;
            lr = tr;
        }
#endif

        return returnData;
    }
    else {
        const int index = (int)readIndexDouble;

        StereoChannelData returnData;

        returnData.isFloatingPoint = localReadBuffer->isFloatingPoint();
        returnData.leftChannel = localReadBuffer->getReadPointer(0, index);
        returnData.rightChannel = localReadBuffer->getReadPointer(localReadBuffer->getNumChannels() > 1 ? 1 : 0, index);

        return returnData;
    }
}

bool SampleLoader::advanceReadIndex(double uptime)
{
    const int numSamplesInBuffer = readBuffer.get()->getNumSamples();
    readIndexDouble = uptime - lastSwapPosition;

    if (readIndexDouble >= numSamplesInBuffer) {
        if (entireSampleIsLoaded) {
            return true;
        }
        else {
            lastSwapPosition = (double)positionInSampleFile;
            positionInSampleFile += getNumSamplesForStreamingBuffers();
            readIndexDouble = uptime - lastSwapPosition;

            swapBuffers();
            const bool queueIsFree = requestNewData();

            return queueIsFree;
        }
    }

    return true;
}

int SampleLoader::getNumSamplesForStreamingBuffers() const
{
    jassert(b1.getNumSamples() == b2.getNumSamples());

    return b1.getNumSamples();
}

bool SampleLoader::requestNewData()
{
#if KILL_VOICES_WHEN_STREAMING_IS_BLOCKED
    if (this->isQueued()) {
        writeBuffer.get()->clear();

#if LOG_SAMPLE_RENDERING
        Logger::writeToLog("hi_streaming KILL_VOICES_WHEN_STREAMING_IS_BLOCKED: Voice killed.");
#endif

        cancelled = true;
        backgroundPool->notify();
        return false;
    }
    else {
        backgroundPool->addJob(shared_from_this(), false);
        return true;
    }
#else
    backgroundPool->addJob(shared_from_this(), false);
    return true;
#endif
};


SampleThreadPoolJob::JobStatus SampleLoader::runJob()
{
    if (cancelled) {
        cancelled = false;
        return SampleThreadPoolJob::jobHasFinished;
    }

    const double readStart = Time::highResolutionTicksToSeconds(Time::getHighResolutionTicks());

    if (writeBufferIsBeingFilled) {
        return SampleThreadPoolJob::jobNeedsRunningAgain;
    }

    writeBufferIsBeingFilled = true; // A poor man's mutex but gets the job done.

    const StreamingSamplerSound *localSound = sound.get();

    if (!voiceCounterWasIncreased && localSound != nullptr) {
        localSound->increaseVoiceCount();
        voiceCounterWasIncreased = true;
    }

    fillInactiveBuffer();

    writeBufferIsBeingFilled = false;

    const double readStop = Time::highResolutionTicksToSeconds(Time::getHighResolutionTicks());
    const double readTime = (readStop - readStart);
    const double timeSinceLastCall = readStop - lastCallToRequestData;
    const float diskUsageThisTime = jmax<float>(diskUsage.get(), (float)(readTime / timeSinceLastCall));
    diskUsage = diskUsageThisTime;
    lastCallToRequestData = readStart;

    return SampleThreadPoolJob::JobStatus::jobHasFinished;
}

String SampleLoader::getName()
{
    String name = "SampleLoader";
    if (sound.get() != nullptr)
        name += ": " + sound.get()->getFileName(/* getFullPath: */ false);
    
    return name;
}

size_t SampleLoader::getActualStreamingBufferSize() const
{
    return b1.getNumSamples() * 2 * 2;
}

void SampleLoader::fillInactiveBuffer()
{
    const StreamingSamplerSound *localSound = sound.get();

    if (localSound == nullptr)
        return;

    if (localSound != nullptr) {
        if (localSound->hasEnoughSamplesForBlock(positionInSampleFile + getNumSamplesForStreamingBuffers())) {
            localSound->fillSampleBuffer(*writeBuffer.get(), getNumSamplesForStreamingBuffers(), (int)positionInSampleFile);
        }
        else if (localSound->hasEnoughSamplesForBlock(positionInSampleFile)) {
            const int numSamplesToFill = (int)localSound->getSampleLength() - positionInSampleFile;
            const int numSamplesToClear = getNumSamplesForStreamingBuffers() - numSamplesToFill;

            localSound->fillSampleBuffer(*writeBuffer.get(), numSamplesToFill, (int)positionInSampleFile);

            writeBuffer.get()->clear(numSamplesToFill, numSamplesToClear);
        }
        else {
            writeBuffer.get()->clear();
        }

#if LOG_SAMPLE_RENDERING
        jassert(localSound != nullptr);
#endif

#if USE_SAMPLE_DEBUG_COUNTER

        DBG(positionInSampleFile);

        const float *l = writeBuffer.get()->getReadPointer(0);
        const float *r = writeBuffer.get()->getReadPointer(1);

        int co = (int)positionInSampleFile;

        for (int i = 0; i < writeBuffer.get()->getNumSamples(); i++) {
            const float tl = l[i];
            const float tr = r[i];
            const float expected = (float)co;

            jassert(tl == tr);
            jassert(tl == 0.0f || (abs(expected - tl) < 0.00001f));

            co++;
        }
#endif
    }
};

void SampleLoader::refreshBufferSizes()
{
    const int numSamplesToUse = jmax<int>(idealBufferSize, minimumBufferSizeForSamplesPerBlock);

    if (getNumSamplesForStreamingBuffers() < numSamplesToUse) {
        StreamingHelpers::increaseBufferIfNeeded(b1, numSamplesToUse);
        StreamingHelpers::increaseBufferIfNeeded(b2, numSamplesToUse);

        readBuffer = &b1;
        writeBuffer = &b2;

        reset();
    }
}

bool SampleLoader::swapBuffers()
{
    auto localReadBuffer = readBuffer.get();

    if (localReadBuffer == &b1) {
        readBuffer = &b2;
        writeBuffer = &b1;
    }
    else // This condition will also be true if the read pointer points at the preload buffer
    {
        readBuffer = &b1;
        writeBuffer = &b2;
    }

    isReadingFromPreloadBuffer = false;
    sampleStartModValue = 0;

    return writeBufferIsBeingFilled == false;
};

// ==================================================================================================== StreamingSamplerVoice methods

StreamingSamplerVoice::StreamingSamplerVoice(SampleThreadPool *pool)
: loader(std::make_shared<SampleLoader>(pool)),
  sampleStartModValue(0)
{
    pitchData = nullptr;
};

void StreamingSamplerVoice::startNote(int /*midiNoteNumber*/,
                                      float /*velocity*/,
                                      SynthesiserSound *s,
                                      int /*currentPitchWheelPosition*/)
{
    StreamingSamplerSound *sound = dynamic_cast<StreamingSamplerSound *>(s);

    if (sound != nullptr && sound->getSampleLength() > 0) {
        loader->startNote(sound, sampleStartModValue);

        jassert(sound != nullptr);
        sound->wakeSound();

        voiceUptime = (double)sampleStartModValue;

        // You have to call setPitchFactor() before startNote().
        jassert(uptimeDelta != 0.0);

        // Resample if sound has different samplerate than the audio sample rate
        uptimeDelta *= (sound->getSampleRate() / getSampleRate());
        uptimeDelta = jmin<double>((double)MAX_SAMPLER_PITCH, uptimeDelta);

        constUptimeDelta = uptimeDelta;

        isActive = true;
    }
    else {
        resetVoice();
    }
}

const StreamingSamplerSound *StreamingSamplerVoice::getLoadedSound()
{
    return loader->getLoadedSound();
}

void StreamingSamplerVoice::setLoaderBufferSize(int newBufferSize)
{
    loader->setBufferSize(newBufferSize);
}

void StreamingSamplerVoice::stopNote(float, bool /*allowTailOff*/)
{
    clearCurrentNote();
    loader->reset();
}

void StreamingSamplerVoice::setDebugLogger(DebugLogger *newLogger)
{
    logger = newLogger;
    loader->setLogger(logger);
}

template<typename SignalType>
void interpolateStereoSamples(const SignalType *inL, const SignalType *inR, const float *pitchData, float *outL, float *outR, int startSample, double indexInBuffer, double uptimeDelta, int numSamples, bool isFloat)
{
    const float gainFactor = isFloat ? 1.0f : (1.0f / (float)INT16_MAX);

    if (pitchData != nullptr) {
        pitchData += startSample;

        float indexInBufferFloat = (float)indexInBuffer;

        for (int i = 0; i < numSamples; i++) {
            const int pos = int(indexInBufferFloat);
            const float alpha = indexInBufferFloat - (float)pos;
            const float invAlpha = 1.0f - alpha;

            float l = ((float)inL[pos] * invAlpha + (float)inL[pos + 1] * alpha);
            float r = ((float)inR[pos] * invAlpha + (float)inR[pos + 1] * alpha);

            outL[i] = l * gainFactor;
            outR[i] = r * gainFactor;

            jassert(*pitchData <= (float)MAX_SAMPLER_PITCH);

            indexInBufferFloat += pitchData[i];
        }
    }
    else {
        float indexInBufferFloat = (float)indexInBuffer;
        const float uptimeDeltaFloat = (float)uptimeDelta;

        while (numSamples > 0) {
            const int pos = int(indexInBufferFloat);
            const float alpha = indexInBufferFloat - (float)pos;
            const float invAlpha = 1.0f - alpha;

            float l = ((float)inL[pos] * invAlpha + (float)inL[pos + 1] * alpha);
            float r = ((float)inR[pos] * invAlpha + (float)inR[pos + 1] * alpha);

            *outL++ = l * gainFactor;
            *outR++ = r * gainFactor;

            indexInBufferFloat += uptimeDeltaFloat;

            numSamples--;
        }
    }
}


void StreamingSamplerVoice::renderNextBlock(AudioSampleBuffer &outputBuffer, int startSample, int numSamples)
{
    const StreamingSamplerSound *sound = loader->getLoadedSound();

#if USE_SAMPLE_DEBUG_COUNTER
    const int startDebug = startSample;
    const int numDebug = numSamples;
#endif

    if (sound != nullptr) {
        const double startAlpha = fmod(voiceUptime, 1.0);

        jassert(pitchCounter != 0);

        auto tempVoiceBuffer = getTemporaryVoiceBuffer();

        jassert(tempVoiceBuffer != nullptr);

        //tempVoiceBuffer->clear();

        // Copy the not resampled values into the voice buffer.
        StereoChannelData data = loader->fillVoiceBuffer(*tempVoiceBuffer, pitchCounter + startAlpha);

        float *outL = outputBuffer.getWritePointer(0, startSample);
        float *outR = outputBuffer.getWritePointer(1, startSample);

        const int startFixed = startSample;
        const int numSamplesFixed = numSamples;


#if USE_SAMPLE_DEBUG_COUNTER
        jassert((int)voiceUptime == data.leftChannel[0]);
#endif

        double indexInBuffer = startAlpha;

        if (data.isFloatingPoint) {
            const float *const inL = static_cast<const float *>(data.leftChannel);
            const float *const inR = static_cast<const float *>(data.rightChannel);

            interpolateStereoSamples(inL, inR, pitchData, outL, outR, startSample, indexInBuffer, uptimeDelta, numSamples, true);
        }
        else {
            const int16 *const inL = static_cast<const int16 *>(data.leftChannel);
            const int16 *const inR = static_cast<const int16 *>(data.rightChannel);

            interpolateStereoSamples(inL, inR, pitchData, outL, outR, startSample, indexInBuffer, uptimeDelta, numSamples, false);
        }

#if USE_SAMPLE_DEBUG_COUNTER

        for (int i = startDebug; i < numDebug; i++) {
            const float l = outputBuffer.getSample(0, i);
            const float r = outputBuffer.getSample(1, i);

            jassert(l == r);
            jassert((abs(l - voiceUptime) < 0.000001) || l == 0.0f);

            voiceUptime += uptimeDelta;
        }

        outputBuffer.clear();
#else
        voiceUptime += pitchCounter;
#endif

        if (!loader->advanceReadIndex(voiceUptime)) {
#if LOG_SAMPLE_RENDERING
            Logger::writeToLog("StreamingSamplerVoice::renderNextBlock error: Streaming failure with voiceUptime: " + String(voiceUptime));
#endif

            outputBuffer.clear(startFixed, numSamplesFixed);

            resetVoice();
            return;
        }

        const bool enoughSamples = sound->hasEnoughSamplesForBlock((int)(voiceUptime)); // +numSamples * MAX_SAMPLER_PITCH));

#if LOG_SAMPLE_RENDERING
        checkSampleData(true, outputBuffer.getReadPointer(0, startFixed), numSamplesFixed);
        checkSampleData(false, outputBuffer.getReadPointer(1, startFixed), numSamplesFixed);
#endif

        if (!enoughSamples)
            resetVoice();
    }
    else {
        resetVoice();
    }
};

void StreamingSamplerVoice::setPitchFactor(int midiNote, int rootNote, StreamingSamplerSound *sound, double globalPitchFactor)
{
    if (midiNote == rootNote) {
        uptimeDelta = jmin(globalPitchFactor, (double)MAX_SAMPLER_PITCH);
    }
    else {
        uptimeDelta = jmin(sound->getPitchFactor(midiNote, rootNote) * globalPitchFactor, (double)MAX_SAMPLER_PITCH);
    }
}

void StreamingSamplerVoice::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    if (sampleRate != -1.0) {
        loader->assertBufferSize(samplesPerBlock * MAX_SAMPLER_PITCH);

        setCurrentPlaybackSampleRate(sampleRate);
    }
}

void StreamingSamplerVoice::resetVoice()
{
    voiceUptime = 0.0;
    uptimeDelta = 0.0;
    isActive = false;
    loader->reset();
    clearCurrentNote();
}

void StreamingSamplerVoice::setSampleStartModValue(int newValue)
{
    jassert(newValue >= 0);

    sampleStartModValue = newValue;
}

hlac::HiseSampleBuffer *StreamingSamplerVoice::getTemporaryVoiceBuffer()
{
    jassert(tvb != nullptr);

    return tvb;
}

void StreamingSamplerVoice::setTemporaryVoiceBuffer(hlac::HiseSampleBuffer *buffer)
{
    tvb = buffer;
}

void StreamingSamplerVoice::initTemporaryVoiceBuffer(hlac::HiseSampleBuffer *bufferToUse, int samplesPerBlock)
{
    // The channel amount must be set correctly in the constructor
    jassert(bufferToUse->getNumChannels() > 0);

    if (bufferToUse->getNumSamples() < samplesPerBlock * MAX_SAMPLER_PITCH) {
        bufferToUse->setSize(bufferToUse->getNumChannels(), samplesPerBlock * MAX_SAMPLER_PITCH);
        bufferToUse->clear();
    }
}

void StreamingSamplerVoice::setStreamingBufferDataType(bool shouldBeFloat)
{
    loader->setStreamingBufferDataType(shouldBeFloat);
}

void SampleLoader::Unmapper::setLoader(SampleLoader *loader_)
{
    loader = loader_;
}

void SampleLoader::Unmapper::setSoundToUnmap(const StreamingSamplerSound *s)
{
    jassert(sound == nullptr);
    sound = const_cast<StreamingSamplerSound *>(s);
}

SampleThreadPool::Job::JobStatus SampleLoader::Unmapper::runJob()
{
    jassert(sound != nullptr);

    if (loader->isRunning()) {
        jassertfalse;
        return SampleThreadPoolJob::jobNeedsRunningAgain;
    }

    if (sound != nullptr) {
        sound->decreaseVoiceCount();
        sound->closeFileHandle();

        sound = nullptr;
    }

    return SampleThreadPoolJob::jobHasFinished;
}

String SampleLoader::Unmapper::getName()
{
    String name = "Unmapper";
    if (sound != nullptr)
        name += ": " + sound->getFileName(/* getFullPath: */ false);
    
    return name;
}

} // namespace hise
