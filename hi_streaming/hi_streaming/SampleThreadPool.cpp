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


struct SampleThreadPool::Pimpl
{
	Pimpl() :
		jobQueue(2048),
		currentlyExecutedJob(nullptr),
		diskUsage(0.0),
		counter(0)
	{};

	~Pimpl()
	{}

	Atomic<int> counter;

	std::atomic<double> diskUsage;

	int64 startTime, endTime;

    moodycamel::ReaderWriterQueue<std::weak_ptr<Job>> jobQueue;

    std::shared_ptr<Job> currentlyExecutedJob;

	static const String errorMessage;
};

SampleThreadPool::SampleThreadPool() :
	Thread("Sample Loading Thread"),
	pimpl(new Pimpl())
{

	startThread(9);
}

SampleThreadPool::~SampleThreadPool()
{
    if (const auto currentJob = std::atomic_load(&pimpl->currentlyExecutedJob))
        currentJob->signalJobShouldExit();
    
    const bool stopped = stopThread(3000);
    jassert(stopped);
}

double SampleThreadPool::getDiskUsage() const noexcept
{
	return pimpl->diskUsage.load();
}

void SampleThreadPool::addJob(std::weak_ptr<Job> jobToAdd, bool)
{
    const auto j = jobToAdd.lock();
    if (!j)
        return;
    
    ++pimpl->counter;

#if ENABLE_CONSOLE_OUTPUT
	if (j->isQueued())
	{
		Logger::writeToLog(pimpl->errorMessage);
		Logger::writeToLog(String(pimpl->counter.get()));
	}
#endif

    j->queued.store(true);
    pimpl->jobQueue.enqueue(j);
    notify();
}

void SampleThreadPool::run()
{
	while (!threadShouldExit())
	{
        if (auto* next = pimpl->jobQueue.peek()) {
            const auto j = next->lock();
            
#if ENABLE_CPU_MEASUREMENT
            const int64 lastEndTime = pimpl->endTime;
            pimpl->startTime = Time::getHighResolutionTicks();
#endif
            
            if (j)
            {
                std::atomic_store(&pimpl->currentlyExecutedJob, j);
                
                j->currentThread.store(this);
                
                j->running.store(true);
                
                const Job::JobStatus status = j->runJob();
                
                j->running.store(false);
                
                if (status == Job::jobHasFinished)
                {
                    pimpl->jobQueue.pop();
                    j->queued.store(false);
                    --pimpl->counter;
                }
                
                std::atomic_store(&pimpl->currentlyExecutedJob, std::shared_ptr<Job>(nullptr));
            }
            else {
                // Job was already deleted. Remove from queue:
                pimpl->jobQueue.pop();
                --pimpl->counter;
            }
            
            
#if ENABLE_CPU_MEASUREMENT
            pimpl->endTime = Time::getHighResolutionTicks();
            
            const int64 idleTime = pimpl->startTime - lastEndTime;
            const int64 busyTime = pimpl->endTime - pimpl->startTime;
            
            pimpl->diskUsage.store((double)busyTime / (double)(idleTime + busyTime));
#endif
            
        }
#if 0 // Set this to true to enable defective threading (for debugging purposes)
        wait(500);
#else
        else
        {
            wait(500);
        }
#endif
	}
}

const String SampleThreadPool::Pimpl::errorMessage("HDD overflow");

} // namespace hise
