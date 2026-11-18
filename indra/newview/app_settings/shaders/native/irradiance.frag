// irradiance.frag: the diffuse irradiance of a reflection probe: 32 cosine weighted samples of the source cube (a layer of the probe array) at a mip level from the sample's solid angle. A line for line
// translation of "Irradiance Gen Shader" (interface/irradianceGenF.glsl, class 2); pairs with radiance.vert.
// Code below is derived from the Khronos GLTF Sample viewer:
// https://github.com/KhronosGroup/glTF-Sample-Viewer/blob/master/source/shaders/ibl_filtering.frag

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    int sourceIdx;
    float max_probe_lod;
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    mat4 modelview_matrix;
    uint ll_ti_cubearray_reflectionProbes;
};

layout(location = 0) in vec3 vary_dir;
layout(location = 0) out vec4 frag_color;

#define MATH_PI 3.1415926535897932384626433832795

float u_roughness = 1.0;
int u_sampleCount = 32;
float u_lodBias = 2.0;
int u_width = 64;

// Hammersley Points on the Hemisphere
// CC BY 3.0 (Holger Dammertz)
// http://holger.dammertz.org/stuff/notes_HammersleyOnHemisphere.html
// with adapted interface
float radicalInverse_VdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10; // / 0x100000000
}

// hammersley2d describes a sequence of points in the 2d unit square [0,1)^2
// that can be used for quasi Monte Carlo integration
vec2 hammersley2d(int i, int N)
{
    return vec2(float(i) / float(N), radicalInverse_VdC(uint(i)));
}

// TBN generates a tangent bitangent normal coordinate frame from the normal (the normal must be normalized)
mat3 generateTBN(vec3 normal)
{
    vec3 bitangent = vec3(0.0, 1.0, 0.0);

    float NdotUp = dot(normal, vec3(0.0, 1.0, 0.0));
    float epsilon = 0.0000001;
    /*if (1.0 - abs(NdotUp) <= epsilon)
    {
        // Sampling +Y or -Y, so we need a more robust bitangent.
        if (NdotUp > 0.0)
        {
            bitangent = vec3(0.0, 0.0, 1.0);
        }
        else
        {
            bitangent = vec3(0.0, 0.0, -1.0);
        }
    }*/

    vec3 tangent = normalize(cross(bitangent, normal));
    bitangent = cross(normal, tangent);

    return mat3(tangent, bitangent, normal);
}

struct MicrofacetDistributionSample
{
    float pdf;
    float cosTheta;
    float sinTheta;
    float phi;
};

MicrofacetDistributionSample Lambertian(vec2 xi, float roughness)
{
    MicrofacetDistributionSample lambertian;

    // Cosine weighted hemisphere sampling
    // http://www.pbr-book.org/3ed-2018/Monte_Carlo_Integration/2D_Sampling_with_Multidimensional_Transformations.html#Cosine-WeightedHemisphereSampling
    lambertian.cosTheta = sqrt(1.0 - xi.y);
    lambertian.sinTheta = sqrt(xi.y); // equivalent to `sqrt(1.0 - cosTheta*cosTheta)`;
    lambertian.phi = 2.0 * MATH_PI * xi.x;

    lambertian.pdf = lambertian.cosTheta / MATH_PI; // evaluation for solid angle, therefore drop the sinTheta

    return lambertian;
}

// getImportanceSample returns an importance sample direction with pdf in the .w component
vec4 getImportanceSample(int sampleIndex, vec3 N, float roughness)
{
    // generate a quasi monte carlo point in the unit square [0.1)^2
    vec2 xi = hammersley2d(sampleIndex, u_sampleCount);

    MicrofacetDistributionSample importanceSample;

    // generate the points on the hemisphere with a fitting mapping for
    // the distribution (e.g. lambertian uses a cosine importance)
    importanceSample = Lambertian(xi, roughness);

    // transform the hemisphere sample to the normal coordinate frame
    // i.e. rotate the hemisphere to the normal direction
    vec3 localSpaceDirection = normalize(vec3(
        importanceSample.sinTheta * cos(importanceSample.phi),
        importanceSample.sinTheta * sin(importanceSample.phi),
        importanceSample.cosTheta
    ));
    mat3 TBN = generateTBN(N);
    vec3 direction = TBN * localSpaceDirection;

    return vec4(direction, importanceSample.pdf);
}

// Mipmap Filtered Samples (GPU Gems 3, 20.4)
// https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-20-gpu-based-importance-sampling
// https://cgg.mff.cuni.cz/~jaroslav/papers/2007-sketch-fis/Final_sap_0073.pdf
float computeLod(float pdf)
{
    // We achieved good results by using the original formulation from Krivanek & Colbert adapted to cubemaps
    // https://cgg.mff.cuni.cz/~jaroslav/papers/2007-sketch-fis/Final_sap_0073.pdf
    float lod = 0.5 * log2(6.0 * float(u_width) * float(u_width) / (float(u_sampleCount) * pdf));
    return lod;
}

vec4 filterColor(vec3 N)
{
    vec4 color = vec4(0.f);

    for (int i = 0; i < u_sampleCount; ++i)
    {
        vec4 importanceSample = getImportanceSample(i, N, 1.0);

        vec3 H = vec3(importanceSample.xyz);
        float pdf = importanceSample.w;

        // mipmap filtered samples (GPU Gems 3, 20.4)
        float lod = computeLod(pdf);

        // apply the bias to the lod
        lod += u_lodBias;

        lod = clamp(lod, 0, max_probe_lod);

        // sample lambertian at a lower resolution to avoid fireflies
        vec4 lambertian = textureLod(ll_tblcubearray[nonuniformEXT(LL_HM(ll_ti_cubearray_reflectionProbes))], vec4(H, sourceIdx), lod);

        color += lambertian;
    }

    color /= float(u_sampleCount);

    return color;
}

// entry point
void main()
{
    vec4 color = vec4(0);
    color = filterColor(vary_dir);
    frag_color = max(color, vec4(0));
}
