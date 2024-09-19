import sys
import json
import logging
from PySide6.QtWidgets import (
    QApplication,
    QWidget,
    QVBoxLayout,
    QLabel,
    QComboBox,
    QListWidget,
    QPushButton,
    QListWidgetItem,
    QMessageBox,
    QLineEdit,
    QHBoxLayout,
)
from PySide6.QtCore import Qt
import win32file
import win32pipe
import zlib
import os


def setup_logging():
    if getattr(sys, "frozen", False):
        log_dir = os.path.join(
            os.path.expanduser("~"), "AppData", "Roaming", "VoiceEngineServer"
        )
    else:
        log_dir = os.path.dirname(os.path.abspath(__file__))

    print(f"Log directory: {log_dir}")

    if not os.path.exists(log_dir):
        os.makedirs(log_dir)
    log_file = os.path.join(log_dir, "register-voice.log")
    print(f"Log file: {log_file}")
    logging.basicConfig(
        filename=log_file,
        filemode="a",
        format="%(asctime)s — %(name)s — %(levelname)s — %(funcName)s:%(lineno)d — %(message)s",
        level=logging.DEBUG,
    )

    return log_file


def decompress_response(compressed_response):
    try:
        # Log the length of the data to verify its integrity
        logging.debug(f"Compressed data length: {len(compressed_response)}")
        # Attempt decompression with the correct headers for both gzip and zlib
        response_data = zlib.decompress(compressed_response, wbits=15 + 32)
        logging.debug(f"Decompressed data length: {len(response_data)}")
        return response_data
    except zlib.error as e:
        logging.error(f"Failed to decompress response: {e}")
        # Optionally log a hex dump of the data for detailed inspection
        logging.debug(
            f"Compressed data content (first 100 bytes): {compressed_response[:100]}"
        )
        return None


def receive_and_decompress(pipe):
    compressed_response = b""
    while True:
        # Read chunks of data from the pipe
        result, chunk = win32file.ReadFile(pipe, 64 * 1024)
        compressed_response += chunk
        # Break when no more data is expected (usually last chunk is less than 64 * 1024 bytes)
        if len(chunk) < 64 * 1024:
            break

    # Attempt to decompress the complete received data
    response_data = decompress_response(compressed_response)
    if response_data:
        logging.info("Decompressed successfully.")
        # Process the decompressed response as needed
        return response_data
    else:
        logging.error("Decompression failed, no response data available.")
        return None


# Pipe interaction utility functions
def send_pipe_request(request):
    try:
        pipe_name = r"\\.\pipe\VoiceEngineServer"
        pipe = win32file.CreateFile(
            pipe_name,
            win32file.GENERIC_READ | win32file.GENERIC_WRITE,
            0,
            None,
            win32file.OPEN_EXISTING,
            0,
            None,
        )

        # Compress the request data using zlib before sending
        request_data = json.dumps(request).encode()
        compressed_request_data = zlib.compress(request_data)

        # Send compressed data over the pipe
        win32file.WriteFile(pipe, compressed_request_data)

        # Read compressed response from pipe
        response_data = receive_and_decompress(pipe)

        if response_data is None:
            logging.error("Decompression failed, no response data available.")
            return

        # Process the decompressed response
        response_text = response_data.decode("utf-8")
        logging.debug(f"Response text: {response_text}")

        response_text = response_data.decode("utf-8")
        logging.debug(f"Response text: {response_text}")
        return json.loads(response_data.decode())

    except Exception as e:
        logging.error(f"Error communicating with pipe: {e}")
        return None


# Main GUI Window
class VoiceSelectionGUI(QWidget):
    def __init__(self):
        super().__init__()

        self.init_ui()
        self.engines = []
        self.voices = []

    def init_ui(self):
        layout = QVBoxLayout()

        # Engine Selection
        self.engine_label = QLabel("Select a Voice Engine:", self)
        self.engine_combo = QComboBox(self)
        self.engine_combo.currentIndexChanged.connect(self.load_voices)

        # Search Box for Filtering Languages
        self.search_box = QLineEdit(self)
        self.search_box.setPlaceholderText("Search by language...")
        self.search_box.textChanged.connect(self.filter_voices)

        # Voice Selection List
        self.voice_label = QLabel("Select Voices:", self)
        self.voice_list = QListWidget(self)
        self.voice_list.setSelectionMode(QListWidget.MultiSelection)

        # Register Button
        self.register_button = QPushButton("Register Selected Voices", self)
        self.register_button.clicked.connect(self.register_selected_voices)

        # Adding widgets to the layout
        layout.addWidget(self.engine_label)
        layout.addWidget(self.engine_combo)
        layout.addWidget(self.search_box)
        layout.addWidget(self.voice_label)
        layout.addWidget(self.voice_list)
        layout.addWidget(self.register_button)

        self.setLayout(layout)
        self.setWindowTitle("Voice Engine and Voice Selection")
        self.resize(400, 400)

        # Load Engines when initializing UI
        self.load_engines()

    def load_engines(self):
        """Loads the available engines from the pipe server."""
        request = {"action": "list_engines"}
        response = send_pipe_request(request)

        if response and "engines" in response:
            self.engines = response["engines"]
            self.engine_combo.clear()
            self.engine_combo.addItems(self.engines)
        else:
            QMessageBox.critical(
                self, "Error", "Failed to load engines from pipe service."
            )

    def load_voices(self):
        """Loads the voices for the selected engine."""
        engine_name = self.engine_combo.currentText()
        if not engine_name:
            return

        request = {"action": "list_voices", "engine": engine_name}
        response = send_pipe_request(request)

        if response and "voices" in response:
            self.voices = response["voices"]
            self.voice_list.clear()
            for voice in self.voices:
                item = QListWidgetItem(voice["name"])
                item.setData(Qt.UserRole, voice)
                self.voice_list.addItem(item)
        else:
            QMessageBox.critical(
                self, "Error", f"Failed to load voices for engine {engine_name}."
            )

    def update_voice_list(self, voices):
        """Updates the voice list with the given voices."""
        self.voice_list.clear()
        for voice in voices:
            languages = ", ".join(voice.get("language_codes", []))
            item = QListWidgetItem(f"{voice['name']} - Languages: {languages}")
            item.setData(Qt.UserRole, voice)
            self.voice_list.addItem(item)

    def filter_voices(self):
        """Filters the voice list based on the search box input."""
        search_text = self.search_box.text().lower()
        filtered_voices = [
            voice
            for voice in self.voices
            if search_text in ", ".join(voice.get("language_codes", [])).lower()
        ]
        self.update_voice_list(filtered_voices)

    def register_selected_voices(self):
        """Registers the selected voices."""
        selected_items = self.voice_list.selectedItems()
        if not selected_items:
            QMessageBox.warning(
                self, "Warning", "Please select at least one voice to register."
            )
            return

        # Send registration request to the pipe service using the 'id' from voice data
        for item in selected_items:
            voice_data = item.data(Qt.UserRole)
            engine_name = self.engine_combo.currentText()  # Extract the engine name
            voice_id = voice_data["id"]  # Use the unique 'id' field from the voice data
            engine_voice_combo = (
                f"{engine_name}-{voice_id}"  # Combine engine and voice_id
            )

            request = {
                "action": "set_voice",
                "engine_voice_combo": engine_voice_combo,  # Send the combined engine and voice_id
            }
            response = send_pipe_request(request)
            if response and response.get("status") == "success":
                logging.info(f"Successfully registered voice: {engine_voice_combo}")
                QMessageBox.information(
                    self, "Success", "Selected voices have been registered."
                )
            else:
                logging.error(f"Failed to register voice: {engine_voice_combo}")
                QMessageBox.critical(
                    self, "Error", f"Failed to register voice: {engine_voice_combo}"
                )


# Run the GUI
if __name__ == "__main__":
    logfile = setup_logging()
    app = QApplication(sys.argv)
    gui = VoiceSelectionGUI()
    gui.show()
    sys.exit(app.exec())
