#include "GameTimer.h"
#include "framework.h"
#include "UnheardEngine.h"

#if WITH_EDITOR
std::mutex GTimeScopeLock;
std::vector<std::pair<std::string, float>> UHGameTimerScope::RegisteredGameTime;
#endif

UHGameTimer::UHGameTimer()
	: DeltaTime(0.0)
	, BaseTime(0)
	, PausedTime(0)
	, StopTime(0)
	, PreviousTime(0)
	, CurrentTime(0)
	, bStopped(false)
{
	// this query will always success after win xp
	int64_t CountsPerSec;
	QueryPerformanceFrequency((LARGE_INTEGER*)&CountsPerSec);
	SecondsPerCount = 1.0 / static_cast<double>(CountsPerSec);
}

// Returns the total time elapsed since Reset() was called, NOT counting any
// time when the clock is stopped.
float UHGameTimer::GetTotalTime() const
{
	if (bStopped)
	{
		return static_cast<float>(((StopTime - PausedTime) - BaseTime) * SecondsPerCount);
	}

	return static_cast<float>(((CurrentTime - PausedTime) - BaseTime) * SecondsPerCount);
}

float UHGameTimer::GetDeltaTime() const
{
	return static_cast<float>(DeltaTime);
}

// get current time, GetCurrentTime() will conflict to other function, so I name it GetTime()
int64_t UHGameTimer::GetTime() const
{
	int64_t CurrTime;
	QueryPerformanceCounter((LARGE_INTEGER*)&CurrTime);
	return CurrTime;
}

double UHGameTimer::GetSecondsPerCount() const
{
	return SecondsPerCount;
}

void UHGameTimer::Reset()
{
	QueryPerformanceCounter((LARGE_INTEGER*)&BaseTime);
	PreviousTime = BaseTime;
	StopTime = 0;
	bStopped = false;
}

void UHGameTimer::Start()
{
	// Accumulate the time elapsed between stop and start pairs
	if (bStopped)
	{
		int64_t StartTime;
		QueryPerformanceCounter((LARGE_INTEGER*)&StartTime);

		PausedTime += StartTime - StopTime;
		PreviousTime = StartTime;
		StopTime = 0;
		bStopped = false;
	}
}

void UHGameTimer::Stop()
{
	if (!bStopped)
	{
		QueryPerformanceCounter((LARGE_INTEGER*)&StopTime);
		bStopped = true;
	}
}

void UHGameTimer::Tick()
{
	if (bStopped)
	{
		DeltaTime = 0.0;
		return;
	}

	QueryPerformanceCounter((LARGE_INTEGER*)&CurrentTime);

	// Time difference between this frame and the previous.
	DeltaTime = static_cast<double>(CurrentTime - PreviousTime) * SecondsPerCount;

	// Prepare for next frame.
	PreviousTime = CurrentTime;

	if (DeltaTime < 0.0)
	{
		DeltaTime = 0.0;
	}

	// check if the true delta time is larger than a threshold
	// this can happen with debug breakpoint or some other pausing mechanisms, correct it to a constant rate
	if (DeltaTime > 1.0f)
	{
		// fix to 60hz should be fine for now, follow the target FPS in the future if needed
		double DesiredDeltaTime = 0.01666666666666666666666666666667;
		DeltaTime = DesiredDeltaTime;
	}
}

// UHGameTimerScope
UHGameTimerScope::UHGameTimerScope(std::string InName, bool bPrintTimeAfterStop)
{
#if WITH_EDITOR
	this->bPrintTimeAfterStop = bPrintTimeAfterStop;
	Name = InName;
	Reset();
	Start();
#endif
}

UHGameTimerScope::~UHGameTimerScope()
{
#if WITH_EDITOR
	Stop();

	const float TotalTime = GetTotalTime() * 1000.0f;
	if (bPrintTimeAfterStop)
	{
		UHE_LOG(Name + " takes " + std::to_string(TotalTime) + " ms.\n");
	}

	std::unique_lock<std::mutex> Lock(GTimeScopeLock);
	RegisteredGameTime.push_back(std::make_pair(Name, TotalTime));
#endif
}

const std::vector<std::pair<std::string, float>>& UHGameTimerScope::GetResiteredGameTime()
{
#if WITH_EDITOR
	std::unique_lock<std::mutex> Lock(GTimeScopeLock);
	return RegisteredGameTime;
#endif
}

void UHGameTimerScope::ClearRegisteredGameTime()
{
#if WITH_EDITOR
	std::unique_lock<std::mutex> Lock(GTimeScopeLock);
	RegisteredGameTime.clear();
#endif
}