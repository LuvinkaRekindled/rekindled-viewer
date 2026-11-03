/**
 * @file audioengine_openal.cpp
 * @brief implementation of audio engine using OpenAL
 * support as a OpenAL 3D implementation
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "linden_common.h"
#include "lldir.h"
#include "llfile.h"

#include <chrono>
#include <iterator>
#include <vector>

#include "llaudioengine_openal.h"
#include "lllistener_openal.h"
#include "llstreamingaudio.h"

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

const float LLAudioEngine_OpenAL::WIND_BUFFER_SIZE_SEC = 0.05f;

LLAudioEngine_OpenAL::LLAudioEngine_OpenAL()
    :
    mWindGen(NULL),
    mWindBuf(NULL),
    mWindBufFreq(0),
    mWindBufSamples(0),
    mWindBufBytes(0),
    mWindSource(AL_NONE),
    mNumEmptyWindALBuffers(MAX_NUM_WIND_BUFFERS)
{
}

// virtual
LLAudioEngine_OpenAL::~LLAudioEngine_OpenAL()
{
}

// virtual
bool LLAudioEngine_OpenAL::init(void* userdata, const std::string &app_title)
{
    mWindGen = NULL;
    LLAudioEngine::init(userdata, app_title);

    // <FS:Ansariel> Output device selection
    mCurrentDevice = alcOpenDevice(nullptr);
    if (!mCurrentDevice)
    {
        LL_WARNS() << "LLAudioEngine_OpenAL::init() could not open the default output device" << LL_ENDL;
        return false;
    }
    mCurrentContext = alcCreateContext(mCurrentDevice, nullptr);
    if (!mCurrentContext || !alcMakeContextCurrent(mCurrentContext))
    {
        LL_WARNS() << "LLAudioEngine_OpenAL::init() could not create the OpenAL context" << LL_ENDL;
        if (mCurrentContext)
        {
            alcDestroyContext(mCurrentContext);
            mCurrentContext = nullptr;
        }
        alcCloseDevice(mCurrentDevice);
        mCurrentDevice = nullptr;
        return false;
    }
    // </FS:Ansariel>

    LL_INFOS() << "LLAudioEngine_OpenAL::init() OpenAL successfully initialized" << LL_ENDL;

    LL_INFOS() << "OpenAL version: "
        << ll_safe_string(alGetString(AL_VERSION)) << LL_ENDL;
    LL_INFOS() << "OpenAL vendor: "
        << ll_safe_string(alGetString(AL_VENDOR)) << LL_ENDL;
    LL_INFOS() << "OpenAL renderer: "
        << ll_safe_string(alGetString(AL_RENDERER)) << LL_ENDL;

    ALCdevice *device = alcGetContextsDevice(alcGetCurrentContext());
    ALint major = 0;
    ALint minor = 0;

    alcGetIntegerv(device, ALC_MAJOR_VERSION, 1, &major);
    alcGetIntegerv(device, ALC_MINOR_VERSION, 1, &minor);
    LL_INFOS() << "ALC version: " << major << "." << minor << LL_ENDL;

    LL_INFOS() << "ALC default device: "
        << ll_safe_string(alcGetString(device,
                           ALC_DEFAULT_DEVICE_SPECIFIER))
        << LL_ENDL;

    // <FS:Ansariel> Output device selection
    enumerateDevices();
    {
        std::lock_guard<std::mutex> lock(mPostMutex);
        mAudioThread = std::make_unique<LL::ThreadPool>("OpenALAudio", 1, 1024);
        mAudioThread->start();
    }
    setupDeviceEvents(device);

    return true;
}

// virtual
std::string LLAudioEngine_OpenAL::getDriverName(bool verbose)
{
    ALCdevice *device = alcGetContextsDevice(alcGetCurrentContext());
    std::ostringstream version;

    version <<
        "OpenAL";

    if (verbose)
    {
        version <<
            ", version " <<
            ll_safe_string(alGetString(AL_VERSION)) <<
            " / " <<
            ll_safe_string(alGetString(AL_VENDOR)) <<
            " / " <<
            ll_safe_string(alGetString(AL_RENDERER));

        if (device)
            version <<
                ": " <<
                ll_safe_string(alcGetString(device,
                    ALC_DEFAULT_DEVICE_SPECIFIER));
    }

    return version.str();
}

// virtual
void LLAudioEngine_OpenAL::allocateListener()
{
    mListenerp = (LLListener *) new LLListener_OpenAL();
    if(!mListenerp)
    {
        LL_WARNS() << "LLAudioEngine_OpenAL::allocateListener() Listener creation failed" << LL_ENDL;
    }
}

// virtual
void LLAudioEngine_OpenAL::shutdown()
{
    // the system event callback is global to OpenAL Soft and holds this engine: take it away first
    if (mCurrentDevice && alcIsExtensionPresent(mCurrentDevice, "ALC_SOFT_system_events"))
    {
        auto alcEventCallbackSOFT = (LPALCEVENTCALLBACKSOFT)alcGetProcAddress(mCurrentDevice, "alcEventCallbackSOFT");
        auto alcEventControlSOFT = (LPALCEVENTCONTROLSOFT)alcGetProcAddress(mCurrentDevice, "alcEventControlSOFT");
        if (alcEventCallbackSOFT && alcEventControlSOFT)
        {
            ALCenum events[] = { ALC_EVENT_TYPE_DEVICE_ADDED_SOFT, ALC_EVENT_TYPE_DEVICE_REMOVED_SOFT, ALC_EVENT_TYPE_DEFAULT_DEVICE_CHANGED_SOFT };
            alcEventControlSOFT(3, events, AL_FALSE);
            alcEventCallbackSOFT(nullptr, nullptr);
        }
    }

    // no device call may run on the audio thread while the channels and the context are torn down
    {
        std::lock_guard<std::mutex> lock(mPostMutex);
        if (mAudioThread)
        {
            mAudioThread->close();      // runs what is queued, then joins
            mAudioThread.reset();
        }
    }

    LL_INFOS() << "About to LLAudioEngine::shutdown()" << LL_ENDL;
    LLAudioEngine::shutdown();

    ALenum error = alGetError();
    if (error != AL_NO_ERROR)
    {
        LL_WARNS() << "Uncleared error state prior to shutdown: " << error << LL_ENDL;
    }

    alcMakeContextCurrent(nullptr);
    if (mCurrentContext)
    {
        alcDestroyContext(mCurrentContext);
        mCurrentContext = nullptr;
    }
    if (mCurrentDevice)
    {
        alcCloseDevice(mCurrentDevice);
        mCurrentDevice = nullptr;
    }

    LL_INFOS() << "LLAudioEngine_OpenAL::shutdown() OpenAL successfully shut down" << LL_ENDL;

    delete mListenerp;
    mListenerp = NULL;
}

LLAudioBuffer *LLAudioEngine_OpenAL::createBuffer()
{
    return new LLAudioBufferOpenAL();
}

LLAudioChannel *LLAudioEngine_OpenAL::createChannel()
{
    return new LLAudioChannelOpenAL();
}

void LLAudioEngine_OpenAL::setInternalGain(F32 gain)
{
    //LL_INFOS() << "LLAudioEngine_OpenAL::setInternalGain() Gain: " << gain << LL_ENDL;
    alListenerf(AL_GAIN, gain);
}

LLAudioChannelOpenAL::LLAudioChannelOpenAL()
    :
    mALSource(AL_NONE),
    mLastSamplePos(0)
{
    alGenSources(1, &mALSource);
}

LLAudioChannelOpenAL::~LLAudioChannelOpenAL()
{
    cleanup();
    alDeleteSources(1, &mALSource);
}

void LLAudioChannelOpenAL::cleanup()
{
    alSourceStop(mALSource);
    alSourcei(mALSource, AL_BUFFER, AL_NONE);

    mCurrentBufferp = NULL;
}

void LLAudioChannelOpenAL::play()
{
    if (mALSource == AL_NONE)
    {
        LL_WARNS() << "Playing without a mALSource, aborting" << LL_ENDL;
        return;
    }

    if(!isPlaying())
    {
        alSourcePlay(mALSource);
        getSource()->setPlayedOnce(true);
    }
}

void LLAudioChannelOpenAL::playSynced(LLAudioChannel *channelp)
{
    if (channelp)
    {
        LLAudioChannelOpenAL *masterchannelp =
            (LLAudioChannelOpenAL*)channelp;
        if (mALSource != AL_NONE &&
            masterchannelp->mALSource != AL_NONE)
        {
            // we have channels allocated to master and slave
            ALfloat master_offset;
            alGetSourcef(masterchannelp->mALSource, AL_SEC_OFFSET,
                     &master_offset);

            LL_INFOS() << "Syncing with master at " << master_offset
                << "sec" << LL_ENDL;
            // *TODO: detect when this fails, maybe use AL_SAMPLE_
            alSourcef(mALSource, AL_SEC_OFFSET, master_offset);
        }
    }
    play();
}

bool LLAudioChannelOpenAL::isPlaying()
{
    if (mALSource != AL_NONE)
    {
        ALint state;
        alGetSourcei(mALSource, AL_SOURCE_STATE, &state);
        if(state == AL_PLAYING)
        {
            return true;
        }
    }

    return false;
}

bool LLAudioChannelOpenAL::updateBuffer()
{
    if (!mCurrentSourcep)
    {
        // This channel isn't associated with any source, nothing
        // to be updated
        return false;
    }

    if (LLAudioChannel::updateBuffer())
    {
        // Base class update returned true, which means that we need to actually
        // set up the source for a different buffer.
        LLAudioBufferOpenAL *bufferp = (LLAudioBufferOpenAL *)mCurrentSourcep->getCurrentBuffer();
        ALuint buffer = bufferp->getBuffer();
        alSourcei(mALSource, AL_BUFFER, buffer);
        mLastSamplePos = 0;
    }

    if (mCurrentSourcep)
    {
        alSourcef(mALSource, AL_GAIN,
              mCurrentSourcep->getGain() * getSecondaryGain());
        alSourcei(mALSource, AL_LOOPING,
              mCurrentSourcep->isLoop() ? AL_TRUE : AL_FALSE);
        alSourcef(mALSource, AL_ROLLOFF_FACTOR,
              gAudiop->mListenerp->getRolloffFactor());
    }

    return true;
}


void LLAudioChannelOpenAL::updateLoop()
{
    if (mALSource == AL_NONE)
    {
        return;
    }

    // Hack:  We keep track of whether we looped or not by seeing when the
    // sample position looks like it's going backwards.  Not reliable; may
    // yield false negatives.
    //
    ALint cur_pos;
    alGetSourcei(mALSource, AL_SAMPLE_OFFSET, &cur_pos);
    if (cur_pos < mLastSamplePos)
    {
        mLoopedThisFrame = true;
    }
    mLastSamplePos = cur_pos;
}


void LLAudioChannelOpenAL::update3DPosition()
{
    if(!mCurrentSourcep)
    {
        return;
    }
    if (mCurrentSourcep->isForcedPriority())
    {
        alSource3f(mALSource, AL_POSITION, 0.0, 0.0, 0.0);
        alSource3f(mALSource, AL_VELOCITY, 0.0, 0.0, 0.0);
        alSourcei (mALSource, AL_SOURCE_RELATIVE, AL_TRUE);
    } else {
        LLVector3 float_pos;
        float_pos.setVec(mCurrentSourcep->getPositionGlobal());
        alSourcefv(mALSource, AL_POSITION, float_pos.mV);
        alSourcefv(mALSource, AL_VELOCITY, mCurrentSourcep->getVelocity().mV);
        alSourcei (mALSource, AL_SOURCE_RELATIVE, AL_FALSE);
    }

    alSourcef(mALSource, AL_GAIN, mCurrentSourcep->getGain() * getSecondaryGain());
}

LLAudioBufferOpenAL::LLAudioBufferOpenAL()
{
    mALBuffer = AL_NONE;
}

LLAudioBufferOpenAL::~LLAudioBufferOpenAL()
{
    cleanup();
}

void LLAudioBufferOpenAL::cleanup()
{
    if(mALBuffer != AL_NONE)
    {
        alGetError(); // <ND/>
        alDeleteBuffers(1, &mALBuffer);

        // <FS:ND> Print warning on possible leak.
        ALenum error = alGetError();
        if( AL_NO_ERROR != error )
            LL_WARNS() << "openal error: " << error << " possible memory leak hit" << LL_ENDL;
        // </FS:ND>

        mALBuffer = AL_NONE;
    }
}

bool LLAudioBufferOpenAL::loadWAV(const std::string& filename)
{
    cleanup();

    // The decoder writes a plain PCM wav (RIFF header, "fmt " and "data" chunks)
    llifstream file(filename.c_str(), std::ios::binary);
    if (!file.is_open())
    {
        // It's common for the file to not actually exist.
        LL_DEBUGS() << "LLAudioBufferOpenAL::loadWAV() Error loading " << filename << ": no such file" << LL_ENDL;
        return false;
    }

    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    auto fail = [&](const char* why)
    {
        LL_WARNS() << "LLAudioBufferOpenAL::loadWAV() Error loading " << filename << ": " << why << LL_ENDL;
        return false;
    };

    auto u16 = [&](size_t at) { return (U32)(U8)bytes[at] | ((U32)(U8)bytes[at + 1] << 8); };
    auto u32 = [&](size_t at) { return u16(at) | (u16(at + 2) << 16); };

    if (bytes.size() < 12 || memcmp(bytes.data(), "RIFF", 4) != 0 || memcmp(bytes.data() + 8, "WAVE", 4) != 0)
    {
        return fail("not a wav file");
    }

    U32 channels = 0;
    U32 rate = 0;
    U32 bits = 0;
    U32 tag = 0;
    size_t data_at = 0;
    size_t data_size = 0;
    size_t at = 12;
    while (at + 8 <= bytes.size())
    {
        const size_t chunk_size = u32(at + 4);
        const size_t body = at + 8;
        if (memcmp(bytes.data() + at, "fmt ", 4) == 0 && chunk_size >= 16 && body + 16 <= bytes.size())
        {
            tag = u16(body);
            channels = u16(body + 2);
            rate = u32(body + 4);
            bits = u16(body + 14);
        }
        else if (memcmp(bytes.data() + at, "data", 4) == 0)
        {
            data_at = body;
            // a size of 0 or past the end (a file still being written) means the rest of the file
            data_size = (chunk_size == 0 || body + chunk_size > bytes.size()) ? bytes.size() - body : chunk_size;
            break;
        }
        at = body + chunk_size + (chunk_size & 1);
    }

    if (data_at == 0 || rate == 0)
    {
        return fail("no fmt or data chunk");
    }
    if (tag != 1 /*PCM*/ && tag != 0xFFFE /*extensible, 16 bit PCM from the decoder*/)
    {
        return fail("not PCM");
    }

    ALenum format = AL_NONE;
    if (bits == 16)
    {
        format = (channels == 1) ? AL_FORMAT_MONO16 : (channels == 2) ? AL_FORMAT_STEREO16 : AL_NONE;
        data_size &= ~(size_t)(2 * channels - 1);
    }
    else if (bits == 8)
    {
        format = (channels == 1) ? AL_FORMAT_MONO8 : (channels == 2) ? AL_FORMAT_STEREO8 : AL_NONE;
        data_size &= ~(size_t)(channels - 1);
    }
    if (format == AL_NONE || data_size == 0)
    {
        return fail("unsupported format or no samples");
    }

    alGetError();
    alGenBuffers(1, &mALBuffer);
    alBufferData(mALBuffer, format, bytes.data() + data_at, (ALsizei)data_size, (ALsizei)rate);
    const ALenum error = alGetError();
    if (error != AL_NO_ERROR)
    {
        cleanup();
        mALBuffer = AL_NONE;
        return fail("OpenAL refused the buffer");
    }

    return true;
}

U32 LLAudioBufferOpenAL::getLength()
{
    if(mALBuffer == AL_NONE)
    {
        return 0;
    }
    ALint length;
    alGetBufferi(mALBuffer, AL_SIZE, &length);
    return length / 2; // convert size in bytes to size in (16-bit) samples
}

// ------------

bool LLAudioEngine_OpenAL::initWind()
{
    ALenum error;
    LL_INFOS() << "LLAudioEngine_OpenAL::initWind() start" << LL_ENDL;

    mNumEmptyWindALBuffers = MAX_NUM_WIND_BUFFERS;

    alGetError(); /* clear error */

    alGenSources(1,&mWindSource);

    if((error=alGetError()) != AL_NO_ERROR)
    {
        LL_WARNS() << "LLAudioEngine_OpenAL::initWind() Error creating wind sources: "<<error<<LL_ENDL;
    }

    mWindGen = new LLWindGen<WIND_SAMPLE_T>;

    mWindBufFreq = mWindGen->getInputSamplingRate();
    mWindBufSamples = llceil(mWindBufFreq * WIND_BUFFER_SIZE_SEC);
    mWindBufBytes = mWindBufSamples * 2 /*stereo*/ * sizeof(WIND_SAMPLE_T);

    mWindCallback = false;
    if (alIsExtensionPresent("AL_SOFT_callback_buffer"))
    {
        auto alBufferCallbackSOFT = (LPALBUFFERCALLBACKSOFT)alGetProcAddress("alBufferCallbackSOFT");
        if (alBufferCallbackSOFT)
        {
            alGenBuffers(1, &mWindBuffer);
            alBufferCallbackSOFT(mWindBuffer, AL_FORMAT_STEREO_FLOAT32, (ALsizei)mWindBufFreq, windCallback, this);
            alSourcei(mWindSource, AL_BUFFER, (ALint)mWindBuffer);
            alSourcei(mWindSource, AL_LOOPING, AL_FALSE);
            alSource3f(mWindSource, AL_POSITION, 0.0, 0.0, 0.0);
            alSource3f(mWindSource, AL_VELOCITY, 0.0, 0.0, 0.0);
            alSourcef(mWindSource, AL_ROLLOFF_FACTOR, 0.0);
            alSourcei(mWindSource, AL_SOURCE_RELATIVE, AL_TRUE);
            if (alGetError() == AL_NO_ERROR)
            {
                mWindCallback = true;
                alSourcePlay(mWindSource);
                LL_INFOS() << "LLAudioEngine_OpenAL::initWind() wind plays from a callback buffer (mixer thread)" << LL_ENDL;
                return true;
            }
            LL_WARNS() << "LLAudioEngine_OpenAL::initWind() callback buffer failed - using queued buffers" << LL_ENDL;
            alSourcei(mWindSource, AL_BUFFER, 0);
            alDeleteBuffers(1, &mWindBuffer);
            mWindBuffer = AL_NONE;
        }
    }

    mWindBuf = new WIND_SAMPLE_T [mWindBufSamples * 2 /*stereo*/];

    if(mWindBuf==NULL)
    {
        LL_ERRS() << "LLAudioEngine_OpenAL::initWind() Error creating wind memory buffer" << LL_ENDL;
        return false;
    }

    LL_INFOS() << "LLAudioEngine_OpenAL::initWind() done" << LL_ENDL;

    return true;
}

void LLAudioEngine_OpenAL::cleanupWind()
{
    LL_INFOS() << "LLAudioEngine_OpenAL::cleanupWind()" << LL_ENDL;

    if (mWindSource != AL_NONE)
    {
        // detach and delete all outstanding buffers on the wind source
        alSourceStop(mWindSource);
        if (mWindCallback)
        {
            // stopped: the mixer thread is out of the callback (alSourceStop returns once it has taken the change)
            alSourcei(mWindSource, AL_BUFFER, 0);
            alDeleteBuffers(1, &mWindBuffer);
            mWindBuffer = AL_NONE;
            mWindCallback = false;
        }
        ALint processed = 0;
        if (mWindBuf)
        {
            alGetSourcei(mWindSource, AL_BUFFERS_PROCESSED, &processed);
        }
        while (processed--)
        {
            ALuint buffer = AL_NONE;
            alSourceUnqueueBuffers(mWindSource, 1, &buffer);
            alDeleteBuffers(1, &buffer);
        }

        // delete the wind source itself
        alDeleteSources(1, &mWindSource);

        mWindSource = AL_NONE;
    }

    delete[] mWindBuf;
    mWindBuf = NULL;

    delete mWindGen;
    mWindGen = NULL;
}

// The mixer thread asks for samples (AL_SOFT_callback_buffer). Runs on OpenAL Soft's thread: no locks, no allocation, no AL calls.
ALsizei AL_APIENTRY LLAudioEngine_OpenAL::windCallback(ALvoid* userptr, ALvoid* data, ALsizei bytes) noexcept
{
    return static_cast<LLAudioEngine_OpenAL*>(userptr)->generateWind(data, bytes);
}

ALsizei LLAudioEngine_OpenAL::generateWind(void* data, ALsizei bytes)
{
    LLWindGen<WIND_SAMPLE_T>* gen = mWindGen;
    const ALsizei frame = (ALsizei)(2 * sizeof(WIND_SAMPLE_T));
    const ALsizei samples = bytes / frame;
    if (!gen || samples <= 0)
    {
        memset(data, 0, bytes);
        return bytes;
    }
    gen->mTargetFreq = mWindTargetFreq.load(std::memory_order_relaxed);
    gen->mTargetGain = mWindTargetGain.load(std::memory_order_relaxed);
    gen->mTargetPanGainR = mWindTargetPanR.load(std::memory_order_relaxed);
    gen->windGenerate(static_cast<WIND_SAMPLE_T*>(data), samples);
    return samples * frame;
}

void LLAudioEngine_OpenAL::updateWind(LLVector3 wind_vec, F32 camera_altitude)
{
    LLVector3 wind_pos;
    F64 pitch;
    F64 center_freq;
    ALenum error;

    if (!mEnableWind)
        return;

    if (!mWindGen || (!mWindCallback && !mWindBuf))
        return;

    if (mWindUpdateTimer.checkExpirationAndReset(LL_WIND_UPDATE_INTERVAL))
    {

        // wind comes in as Linden coordinate (+X = forward, +Y = left, +Z = up)
        // need to convert this to the conventional orientation DS3D and OpenAL use
        // where +X = right, +Y = up, +Z = backwards

        wind_vec.setVec(-wind_vec.mV[1], wind_vec.mV[2], -wind_vec.mV[0]);

        pitch = 1.0 + mapWindVecToPitch(wind_vec);
        center_freq = 80.0 * pow(pitch,2.5*(mapWindVecToGain(wind_vec)+1.0));

        if (mWindCallback)
        {
            // three numbers for the mixer thread; the source was set up once
            mWindTargetFreq = (F32)center_freq;
            mWindTargetGain = (F32)mapWindVecToGain(wind_vec) * mMaxWindGain;
            mWindTargetPanR = (F32)mapWindVecToPan(wind_vec);

            // a device that went away stops the source: start it again
            ALint state = AL_PLAYING;
            alGetSourcei(mWindSource, AL_SOURCE_STATE, &state);
            if (state != AL_PLAYING)
            {
                alSourcePlay(mWindSource);
            }
            return;
        }

        mWindGen->mTargetFreq = (F32)center_freq;
        mWindGen->mTargetGain = (F32)mapWindVecToGain(wind_vec) * mMaxWindGain;
        mWindGen->mTargetPanGainR = (F32)mapWindVecToPan(wind_vec);

        alSourcei(mWindSource, AL_LOOPING, AL_FALSE);
        alSource3f(mWindSource, AL_POSITION, 0.0, 0.0, 0.0);
        alSource3f(mWindSource, AL_VELOCITY, 0.0, 0.0, 0.0);
        alSourcef(mWindSource, AL_ROLLOFF_FACTOR, 0.0);
        alSourcei(mWindSource, AL_SOURCE_RELATIVE, AL_TRUE);
    }

    if (mWindCallback)
    {
        return;
    }

    // ok lets make a wind buffer now

    ALint processed, queued, unprocessed;
    alGetSourcei(mWindSource, AL_BUFFERS_PROCESSED, &processed);
    alGetSourcei(mWindSource, AL_BUFFERS_QUEUED, &queued);
    unprocessed = queued - processed;

    // ensure that there are always at least 3x as many filled buffers
    // queued as we managed to empty since last time.
    mNumEmptyWindALBuffers = llmin(mNumEmptyWindALBuffers + processed * 3 - unprocessed, MAX_NUM_WIND_BUFFERS-unprocessed);
    mNumEmptyWindALBuffers = llmax(mNumEmptyWindALBuffers, 0);

    //LL_INFOS() << "mNumEmptyWindALBuffers: " << mNumEmptyWindALBuffers    <<" (" << unprocessed << ":" << processed << ")" << LL_ENDL;

    while(processed--) // unqueue old buffers
    {
        ALuint buffer;
        ALenum error;
        alGetError(); /* clear error */
        alSourceUnqueueBuffers(mWindSource, 1, &buffer);
        error = alGetError();
        if(error != AL_NO_ERROR)
        {
            LL_WARNS() << "LLAudioEngine_OpenAL::updateWind() error swapping (unqueuing) buffers" << LL_ENDL;
        }
        else
        {
            alDeleteBuffers(1, &buffer);
        }
    }

    unprocessed += mNumEmptyWindALBuffers;
    while (mNumEmptyWindALBuffers > 0) // fill+queue new buffers
    {
        ALuint buffer;
        alGetError(); /* clear error */
        alGenBuffers(1,&buffer);
        if((error=alGetError()) != AL_NO_ERROR)
        {
            LL_WARNS() << "LLAudioEngine_OpenAL::updateWind() Error creating wind buffer: " << error << LL_ENDL;
            break;
        }

        alBufferData(buffer,
                 AL_FORMAT_STEREO_FLOAT32,
                 mWindGen->windGenerate(mWindBuf,
                            mWindBufSamples),
                 mWindBufBytes,
                 mWindBufFreq);
        error = alGetError();
        if(error != AL_NO_ERROR)
        {
            LL_WARNS() << "LLAudioEngine_OpenAL::updateWind() error swapping (bufferdata) buffers" << LL_ENDL;
        }

        alSourceQueueBuffers(mWindSource, 1, &buffer);
        error = alGetError();
        if(error != AL_NO_ERROR)
        {
            LL_WARNS() << "LLAudioEngine_OpenAL::updateWind() error swapping (queuing) buffers" << LL_ENDL;
        }

        --mNumEmptyWindALBuffers;
    }

    ALint playing;
    alGetSourcei(mWindSource, AL_SOURCE_STATE, &playing);
    if(playing != AL_PLAYING)
    {
        alSourcePlay(mWindSource);

        LL_DEBUGS() << "Wind had stopped - probably ran out of buffers - restarting: " << (unprocessed+mNumEmptyWindALBuffers) << " now queued." << LL_ENDL;
    }
}

// <FS:Ansariel> Output device selection
static LLUUID generate_uuid_from_string(const std::string& str)
{
    // Setup standard DNS namespace
    boost::uuids::string_generator string_generator;
    boost::uuids::uuid dns_namespace = string_generator("6144c9de-e8f3-4603-b8f7-a195db46f0c0");

    // Setup SHA1 name generator with the namespace
    boost::uuids::name_generator_sha1 uuid_generator(dns_namespace);

    // Generate deterministic UUID based on a string
    boost::uuids::uuid deterministic_uuid = uuid_generator(str);

    return LLUUID(boost::uuids::to_string(deterministic_uuid));
}

static void ALC_APIENTRY device_change_callback(ALCenum eventType, ALCenum deviceType, ALCdevice* device, ALCsizei length, const ALCchar* message, void* userParam) noexcept
{
    LLAudioEngine_OpenAL* audio_engine = (LLAudioEngine_OpenAL*)userParam;

    if (!audio_engine)
        return;

    if (eventType == ALC_EVENT_TYPE_DEVICE_ADDED_SOFT || eventType == ALC_EVENT_TYPE_DEVICE_REMOVED_SOFT)
    {
        if (deviceType == ALC_PLAYBACK_DEVICE_SOFT)
        {
            LL_INFOS() << "LLAudioEngine_OpenAL:: Playback device list changed" << LL_ENDL;
            audio_engine->postAudio([audio_engine]() { audio_engine->deviceListChanged(); });
            return;
        }
    }

    if (eventType == ALC_EVENT_TYPE_DEFAULT_DEVICE_CHANGED_SOFT)
    {
        LL_INFOS() << "LLAudioEngine_OpenAL:: Default playback device changed" << LL_ENDL;
        audio_engine->postAudio([audio_engine]() { audio_engine->deviceListChanged(); });
    }
}

void LLAudioEngine_OpenAL::setupDeviceEvents(ALCdevice* device)
{
    if (!alcIsExtensionPresent(device, "ALC_SOFT_system_events"))
    {
        LL_WARNS() << "LLAudioEngine_OpenAL:: ALC_SOFT_system_events not supported by backend" << LL_ENDL;
        return;
    }

    auto alcEventCallbackSOFT = (LPALCEVENTCALLBACKSOFT)alcGetProcAddress(device, "alcEventCallbackSOFT");
    auto alcEventControlSOFT = (LPALCEVENTCONTROLSOFT)alcGetProcAddress(device, "alcEventControlSOFT");
    if (!alcEventCallbackSOFT || !alcEventControlSOFT)
    {
        LL_WARNS() << "LLAudioEngine_OpenAL:: ALC_SOFT_system_events functions not available" << LL_ENDL;
        return;
    }

    // Register the callback function
    alcEventCallbackSOFT(device_change_callback, this);

    // Define which events we want to listen to
    ALCenum events[] = {
        ALC_EVENT_TYPE_DEVICE_ADDED_SOFT,
        ALC_EVENT_TYPE_DEVICE_REMOVED_SOFT,
        ALC_EVENT_TYPE_DEFAULT_DEVICE_CHANGED_SOFT
    };

    // Enable events
    alcEventControlSOFT(3, events, AL_TRUE);
}

void LLAudioEngine_OpenAL::changeDevice()
{
    LPALCREOPENDEVICESOFT alcReopenDeviceSOFT = (LPALCREOPENDEVICESOFT)alcGetProcAddress(mCurrentDevice, "alcReopenDeviceSOFT");

    if (!alcReopenDeviceSOFT || !alcIsExtensionPresent(mCurrentDevice, "ALC_SOFT_reopen_device"))
    {
        LL_WARNS() << "LLAudioEngine_OpenAL::setDevice(): Cannot change output device - OpenAL ALC_SOFT_reopen_device extension missing" << LL_ENDL;
        return;
    }

    std::optional<std::string> device_name{};

    if (mSelectedDeviceUUID.isNull())
    {
        LL_INFOS() << "LLAudioEngine_OpenAL::setDevice(): Setting new output to system default (" << mSelectedDeviceUUID.asString() << ")" << LL_ENDL;
    }
    else
    {
        for (const auto& [id, name] : getDevices())
        {
            if (id == mSelectedDeviceUUID)
            {
                LL_INFOS() << "LLAudioEngine_OpenAL::setDevice(): Setting new output " << name << " (" << id.asString() << ")" << LL_ENDL;
                device_name = name;
                break;
            }
        }

        if (!device_name.has_value())
        {
            LL_WARNS() << "LLAudioEngine_OpenAL::setDevice(): Output device not found - falling back to system default" << LL_ENDL;
        }
    }

    const auto reopen_start = std::chrono::steady_clock::now();
    if (alcReopenDeviceSOFT(mCurrentDevice, device_name.has_value() ? device_name.value().c_str() : nullptr, nullptr))
    {
        alcMakeContextCurrent(mCurrentContext);
        {
            std::lock_guard<std::mutex> lock(mDeviceMutex);
            mOpenDeviceName = device_name.value_or(std::string());
        }
        ++mDeviceGeneration;
        LL_INFOS() << "LLAudioEngine_OpenAL::changeDevice(): reopened in "
            << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - reopen_start).count() << " ms (audio thread)" << LL_ENDL;
        applyHRTF(true);
    }
    else
    {
        LL_WARNS() << "LLAudioEngine_OpenAL::setDevice() error: Reopen failed" << LL_ENDL;
    }
}

void LLAudioEngine_OpenAL::setHRTF(bool enable)
{
    postAudio([this, enable]()
    {
        mHRTF = enable;
        applyHRTF(false);
    });
}

// OpenAL Soft turns HRTF on by itself for a headphone output; the setting decides (off by default)
void LLAudioEngine_OpenAL::applyHRTF(bool force)
{
    if (!mCurrentDevice || !alcIsExtensionPresent(mCurrentDevice, "ALC_SOFT_HRTF"))
    {
        return;
    }

    if (!force && mHRTFApplied == (mHRTF ? 1 : 0))
    {
        return;
    }

    LPALCRESETDEVICESOFT alcResetDeviceSOFT = (LPALCRESETDEVICESOFT)alcGetProcAddress(mCurrentDevice, "alcResetDeviceSOFT");
    if (!alcResetDeviceSOFT)
    {
        return;
    }

    ALCint attribs[] = { ALC_HRTF_SOFT, mHRTF ? ALC_TRUE : ALC_FALSE, 0 };
    if (!alcResetDeviceSOFT(mCurrentDevice, attribs))
    {
        LL_WARNS() << "LLAudioEngine_OpenAL::applyHRTF(): alcResetDeviceSOFT failed" << LL_ENDL;
        return;
    }

    mHRTFApplied = mHRTF ? 1 : 0;
    ALCint status = 0;
    alcGetIntegerv(mCurrentDevice, ALC_HRTF_STATUS_SOFT, 1, &status);
    LL_INFOS() << "LLAudioEngine_OpenAL::applyHRTF(): requested " << (mHRTF ? "on" : "off") << ", HRTF status " << status
        << " (0 disabled, 1 enabled, 2 denied, 3 required, 4 headphones detected, 5 unsupported format)" << LL_ENDL;
}

LLAudioEngine_OpenAL::output_device_map_t LLAudioEngine_OpenAL::getDevices()
{
    std::lock_guard<std::mutex> lock(mDeviceMutex);
    return mDeviceCache;
}

void LLAudioEngine_OpenAL::postAudio(std::function<void()> work)
{
    std::lock_guard<std::mutex> lock(mPostMutex);
    if (mAudioThread)
    {
        mAudioThread->getQueue().post(std::move(work));
    }
}

void LLAudioEngine_OpenAL::deviceListChanged()
{
    enumerateDevices();
    changeDevice();
    mDeviceListChanged = true;
}

// Reads the playback devices from OpenAL Soft (a call into the Windows audio service: audio thread, or init) and keeps them for getDevices()
void LLAudioEngine_OpenAL::enumerateDevices()
{
    if (!alcIsExtensionPresent(nullptr, "ALC_ENUMERATE_ALL_EXT"))
    {
        LL_WARNS() << "LLAudioEngine_OpenAL::enumerateDevices(): Cannot determine playback devices - OpenAL ALC_ENUMERATE_ALL_EXT extension missing" << LL_ENDL;
        return;
    }

    const ALCchar* deviceList = alcGetString(nullptr, ALC_ALL_DEVICES_SPECIFIER);

    output_device_map_t devices;
    const ALCchar* nextDevice = deviceList;

    // Parse double-null terminated string block
    while (nextDevice && *nextDevice != '\0')
    {
        std::string device(nextDevice);
        devices.emplace(generate_uuid_from_string(device), device);
        nextDevice += strlen(nextDevice) + 1;
    }

    std::lock_guard<std::mutex> lock(mDeviceMutex);
    mDeviceCache = std::move(devices);
}

void LLAudioEngine_OpenAL::setDevice(const LLUUID& device_uuid)
{
    postAudio([this, device_uuid]()
    {
        mSelectedDeviceUUID = device_uuid;
        changeDevice();
    });
}

void LLAudioEngine_OpenAL::idle()
{
    // the stream plays through the VLC plugin, not through OpenAL: tell it where the output went
    LLStreamingAudioInterface* stream = getStreamingAudioImpl();
    if (mStreamDeviceGeneration != mDeviceGeneration && stream)
    {
        mStreamDeviceGeneration = mDeviceGeneration;
        std::string name;
        {
            std::lock_guard<std::mutex> lock(mDeviceMutex);
            name = mOpenDeviceName;
        }
        stream->setOutputDevice(name);
    }


    if (mDeviceListChanged)
    {
        mDeviceListChanged = false;
        OnOutputDeviceListChanged(getDevices());
    }

    LLAudioEngine::idle();
}
