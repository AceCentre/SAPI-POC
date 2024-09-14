from abc import ABC, abstractmethod
from typing import Iterator

import json
import win32file
import win32pipe

class PipeClient:
    PIPE_NAME = r'\\.\pipe\AACSpeakHelper'

    @staticmethod
    def send_request(data: dict) -> dict:
        try:
            # Connect to the pipe
            pipe = win32file.CreateFile(
                PipeClient.PIPE_NAME,
                win32file.GENERIC_READ | win32file.GENERIC_WRITE,
                0, None, win32file.OPEN_EXISTING, 0, None
            )
            
            # Send request to the pipe service
            request_data = json.dumps(data).encode()
            win32file.WriteFile(pipe, request_data)
            
            # Read response from the pipe
            response = win32file.ReadFile(pipe, 65536)[1]
            return json.loads(response.decode())
        
        except Exception as e:
            print(f"Error communicating with pipe: {e}")
            return {"status": "error", "message": str(e)}

        finally:
            win32file.CloseHandle(pipe)

class Voice(ABC):
    @abstractmethod
    def speak(self, text: str) -> Iterator[bytes]:
        pass

class VoiceServerVoice(Voice):
    def __init__(self, engine_name: str):
        # Any necessary setup, credentials can be handled in pipe service
        self.engine_name = engine_name

    def speak(self, text: str) -> Iterator[bytes]:
        # Send request to pipe service for TTS
        request = {
            "action": "speak",
            "engine": self.engine_name,  # Use the engine_name passed during initialization
            "text": text
        }
        response = PipeClient.send_request(request)
        
        if response.get("status") == "success":
            # Yield the audio data back to the engine in chunks
            for chunk in response["audio_data"]:
                yield bytes(chunk)
        else:
            print(f"Error in TTS: {response.get('message')}")
            return