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
regvoice.exe --token PYTTS_JessaNeural --name "Jessa Neural" --vendor Microsoft --path C:\Work\PySAPI\voices --module pysapitts --class JessaNeuralVoice
```
