/**
 * @file asio.h
 * @brief Steinberg ASIO 2.3 Core Type, Constant, and Structure Definitions (x64 ABI)
 *
 * Binary-compatible with Steinberg ASIO 2.3 specification on 64-bit Windows (MSVC x64).
 */

#ifndef BCD2000_ASIO_H
#define BCD2000_ASIO_H

#include <cstdint>

#pragma pack(push, 4)

// Boolean values
typedef long ASIOBool;
enum {
    ASIOFalse = 0,
    ASIOTrue  = 1
};

// Sample rate (IEEE 754 64-bit double)
typedef double ASIOSampleRate;

// Error codes
typedef long ASIOError;
enum {
    ASE_OK               = 0,
    ASE_SUCCESS          = 0x3f4847a0,
    ASE_NotPresent       = -1000,
    ASE_HWMalfunction,
    ASE_InvalidParameter,
    ASE_InvalidMode,
    ASE_SPNotAdvancing,
    ASE_NoClock,
    ASE_NoMemory
};

// 64-bit integer split into two 32-bit halves (big-endian field order in struct: hi, lo)
typedef struct ASIOSamples {
    unsigned long hi;
    unsigned long lo;
} ASIOSamples;

typedef struct ASIOTimeStamp {
    unsigned long hi;
    unsigned long lo;
} ASIOTimeStamp;

// Sample format types
typedef long ASIOSampleType;
enum {
    ASIOSTInt16MSB   = 0,
    ASIOSTInt24MSB   = 1,
    ASIOSTInt32MSB   = 2,
    ASIOSTFloat32MSB = 3,
    ASIOSTFloat64MSB = 4,

    ASIOSTInt32MSB16 = 8,
    ASIOSTInt32MSB18 = 9,
    ASIOSTInt32MSB20 = 10,
    ASIOSTInt32MSB24 = 11,

    ASIOSTInt16LSB   = 16,
    ASIOSTInt24LSB   = 17,
    ASIOSTInt32LSB   = 18,
    ASIOSTFloat32LSB = 19,
    ASIOSTFloat64LSB = 20,

    ASIOSTInt32LSB16 = 24,
    ASIOSTInt32LSB18 = 25,
    ASIOSTInt32LSB20 = 26,
    ASIOSTInt32LSB24 = 27,

    ASIOSTDSDInt8LSB1 = 32,
    ASIOSTDSDInt8MSB1 = 33,
    ASIOSTDSDInt8NER8 = 40,

    ASIOSTLastEntry
};

// Clock source descriptor
typedef struct ASIOClockSource {
    long index;
    long associatedChannel;
    long associatedGroup;
    ASIOBool isCurrentSource;
    char name[32];
} ASIOClockSource;

// Channel properties descriptor
typedef struct ASIOChannelInfo {
    long channel;
    ASIOBool isInput;
    ASIOBool isActive;
    long channelGroup;
    ASIOSampleType type;
    char name[32];
} ASIOChannelInfo;

// Buffer allocation descriptor
typedef struct ASIOBufferInfo {
    ASIOBool isInput;
    long channelNum;
    void *buffers[2];
} ASIOBufferInfo;

// TimeCode structure
typedef struct ASIOTimeCode {
    double speed;
    ASIOSamples timeCodeSamples;
    unsigned long flags;
    char future[64];
} ASIOTimeCode;

typedef enum ASIOTimeCodeFlags {
    kTcValid      = 1,
    kTcRunning    = 1 << 1,
    kTcReverse    = 1 << 2,
    kTcOnspeed    = 1 << 3,
    kTcStill      = 1 << 4,
    kTcSpeedValid = 1 << 8
} ASIOTimeCodeFlags;

// TimeInfo structure
typedef struct AsioTimeInfo {
    double speed;
    ASIOTimeStamp systemTime;
    ASIOSamples samplePosition;
    ASIOSampleRate sampleRate;
    unsigned long flags;
    char reserved[12];
} AsioTimeInfo;

typedef enum AsioTimeInfoFlags {
    kSystemTimeValid     = 1,
    kSamplePositionValid = 1 << 1,
    kSampleRateValid     = 1 << 2,
    kSpeedValid          = 1 << 3,
    kSampleRateChanged   = 1 << 4,
    kClockSourceChanged  = 1 << 5
} AsioTimeInfoFlags;

// Combined ASIOTime structure passed to bufferSwitchTimeInfo
typedef struct ASIOTime {
    long reserved[4];
    struct AsioTimeInfo timeInfo;
    struct ASIOTimeCode timeCode;
} ASIOTime;

// Host callback function table
typedef struct ASIOCallbacks {
    void (*bufferSwitch) (long doubleBufferIndex, ASIOBool directProcess);
    void (*sampleRateDidChange) (ASIOSampleRate sRate);
    long (*asioMessage) (long selector, long value, void* message, double* opt);
    ASIOTime* (*bufferSwitchTimeInfo) (ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess);
} ASIOCallbacks;

// asioMessage selectors
enum {
    kAsioSelectorSupported    = 1,
    kAsioEngineVersion,
    kAsioResetRequest,
    kAsioBufferSizeChange,
    kAsioResyncRequest,
    kAsioLatenciesChanged,
    kAsioSupportsTimeInfo,
    kAsioSupportsTimeCode,
    kAsioMMCCommand,
    kAsioSupportsInputMonitor,
    kAsioSupportsInputGain,
    kAsioSupportsInputMeter,
    kAsioSupportsOutputGain,
    kAsioSupportsOutputMeter,
    kAsioOverload,

    kAsioNumMessageSelectors
};

#pragma pack(pop)

// Helper functions to pack/unpack 64-bit integers into ASIOSamples / ASIOTimeStamp
inline void Uint64ToAsioSamples(uint64_t val, ASIOSamples &out) {
    out.hi = static_cast<unsigned long>((val >> 32) & 0xFFFFFFFFULL);
    out.lo = static_cast<unsigned long>(val & 0xFFFFFFFFULL);
}

inline void Uint64ToAsioTimeStamp(uint64_t val, ASIOTimeStamp &out) {
    out.hi = static_cast<unsigned long>((val >> 32) & 0xFFFFFFFFULL);
    out.lo = static_cast<unsigned long>(val & 0xFFFFFFFFULL);
}

inline uint64_t AsioSamplesToUint64(const ASIOSamples &in) {
    return (static_cast<uint64_t>(in.hi) << 32) | static_cast<uint64_t>(in.lo);
}

inline uint64_t AsioTimeStampToUint64(const ASIOTimeStamp &in) {
    return (static_cast<uint64_t>(in.hi) << 32) | static_cast<uint64_t>(in.lo);
}

#endif // BCD2000_ASIO_H
