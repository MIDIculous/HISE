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

MainController::MainController():
	sampleManager(new SampleManager(this)),
	allNotesOffFlag(false),
	maxBufferSize(-1),
	cpuBufferSize(0),
	sampleRate(-1.0),
	temp_usage(0.0f),
	uptime(0.0),
	bpm(120.0),
	bpmFromHost(120.0),
	hostIsPlaying(false),
	console(nullptr),
	voiceAmount(0),
	scrollY(0),
	mainLookAndFeel(new KnobLookAndFeel()),
	mainCommandManager(new ApplicationCommandManager()),
	shownComponents(0),
	plotter(nullptr),
	usagePercent(0),
	scriptWatchTable(nullptr),
    globalPitchFactor(1.0),
    midiInputFlag(false),
	macroManager(this),
	autoSaver(this),
	delayedRenderer(this),
	enablePluginParameterUpdate(true),
	customTypeFaceData(ValueTree("CustomFonts")),
	masterEventBuffer(),
	eventIdHandler(masterEventBuffer),
	userPresetHandler(this),
	codeHandler(this),
	processorChangeHandler(this),
	killStateHandler(this),
	debugLogger(this),
	//presetLoadRampFlag(OldUserPresetHandler::Active),
	suspendIndex(0),
	controlUndoManager(new UndoManager())
{
	BACKEND_ONLY(popupConsole = nullptr);
	BACKEND_ONLY(usePopupConsole = false);

	BACKEND_ONLY(shownComponents.setBit(BackendCommandTarget::Keyboard, 1));
	BACKEND_ONLY(shownComponents.setBit(BackendCommandTarget::Macros, 0));

	LOG_START("Initialising MainController"); 

	TempoSyncer::initTempoData();
    
	globalVariableArray.insertMultiple(0, var::undefined(), NUM_GLOBAL_VARIABLES);
	globalVariableObject = new DynamicObject();

	toolbarProperties = DefaultFrontendBar::createDefaultProperties();

	hostInfo = new DynamicObject();
    
#if HI_RUN_UNIT_TESTS

	UnitTestRunner runner;

	runner.setAssertOnFailure(false);

	runner.runAllTests();

	

#endif
};


MainController::~MainController()
{


//    sampleManager = nullptr;
//    Logger::setCurrentLogger(nullptr);
	logger = nullptr;
	masterReference.clear();
	customTypeFaces.clear();
	userPresetData = nullptr;
}


const CriticalSection & MainController::getLock() const
{
	if (getDebugLogger().isLogging() && MessageManager::getInstance()->isThisTheMessageThread())
	{
		ScopedTryLock sl(processLock);

		if (sl.isLocked())
		{
			getDebugLogger().setStackBacktrace(SystemStats::getStackBacktrace());
		}
	}

	return processLock;
}

void MainController::loadPresetFromFile(const File &f, Component* /*mainEditor*/)
{
	auto f2 = [f](Processor* p)
	{
		FileInputStream fis(f);

		ValueTree v = ValueTree::readFromStream(fis);
		p->getMainController()->loadPresetFromValueTree(v);
		return true;
	};

#if USE_BACKEND
	const bool synchronous = CompileExporter::isExportingFromCommandLine();

	if (synchronous)
		f2(getMainSynthChain());
	else
		killAndCallOnLoadingThread(f2);
#else
	jassertfalse;
#endif
}

void MainController::clearPreset()
{
	ScopedLock sl(getLock());

	jassert(!getMainSynthChain()->areVoicesActive());

	getMacroManager().getMidiControlAutomationHandler()->getMPEData().clear();

	getScriptComponentEditBroadcaster()->getUndoManager().clearUndoHistory();

	getMainSynthChain()->reset();

	globalVariableObject->clear();

	toolbarProperties = DefaultFrontendBar::createDefaultProperties();

	for (int i = 0; i < 127; i++)
	{
		setKeyboardCoulour(i, Colours::transparentBlack);
	}
    
	clearIncludedFiles();

	getSampleManager().getImagePool()->clearData();
	getSampleManager().getAudioSampleBufferPool()->clearData();


    changed = false;
}

void MainController::loadPresetFromValueTree(const ValueTree &v, Component* /*mainEditor*/)
{
#if USE_BACKEND
    const bool isCommandLine = CompileExporter::isExportingFromCommandLine();
    const bool isSampleLoadingThread = killStateHandler.getCurrentThread() == KillStateHandler::SampleLoadingThread;
    
	jassert(isCommandLine || isSampleLoadingThread);
    ignoreUnused(isCommandLine, isSampleLoadingThread);
#endif

	if (v.isValid() && v.getProperty("Type", var::undefined()).toString() == "SynthChain")
	{

		if (v.getType() != Identifier("Processor"))
		{
			jassertfalse;
			
		}

		loadPresetInternal(v);
	}
	else
	{
		PresetHandler::showMessageWindow("No valid container", "This preset is not a container file", PresetHandler::IconType::Error);
	}
}


void MainController::loadPresetInternal(const ValueTree& v)
{
	try
	{
		getSampleManager().setPreloadFlag();

		ModulatorSynthChain *synthChain = getMainSynthChain();

#if USE_BACKEND
        const bool isCommandLine = CompileExporter::isExportingFromCommandLine();
        const bool isSampleLoadingThread = killStateHandler.getCurrentThread() == KillStateHandler::SampleLoadingThread;
        
		jassert(isCommandLine || isSampleLoadingThread);
        ignoreUnused(isCommandLine, isSampleLoadingThread);
#endif
        
		jassert(!synthChain->areVoicesActive());

		clearPreset();

		getSampleManager().setShouldSkipPreloading(true);

		// Reset the sample rate so that prepareToPlay does not get called in restoreFromValueTree
		// synthChain->setCurrentPlaybackSampleRate(-1.0);
		synthChain->setId(v.getProperty("ID", "MainSynthChain"));

		skipCompilingAtPresetLoad = true;

		synthChain->restoreFromValueTree(v);

		skipCompilingAtPresetLoad = false;

		synthChain->compileAllScripts();

		if (sampleRate > 0.0)
		{
			LOG_START("Initialising audio callback");

			synthChain->prepareToPlay(sampleRate, maxBufferSize.get());
		}

		synthChain->loadMacrosFromValueTree(v);


		getSampleManager().getAudioSampleBufferPool()->clearData();

#if USE_BACKEND
		Processor::Iterator<ModulatorSynth> iter(synthChain, false);

		while (ModulatorSynth *synth = iter.getNextProcessor())
		{
			synth->setEditorState(Processor::EditorState::Folded, true);
		}

		changed = false;

		auto f = [](Processor* synthChain)
		{
			synthChain->sendRebuildMessage(true);
			return true;
		};

		killAndCallOnMessageThread(f);
		

		getSampleManager().preloadEverything();

#endif

		allNotesOff(true);
	}
	catch (String& errorMessage)
	{
		ignoreUnused(errorMessage);

#if USE_BACKEND
		writeToConsole(errorMessage, 1, getMainSynthChain());
#else
		DBG(errorMessage);
#endif
	}

	
}




void MainController::startCpuBenchmark(int bufferSize_)
{
	cpuBufferSize.set(bufferSize_);
	temp_usage = (Time::highResolutionTicksToSeconds(Time::getHighResolutionTicks()));
}

void MainController::compileAllScripts()
{
	Processor::Iterator<JavascriptProcessor> it(getMainSynthChain());

	auto& set = globalVariableObject->getProperties();

	for (int i = 0; i < set.size(); i++)
	{
		set.set(set.getName(i), var());
	}

	JavascriptProcessor *sp;
		
	while((sp = it.getNextProcessor()) != nullptr)
	{
		if (sp->isConnectedToExternalFile())
		{
			sp->reloadFromFile();
		}
		else
		{
			sp->compileScript();
		}
	}
};

void MainController::allNotesOff(bool resetSoftBypassState/*=false*/)
{
	if (resetSoftBypassState)
	{
		auto f = [](Processor* p)
		{
			Processor::Iterator<ModulatorSynth> iter(p);

			while (auto s = iter.getNextProcessor())
			{
				if (!s->isBypassed())
				{
					s->setSoftBypass(false);
				}
			}

			return true;
		};

		getKillStateHandler().killVoicesAndCall(getMainSynthChain(), f, KillStateHandler::TargetThread::MessageThread);
	}
	else
	{
		allNotesOffFlag = true;
	}
}

void MainController::stopCpuBenchmark()
{
	const float thisUsage = 100.0f * (float)((Time::highResolutionTicksToSeconds(Time::getHighResolutionTicks()) - temp_usage) * sampleRate / cpuBufferSize.get());
	
	const float lastUsage = usagePercent.load();
	
	if (thisUsage > lastUsage)
	{
		usagePercent.store(thisUsage);
	}
	else
	{
		usagePercent.store(lastUsage*0.99f);
	}
}

void MainController::killAndCallOnMessageThread(const ProcessorFunction& f)
{
	getKillStateHandler().killVoicesAndCall(getMainSynthChain(), f, KillStateHandler::MessageThread);
}

void MainController::killAndCallOnAudioThread(const ProcessorFunction& f)
{
	getKillStateHandler().killVoicesAndCall(getMainSynthChain(), f, KillStateHandler::AudioThread);
}

void MainController::killAndCallOnLoadingThread(const ProcessorFunction& f)
{
	getKillStateHandler().killVoicesAndCall(getMainSynthChain(), f, KillStateHandler::SampleLoadingThread);
}

int MainController::getNumActiveVoices() const
{
	return getMainSynthChain()->getNumActiveVoices();
}

void MainController::replaceReferencesToGlobalFolder()
{
	ModulatorSynthChain *root = getMainSynthChain();

	Processor::Iterator<ExternalFileProcessor> it(root);

	ExternalFileProcessor *p = nullptr;

	while ((p = it.getNextProcessor()) != nullptr)
	{
		p->replaceReferencesWithGlobalFolder();
	}
}

void MainController::beginParameterChangeGesture(int index)			{ dynamic_cast<PluginParameterAudioProcessor*>(this)->beginParameterChangeGesture(index); }

void MainController::endParameterChangeGesture(int index)			{ dynamic_cast<PluginParameterAudioProcessor*>(this)->endParameterChangeGesture(index); }

void MainController::setPluginParameter(int index, float newValue)  { dynamic_cast<PluginParameterAudioProcessor*>(this)->setParameterNotifyingHost(index, newValue); }

Processor *MainController::createProcessor(FactoryType *factory,
											 const Identifier &typeName,
											 const String &id)
{		
	// Every chain must have a factory type!
	jassert(factory != nullptr);

	// Create the processor using the factory type of the parent chain
	Processor *p = factory->createProcessor(factory->getProcessorTypeIndex(typeName), id);

	return p;
};


void MainController::setKeyboardCoulour(int keyNumber, Colour colour)
{
	keyboardState.setColourForSingleKey(keyNumber, colour);
}

CustomKeyboardState & MainController::getKeyboardState()
{
	return keyboardState;
}

void MainController::setLowestKeyToDisplay(int lowestKeyToDisplay)
{
	keyboardState.setLowestKeyToDisplay(lowestKeyToDisplay);
};

float MainController::getVoiceAmountMultiplier() const
{
    if(HiseDeviceSimulator::isAUv3())
    {
        return 0.25f;
    }
    
	auto m = dynamic_cast<const GlobalSettingManager*>(this)->voiceAmountMultiplier;

	switch (m)
	{
	case 8:  return 0.125f;
	case 4:  return 0.25f;
	case 2:  return 0.5f;
	case 1: return 1.0f;
	default:  return 1.0f;
	}
}

void MainController::setPlotter(Plotter *p)
{
	plotter = p;
};

void MainController::skin(Component &c)
{
    c.setLookAndFeel(mainLookAndFeel);
    
    c.setColour(MacroControlledObject::HiBackgroundColours::upperBgColour, Colour(0x66333333));
    c.setColour(MacroControlledObject::HiBackgroundColours::lowerBgColour, Colour(0xfb111111));
    c.setColour(MacroControlledObject::HiBackgroundColours::outlineBgColour, Colours::white.withAlpha(0.3f));
	c.setColour(MacroControlledObject::HiBackgroundColours::textColour, Colours::white);


#if 0
    if(dynamic_cast<Slider*>(&c) != nullptr) 
		dynamic_cast<Slider*>(&c)->setScrollWheelEnabled(false);
#endif
};



void MainController::setCurrentViewChanged()
{
#if USE_BACKEND
	if(getMainSynthChain() != nullptr)
	{
		getMainSynthChain()->setCurrentViewChanged();
	}
#endif
}

void MainController::setGlobalVariable(int index, var newVariable)
{
	if (index >= NUM_GLOBAL_VARIABLES || index < 0)
	{
		jassertfalse;
		return;
	}

	ScopedLock sl(getLock());
	globalVariableArray.setUnchecked(index, newVariable.clone());
}

var MainController::getGlobalVariable(int index) const
{
	if (index >= NUM_GLOBAL_VARIABLES || index < 0)
	{
		jassertfalse;
		return var::undefined();
	}

	ScopedLock sl(getLock());
	return globalVariableArray.getUnchecked(index);
}

void MainController::storePlayheadIntoDynamicObject(AudioPlayHead::CurrentPositionInfo &/*newPosition*/)
{
	//static const Identifier bpmId("bpm");
	//static const Identifier timeSigNumerator("timeSigNumerator");
	//static const Identifier timeSigDenominator("timeSigDenominator");
	//static const Identifier timeInSamples("timeInSamples");
	//static const Identifier timeInSeconds("timeInSeconds");
	//static const Identifier editOriginTime("editOriginTime");
	//static const Identifier ppqPosition("ppqPosition");
	//static const Identifier ppqPositionOfLastBarStart("ppqPositionOfLastBarStart");
	//static const Identifier frameRate("frameRate");
	//static const Identifier isPlaying("isPlaying");
	//static const Identifier isRecording("isRecording");
	//static const Identifier ppqLoopStart("ppqLoopStart");
	//static const Identifier ppqLoopEnd("ppqLoopEnd");
	//static const Identifier isLooping("isLooping");

	//ScopedLock sl(getLock());

	//hostInfo->setProperty(bpmId, newPosition.bpm);
	//hostInfo->setProperty(timeSigNumerator, newPosition.timeSigNumerator);
	//hostInfo->setProperty(timeSigDenominator, newPosition.timeSigDenominator);
	//hostInfo->setProperty(timeInSamples, newPosition.timeInSamples);
	//hostInfo->setProperty(timeInSeconds, newPosition.timeInSeconds);
	//hostInfo->setProperty(editOriginTime, newPosition.editOriginTime);
	//hostInfo->setProperty(ppqPosition, newPosition.ppqPosition);
	//hostInfo->setProperty(ppqPositionOfLastBarStart, newPosition.ppqPositionOfLastBarStart);
	//hostInfo->setProperty(frameRate, newPosition.frameRate);
	//hostInfo->setProperty(isPlaying, newPosition.isPlaying);
	//hostInfo->setProperty(isRecording, newPosition.isRecording);
	//hostInfo->setProperty(ppqLoopStart, newPosition.ppqLoopStart);
	//hostInfo->setProperty(ppqLoopEnd, newPosition.ppqLoopEnd);
	//hostInfo->setProperty(isLooping, newPosition.isLooping);
}

void MainController::processBlockCommon(AudioSampleBuffer &buffer, MidiBuffer &midiMessages)
{
	ADD_GLITCH_DETECTOR(getMainSynthChain(), DebugLogger::Location::MainRenderCallback);
    
	numSamplesThisBlock = buffer.getNumSamples();

	getDebugLogger().checkAudioCallbackProperties(thisAsProcessor->getSampleRate(), numSamplesThisBlock);

	ScopedNoDenormals snd;


	getDebugLogger().checkPriorityInversion(processLock);

	ScopedTryLock sl(processLock);

	if (!sl.isLocked())
	{
		buffer.clear();
		midiMessages.clear();
		return;
	}

	ModulatorSynthChain *synthChain = getMainSynthChain();

	jassert(maxBufferSize.get() >= numSamplesThisBlock);

#if !FRONTEND_IS_PLUGIN
    
	keyboardState.processNextMidiBuffer(midiMessages, 0, numSamplesThisBlock, true);

	getMacroManager().getMidiControlAutomationHandler()->handleParameterData(midiMessages); // TODO_BUFFER: Move this after the next line...

	masterEventBuffer.addEvents(midiMessages);

	killStateHandler.handleKillState();

    if (!masterEventBuffer.isEmpty()) setMidiInputFlag();
    
	eventIdHandler.handleEventIds();

	getDebugLogger().logEvents(masterEventBuffer);

#else
	ignoreUnused(midiMessages);

	masterEventBuffer.clear();
#endif

#if ENABLE_HOST_INFO
	AudioPlayHead::CurrentPositionInfo newTime;

	if ( thisAsProcessor->getPlayHead() != nullptr && thisAsProcessor->getPlayHead()->getCurrentPosition(newTime))
	{
		lastPosInfo = newTime;
	}
	else lastPosInfo.resetToDefault();

	storePlayheadIntoDynamicObject(lastPosInfo);
	
	bpmFromHost = lastPosInfo.bpm;

	if (hostIsPlaying != lastPosInfo.isPlaying)
	{
		hostIsPlaying = lastPosInfo.isPlaying;

		FX_ONLY(masterEventBuffer.addEvent(HiseEvent(hostIsPlaying ? HiseEvent::Type::NoteOn :
															 HiseEvent::Type::NoteOff, 
											 60, 127, 1));)

	}

	if (bpmFromHost == 0.0)
		bpmFromHost = 120.0;

	auto otherBpm = dynamic_cast<GlobalSettingManager*>(this)->globalBPM;

	if (otherBpm > 0)
		setBpm((double)otherBpm);
	else
	{
		setBpm(bpmFromHost);
	}
	
#endif

#if ENABLE_CPU_MEASUREMENT
	startCpuBenchmark(numSamplesThisBlock);
#endif

#if !FRONTEND_IS_PLUGIN

	if(replaceBufferContent) buffer.clear();

	checkAllNotesOff();

#endif


#if USE_MIDI_CONTROLLERS_FOR_MACROS
	handleControllersForMacroKnobs(midiMessages);
#endif

	
#if FRONTEND_IS_PLUGIN


    const bool isUsingMultiChannel = multiChannelBuffer.getNumChannels() > 2;
    
    if(isUsingMultiChannel)
    {
        AudioSampleBuffer thisMultiChannelBuffer(multiChannelBuffer.getArrayOfWritePointers(), multiChannelBuffer.getNumChannels(), 0, numSamplesThisBlock);
        
        thisMultiChannelBuffer.clear();
        
        FloatVectorOperations::copy(thisMultiChannelBuffer.getWritePointer(0), buffer.getReadPointer(0), numSamplesThisBlock);
        FloatVectorOperations::copy(thisMultiChannelBuffer.getWritePointer(1), buffer.getReadPointer(1), numSamplesThisBlock);
        
        synthChain->renderNextBlockWithModulators(thisMultiChannelBuffer, masterEventBuffer);
        
        buffer.clear();
        
		// Just use the first two channels. You need to route back all your send channels to the first stereo pair.
		FloatVectorOperations::add(buffer.getWritePointer(0), thisMultiChannelBuffer.getReadPointer(0), numSamplesThisBlock);
		FloatVectorOperations::add(buffer.getWritePointer(1), thisMultiChannelBuffer.getReadPointer(1), numSamplesThisBlock);
    }
    else
    {
        synthChain->renderNextBlockWithModulators(buffer, masterEventBuffer);
    }
	

#else


	AudioSampleBuffer thisMultiChannelBuffer(multiChannelBuffer.getArrayOfWritePointers(), multiChannelBuffer.getNumChannels(), 0, numSamplesThisBlock);

	thisMultiChannelBuffer.clear();

	if (previewBufferIndex != -1)
	{
		int numToPlay = jmin<int>(numSamplesThisBlock, previewBuffer.getNumSamples() - previewBufferIndex);

		FloatVectorOperations::copy(multiChannelBuffer.getWritePointer(0, 0), previewBuffer.getReadPointer(0, previewBufferIndex), numToPlay);
		FloatVectorOperations::copy(multiChannelBuffer.getWritePointer(1, 0), previewBuffer.getReadPointer(1, previewBufferIndex), numToPlay);

		previewBufferIndex += numToPlay;

		if (previewBufferIndex >= previewBuffer.getNumSamples())
		{
			previewBuffer = AudioSampleBuffer();
			previewBufferIndex = -1;
		}
	}


	synthChain->renderNextBlockWithModulators(thisMultiChannelBuffer, masterEventBuffer);

	const bool isUsingMultiChannel = buffer.getNumChannels() != 2;

	if (!isUsingMultiChannel)
	{
		if (replaceBufferContent)
		{
			FloatVectorOperations::copy(buffer.getWritePointer(0), thisMultiChannelBuffer.getReadPointer(0), numSamplesThisBlock);
			FloatVectorOperations::copy(buffer.getWritePointer(1), thisMultiChannelBuffer.getReadPointer(1), numSamplesThisBlock);
		}
		else
		{
			FloatVectorOperations::add(buffer.getWritePointer(0), thisMultiChannelBuffer.getReadPointer(0), numSamplesThisBlock);
			FloatVectorOperations::add(buffer.getWritePointer(1), thisMultiChannelBuffer.getReadPointer(1), numSamplesThisBlock);
		}
	}
	else
	{
		auto& matrix = getMainSynthChain()->getMatrix();

		for (int i = 0; i < matrix.getNumSourceChannels(); i++)
		{
			if (replaceBufferContent)
				FloatVectorOperations::copy(buffer.getWritePointer(i), thisMultiChannelBuffer.getReadPointer(i), numSamplesThisBlock);
			else
				FloatVectorOperations::add(buffer.getWritePointer(i), thisMultiChannelBuffer.getReadPointer(i), numSamplesThisBlock);
		}
	}

#if USE_HARD_CLIPPER
	
#else
	// on iOS samples above 1.0f create a nasty digital distortion
	if (HiseDeviceSimulator::isMobileDevice())
	{
		for (int i = 0; i < buffer.getNumChannels(); i++)
			FloatVectorOperations::clip(buffer.getWritePointer(i, 0), buffer.getReadPointer(i, 0), -1.0f, 1.0f, numSamplesThisBlock);
	}
#endif

	

#endif

#if ENABLE_CPU_MEASUREMENT
	stopCpuBenchmark();
#endif

    if(sampleRate > 0.0)
    {
        uptime += double(numSamplesThisBlock) / sampleRate;
    }

#if USE_BACKEND
	getDebugLogger().recordOutput(buffer);
#endif

	midiMessages.clear();

}

void MainController::prepareToPlay(double sampleRate_, int samplesPerBlock)
{
    LOG_START("Preparing playback");
    
	maxBufferSize = samplesPerBlock;
	sampleRate = sampleRate_;
 
	// Prevent high buffer sizes from blowing up the 350MB limitation...
	if (HiseDeviceSimulator::isAUv3())
	{
		maxBufferSize = jmin<int>(samplesPerBlock, 1024);
	}

    thisAsProcessor = dynamic_cast<AudioProcessor*>(this);
    
#if ENABLE_CONSOLE_OUTPUT
	if (logger == nullptr)
	{
		logger = new ConsoleLogger(getMainSynthChain());
//        Logger::setCurrentLogger(logger);
	}

#endif
    
	updateMultiChannelBuffer(getMainSynthChain()->getMatrix().getNumSourceChannels());

	

#if IS_STANDALONE_APP || IS_STANDALONE_FRONTEND
	getMainSynthChain()->getMatrix().setNumDestinationChannels(2);
#else
    
#if HISE_IOS
    getMainSynthChain()->getMatrix().setNumDestinationChannels(2);
#else
	getMainSynthChain()->getMatrix().setNumDestinationChannels(HISE_NUM_PLUGIN_CHANNELS);
#endif
    
#endif

    getMainSynthChain()->prepareToPlay(sampleRate, maxBufferSize.get());

	getMainSynthChain()->setIsOnAir(true);
}

void MainController::setBpm(double newTempo)
{
    
    
	if(bpm != newTempo)
	{
		bpm = newTempo;

		for(int i = 0; i < tempoListeners.size(); i++)
		{
			if(tempoListeners[i].get() != nullptr)
			{
				tempoListeners[i].get()->tempoChanged(bpm);
			}
			else
			{
				// delete it with removeTempoListener!
				jassertfalse;
			}
		}
	}
};

void MainController::setHostBpm(double newTempo)
{
	if (newTempo > 0.0)
	{
		int nt = jlimit(32, 280, (int)newTempo);

		dynamic_cast<GlobalSettingManager*>(this)->globalBPM = nt;
		
		setBpm(newTempo);
	}
	else
	{
		dynamic_cast<GlobalSettingManager*>(this)->globalBPM = -1;
		
		setBpm(bpmFromHost);
	}
}

void MainController::addTempoListener(TempoListener *t)
{
	ScopedLock sl(getLock());
	tempoListeners.addIfNotAlreadyThere(t);
}

void MainController::removeTempoListener(TempoListener *t)
{
	ScopedLock sl(getLock());
	tempoListeners.removeAllInstancesOf(t);
}

juce::Typeface* MainController::getFont(const String &fontName) const
{
	for (auto& tf: customTypeFaces)
	{
		auto nameToUse = tf.id.isValid() ? tf.id.toString() : tf.typeface->getName();

		if (nameToUse == fontName)
		{
			return tf.typeface.get();
		}
	}

	return nullptr;
}

Font MainController::getFontFromString(const String& fontName, float fontSize) const
{
	const Identifier id(fontName);

	for (auto& tf : customTypeFaces)
	{
		if (tf.id.isValid() && tf.id == id)
		{
			Font currentFont;
			juce::Typeface::Ptr typeface = tf.typeface;
			return Font(typeface).withHeight(fontSize);
		}
	}

	static const String boldString(" Bold");
	static const String italicString(" Italic");

	bool isBold = fontName.contains(boldString);
	bool isItalic = fontName.contains(italicString);

	auto fn = fontName.replace(boldString, "");
	fn = fn.replace(italicString, "");

	Font currentFont;

	juce::Typeface::Ptr typeface = getFont(fn);

	if (typeface != nullptr)	currentFont = Font(typeface).withHeight(fontSize);
	else						currentFont = Font(fn, fontSize, Font::plain);

	if (isBold)					currentFont = currentFont.boldened();
	if (isItalic)				currentFont = currentFont.italicised();

	return currentFont;
}


void MainController::fillWithCustomFonts(StringArray &fontList)
{
	for (auto& tf : customTypeFaces)
	{
		auto nameToUse = tf.id.isValid() ? tf.id.toString() : tf.typeface->getName();
		fontList.addIfNotAlreadyThere(nameToUse);
	}
}

void MainController::loadTypeFace(const String& fileName, const void* fontData, size_t fontDataSize, const String& fontId/*=String()*/)
{
	if (customTypeFaceData.getChildWithProperty("Name", fileName).isValid()) return;

	if (fontId.isNotEmpty() && customTypeFaceData.getChildWithProperty("FontId", fontId).isValid()) return;

	Identifier id_ = fontId.isEmpty() ? Identifier() : Identifier(fontId);

	customTypeFaces.add(CustomTypeFace(juce::Typeface::createSystemTypefaceFor(fontData, fontDataSize), id_));

	MemoryBlock mb(fontData, fontDataSize);
	
	ValueTree v("Font");
	v.setProperty("Name", fileName, nullptr);
	v.setProperty("Data", var(mb), nullptr);
	v.setProperty("Size", var((int)mb.getSize()), nullptr);
	
	if (fontId.isNotEmpty())
		v.setProperty("FontId", fontId, nullptr);


	customTypeFaceData.addChild(v, -1, nullptr);
}

int MainController::getBufferSizeForCurrentBlock() const noexcept
{
	jassert(getKillStateHandler().getCurrentThread() == KillStateHandler::AudioThread);

	return numSamplesThisBlock;
}

ValueTree MainController::exportCustomFontsAsValueTree() const
{
	return customTypeFaceData;
}


void MainController::restoreCustomFontValueTree(const ValueTree &v)
{
	customTypeFaceData = v;

	for (int i = 0; i < customTypeFaceData.getNumChildren(); i++)
	{
		ValueTree child = customTypeFaceData.getChild(i);

		if (!child.isValid())
		{
			jassertfalse;
			return;
		}

		var c = child.getProperty("Data", var::undefined());

		if (!c.isBinaryData())
		{
			jassertfalse;
			return;
		}

		MemoryBlock *mb = c.getBinaryData();

		if (mb != nullptr)
		{
			auto fontId = child.getProperty("FontId", "").toString();

			if (fontId.isNotEmpty())
			{
				Identifier id_(fontId);
				customTypeFaces.add(CustomTypeFace(juce::Typeface::createSystemTypefaceFor(mb->getData(), mb->getSize()), id_));
			}
			else
			{
				customTypeFaces.add(CustomTypeFace(juce::Typeface::createSystemTypefaceFor(mb->getData(), mb->getSize()), Identifier()));
			}
		}
		else
		{
			jassertfalse;
		}
	}
}


void MainController::insertStringAtLastActiveEditor(const String &string, bool selectArguments)
{
	if (lastActiveEditor.getComponent() != nullptr)
	{
		lastActiveEditor->getDocument().deleteSection(lastActiveEditor->getSelectionStart(), lastActiveEditor->getSelectionEnd());
        lastActiveEditor->moveCaretTo(CodeDocument::Position(lastActiveEditor->getDocument(), lastCharacterPositionOfSelectedEditor), false);

		lastActiveEditor->insertTextAtCaret(string);



		if (selectArguments)
		{
			lastActiveEditor->moveCaretLeft(false, false);

			while (!lastActiveEditor->getTextInRange(lastActiveEditor->getHighlightedRegion()).contains("("))
			{
				lastActiveEditor->moveCaretLeft(false, true);
			}

			lastActiveEditor->moveCaretRight(false, true);
		}

		lastActiveEditor->grabKeyboardFocus();
	}
}

bool MainController::checkAndResetMidiInputFlag()
{
	const bool returnValue = midiInputFlag;
	midiInputFlag = false;

	return returnValue;
}

float MainController::getGlobalCodeFontSize() const
{
	return (float)dynamic_cast<const GlobalSettingManager*>(this)->getSettingsObject().getSetting(HiseSettings::Scripting::CodeFontSize);
}

void MainController::loadUserPresetAsync(const ValueTree& v)
{
	//getMainSynthChain()->killAllVoices();
	//presetLoadRampFlag.set(OldUserPresetHandler::FadeOut);
	userPresetHandler.loadUserPreset(v);
}

#if USE_BACKEND

void MainController::writeToConsole(const String &message, int warningLevel, const Processor *p, Colour c)
{
	codeHandler.writeToConsole(message, warningLevel, p, c);

	

}

void MainController::setWatchedScriptProcessor(JavascriptProcessor *p, Component *editor)
{
	if (scriptWatchTable.getComponent() != nullptr)
	{
		scriptWatchTable->setScriptProcessor(p, dynamic_cast<ScriptingEditor*>(editor));
	}
};;

void MainController::setScriptWatchTable(ScriptWatchTable *table)
{
	scriptWatchTable = table;
}


#endif

void MainController::rebuildVoiceLimits()
{
	Processor::Iterator<ModulatorSynth> iter(getMainSynthChain());

	while (auto synth = iter.getNextProcessor())
	{
		synth->setVoiceLimit((int)synth->getAttribute(ModulatorSynth::VoiceLimit));
	}
}

void MainController::updateMultiChannelBuffer(int numNewChannels)
{
	ScopedLock sl(processLock);

	// Updates the channel amount
	multiChannelBuffer.setSize(numNewChannels, multiChannelBuffer.getNumSamples());

	ProcessorHelpers::increaseBufferIfNeeded(multiChannelBuffer, maxBufferSize.get());
}

MainController::SampleManager::~SampleManager()
{
	preloadListeners.clear();

	internalPreloadJob->signalJobShouldExit();

	const bool ok = samplerLoaderThreadPool->stopThread(2000);
	
	samplerLoaderThreadPool = nullptr;
}

void MainController::SampleManager::setShouldSkipPreloading(bool skip)
{
	skipPreloading = skip;
}

void MainController::SampleManager::preloadEverything()
{
	
	jassert(skipPreloading);

	skipPreloading = false;
	
	jassert(mc->getKillStateHandler().voicesAreKilled());

	Processor::Iterator<ModulatorSampler> it(mc->getMainSynthChain());

	Array<WeakReference<Processor>> samplersToPreload;

	while (ModulatorSampler* s = it.getNextProcessor())
	{
		if (s->hasPendingSampleLoad())
		{
			auto f = [](Processor* p)->bool {
				return static_cast<ModulatorSampler*>(p)->preloadAllSamples();
			};

			mc->getKillStateHandler().killVoicesAndCall(s, f, KillStateHandler::SampleLoadingThread);
		}
	}
}


double& MainController::SampleManager::getPreloadProgress()
{
	return internalPreloadJob->progress;
}

void MainController::SampleManager::cancelAllJobs()
{
	ScopedLock sl(getSamplerSoundLock());

	internalPreloadJob->signalJobShouldExit();
	samplerLoaderThreadPool->stopThread(2000);

	

}

void MainController::CodeHandler::setMainConsole(Console* console)
{
	mainConsole = dynamic_cast<Component*>(console);
}


} // namespace hise
