
import struct

def read_tq_string(f):
    try:
        length_bytes = f.read(4)
        if not length_bytes: return None
        length = struct.unpack('<I', length_bytes)[0]
        return f.read(length).decode('ascii', errors='ignore')
    except:
        return None

def find_tags(filename):
    with open(filename, 'rb') as f:
        data = f.read()
    
    pos = 0
    while True:
        pos = data.find(b'baseName', pos)
        if pos == -1: break
        
        # Tag is baseName. It should be preceded by its length (8)
        # But let's just look at what's after it.
        # Usually: [type:4 bytes] [len:4 bytes] [string:len bytes]
        # Type for baseName is usually 0.
        
        val_pos = pos + 8
        if val_pos + 4 > len(data): break
        len_val = struct.unpack('<I', data[val_pos:val_pos+4])[0]
        if len_val < 1000: # reasonable length for a filename
            string_val = data[val_pos+4:val_pos+4+len_val].decode('ascii', errors='ignore')
            print(f"Offset {pos}: baseName = {string_val}")
        
        pos += 8

print("--- All baseNames ---")
find_tags('example.chr')
