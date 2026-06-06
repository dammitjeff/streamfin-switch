#include "source.hpp"

#include "sdmc/sdmc.hpp"
#include "../jelly_net.hpp"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>

// ----- byte backends behind Source -----
namespace {

    class FileBackend final : public IoBackend {
        FsFile m_file;
      public:
        FileBackend(FsFile &&file) : m_file(file) {}
        ~FileBackend() override { fsFileClose(&m_file); }
        u64 read(s64 offset, void *buf, u64 size) override {
            u64 br = 0;
            if (R_FAILED(fsFileRead(&m_file, offset, buf, size, 0, &br))) return 0;
            return br;
        }
        s64 size() override {
            s64 sz = 0;
            if (R_FAILED(fsFileGetSize(&m_file, &sz))) return 0;
            return sz;
        }
    };

    // Persistent streaming HTTP backend: one connection, sequential reads, reopen
    // only on seek. Fixes the per-chunk-reconnect audio gaps.
    class HttpBackend final : public IoBackend {
        jelly::Stream m_stream;
      public:
        HttpBackend(const char *path) : m_stream(path) {}
        u64 read(s64 offset, void *buf, u64 size) override {
            long n = m_stream.read(offset, buf, (long)size);
            return (n > 0) ? (u64)n : 0;
        }
        s64 size() override {
            return m_stream.total();
        }
    };

}

std::unique_ptr<IoBackend> MakeFileBackend(FsFile &&file) { return std::make_unique<FileBackend>(std::move(file)); }
std::unique_ptr<IoBackend> MakeHttpBackend(const char *path) { return std::make_unique<HttpBackend>(path); }

// NOTE: when updating dr_libs, check for TUNE-FIX comment for patches.
#ifdef WANT_FLAC
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_STDIO
#include "dr_flac.h"
#endif

#ifdef WANT_MP3
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#define DRMP3_DATA_CHUNK_SIZE DRMP3_MIN_DATA_CHUNK_SIZE
#include "dr_mp3.h"
#endif

#ifdef WANT_WAV
#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#include "dr_wav.h"
#endif

namespace {

    enum SeekOrigin {
        SeekOrigin_SET,
        SeekOrigin_CUR,
        SeekOrigin_END
    };

    size_t ReadCallback(void *pUserData, void *pBufferOut, size_t bytesToRead) {
        auto data = static_cast<Source *>(pUserData);

        return data->ReadFile(pBufferOut, bytesToRead);
    }

#ifdef WANT_FLAC
    drflac_bool32 FlacSeekCallback(void *pUserData, int offset, drflac_seek_origin origin) {
        auto data = static_cast<Source *>(pUserData);

        return data->SeekFile(offset, origin);
    }

    drflac_bool32 FlacTellCallback(void *pUserData, drflac_int64* pCursor) {
        auto data = static_cast<Source *>(pUserData);

        *pCursor = data->TellFile();
        return true;
    }
#endif

#ifdef WANT_MP3
    drmp3_bool32 Mp3SeekCallback(void *pUserData, int offset, drmp3_seek_origin origin) {
        auto data = static_cast<Source *>(pUserData);

        return data->SeekFile(offset, origin);
    }

    drmp3_bool32 Mp3TellCallback(void *pUserData, drmp3_int64* pCursor) {
        auto data = static_cast<Source *>(pUserData);

        *pCursor = data->TellFile();
        return true;
    }
#endif

#ifdef WANT_WAV
    drwav_bool32 WavSeekCallback(void *pUserData, int offset, drwav_seek_origin origin) {
        auto data = static_cast<Source *>(pUserData);

        return data->SeekFile(offset, origin);
    }

    drwav_bool32 WavTellCallback(void *pUserData, drwav_int64* pCursor) {
        auto data = static_cast<Source *>(pUserData);

        *pCursor = data->TellFile();
        return true;
    }
#endif

#ifdef DEBUG
    void *log_malloc(size_t sz, void *) {
        std::printf("malloc: 0x%lX\n", sz);
        return malloc(sz);
    }
    void *log_realloc(void *p, size_t sz, void *) {
        std::printf("realloc: %p, 0x%lX\n", p, sz);
        return realloc(p, sz);
    }
    void log_free(void *p, void *) {
        std::printf("free: %p\n", p);
        free(p);
    }
    constexpr const drflac_allocation_callbacks flac_alloc = {
        .onMalloc  = log_malloc,
        .onRealloc = log_realloc,
        .onFree    = log_free,
    };
    constexpr const drmp3_allocation_callbacks mp3_alloc = {
        .onMalloc  = log_malloc,
        .onRealloc = log_realloc,
        .onFree    = log_free,
    };
    constexpr const drwav_allocation_callbacks wav_alloc = {
        .onMalloc  = log_malloc,
        .onRealloc = log_realloc,
        .onFree    = log_free,
    };
    constexpr const drflac_allocation_callbacks *flac_alloc_ptr = &flac_alloc;
    constexpr const drmp3_allocation_callbacks *mp3_alloc_ptr   = &mp3_alloc;
    constexpr const drwav_allocation_callbacks *wav_alloc_ptr   = &wav_alloc;
#else
#ifdef WANT_FLAC
    constexpr const drflac_allocation_callbacks *flac_alloc_ptr = nullptr;
#endif
#ifdef WANT_MP3
    constexpr const drmp3_allocation_callbacks *mp3_alloc_ptr   = nullptr;
#endif
#ifdef WANT_WAV
    constexpr const drwav_allocation_callbacks *wav_alloc_ptr   = nullptr;
#endif
#endif

}

Source::Source(std::unique_ptr<IoBackend> backend) : m_backend(std::move(backend)), m_offset(0), m_size(0) {
    m_buffered.off = m_buffered.size = 0;
    this->m_size = this->m_backend ? this->m_backend->size() : 0;
    if (this->m_size < 0)
        this->m_size = 0;
}

Source::~Source() {
    this->m_backend.reset();   // closes the file / socket state
    this->m_offset = 0;
    this->m_size   = 0;
}

bool Source::SetupResampler(int output_channels, int output_sample_rate) {
    // check if we even need the resampler.
    m_native_stream = GetChannelCount() == output_channels && GetSampleRate() == output_sample_rate;
    if (m_native_stream) {
        return true;
    }

    m_sdl_stream = UniqueAudioStream{
        SDL_NewAudioStreamEX(
        AUDIO_S16, GetChannelCount(), GetSampleRate(),
        AUDIO_S16, output_channels, output_sample_rate)
    };

    return m_sdl_stream != nullptr;
}

s64 Source::Resample(u8* out, std::size_t size) {
    if (!out || !size) {
        return -1;
    }

    if (m_native_stream) {
        return Decode(size / sizeof(s16), (s16*)out);
    } else {
        s64 data_read = 0;
        while (size > 0) {
            const auto sz = SDL_AudioStreamGetEX(m_sdl_stream.get(), out, size);

            if (sz < 0) {
                return -1;
            } else if (sz > 0) {
                size -= sz;
                out += sz;
                data_read += sz;
            } else {
                const auto dec_got = Decode(m_resample_buffer.size(), m_resample_buffer.data());
                if (dec_got == 0) {
                    return data_read;
                }
                if (0 != SDL_AudioStreamPutEX(m_sdl_stream.get(), m_resample_buffer.data(), dec_got)) {
                    return -1;
                }
            }
        }

        return data_read;
    }
}

size_t Source::ReadFile(void *_buffer, size_t read_size) {
    auto dst = static_cast<u8*>(_buffer);
    size_t amount = 0;

    // check if we already have this data buffered.
    if (m_buffered.size) {
        // check if we can read this data into the beginning of dst.
        if (this->m_offset < m_buffered.off + m_buffered.size && this->m_offset >= m_buffered.off) {
            const auto off = this->m_offset - m_buffered.off;
            const auto size = std::min<s64>(read_size, m_buffered.size - off);
            std::memcpy(dst, m_buffered.data + off, size);

            read_size -= size;
            m_offset += size;
            amount += size;
            dst += size;
        }
    }

    if (read_size) {
        u64 bytes_read = 0;

        // if the dst dst is big enough, read data in place.
        if (read_size >= sizeof(m_buffered.data)) {
            if ((bytes_read = this->m_backend->read(this->m_offset, dst, read_size)) != 0) {
                read_size -= bytes_read;
                m_offset += bytes_read;
                amount += bytes_read;
                dst += bytes_read;

                // save the last chunk of data to the m_buffered io.
                const auto max_advance = std::min(amount, sizeof(m_buffered.data));
                m_buffered.off = m_offset - max_advance;
                m_buffered.size = max_advance;
                std::memcpy(m_buffered.data, dst - max_advance, max_advance);
            }
        } else if ((bytes_read = this->m_backend->read(this->m_offset, m_buffered.data, sizeof(m_buffered.data))) != 0) {
            const auto max_advance = std::min(read_size, bytes_read);
            std::memcpy(dst, m_buffered.data, max_advance);

            m_buffered.off = m_offset;
            m_buffered.size = bytes_read;

            read_size -= max_advance;
            m_offset += max_advance;
            amount += max_advance;
            dst += max_advance;
        }
    }

    return amount;
}

bool Source::SeekFile(s64 offset, int origin) {
    s64 new_offset;
    switch (origin) {
        case SeekOrigin_SET:
            new_offset = offset;
            break;
        case SeekOrigin_CUR:
            new_offset = this->m_offset + offset;
            break;
        case SeekOrigin_END:
            new_offset = this->m_size + offset;
            break;
        default:
            return false;
    }

    if (new_offset >= 0 && new_offset <= this->m_size) {
        this->m_offset = new_offset;
        return true;
    } else {
        return false;
    }
}

s64 Source::TellFile() {
    return this->m_offset;
}

bool Source::Done() {
    auto [current, total] = this->Tell();

    return current == total;
}

#ifdef WANT_FLAC
class FlacFile final : public Source {
  private:
    drflac *m_flac;

  public:
    FlacFile(std::unique_ptr<IoBackend> backend) : Source(std::move(backend)) {
        this->m_flac = drflac_open(ReadCallback, FlacSeekCallback, FlacTellCallback, this, flac_alloc_ptr);
    }
    ~FlacFile() {
        if (this->m_flac != nullptr)
            drflac_close(this->m_flac);
    }

    bool IsOpen() override {
        return this->m_flac != nullptr;
    }

    size_t Decode(size_t sample_count, s16 *data) override {
        std::scoped_lock lk(this->m_mutex);

        return GetChannelCount() * sizeof(s16) * drflac_read_pcm_frames_s16(this->m_flac, sample_count / GetChannelCount(), data);
    }

    std::pair<u32, u32> Tell() override {
        std::scoped_lock lk(this->m_mutex);

        return {this->m_flac->currentPCMFrame, this->m_flac->totalPCMFrameCount};
    }

    bool Seek(u64 target) override {
        std::scoped_lock lk(this->m_mutex);

        return drflac_seek_to_pcm_frame(this->m_flac, target);
    }

    int GetSampleRate() override {
        return this->m_flac->sampleRate;
    }

    int GetChannelCount() override {
        return this->m_flac->channels;
    }
};
#endif

#ifdef WANT_MP3
class Mp3File final : public Source {
  private:
    drmp3 m_mp3;
    bool initialized;
    u64 m_total_frame_count;

  public:
    Mp3File(std::unique_ptr<IoBackend> backend) : Source(std::move(backend)) {
        if (drmp3_init(&this->m_mp3, ReadCallback, Mp3SeekCallback, Mp3TellCallback, nullptr, this, mp3_alloc_ptr)) {
            this->m_total_frame_count = drmp3_get_pcm_frame_count(&this->m_mp3);
            this->initialized         = true;
        }
    }
    ~Mp3File() {
        if (initialized)
            drmp3_uninit(&this->m_mp3);
    }

    bool IsOpen() override {
        return initialized;
    }

    size_t Decode(size_t sample_count, s16 *data) override {
        std::scoped_lock lk(this->m_mutex);

        return GetChannelCount() * sizeof(s16) * drmp3_read_pcm_frames_s16(&this->m_mp3, sample_count / GetChannelCount(), data);
    }

    std::pair<u32, u32> Tell() override {
        std::scoped_lock lk(this->m_mutex);

        return {this->m_mp3.currentPCMFrame, this->m_total_frame_count};
    }

    bool Seek(u64 target) override {
        std::scoped_lock lk(this->m_mutex);

        return drmp3_seek_to_pcm_frame(&this->m_mp3, target);
    }

    int GetSampleRate() override {
        return this->m_mp3.sampleRate;
    }

    int GetChannelCount() override {
        return this->m_mp3.channels;
    }
};
#endif

#ifdef WANT_WAV
class WavFile final : public Source {
  private:
    drwav m_wav;
    bool initialized;
    s32 m_bytes_per_pcm;

  public:
    WavFile(std::unique_ptr<IoBackend> backend) : Source(std::move(backend)) {
        if (drwav_init(&this->m_wav, ReadCallback, WavSeekCallback, WavTellCallback, this, wav_alloc_ptr)) {
            this->m_bytes_per_pcm = drwav_get_bytes_per_pcm_frame(&this->m_wav);
            this->initialized     = true;
        }
    }
    ~WavFile() {
        if (initialized)
            drwav_uninit(&this->m_wav);
    }

    bool IsOpen() override {
        return initialized;
    }

    size_t Decode(size_t sample_count, s16 *data) override {
        std::scoped_lock lk(this->m_mutex);

        return GetChannelCount() * sizeof(s16) * drwav_read_pcm_frames_s16(&this->m_wav, sample_count / GetChannelCount(), data);
    }

    std::pair<u32, u32> Tell() override {
        std::scoped_lock lk(this->m_mutex);

        u64 byte_position = this->m_wav.dataChunkDataSize - this->m_wav.bytesRemaining;
        return {byte_position / this->m_bytes_per_pcm, this->m_wav.totalPCMFrameCount};
    }

    bool Seek(u64 target) override {
        std::scoped_lock lk(this->m_mutex);

        return drwav_seek_to_pcm_frame(&this->m_wav, target);
    }

    int GetSampleRate() override {
        return this->m_wav.sampleRate;
    }

    int GetChannelCount() override {
        return this->m_wav.channels;
    }
};
#endif

namespace {
    // Construct the right decoder over a ready backend. Backend is consumed.
    std::unique_ptr<Source> make_decoder(SourceType type, std::unique_ptr<IoBackend> backend) {
        if (!backend) return nullptr;
        if (false) {}
#ifdef WANT_MP3
        else if (type == SourceType::MP3)  return std::make_unique<Mp3File>(std::move(backend));
#endif
#ifdef WANT_FLAC
        else if (type == SourceType::FLAC) return std::make_unique<FlacFile>(std::move(backend));
#endif
#ifdef WANT_WAV
        else if (type == SourceType::WAV)  return std::make_unique<WavFile>(std::move(backend));
#endif
        return nullptr;
    }
}

std::unique_ptr<Source> OpenFile(const char *path) {
    const auto type = GetSourceType(path);
    if (type == SourceType::NONE)
        return nullptr;

    FsFile file;
    if (R_FAILED(sdmc::OpenFile(&file, path)))
        return nullptr;

    // backend takes ownership of the file (closes it in its dtor).
    return make_decoder(type, MakeFileBackend(std::move(file)));
}

bool IsJellyPath(const char *path) {
    return std::strncmp(path, "jelly://", 8) == 0;
}

std::unique_ptr<Source> OpenJelly(const char *path) {
    // path = "jelly://<fmt>/<id>"
    const auto type = GetSourceType(path);
    if (type == SourceType::NONE)
        return nullptr;

    // Pick up the latest signed-in server + token from the SD config.
    jelly::LoadConfig();

    const char *rest  = path + 8;             // "<fmt>/<id>"
    const char *slash = std::strchr(rest, '/');
    if (!slash || !slash[1])
        return nullptr;
    const char *id = slash + 1;

    // direct-play request path; token is sent in the auth header by jelly_net.
    char req[192];
    std::snprintf(req, sizeof req, "/Audio/%s/stream?static=true", id);

    return make_decoder(type, MakeHttpBackend(req));
}

SourceType GetSourceType(const char* path) {
    if (IsJellyPath(path)) {
        const char *fmt = path + 8;           // "<fmt>/..."
        if (!strncasecmp(fmt, "mp3/", 4))  return SourceType::MP3;
        if (!strncasecmp(fmt, "flac/", 5)) return SourceType::FLAC;
        if (!strncasecmp(fmt, "wav/", 4))  return SourceType::WAV;
        return SourceType::NONE;
    }

    const auto ext = std::strrchr(path, '.');
    if (!ext) {
        return SourceType::NONE;
    }

    if (!strcasecmp(ext, ".mp3")) {
        return SourceType::MP3;
    } else if (!strcasecmp(ext, ".flac")) {
        return SourceType::FLAC;
    } else if (!strcasecmp(ext, ".wav") || !strcasecmp(ext, ".wave")) {
        return SourceType::WAV;
    }

    return SourceType::NONE;
}
