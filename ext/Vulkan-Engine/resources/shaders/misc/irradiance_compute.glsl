#shader vertex
#version 460 core

layout(location = 0) in vec3 pos;

void main()
{
    gl_Position = vec4(pos, 1.0);
}

#shader geometry
#version 460

layout(triangles) in;
layout(triangle_strip, max_vertices = 18) out; 



layout(location = 0) out vec3 _pos;


void main() {


    for(int i = 0; i < 6; i++) {

        gl_Layer = i;
		
        for (int j = 0; j < 3; j++) {
            _pos =gl_in[j].gl_Position.xyz; 
            gl_Position = gl_in[j].gl_Position; 
            EmitVertex();
        }
        EndPrimitive(); 

    }
}

#shader fragment
#version 460 core

#define PI 3.1415926535897932384626433832795

layout(location = 0) in vec3 _pos;

layout(set = 0, binding = 1) uniform CaptureData{
    mat4 proj;
	mat4 views[6];
} capture;

layout(location = 0) out vec4 li;

layout(set = 0, binding = 0) uniform samplerCube u_envMap;

// Per-face direction reconstruction. Must match panorama_converter.glsl::uvToXYZ
// EXACTLY so this irradiance cube shares the env cubemap's (and the hardware's)
// face convention. The cube is drawn untransformed (gl_Position = vec4(pos,1)),
// so for every covered pixel _pos.xy equals the screen NDC, which is identical to
// the panorama baker's texCoordNew — feeding it here yields the same mapping.
// (The previous `normalize(proj * views[gl_Layer] * pos)` was wrong: it inverted
// the ±X faces and warped up to 90° toward face edges. capture.proj/views are now
// unused for the direction but left bound to avoid a descriptor-layout change.)
vec3 uvToXYZ(int face, vec2 uv)
{
	if(face == 0)      return vec3(  1.0,  uv.y, -uv.x);
	else if(face == 1) return vec3( -1.0,  uv.y,  uv.x);
	else if(face == 2) return vec3( uv.x,  -1.0,  uv.y);
	else if(face == 3) return vec3( uv.x,   1.0, -uv.y);
	else if(face == 4) return vec3( uv.x,  uv.y,   1.0);
	else               return vec3(-uv.x,  uv.y,  -1.0);
}

void main()
{

    vec3 n = normalize(uvToXYZ(gl_Layer, _pos.xy));

    vec3 irradiance = vec3(0.0);   
    
    vec3 up    = vec3(0.0, 1.0, 0.0);
    vec3 right = normalize(cross(up, n));
    up         = normalize(cross(n, right));
       
    float sampleDelta = 0.025;
    float nrSamples = 0.0;
    for(float phi = 0.0; phi < 2.0 * PI; phi += sampleDelta)
    {
        for(float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta)
        {
            vec3 tangentSample = vec3(sin(theta) * cos(phi),  sin(theta) * sin(phi), cos(theta));
            vec3 sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * n; 

            // Firefly clamp: HDRis with direct sun pixels (e.g. nature_demo.hdr) carry
            // values in the hundreds. Without a cap the cosine-weighted integral makes
            // silhouettes glow like wet specular. 50 keeps a bright sky punch without
            // blow-out.
            vec3 envSample = min(texture(u_envMap, sampleVec).rgb, vec3(50.0));
            irradiance += envSample * cos(theta) * sin(theta);
            nrSamples++;
        }
    }
    irradiance = PI * irradiance * (1.0 / float(nrSamples));
    
    li = vec4(irradiance, 1.0);
}