from abc import ABC, abstractmethod
from typing import Iterator
import json
import azure.cognitiveservices.speech as speechsdk


class Voice(ABC):
    def __init__(self):
        pass

    @abstractmethod
    def speak(self, text: str) -> Iterator[bytes]:
        pass


class AzureNeuralVoice(Voice):
    def __init__(self):
        super().__init__()

        with open("credentials-private.json") as f:
            doc = json.load(f)
        subscription = doc["Microsoft"]["token"]
        region = doc["Microsoft"]["region"]

        speech_config = speechsdk.SpeechConfig(subscription, region)
        speech_config.set_speech_synthesis_output_format(
            speechsdk.SpeechSynthesisOutputFormat.Riff24Khz16BitMonoPcm
        )
        speech_config.speech_synthesis_voice_name = "en-US-AvaMultilingualNeural"

        self.speech_synthesizer = speechsdk.SpeechSynthesizer(
            speech_config=speech_config, audio_config=None
        )

        self.buffer = bytes(24000 * 2)

    def speak(self, text: str) -> Iterator[bytes]:
        result = self.speech_synthesizer.speak_text_async(text).get()

        if result.reason == speechsdk.ResultReason.SynthesizingAudioCompleted:
            stream = speechsdk.AudioDataStream(result)

            while True:
                n = stream.read_data(self.buffer)
                if n == 0:
                    break
                yield self.buffer[:n]

        elif result.reason == speechsdk.ResultReason.Canceled:
            print(f"error_code={result.cancellation_details.error_code}"
                  f", error_details={
                      result.cancellation_details.error_details}"
                  f", reason={result.cancellation_details.reason}")
