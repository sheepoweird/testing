#!/usr/bin/env python3
"""
Pico CDC Serial Monitor - Enhanced with infinite auto-retry
Only connects to CDC and listens for printf output from Raspberry Pi Pico
"""

import time
import sys
import os
from datetime import datetime

# Try to import serial, provide helpful message if not available
try:
    import serial
    import serial.tools.list_ports

    SERIAL_AVAILABLE = True
except ImportError:
    SERIAL_AVAILABLE = False
    print("❌ pyserial not installed!")
    print("💡 Run: pip install pyserial")
    print("   or download from: https://pypi.org/project/pyserial/")
    sys.exit(1)


class PicoSerialMonitor:
    def __init__(self, pico_port=None, baudrate=115200, log_file="pico_printf.log"):
        self.pico_port = pico_port
        self.baudrate = baudrate
        self.log_file = log_file
        self.running = False
        self.serial_connection = None
        self.start_time = time.time()
        self.error_count = 0
        self.max_errors = 10
        self.retry_count = 0
        self.retry_delay = 3  # seconds between retries
        self.last_successful_connection = None
        self.total_reconnections = 0

    def find_pico_port(self):
        """Find the Pico's CDC serial port"""
        if not SERIAL_AVAILABLE:
            return None

        ports = serial.tools.list_ports.comports()

        print("Scanning COM ports for Pico...")

        for port in ports:
            # Skip Bluetooth ports
            if 'Bluetooth' in port.description.upper() or 'BT' in port.description.upper():
                continue

            print(f"  {port.device}: {port.description}")

            # Look for Pico identifiers
            if 'Pico' in port.description or 'RP2040' in port.description:
                return port.device

            # Look for USB Serial devices (common Pico pattern)
            if 'USB' in port.description.upper() and 'Serial' in port.description:
                return port.device

        return None

    def connect_to_pico(self):
        """Establish serial connection to Pico with auto-retry"""
        if not SERIAL_AVAILABLE:
            print("❌ pyserial not available")
            return False

        if not self.pico_port:
            self.pico_port = self.find_pico_port()

        if not self.pico_port:
            print("\n❌ Could not find Pico automatically!")
            # Don't exit, just keep trying to find the port
            print("🔄 Will keep retrying port discovery...")
            return False

        print(f"\n✓ Selected port: {self.pico_port}")

        try:
            print(f"Opening {self.pico_port} at {self.baudrate} baud...")
            self.serial_connection = serial.Serial(
                self.pico_port,
                self.baudrate,
                timeout=0.1,  # Shorter timeout
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                rtscts=False,  # Disable hardware flow control
                dsrdtr=False,  # Disable hardware flow control
                xonxoff=False  # Disable software flow control
            )

            # Wait for connection to stabilize
            time.sleep(2)

            # Clear any buffered data
            if self.serial_connection.in_waiting:
                self.serial_connection.reset_input_buffer()

            print(f"✓ Connected to {self.pico_port}")
            print("📡 Listening for Pico printf output...")
            print("-" * 60)
            self.error_count = 0  # Reset error count on successful connection
            self.retry_count = 0  # Reset retry count on successful connection
            self.last_successful_connection = time.time()
            self.total_reconnections += 1
            return True

        except PermissionError:
            print(f"\n❌ Permission denied! Port {self.pico_port} is already in use!")
            print("   Close other serial terminals using this port.")
            return False
        except Exception as e:
            print(f"❌ Could not open serial port: {e}")
            return False

    def log_to_file(self, data):
        """Write data to log file with timestamp"""
        try:
            with open(self.log_file, 'a', encoding='utf-8') as f:
                timestamp = datetime.now().strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]
                f.write(f"[{timestamp}] {data}\n")
                f.flush()
        except Exception as e:
            print(f"Error writing to log file: {e}")

    def safe_read_serial(self):
        """Safely read from serial port with comprehensive error handling"""
        if not self.serial_connection or not self.serial_connection.is_open:
            return None

        try:
            # Check if data is available
            if self.serial_connection.in_waiting > 0:
                # Read available data
                data = self.serial_connection.readline()
                if data:
                    try:
                        decoded = data.decode('utf-8', errors='ignore').strip()
                        if decoded:  # Only return non-empty lines
                            return decoded
                    except UnicodeDecodeError:
                        # Handle binary data or corruption
                        hex_data = data.hex()
                        return f"[BINARY_DATA: {hex_data}]"
            return None

        except serial.SerialException as e:
            self.error_count += 1
            if self.error_count <= 3:  # Only show first few errors
                print(f"⚠️  Serial communication error: {e}")
            elif self.error_count == 4:
                print("⚠️  Multiple serial errors occurred...")

            if self.error_count > self.max_errors:
                print("❌ Too many serial errors. Reconnecting...")
                self.auto_reconnect()
            return None

        except Exception as e:
            self.error_count += 1
            if self.error_count <= 3:
                print(f"⚠️  Unexpected error reading serial: {e}")
            return None

    def auto_reconnect(self):
        """Automatically attempt to reconnect forever with exponential backoff"""
        self.retry_count += 1

        # Calculate backoff delay (exponential with max of 60 seconds)
        delay = min(self.retry_delay * (2 ** (self.retry_count - 1)), 1)

        print(f"🔄 Auto-reconnect attempt {self.retry_count} in {delay} seconds...")
        print("   Press Ctrl+C to stop monitoring entirely")

        # Countdown with ability to interrupt
        for i in range(delay, 0, -1):
            try:
                print(f"   Retrying in {i} seconds...", end='\r')
                time.sleep(1)
            except KeyboardInterrupt:
                print("\n\n✋ Monitoring stopped by user")
                self.running = False
                return False

        print("   " + " " * 30 + "\r", end='')  # Clear countdown line

        self.stop_serial()

        if self.connect_to_pico():
            print("✓ Auto-reconnect successful!")
            return True
        else:
            print("❌ Auto-reconnect failed, will try again...")
            return False

    def reconnect(self):
        """Manual reconnect (for backward compatibility)"""
        print("🔄 Attempting to reconnect...")
        self.stop_serial()
        time.sleep(2)

        if self.connect_to_pico():
            print("✓ Reconnected successfully")
            return True
        else:
            print("❌ Failed to reconnect, will keep trying...")
            return False

    def stop_serial(self):
        """Safely close serial connection"""
        if self.serial_connection and self.serial_connection.is_open:
            try:
                self.serial_connection.close()
            except:
                pass
            self.serial_connection = None

    def read_serial_data(self):
        """Read and process serial data"""
        line = self.safe_read_serial()
        if line:
            # Display with timestamp
            timestamp = datetime.now().strftime('%H:%M:%S')
            print(f"[{timestamp}] {line}")

            # Log to file
            self.log_to_file(line)

            # Reset error count on successful read
            self.error_count = 0
            self.retry_count = 0  # Also reset retry count on successful data

    def check_connection_health(self):
        """Check if connection is still healthy"""
        if not self.serial_connection or not self.serial_connection.is_open:
            return False

        # If we haven't received data in a long time, connection might be dead
        if (self.last_successful_connection and
                time.time() - self.last_successful_connection > 60):
            print("⚠️  No data received for 60 seconds. Checking connection...")
            try:
                # Try to write a harmless character to test connection
                self.serial_connection.write(b'\n')
                return True
            except:
                print("❌ Connection test failed")
                return False

        return True

    def ensure_connection(self):
        """Ensure we have a connection, retry forever if needed"""
        while self.running and (not self.serial_connection or not self.serial_connection.is_open):
            if not self.connect_to_pico():
                # If connection fails, use auto_reconnect which will retry forever
                if not self.auto_reconnect():
                    break  # Only break if user pressed Ctrl+C
            else:
                break

    def monitor_serial(self):
        """Main monitoring loop for serial data with infinite auto-retry"""
        print("=" * 60)
        print("  PICO CDC SERIAL MONITOR WITH INFINITE AUTO-RETRY")
        print("=" * 60)
        print("This tool only listens for printf output from Pico")
        print("No health data is being sent to the Pico")
        print("♾️  Auto-reconnect: INFINITE (never gives up)")
        print()

        # Create/clear log file with header
        try:
            with open(self.log_file, 'w', encoding='utf-8') as f:
                f.write(f"PICO PRINTF LOG - Started {datetime.now().isoformat()}\n")
                f.write(f"Port: {self.pico_port}\n")
                f.write(f"Baudrate: {self.baudrate}\n")
                f.write("=" * 80 + "\n\n")
            print(f"📄 Logging to: {os.path.abspath(self.log_file)}")
        except Exception as e:
            print(f"Error creating log file: {e}")

        self.running = True
        print("\n🎯 Ready! Waiting for Pico output...")
        print("Press Ctrl+C to stop monitoring\n")

        last_activity_time = time.time()
        connection_check_interval = 10  # Check connection every 10 seconds
        last_connection_check = time.time()

        try:
            while self.running:
                # Ensure we have a connection before trying to read
                self.ensure_connection()

                if not self.running:
                    break

                # Read and display any serial data
                self.read_serial_data()

                # Update last activity time if we received data
                if self.serial_connection and self.serial_connection.in_waiting:
                    last_activity_time = time.time()
                    self.last_successful_connection = time.time()

                # Check connection health periodically
                current_time = time.time()
                if current_time - last_connection_check > connection_check_interval:
                    if not self.check_connection_health():
                        print("⚠️  Connection health check failed")
                        self.auto_reconnect()
                    last_connection_check = current_time

                # Check for extended inactivity (connection might be dead)
                if current_time - last_activity_time > 30:
                    if self.serial_connection and self.serial_connection.is_open:
                        # Send a keep-alive or check connection
                        try:
                            self.serial_connection.write(b'\n')  # Send newline to trigger response
                        except:
                            print("⚠️  Keep-alive failed, connection may be lost")
                            self.auto_reconnect()
                    else:
                        # No connection, ensure we try to reconnect
                        self.ensure_connection()

                # Small delay to prevent CPU hogging
                time.sleep(0.05)

        except KeyboardInterrupt:
            print("\n\n✋ Monitoring stopped by user")
        except Exception as e:
            print(f"\n\n❌ Unexpected error during monitoring: {e}")
            print("🔄 Will attempt to recover and continue...")
            # Don't stop on errors, just try to reconnect
            if self.running:
                time.sleep(2)
                self.monitor_serial()  # Restart monitoring
        finally:
            self.stop()

    def stop(self):
        """Stop monitoring and close connection"""
        self.running = False
        self.stop_serial()

        # Write final log entry
        try:
            with open(self.log_file, 'a', encoding='utf-8') as f:
                f.write(f"\nMonitoring ended at {datetime.now().isoformat()}\n")
                f.write(f"Total runtime: {(time.time() - self.start_time):.1f} seconds\n")
                f.write(f"Total reconnections: {self.total_reconnections}\n")
                f.write("=" * 80 + "\n")
        except:
            pass

        print(f"📄 Log saved to: {os.path.abspath(self.log_file)}")
        print(f"⏱️  Total runtime: {(time.time() - self.start_time):.1f} seconds")
        print(f"🔄 Total reconnections: {self.total_reconnections}")

    def send_command(self, command):
        """Optional: Send a command to Pico if needed"""
        if not self.serial_connection:
            print("Not connected to Pico")
            return False

        try:
            self.serial_connection.write((command + '\n').encode('utf-8'))
            self.serial_connection.flush()
            print(f"Sent: {command}")
            return True
        except Exception as e:
            print(f"Error sending command: {e}")
            return False


def main():
    import argparse

    parser = argparse.ArgumentParser(
        description='Pico CDC Serial Monitor - Listen for printf output with infinite auto-retry')
    parser.add_argument('--port', '-p', default=None,
                        help='COM port (e.g., COM9). Auto-detect if not specified')
    parser.add_argument('--baud', '-b', type=int, default=115200,
                        help='Baud rate (default: 115200)')
    parser.add_argument('--file', '-f', default='pico_printf.log',
                        help='Log file name (default: pico_printf.log)')
    parser.add_argument('--send', '-s', default=None,
                        help='Send a command to Pico and exit')
    parser.add_argument('--retry-delay', '-d', type=int, default=3,
                        help='Initial retry delay in seconds (default: 3)')

    args = parser.parse_args()

    monitor = PicoSerialMonitor(
        pico_port=args.port,
        baudrate=args.baud,
        log_file=args.file
    )

    # Set retry parameters from command line
    monitor.retry_delay = args.retry_delay

    if args.send:
        # Just send a command and exit
        if monitor.connect_to_pico():
            monitor.send_command(args.send)
            time.sleep(0.5)
            # Read any immediate response
            start_time = time.time()
            while time.time() - start_time < 2:  # Read for 2 seconds
                monitor.read_serial_data()
                time.sleep(0.1)
        monitor.stop()
    else:
        # Start continuous monitoring
        monitor.monitor_serial()


if __name__ == "__main__":
    main()