struct AntipopFilter {
    
    float alpha_ = 0.00001f;
    float filter_;

    void trigger() {
        alpha_ = 0.0f;
    }

    float process(float in, const Module::ProcessArgs &args) {
        if (alpha_ >= 1.0f) {
            filter_ = in;
            return in;
        }

        alpha_ += args.sampleTime * 1500; //  (args.sampleRate / 32);
       
        filter_ += alpha_ * (in - filter_);

        return filter_;
    }
};
