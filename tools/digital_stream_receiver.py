#!/usr/bin/env python3
"""
Digital Stream Receiver for SDR++ Digital Demodulators
Receives bit/dibit streams from SDR++ via TCP/UDP and provides basic decoding

Usage:
    python digital_stream_receiver.py --host localhost --port 7355 --protocol p25
    python digital_stream_receiver.py --udp --port 7356 --protocol dmr --output audio
"""

import socket
import struct
import argparse
import time
import sys
from typing import Optional, Tuple
from enum import IntEnum
import threading
import queue

class ProtocolType(IntEnum):
    P25_FSK4 = 1
    P25_CQPSK_4800 = 2
    P25_CQPSK_6000 = 3
    P25_H_DQPSK = 4
    P25_H_CPM = 5
    DMR_FSK4 = 6
    M17_FSK4 = 7
    YSF_FSK4 = 8
    NXDN_4800 = 9
    NXDN_9600 = 10
    DSTAR_FSK2 = 11
    EDACS_FSK2 = 12
    PROVOICE_FSK2 = 13

PROTOCOL_NAMES = {
    ProtocolType.P25_FSK4: "P25 FSK4",
    ProtocolType.P25_CQPSK_4800: "P25 CQPSK 4800",
    ProtocolType.P25_CQPSK_6000: "P25 CQPSK 6000",
    ProtocolType.P25_H_DQPSK: "P25 H-DQPSK",
    ProtocolType.P25_H_CPM: "P25 H-CPM",
    ProtocolType.DMR_FSK4: "DMR FSK4",
    ProtocolType.M17_FSK4: "M17 FSK4",
    ProtocolType.YSF_FSK4: "YSF Fusion FSK4",
    ProtocolType.NXDN_4800: "NXDN 4800",
    ProtocolType.NXDN_9600: "NXDN 9600",
    ProtocolType.DSTAR_FSK2: "D-STAR FSK2",
    ProtocolType.EDACS_FSK2: "EDACS FSK2",
    ProtocolType.PROVOICE_FSK2: "ProVoice FSK2"
}

class DigitalStreamHeader:
    """Digital stream header structure"""
    MAGIC = 0x44494749  # "DIGI"
    FORMAT = '<IHHB3xQ'  # little-endian: magic(4), protocol_id(2), symbol_rate(2), bits_per_symbol(1), reserved(3), timestamp(8)
    SIZE = struct.calcsize(FORMAT)
    
    def __init__(self, data: bytes = None):
        if data and len(data) >= self.SIZE:
            unpacked = struct.unpack(self.FORMAT, data[:self.SIZE])
            self.magic = unpacked[0]
            self.protocol_id = unpacked[1]
            self.symbol_rate = unpacked[2]
            self.bits_per_symbol = unpacked[3]
            self.timestamp = unpacked[4]
        else:
            self.magic = 0
            self.protocol_id = 0
            self.symbol_rate = 0
            self.bits_per_symbol = 0
            self.timestamp = 0
    
    def is_valid(self) -> bool:
        return self.magic == self.MAGIC
    
    def get_protocol_name(self) -> str:
        try:
            return PROTOCOL_NAMES[ProtocolType(self.protocol_id)]
        except (ValueError, KeyError):
            return f"Unknown Protocol {self.protocol_id}"
    
    def __str__(self) -> str:
        return (f"Protocol: {self.get_protocol_name()}, "
                f"Symbol Rate: {self.symbol_rate} sym/s, "
                f"Bits/Symbol: {self.bits_per_symbol}, "
                f"Timestamp: {self.timestamp}")

class DigitalStreamReceiver:
    """Receives and processes digital streams from SDR++"""
    
    def __init__(self, host: str, port: int, use_udp: bool = False):
        self.host = host
        self.port = port
        self.use_udp = use_udp
        self.socket = None
        self.running = False
        self.header: Optional[DigitalStreamHeader] = None
        self.stats = {
            'bytes_received': 0,
            'symbols_received': 0,
            'packets_received': 0,
            'header_received': False,
            'start_time': None
        }
        
        # Data processing
        self.data_queue = queue.Queue(maxsize=1000)
        self.processor_thread = None
        
    def connect(self) -> bool:
        """Establish connection to SDR++"""
        try:
            if self.use_udp:
                self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                self.socket.bind((self.host, self.port))
                print(f"UDP socket listening on {self.host}:{self.port}")
            else:
                # Connect TO SDR++ digital network sink server
                self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                print(f"Connecting to SDR++ digital sink server at {self.host}:{self.port}")
                self.socket.connect((self.host, self.port))
                print(f"✅ Connected to SDR++ digital stream")
                
            return True
            
        except Exception as e:
            print(f"Connection failed: {e}")
            return False
    
    def start(self) -> bool:
        """Start receiving data"""
        if not self.connect():
            return False
            
        self.running = True
        self.stats['start_time'] = time.time()
        
        # Start data processor thread
        self.processor_thread = threading.Thread(target=self._process_data)
        self.processor_thread.daemon = True
        self.processor_thread.start()
        
        return True
    
    def stop(self):
        """Stop receiving data"""
        self.running = False
        if self.socket:
            self.socket.close()
        if self.processor_thread:
            self.processor_thread.join(timeout=1.0)
    
    def receive_loop(self):
        """Main receive loop"""
        try:
            while self.running:
                if self.use_udp:
                    data, addr = self.socket.recvfrom(4096)
                else:
                    data = self.socket.recv(4096)
                
                if not data:
                    print("Connection closed by SDR++")
                    break
                
                self._handle_data(data)
                
        except Exception as e:
            if self.running:
                print(f"Receive error: {e}")
        finally:
            self.stop()
    
    def _handle_data(self, data: bytes):
        """Handle received data"""
        self.stats['bytes_received'] += len(data)
        
        # Debug: Show first few bytes of each packet
        if len(data) > 0:
            print(f"📦 Received {len(data)} bytes: {data[:min(16, len(data))].hex()}")
        
        # Check for header if not yet received
        if not self.stats['header_received'] and len(data) >= DigitalStreamHeader.SIZE:
            potential_header = DigitalStreamHeader(data)
            print(f"🔍 Checking header: magic={potential_header.magic:08X}, expected={DigitalStreamHeader.MAGIC:08X}")
            if potential_header.is_valid():
                self.header = potential_header
                self.stats['header_received'] = True
                print(f"\n📡 Digital Stream Header Received:")
                print(f"   {self.header}")
                print()
                
                # Skip header bytes for data processing
                data = data[DigitalStreamHeader.SIZE:]
            else:
                print(f"❌ Invalid header magic: {potential_header.magic:08X}")
                return
        
        # Queue data for processing
        if data and not self.data_queue.full():
            self.data_queue.put(data)
    
    def _process_data(self):
        """Process received digital data"""
        while self.running:
            try:
                data = self.data_queue.get(timeout=1.0)
                self._decode_symbols(data)
                self.stats['packets_received'] += 1
            except queue.Empty:
                continue
    
    def _decode_symbols(self, data: bytes):
        """Decode symbols based on protocol type"""
        if not self.header:
            return
            
        symbols_count = len(data)
        self.stats['symbols_received'] += symbols_count
        
        # Basic symbol analysis
        if self.header.bits_per_symbol == 1:
            # FSK2 - bit stream (0, 1)
            bits = [f"{b:01b}" for b in data]
            if len(bits) > 0:
                print(f"Bits: {' '.join(bits[:16])}{'...' if len(bits) > 16 else ''}")
        else:
            # FSK4 - dibit stream (0, 1, 2, 3)
            dibits = [f"{b:02b}" for b in data if b < 4]
            if len(dibits) > 0:
                print(f"Dibits: {' '.join(dibits[:16])}{'...' if len(dibits) > 16 else ''}")
    
    def print_stats(self):
        """Print reception statistics"""
        if self.stats['start_time']:
            runtime = time.time() - self.stats['start_time']
            bytes_per_sec = self.stats['bytes_received'] / runtime if runtime > 0 else 0
            symbols_per_sec = self.stats['symbols_received'] / runtime if runtime > 0 else 0
            
            print(f"\n📊 Reception Statistics:")
            print(f"   Runtime: {runtime:.1f} seconds")
            print(f"   Bytes: {self.stats['bytes_received']} ({bytes_per_sec:.1f} B/s)")
            print(f"   Symbols: {self.stats['symbols_received']} ({symbols_per_sec:.1f} sym/s)")
            print(f"   Packets: {self.stats['packets_received']}")
            if self.header:
                print(f"   Expected Symbol Rate: {self.header.symbol_rate} sym/s")
                efficiency = (symbols_per_sec / self.header.symbol_rate * 100) if self.header.symbol_rate > 0 else 0
                print(f"   Efficiency: {efficiency:.1f}%")

def main():
    parser = argparse.ArgumentParser(description="Digital Stream Receiver for SDR++")
    parser.add_argument("--host", default="localhost", help="Host to listen on")
    parser.add_argument("--port", type=int, default=7355, help="Port to listen on")
    parser.add_argument("--udp", action="store_true", help="Use UDP instead of TCP")
    parser.add_argument("--protocol", help="Expected protocol (for validation)")
    parser.add_argument("--output", choices=["console", "file", "audio"], default="console", 
                       help="Output mode")
    parser.add_argument("--verbose", "-v", action="store_true", help="Verbose output")
    
    args = parser.parse_args()
    
    print("🚀 SDR++ Digital Stream Receiver")
    print(f"   Listening: {args.host}:{args.port} ({'UDP' if args.udp else 'TCP'})")
    print(f"   Output: {args.output}")
    if args.protocol:
        print(f"   Expected Protocol: {args.protocol}")
    print()
    
    # Create receiver
    receiver = DigitalStreamReceiver(args.host, args.port, args.udp)
    
    try:
        # Start receiving
        if not receiver.start():
            print("❌ Failed to start receiver")
            return 1
        
        print("✅ Receiver started. Press Ctrl+C to stop.")
        print("📡 Waiting for digital stream from SDR++...")
        print()
        
        # Main receive loop
        last_stats_time = time.time()
        receiver.receive_loop()
        
    except KeyboardInterrupt:
        print("\n🛑 Stopping receiver...")
    except Exception as e:
        print(f"❌ Error: {e}")
    finally:
        receiver.stop()
        receiver.print_stats()
        print("👋 Receiver stopped.")
    
    return 0

if __name__ == "__main__":
    sys.exit(main())
