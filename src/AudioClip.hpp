#include "dep/dr_wav/dr_wav.h"
#include "dsp/Interpolation.hpp"
#define WAVEFORM_RESOLUTION 64

struct AudioClip
{
    AudioClip() {};

    unsigned int getSampleCount() {
        return left_channel_.size();
    }

    unsigned int getChannelCount() {
        return channels_;
    }

    unsigned int getSampleRate() {
        return sampleRate_;
    }

    bool isLoaded() {
        return left_channel_.size() > 0;
    }

    float getSeconds() {
        return (float)left_channel_.size() / (float)sampleRate_;
    }

    float getDt() {
        return dt_;
    }

    inline float getSample(double index) {
        int x1 = floorf(index);
        return left_channel_[x1];
    }

    inline float getSampleLinear(double index) {
        int x1 = floorf(index);
        int x2 = (x1 + 1) % left_channel_.size();
        float t = index - x1;
        return crossfade(left_channel_[x1], left_channel_[x2], t);
    }

    inline float getSampleHermite(double index) {
        int x1 = floorf(index);
        int x0 = clamp(x1 - 1, 0, x1);
        int x2 = (x1 + 1) % left_channel_.size();
        int x3 = (x2 + 1) % left_channel_.size();
        float t = index - x1;
        return Hermite4pt3oX(left_channel_[x0], left_channel_[x1], left_channel_[x2], left_channel_[x3], t);
    }

    inline float getSampleSpline(double index) {
        int x1 = floorf(index);
        int x0 = clamp(x1 - 1, 0, x1);
        int x2 = (x1 + 1) % left_channel_.size();
        int x3 = (x2 + 1) % left_channel_.size();
        float t = index - x1;
        return BSpline(left_channel_[x0], left_channel_[x1], left_channel_[x2], left_channel_[x3], t);
    }

    inline float getSample(double index, Interpolations interpolation_mode, bool reverse) {
        switch (interpolation_mode) {
        case NONE:
            return getSample(index);
            break;
        case LINEAR:
            return getSampleLinear(index);
            break;
        case HERMITE:
            return getSampleHermite(index);
            break;
        case BSPLINE:
            return getSampleSpline(index);
            break;
        default:
            return getSample(index);
        }
    }

    void calculateWaveform() {
        dt_ = 1.0f / left_channel_.size();
        int pos = 0;
        int samplesPerSlice = floorf(left_channel_.size() / WAVEFORM_RESOLUTION);
        float max = 0;
        for (int i = 0; i < WAVEFORM_RESOLUTION; i++) {
            float acumulator = 0;
            for (int s = 0; s < samplesPerSlice; s++) {
                acumulator += abs(left_channel_[pos]);
                pos++;
            }
            waveform_[i] = acumulator / samplesPerSlice;
            max = waveform_[i] > max ? waveform_[i] : max;
        }
        // Rescale amplitude.
        for (int i = 0; i < WAVEFORM_RESOLUTION; i++) {
            waveform_[i] = rescale(waveform_[i], -max, max, -0.8f, 0.8f);
        }
    }

    void startRec(unsigned int sampleRate)
    {
        sampleRate_ = sampleRate;
        left_channel_.clear();
        counter_ = 0;
        acumulator_ = 0;
        waveform_index_ = 0;
        for (size_t i = 0; i < WAVEFORM_RESOLUTION; i++)
            waveform_[i] = 0;
    }

    // Returns true while recording. Until max recording time.
    bool rec(float sample) {
        // Save data
        left_channel_.push_back(clamp(sample, -1.0f, 1.0f));

        // Online waveform
        if (left_channel_.size() > WAVEFORM_RESOLUTION) {
            counter_++;
            acumulator_ += abs(sample);
            int samplesPerSlice = floorf(maxRecordSamples / WAVEFORM_RESOLUTION);
            if (counter_ > samplesPerSlice)	{
                waveform_[waveform_index_] = acumulator_ / samplesPerSlice;
                counter_ = 0;
                acumulator_ = 0;
                waveform_index_++;
            }
        }

        // Stop recording
        if (left_channel_.size() >= maxRecordSamples) {
            calculateWaveform();
            return false;
        }

        return true;
    }

    void load(const std::string &path) {
        drwav_uint64 totalSampleCount = 0;

        float *pSampleData = drwav_open_and_read_file_f32(path.c_str(), &channels_, &sampleRate_, &totalSampleCount);

        if (pSampleData == NULL)
            return;

        left_channel_.clear();

        for (size_t i = 0; i < totalSampleCount; i += channels_)
            left_channel_.push_back(pSampleData[i]);

        drwav_free(pSampleData);

        calculateWaveform();
    }

    void saveToDisk(std::string path) {
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

    void trim(double start, double end) {
        int samples_before = getSampleCount();
        int samples_to_copy = (end - start) * samples_before;

        int remove_l = start * samples_before;

        std::vector<float> left_channel_copy;

        for (int i = remove_l; i < samples_to_copy; i++)
            left_channel_copy.push_back(left_channel_[i]);

        left_channel_.clear();

        for (int i = 0; i < samples_to_copy; i++)
            left_channel_.push_back(left_channel_copy[i]);

        calculateWaveform();
    }

    const unsigned int maxRecordSamples = 44100 * 10;
    float waveform_[WAVEFORM_RESOLUTION] = {0, 0, 0, 0};

private:

    std::vector<float> left_channel_;
    unsigned int channels_ = 0;
    unsigned int sampleRate_ = 0;
    float dt_ = 1.0f / maxRecordSamples;

    // Used for waveform while recording
    int waveform_index_ = 0;
    int counter_ = 0;
    float acumulator_ = 0;
};
