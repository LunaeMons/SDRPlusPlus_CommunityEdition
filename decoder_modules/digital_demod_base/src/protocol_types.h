#pragma once
#include <cstdint>
#include <string>

// Windows header protection
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

// Cross-platform packed struct support
#ifdef _WIN32
    #pragma pack(push, 1)
    #define PACKED
#else
    #define PACKED __attribute__((packed))
#endif

namespace digital_demod {

    // Protocol enumeration for digital demodulators
    enum class ProtocolType : uint16_t {
        P25_FSK4 = 1,
        P25_CQPSK_4800 = 2,
        P25_CQPSK_6000 = 3,
        P25_H_DQPSK = 4,
        P25_H_CPM = 5,
        DMR_FSK4 = 6,
        M17_FSK4 = 7,
        YSF_FSK4 = 8,
        NXDN_4800 = 9,
        NXDN_9600 = 10,
        DSTAR_FSK2 = 11,
        EDACS_FSK2 = 12,
        PROVOICE_FSK2 = 13
    };

    // Protocol configuration structure
    struct ProtocolConfig {
        ProtocolType type;
        std::string name;
        double symbolRate;          // Symbols per second
        uint8_t bitsPerSymbol;      // 1 for FSK2, 2 for FSK4
        double deviation;           // FM deviation in Hz (for FSK)
        double rrcBeta;             // RRC filter roll-off factor
        int rrcTaps;                // RRC filter tap count
        double bandwidth;           // Signal bandwidth in Hz
        bool isDifferential;        // Requires differential decoding
    };

    // Network stream header for digital data
    struct DigitalStreamHeader {
        uint32_t magic = 0x44494749;     // "DIGI"
        uint16_t protocol_id;            // ProtocolType value
        uint16_t symbol_rate;            // Symbols per second
        uint8_t bits_per_symbol;         // 1 for FSK2, 2 for FSK4
        uint8_t reserved[3];             // Future expansion
        uint64_t timestamp;              // Unix timestamp in microseconds
    } PACKED;

    // File format header for recorded digital streams
    struct DigitalFileHeader {
        uint32_t magic = 0x44494749;     // "DIGI"
        uint32_t version = 1;            // File format version
        uint16_t protocol_id;            // ProtocolType value
        uint16_t symbol_rate;            // Symbols per second
        uint8_t bits_per_symbol;         // 1 for FSK2, 2 for FSK4
        uint8_t reserved[7];             // Future expansion
        uint64_t start_timestamp;        // Recording start time
        uint64_t sample_count;           // Total samples in file
        char description[64];            // Human-readable description
    } PACKED;

    // Protocol configuration lookup table
    static const ProtocolConfig PROTOCOL_CONFIGS[] = {
        {ProtocolType::P25_FSK4, "P25 FSK4", 4800, 2, 1800, 0.2, 31, 9600, false},
        {ProtocolType::P25_CQPSK_4800, "P25 CQPSK 4800", 4800, 2, 0, 0.2, 31, 9600, true},
        {ProtocolType::P25_CQPSK_6000, "P25 CQPSK 6000", 6000, 2, 0, 0.2, 31, 12000, true},
        {ProtocolType::P25_H_DQPSK, "P25 H-DQPSK", 4800, 2, 0, 0.2, 31, 9600, true},
        {ProtocolType::P25_H_CPM, "P25 H-CPM", 4800, 2, 1800, 0.2, 31, 9600, false},
        {ProtocolType::DMR_FSK4, "DMR FSK4", 4800, 2, 1944, 0.2, 31, 9600, false},
        {ProtocolType::M17_FSK4, "M17 FSK4", 4800, 2, 2400, 0.5, 31, 9600, false},
        {ProtocolType::YSF_FSK4, "YSF Fusion FSK4", 4800, 2, 1800, 0.2, 31, 9600, false},
        {ProtocolType::NXDN_4800, "NXDN 4800", 2400, 2, 1200, 0.2, 31, 4800, false},
        {ProtocolType::NXDN_9600, "NXDN 9600", 4800, 2, 2400, 0.2, 31, 9600, false},
        {ProtocolType::DSTAR_FSK2, "D-STAR GMSK", 4800, 1, 1200, 0.5, 31, 4800, false},
        {ProtocolType::EDACS_FSK2, "EDACS FSK2", 9600, 1, 4800, 0.2, 31, 19200, false},
        {ProtocolType::PROVOICE_FSK2, "ProVoice FSK2", 9600, 1, 4800, 0.2, 31, 19200, false}
    };

    // Helper function to get protocol configuration
    inline const ProtocolConfig* getProtocolConfig(ProtocolType type) {
        for (const auto& config : PROTOCOL_CONFIGS) {
            if (config.type == type) {
                return &config;
            }
        }
        return nullptr;
    }

    // Helper function to get protocol name
    inline std::string getProtocolName(ProtocolType type) {
        const auto* config = getProtocolConfig(type);
        return config ? config->name : "Unknown";
    }
}

// Restore packing settings
#ifdef _WIN32
    #pragma pack(pop)
#endif
