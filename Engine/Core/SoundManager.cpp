#include "SoundManager.h"

#include "../Components/AudioSource.h"
#include "../Components/AudioListener.h"
#include "../Components/Transform.h"
#include "../Core/GameObject.h"
#include "../Math/Mathf.h"

#include "../Core/Engine.h"
#include "Debug.h"

namespace GX
{
	SoundManager SoundManager::singleton;
	//BufferList SoundManager::Buffers;

	SoundManager::SoundManager()
	{
	}

	SoundManager::~SoundManager()
	{
	}

	//t800

	/*

	bool SoundManager::init()
	{
		ALfloat ListenerPos[] = { 0.0, 0.0, 0.0 };
		ALfloat ListenerVel[] = { 0.0, 0.0, 0.0 };
		ALfloat ListenerOri[] = { 0.0, 0.0, -1.0,  0.0, 1.0, 0.0 };

		ALboolean enumeration = alcIsExtensionPresent(NULL, "ALC_ENUMERATION_EXT");
		
		const ALCchar* devices = alcGetString(NULL, ALC_DEVICE_SPECIFIER);
		const ALCchar *device = devices, *next = devices + 1;
		size_t len = 0;

		std::vector<std::string> deviceList;

		while (device && *device != '\0' && next && *next != '\0')
		{
				deviceList.push_back(device);
				len = strlen(device);
				device += (len + 1);
				next += (len + 2);
		}

		const ALCchar* currentDevice = "";
		if (deviceList.size() > 0)
			currentDevice = deviceList[0].c_str();
			
		pDevice = alcOpenDevice(currentDevice);

		if (!pDevice)
		{
			Debug::logError("Default sound device not present");
			return false;
		}

		pContext = alcCreateContext(pDevice, NULL);

		if (checkALCError(pDevice)) return false;

		alcMakeContextCurrent(pContext);

		alListenerfv(AL_POSITION, ListenerPos);
		alListenerfv(AL_VELOCITY, ListenerVel);
		alListenerfv(AL_ORIENTATION, ListenerOri);
		
		if (listener != nullptr)
			alListenerf(AL_GAIN, listener->getVolume());

		alDistanceModel(AL_LINEAR_DISTANCE);

		if (!Engine::getSingleton()->getAssetsPath().empty())
			Debug::log("Sound initialized", Debug::DbgColorGreen);

		return true;
	}

	*/

	bool SoundManager::init()
	{
		Debug::log("Initializing SoundManager...", Debug::DbgColorYellow);

		ALfloat ListenerPos[] = { 0.0, 0.0, 0.0 };
		ALfloat ListenerVel[] = { 0.0, 0.0, 0.0 };
		ALfloat ListenerOri[] = { 0.0, 0.0, -1.0,  0.0, 1.0, 0.0 };

		Debug::log("Checking for ALC enumeration extension...");
		ALboolean enumeration = alcIsExtensionPresent(NULL, "ALC_ENUMERATION_EXT");
		if (enumeration == AL_FALSE)
		{
			Debug::log("Warning: ALC_ENUMERATION_EXT not supported", Debug::DbgColorYellow);
		}
		else
		{
			Debug::log("ALC_ENUMERATION_EXT is supported");
		}

		Debug::log("Retrieving sound devices...");
		const ALCchar* devices = alcGetString(NULL, ALC_DEVICE_SPECIFIER);
		if (!devices)
		{
			Debug::log("Error: No sound devices found", Debug::DbgColorRed);
			return false;
		}

		const ALCchar* device = devices;
		const ALCchar* next = devices + 1;
		size_t len = 0;
		std::vector<std::string> deviceList;

		while (device && *device != '\0' && next && *next != '\0')
		{
			deviceList.push_back(device);
			len = strlen(device);
			device += (len + 1);
			next += (len + 2);
		}

		Debug::log("Available sound devices:");
		for (const auto& dev : deviceList)
		{
			Debug::log("-- " + dev);
		}

		const ALCchar* currentDevice = "";
		if (!deviceList.empty())
		{
			currentDevice = deviceList[0].c_str();
			Debug::log("Using default sound device: " + std::string(currentDevice));
		}
		else
		{
			Debug::log("Warning: No default device found, trying to open default device", Debug::DbgColorYellow);
		}

		Debug::log("Opening sound device...");
		pDevice = alcOpenDevice(currentDevice);
		if (!pDevice)
		{
			Debug::log("Error: Failed to open default sound device", Debug::DbgColorRed);
			return false;
		}
		Debug::log("Sound device opened successfully");

		Debug::log("Creating sound context...");
		pContext = alcCreateContext(pDevice, NULL);
		if (!pContext || checkALCError(pDevice))
		{
			Debug::log("Error: Failed to create sound context", Debug::DbgColorRed);
			return false;
		}
		Debug::log("Sound context created successfully");

		Debug::log("Making context current...");
		if (!alcMakeContextCurrent(pContext))
		{
			Debug::log("Error: Failed to make context current", Debug::DbgColorRed);
			return false;
		}
		Debug::log("Context made current");

		Debug::log("Setting listener properties...");
		alListenerfv(AL_POSITION, ListenerPos);
		alListenerfv(AL_VELOCITY, ListenerVel);
		alListenerfv(AL_ORIENTATION, ListenerOri);

		if (listener != nullptr)
		{
			Debug::log("Setting listener volume...");
			alListenerf(AL_GAIN, listener->getVolume());
			Debug::log("Listener volume set to: " + std::to_string(listener->getVolume()));
		}

		Debug::log("Setting distance model...");
		alDistanceModel(AL_LINEAR_DISTANCE);
		Debug::log("Distance model set to AL_LINEAR_DISTANCE");

		if (!Engine::getSingleton()->getAssetsPath().empty())
		{
			Debug::log("SoundManager initialized successfully", Debug::DbgColorGreen);
		}
		else
		{
			Debug::log("Warning: Engine assets path is empty", Debug::DbgColorYellow);
		}

		return true;
	}


	void SoundManager::update()
	{
		if (listener != nullptr)
		{
			Transform * listenerTransform = listener->getGameObject()->getTransform();

			ALfloat ListenerPos[] = {
				listenerTransform->getPosition().x,
				listenerTransform->getPosition().y,
				listenerTransform->getPosition().z
			};

			glm::vec3 rot = listenerTransform->getForward();
			glm::vec3 up = listenerTransform->getUp();
			
			ALfloat ListenerOri[] = {
				rot.x,
				rot.y,
				rot.z,
				up.x,
				up.y,
				up.z
			};

			alListenerfv(AL_POSITION, ListenerPos);
			alListenerfv(AL_ORIENTATION, ListenerOri);
		}

		for (std::vector<AudioSource*>::iterator it = sources.begin(); it != sources.end(); ++it)
		{
			(*it)->update();
		}
	}

	void SoundManager::deleteSource(AudioSource* src)
	{
		auto it = std::find(sources.begin(), sources.end(), src);

		if (it != sources.end())
			sources.erase(it);
	}

	void SoundManager::destroy()
	{
		alcMakeContextCurrent(NULL);
		if (pContext != nullptr)
			alcDestroyContext(pContext);

		if (pDevice != nullptr)
			alcCloseDevice(pDevice);

		pDevice = nullptr;
		pContext = nullptr;
	}

	void SoundManager::setMasterVolume(float volume)
	{
		alListenerf(AL_GAIN, volume);
	}

	ALboolean SoundManager::checkALCError(ALCdevice * pDevice)
	{
		ALenum ErrCode;
		std::string Err = "ALC error: ";
		if ((ErrCode = alcGetError(pDevice)) != ALC_NO_ERROR)
		{
			Err += (char *)alcGetString(pDevice, ErrCode);
			Debug::logError(Err.data());
			return AL_TRUE;
		}
		return AL_FALSE;
	}

	ALboolean SoundManager::checkALError()
	{
		ALenum ErrCode;
		std::string Err = "OpenAL error: ";
		if ((ErrCode = alGetError()) != AL_NO_ERROR)
		{
			Err += (char *)alGetString(ErrCode);
			Debug::logError(Err.data());
			return AL_TRUE;
		}

		return AL_FALSE;
	}

	void SoundManager::setPaused(bool value)
	{
		if (paused != value)
		{
			paused = value;

			if (paused)
			{
				for (auto it = sources.begin(); it != sources.end(); ++it)
				{
					AudioSource* src = *it;
					src->pauseGlobal();
				}
			}
			else
			{
				for (auto it = sources.begin(); it != sources.end(); ++it)
				{
					AudioSource* src = *it;
					src->resumeGlobal();
				}
			}
		}
	}
}