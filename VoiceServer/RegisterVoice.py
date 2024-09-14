import sys
import json
import logging
from PySide6.QtWidgets import QApplication, QWidget, QVBoxLayout, QLabel, QComboBox, QListWidget, QPushButton, QListWidgetItem, QMessageBox
from PySide6.QtCore import Qt
import win32file
import win32pipe

# Pipe interaction utility functions
def send_pipe_request(request):
    try:
        pipe_name = r'\\.\pipe\VoiceEngineServer'
        pipe = win32file.CreateFile(
            pipe_name,
            win32file.GENERIC_READ | win32file.GENERIC_WRITE,
            0, None, win32file.OPEN_EXISTING, 0, None
        )
        request_data = json.dumps(request).encode()
        win32file.WriteFile(pipe, request_data)
        
        result, response = win32file.ReadFile(pipe, 64 * 1024)
        win32file.CloseHandle(pipe)
        return json.loads(response.decode())
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

        # Label and ComboBox for Engine Selection
        self.engine_label = QLabel("Select a Voice Engine:", self)
        self.engine_combo = QComboBox(self)
        self.engine_combo.currentIndexChanged.connect(self.load_voices)

        # Label and ListWidget for Voice Selection
        self.voice_label = QLabel("Select Voices:", self)
        self.voice_list = QListWidget(self)
        self.voice_list.setSelectionMode(QListWidget.MultiSelection)

        # Register Button
        self.register_button = QPushButton("Register Selected Voices", self)
        self.register_button.clicked.connect(self.register_selected_voices)

        # Adding widgets to the layout
        layout.addWidget(self.engine_label)
        layout.addWidget(self.engine_combo)
        layout.addWidget(self.voice_label)
        layout.addWidget(self.voice_list)
        layout.addWidget(self.register_button)

        self.setLayout(layout)
        self.setWindowTitle("Voice Engine and Voice Selection")
        self.resize(400, 300)

        # Load Engines when initializing UI
        self.load_engines()

    def load_engines(self):
        """Loads the available engines from the pipe server."""
        request = {"action": "list_engines"}
        response = send_pipe_request(request)

        if response and 'engines' in response:
            self.engines = response['engines']
            self.engine_combo.clear()
            self.engine_combo.addItems(self.engines)
        else:
            QMessageBox.critical(self, "Error", "Failed to load engines from pipe service.")

    def load_voices(self):
        """Loads the voices for the selected engine."""
        engine_name = self.engine_combo.currentText()
        if not engine_name:
            return

        request = {"action": "list_voices", "engine": engine_name}
        response = send_pipe_request(request)

        if response and 'voices' in response:
            self.voices = response['voices']
            self.voice_list.clear()
            for voice in self.voices:
                item = QListWidgetItem(voice['name'])
                item.setData(Qt.UserRole, voice)
                self.voice_list.addItem(item)
        else:
            QMessageBox.critical(self, "Error", f"Failed to load voices for engine {engine_name}.")

    def register_selected_voices(self):
        """Registers the selected voices."""
        selected_items = self.voice_list.selectedItems()
        if not selected_items:
            QMessageBox.warning(self, "Warning", "Please select at least one voice to register.")
            return

        engine_name = self.engine_combo.currentText()  # Extract the engine name from the combo box
        selected_voices = [f"{engine_name}-{item.data(Qt.UserRole)['name']}" for item in selected_items]

        # Send registration request to the pipe service
        for voice_iso_code in selected_voices:
            request = {
                "action": "set_voice",
                "voice_iso_code": voice_iso_code  # Now includes both engine and voice
            }
            response = send_pipe_request(request)
            if response and response.get("status") == "success":
                logging.info(f"Successfully registered voice: {voice_iso_code}")
            else:
                logging.error(f"Failed to register voice: {voice_iso_code}")
                QMessageBox.critical(self, "Error", f"Failed to register voice: {voice_iso_code}")

        QMessageBox.information(self, "Success", "Selected voices have been registered.")

# Run the GUI
if __name__ == '__main__':
    app = QApplication(sys.argv)
    gui = VoiceSelectionGUI()
    gui.show()
    sys.exit(app.exec())