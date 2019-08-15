#include "dep/dr_wav/dr_wav.h"
#include "Interpolation.hpp"
#define WAVEFORM_RESOLUTION 64

struct AudioClip
{
	AudioClip() {}

	unsigned int getSampleCount() { return sampleCount_; }

	unsigned int getChannelCount() { return channels_; }

	unsigned int getSampleRate() { return sampleRate_; }

	bool isLoaded() { return sampleCount_ > 0; }

	float getSeconds() { return (float)sampleCount_ / (float)sampleRate_; }

	float getSample(float index, Interpolations interpolation_mode)
	{
		index = clamp(index, 1.0f, (float)(sampleCount_ - 2));

		int x1 = floorf(index);
		int x0 = x1 - 1;
		int x2 = x1 + 1;
		int x3 = x2 + 1;
		float t = index - x1;

		switch (interpolation_mode)
		{
		case NONE:
			return left_channel_[x1];
			break;
		case LINEAR:
			return crossfade(x1, x2, t);
			break;
		case CUBIC:
			Cubic(left_channel_[x0], left_channel_[x1], left_channel_[x2], left_channel_[x3], t);
			break;
		case HERMITE:
			Hermite4pt3oX(left_channel_[x0], left_channel_[x1], left_channel_[x2], left_channel_[x3], t);
			break;
		case BSPLINE:
			BSpline(left_channel_[x0], left_channel_[x1], left_channel_[x2], left_channel_[x3], t);
			break;
		default:
			return left_channel_[x1];
		}

		return left_channel_[x1];
	}

	void unLoad()
	{
		left_channel_.clear();
		left_channel_.push_back(0);
		sampleCount_ = 0;
	}

	void calculateWaveform()
	{
		int pos = 0;
		int samplesPerSlice = floorf(sampleCount_ / WAVEFORM_RESOLUTION);
		for (int i = 0; i < WAVEFORM_RESOLUTION; i++)
		{
			float acumulator = 0;
			for (int s = 0; s < samplesPerSlice; s++)
			{
				acumulator += abs(left_channel_[pos]);
				pos++;
			}
			waveform_[i] = acumulator / samplesPerSlice;
		}
	}

	void startRec(unsigned int sampleRate)
	{
		sampleRate_ = sampleRate;
		left_channel_.clear();
		sampleCount_ = 0;
	}

	// Returns true until max recording time.
	bool rec(float sample)
	{
		left_channel_.push_back(clamp(sample, -1.0f, 1.0f));
		sampleCount_++;
		if (sampleCount_ >= maxRecordSamples)
		{
			stopRec();
			return false;
		}
		return true;
	}

	void stopRec()
	{
		// We didnt add on startRec. Is fine like this.

		// This so we don't have to clamp on interolation samplepos + 2
		left_channel_.push_back(0);
		left_channel_.push_back(0);

		calculateWaveform();
	}

	int findCrosZero(int first_sample)
	{
		for (unsigned int i = first_sample; i < sampleCount_; i++)
		{
			if (left_channel_[i] == 0.0f)
				return i;
		}
		return first_sample;
	}

	void load(const std::string &path)
	{
		drwav_uint64 totalSampleCount = 0;

		float *pSampleData = drwav_open_and_read_file_f32(path.c_str(), &channels_, &sampleRate_, &totalSampleCount);

		if (pSampleData == NULL)
			return;

		left_channel_.clear();


		for (size_t i = 0; i < totalSampleCount; i += channels_)
			left_channel_.push_back(pSampleData[i]);

		sampleCount_ = left_channel_.size();


		drwav_free(pSampleData);

		calculateWaveform();
	}

	void saveToDisk(std::string path)
	{
		int samples = left_channel_.size();

		float data[samples];

		for (int i = 0; i < samples; i++)
			data[i] = left_channel_[i];

		drwav_data_format format;

		format.container = drwav_container_riff;   // <-- drwav_container_riff = normal WAV files, drwav_container_w64 = Sony Wave64.
		format.format = DR_WAVE_FORMAT_IEEE_FLOAT; // <-- Any of the DR_WAVE_FORMAT_* codes.
		format.channels = 1;
		format.sampleRate = sampleRate_;
		format.bitsPerSample = 32;
		drwav *pWav = drwav_open_file_write(path.c_str(), &format);

		drwav_write(pWav, samples, data);

		drwav_close(pWav);
	}

	const unsigned int maxRecordSamples = 44100 * 10;
	float waveform_[WAVEFORM_RESOLUTION] = {0, 0, 0, 0};

private:
	std::vector<float> left_channel_;
	unsigned int sampleCount_ = 0;
	unsigned int channels_ = 0;
	unsigned int sampleRate_ = 0;
};
