#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <signal_path/sink.h>
#include <dsp/buffer/packer.h>
#include <dsp/convert/stereo_to_mono.h>
#include <utils/flog.h>
#include <rtaudio/RtAudio.h>
#include <rnnoise.h>
#include <config.h>
#include <core.h>

SDRPP_MOD_INFO{
    /* Name:            */ "rnnoise_audio_sink",
    /* Description:     */ "Audio sink with RNNoise noise reduction for SDR++",
    /* Author:          */ "Jack Heinlein", 
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

class RNNoiseAudioSink : public SinkManager::Sink {
public:
    RNNoiseAudioSink(SinkManager::Stream* stream, std::string streamName) {
        _stream = stream;
        _streamName = streamName;
        s2m.init(_stream->sinkOut);
        monoPacker.init(&s2m.out, 512);
        stereoPacker.init(_stream->sinkOut, 512);

        rnnFrameSize = rnnoise_get_frame_size();
        processingBufferL.resize(rnnFrameSize);
        processingBufferR.resize(rnnFrameSize);

#if RTAUDIO_VERSION_MAJOR >= 6
        audio.setErrorCallback(&errorCallback);
#endif
        config.acquire();
        if (!config.conf.contains(_streamName)) {
            config.conf[_streamName]["device"] = "";
            config.conf[_streamName]["devices"] = json({});
            config.conf[_streamName]["rnnoise_enabled"] = false;
            config.conf[_streamName]["use_vad_gating"] = false;
            config.conf[_streamName]["vad_threshold"] = 0.6f;
            config.conf[_streamName]["vad_grace_period"] = 20;
            config.conf[_streamName]["output_gain"] = 1.2f;
        }

        device = config.conf[_streamName]["device"];
        rnNoiseEnabled = config.conf[_streamName]["rnnoise_enabled"];
        useVADGating = config.conf[_streamName]["use_vad_gating"];
        vadThreshold = config.conf[_streamName]["vad_threshold"];
        vadGracePeriod = config.conf[_streamName]["vad_grace_period"];
        outputGain = config.conf[_streamName]["output_gain"];
        config.release();

        // Initialize audio devices
        RtAudio::DeviceInfo info;
#if RTAUDIO_VERSION_MAJOR >= 6
        for (int i : audio.getDeviceIds()) {
#else
        int count = audio.getDeviceCount();
        for (int i = 0; i < count; i++) {
#endif
            try {
                info = audio.getDeviceInfo(i);
#if !defined(RTAUDIO_VERSION_MAJOR) || RTAUDIO_VERSION_MAJOR < 6
                if (!info.probed) { continue; }
#endif
                if (info.outputChannels == 0) { continue; }
                if (info.isDefaultOutput) { defaultDevId = devList.size(); }
                devList.push_back(info);
                deviceIds.push_back(i);
                txtDevList += info.name;
                txtDevList += '\0';
            }
            catch (const std::exception& e) {
                flog::error("RNNoiseAudioSink Error getting audio device ({}) info: {}", i, e.what());
            }
        }
        
        selectByName(device);
    }

    ~RNNoiseAudioSink() {
        stop();
        if (rnNoiseStateL) rnnoise_destroy(rnNoiseStateL);
        if (rnNoiseStateR) rnnoise_destroy(rnNoiseStateR);
    }

    void start() {
        if (running) return;
        running = doStart();
    }

    void stop() {
        if (!running) return;
        doStop();
        running = false;
    }

    void menuHandler() {
        float menuWidth = ImGui::GetContentRegionAvail().x;

        ImGui::Text("Noise Suppressor for Voice");
        ImGui::Separator();

        ImGui::SetNextItemWidth(menuWidth);
        if (ImGui::Combo(("Device##_rnnoise_dev_" + _streamName).c_str(), &devId, txtDevList.c_str())) {
            selectById(devId);
            config.acquire();
            config.conf[_streamName]["device"] = devList[devId].name;
            config.release(true);
        }

        ImGui::SetNextItemWidth(menuWidth);
        if (ImGui::Combo(("Sample Rate##_rnnoise_sr_" + _streamName).c_str(), &srId, sampleRatesTxt.c_str())) {
            sampleRate = sampleRates[srId];
            _stream->setSampleRate(sampleRate);
            if (running) {
                doStop();
                doStart();
            }
            config.acquire();
            config.conf[_streamName]["devices"][devList[devId].name] = sampleRate;
            config.release(true);
        }

        ImGui::Separator();

        // Enable noise reduction
        if (ImGui::Checkbox(("Enable Noise Reduction##_rnnoise_" + _streamName).c_str(), &rnNoiseEnabled)) {
            if (rnNoiseEnabled && !rnNoiseStateL) {
                createDenoiseState();
            }
            config.acquire();
            config.conf[_streamName]["rnnoise_enabled"] = rnNoiseEnabled;
            config.release(true);
        }

        if (rnNoiseEnabled) {
            ImGui::Spacing();
            
            // RNNoise Pure Mode vs VAD Gating
            if (ImGui::Checkbox(("Use VAD Gating##_vad_gating_" + _streamName).c_str(), &useVADGating)) {
                config.acquire();
                config.conf[_streamName]["use_vad_gating"] = useVADGating;
                config.release(true);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Enable additional VAD-based muting (may reduce background noise but can cut speech)");
            }

            if (useVADGating) {
                // VAD Threshold (0.0-1.0, default 0.6)
                ImGui::Text("VAD Threshold");
                ImGui::SetNextItemWidth(menuWidth * 0.6f);
                if (ImGui::SliderFloat(("##vad_threshold_" + _streamName).c_str(), &vadThreshold, 0.0f, 1.0f, "%.3f")) {
                    config.acquire();
                    config.conf[_streamName]["vad_threshold"] = vadThreshold;
                    config.release(true);
                }
                ImGui::SameLine();
                if (ImGui::Button(("-##vad_threshold_dec_" + _streamName).c_str())) {
                    vadThreshold = std::max(vadThreshold - 0.01f, 0.0f);
                    config.acquire();
                    config.conf[_streamName]["vad_threshold"] = vadThreshold;
                    config.release(true);
                }
                ImGui::SameLine();
                if (ImGui::Button(("+##vad_threshold_inc_" + _streamName).c_str())) {
                    vadThreshold = std::min(vadThreshold + 0.01f, 1.0f);
                    config.acquire();
                    config.conf[_streamName]["vad_threshold"] = vadThreshold;
                    config.release(true);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Voice activity detection threshold (Default: 0.6)");
                }

                ImGui::Text("VAD Grace Period (10ms per unit)");
                ImGui::SetNextItemWidth(menuWidth * 0.6f);
                if (ImGui::SliderInt(("##vad_grace_period_" + _streamName).c_str(), &vadGracePeriod, 0, 500)) {
                    config.acquire();
                    config.conf[_streamName]["vad_grace_period"] = vadGracePeriod;
                    config.release(true);
                }
                ImGui::SameLine();
                if (ImGui::Button(("-##vad_grace_dec_" + _streamName).c_str())) {
                    vadGracePeriod = std::max(vadGracePeriod - 1, 0);
                    config.acquire();
                    config.conf[_streamName]["vad_grace_period"] = vadGracePeriod;
                    config.release(true);
                }
                ImGui::SameLine();
                if (ImGui::Button(("+##vad_grace_inc_" + _streamName).c_str())) {
                    vadGracePeriod = std::min(vadGracePeriod + 1, 500);
                    config.acquire();
                    config.conf[_streamName]["vad_grace_period"] = vadGracePeriod;
                    config.release(true);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Blocks to keep unmuted after voice detection (Default: 20)");
                }
            } else {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Pure RNNoise Processing");
                ImGui::Text("Using only RNNoise noise reduction");
                ImGui::Text("without additional gating");
            }
            
            // GAIN
            ImGui::Separator();
            ImGui::Text("Output Gain");
            ImGui::SetNextItemWidth(menuWidth * 0.6f);
            if (ImGui::SliderFloat(("##output_gain_" + _streamName).c_str(), &outputGain, 0.5f, 5.0f, "%.1fx")) {
                config.acquire();
                config.conf[_streamName]["output_gain"] = outputGain;
                config.release(true);
            }
            ImGui::SameLine();
            if (ImGui::Button(("Reset##output_gain_reset_" + _streamName).c_str())) {
                outputGain = 1.2f;
                config.acquire();
                config.conf[_streamName]["output_gain"] = outputGain;
                config.release(true);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Adjust output volume after noise reduction (Default: 1.2x)");
            }
        }
    }

private:
    void createDenoiseState() {
        if (rnNoiseStateL) {
            rnnoise_destroy(rnNoiseStateL);
            rnNoiseStateL = nullptr;
        }
        if (rnNoiseStateR) {
            rnnoise_destroy(rnNoiseStateR);
            rnNoiseStateR = nullptr;
        }

        rnNoiseStateL = rnnoise_create(nullptr);
        rnNoiseStateR = rnnoise_create(nullptr);
        
        if (!rnNoiseStateL || !rnNoiseStateR) {
            flog::error("Failed to create RNNoise states");
        }
    }

    void processAudio(float* buffer, int frameCount) {
        if (!rnNoiseEnabled || !rnNoiseStateL || !rnNoiseStateR) return;

        for (int offset = 0; offset < frameCount; offset += rnnFrameSize) {
            int remainingFrames = std::min(rnnFrameSize, frameCount - offset);

            for (int i = 0; i < remainingFrames; i++) {
                processingBufferL[i] = buffer[(offset + i) * 2] * 32768.0f;
                processingBufferR[i] = buffer[(offset + i) * 2 + 1] * 32768.0f;
            }

            // Process with RNNoise
            float vadProbL = rnnoise_process_frame(rnNoiseStateL, processingBufferL.data(), processingBufferL.data());
            float vadProbR = rnnoise_process_frame(rnNoiseStateR, processingBufferR.data(), processingBufferR.data());
            
            for (int i = 0; i < remainingFrames; i++) {
                processingBufferL[i] /= 32768.0f;
                processingBufferR[i] /= 32768.0f;
            }
            
            // Optional VAD-based gating
            if (useVADGating) {
                float maxVadProb = std::max(vadProbL, vadProbR);
                static int blocksAfterVAD = 0;
                bool isVoiceActive = maxVadProb >= vadThreshold;
                
                if (isVoiceActive) {
                    blocksAfterVAD = 0;
                } else {
                    blocksAfterVAD++;
                }

                bool shouldMute = !isVoiceActive && blocksAfterVAD >= vadGracePeriod;
                
                if (shouldMute) {
                    for (int i = 0; i < remainingFrames; i++) {
                        processingBufferL[i] = 0.0f;
                        processingBufferR[i] = 0.0f;
                    }
                }
            }
            
            // Apply output gain
            for (int i = 0; i < remainingFrames; i++) {
                processingBufferL[i] *= outputGain;
                processingBufferR[i] *= outputGain;
            }

            for (int i = 0; i < remainingFrames; i++) {
                buffer[(offset + i) * 2] = processingBufferL[i];
                buffer[(offset + i) * 2 + 1] = processingBufferR[i];
            }
        }
    }

    bool doStart() {
        RtAudio::StreamParameters parameters;
        parameters.deviceId = deviceIds[devId];
        parameters.nChannels = 2;
        unsigned int bufferFrames = sampleRate / 60;
        RtAudio::StreamOptions opts;
        opts.flags = RTAUDIO_MINIMIZE_LATENCY;
        opts.streamName = _streamName;

        try {
            audio.openStream(&parameters, NULL, RTAUDIO_FLOAT32, sampleRate, &bufferFrames, &callback, this, &opts);
            stereoPacker.setSampleCount(bufferFrames);
            audio.startStream();
            stereoPacker.start();
        }
        catch (const std::exception& e) {
            flog::error("Could not open audio device: {0}", e.what());
            return false;
        }
        return true;
    }

    void doStop() {
        s2m.stop();
        monoPacker.stop();
        stereoPacker.stop();
        monoPacker.out.stopReader();
        stereoPacker.out.stopReader();
        audio.stopStream();
        audio.closeStream();
        monoPacker.out.clearReadStop();
        stereoPacker.out.clearReadStop();
    }

    void selectFirst() {
        selectById(defaultDevId);
    }

    void selectByName(std::string name) {
        for (int i = 0; i < devList.size(); i++) {
            if (devList[i].name == name) {
                selectById(i);
                return;
            }
        }
        selectFirst();
    }

    void selectById(int id) {
        devId = id;
        bool created = false;
        config.acquire();
        if (!config.conf[_streamName]["devices"].contains(devList[id].name)) {
            created = true;
            config.conf[_streamName]["devices"][devList[id].name] = devList[id].preferredSampleRate;
        }
        sampleRate = config.conf[_streamName]["devices"][devList[id].name];
        config.release(created);

        sampleRates = devList[id].sampleRates;
        sampleRatesTxt = "";
        char buf[256];
        bool found = false;
        unsigned int defaultId = 0;
        unsigned int defaultSr = devList[id].preferredSampleRate;
        for (int i = 0; i < sampleRates.size(); i++) {
            if (sampleRates[i] == sampleRate) {
                found = true;
                srId = i;
            }
            if (sampleRates[i] == defaultSr) {
                defaultId = i;
            }
            sprintf(buf, "%d", sampleRates[i]);
            sampleRatesTxt += buf;
            sampleRatesTxt += '\0';
        }
        if (!found) {
            sampleRate = defaultSr;
            srId = defaultId;
        }

        _stream->setSampleRate(sampleRate);

        if (running) {
            doStop();
            doStart();
        }
    }

    static int callback(void* outputBuffer, void* inputBuffer, unsigned int nBufferFrames, 
                       double streamTime, RtAudioStreamStatus status, void* userData) {
        RNNoiseAudioSink* _this = (RNNoiseAudioSink*)userData;
        int count = _this->stereoPacker.out.read();
        if (count < 0) return 0;

        if (_this->rnNoiseEnabled) {
            _this->processAudio((float*)_this->stereoPacker.out.readBuf, nBufferFrames);
        }

        memcpy(outputBuffer, _this->stereoPacker.out.readBuf, nBufferFrames * sizeof(dsp::stereo_t));
        _this->stereoPacker.out.flush();
        return 0;
    }

#if RTAUDIO_VERSION_MAJOR >= 6
    static void errorCallback(RtAudioErrorType type, const std::string& errorText) {
        switch (type) {
        case RtAudioErrorType::RTAUDIO_NO_ERROR:
            return;
        case RtAudioErrorType::RTAUDIO_WARNING:
        case RtAudioErrorType::RTAUDIO_NO_DEVICES_FOUND:
        case RtAudioErrorType::RTAUDIO_DEVICE_DISCONNECT:
            flog::warn("RNNoiseAudioSink Warning: {} ({})", errorText, (int)type);
            break;
        default:
            throw std::runtime_error(errorText);
        }
    }
#endif

    // Member variables
    SinkManager::Stream* _stream;
    std::string _streamName;
    dsp::convert::StereoToMono s2m;
    dsp::buffer::Packer<float> monoPacker;
    dsp::buffer::Packer<dsp::stereo_t> stereoPacker;

    // RNNoise processing
    DenoiseState* rnNoiseStateL = nullptr;
    DenoiseState* rnNoiseStateR = nullptr;
    bool rnNoiseEnabled = false;
    int rnnFrameSize = 0;
    std::vector<float> processingBufferL;
    std::vector<float> processingBufferR;

    // VAD parameters
    bool useVADGating = false;
    float vadThreshold = 0.6f;
    int vadGracePeriod = 20;
    
    // Audio processing parameters
    float outputGain = 1.2f;

    RtAudio audio;
    int srId = 0;
    int devId = 0;
    bool running = false;
    unsigned int defaultDevId = 0;
    std::vector<RtAudio::DeviceInfo> devList;
    std::vector<unsigned int> deviceIds;
    std::string txtDevList;
    std::vector<unsigned int> sampleRates;
    std::string sampleRatesTxt;
    unsigned int sampleRate = 48000;
    std::string device = "";
};

class RNNoiseAudioSinkModule : public ModuleManager::Instance {
public:
    RNNoiseAudioSinkModule(std::string name) {
        this->name = name;
        provider.create = create_sink;
        provider.ctx = this;
        
        flog::info("RNNoiseAudioSink: Registering sink provider 'RNNoise Audio'");
        sigpath::sinkManager.registerSinkProvider("RNNoise Audio", provider);
    }

    ~RNNoiseAudioSinkModule() {
        flog::info("RNNoiseAudioSink: Unregistering sink provider 'RNNoise Audio'");
        sigpath::sinkManager.unregisterSinkProvider("RNNoise Audio");
    }

    void postInit() {}
    void enable() { enabled = true; }
    void disable() { enabled = false; }
    bool isEnabled() { return enabled; }

private:
    static SinkManager::Sink* create_sink(SinkManager::Stream* stream, std::string streamName, void* ctx) {
        return (SinkManager::Sink*)(new RNNoiseAudioSink(stream, streamName));
    }

    std::string name;
    bool enabled = true;
    SinkManager::SinkProvider provider;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/rnnoise_sink_config.json");
    config.load(def);
    config.enableAutoSave();
    flog::info("RNNoiseAudioSink: Module initialized");
}

MOD_EXPORT void* _CREATE_INSTANCE_(std::string name) {
    flog::info("RNNoiseAudioSink: Creating instance '{}'", name);
    return new RNNoiseAudioSinkModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    flog::info("RNNoiseAudioSink: Deleting instance");
    delete (RNNoiseAudioSinkModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
    flog::info("RNNoiseAudioSink: Module terminated");
}