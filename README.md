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