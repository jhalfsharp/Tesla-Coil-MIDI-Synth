#pragma once

#include <list>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <deque>
#include <memory>

#include <portaudiocpp/PortAudioCpp.hxx>

#include "Coil.h"
#include "Convolution.h"
#include "RingBuffer.h"

#define F_SAMP 48000
#define FRAMES_PER_BUFFER 128
#define CHANNELS 2

#define MIN_FIFO_SIZE (FRAMES_PER_BUFFER*CHANNELS*2)
#define MAX_FIFO_SIZE (FRAMES_PER_BUFFER*CHANNELS*4)

#define VOLUME 0.8f
#define STEREO_SEPARATION 0.4f

// Impulse response data loaded from ir.bin
// Data has been pre-FFTed, so length is actually 2*(IR_LENGTH/2+1) = 4098 floats in RE-IM-RE-IM format
#define IR_LENGTH 4096
extern const float ir[IR_LENGTH/2+1];

class AudioEngine {
public:
	AudioEngine(std::list<Coil> &coils);
	~AudioEngine();

	// Start/stop audio generator
	void startStream();
	void stopStream();

	// portaudio callback
	static int genAudio(const void *input, void *output, unsigned long frameCount, const PaStreamCallbackTimeInfo *timeInfo, PaStreamCallbackFlags statusFlags, void *userData);

protected:
	std::list<Coil> &coils;

	// Buffers before and after processing
	RingBuffer<float, MAX_FIFO_SIZE+1> inputBuffer;
	RingBuffer<float, MAX_FIFO_SIZE+1> outputBuffer;

	// Unprocessed audio generation state
	unsigned long sample; // Index of last sample generated
	unsigned long lastUpdateSample; // Index of last sample when synth was updated
	std::chrono::steady_clock::time_point wakeTime; // Time when generator thread should wake up
	constexpr static std::chrono::duration wakePeriod = std::chrono::microseconds(500);
	std::atomic<bool> runGenerator;
	std::unique_ptr<std::thread> generator;

	// Convolution objects to apply IR filter to each channel
	std::deque<Convolution> conv;

	// Thread function
	void generatorThread();

	// Generate a single unprocessed sample
	void genInput();

	// Apply IR filter
	void genOutput();
};
