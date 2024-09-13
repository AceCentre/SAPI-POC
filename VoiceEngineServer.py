import logging
import os
import sys
import warnings
import json
import win32file
import win32pipe
from PySide6.QtWidgets import QApplication, QWidget, QSystemTrayIcon, QMenu
from PySide6.QtGui import QIcon, QAction
from PySide6.QtCore import QThread, Signal, Slot, QTimer
import configparser
from dotenv import load_dotenv
from tts_wrapper import MicrosoftClient, GoogleClient, PollyClient, SherpaOnnxClient
from tts_wrapper import MicrosoftTTS, GoogleTTS, PollyTTS, SherpaOnnxTTS

# Suppress warnings
warnings.filterwarnings("ignore")

# Helper to setup logging
def setup_logging():
    if getattr(sys, 'frozen', False):
        log_dir = os.path.join(os.path.expanduser("~"), 'AppData', 'Roaming', 'VoiceEngineServer')
    else:
        log_dir = os.path.dirname(os.path.abspath(__file__))

    if not os.path.exists(log_dir):
        os.makedirs(log_dir)
    log_file = os.path.join(log_dir, 'app.log')

    logging.basicConfig(
        filename=log_file,
        filemode='a',
        format="%(asctime)s — %(name)s — %(levelname)s — %(funcName)s:%(lineno)d — %(message)s",
        level=logging.DEBUG
    )

    return log_file


def load_config(config_path='settings.cfg'):
    """Load the configuration file and parse available TTS engines and credentials."""
    if not os.path.exists(config_path):
        raise FileNotFoundError(f"Config file not found at {config_path}")

    config = configparser.ConfigParser()
    config.read(config_path)

    engines = {}

    # Microsoft TTS configuration
    if 'Microsoft' in config:
        engines['Microsoft'] = {
            'token': config.get('Microsoft', 'token', fallback=os.getenv('MICROSOFT_TOKEN')),
            'region': config.get('Microsoft', 'region', fallback=os.getenv('MICROSOFT_REGION')),
        }
    
    # Google TTS configuration
    if 'Google' in config:
        engines['Google'] = {
            'cred_json': config.get('Google', 'cred_json', fallback=os.getenv('GOOGLE_CREDS_JSON')),
        }

    # AWS Polly configuration
    if 'AWS' in config:
        engines['AWS'] = {
            'access_key': config.get('AWS', 'access_key'),
            'secret_key': config.get('AWS', 'secret_key'),
            'region': config.get('AWS', 'region'),
        }

    # Sherpa-ONNX configuration
    if 'SherpaOnnx' in config:
        engines['SherpaOnnx'] = {
            'model_path': config.get('SherpaOnnx', 'model_path', fallback=None),
            'tokens_path': config.get('SherpaOnnx', 'tokens_path', fallback=None),
        }

    return engines


def init_engines(engines):
    """Initialize TTS clients and TTS classes for the engines specified in the configuration."""
    initialized_engines = {}

    # Microsoft TTS
    if 'Microsoft' in engines:
        ms_token = engines['Microsoft']['token']
        ms_region = engines['Microsoft']['region']
        try:
            ms_client = MicrosoftClient(credentials=(ms_token, ms_region))
            ms_tts = MicrosoftTTS(ms_client)
            ms_tts.get_voices()  # Validate by fetching voices
            initialized_engines['Microsoft'] = ms_tts
        except Exception as e:
            logging.error(f"Error initializing Microsoft TTS: {e}")

    # Google TTS
    if 'Google' in engines:
        google_cred_path = engines['Google']['cred_json']
        try:
            google_client = GoogleClient(credentials=(google_cred_path,))
            google_tts = GoogleTTS(google_client)
            google_tts.get_voices()  # Validate by fetching voices
            initialized_engines['Google'] = google_tts
        except Exception as e:
            logging.error(f"Error initializing Google TTS: {e}")

    # AWS Polly TTS
    if 'AWS' in engines:
        aws_key = engines['AWS']['access_key']
        aws_secret = engines['AWS']['secret_key']
        aws_region = engines['AWS']['region']
        try:
            polly_client = PollyClient(credentials=(aws_key, aws_secret, aws_region))
            polly_tts = PollyTTS(polly_client)
            polly_tts.get_voices()  # Validate by fetching voices
            initialized_engines['AWS'] = polly_tts
        except Exception as e:
            logging.error(f"Error initializing AWS Polly: {e}")

    # Sherpa-ONNX TTS
    if 'SherpaOnnx' in engines:
        model_path = engines['SherpaOnnx']['model_path']
        tokens_path = engines['SherpaOnnx']['tokens_path']
        try:
            sherpa_client = SherpaOnnxClient(model_path=model_path, tokens_path=tokens_path)
            sherpa_tts = SherpaOnnxTTS(sherpa_client)
            sherpa_tts.get_voices()  # Validate by fetching voices
            initialized_engines['SherpaOnnx'] = sherpa_tts
        except Exception as e:
            logging.error(f"Error initializing Sherpa-ONNX TTS: {e}")

    return initialized_engines


class PipeServerThread(QThread):
    message_received = Signal(str)
    voices = None
    available_engines = None

    def __init__(self):
        super().__init__()
        self.engines = None

    def init_engines(self, config_path):
        """Initialize engines based on the configuration file."""
        config = load_config(config_path)
        self.engines = init_engines(config)
        self.available_engines = list(self.engines.keys())
        logging.info(f"Initialized Engines: {self.available_engines}")

    def run(self):
        """Run the pipe server to listen for client requests."""
        pipe_name = r'\\.\pipe\VoiceEngineServer'
        while True:
            pipe = None
            try:
                pipe = win32pipe.CreateNamedPipe(
                    pipe_name,
                    win32pipe.PIPE_ACCESS_DUPLEX,
                    win32pipe.PIPE_TYPE_MESSAGE | win32pipe.PIPE_READMODE_MESSAGE | win32pipe.PIPE_WAIT,
                    win32pipe.PIPE_UNLIMITED_INSTANCES, 65536, 65536,
                    0,
                    None)

                logging.info("Waiting for client connection...")
                win32pipe.ConnectNamedPipe(pipe, None)
                logging.info("Client connected.")

                result, data = win32file.ReadFile(pipe, 64 * 1024)
                if result == 0:
                    message = data.decode()
                    logging.info(f"Received data: {message[:50]}...")
                    request = json.loads(message)

                    # Handle different requests
                    if request.get('action') == 'list_engines':
                        response = {'engines': self.available_engines}
                        win32file.WriteFile(pipe, json.dumps(response).encode())
                    elif request.get('action') == 'list_voices':
                        engine_name = request.get('engine')
                        if engine_name in self.engines:
                            voices = self.fetch_voices(engine_name)
                            win32file.WriteFile(pipe, json.dumps(voices).encode())
                    elif request.get('action') == 'set_voice':
                        engine_name = request.get('engine')
                        voice_iso_code = request.get('voice_iso_code')
                        if engine_name in self.engines:
                            tts_engine = self.engines[engine_name]
                            tts_engine.set_voice(voice_iso_code)
                            logging.info(f"Voice {voice_iso_code} set for {engine_name}")
                    elif request.get('action') == 'set_voice':
                        engine_name = request.get('engine')
                        voice_iso_code = request.get('voice_iso_code')
                        if engine_name in self.engines:
                            tts_engine = self.engines[engine_name]
                            success = self.register_voice(tts_engine, engine_name, voice_iso_code)
                            response = {"status": "success" if success else "failure"}
                            win32file.WriteFile(pipe, json.dumps(response).encode())
                    elif request.get('action') == 'speak_text':
                        engine_name = request.get('engine')
                        text = request.get('text')
                        if engine_name in self.engines:
                            tts_engine = self.engines[engine_name]
                            logging.info(f"Speaking text with {engine_name}: {text[:50]}...")
                            self.speak_text_streamed(pipe, tts_engine, text)
                logging.info("Processing complete. Ready for next connection.")
            except Exception as e:
                logging.error(f"Pipe server error: {e}", exc_info=True)
            finally:
                if pipe:
                    win32file.CloseHandle(pipe)
                logging.info("Pipe closed. Reopening for next connection.")
   
    def fetch_voices(self, engine_name):
        """Fetch voices for the selected engine and ensure the request completes."""
        try:
            tts_engine = self.engines[engine_name]
            voices = tts_engine.get_voices()

            # Make sure that we wait for the voices to be completely fetched
            if not voices or len(voices) == 0:
                logging.error(f"No voices found for engine: {engine_name}")
                return {"status": "error", "message": "No voices found"}

            logging.info(f"Fetched {len(voices)} voices for engine: {engine_name}")
            return {"status": "success", "voices": voices}

        except Exception as e:
            logging.error(f"Error fetching voices for engine {engine_name}: {e}")
            return {"status": "error", "message": str(e)}
        
    def speak_text_streamed(self, tts_engine, text):
        """Stream the TTS output using speak_streamed (audio will play directly)."""
        try:
            # Directly play the TTS audio using speak_streamed
            tts_engine.speak_streamed(text)
            logging.info(f"Finished streaming TTS audio for text: {text[:50]}...")
        except Exception as e:
            logging.error(f"Error streaming TTS audio: {e}", exc_info=True)
    
    def register_voice(self, tts_engine, engine_name, voice_iso_code):
        """Registers the voice by first registering the engine, then the voice."""
        try:
            # Step 1: Register the engine (assumed dll file is named pysapittsengine.dll)
            engine_dll = "pysapittsengine.dll"
            if not self.is_engine_registered(engine_dll):
                logging.info(f"Registering engine: {engine_dll}")
                self.run_command(["regsvr32.exe", "/s", engine_dll])
                logging.info(f"Engine {engine_dll} registered successfully.")

            # Step 2: Register the voice using `regvoice.exe`
            voice_name = tts_engine.get_voice_name(voice_iso_code)
            voice_registration_command = [
                "regvoice.exe",
                "--token", f"PYTTS-{voice_name.replace(' ', '')}",
                "--name", voice_name,
                "--vendor", "Microsoft",  # Adjust as needed
                "--path", "C:\\Work\\SAPI-POC;C:\\Work\\build\\venv\\Lib\\site-packages",
                "--module", "voices",
                "--class", voice_iso_code  # Assuming the voice ISO code corresponds to the class
            ]
            logging.info(f"Registering voice: {voice_name}")
            self.run_command(voice_registration_command)
            logging.info(f"Voice {voice_name} registered successfully.")
            return True

        except Exception as e:
            logging.error(f"Error registering voice: {e}")
            return False

    def is_engine_registered(self, dll_name):
        """Check if the engine DLL is already registered. This is a simplified check."""
        try:
            result = subprocess.run(["regsvr32.exe", "/n", dll_name], capture_output=True)
            return result.returncode == 0
        except Exception as e:
            logging.error(f"Failed to check engine registration: {e}")
            return False

    def run_command(self, command):
        """Utility to run a system command."""
        try:
            subprocess.run(command, check=True)
        except subprocess.CalledProcessError as e:
            logging.error(f"Command failed: {command}\nError: {e}")
            raise

class MainWindow(QWidget):
    def __init__(self):
        super().__init__()
        self.pipe_thread = None
        self.tray_icon = None
        self.icon = QIcon('assets/icon.ico')
        self.init_ui()
        self.init_pipe_server()

    def init_ui(self):
        """Initialize the system tray icon."""
        self.tray_icon = SystemTrayIcon(self.icon, self)
        self.tray_icon.setVisible(True)

    def init_pipe_server(self):
        """Initialize and start the pipe server."""
        self.pipe_thread = PipeServerThread()
        self.pipe_thread.init_engines(config_path='settings.cfg')
        self.pipe_thread.start()

class SystemTrayIcon(QSystemTrayIcon):
    def __init__(self, icon, parent=None):
        super().__init__(icon, parent)
        menu = QMenu(parent)
        logging.info("System tray initialized")

        openLogsAction = QAction("Open logs", self)
        menu.addAction(openLogsAction)
        openLogsAction.triggered.connect(self.open_logs)

        exitAction = menu.addAction("Exit")
        exitAction.triggered.connect(self.exit)

        self.setContextMenu(menu)

    def open_logs(self):
        """Open the log file."""
        logging.info("Opening logs...")
        subprocess.Popen(['notepad', logfile])

    def exit(self):
        """Quit the application."""
        logging.info("Exiting application")
        QApplication.quit()

# Main application entry point
if __name__ == '__main__':
    logfile = setup_logging()
    # Load configuration from .env if necessary
    load_dotenv(dotenv_path='./.env')
    app = QApplication(sys.argv)
    window = MainWindow()
    sys.exit(app.exec())