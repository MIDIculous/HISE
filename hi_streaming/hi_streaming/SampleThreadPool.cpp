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

struct SampleThreadPool::Pimpl
{
    Pimpl()
    : jobQueue(2048),
      currentlyExecutedJob(nullptr),
      diskUsage(0.0),
      counter(0)
    {}

    Atomic<int> counter;

    std::atomic<double> diskUsage;

    int64 startTime, endTime;

    moodycamel::ReaderWriterQueue<std::weak_ptr<Job>> jobQueue;

    std::shared_ptr<Job> currentlyExecutedJob;

    static const String errorMessage;
};

SampleThreadPool::SampleThreadPool()
: Thread("Sample Loading Thread"),
  pimpl(std::make_unique<Pimpl>())
{
#if LOG_POOL_ACTIVITY
    Logger::writeToLog("SampleThreadPool(): Starting thread...");
#endif
    startThread(9);
}

SampleThreadPool::~SampleThreadPool()
{
#if LOG_POOL_ACTIVITY
    Logger::writeToLog("~SampleThreadPool()");
#endif

    if (const auto currentJob = std::atomic_load(&pimpl->currentlyExecutedJob)) {
#if LOG_POOL_ACTIVITY
        Logger::writeToLog("currentJob != nullptr. Calling currentJob->signalJobShouldExit()...");
#endif
        currentJob->signalJobShouldExit();
    }

#if LOG_POOL_ACTIVITY
    Logger::writeToLog("~SampleThreadPool(): Calling stopThread()...");
#endif
    const bool stopped = stopThread(3000);
    jassert(stopped);
#if LOG_POOL_ACTIVITY
    Logger::writeToLog(String("~SampleThreadPool(): ") + (stopped ? "stopped." : "ERROR: NOT stopped."));
#endif
}

double SampleThreadPool::getDiskUsage() const noexcept
{
    return pimpl->diskUsage.load();
}

void SampleThreadPool::addJob(std::weak_ptr<Job> jobToAdd, bool)
{
    const auto j = jobToAdd.lock();
    if (!j) {
#if LOG_POOL_ACTIVITY
        Logger::writeToLog("SampleThreadPool::addJob(): Not adding job (already expired).");
#endif
        return;
    }

    ++pimpl->counter;

#if LOG_SAMPLE_RENDERING
    if (j->isQueued()) {
        Logger::writeToLog(pimpl->errorMessage);
        Logger::writeToLog(String(pimpl->counter.get()));
    }
#endif

#if LOG_POOL_ACTIVITY
    Logger::writeToLog("SampleThreadPool::addJob(): Adding job: " + j->getName() + "...");
#endif
    j->setQueued(true);
    pimpl->jobQueue.enqueue(j);
    notify();
}

void SampleThreadPool::run()
{
    JUCE_TRY
    {
#if LOG_POOL_ACTIVITY
        bool wasWorking = false;
#endif

        while (!threadShouldExit()) {
            if (auto* next = pimpl->jobQueue.peek()) {
#if LOG_POOL_ACTIVITY
                if (!wasWorking) {
                    Logger::writeToLog("SampleThreadPool::run(): Starting work.");
                    wasWorking = true;
                }
#endif

                const auto j = next->lock();

#if ENABLE_CPU_MEASUREMENT
                const int64 lastEndTime = pimpl->endTime;
                pimpl->startTime = Time::getHighResolutionTicks();
#endif

                if (j) {
#if LOG_POOL_ACTIVITY
                    Logger::writeToLog("SampleThreadPool::run(): Running job: " + j->getName() + "...");
#endif
                    std::atomic_store(&pimpl->currentlyExecutedJob, j);

                    j->setRunning(true);
                    const auto status = j->runJob();
                    j->setRunning(false);

                    if (status == Job::jobHasFinished) {
#if LOG_POOL_ACTIVITY
                        Logger::writeToLog("SampleThreadPool::run(): Job finished: " + j->getName() + ".");
#endif
                        pimpl->jobQueue.pop();
                        j->setQueued(false);
                        --pimpl->counter;
                    }
#if LOG_POOL_ACTIVITY
                    else {
                        Logger::writeToLog("SampleThreadPool::run(): Job didn't finish: " + j->getName() + ". Keeping it in queue.");
                    }
#endif

                    std::atomic_store(&pimpl->currentlyExecutedJob, std::shared_ptr<Job>(nullptr));
                }
                else {
#if LOG_POOL_ACTIVITY
                    Logger::writeToLog("SampleThreadPool::run(): Job was already deleted.");
#endif
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

            else {
#if LOG_POOL_ACTIVITY
                if (wasWorking) {
                    wasWorking = false;
                    Logger::writeToLog("SampleThreadPool::run(): Stopping work.");
                }
#endif
                wait(500);
            }
        }
#if LOG_POOL_ACTIVITY
        Logger::writeToLog("SampleThreadPool::run(): threadShouldExit() -> stopped.");
#endif
    }
    JUCE_CATCH_EXCEPTION
}

const String SampleThreadPool::Pimpl::errorMessage("HDD overflow");

} // namespace hise
