## SAPI-Bridge

## What are we trying to do?

- Create a SAPI engine
- No synth directly in the dll. Pass all speak calls - infact all calls for other methods to a pipe service (see VoiceServer)

So to be clear
- Have a pipe service running in the background which deals with all sapi calls
- A GUI "Register your voice" app (see VoiceServer/RegisterVoice)
- A SAPI Dll that is registered in windows which for all purposes to windows works as a sapi system but redirects to a pipe service

## Why like this? Why not build all on the dll?

Because some TTS systems we are using have really slow cold-start times to call each time. Take for example sherpa-onnx models. Its far faster to hold the client object in memory and next synth streaming calls are faster

## Are you sure? There are going to be loads of methods you cant support

For sure - we arent totally sure. Infact we can do lots to improve coldstart times - but we already have most of the service code working and a pretty decent tts-wrapper that allows us to do synth streaming calls across a wide range of speech engines that are on and offline. 

For all the methods - you are right - we are going to struggle to do things like wordEvents for all engines. But for some we already have this (https://github.com/willwade/tts-wrapper?tab=readme-ov-file#feature-set-overview)

## What was your code originally doing here

We built a simple proof of concept to build a SAPI dll and integrate it into python. Its a total PITA to build and - we figured why not just abstract this and remove the python headaches. Redirect to a pipe service..

**WE HAVENT MIGRATED AWAY FROM THIS DLL CODE MUCH - INFACT IT JUST DOESNT WORK**


** So what should this dll do?**

1. Register as a SAPI engine
2. When voices call this engine it redirects speak calls - and other callls it can support to a pipe service
3. We should safely pass on calls we cant support

** How could/should we test this?**

Use https://www.cross-plus-a.com/balabolka.htm and see if the voices get registered (they do now) but they dont speak



So what follows below is specific code details on the DLL part.. 


# Building

Use Visual Studio  to build the project. The project is a CMake project, so you can use CMake to generate the Visual Studio solution file
So the build process is as follows:

1. Download opensource jsoncpp.
2. Build jsoncpp.lib and jsoncpp.dll
3. Link jsoncpp.lib and dll to pysapittsengine project.
4. Then build the pysapittsengine.dll


```
cd engine
mkdir build
cd build
cmake ..
cmake --build . --config Debug
cmake --build . --config Release
```

# Registering engine (run as Administrator)
```
regsvr32.exe pysapittsengine.dll
```

# Registering voice (run as Administrator)
```
regvoice.exe --token PYTTS-AzureNeural --name "Azure Neural" --vendor Microsoft --path C:\Work\SAPI-POC;C:\Work\build\venv\Lib\site-packages --module voices --class AzureNeuralVoice
```

Or use the GUI to register voices.
See VoiceServer/README.md for more information.
