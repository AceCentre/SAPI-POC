# Building
```
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
