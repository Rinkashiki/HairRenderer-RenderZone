import bpy
import struct

# --- CONFIGURATION ---
# Change this to your desired output path
output_path = "C:/Users/diegs/Downloads/nadia_1.hair"
# ---------------------

def export_to_yuksel_hair(filepath):
    # Find the imported curves object in the scene
    # Assumes you have selected your imported Alembic hair object
    hair_obj = bpy.context.active_object
    
    if not hair_obj or hair_obj.type != 'CURVES':
        print("Please select the imported Alembic Curve object!")
        return

    curve_data = hair_obj.data
    strands = curve_data.curves
    
    num_strands = len(strands)
    total_points = 0
    
    segments_array = []
    points_array = []
    
    # Process each strand (spline)
    for strand in strands:
        # Number of vertices in this strand
        num_verts = len(strand.points) if len(strand.points) > 0 else len(strand.bezier_points)
        
        if num_verts < 2:
            continue # Ignore broken single-point strands
            
        # Cem Yuksel format stores SEGMENTS (vertices minus 1)
        segments_array.append(num_verts - 1)
        total_points += num_verts
        
        # Get point coordinates
        points = strand.points if len(strand.points) > 0 else strand.bezier_points
        for pt in points:
            # Transform local coordinates to world coordinates
            world_pt = hair_obj.matrix_world @ pt.position
            points_array.extend([world_pt.x, world_pt.y, world_pt.z])

    # --- BUILD THE 128-BYTE CEM YUKSEL HEADER ---
    magic = b"HAIR"
    bit_array = 3 
    
    default_segments = 0
    default_thickness = 0.1
    default_transparency = 0.0
    
    # We break these out explicitly
    default_r = 1.0
    default_g = 1.0
    default_b = 1.0
    
    file_info = b"Converted from Alembic via Python".ljust(88, b"\x00")[:88]

    try:
        header = struct.pack(
            "<4sIIIIfffff88s",
            magic,
            num_strands,
            total_points,
            bit_array,
            default_segments,
            default_thickness,
            default_transparency,
            default_r,
            default_g,
            default_b,
            file_info
        )
    except struct.error as e:
        print(f"PACKING CRASHED AT HEADER: {e}")
        return

    # --- WRITE BINARY FILE ---
    try:
        with open(filepath, "wb") as f:
            f.write(header)
            f.write(struct.pack(f"<{len(segments_array)}H", *segments_array))
            f.write(struct.pack(f"<{len(points_array)}f", *points_array))
        print(f"SUCCESS: Exported {num_strands} strands to {filepath}")
    except Exception as e:
        print(f"FILE WRITE ERROR: {e}")

# Execute the converter
export_to_yuksel_hair(output_path)