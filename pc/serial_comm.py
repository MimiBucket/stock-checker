"""
Serial communication layer for the desktop app.

The logger board talks plain-text, newline-terminated lines over USB 
serial at 115200 baud (see logger_node/components/pc_comm). This module
is the only place in the app that touches the actual serial port.

Threading model
----------------
Reading from a serial port is a *blocking* operation (readline() waits
for bytes to show up), so it can't run on the same thread as the GUI --
if it did, the whole window would freeze every time we're waiting on
data. So all serial I/O happens on a background thread, implemented as
SerialWorker(QThread).

The background thread is not allowed to touch any GUI widgets directly
(Qt widgets are not thread-safe). Instead:

  * Background -> GUI: SerialWorker emits Qt *signals* (connected,
    disconnected, data_received, sensors_received, freq_received,
    provisioning_started). Qt automatically delivers a signal to a slot
    on another thread safely, queuing it to run on the receiving
    object's thread. ui.py connects
    these signals to methods that update widgets, so widget updates
    always happen on the GUI thread even though the data originated on
    the background thread.

  * GUI -> background: the GUI thread never calls into the worker's
    Serial object directly. Instead it calls worker.send_line(...),
    which just drops the text onto a thread-safe queue.Queue. The
    worker thread picks queued lines up and writes them to the port.
    queue.Queue is designed to be safely shared between threads, so
    this needs no extra locking.
"""

import logging
import queue
import re
import threading
import time

import serial
from serial.tools import list_ports
from PySide6.QtCore import QThread, Signal

logger = logging.getLogger(__name__)

DEFAULT_BAUD_RATE = 115200

# How long ser.read() will block waiting for at least one byte before
# giving up and looping again. Keeps the read loop responsive to stop()
# without busy-waiting.
READ_TIMEOUT_SEC = 0.2

# ESP32 dev boards reset when a serial connection is first opened (the
# USB-serial chip toggles DTR, which is wired to EN/RESET). The logger
# then re-runs app_main() and won't respond to anything until it has
# rebooted, so we wait a bit before sending the first command.
BOOT_SETTLE_SEC = 2.0

# When auto-detecting, how long to wait after sending SETTIME to a
# candidate port for it to reply with something recognizable (FREQ,
# SENSORS, or DATA) before giving up on that port and trying the next
# one. Needs to be comfortably longer than the logger's own startup
# sequence (time sync -> FREQ -> ESP-NOW init -> SENSORS).
PROBE_TIMEOUT_SEC = 3.0

# If no candidate port matches on a scan pass (or there were no ports at
# all), how long to wait before re-listing ports and trying again. This
# is what lets the app notice a logger that gets plugged in later.
RESCAN_INTERVAL_SEC = 2.0

# A candidate port only counts as "confirmed" once it sends a line
# starting with one of these -- i.e. it's actually speaking our protocol,
# not just any device that happens to print text (e.g. a sensor node's
# own plain ESP_LOG console output, which is unrelated chatter as far as
# the PC app is concerned).
_RECOGNIZED_LINE_PREFIXES = ("DATA ", "SENSORS ", "FREQ ", "PROVISIONING ")

# Matches "xx:xx:xx:xx:xx:xx" (case-insensitive hex). Used to reject a
# malformed/corrupted field instead of letting garbage into the UI as if
# it were a real MAC -- e.g. two lines that got merged on the wire (a
# dropped newline) would otherwise slip a whole extra "SENSORS ..." tail
# through as if it were one long, bogus MAC address.
_MAC_RE = re.compile(r"^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$")


def _is_valid_mac(text):
    return bool(_MAC_RE.match(text))


def list_available_ports():
    """
    Returns a list of (device, description) tuples for every serial port
    currently visible to the OS, e.g. [("/dev/ttyUSB0", "CP2102 USB to
    UART Bridge"), ...]. Works the same way on Windows/macOS/Linux.
    """
    ports = list_ports.comports()
    return [(p.device, p.description) for p in ports]


class SerialWorker(QThread):
    """
    Owns one serial connection and runs entirely on a background thread.

    Usage: create one instance per connection attempt (it's cheap), wire
    up its signals, then call .start() (inherited from QThread) to launch
    run() on the background thread. Call .stop() to ask it to shut down
    and block until it has.

    If port_name is None, this worker auto-detects the logger: it tries
    every serial port the OS currently reports, one at a time, and
    confirms a match not by guessing from the port's name/description
    (which varies by USB-serial chip and OS) but by actually speaking
    the protocol -- it sends SETTIME and waits briefly for a recognized
    reply. If nothing matches, it waits and tries again, so a logger
    plugged in after the app starts is still picked up automatically.
    If port_name is a specific device path, only that port is tried
    (used when the user overrides auto-detect from the port dropdown).
    """

    # --- Signals: background thread -> GUI thread ------------------- 
    # the seven "channels" the background thread uses to talk to the GUI thread. The GUI thread connects these signals to slots that update widgets, so the background thread never touches any Qt widgets directly.
    connected = Signal(str)              # port name that just opened
    disconnected = Signal(str)           # human-readable reason (a *confirmed* connection was lost)
    scanning = Signal(str)               # human-readable status while still searching for the logger 
    data_received = Signal(str, int)     # mac address, distance_mm (DATA)
    sensors_received = Signal(list)      # list[str] of mac addresses (SENSORS)
    freq_received = Signal(str, int, int) # mac address, interval_seconds, anchor_epoch (FREQ)
    provisioning_started = Signal(str)   # mac address of sensor that just started provisioning (PROVISIONING)

    def __init__(self, port_name=None, baud_rate=DEFAULT_BAUD_RATE, parent=None):
        super().__init__(parent)
        self._port_name = port_name
        self._baud_rate = baud_rate

        # Lines waiting to be written to the logger. Only ever touched
        # via .put() (from the GUI thread, through send_line/send_set_frequency)
        # and .get_nowait() (from this worker's own thread in run()).
        self._outbox = queue.Queue()

        # Set from the GUI thread (via stop()) to ask run()'s loop to exit.
        self._stop_event = threading.Event()

        # Whether the port currently being tried has been confirmed as a
        # real logger. Only ever touched on this worker's own
        # thread, from inside _probe_and_run/_read_loop.
        self._session_confirmed = False

    # --- Public API, safe to call from the GUI thread ----------------

    def send_line(self, text): # pc -> logger
        """Queue a raw line to be sent to the logger (newline optional)."""
        self._outbox.put(text)

    def send_set_frequency(self, target, interval_sec, anchor_epoch=None): # pc -> logger
        """Ask the logger to change the reporting schedule. target is either
        a MAC address string (that one sensor only) or the literal "ALL"
        (every currently registered sensor). anchor_epoch is an optional
        Unix timestamp that every wake is aligned to (valid wake times are
        anchor_epoch + k*interval_sec) -- e.g. to get hourly readings on
        the hour, pass the epoch time of any past or future top-of-the-hour.
        Omit it (or pass None) for plain wall-clock alignment (anchor 0,
        e.g. every hour on the hour with no specific reference time)."""
        if anchor_epoch is None:
            self.send_line(f"SETFREQ {target} {int(interval_sec)}")
        else:
            self.send_line(f"SETFREQ {target} {int(interval_sec)} {int(anchor_epoch)}")

    def stop(self): # use case: GUI is closing, or user hit "Disconnect"
        """Ask the read loop to exit, then block until the thread has
        actually finished (with a generous timeout so the GUI never
        hangs forever on shutdown)."""
        self._stop_event.set()
        self.wait(2000)

    # --- Runs on the background thread --------------------------------

    def run(self):
        # A fixed, non-None port_name means "only ever try this one port" (manual override); 
        # None means "keep listing the OS's ports and try each of them" so newly-plugged-in devices get picked up.
        pinned_port = self._port_name  # e.g.) "/dev/ttyUSB0" or "COM3" or None

        while not self._stop_event.is_set():
            candidates = [pinned_port] if pinned_port else [dev for dev, _ in list_available_ports()]

            if not candidates:
                self.scanning.emit("No serial ports found -- waiting...")

            for port_name in candidates:
                if self._stop_event.is_set():
                    break 
                self.scanning.emit(f"Checking {port_name}...")

                if self._probe_and_run(port_name):
                    # Either ran a full confirmed session until it ended,
                    # or was told to stop mid-probe -- either way, done.
                    break 

            # None of this pass's candidates panned out; wait a bit
            # before re-listing ports and trying again.
            if self._stop_event.wait(RESCAN_INTERVAL_SEC):
                return

    def _probe_and_run(self, port_name):
        """
        Opens port_name, sends SETTIME, and waits up to PROBE_TIMEOUT_SEC
        for a recognized reply to confirm it's actually a logger.

        If confirmed: emits `connected` and stays in the normal read loop
        for as long as the connection lasts, emitting `disconnected` only
        if it's later lost. Returns True (this worker's job is done,
        whether that ended in a clean stop() or a real disconnect).

        If not confirmed (open failed, or nothing recognizable arrived
        in time): closes the port quietly -- no `disconnected` signal,
        since from the UI's point of view we were never connected --
        and returns False so the caller moves on to the next candidate.
        """
        try:
            ser = serial.Serial(port_name, self._baud_rate, timeout=READ_TIMEOUT_SEC)
        except serial.SerialException as exc:
            logger.debug("Could not open %s: %s", port_name, exc)
            return False

        logger.debug("Opened %s, probing for a logger...", port_name)

        # Tracked on self (not a local) so it's still readable in the
        # except block below even if the exception was raised partway
        # through _read_loop -- a local variable only gets whatever
        # _read_loop returns, and a function that raises never reaches
        # its own return statement, so a local would still read as
        # "never confirmed" here even for a connection that very much
        # had been confirmed moments earlier. This flag is set the
        # instant `connected` is emitted, inside _read_loop below.
        self._session_confirmed = False
        try:
            time.sleep(BOOT_SETTLE_SEC)
            # The logger blocks at startup until it receives this -- see
            # pc_comm_wait_for_initial_sync() in the logger firmware.
            # Sending it is also our probe: only a real logger will
            # reply to it. This whole block -- including this initial
            # write -- has to be inside the try, not just the read loop:
            # a port that dies right after open() would otherwise raise
            # here uncaught and crash this thread instead of being
            # treated like any other failed candidate.
            self._flush_outbox(ser)  # in case something was queued before we even opened
            self._write_line(ser, f"SETTIME {int(time.time())}") # write line in serial, then wait for a response in the read loop
            self._read_loop(ser, port_name, probe_deadline=time.time() + PROBE_TIMEOUT_SEC) # read lines until stop() is called by the GUI thread, the connection drops, or the probe deadline passes
        except serial.SerialException as exc:
            if self._session_confirmed:
                logger.error("Serial error on %s: %s", port_name, exc)
                self.disconnected.emit(f"Lost connection to {port_name}: {exc}")
            else:
                logger.debug("Error probing %s: %s", port_name, exc)
        finally:
            ser.close()
            logger.info("Closed serial port %s", port_name)

        # Whether we end up here via a clean stop(), a confirmed-then-lost
        # connection, or exhausting the probe window, this candidate's
        # story is over -- either it was THE logger (done either way) or
        # it never will be (caller should try the next one instead).
        return self._session_confirmed or self._stop_event.is_set()

    def _read_loop(self, ser, port_name, probe_deadline):
        """
        Reads lines until stopped, the connection drops, or (while still
        unconfirmed) the probe deadline passes. Sets self._session_confirmed
        and emits `connected` as soon as the first recognized line arrives.
        """
        # Bytes received so far that don't yet form a complete line. We
        # buffer manually (rather than relying on pyserial's readline())
        # because readline() with a timeout can hand back a partial line
        # if the timeout elapses mid-message -- buffering and splitting
        # on '\n' ourselves means a message is never processed until it
        # has fully arrived, no matter how it gets chunked across reads.
        buffer = b""

        while not self._stop_event.is_set():
            if not self._session_confirmed and time.time() > probe_deadline:
                logger.debug("%s did not respond like a logger", port_name)
                break

            self._flush_outbox(ser)

            # If there's already data waiting, read all of it immediately;
            # otherwise ask for 1 byte, which blocks up to READ_TIMEOUT_SEC
            # and returns b"" on timeout -- that's what keeps this loop
            # checking the stop/deadline conditions regularly instead of
            # blocking forever.
            chunk = ser.read(ser.in_waiting or 1)
            if not chunk:
                continue
            buffer += chunk

            while b"\n" in buffer:
                raw_line, buffer = buffer.split(b"\n", 1)
                text = raw_line.decode("ascii", errors="replace").strip()
                if not text:
                    continue
                logger.debug("Received: %s", text)
                # Only a line that actually matches the protocol counts as
                # confirmation -- anything else (e.g. a sensor node's own
                # plain ESP_LOG console output, if we happened to open its
                # port instead of the logger's) must NOT be treated as
                # proof this is the logger, or we'd falsely lock onto the
                # wrong device just because it printed something.
                if not self._session_confirmed and text.startswith(_RECOGNIZED_LINE_PREFIXES):
                    self._session_confirmed = True
                    logger.info("Confirmed logger found on %s", port_name)
                    self.connected.emit(port_name)
                self._handle_line(text)

    def _write_line(self, ser, text):
        line = text if text.endswith("\n") else text + "\n"
        ser.write(line.encode("ascii"))
        logger.debug("Sent: %s", text)

    def _flush_outbox(self, ser):
        while True:
            try:
                text = self._outbox.get_nowait()
            except queue.Empty:
                return
            self._write_line(ser, text)

    def _handle_line(self, text):
        if text.startswith("DATA "):  
            self._handle_data_line(text)
        elif text.startswith("SENSORS "):
            self._handle_sensors_line(text)
        elif text.startswith("FREQ "):
            self._handle_freq_line(text)
        elif text.startswith("PROVISIONING "):
            self._handle_provisioning_line(text)
        else:
            logger.debug("Ignoring unrecognized line: %r", text)

    
    def _handle_data_line(self, text):
        """
            DATA <mac> <distance_mm>
            Example: "DATA  12:34:56:78:9A:BC 120mm"
        """
        parts = text[len("DATA "):].split()
        if len(parts) != 2 or not _is_valid_mac(parts[0]):
            logger.warning("Malformed DATA line: %r", text)
            return
        mac, distance_str = parts
        try:
            distance_mm = int(distance_str)
        except ValueError:
            logger.warning("Malformed DATA line: %r", text)
            return
        self.data_received.emit(mac, distance_mm) # .emit() is like marking message onto the signal.

    def _handle_sensors_line(self, text):
        """
            SENSORS <mac1>,<mac2>,<mac3>,...
            Example: "SENSORS  12:34:56:78:9A:BC,AB:CD:EF:01:23:45"
        """
        tokens = [mac for mac in text[len("SENSORS "):].split(",") if mac]
        macs = [mac for mac in tokens if _is_valid_mac(mac)]
        if len(macs) != len(tokens):
            logger.warning("Malformed SENSORS line, dropping bad entries: %r", text)
        self.sensors_received.emit(macs)

    def _handle_freq_line(self, text):
        """
            FREQ <mac> <interval_seconds> <anchor_epoch>
            Example: "FREQ  12:34:56:78:9A:BC 5 0"
        """
        parts = text[len("FREQ "):].split()
        if len(parts) != 3 or not _is_valid_mac(parts[0]):
            logger.warning("Malformed FREQ line: %r", text)
            return
        mac, interval_str, anchor_str = parts
        try:
            interval_sec = int(interval_str)
            anchor_epoch = int(anchor_str)
        except ValueError:
            logger.warning("Malformed FREQ line: %r", text)
            return
        self.freq_received.emit(mac, interval_sec, anchor_epoch)

    def _handle_provisioning_line(self, text):
        """
            PROVISIONING <mac>
            Example: "PROVISIONING  12:34:56:78:9A:BC"

            Provisioning mode = sensor sends PKT_PROVISION_REQUEST (can i get my schedule) every 5s
            end of provisioning mode = logger sends PKT_PROVISION_ACK -> sensor saves interval to NVS
        """
        mac = text[len("PROVISIONING "):].strip()
        if _is_valid_mac(mac):
            self.provisioning_started.emit(mac)
        else:
            logger.warning("Malformed PROVISIONING line: %r", text)
