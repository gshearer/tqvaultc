
import struct

def find_all_basenames(filename, start_offset):
    with open(filename, 'rb') as f:
        data = f.read()
    
    pos = start_offset
    while pos < len(data) - 8:
        # Look for 'baseName'
        pos = data.find(b'baseName', pos)
        if pos == -1: break
        
        # Tag is baseName. Length (8) should be before it.
        # But let's just extract the value.
        val_pos = pos + 8
        if val_pos + 4 > len(data): break
        val_len = struct.unpack('<I', data[val_pos:val_pos+4])[0]
        if val_len < 500: # Reasonable
            val = data[val_pos+4:val_pos+4+val_len].decode('ascii', errors='ignore')
            print(f"{pos}: baseName = {val}")
        
        pos += 8

print("--- Equipment Block baseNames (from 425019) ---")
find_all_basenames('example.chr', 425019)
