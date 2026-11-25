#include "Animator.h"
#include <algorithm>

// Hours of game time since the start of the game; animations run on game time.
static inline float GameHoursNow(TimeGlobals* t) {
	return t->GameDaysPassed->data * 24;
}

Animator::Animator()
	: time(TimeGlobals::Get()), startValue(0), endValue(0), startTime(0), endTime(0), running(false), switched(false) {
}

Animator::~Animator() {
}

// Sets both ends of the animation to one value (it then holds still).
void Animator::Initialize(float value) {
	startValue = endValue = value;
	running = false;
}

// Begins moving from wherever the value is now to finalValue over duration game hours.
void Animator::Start(float duration, float finalValue) {
	if (!time || !time->GameDaysPassed) {
		Initialize(finalValue);
		return;
	}
	const float now = GameHoursNow(time);
	const float current = GetValue();	// read by the OLD times before overwriting them

	if (!(duration > 0.0f)) {
		startTime = endTime = now;
		startValue = endValue = finalValue;
		running = false;
		return;
	}

	startTime = now;
	endTime = now + duration;
	startValue = current;
	endValue = finalValue;
	running = true;
}

// The animated value at the current game time.
float Animator::GetValue() {
	if (!running || !time || !time->GameDaysPassed) return startValue;

	const float now = GameHoursNow(time);
	// Clock rewound (e.g. an earlier save loaded), animation finished, or degenerate span: snap to endValue instead of extrapolating.
	if (now < startTime || now >= endTime || !(endTime > startTime)) {
		running = false;
		startValue = endValue;
		return endValue;
	}

	const float t = (std::clamp)(invLerp(startTime, endTime, now), 0.0f, 1.0f);
	return std::lerp(startValue, endValue, t);
}
