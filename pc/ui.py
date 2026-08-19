"""
Main window for the Stock Checker desktop app.

This file only deals with widgets and how they react to signals from
SerialWorker (see serial_comm.py) -- it never touches the serial port
directly. All serial I/O happens on a background thread; everything in
this file runs on the GUI thread, including the slot methods connected
to SerialWorker's signals (Qt marshals that delivery automatically).
"""

import datetime
import logging
import time

from PySide6.QtCore import QSettings, QTime, QTimer
from PySide6.QtGui import QColor, QFont
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QGroupBox,
    QHBoxLayout,
    QHeaderView,
    QInputDialog,
    QLabel,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QSpinBox,
    QTableWidget,
    QTableWidgetItem,
    QTimeEdit,
    QVBoxLayout,
    QWidget,
)

from serial_comm import SerialWorker, is_valid_mac, list_available_ports

logger = logging.getLogger(__name__)

APP_TITLE = "Stock Checker"

# Persists the "Add Sensor" MAC history (QSettings picks a sensible
# per-OS storage location automatically -- registry on Windows, a config
# file under ~/.config on Linux, a plist on macOS).
SETTINGS_ORG = "StockChecker"
SETTINGS_APP = "StockChecker"
SETTINGS_KEY_SENSOR_HISTORY = "sensor_mac_history"

# Seeds the "Add Sensor" history on first run (before anything's been
# persisted yet) -- these are the two bin sensors that used to be
# hardcoded into the logger firmware's SENSOR_MACS list, so re-adding
# them (e.g. after a logger NVS wipe or a swap to a new logger board)
# doesn't mean hunting up their MAC addresses again.
SEED_SENSOR_MACS = ["78:e3:6d:de:9c:d8", "58:bf:25:34:38:7c"]  # bin sensor 1, bin sensor 2

# Target used in the frequency dropdown/protocol to mean "every currently
# registered sensor" rather than one specific MAC. Must match the literal
# the logger firmware checks for (see pc_comm.c's SETFREQ parsing).
ALL_SENSORS_TARGET = "ALL"

# --- Stock status thresholds --------------------------------------------
# Distance is measured by the ToF sensor from itself down to whatever is
# below it, so a LARGER distance means an emptier bin. These are simple
# global constants for now; a later version could make them per-sensor
# and editable from the UI (e.g. one bin might be deeper than another).
STOCK_LOW_THRESHOLD_MM = 300     # distance >= this => "Low"
STOCK_EMPTY_THRESHOLD_MM = 500   # distance >= this => "Empty"

# --- Activity/overdue detection ------------------------------------------
# A sensor deep-sleeps between reports, so there's no way to ask it "are
# you awake" -- the radio is off. What we CAN do is infer status from
# timing: how long ago it last reported vs. how often it's supposed to.
# OVERDUE_GRACE_FACTOR is how much slack (as a multiple of the sensor's
# own interval) to allow before flagging it "Overdue" instead of just
# "due soon" -- accounts for normal jitter/drift, not exact timing.
# Configurable.
OVERDUE_GRACE_FACTOR = 1.5

# How often to recompute the Activity column against the current time.
# This is the only thing in the UI driven by a plain clock tick rather
# than an incoming message -- the "Overdue" transition has to happen
# even if nothing new ever arrives.
ACTIVITY_REFRESH_MS = 2000

# How long to wait for the logger to confirm a Set Frequency / Add Sensor
# request before telling the user it may not have gone through. Generous
# enough to cover a normal UART round trip; not so long that a genuinely
# dropped request leaves the button looking unresponsive for a while.
REQUEST_TIMEOUT_MS = 4000

# Table column indices, named so the rest of the code doesn't use magic numbers.
COL_MAC = 0
COL_READING = 1
COL_STATUS = 2
COL_INTERVAL = 3
COL_ACTIVITY = 4
COL_UPDATED = 5
COLUMN_HEADERS = [
    "MAC Address", "Last Reading (mm)", "Stock Status",
    "Interval (s)", "Activity", "Last Updated",
]

# Colors used for the connection-status dot, stock-status cells, and
# activity cells. Centralized here so they all stay visually consistent.
COLOR_GOOD = "#1e8e5a"      # connected / OK / reporting on schedule
COLOR_WARN = "#b98900"      # scanning / low stock / provisioning
COLOR_BAD = "#d13a3a"       # disconnected / empty / overdue
COLOR_MUTED = "#8a94a6"     # waiting for data / not-yet-known

STATUS_COLORS = {
    "OK": COLOR_GOOD,
    "Low": COLOR_WARN,
    "Empty": COLOR_BAD,
}

# A light, flat stylesheet so the window reads as a real desktop tool
# rather than an unstyled default Qt form. Kept as one QSS string and
# applied once in __init__ -- see https://doc.qt.io/qt-6/stylesheet.html
# for the syntax if this needs extending later.
_STYLESHEET = """
QMainWindow, QWidget#centralWidget {
    background-color: #f3f4f6;
}
QLabel {
    color: #1f2430;
}
QGroupBox {
    font-weight: 600;
    border: 1px solid #dcdfe4;
    border-radius: 8px;
    margin-top: 14px;
    padding: 14px 12px 12px 12px;
    background-color: #ffffff;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 12px;
    padding: 0 6px;
    color: #4b5566;
}
QPushButton {
    background-color: #2f6fed;
    color: white;
    border: none;
    border-radius: 6px;
    padding: 6px 16px;
    font-weight: 600;
}
QPushButton:hover {
    background-color: #2a5fd0;
}
QPushButton:pressed {
    background-color: #234ea8;
}
QPushButton:disabled {
    background-color: #b7c3d9;
    color: #eef1f6;
}
QPushButton#secondaryButton {
    background-color: #e7e9ed;
    color: #1f2430;
}
QPushButton#secondaryButton:hover {
    background-color: #d9dce2;
}
QComboBox, QSpinBox {
    border: 1px solid #d3d7dd;
    border-radius: 6px;
    padding: 4px 8px;
    background-color: white;
    min-height: 24px;
}
QTableWidget {
    border: 1px solid #dcdfe4;
    border-radius: 8px;
    background-color: white;
    gridline-color: #edeef1;
    alternate-background-color: #f8f9fb;
}
QTableWidget::item {
    padding: 6px;
}
QHeaderView::section {
    background-color: #f3f4f6;
    color: #4b5566;
    font-weight: 600;
    padding: 8px 6px;
    border: none;
    border-bottom: 1px solid #dcdfe4;
}
QStatusBar {
    background-color: #eceef1;
    color: #4b5566;
}
"""


def stock_status_for(distance_mm):
    """Maps a raw distance reading to a human status label."""
    if distance_mm >= STOCK_EMPTY_THRESHOLD_MM:
        return "Empty"
    if distance_mm >= STOCK_LOW_THRESHOLD_MM:
        return "Low"
    return "OK"


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle(APP_TITLE)
        self.resize(880, 520)
        self.setMinimumSize(680, 420)
        self.setStyleSheet(_STYLESHEET)

        # None while disconnected/scanning has never started a worker,
        # or after it's been stopped. Recreated fresh on every connection
        # attempt (including the automatic one below).
        self._worker = None

        # Maps mac address -> row index in the sensor table, so DATA/
        # SENSORS/FREQ/PROVISIONING messages can find (or idempotently
        # create) the right row instead of scanning the whole table every
        # time. The "Add Sensor" flow reuses this same _ensure_row() path --
        # nothing about this mapping assumes the sensor list is fixed or
        # only ever grows via SENSORS.
        self._row_for_mac = {}

        # Per-sensor state used to compute the Activity column -- this is
        # NOT just a mirror of what's on screen, since Activity has to be
        # recomputed against the current time even when nothing new has
        # arrived (see the periodic timer started below).
        self._sensor_state = {}

        # Tracks whether the logger has confirmed the most recent Set
        # Frequency / Add Sensor request yet -- lets the timeout check
        # (see REQUEST_TIMEOUT_MS below) tell "confirmed while we were
        # waiting" apart from "still nothing back", so a click always
        # produces visible feedback instead of silently doing nothing if
        # the logger never replies.
        self._freq_request_confirmed = True
        self._add_sensor_confirmed = True

        # MAC addresses offered by the "Add Sensor" dialog's dropdown,
        # persisted across runs so once a sensor's been seen once (added,
        # or just reported by the logger), you never have to retype its
        # MAC again. Grows automatically as sensors are discovered -- see
        # _remember_sensor_mac(), called from _ensure_row().
        self._settings = QSettings(SETTINGS_ORG, SETTINGS_APP)
        self._sensor_mac_history = self._load_sensor_mac_history()

        self._build_ui()
        self._refresh_ports()

        # Start looking for the logger immediately -- no click required.
        # The user can still override with a specific port from the
        # dropdown, or hit Disconnect to stop the search entirely.
        self._start_connection()

        self._activity_timer = QTimer(self)
        self._activity_timer.timeout.connect(self._refresh_activity_column)
        self._activity_timer.start(ACTIVITY_REFRESH_MS)

    # ----------------------------------------------------------------
    # UI construction
    # ----------------------------------------------------------------

    def _build_ui(self):
        central = QWidget()
        central.setObjectName("centralWidget")
        self.setCentralWidget(central)
        layout = QVBoxLayout(central)
        layout.setContentsMargins(16, 16, 16, 16)
        layout.setSpacing(12)

        layout.addLayout(self._build_connection_bar())
        layout.addWidget(self._build_frequency_group())
        layout.addLayout(self._build_sensor_table_bar())
        layout.addWidget(self._build_sensor_table())

        self.statusBar().showMessage("Starting...")

    def _build_connection_bar(self):
        row = QHBoxLayout()
        row.setSpacing(8)

        row.addWidget(QLabel("Logger Port:"))

        self.port_combo = QComboBox()
        self.port_combo.setMinimumWidth(260)
        row.addWidget(self.port_combo)

        self.refresh_button = QPushButton("Refresh")
        self.refresh_button.setObjectName("secondaryButton")
        self.refresh_button.clicked.connect(self._refresh_ports)
        row.addWidget(self.refresh_button)

        self.connect_button = QPushButton("Connect")
        self.connect_button.clicked.connect(self._on_connect_clicked)
        row.addWidget(self.connect_button)

        row.addStretch()

        # Simple colored-dot connection indicator, updated by
        # _set_connection_state() below.
        self.status_label = QLabel()
        font = self.status_label.font()
        font.setBold(True)
        self.status_label.setFont(font)
        row.addWidget(self.status_label)

        return row

    def _build_frequency_group(self):
        group = QGroupBox("Reporting Frequency")
        row = QHBoxLayout(group)

        row.addWidget(QLabel("Target:"))
        self.freq_target_combo = QComboBox()
        self.freq_target_combo.addItem("All Sensors", userData=ALL_SENSORS_TARGET)
        # Individual sensor entries are added as they're discovered --
        # see _refresh_freq_target_combo(), called from _ensure_row().
        row.addWidget(self.freq_target_combo)

        row.addStretch()

        row.addWidget(QLabel("New interval (seconds):"))
        self.freq_spinbox = QSpinBox()
        self.freq_spinbox.setRange(1, 24 * 60 * 60)  # 1 second .. 24 hours
        self.freq_spinbox.setValue(60)
        row.addWidget(self.freq_spinbox)

        # Unchecked (the default) means "no specific start time" -- the
        # logger anchors the schedule to epoch 0, which still gives clean
        # wall-clock-aligned wakes (e.g. every hour on the hour) since
        # 1970-01-01 00:00:00 UTC sits exactly on every such boundary.
        # Checking this lets you pin the schedule to an arbitrary time of
        # day instead, e.g. "every hour, starting at 13:00" -- every
        # sensor on that schedule will always wake at :00, never drift to
        # its own first-checked-in-at offset.
        self.align_checkbox = QCheckBox("Align to start time:")
        self.align_checkbox.toggled.connect(self._on_align_toggled)
        row.addWidget(self.align_checkbox)

        self.align_time_edit = QTimeEdit()
        self.align_time_edit.setDisplayFormat("HH:mm:ss")
        self.align_time_edit.setTime(QTime.currentTime())
        self.align_time_edit.setEnabled(False)
        row.addWidget(self.align_time_edit)

        self.set_freq_button = QPushButton("Set Frequency")
        self.set_freq_button.setEnabled(False)  # enabled once connected
        self.set_freq_button.clicked.connect(self._on_set_frequency_clicked)
        row.addWidget(self.set_freq_button)

        return group

    def _build_sensor_table_bar(self):
        row = QHBoxLayout()
        row.addWidget(QLabel("Sensors"))
        row.addStretch()

        self.add_sensor_button = QPushButton("Add Sensor")
        self.add_sensor_button.setObjectName("secondaryButton")
        self.add_sensor_button.setEnabled(False)  # enabled once connected
        self.add_sensor_button.clicked.connect(self._on_add_sensor_clicked)
        row.addWidget(self.add_sensor_button)

        return row

    def _build_sensor_table(self):
        self.table = QTableWidget(0, len(COLUMN_HEADERS))
        self.table.setHorizontalHeaderLabels(COLUMN_HEADERS)

        # Activity text can get long ("Overdue (expected ~123s ago)"), so
        # it gets to stretch and soak up the remaining space; everything
        # else just sizes to fit its own (short, fairly fixed-width) content.
        header = self.table.horizontalHeader()
        for col in (COL_MAC, COL_READING, COL_STATUS, COL_INTERVAL, COL_UPDATED):
            header.setSectionResizeMode(col, QHeaderView.ResizeToContents)
        header.setSectionResizeMode(COL_ACTIVITY, QHeaderView.Stretch)

        self.table.verticalHeader().setVisible(False)
        self.table.setEditTriggers(QTableWidget.NoEditTriggers)
        self.table.setSelectionMode(QTableWidget.NoSelection)
        self.table.setAlternatingRowColors(True)
        self.table.setMinimumHeight(240)
        # Sorting is left off deliberately: self._row_for_mac tracks rows
        # by index, and enabling sorting would let Qt reorder rows out
        # from under that mapping.
        self.table.setSortingEnabled(False)
        return self.table

    # ----------------------------------------------------------------
    # Connection handling
    # ----------------------------------------------------------------

    def _refresh_ports(self):
        selected = self.port_combo.currentData() if self.port_combo.count() else None
        self.port_combo.clear()
        self.port_combo.addItem("Auto-detect (recommended)", userData=None)
        for device, description in list_available_ports():
            self.port_combo.addItem(f"{device} -- {description}", userData=device)

        # Try to restore whatever was selected before the refresh, so
        # clicking Refresh doesn't silently reset a manual port choice.
        index = self.port_combo.findData(selected)
        self.port_combo.setCurrentIndex(index if index >= 0 else 0)

    def _on_connect_clicked(self):
        if self._worker is not None:
            self._disconnect()
            return
        self._start_connection(self.port_combo.currentData())

    def _start_connection(self, port_name=None):
        """port_name=None means auto-detect (see SerialWorker); a device
        path pins the search to exactly that port."""
        logger.info("Starting connection attempt (port=%s)", port_name or "auto-detect")
        self._worker = SerialWorker(port_name)
        self._worker.connected.connect(self._on_serial_connected)
        self._worker.disconnected.connect(self._on_serial_disconnected)
        self._worker.scanning.connect(self._on_scanning)
        self._worker.data_received.connect(self._on_data_received)
        self._worker.sensors_received.connect(self._on_sensors_received)
        self._worker.freq_received.connect(self._on_freq_received)
        self._worker.provisioning_started.connect(self._on_provisioning_started)
        self._worker.add_sensor_result.connect(self._on_add_sensor_result)
        self._worker.start()

        self.connect_button.setText("Disconnect")
        # Locked while a connection attempt is active (scanning or
        # connected) so you can't pick a different port while it's
        # ambiguous which one "Disconnect" would actually apply to --
        # you have to disconnect first, then the dropdown is meaningful
        # again.
        self.port_combo.setEnabled(False)
        self.refresh_button.setEnabled(False)
        self._set_connection_state("scanning", "Looking for logger...")

    def _disconnect(self):
        if self._worker is None:
            return
        logger.info("Stopping connection")
        # stop() blocks (up to 2s) waiting for the worker thread to notice
        # the stop event and exit -- but Qt queues cross-thread signals
        # for later delivery rather than blocking on them, so a signal
        # the worker thread was mid-emitting right as we call stop() can
        # still arrive here *after* this method returns (even after a
        # brand new worker has been started by a quick Connect click).
        # None of the _on_serial_* slots check whether the sender is
        # still self._worker, so a late signal from this dying worker
        # would silently clobber the new one's state. Disconnecting every
        # signal from this worker first guarantees nothing more from it
        # can reach those slots, no matter when it actually finishes.
        # PySide6's QObject.disconnect() (unlike PyQt5's) doesn't support
        # being called with no arguments to drop every connection at
        # once, so each signal has to be disconnected individually.
        for signal in (
            self._worker.connected,
            self._worker.disconnected,
            self._worker.scanning,
            self._worker.data_received,
            self._worker.sensors_received,
            self._worker.freq_received,
            self._worker.provisioning_started,
            self._worker.add_sensor_result,
        ):
            signal.disconnect()
        self._worker.stop()
        self._worker = None
        self._set_connection_state("disconnected", "Not connected")
        self.connect_button.setText("Connect")
        self.port_combo.setEnabled(True)
        self.refresh_button.setEnabled(True)
        self.set_freq_button.setEnabled(False)
        self.add_sensor_button.setEnabled(False)

    def _on_scanning(self, detail):
        self._set_connection_state("scanning", detail)

    def _on_serial_connected(self, port_name):
        self._set_connection_state("connected", f"Connected to {port_name}")
        self.set_freq_button.setEnabled(True)
        self.add_sensor_button.setEnabled(True)

    def _on_serial_disconnected(self, reason):
        logger.warning("Disconnected: %s", reason)
        self._set_connection_state("disconnected", reason)
        self.connect_button.setText("Connect")
        self.port_combo.setEnabled(True)
        self.refresh_button.setEnabled(True)
        self.set_freq_button.setEnabled(False)
        self.add_sensor_button.setEnabled(False)
        self._worker = None

    def _set_connection_state(self, state, detail):
        color = {"connected": COLOR_GOOD, "scanning": COLOR_WARN, "disconnected": COLOR_BAD}[state]
        self.status_label.setStyleSheet(f"color: {color};")
        self.status_label.setText(f"\N{BLACK CIRCLE} {detail}")
        self.statusBar().showMessage(detail)

    # ----------------------------------------------------------------
    # Frequency control
    # ----------------------------------------------------------------

    def _refresh_freq_target_combo(self, mac):
        if self.freq_target_combo.findData(mac) < 0:
            self.freq_target_combo.addItem(mac, userData=mac)

    def _on_align_toggled(self, checked):
        self.align_time_edit.setEnabled(checked)

    def _compute_anchor_epoch(self):
        """Returns the epoch time to use as the schedule anchor, or None
        for "no specific start time" (logger defaults to plain wall-clock
        alignment). The exact date used doesn't matter mathematically --
        the logger only ever uses this modulo the interval -- so "today"
        at the picked time-of-day is as good an anchor as any."""
        if not self.align_checkbox.isChecked():
            return None
        qtime = self.align_time_edit.time()
        local_dt = datetime.datetime.combine(
            datetime.date.today(),
            datetime.time(qtime.hour(), qtime.minute(), qtime.second()),
        )
        return int(local_dt.timestamp())

    def _describe_schedule(self, anchor_epoch):
        if not anchor_epoch:
            return "wall-clock aligned (e.g. on the hour)"
        aligned_dt = datetime.datetime.fromtimestamp(anchor_epoch)
        return f"aligned to {aligned_dt.strftime('%H:%M:%S')}"

    def _on_set_frequency_clicked(self):
        if self._worker is None:
            return
        target = self.freq_target_combo.currentData()
        interval_sec = self.freq_spinbox.value()
        anchor_epoch = self._compute_anchor_epoch()
        logger.info("Requesting new schedule for %s: %d sec, %s",
                     target, interval_sec, self._describe_schedule(anchor_epoch))
        # Immediate feedback that the click did something, since the
        # Interval column itself only updates once freq_received actually
        # confirms it (per sensor) -- without this, a slow or dropped
        # reply makes the button look like it did nothing at all.
        self.statusBar().showMessage(f"Requesting schedule for {target}...")
        self.set_freq_button.setEnabled(False)
        self._freq_request_confirmed = False
        QTimer.singleShot(REQUEST_TIMEOUT_MS, self._check_freq_request_timeout)
        self._worker.send_set_frequency(target, interval_sec, anchor_epoch)

    def _check_freq_request_timeout(self):
        if self._worker is not None:
            self.set_freq_button.setEnabled(True)
        if not self._freq_request_confirmed:
            self.statusBar().showMessage(
                "No response from logger to the frequency request -- it may not have gone through.", 6000)

    def _on_freq_received(self, mac, interval_sec, anchor_epoch):
        logger.info("Logger confirmed schedule for %s: %d sec, %s",
                     mac, interval_sec, self._describe_schedule(anchor_epoch))
        self._freq_request_confirmed = True
        row = self._ensure_row(mac)
        interval_item = self.table.item(row, COL_INTERVAL)
        interval_item.setText(str(interval_sec))
        interval_item.setToolTip(self._describe_schedule(anchor_epoch).capitalize())
        self._sensor_state[mac]["interval_sec"] = interval_sec
        self._refresh_activity_column()
        self.statusBar().showMessage(f"Schedule confirmed for {mac}: {interval_sec}s", 4000)

    # ----------------------------------------------------------------
    # Sensor registration
    # ----------------------------------------------------------------

    def _load_sensor_mac_history(self):
        stored = self._settings.value(SETTINGS_KEY_SENSOR_HISTORY)
        if not stored:
            return list(SEED_SENSOR_MACS)
        # QSettings hands back a bare str (not a 1-element list) when only
        # one value was ever stored -- normalize so callers always get a list.
        return [stored] if isinstance(stored, str) else list(stored)

    def _remember_sensor_mac(self, mac):
        if mac in self._sensor_mac_history:
            return
        self._sensor_mac_history.append(mac)
        self._settings.setValue(SETTINGS_KEY_SENSOR_HISTORY, self._sensor_mac_history)

    def _on_add_sensor_clicked(self):
        if self._worker is None:
            return
        # Editable combo box: pick a previously-seen MAC from the dropdown,
        # or type a brand new one -- either way it lands in the same text field.
        mac, ok = QInputDialog.getItem(
            self, "Add Sensor", "Sensor MAC address (select or type xx:xx:xx:xx:xx:xx):",
            self._sensor_mac_history, 0, True,
        )
        if not ok or not mac.strip():
            return
        mac = mac.strip()
        if not is_valid_mac(mac):
            QMessageBox.warning(
                self, "Add Sensor",
                f"\"{mac}\" doesn't look like a MAC address (expected xx:xx:xx:xx:xx:xx)."
            )
            return
        logger.info("Requesting logger register new sensor %s", mac)
        # Same reasoning as Set Frequency: give immediate feedback that
        # the click did something, and a bounded timeout so a dropped/
        # unanswered ADDSENSOR doesn't leave the button looking dead.
        self.statusBar().showMessage(f"Requesting to add sensor {mac}...")
        self.add_sensor_button.setEnabled(False)
        self._add_sensor_confirmed = False
        QTimer.singleShot(REQUEST_TIMEOUT_MS, self._check_add_sensor_timeout)
        self._worker.send_add_sensor(mac)

    def _check_add_sensor_timeout(self):
        if self._worker is not None:
            self.add_sensor_button.setEnabled(True)
        if not self._add_sensor_confirmed:
            self.statusBar().showMessage(
                "No response from logger to the Add Sensor request -- it may not have gone through.", 6000)

    def _on_add_sensor_result(self, status, mac):
        self._add_sensor_confirmed = True
        if status == "ok":
            logger.info("Logger registered new sensor %s", mac)
            self.statusBar().showMessage(f"Added sensor {mac}", 5000)
            return
        reasons = {
            "already_registered": "is already registered",
            "table_full": "couldn't be added -- the sensor table is full",
            "peer_add_failed": "couldn't be added -- ESP-NOW peer registration failed",
            "bad_mac": "isn't a valid MAC address",
        }
        logger.warning("Add sensor %s failed: %s", mac, status)
        QMessageBox.warning(self, "Add Sensor", f"{mac} {reasons.get(status, status)}.")

    # ----------------------------------------------------------------
    # Sensor table
    # ----------------------------------------------------------------

    def _ensure_row(self, mac):
        """Returns the row index for `mac`, creating a new row (and
        tracking state for it) if this is the first time we've seen it."""
        if mac in self._row_for_mac:
            return self._row_for_mac[mac]

        self._remember_sensor_mac(mac)

        row = self.table.rowCount()
        self.table.insertRow(row)

        mac_item = QTableWidgetItem(mac)
        mac_item.setFont(QFont("Monospace"))
        self.table.setItem(row, COL_MAC, mac_item)

        self.table.setItem(row, COL_READING, QTableWidgetItem("--"))

        status_item = QTableWidgetItem("Waiting for data")
        status_item.setForeground(QColor(COLOR_MUTED))
        status_font = status_item.font()
        status_font.setBold(True)
        status_item.setFont(status_font)
        self.table.setItem(row, COL_STATUS, status_item)

        self.table.setItem(row, COL_INTERVAL, QTableWidgetItem("--"))
        self.table.setItem(row, COL_ACTIVITY, QTableWidgetItem())
        self.table.setItem(row, COL_UPDATED, QTableWidgetItem("--"))

        self._row_for_mac[mac] = row
        self._sensor_state[mac] = {"last_data_ts": None, "interval_sec": None, "provisioning": False}
        self._refresh_freq_target_combo(mac)
        self._update_activity_cell(mac)
        return row

    def _on_sensors_received(self, macs):
        logger.info("Logger reports %d registered sensor(s): %s", len(macs), macs)
        for mac in macs:
            self._ensure_row(mac)

    def _on_data_received(self, mac, distance_mm):
        row = self._ensure_row(mac)
        status = stock_status_for(distance_mm)
        timestamp_text = time.strftime("%H:%M:%S")

        self.table.item(row, COL_READING).setText(str(distance_mm))

        status_item = self.table.item(row, COL_STATUS)
        status_item.setText(status)
        status_item.setForeground(QColor(STATUS_COLORS[status]))

        self.table.item(row, COL_UPDATED).setText(timestamp_text)

        state = self._sensor_state[mac]
        state["last_data_ts"] = time.time()
        state["provisioning"] = False
        self._update_activity_cell(mac)

    def _on_provisioning_started(self, mac):
        logger.info("Sensor %s is negotiating its reporting interval", mac)
        self._ensure_row(mac)
        self._sensor_state[mac]["provisioning"] = True
        self._update_activity_cell(mac)

    # --- Activity column: inferred, not a live ping -------------------
    # A sleeping sensor can't be asked "are you awake" without keeping
    # its radio on (which would defeat the point of deep sleep). Instead
    # this compares "when did we last hear from it" against "how often
    # is it supposed to report" to show a reasonable guess.

    def _compute_activity(self, mac):
        """Returns (text, color) for mac's current Activity cell."""
        state = self._sensor_state.get(mac, {})
        if state.get("provisioning"):
            return "Provisioning...", COLOR_WARN

        last_ts = state.get("last_data_ts")
        if last_ts is None:
            return "Waiting for first reading", COLOR_MUTED

        interval_sec = state.get("interval_sec")
        elapsed = time.time() - last_ts
        if interval_sec is None:
            return f"Last seen {int(elapsed)}s ago", COLOR_MUTED

        if elapsed >= interval_sec * OVERDUE_GRACE_FACTOR:
            return f"Overdue (expected ~{int(elapsed - interval_sec)}s ago)", COLOR_BAD

        remaining = max(0, interval_sec - elapsed)
        return f"Asleep (next in ~{int(remaining)}s)", COLOR_GOOD

    def _update_activity_cell(self, mac):
        row = self._row_for_mac.get(mac)
        if row is None:
            return
        text, color = self._compute_activity(mac)
        item = self.table.item(row, COL_ACTIVITY)
        item.setText(text)
        item.setForeground(QColor(color))

    def _refresh_activity_column(self):
        for mac in self._row_for_mac:
            self._update_activity_cell(mac)

    # ----------------------------------------------------------------

    def closeEvent(self, event):
        # Make sure the background thread shuts down cleanly instead of
        # being killed out from under itself when the process exits.
        self._disconnect()
        super().closeEvent(event)
