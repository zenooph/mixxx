/***************************************************************************
                          soundmanager.cpp
                             -------------------
    begin                : Sun Aug 15, 2007
    copyright            : (C) 2007 Albert Santoni
    email                : gamegod \a\t users.sf.net
***************************************************************************/

/***************************************************************************
*                                                                         *
*   This program is free software; you can redistribute it and/or modify  *
*   it under the terms of the GNU General Public License as published by  *
*   the Free Software Foundation; either version 2 of the License, or     *
*   (at your option) any later version.                                   *
*                                                                         *
***************************************************************************/

#include <QtDebug>
#include <QtCore>
#include <portaudio.h>
#include <cstring> // for memcpy and strcmp
#include "soundmanager.h"
#include "sounddevice.h"
#include "sounddeviceportaudio.h"
#include "engine/enginemaster.h"
#include "controlobjectthreadmain.h"
#include "soundmanagerutil.h"

#ifdef __VINYLCONTROL__
#include "vinylcontrolxwax.h"
#endif

/** Initializes Mixxx's audio core
 *  @param pConfig The config key table
 *  @param _master A pointer to the audio engine's mastering class.
 */
SoundManager::SoundManager(ConfigObject<ConfigValue> * pConfig, EngineMaster * _master)
    : QObject()
    , m_pErrorDevice(NULL)
#ifdef __PORTAUDIO__
    , m_paInitialized(false)
    , m_jackSampleRate(-1)
#endif
{
    //qDebug() << "SoundManager::SoundManager()";
    m_pConfig = pConfig;
    m_pMaster = _master;

    clearOperativeVariables();

    //These are ControlObjectThreadMains because all the code that
    //uses them is called from the GUI thread (stuff like opening soundcards).
    ControlObjectThreadMain* pControlObjectLatency = new ControlObjectThreadMain(ControlObject::getControl(ConfigKey("[Master]", "latency")));
    ControlObjectThreadMain* pControlObjectSampleRate = new ControlObjectThreadMain(ControlObject::getControl(ConfigKey("[Master]", "samplerate")));
    m_pControlObjectInputPassthrough1 = new ControlObjectThreadMain(ControlObject::getControl(ConfigKey("[Channel1]", "inputpassthrough")));
    m_pControlObjectInputPassthrough2 = new ControlObjectThreadMain(ControlObject::getControl(ConfigKey("[Channel2]", "inputpassthrough")));
    m_pControlObjectVinylStatus1 = new ControlObjectThreadMain(ControlObject::getControl(ConfigKey("[Channel1]", "vinylcontrol_status")));
    m_pControlObjectVinylStatus2 = new ControlObjectThreadMain(ControlObject::getControl(ConfigKey("[Channel2]", "vinylcontrol_status")));
    m_bPassthroughActive[0] = false;
    m_bPassthroughActive[1] = false;
    
    connect(m_pControlObjectInputPassthrough1, SIGNAL(valueChanged(double)),
            this, SLOT(slotInputPassthrough1(double)),
            Qt::DirectConnection);
            
	connect(m_pControlObjectInputPassthrough2, SIGNAL(valueChanged(double)),
            this, SLOT(slotInputPassthrough2(double)),
            Qt::DirectConnection);            	

    ControlObjectThreadMain* pControlObjectVinylControlMode = new ControlObjectThreadMain(new ControlObject(ConfigKey("[VinylControl]", "mode")));
    ControlObjectThreadMain* pControlObjectVinylControlMode1 = new ControlObjectThreadMain(ControlObject::getControl(ConfigKey("[Channel1]", "vinylcontrol_mode")));
    ControlObjectThreadMain* pControlObjectVinylControlMode2 = new ControlObjectThreadMain(ControlObject::getControl(ConfigKey("[Channel2]", "vinylcontrol_mode")));
    ControlObjectThreadMain* pControlObjectVinylControlGain = new ControlObjectThreadMain(new ControlObject(ConfigKey("[VinylControl]", "gain")));

    //Hack because PortAudio samplerate enumeration is slow as hell on Linux (ALSA dmix sucks, so we can't blame PortAudio)
    m_samplerates.push_back(44100);
    m_samplerates.push_back(48000);
    m_samplerates.push_back(96000);

    queryDevices(); // initializes PortAudio so SMConfig:loadDefaults can do
                    // its thing if it needs to

    if (!m_config.readFromDisk()) {
        m_config.loadDefaults(this, SoundManagerConfig::ALL);
    }
    checkConfig();
    m_config.writeToDisk(); // in case anything changed by applying defaults

    // TODO(bkgood) do these really need to be here? they're set in
    // SoundDevicePortAudio::open
    pControlObjectLatency->slotSet(m_config.getFramesPerBuffer() / m_config.getSampleRate() * 1000);
    pControlObjectSampleRate->slotSet(m_config.getSampleRate());
}

/** Destructor for the SoundManager class. Closes all the devices, cleans up their pointers
  and terminates PortAudio. */
SoundManager::~SoundManager()
{
    //Clean up devices.
    clearDeviceList();

    if (m_paInitialized) {
        Pa_Terminate();
        m_paInitialized = false;
    }
    // vinyl control proxies and input buffers are freed in closeDevices, called
    // by clearDeviceList -- bkgood
}

/**
 * Clears all variables used in operation.
 * @note This is in a function because it's done in a few places
 */
void SoundManager::clearOperativeVariables()
{
    iNumDevicesOpenedForOutput = 0;
    iNumDevicesOpenedForInput = 0;
    m_pClkRefDevice = NULL;
}

/**
 * Returns a pointer to the EngineMaster instance this SoundManager
 * is using.
 * @note pointer is const at this point because this is only being inserted
 *       so that the prefs can find out how many channels there are and I
 *       feel uneasy about providing mutable access to the engine. Make it
 *       non-const if you end up needing a non-const pointer to the engine
 *       where you only have SoundMan.
 */
const EngineMaster* SoundManager::getEngine() const
{
    return m_pMaster;
}

/** Returns a list of all the devices we've enumerated through PortAudio.
  *
  * @param filterAPI If filterAPI is the name of an audio API used by PortAudio, this function
  * will only return devices that belong to that API. Otherwise, the list will
  * contain all devices on all PortAudio-supported APIs.
  * @param bOutputDevices If bOutputDevices is true, then devices
  *                       supporting audio output will be listed.
  * @param bInputDevices If bInputDevices is true, then devices supporting
  *                      audio input will be listed too.
  */
QList<SoundDevice*> SoundManager::getDeviceList(QString filterAPI, bool bOutputDevices, bool bInputDevices)
{
    //qDebug() << "SoundManager::getDeviceList";
    bool bMatchedCriteria = true;   //Whether or not the current device matched the filtering criteria

    if (m_devices.empty())
        this->queryDevices();

    if (filterAPI == "None")
    {
        QList<SoundDevice*> emptyList;
        return emptyList;
    }
    else
    {
        //Create a list of sound devices filtered to match given API and input/output .
        QList<SoundDevice*> filteredDeviceList;
        QListIterator<SoundDevice*> dev_it(m_devices);
        while (dev_it.hasNext())
        {
            bMatchedCriteria = true;                //Reset this for the next device.
            SoundDevice *device = dev_it.next();
            if (device->getHostAPI() != filterAPI)
                bMatchedCriteria = false;
            if (bOutputDevices)
            {
                 if (device->getNumOutputChannels() <= 0)
                    bMatchedCriteria = false;
            }
            if (bInputDevices)
            {
                if (device->getNumInputChannels() <= 1) //Ignore mono input and no-input devices
                    bMatchedCriteria = false;
            }

            if (bMatchedCriteria)
                filteredDeviceList.push_back(device);
        }
        return filteredDeviceList;
    }

    return m_devices;
}

/** Get a list of host APIs supported by PortAudio.
 *  @return The list of audio APIs supported on the current computer.
 */
QList<QString> SoundManager::getHostAPIList() const
{
    QList<QString> apiList;

    for (PaHostApiIndex i = 0; i < Pa_GetHostApiCount(); i++)
    {
        const PaHostApiInfo *api = Pa_GetHostApiInfo(i);
        if (api) {
            if (QString(api->name) != "skeleton implementation") apiList.push_back(api->name);
        }
    }

    return apiList;
}

/** Closes all the open sound devices.
 *
 *  Because multiple soundcards might be open, this member function
 *  simply runs through the list of all known soundcards (from PortAudio)
 *  and attempts to close them all. Closing a soundcard that isn't open
 *  is safe.
 */
void SoundManager::closeDevices()
{
    //qDebug() << "SoundManager::closeDevices()";
    QListIterator<SoundDevice*> dev_it(m_devices);

    //requestBufferMutex.lock(); //Ensures we don't kill a stream in the middle of a callback call.
                                 //Note: if we're using Pa_StopStream() (like now), we don't need
                                 //      to lock. PortAudio stops the threads nicely.
    while (dev_it.hasNext())
    {
        //qDebug() << "closing a device...";
        dev_it.next()->close();
    }
    //requestBufferMutex.unlock();

    //requestBufferMutex.lock();
    clearOperativeVariables();
    //requestBufferMutex.unlock();

    m_outputBuffers.clear(); // anti-cruft (safe because outputs only have
                             // pointers to memory owned by EngineMaster)

    foreach (AudioInput in, m_inputBuffers.keys()) {
        // Need to tell all registered AudioDestinations for this AudioInput
        // that the input was disconnected.
        if (m_registeredDestinations.contains(in)) {
            m_registeredDestinations[in]->onInputDisconnected(in);
        }

        short *buffer = m_inputBuffers[in];
        if (buffer != NULL) {
            delete [] buffer;
            m_inputBuffers[in] = buffer = NULL;
        }
    }
    m_inputBuffers.clear();

#ifdef __VINYLCONTROL__
    // TODO(bkgood) see comment where these objects are created in setupDevices,
    // this should probably be in the dtor or at least somewhere other
    // than here.
    while (!m_vinylControl.empty()) {
        VinylControlProxy *vc = m_vinylControl.takeLast();
        if (vc != NULL) {
            delete vc;
        }
        //xwax has a global LUT that we need to free after we've shut down our
        //vinyl control threads because it's not thread-safe.
        VinylControlXwax::freeLUTs();
    }
#endif
}

/** Closes all the devices and empties the list of devices we have. */
void SoundManager::clearDeviceList()
{
    //qDebug() << "SoundManager::clearDeviceList()";

    //Close the devices first.
    closeDevices();

    //Empty out the list of devices we currently have.
    while (!m_devices.empty())
    {
        SoundDevice* dev = m_devices.takeLast();
        delete dev;
    }

#ifdef __PORTAUDIO__
    if (m_paInitialized) {
        Pa_Terminate();
        m_paInitialized = false;
    }
#endif
}

/** Returns a list of samplerates we will attempt to support for a given API.
 *  @param API a string describing the API, some APIs support a more limited
 *             subset of APIs (for instance, JACK)
 *  @return The list of available samplerates.
 */
QList<unsigned int> SoundManager::getSampleRates(QString api) const
{
#ifdef __PORTAUDIO__
    if (api == MIXXX_PORTAUDIO_JACK_STRING) {
        // queryDevices must have been called for this to work, but the
        // ctor calls it -bkgood
        QList<unsigned int> samplerates;
        samplerates.append(m_jackSampleRate);
        return samplerates;
    }
#endif
    return m_samplerates;
}

/**
 * Convenience overload for SoundManager::getSampleRates(QString)
 */
QList<unsigned int> SoundManager::getSampleRates() const
{
    return getSampleRates("");
}

//Creates a list of sound devices that PortAudio sees.
void SoundManager::queryDevices()
{
    //qDebug() << "SoundManager::queryDevices()";
    clearDeviceList();

#ifdef __PORTAUDIO__
    PaError err = paNoError;
    if (!m_paInitialized) {
        err = Pa_Initialize();
        m_paInitialized = true;
    }
    if (err != paNoError)
    {
        qDebug() << "Error:" << Pa_GetErrorText(err);
        m_paInitialized = false;
        return;
    }

    int iNumDevices;
    iNumDevices = Pa_GetDeviceCount();
    if(iNumDevices < 0)
    {
        qDebug() << "ERROR: Pa_CountDevices returned" << iNumDevices;
        return;
    }

    const PaDeviceInfo* deviceInfo;
    for (int i = 0; i < iNumDevices; i++)
    {
        deviceInfo = Pa_GetDeviceInfo(i);
        if (!deviceInfo)
            continue;
        /* deviceInfo fields for quick reference:
            int 	structVersion
            const char * 	name
            PaHostApiIndex 	hostApi
            int 	maxInputChannels
            int 	maxOutputChannels
            PaTime 	defaultLowInputLatency
            PaTime 	defaultLowOutputLatency
            PaTime 	defaultHighInputLatency
            PaTime 	defaultHighOutputLatency
            double 	defaultSampleRate
         */
        SoundDevicePortAudio *currentDevice = new SoundDevicePortAudio(m_pConfig, this, deviceInfo, i);
        m_devices.push_back(currentDevice);
        if (!strcmp(Pa_GetHostApiInfo(deviceInfo->hostApi)->name,
                    MIXXX_PORTAUDIO_JACK_STRING)) {
            m_jackSampleRate = deviceInfo->defaultSampleRate;
        }
    }
#endif
    // now tell the prefs that we updated the device list -- bkgood
    emit(devicesUpdated());
}

//Opens all the devices chosen by the user in the preferences dialog, and establishes
//the proper connections between them and the mixing engine.
int SoundManager::setupDevices()
{
    qDebug() << "SoundManager::setupDevices()";
    int err = 0;
    clearOperativeVariables();
    int devicesAttempted = 0;
    int devicesOpened = 0;

    // filter out any devices in the config we don't actually have
    m_config.filterOutputs(this);
    m_config.filterInputs(this);

    // close open devices, close running vinyl control proxies
    closeDevices();
#ifdef __VINYLCONTROL__
    //Initialize vinyl control
    // TODO(bkgood) this ought to be done in the ctor or something. Not here. Really
    // shouldn't be any reason for these to be reinitialized every time the
    // audio prefs are updated. Will require work in DlgPrefVinyl.
    m_vinylControl.append(new VinylControlProxy(m_pConfig, "[Channel1]"));
    m_vinylControl.append(new VinylControlProxy(m_pConfig, "[Channel2]"));
    qDebug() << "Created VinylControlProxies" <<
                m_vinylControl[0] << m_vinylControl[1];
    registerInput(AudioInput(AudioInput::VINYLCONTROL, 0, 0), m_vinylControl[0]);
    registerInput(AudioInput(AudioInput::VINYLCONTROL, 0, 1), m_vinylControl[1]);
#endif
    foreach (SoundDevice *device, m_devices) {
        bool isInput = false;
        bool isOutput = false;
        device->clearInputs();
        device->clearOutputs();
        m_pErrorDevice = device;
        foreach (AudioInput in, m_config.getInputs().values(device->getInternalName())) {
            isInput = true;
            err = device->addInput(in);
            if (err != OK) return err;
            if (!m_inputBuffers.contains(in)) {
                // TODO(bkgood) look into allocating this with the frames per
                // buffer value from SMConfig
                m_inputBuffers[in] = new short[MAX_BUFFER_LEN];
            }

            // Check if any AudioDestination is registered for this AudioInput,
            // and call the onInputConnected method.
            if (m_registeredDestinations.contains(in)) {
                m_registeredDestinations[in]->onInputConnected(in);
            }
        }
        foreach (AudioOutput out, m_config.getOutputs().values(device->getInternalName())) {
            isOutput = true;
            // following keeps us from asking for a channel buffer EngineMaster
            // doesn't have -- bkgood
            if (m_registeredSources[out]->buffer(out) == NULL) {
                qDebug() << "AudioSource returned null for" << out.getString();
                continue;
            }
            err = device->addOutput(out);
            if (err != OK)
                return err;
            m_outputBuffers[out] = m_registeredSources[out]->buffer(out);
            if (out.getType() == AudioOutput::MASTER) {
                m_pClkRefDevice = device;
            } else if (out.getType() == AudioOutput::DECK
                    && !m_pClkRefDevice) {
                m_pClkRefDevice = device;
            }
        }
        if (isInput || isOutput) {
            device->setSampleRate(m_config.getSampleRate());
            device->setFramesPerBuffer(m_config.getFramesPerBuffer());
            ++devicesAttempted;
            err = device->open();
            if (err != OK) {
                return err;
            } else {
                ++devicesOpened;
                if (isOutput)
                    ++iNumDevicesOpenedForOutput;
                if (isInput)
                    ++iNumDevicesOpenedForInput;
            }
        }
    }

    if (!m_pClkRefDevice) {
        QList<SoundDevice*> outputDevices = getDeviceList(m_config.getAPI(), true, false);
        SoundDevice* device = outputDevices.first();
        qWarning() << "Output sound device clock reference not set! Using" << device->getDisplayName();
        m_pClkRefDevice = device;
    }
    else qDebug() << "Using" << m_pClkRefDevice->getDisplayName() << "as output sound device clock reference";

    qDebug() << iNumDevicesOpenedForOutput << "output sound devices opened";
    qDebug() << iNumDevicesOpenedForInput << "input sound devices opened";

    // returns OK if we were able to open all the devices the user
    // wanted
    if (devicesAttempted == devicesOpened) {
        emit(devicesSetup());
        return OK;
    }
    m_pErrorDevice = NULL;
    return ERR;
}

SoundDevice* SoundManager::getErrorDevice() const {
    return m_pErrorDevice;
}

SoundManagerConfig SoundManager::getConfig() const {
    return m_config;
}

#ifdef __VINYLCONTROL__
bool SoundManager::hasVinylInput(int deck)
{
    if (deck >= m_vinylControl.length())
        return false;
    VinylControlProxy* vinyl_control = m_vinylControl[deck];

    bool hasInput = false;
    QList<AudioInput> inputs = getConfig().getInputs().values();
    foreach (AudioInput in, inputs) {
        if (in.getType() == AudioInput::VINYLCONTROL
                && in.getIndex() == deck) {
            hasInput = true;
            break;
        }
    }

    return vinyl_control != NULL && hasInput;
}

QList<VinylControlProxy*> SoundManager::getVinylControlProxies()
{
    return m_vinylControl;
}
#endif

int SoundManager::setConfig(SoundManagerConfig config) {
    int err = OK;
    m_config = config;
    checkConfig();

    // certain parts of mixxx rely on this being here, for the time being, just
    // letting those be -- bkgood
    // Do this first so vinyl control gets the right samplerate -- Owen W.
    m_pConfig->set(ConfigKey("[Soundcard]","Samplerate"), ConfigValue(m_config.getSampleRate()));

    err = setupDevices();
    if (err == OK) {
        m_config.writeToDisk();
    }
    return err;
}

void SoundManager::checkConfig() {
    if (!m_config.checkAPI(*this)) {
        m_config.setAPI(DEFAULT_API);
        m_config.loadDefaults(this, SoundManagerConfig::API | SoundManagerConfig::DEVICES);
    }
    if (!m_config.checkSampleRate(*this)) {
        m_config.setSampleRate(DEFAULT_SAMPLE_RATE);
        m_config.loadDefaults(this, SoundManagerConfig::OTHER);
    }
    // latency checks itself for validity on SMConfig::setLatency()
}

void SoundManager::sync()
{
    ControlObject::sync();
    //qDebug() << "sync";

}

void SoundManager::slotInputPassthrough1(double toggle)
{
	if (m_bPassthroughActive[0] != (bool)toggle)
		m_bPassthroughActive[0] = (bool)toggle;
	if ((bool)toggle)
	{
		//iterate through inputs.  if none for deck 0, then toggle it back again
		//this is separate from hasvinylinput because it has to work even if
		//vinyl support is not compiled
		foreach (AudioInput in, m_inputBuffers.keys())
		{
			if (in.getIndex() == 0)
			{
				m_pControlObjectVinylStatus1->slotSet(VINYL_STATUS_PASSTHROUGH);
				return;
			}
		}
		//didn't find itm_pControlObjectVinylStatus1 = new ControlObjectThreadMain(ControlObject::getControl(ConfigKey("[Channel1]", "VinylStatus")));
		m_pControlObjectInputPassthrough1->slotSet(false);
		
	}
}

void SoundManager::slotInputPassthrough2(double toggle)
{
	if (m_bPassthroughActive[1] != (bool)toggle)
		m_bPassthroughActive[1] = (bool)toggle;
	if (toggle)
	{
		//iterate through inputs.  if none for deck 0, then toggle it back again
		//this is separate from hasvinylinput because it has to work even if
		//vinyl support is not compiled
		foreach (AudioInput in, m_inputBuffers.keys())
		{
			if (in.getIndex() == 1)
			{
				m_pControlObjectVinylStatus2->slotSet(VINYL_STATUS_PASSTHROUGH);
				return;
			}
		}
		//didn't find it
		m_pControlObjectInputPassthrough2->slotSet(false);
	}
}

//Requests a buffer in the proper format, if we're prepared to give one.
QHash<AudioOutput, const CSAMPLE*>
SoundManager::requestBuffer(QList<AudioOutput> outputs, unsigned long iFramesPerBuffer, SoundDevice* device, double streamTime)
{
    Q_UNUSED(outputs); // unused, we just give the caller the full hash -bkgood
    //qDebug() << "SoundManager::requestBuffer()";

    /*
    // Display when sound cards drop or duplicate buffers (use for testing only)
    if (iNumDevicesOpenedForOutput>1) {
        // Running total of requested frames
        long currentFrameCount = 0;
        if (m_deviceFrameCount.contains(device)) currentFrameCount=m_deviceFrameCount.value(device);
        m_deviceFrameCount.insert(device, currentFrameCount+iFramesPerBuffer);  // Overwrites existing value if already present
        // Get current time in milliseconds
//         uint t = QDateTime::currentDateTime().toTime_t()*1000+QDateTime::currentDateTime().toString("zzz").toUint();

        if (device != m_pClkRefDevice) {  // If not the reference device,
            // Detect dropped frames/buffers
            long sdifference = m_deviceFrameCount.value(m_pClkRefDevice)-m_deviceFrameCount.value(device);
            QString message = "dropped";
            if (sdifference < 0) message = "duplicated";
            if (sdifference != 0) {
                m_deviceFrameCount.clear();
                message = QString("%1 %2 %3 frames (%4 buffers)")
                            .arg(device->getDisplayName())
                            .arg(message)
                            .arg(fabs(sdifference))
                            .arg(fabs(sdifference)/iFramesPerBuffer);
                qWarning() << message;
            }
        }
    }
    //  End dropped/duped buffer display
    */

    //When the clock reference device requests a buffer...
    if (device == m_pClkRefDevice && requestBufferMutex.tryLock())
    {
        // Only generate a new buffer for the clock reference card
//         qDebug() << "New buffer for" << device->getDisplayName() << "of size" << iFramesPerBuffer;
        //First, sync control parameters with changes from GUI thread
        sync();

        //Process a block of samples for output. iFramesPerBuffer is the
        //number of samples for one channel, but the EngineObject
        //architecture expects number of samples for two channels
        //as input (buffer size) so...
        m_pMaster->process(0, 0, iFramesPerBuffer*2);

        requestBufferMutex.unlock();
    }
    return m_outputBuffers;
}

//Used by SoundDevices to "push" any audio from their inputs that they have into the mixing engine.
void SoundManager::pushBuffer(QList<AudioInput> inputs, short * inputBuffer,
                              unsigned long iFramesPerBuffer, unsigned int iFrameSize)
{
    //This function is called a *lot* and is a big source of CPU usage.
    //It needs to be very fast.

//    m_inputBuffers[RECEIVER_VINYLCONTROL_ONE]

    //short vinylControlBuffer1[iFramesPerBuffer * 2];
    //short vinylControlBuffer2[iFramesPerBuffer * 2];
    //short *vinylControlBuffer1 = (short*) alloca(iFramesPerBuffer * 2 * sizeof(short));
    //short *vinylControlBuffer2 = (short*) alloca(iFramesPerBuffer * 2 * sizeof(short));

    //memset(vinylControlBuffer1, 0, iFramesPerBuffer * iFrameSize * sizeof(*vinylControlBuffer1));

    // IMPORTANT -- Mixxx should ALWAYS be the owner of whatever input buffer we're using,
    // otherwise we double-free (well, PortAudio frees and then we free) and everything
    // goes to hell -- bkgood

    /** If the framesize is only 2, then we only have one pair of input channels
     *  That means we don't have to do any deinterlacing, and we can pass
     *  the audio on to its intended destination. */
    // this special casing is probably not worth keeping around. It had a speed
    // advantage before because it just assigned a pointer instead of copying data,
    // but this meant we couldn't free all the receiver buffer pointers, because some
    // of them might potentially be owned by portaudio. Not freeing them means we leak
    // memory in certain cases -- bkgood
    if (iFrameSize == 2)
    {
        for (QList<AudioInput>::const_iterator i = inputs.begin(),
                     e = inputs.end(); i != e; ++i) {
            const AudioInput& in = *i;
            memcpy(m_inputBuffers[in], inputBuffer,
                   sizeof(*inputBuffer) * iFrameSize * iFramesPerBuffer);
        }
    }

/*
    //If we have two stereo input streams (interlaced as one), then
    //break them up into two separate interlaced streams
    if (iFrameSize == 4)
    {
        for (int i = 0; i < iFramesPerBuffer; i++) //For each frame of audio
        {
            m_inputBuffers[RECEIVER_VINYLCONTROL_ONE][i*2    ] = inputBuffer[i*iFrameSize    ];
            m_inputBuffers[RECEIVER_VINYLCONTROL_ONE][i*2 + 1] = inputBuffer[i*iFrameSize + 1];
            m_inputBuffers[RECEIVER_VINYLCONTROL_TWO][i*2    ] = inputBuffer[i*iFrameSize + 2];
            m_inputBuffers[RECEIVER_VINYLCONTROL_TWO][i*2 + 1] = inputBuffer[i*iFrameSize + 3];
        }
        //Set the pointers to point to the de-interlaced input audio
        vinylControlBuffer1 = m_inputBuffers[RECEIVER_VINYLCONTROL_ONE];
        vinylControlBuffer2 = m_inputBuffers[RECEIVER_VINYLCONTROL_TWO];
    }
*/
    else { //More than two channels of input (iFrameSize > 2)

        // Do crazy deinterleaving of the audio into the correct m_inputBuffers.

        for (QList<AudioInput>::const_iterator i = inputs.begin(),
                     e = inputs.end(); i != e; ++i) {
            const AudioInput& in = *i;
            short* pInputBuffer = m_inputBuffers[in];
            ChannelGroup chanGroup = in.getChannelGroup();
            int iChannelCount = chanGroup.getChannelCount();
            int iChannelBase = chanGroup.getChannelBase();

            for (unsigned int iFrameNo = 0; iFrameNo < iFramesPerBuffer; ++iFrameNo) {
                // iFrameBase is the "base sample" in a frame (ie. the first
                // sample in a frame)
                unsigned int iFrameBase = iFrameNo * iFrameSize;
                unsigned int iLocalFrameBase = iFrameNo * iChannelCount;

                // this will make sure a sample from each channel is copied
                for (int iChannel = 0; iChannel < iChannelCount; ++iChannel) {
                    //output[iFrameBase + src.channelBase + iChannel] +=
                    //  outputAudio[src.type][iLocalFrameBase + iChannel] * SHRT_CONVERSION_FACTOR;

                    pInputBuffer[iLocalFrameBase + iChannel] =
                            inputBuffer[iFrameBase + iChannelBase + iChannel];
                }
            }
        }
    }

    if (inputBuffer)
    {
        for (QList<AudioInput>::ConstIterator i = inputs.begin(),
                     e = inputs.end(); i != e; ++i) {
            const AudioInput& in = *i;

            // Sanity check.
            if (!m_inputBuffers.contains(in)) {
                continue;
            }

            short* pInputBuffer = m_inputBuffers[in];
            
            if (in.getIndex() >=0 && in.getIndex() < 2)
            	if (m_bPassthroughActive[in.getIndex()])
					m_pMaster->pushPassthroughBuffer(in.getIndex(), m_inputBuffers[in], iFrameSize * iFramesPerBuffer);

            if (m_registeredDestinations.contains(in)) {
                AudioDestination* destination = m_registeredDestinations[in];
                if (destination) {
                    destination->receiveBuffer(in, pInputBuffer, iFramesPerBuffer);
                }
            }
        }
    }
}

void SoundManager::registerOutput(AudioOutput output, const AudioSource *src) {
    if (m_registeredSources.contains(output)) {
        qDebug() << "WARNING: AudioOutput already registered!";
    }
    m_registeredSources[output] = src;
    emit(outputRegistered(output, src));
}

void SoundManager::registerInput(AudioInput input, AudioDestination *dest) {
    if (m_registeredDestinations.contains(input)) {
        // note that this can be totally ok if we just want a certain
        // AudioInput to be going to a different AudioDest -bkgood
        qDebug() << "WARNING: AudioInput already registered!";
    }
    m_registeredDestinations[input] = dest;
    emit(inputRegistered(input, dest));
}

QList<AudioOutput> SoundManager::registeredOutputs() const {
    return m_registeredSources.keys();
}

QList<AudioInput> SoundManager::registeredInputs() const {
    return m_registeredDestinations.keys();
}
