#include "qdirectsound.h"

#include "loggingcategory.h"
#include "qdirectsound.h"
#include "windows.h"
#include "linux/linuxinterface.h"
#include <unordered_map>
#include <memory>

#include <QAudioOutput>

namespace
{

    class DirectSoundGenerator : public IDirectSoundBuffer
    {
    public:
        DirectSoundGenerator(LPCDSBUFFERDESC lpcDSBufferDesc);

        void start();
        void stop();
        void writeAudio();
        void stateChanged(QAudio::State state);
        void setOptions(const qint32 initialSilence);

        virtual HRESULT Release() override;

    private:
        typedef short int audio_t;

        std::shared_ptr<QAudioOutput> myAudioOutput;
        QAudioFormat myAudioFormat;
        QIODevice * myDevice;

        // options
        qint32 myInitialSilence;

        void setVolume();
        bool isRunning();
        void initialise();
        void writeEnoughSilence(const qint64 ms);
    };


    std::unordered_map<IDirectSoundBuffer *, DirectSoundGenerator *> activeSoundGenerators;

    DirectSoundGenerator::DirectSoundGenerator(LPCDSBUFFERDESC lpcDSBufferDesc) : IDirectSoundBuffer(lpcDSBufferDesc)
    {
        myInitialSilence = 200;
    }

    HRESULT DirectSoundGenerator::Release()
    {
        stop();
        activeSoundGenerators.erase(this);
        return IDirectSoundBuffer::Release();
    }

    void DirectSoundGenerator::initialise()
    {
        // only initialise here to skip all the buffers which are not in DSBSTATUS_PLAYING mode
        QAudioFormat audioFormat;
        audioFormat.setSampleRate(mySampleRate);
        audioFormat.setChannelCount(myChannels);
        audioFormat.setSampleSize(myBitsPerSample);
        audioFormat.setCodec(QString::fromUtf8("audio/pcm"));
        audioFormat.setByteOrder(QAudioFormat::LittleEndian);
        audioFormat.setSampleType(QAudioFormat::SignedInt);

        myAudioOutput = std::make_shared<QAudioOutput>(audioFormat);
        myAudioFormat = myAudioOutput->format();
    }

    void DirectSoundGenerator::setOptions(const qint32 initialSilence)
    {
        myInitialSilence = std::max(0, initialSilence);
    }

    bool DirectSoundGenerator::isRunning()
    {
        if (!myAudioOutput)
        {
            return false;
        }

        const QAudio::State state = myAudioOutput->state();
        const QAudio::Error error = myAudioOutput->error();
        if (state == QAudio::ActiveState)
        {
            return true;
        }
        if (state == QAudio::IdleState && error == QAudio::NoError)
        {
            return true;
        }
        return false;
    }

    void DirectSoundGenerator::setVolume()
    {
        const qreal logVolume = GetLogarithmicVolume();
        const qreal linVolume = QAudio::convertVolume(logVolume, QAudio::LogarithmicVolumeScale, QAudio::LinearVolumeScale);
        myAudioOutput->setVolume(linVolume);
    }

    void DirectSoundGenerator::start()
    {
        if (isRunning())
        {
            return;
        }

        DWORD dwStatus;
        GetStatus(&dwStatus);
        if (!(dwStatus & DSBSTATUS_PLAYING))
        {
            return;
        }

        if (!myAudioOutput)
        {
            initialise();
        }

        // restart as we are either starting or recovering from underrun
        myDevice = myAudioOutput->start();

        if (!myDevice)
        {
            return;
        }

        qDebug(appleAudio) << "Restarting the AudioGenerator";

        setVolume();

        const int bytesSize = myAudioOutput->bufferSize();
        const qint32 frameSize = myAudioFormat.framesForBytes(bytesSize);

        const qint32 framePeriod = myAudioFormat.framesForBytes(myAudioOutput->periodSize());
        qDebug(appleAudio) << "AudioOutput: size =" << frameSize << "f, period =" << framePeriod << "f";
        writeEnoughSilence(myInitialSilence); // ms
    }

    void DirectSoundGenerator::stop()
    {
        if (!isRunning())
        {
            return;
        }

        const qint32 bytesFree = myAudioOutput->bytesFree();

        // fill with zeros and stop
        std::vector<char> silence(bytesFree);
        myDevice->write(silence.data(), silence.size());

        const qint32 framesFree = myAudioFormat.framesForBytes(bytesFree);
        const qint64 duration = myAudioFormat.durationForFrames(framesFree);
        qDebug(appleAudio) << "Stopping with silence: frames =" << framesFree << ", duration =" << duration / 1000 << "ms";
        myAudioOutput->stop();
    }

    void DirectSoundGenerator::writeEnoughSilence(const qint64 ms)
    {
        // write a few ms of silence
        const qint32 framesSilence = myAudioFormat.framesForDuration(ms * 1000);  // target frames to write

        const qint32 bytesFree = myAudioOutput->bytesFree();
        const qint32 framesFree = myAudioFormat.framesForBytes(bytesFree);  // number of frames avilable to write
        const qint32 framesToWrite = std::min(framesFree, framesSilence);
        const qint64 bytesToWrite = myAudioFormat.bytesForFrames(framesToWrite);

        std::vector<char> silence(bytesToWrite);
        myDevice->write(silence.data(), silence.size());

        const qint64 duration = myAudioFormat.durationForFrames(framesToWrite);
        qDebug(appleAudio) << "Written some silence: frames =" << framesToWrite << ", duration =" << duration / 1000 << "ms";
    }

    void DirectSoundGenerator::writeAudio()
    {
        if (!isRunning())
        {
            return;
        }

        // we write all we have available (up to the free bytes)
        const DWORD bytesFree = myAudioOutput->bytesFree();

        LPVOID lpvAudioPtr1, lpvAudioPtr2;
        DWORD dwAudioBytes1, dwAudioBytes2;
        // this function reads as much as possible up to bytesFree
        Read(bytesFree, &lpvAudioPtr1, &dwAudioBytes1, &lpvAudioPtr2, &dwAudioBytes2);

        qint64 bytesWritten = 0;
        qint64 bytesToWrite = 0;
        if (lpvAudioPtr1)
        {
            bytesWritten += myDevice->write((char *)lpvAudioPtr1, dwAudioBytes1);
            bytesToWrite += dwAudioBytes1;
        }
        if (lpvAudioPtr2)
        {
            bytesWritten += myDevice->write((char *)lpvAudioPtr2, dwAudioBytes2);
            bytesToWrite += dwAudioBytes2;
        }

        if (bytesToWrite != bytesWritten)
        {
            qDebug(appleAudio) << "Mismatch:" << bytesToWrite << "!=" << bytesWritten;
        }
    }
}

IDirectSoundBuffer * iCreateDirectSoundBuffer(LPCDSBUFFERDESC lpcDSBufferDesc)
{
    DirectSoundGenerator * generator = new DirectSoundGenerator(lpcDSBufferDesc);
    activeSoundGenerators[generator] = generator;
    return generator;
}

namespace QDirectSound
{

    void start()
    {
        for (auto & it : activeSoundGenerators)
        {
            const auto generator = it.second;
            generator->start();
        }
    }

    void stop()
    {
        for (auto & it : activeSoundGenerators)
        {
            const auto generator = it.second;
            generator->stop();
        }
    }

    void writeAudio()
    {
        for (auto & it : activeSoundGenerators)
        {
            const auto generator = it.second;
            generator->writeAudio();
        }
    }

    void setOptions(const qint32 initialSilence)
    {
        for (auto & it : activeSoundGenerators)
        {
            const auto generator = it.second;
            generator->setOptions(initialSilence);
        }
    }

}
