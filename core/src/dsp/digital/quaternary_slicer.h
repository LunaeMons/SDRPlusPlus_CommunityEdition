#pragma once
#include "../processor.h"
#include <cmath>

namespace dsp::digital {
    // 4-level slicer for FSK4 and QPSK signals
    // Converts float symbols to 2-bit values (0, 1, 2, 3)
    class QuaternarySlicer : public Processor<float, uint8_t> {
        using base_type = Processor<float, uint8_t>;
    public:
        QuaternarySlicer() {}

        QuaternarySlicer(stream<float>* in, float threshold1 = -0.5f, float threshold2 = 0.5f) {
            init(in, threshold1, threshold2);
        }

        void init(stream<float>* in, float threshold1 = -0.5f, float threshold2 = 0.5f) {
            _threshold1 = threshold1;
            _threshold2 = threshold2;
            base_type::init(in);
        }

        void setThresholds(float threshold1, float threshold2) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            _threshold1 = threshold1;
            _threshold2 = threshold2;
        }

        // Process function for 4-level FSK slicing
        static inline int process(int count, const float* in, uint8_t* out, float threshold1, float threshold2) {
            for (int i = 0; i < count; i++) {
                float sample = in[i];
                if (sample < threshold1) {
                    out[i] = 0;  // Most negative level
                } else if (sample < 0.0f) {
                    out[i] = 1;  // Slightly negative level
                } else if (sample < threshold2) {
                    out[i] = 2;  // Slightly positive level
                } else {
                    out[i] = 3;  // Most positive level
                }
            }
            return count;
        }

        int run() {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }

            process(count, base_type::_in->readBuf, base_type::out.writeBuf, _threshold1, _threshold2);

            base_type::_in->flush();
            if (!base_type::out.swap(count)) { return -1; }
            return count;
        }

    private:
        float _threshold1 = -0.5f;  // Threshold between levels 0 and 1
        float _threshold2 = 0.5f;   // Threshold between levels 2 and 3
    };

    // Complex constellation slicer for QPSK signals
    // Converts complex symbols to 2-bit values based on quadrant
    class QPSKSlicer : public Processor<complex_t, uint8_t> {
        using base_type = Processor<complex_t, uint8_t>;
    public:
        QPSKSlicer() {}

        QPSKSlicer(stream<complex_t>* in) { base_type::init(in); }

        // Process function for QPSK constellation slicing
        static inline int process(int count, const complex_t* in, uint8_t* out) {
            for (int i = 0; i < count; i++) {
                complex_t sample = in[i];
                
                // Determine quadrant based on I and Q signs
                uint8_t symbol = 0;
                if (sample.re >= 0.0f) symbol |= 0x01;  // I bit
                if (sample.im >= 0.0f) symbol |= 0x02;  // Q bit
                
                out[i] = symbol;
            }
            return count;
        }

        int run() {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }

            process(count, base_type::_in->readBuf, base_type::out.writeBuf);

            base_type::_in->flush();
            if (!base_type::out.swap(count)) { return -1; }
            return count;
        }
    };
}
