import struct
import os

def yaz0_decompress(input_data, output_data, output_offset, output_length):
    input_pos = 0
    output_pos = output_offset
    output_end = output_offset + output_length
    
    while input_pos < len(input_data):
        if output_pos >= output_end:
            break
        layoutBits = input_data[input_pos]
        input_pos += 1
        
        for _ in range(8):
            if input_pos >= len(input_data) or output_pos >= output_end:
                break
                
            if layoutBits & 0x80:
                output_data[output_pos] = input_data[input_pos]
                output_pos += 1
                input_pos += 1
            else:
                firstByte = input_data[input_pos]
                secondByte = input_data[input_pos+1]
                input_pos += 2
                
                val = (firstByte << 8) | secondByte
                offset = (val & 0x0FFF) + 1
                
                if (firstByte & 0xF0) == 0:
                    thirdByte = input_data[input_pos]
                    input_pos += 1
                    length = thirdByte + 0x12
                else:
                    length = ((val & 0xF000) >> 12) + 2
                    
                # Copy byte-by-byte to handle overlapping memory correctly
                for i in range(length):
                    output_data[output_pos + i] = output_data[output_pos - offset + i]
                output_pos += length
                
            layoutBits = (layoutBits << 1) & 0xFF

def main():
    rom_path = "mm.us.rev1.rom_uncompressed.z64"
    if not os.path.exists(rom_path):
        print(f"Error: {rom_path} not found.")
        return

    with open(rom_path, 'rb') as f:
        compressed_rom = f.read()

    if len(compressed_rom) != 0x2000000:
        print(f"Error: ROM size must be exactly 0x2000000 bytes, got {len(compressed_rom)}")
        return

    if compressed_rom[0x3B:0x3F] != b'NZSE':
        print(f"Error: Invalid ROM header. Expected 'NZSE' at 0x3B, got {compressed_rom[0x3B:0x3F]}")
        return

    print("Decompressing ROM...")
    
    # Output array size 0x2F00000 bytes
    output_rom = bytearray(0x2F00000)
    
    dma_data_rom_addr = 0x1A500
    cur_entry_index = 0
    content_end = 0

    while True:
        entry_offset = dma_data_rom_addr + cur_entry_index * 16
        cur_entry_index += 1
        
        entry_bytes = compressed_rom[entry_offset : entry_offset + 16]
        vrom_start, vrom_end, rom_start, rom_end = struct.unpack(">IIII", entry_bytes)
        
        if vrom_start == 0 and vrom_end == 0 and rom_start == 0 and rom_end == 0:
            break
            
        entry_decompressed_size = vrom_end - vrom_start
        
        if rom_end == 0:
            # Copy as-is
            output_rom[vrom_start : vrom_start + entry_decompressed_size] = compressed_rom[rom_start : rom_start + entry_decompressed_size]
            new_rom_start = vrom_start
            new_rom_end = 0
        else:
            if rom_end != rom_start:
                if compressed_rom[rom_start : rom_start + 4] != b'Yaz0':
                    print(f"Error: Yaz0 header missing at 0x{rom_start:X}")
                    return
                
                compressed_data_rom_start = rom_start + 0x10
                entry_compressed_size = rom_end - compressed_data_rom_start
                
                input_span = compressed_rom[compressed_data_rom_start : compressed_data_rom_start + entry_compressed_size]
                yaz0_decompress(input_span, output_rom, vrom_start, entry_decompressed_size)
                
                new_rom_start = vrom_start
                new_rom_end = 0
            else:
                new_rom_start = rom_start
                new_rom_end = rom_end

        if entry_decompressed_size != 0:
            if vrom_end > content_end:
                content_end = vrom_end
                
        # Pack and write modified DMA entry to output
        new_entry_bytes = struct.pack(">IIII", vrom_start, vrom_end, new_rom_start, new_rom_end)
        output_rom[entry_offset : entry_offset + 16] = new_entry_bytes
        
        if vrom_end == 0:
            break

    # Align the start of padding to the closest 0x1000
    content_end = (content_end + 0x1000 - 1) & ~0xFFF
    
    # Pad with 0xFF
    for i in range(content_end, len(output_rom)):
        output_rom[i] = 0xFF

    # Save to file
    out_rom_path = "mm.us.rev1.rom_uncompressed.z64"
    # Back up the compressed ROM first
    backup_path = "mm.us.rev1.rom_compressed.z64"
    if os.path.exists(backup_path):
        os.remove(backup_path)
    os.rename(rom_path, backup_path)
    print(f"Backed up compressed ROM to {backup_path}")
    
    with open(out_rom_path, 'wb') as f:
        f.write(output_rom)
        
    print(f"Decompressed ROM saved to {out_rom_path} (size: {len(output_rom)} bytes)")

if __name__ == '__main__':
    main()
