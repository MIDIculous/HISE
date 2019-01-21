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

#ifndef SAMPLETHREADPOOL_H_INCLUDED
#define SAMPLETHREADPOOL_H_INCLUDED

namespace hise {
using namespace juce;

class SampleThreadPool : public Thread
{
public:
    SampleThreadPool();

    ~SampleThreadPool();


    class Job
    {
    public:
        Job() = default;

        enum JobStatus {
            jobHasFinished = 0,
            jobNeedsRunningAgain
        };

        virtual JobStatus runJob() = 0;

        virtual String getName() = 0;

        bool shouldExit() const noexcept { return shouldExit_; }

        void signalJobShouldExit() noexcept { shouldExit_ = true; }

        void setRunning(bool running) noexcept { this->running = running; }
        bool isRunning() const noexcept { return running; };

        void setQueued(bool queued) noexcept { this->queued = queued; }
        bool isQueued() const noexcept { return queued; };

    private:
        std::atomic<bool> queued{ false };
        std::atomic<bool> running{ false };
        std::atomic<bool> shouldExit_{ false };
    };

    double getDiskUsage() const noexcept;

    void addJob(std::weak_ptr<Job> jobToAdd, bool unused);

    void run() override;

private:
    struct Pimpl;

    const std::unique_ptr<Pimpl> pimpl;
};

typedef SampleThreadPool::Job SampleThreadPoolJob;

} // namespace hise
#endif // SAMPLETHREADPOOL_H_INCLUDED
