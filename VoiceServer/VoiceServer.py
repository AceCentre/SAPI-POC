import logging
import os
import sys
import warnings
import json
import win32file
import win32pipe
import win32security
import ntsecuritycon as con

from PySide6.QtWidgets import QApplication, QWidget, QSystemTrayIcon, QMenu
from PySide6.QtGui import QIcon, QAction
from PySide6.QtCore import QThread, Signal, Slot, QTimer
import configparser
from dotenv import load_dotenv
from tts_wrapper import MicrosoftClient, GoogleClient, PollyClient, SherpaOnnxClient
from tts_wrapper import MicrosoftTTS, GoogleTTS, PollyTTS, SherpaOnnxTTS
import subprocess

# Suppress warnings
warnings.filterwarnings("ignore")

# Helper to setup logging
def setup_logging():
    if getattr(sys, 'frozen', False):
        log_dir = os.path.join(os.path.expanduser("~"), 'AppData', 'Roaming', 'VoiceEngineServer')
    else:
        log_dir = os.path.dirname(os.path.abspath(__file__))

    print(f"Log directory: {log_dir}")

    if not os.path.exists(log_dir):
        os.makedirs(log_dir)
    log_file = os.path.join(log_dir, 'app.log')
    print(f"Log file: {log_file}")
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

    # Polly Polly configuration
    if 'Polly' in config:
        engines['Polly'] = {
            'aws_key_id': config.get('Polly', 'aws_key_id'),
            'aws_access_key': config.get('Polly', 'aws_access_key'),
            'region': config.get('Polly', 'region'),
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

    # Polly Polly TTS
    if 'Polly' in engines:
        polly_key = engines['Polly']['aws_access_key']
        polly_key_id = engines['Polly']['aws_key_id']
        polly_region = engines['Polly']['region']
        try:
            polly_client = PollyClient(credentials=(polly_region, polly_key_id, polly_key))
            polly_tts = PollyTTS(polly_client)
            polly_tts.get_voices()  # Validate by fetching voices
            initialized_engines['Polly'] = polly_tts
        except Exception as e:
            logging.error(f"Error initializing Polly Polly: {e}")

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
        self.libs_directory = os.path.join(os.getcwd(), '_libs') 

    def init_engines(self, config_path):
        """Initialize engines based on the configuration file."""
        config = load_config(config_path)
        self.engines = init_engines(config)
        self.available_engines = list(self.engines.keys())
        logging.info(f"Initialized Engines: {self.available_engines}")

    def run(self):
        """Run the pipe server to listen for client requests."""
        pipe_name = r'\\.\pipe\VoiceEngineServer'
        security_attributes = win32security.SECURITY_ATTRIBUTES()
        security_descriptor = win32security.SECURITY_DESCRIPTOR()
        security_descriptor.SetSecurityDescriptorDacl(1, None, 0)  # Allow full access to everyone
        security_attributes.SECURITY_DESCRIPTOR = security_descriptor

        while True:
            pipe = None
            try:
                pipe = win32pipe.CreateNamedPipe(
                pipe_name,
                win32pipe.PIPE_ACCESS_DUPLEX,
                win32pipe.PIPE_TYPE_MESSAGE | win32pipe.PIPE_READMODE_MESSAGE | win32pipe.PIPE_WAIT,
                win32pipe.PIPE_UNLIMITED_INSTANCES,
                1024 * 1024,  # Increase the buffer size to 1MB
                1024 * 1024,  # Increase the buffer size for reading
                0,
                security_attributes)
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
                            self.fetch_voices(engine_name, pipe)
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

    def fetch_voices(self, engine_name, pipe):
        """Fetch voices for the selected engine and ensure the response is fully transmitted."""
        try:
            tts_engine = self.engines[engine_name]
            voices = tts_engine.get_voices()

            if not voices or len(voices) == 0:
                logging.error(f"No voices found for engine: {engine_name}")
                response_data = {"status": "error", "message": "No voices found"}
                self.send_large_data(pipe, response_data)
                return

            response_data = {"status": "success", "voices": voices}

            # Log the size of the response for debugging
            json_response = json.dumps(response_data, ensure_ascii=False)
            logging.info(f"Response size for {engine_name} voices: {len(json_response)} bytes")

            # Send the response data in chunks
            self.send_large_data(pipe, response_data)

        except Exception as e:
            logging.error(f"Error fetching voices for engine {engine_name}: {e}")
            response_data = {"status": "error", "message": str(e)}
            self.send_large_data(pipe, response_data)

    def send_large_data(self, pipe, data):
        """Send large data in chunks over the pipe."""
        try:
            chunk_size = 60 * 1024  # 60 KB per chunk

            # Convert data to a UTF-8 encoded JSON string
            data = json.dumps(data, ensure_ascii=False).encode('utf-8')

            # Send the data in chunks
            for i in range(0, len(data), chunk_size):
                chunk = data[i:i + chunk_size]
                win32file.WriteFile(pipe, chunk)
            
            # Optionally, send a termination signal or an empty chunk at the end
            # to indicate the end of the transmission
            win32file.WriteFile(pipe, b'')  # An empty write to signal the end of the transmission
            
        except Exception as e:
            logging.error(f"Error sending large data: {e}")
    
    def speak_text_streamed(self, tts_engine, text):
        """Stream the TTS output using speak_streamed (audio will play directly)."""
        try:
            # Directly play the TTS audio using speak_streamed
            tts_engine.speak_streamed(text)
            logging.info(f"Finished streaming TTS audio for text: {text[:50]}...")
        except Exception as e:
            logging.error(f"Error streaming TTS audio: {e}", exc_info=True)
    
    def register_voice(self, tts_engine, engine_name, voice_iso_code):
        """Registers the voice with the system."""
        voice_name = None  # Define voice_name at the start to avoid UnboundLocalError

        try:
            # Assume engine_dll points to the correct DLL for registration
            engine_dll = os.path.join(self.libs_directory, 'pysapittsengine.dll')
            regvoice_exe = os.path.join(self.libs_directory, 'regvoice.exe')

            # Check if the DLL and regvoice.exe exist
            if not os.path.exists(engine_dll):
                logging.error(f"Engine DLL not found at {engine_dll}")
                return False
            if not os.path.exists(regvoice_exe):
                logging.error(f"regvoice.exe not found at {regvoice_exe}")
                return False
            logging.info(f"Registering engine: {engine_dll}")

            # Run the registration command
            self.run_command(["regsvr32.exe", "/s", engine_dll])

            # Assume regvoice.exe command is used to register the voice
            voice_name = f"{engine_name}-{voice_iso_code}"
            # Set the correct paths for dependencies (adjust this based on actual locations)
            dependencies_path = f"{self.libs_directory};{os.path.dirname(sys.executable)}\\Lib\\site-packages"

            register_command = [
                regvoice_exe, 
                "--token", f"PYTTS-{engine_name}", 
                "--name", voice_name, 
                "--vendor", "Microsoft", 
                "--path", dependencies_path,
                "--module", "voices",
                "--class", f"{engine_name}Voice"
            ]
            self.run_command(register_command)

            logging.info(f"Successfully registered voice: {voice_name}")
            return True

        except subprocess.CalledProcessError as e:
            logging.error(f"Command failed: {e}")
            logging.error(f"Failed to register voice {voice_name}: {e}")
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
        self.icon = QIcon('icon.ico')
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