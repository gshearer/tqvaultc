import struct
import sys
import zlib

def extract_from_arc(arc_path, target_file):
    with open(arc_path, 'rb') as f:
        magic = f.read(4)
        if magic != b'ARC\x00': return None
        f.read(4) # version
        num_files = struct.unpack('<I', f.read(4))[0]
        f.read(4) # toc offset
        toc_offset = struct.unpack('<I', f.read(4))[0]
        
        f.seek(toc_offset)
        for _ in range(num_files):
            offset = struct.unpack('<I', f.read(4))[0]
            comp_size = struct.unpack('<I', f.read(4))[0]
            real_size = struct.unpack('<I', f.read(4))[0]
            f.read(4) # crap
            path_len = struct.unpack('<I', f.read(4))[0]
            path = f.read(path_len).decode('utf-8', errors='ignore').replace('/', '\\').lower()
            
            if path == target_file.replace('/', '\\').lower() or path.endswith(target_file.replace('/', '\\').lower()):
                # Found it
                curr = f.tell()
                f.seek(offset)
                data = f.read(comp_size)
                try:
                    decomp = zlib.decompress(data)
                except:
                    decomp = data
                return decomp
    return None

if __name__ == '__main__':
    arc = sys.argv[1]
    file = sys.argv[2]
    data = extract_from_arc(arc, file)
    if data:
        print(f"File: {file} ({len(data)} bytes)")
        print("Hex: " + " ".join(f"{b:02X}" for b in data[:64]))
    else:
        print("Not found")
