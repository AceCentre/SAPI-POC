# Building

You need cmake. So if you have chocolatey, you can install it with the following command:

```
choco install cmake
```

or you can download it from [here](https://cmake.org/download/).

So the build process is as follows:

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