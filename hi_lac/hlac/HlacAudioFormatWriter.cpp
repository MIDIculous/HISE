/*  HISE Lossless Audio Codec
*	�2017 Christoph Hart
*
*	Redistribution and use in source and binary forms, with or without modification,
*	are permitted provided that the following conditions are met:
*
*	1. Redistributions of source code must retain the above copyright notice,
*	   this list of conditions and the following disclaimer.
*
*	2. Redistributions in binary form must reproduce the above copyright notice,
*	   this list of conditions and the following disclaimer in the documentation
*	   and/or other materials provided with the distribution.
*
*	3. All advertising materials mentioning features or use of this software must
*	   display the following acknowledgement:
*	   This product includes software developed by Hart Instruments
*
*	4. Neither the name of the copyright holder nor the names of its contributors may be used
*	   to endorse or promote products derived from this software without specific prior written permission.
*
*	THIS SOFTWARE IS PROVIDED BY CHRISTOPH HART "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING,
*	BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
*	DISCLAIMED. IN NO EVENT SHALL COPYRIGHT HOLDER BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
*	SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
*	GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
*	THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
*	ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
*/

namespace hlac { using namespace juce;

HiseLosslessAudioFormatWriter::HiseLosslessAudioFormatWriter(EncodeMode mode_, OutputStream* output, double sampleRate, int numChannels, uint32* blockOffsetBuffer) :
	AudioFormatWriter(output, "HLAC", sampleRate, numChannels, 16),
	mode(mode_),
	tempOutputStream(new MemoryOutputStream()),
	blockOffsets(blockOffsetBuffer)
{
	auto option = HlacEncoder::CompressorOptions::getPreset(HlacEncoder::CompressorOptions::Presets::Diff);

	encoder.setOptions(option);

	usesFloatingPointData = true;
}


HiseLosslessAudioFormatWriter::~HiseLosslessAudioFormatWriter()
{
	flush();
}

bool HiseLosslessAudioFormatWriter::flush()
{
	if (tempWasFlushed)
		return true;

	if (!writeHeader())
		return false;

	if (!writeDataFromTemp())
		return false;

	tempWasFlushed = true;
    
    tempOutputStream->flush();
    
	deleteTemp();
	return true;
}


void HiseLosslessAudioFormatWriter::setOptions(HlacEncoder::CompressorOptions& newOptions)
{
	options = newOptions;
	encoder.setOptions(newOptions);
}

bool HiseLosslessAudioFormatWriter::write(const int** samplesToWrite, int numSamples)
{
	tempWasFlushed = false;

	bool isStereo = samplesToWrite[1] != nullptr;

	if (options.useCompression)
	{
		if (isStereo)
		{
			float* const* r = const_cast<float**>(reinterpret_cast<const float**>(samplesToWrite));

			AudioSampleBuffer b = AudioSampleBuffer(r, 2, numSamples);

			encoder.compress(b, *tempOutputStream, blockOffsets);
		}
		else
		{
			float* r = const_cast<float*>(reinterpret_cast<const float*>(samplesToWrite[0]));

			AudioSampleBuffer b = AudioSampleBuffer(&r, 1, numSamples);

			encoder.compress(b, *tempOutputStream, blockOffsets);
		}

	}
	else
	{
		float* const* r = const_cast<float**>(reinterpret_cast<const float**>(samplesToWrite));
		numChannels = isStereo ? 2 : 1;

		AudioSampleBuffer source = AudioSampleBuffer(r, numChannels, numSamples);

		MemoryBlock tempBlock;

		const int bytesToWrite = numSamples * numChannels * sizeof(int16);

		tempBlock.setSize(bytesToWrite, false);

		AudioFormatWriter::WriteHelper<AudioData::Int16, AudioData::Float32, AudioData::LittleEndian>::write(
			tempBlock.getData(), numChannels, (const int* const *)source.getArrayOfReadPointers(), numSamples);

		tempOutputStream->write(tempBlock.getData(), bytesToWrite);
	}
	
	return true;
}


void HiseLosslessAudioFormatWriter::setTemporaryBufferType(bool shouldUseTemporaryFile)
{
	usesTempFile = shouldUseTemporaryFile;

	deleteTemp();

	if (shouldUseTemporaryFile)
	{
		if (auto* fosOriginal = getFileOutputStream())
		{
			File originalFile = fosOriginal->getFile();
			tempFile = new TemporaryFile(originalFile, TemporaryFile::OptionFlags::putNumbersInBrackets);
            File tempTarget = tempFile->getFile();
            jassert(tempTarget.getParentDirectory() == originalFile.getParentDirectory());
			tempOutputStream = new FileOutputStream(tempTarget);
		}
		else
		{
			tempFile = new TemporaryFile(File::getCurrentWorkingDirectory(), TemporaryFile::OptionFlags::putNumbersInBrackets);
			File tempTarget = tempFile->getFile();
		}
	}
	else
	{
		tempOutputStream = new MemoryOutputStream();
	}
}

void HiseLosslessAudioFormatWriter::preallocateMemory(int64 numSamplesToWrite, int numChannels)
{
   if (auto mos = dynamic_cast<MemoryOutputStream*>(tempOutputStream.get()))
   {
       int64 b = numSamplesToWrite * numChannels * 2 * 2 / 3;

       // Set the limit to 1.5GB
       int64 limit = 1024;
       limit *= 1024;
       limit *= 1024;
       limit *= 3;
       limit /= 2;

       if (b > limit)
           setTemporaryBufferType(true);
       else
           mos->preallocate(b);
   }
}

bool HiseLosslessAudioFormatWriter::writeHeader()
{
	if (options.useCompression)
	{
		auto numBlocks = encoder.getNumBlocksWritten();

		HiseLosslessHeader header(useEncryption, globalBitShiftAmount, sampleRate, numChannels, bitsPerSample, useCompression, numBlocks);

		jassert(header.getVersion() == HLAC_VERSION);
		jassert(header.getBitShiftAmount() == globalBitShiftAmount);
		jassert(header.getNumChannels() == numChannels);
		jassert(header.usesCompression() == useCompression);
		jassert(header.getSampleRate() == sampleRate);
		jassert(header.getBitsPerSample() == bitsPerSample);

		header.storeOffsets(blockOffsets, numBlocks);

		return header.write(output);
	}
	else
	{
		auto monoHeader = HiseLosslessHeader::createMonolithHeader(numChannels, sampleRate);

		return monoHeader.write(output);
	}

	
}


bool HiseLosslessAudioFormatWriter::writeDataFromTemp()
{
	if (usesTempFile)
	{
		FileOutputStream* to = dynamic_cast<FileOutputStream*>(tempOutputStream.get());

		jassert(to != nullptr);
        jassert(to->getFile() == tempFile->getFile());
        
        // Try to just move the temp file to its target, but only if they're in the same directory
        auto* fileOutputStream = getFileOutputStream();
        if (fileOutputStream
            && fileOutputStream->getFile() == tempFile->getTargetFile()
            && fileOutputStream->getFile().getParentDirectory() == tempFile->getFile().getParentDirectory()
            && tempFile->overwriteTargetFileWithTemporary()) {
            tempFile->deleteTemporaryFile();
            return true;
        }

        // Otherwise, copy it over
        FileInputStream fis(to->getFile());
        return output->writeFromInputStream(fis, fis.getTotalLength()) == fis.getTotalLength();
	}
	else
	{
		MemoryOutputStream* to = dynamic_cast<MemoryOutputStream*>(tempOutputStream.get());

		jassert(to != nullptr);

		MemoryInputStream mis(to->getData(), to->getDataSize(), false);
		return output->writeFromInputStream(mis, mis.getTotalLength()) == mis.getTotalLength();
	}
}

FileOutputStream* HiseLosslessAudioFormatWriter::getFileOutputStream()
{
    return dynamic_cast<FileOutputStream*>(output);
}

void HiseLosslessAudioFormatWriter::deleteTemp()
{
	// If you hit this assertion, it means that you didn't call flush after writing the last data.
	// This means nothing will get written to the actual output stream...
	jassert(tempWasFlushed);

    tempOutputStream = nullptr;
    tempFile = nullptr;
}

} // namespace hlac
