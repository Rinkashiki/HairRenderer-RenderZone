import bpy
import struct

# --- CONFIGURATION ---
output_path = "C:/Users/diegs/Downloads/alex_1.hair"
# ---------------------

def export_to_yuksel_hair(filepath):
    hair_obj = bpy.context.active_object
    
    if not hair_obj or hair_obj.type != 'CURVES':
        print("Please select the imported Alembic Curve object!")
        return

    curve_data = hair_obj.data
    strands = curve_data.curves
    
    num_strands = 0
    total_points = 0
    segments_array = []
    points_array = []
    
    # --- FILTER THRESHOLDS ---
    MIN_POINTS_REQUIRED = 1
    
    # Maximum allowed distance between two consecutive points in the same hair.
    # IMPORTANT: This is in Blender units (meters). 0.05 = 5 centimeters.
    # If your scene scale is different, you may need to adjust this!
    MAX_SEGMENT_LENGTH = 8.0
    
    for strand in strands:
        points = strand.points if len(strand.points) > 0 else strand.bezier_points
        num_verts = len(points)
        
        if num_verts < MIN_POINTS_REQUIRED:
            continue 
            
        valid_points = []
        
        # Grab the first point
        p_prev = hair_obj.matrix_world @ points[0].position
        valid_points.append(p_prev)
        
        # Loop through the rest of the points in the strand
        for i in range(1, num_verts):
            p_curr = hair_obj.matrix_world @ points[i].position
            dist = (p_curr - p_prev).length
            
            # THE TRIMMER: If the distance to the next point is massive, 
            # we assume the data is corrupted from here on out. Break the loop.
            if dist > MAX_SEGMENT_LENGTH:
                break
                
            valid_points.append(p_curr)
            p_prev = p_curr

        # After trimming, is the surviving hair still long enough to be useful?
        if len(valid_points) < MIN_POINTS_REQUIRED:
            continue

        # Add the SURVIVING points to the Yuksel format arrays
        segments_array.append(len(valid_points) - 1)
        total_points += len(valid_points)
        num_strands += 1
        
        for pt in valid_points:
            points_array.extend([pt.x, pt.y, pt.z])

    # --- BUILD THE 128-BYTE CEM YUKSEL HEADER ---
    magic = b"HAIR"
    bit_array = 3 
    default_segments = 0
    default_thickness = 0.1
    default_transparency = 0.0
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

export_to_yuksel_hair(output_path)