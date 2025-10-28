import psutil
import json
import time
import os
import sys
import serial
import serial.tools.list_ports
from datetime import datetime
from collections import deque


class HealthMonitorPico:
    def __init__(self, pico_port=None, interval=15, log_file="health_monitor.txt"):
        self.pico_port = pico_port
        self.interval = interval
        self.running = False
        self.network_history = deque(maxlen=10)
        self.pico_messages = deque(maxlen=10)  # Keep last 10 Pico messages
        self.start_time = time.time()
        self.sample_count = 0
        self.serial_connection = None
        self.write_timeout_count = 0
        self.max_consecutive_timeouts = 3

        # Determine base directory (works for .py and PyInstaller .exe)
        if getattr(sys, 'frozen', False):
            base_dir = os.path.dirname(sys.executable)
        else:
            base_dir = os.path.dirname(os.path.abspath(__file__))

        if not os.path.isabs(log_file):
            self.log_file = os.path.join(base_dir, log_file)
        else:
            self.log_file = log_file

    def find_pico_port(self):
        """Find the Pico's CDC serial port"""
        ports = serial.tools.list_ports.comports()

        print("Scanning COM ports...")

        usb_ports = []
        for port in ports:
            # Skip Bluetooth ports completely
            if 'Bluetooth' in port.description.upper() or 'BT' in port.description.upper():
                continue

            print(f"  {port.device}: {port.description}")

            # Look for Pico identifiers
            if 'Pico' in port.description or 'RP2040' in port.description:
                return port.device

            # Look for USB Serial devices
            if 'USB' in port.description.upper() and 'Serial' in port.description:
                usb_ports.append(port)

        if usb_ports:
            return usb_ports[0].device

        return None

    def connect_to_pico(self):
        """Establish serial connection to Pico"""
        if not self.pico_port:
            self.pico_port = self.find_pico_port()

        if not self.pico_port:
            print("\n❌ Could not find Pico automatically!")
            self.pico_port = input("Enter COM port (e.g., COM9): ").strip()

            if not self.pico_port:
                print("No port specified. Exiting.")
                return False

        print(f"\n✓ Selected port: {self.pico_port}")

        try:
            print(f"Opening {self.pico_port}...")
            self.serial_connection = serial.Serial(
                self.pico_port,
                115200,
                timeout=1,
                write_timeout=2.0,
                xonxoff=False,
                rtscts=False,
                dsrdtr=False
            )
            time.sleep(2)
            print(f"✓ Connected to {self.pico_port}\n")

            # Read and display Pico startup messages
            time.sleep(0.5)
            while self.serial_connection.in_waiting:
                try:
                    line = self.serial_connection.readline().decode('utf-8', errors='ignore').strip()
                    if line:
                        print(f"Pico: {line}")
                except:
                    pass

            return True

        except PermissionError:
            print(f"\n❌ Permission denied!")
            print("⚠️  The COM port is already in use!")
            print(f"   Close TeraTerm/PuTTY using {self.pico_port}, then run again.")
            return False
        except Exception as e:
            print(f"❌ Could not open serial port: {e}")
            return False

    def get_comprehensive_health_data(self):
        """Collect detailed system health information"""

        # CPU metrics
        cpu_percent = psutil.cpu_percent(interval=1, percpu=False)

        # Memory metrics
        memory = psutil.virtual_memory()

        # Disk metrics
        disk_usage = psutil.disk_usage('/')

        # Network metrics
        try:
            network_io = psutil.net_io_counters()
        except:
            network_io = None

        # Process count
        try:
            total_processes = len(psutil.pids())
        except:
            total_processes = 0

        # Calculate network speeds
        network_speed_in = network_speed_out = 0
        if network_io and len(self.network_history) > 0:
            prev_net = self.network_history[-1]
            time_diff = max(1, self.interval)
            network_speed_in = max(0, (network_io.bytes_recv - prev_net['bytes_recv']) / time_diff / 1024)
            network_speed_out = max(0, (network_io.bytes_sent - prev_net['bytes_sent']) / time_diff / 1024)

        if network_io:
            self.network_history.append({
                'bytes_recv': network_io.bytes_recv,
                'bytes_sent': network_io.bytes_sent,
                'timestamp': time.time()
            })

        # Simplified data for Pico
        pico_data = {
            'cpu': round(cpu_percent, 1),
            'memory': round(memory.percent, 1),
            'disk': round((disk_usage.used / disk_usage.total) * 100, 1),
            'net_in': round(network_speed_in, 1),
            'net_out': round(network_speed_out, 1),
            'processes': total_processes,
            'timestamp': int(time.time())
        }

        self.sample_count += 1
        return pico_data

    def send_to_pico(self, data):
        """Send JSON data to Pico with improved error handling"""
        if not self.serial_connection:
            return False

        try:
            json_data = json.dumps(data)

            # Check if there's too much data waiting to be read
            if self.serial_connection.in_waiting > 1000:
                try:
                    self.serial_connection.read(self.serial_connection.in_waiting)
                except:
                    pass

            self.serial_connection.write(json_data.encode('utf-8'))
            self.serial_connection.write(b'\n')
            self.serial_connection.flush()

            # Reset timeout counter on success
            self.write_timeout_count = 0
            return True

        except serial.SerialTimeoutException:
            self.write_timeout_count += 1
            self.pico_messages.append(f"⚠️  Write timeout ({self.write_timeout_count}/{self.max_consecutive_timeouts})")

            if self.write_timeout_count >= self.max_consecutive_timeouts:
                self.pico_messages.append("❌ Too many consecutive timeouts. Pico may be busy.")
                # Reset counter but continue
                self.write_timeout_count = 0

            return False

        except Exception as e:
            self.pico_messages.append(f"❌ Error sending to Pico: {e}")
            return False

    def read_pico_response(self, timeout_ms=100):
        """Read any response from Pico with timeout"""
        if not self.serial_connection:
            return

        start_time = time.time()
        try:
            while self.serial_connection.in_waiting:
                # Don't spend too long reading
                if (time.time() - start_time) * 1000 > timeout_ms:
                    break

                try:
                    line = self.serial_connection.readline().decode('utf-8', errors='ignore').strip()
                    if line:
                        # Add timestamp to message
                        timestamp = datetime.now().strftime('%H:%M:%S')
                        self.pico_messages.append(f"[{timestamp}] {line}")
                except:
                    break
        except:
            pass

    def log_to_file(self, data):
        """Write data to log file"""
        try:
            with open(self.log_file, 'a', encoding='utf-8') as f:
                timestamp = datetime.now().isoformat()
                f.write(f"[{self.sample_count:04d}] {timestamp} | ")
                f.write(f"CPU={data['cpu']:5.1f}% | RAM={data['memory']:5.1f}% | ")
                f.write(f"DISK={data['disk']:5.1f}% | ")
                f.write(f"NET=↓{data['net_in']:6.1f}↑{data['net_out']:6.1f} KB/s | ")
                f.write(f"PROC={data['processes']}\n")
                f.write(f"JSON: {json.dumps(data)}\n")
                f.write("-" * 80 + "\n")
                f.flush()

        except Exception as e:
            self.pico_messages.append(f"Error writing to log: {e}")

    def print_status(self, data):
        """Print current status to console with persistent Pico message log"""
        # Clear screen
        os.system('cls' if os.name == 'nt' else 'clear')

        # === STATIC HEADER SECTION (stays the same) ===
        print("=" * 70)
        print("  PC HEALTH MONITOR - PICO CDC MODE")
        print("=" * 70)
        print(f"Sample #{self.sample_count} | {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
        print(f"Connected to: {self.pico_port}")
        print(f"Logging to: {os.path.abspath(self.log_file)}")
        print(f"Runtime: {(time.time() - self.start_time) / 60:.1f} minutes")
        print("-" * 70)

        # === HEALTH DATA SECTION ===
        cpu = data['cpu']
        mem = data['memory']
        disk = data['disk']

        print(f"CPU:  {cpu:5.1f}% {'🔥' if cpu > 80 else '✅'}")
        print(f"RAM:  {mem:5.1f}% {'🔥' if mem > 85 else '✅'}")
        print(f"DISK: {disk:5.1f}% {'🔥' if disk > 90 else '✅'}")
        print(f"NET:  ↓{data['net_in']:6.1f} KB/s | ↑{data['net_out']:6.1f} KB/s")
        print(f"PROC: {data['processes']} processes")

        if self.write_timeout_count > 0:
            print(f"\n⚠️  Write timeouts: {self.write_timeout_count}")

        print("-" * 70)
        print(f"JSON sent: {json.dumps(data)[:65]}...")

        print("\nPress Ctrl+C to stop monitoring...")
        print("\nNote: Pico may be slow to respond during webhook POSTs")

        # === PICO MESSAGE LOG SECTION (last 10 messages) ===
        print("\n" + "=" * 70)
        print("  PICO MESSAGES (Last 10)")
        print("=" * 70)

        if self.pico_messages:
            for msg in self.pico_messages:
                print(msg)
        else:
            print("  (No messages yet)")

        print("=" * 70)
        print("\nPress Ctrl+C to stop monitoring...")

    def run(self):
        """Main monitoring loop"""
        print("=" * 70)
        print("  PC HEALTH MONITOR -> PICO CDC")
        print("=" * 70)
        print()

        if not self.connect_to_pico():
            return

        # Create/clear log file with header
        try:
            with open(self.log_file, 'w', encoding='utf-8') as f:
                f.write(f"PC HEALTH MONITOR LOG - Started {datetime.now().isoformat()}\n")
                f.write(f"Monitoring interval: {self.interval} seconds\n")
                f.write(f"Connected to: {self.pico_port}\n")
                f.write("=" * 100 + "\n\n")
        except Exception as e:
            print(f"Error creating log file: {e}")
            return

        self.running = True

        print("Starting health monitoring...")
        print(f"Update interval: {self.interval} seconds")
        print(f"Logging to: {os.path.abspath(self.log_file)}")
        print("Press Ctrl+C to stop\n")
        time.sleep(2)

        try:
            while self.running:
                # Collect health data
                health_data = self.get_comprehensive_health_data()

                # Log to file
                self.log_to_file(health_data)

                # Send to Pico (non-blocking, continues even on timeout)
                send_success = self.send_to_pico(health_data)

                # Read any response from Pico (with short timeout)
                if send_success:
                    time.sleep(0.1)
                    self.read_pico_response(timeout_ms=200)

                # Display status (this now includes persistent Pico messages)
                self.print_status(health_data)

                # Wait for next interval
                time.sleep(self.interval)

        except KeyboardInterrupt:
            print("\n\n✋ Monitoring stopped by user")
        except Exception as e:
            print(f"\n\n❌ Error during monitoring: {e}")
        finally:
            self.stop()

    def stop(self):
        """Stop monitoring and close connection"""
        self.running = False

        # Write final log entry
        try:
            with open(self.log_file, 'a', encoding='utf-8') as f:
                f.write(f"\nMonitoring ended at {datetime.now().isoformat()}\n")
                f.write(f"Total samples collected: {self.sample_count}\n")
                f.write(f"Total runtime: {(time.time() - self.start_time) / 60:.1f} minutes\n")
                f.write("=" * 100 + "\n")
        except:
            pass

        if self.serial_connection:
            try:
                self.serial_connection.close()
            except:
                pass

        print(f"📊 Collected {self.sample_count} samples")
        print(f"📄 Log saved to: {os.path.abspath(self.log_file)}")
        print(f"⏱️  Total runtime: {(time.time() - self.start_time) / 60:.1f} minutes")
        print("✓ Serial port closed")


def main():
    import argparse

    parser = argparse.ArgumentParser(description='PC Health Monitor with Pico CDC')
    parser.add_argument('--port', '-p', default=None,
                        help='COM port (e.g., COM9). Auto-detect if not specified')
    parser.add_argument('--interval', '-i', type=int, default=5,
                        help='Monitoring interval in seconds (default: 5)')
    parser.add_argument('--file', '-f', default='health_monitor.txt',
                        help='Log file name (default: health_monitor.txt)')

    args = parser.parse_args()

    monitor = HealthMonitorPico(pico_port=args.port, interval=args.interval, log_file=args.file)
    monitor.run()


if __name__ == "__main__":
    main()