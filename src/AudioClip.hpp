#include "dep/dr_wav/dr_wav.h"
#include "dsp/Interpolation.hpp"
#define WAVEFORM_RESOLUTION 64

struct AudioClip
{
    AudioClip() {};

    unsigned int getSampleCount() { return left_channel_.size(); }

    unsigned int getChannelCount() { return channels_; }

    unsigned int getSampleRate() { return sampleRate_; }

    bool isLoaded() { return left_channel_.size() > 0; }

    float getSeconds() { return (float)left_channel_.size() / (float)sampleRate_; }

    float getSampleTime() { return 1.0f / left_channel_.size(); }

    float* data() { return left_channel_.data(); }
    
    float* waveform() { return waveform_; }

    inline float getSamplePhase(double phase, Interpolations interpolation_mode) {
        double index = phase * getSampleCount();
        return getSampleIndex(index, interpolation_mode);
    }

    inline float getSampleIndex(double index, Interpolations interpolation_mode) {
        switch (interpolation_mode) {
        case NONE:
            return left_channel_[floor(index)];
            break;
        case LINEAR:
            return interpolateLinearD(left_channel_.data(), index);
            break;
        case HERMITE:
            return InterpolateHermite(left_channel_.data(), index, left_channel_.size());
            break;
        case BSPLINE:
            return interpolateBSpline(left_channel_.data(), index);
            break;
        default:
            return left_channel_[floor(index)];
        }
    }

    void calculateWaveform() {
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

private:

    std::vector<float> left_channel_;
    unsigned int channels_ = 0;
    unsigned int sampleRate_ = 0;

    const unsigned int maxRecordSamples = 44100 * 10;
    float waveform_[WAVEFORM_RESOLUTION] = {0, 0, 0, 0};

    // Used for waveform while recording
    int waveform_index_ = 0;
    int counter_ = 0;
    float acumulator_ = 0;
};
