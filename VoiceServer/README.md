
# Voice Engine Registration and TTS Pipe Service

This project provides a **pipe service** that allows users to:
- Get available TTS engines.
- List available voices for each engine.
- Speak text using a selected voice engine.
- Register selected voices from the available engines.

Additionally, a **simple GUI** is included to:
- Select a voice engine.
- Choose multiple voices from that engine.
- Register the selected voices using the pipe service.

## Table of Contents
1. [Features](#features)
2. [Requirements](#requirements)
3. [Installation](#installation)
4. [Usage](#usage)
5. [Pipe Service](#pipe-service)
6. [GUI Application](#gui-application)
7. [Contributing](#contributing)
8. [License](#license)

## Features

- **Pipe Service**: A Windows named pipe server to communicate with TTS engines and register voices.
- **GUI**: A simple graphical user interface for users to select TTS engines, choose voices, and register them.
- **TTS Engine Support**: Microsoft, Google, AWS Polly, Sherpa-ONNX, and others supported through `py3-tts-wrapper`.
- **Voice Registration**: Register selected voices for use through SAPI or other systems.

## Requirements

- Python 3.11+
- Windows OS

## Installation

### 1. Install Dependencies

Using **\`uv\`** package manager, you can install all the necessary dependencies:

```powershell
powershell -ExecutionPolicy ByPass -c "irm https://astral.sh/uv/install.ps1 | iex"
uv sync
```

Next - YOU MUST Build the dll. so like this
    
```powershell
cd ..\VoiceEngineServer\engine
mkdir build
cd build
cmake ..
cmake --build . --config Debug
cmake --build . --config Release
```

move the Release folder to the VoiceEngineServer folder to _libs dir relative to the VoiceServer.py file

```powershell
mv .\engine\build\Release VoiceServer\_libs
```


### 2. Clone the Repository

```bash
git clone https://github.com/AceCentre/SAPI-POC/VoiceSAPI-POC.git
cd VoiceEngineServer
```

! Note: You may need to update your PATH environment variable to include the `uv` package manager.

### 3. Run the Pipe Service

YOU MUST RUN THIS PART ELEVATED AS IT CALLS regsrvr 32


```bash
uv run  VoiceServer.py
```

This starts the pipe service that listens for TTS-related requests.

### 4. Run the GUI Application

(you can run this as any user)


```bash
uv run  VoiceServerGUI.py
```

This launches the GUI that allows you to select and register TTS voices.

## Usage

### 1. Pipe Service

The pipe service listens for JSON-encoded requests to:
- **List available engines**: Provides a list of TTS engines configured in the system.
- **List available voices**: Lists voices available for a selected engine.
- **Speak text**: Streams text-to-speech output using the selected voice engine.
- **Register voices**: Registers selected voices for use with the TTS system.

The service communicates through the Windows named pipe `\\.\pipe\VoiceEngineServer`.

### 2. GUI Application

The GUI provides a simple way to:
1. **Select a Voice Engine**: Choose a TTS engine from the available engines.
2. **Select Voices**: Choose one or more voices from the selected engine.
3. **Register Voices**: Register the selected voices for use with the TTS system.

#### Steps:
1. Select a voice engine from the dropdown.
2. Choose one or more voices from the list.
3. Click the "Register Selected Voices" button to register the selected voices.

## Pipe Service

### Example JSON Requests:

#### List Engines:

```json
{
  "action": "list_engines"
}
```

#### List Voices for an Engine:

```json
{
  "action": "list_voices",
  "engine": "Microsoft"
}
```

#### Speak Text:

```json
{
  "action": "speak_text",
  "engine": "Google",
  "text": "Hello, how are you?"
}
```

#### Register a Voice:

```json
{
  "action": "set_voice",
  "engine": "Microsoft",
  "voice_iso_code": "en-US-JessaNeural"
}
```

## GUI Application

The GUI is a simple interface for interacting with the pipe service. It allows users to choose a TTS engine, select voices, and register them with a few clicks.

### GUI Components:
- **Engine Dropdown**: Choose a TTS engine from the available engines.
- **Voice List**: Select one or more voices to register.
- **Register Button**: Click to register the selected voices.

## Contributing

Feel free to fork the repository, submit issues, and create pull requests. Contributions are welcome!

## License

This project is licensed under the MIT License.

### Additional Notes:
- Make sure you have all necessary credentials for the TTS engines (Microsoft, Google, AWS, etc.).
- Update the `settings.cfg` file with your API keys and other configuration settings.
