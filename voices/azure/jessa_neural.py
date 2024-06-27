from abc import ABC, abstractmethod
from typing import Generator
import struct
import math

class Voice(ABC):
    def __init__(self):
        pass

    @abstractmethod
    def speak(self, text: str) -> Generator[bytes, None, None]:
        pass


class JessaNeuralVoice(Voice):
    def __init__(self):
        super().__init__()

    def speak(self, text: str) -> Generator[bytes, None, None]:
        for _ in text:
            yield generate_sine_wave(16000, 0.5, 0.2, 400)
            yield bytes(int(16000 * 0.2))


def generate_sine_wave(sampling_rate: int, volume: float, duration: float, frequency: float):
    num_samples = int(sampling_rate * duration)
    samples = [int(volume * 32767 * math.sin(2 * math.pi * k * frequency / sampling_rate))
        for k in range(0, num_samples)]
    return struct.pack(f"<{num_samples}h", *samples)
