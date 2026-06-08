#include "scene_loader.h"
#include "hair_loader.h"

#include <engine/core/animation_json.h>
#include <engine/core/scene/joint_attachment.h>
#include <engine/systems/renderers/forward.h>
#include <engine/tools/loaders.h>

#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

using json = nlohmann::json;

namespace scene_loader {

// ─── helpers ────────────────────────────────────────────────────────────────

static Vec3 to_vec3(const json& j, const Vec3& fallback = Vec3(0.0f)) {
    if (!j.is_array() || j.size() < 3)
        return fallback;
    return Vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
}

static Vec4 to_vec4(const json& j, const Vec4& fallback = Vec4(0.0f)) {
    if (!j.is_array() || j.size() < 4)
        return fallback;
    return Vec4(j[0].get<float>(), j[1].get<float>(),
                j[2].get<float>(), j[3].get<float>());
}

// Accepts a number (uniform scale) or a 3-element array.
static Vec3 to_scale(const json& j, const Vec3& fallback = Vec3(1.0f)) {
    if (j.is_number())
        return Vec3(j.get<float>());
    return to_vec3(j, fallback);
}

static void require(const json& j, const char* key, const std::string& context) {
    if (!j.contains(key))
        throw std::runtime_error("scene_loader: missing required field '" + std::string(key) +
                                 "' in " + context);
}

// Warn about unknown keys at this JSON level.
static void warn_unknown(const json& j,
                         const std::unordered_set<std::string>& known,
                         const std::string& context) {
    if (!j.is_object())
        return;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (known.find(it.key()) == known.end())
            LOG_WARN("scene_loader: unknown key '" + it.key() + "' in " + context + " (ignored)");
    }
}

// ─── lights ─────────────────────────────────────────────────────────────────

static Core::Light* build_light(const json&        jl,
                                const std::string& engineResourcesPath) {
    require(jl, "type", "light");

    const std::string type = jl.at("type").get<std::string>();

    Core::Light* light = nullptr;

    if (type == "point") {
        auto* p = new Core::PointLight();
        if (jl.contains("position"))      p->set_position(to_vec3(jl["position"]));
        if (jl.contains("color"))         p->set_color(to_vec3(jl["color"], Vec3(1.0f)));
        if (jl.contains("intensity"))     p->set_intensity(jl["intensity"].get<float>());
        if (jl.contains("shadow_fov"))    p->set_shadow_fov(jl["shadow_fov"].get<float>());
        if (jl.contains("shadow_bias"))   p->set_shadow_bias(jl["shadow_bias"].get<float>());
        if (jl.contains("shadow_near"))   p->set_shadow_near(jl["shadow_near"].get<float>());
        if (jl.contains("shadow_far"))    p->set_shadow_far(jl["shadow_far"].get<float>());
        if (jl.contains("area_of_effect")) p->set_area_of_effect(jl["area_of_effect"].get<float>());
        if (jl.contains("cast_shadows"))  p->set_cast_shadows(jl["cast_shadows"].get<bool>());
        light = p;
    } else if (type == "directional") {
        Vec3 dir(0.0f, -1.0f, 0.0f);
        if (jl.contains("direction"))
            dir = to_vec3(jl["direction"], dir);
        auto* d = new Core::DirectionalLight(dir);
        if (jl.contains("position"))      d->set_position(to_vec3(jl["position"]));
        if (jl.contains("color"))         d->set_color(to_vec3(jl["color"], Vec3(1.0f)));
        if (jl.contains("intensity"))     d->set_intensity(jl["intensity"].get<float>());
        if (jl.contains("shadow_fov"))    d->set_shadow_fov(jl["shadow_fov"].get<float>());
        if (jl.contains("shadow_bias"))   d->set_shadow_bias(jl["shadow_bias"].get<float>());
        if (jl.contains("shadow_near"))   d->set_shadow_near(jl["shadow_near"].get<float>());
        if (jl.contains("shadow_far"))    d->set_shadow_far(jl["shadow_far"].get<float>());
        if (jl.contains("cast_shadows"))  d->set_cast_shadows(jl["cast_shadows"].get<bool>());
        light = d;
    } else {
        throw std::runtime_error("scene_loader: unknown light type '" + type + "'");
    }

    if (jl.contains("name"))
        light->set_name(jl["name"].get<std::string>());

    // Optional marker mesh — engine built-in resolved against engineResourcesPath.
    // "dummy_visible" (default true) sets the dummy's initial active state so it
    // can still be toggled back on from the GUI — same semantics as the GUI checkbox.
    if (jl.contains("dummy_mesh")) {
        const std::string dummyName = jl["dummy_mesh"].get<std::string>();
        auto* dummy = new Core::Mesh();
        Tools::Loaders::load_3D_file(dummy, engineResourcesPath + "meshes/" + dummyName, false);
        dummy->push_material(new Core::UnlitMaterial());
        dummy->cast_shadows(false);
        dummy->set_name((light->get_name().empty() ? std::string("Light") : light->get_name()) + "Dummy");
        dummy->set_active(jl.value("dummy_visible", true));
        light->add_child(dummy);
    }

    warn_unknown(jl,
        {"type", "name", "position", "color", "intensity",
         "shadow_fov", "shadow_bias", "shadow_near", "shadow_far",
         "area_of_effect", "cast_shadows", "direction", "dummy_mesh", "dummy_visible"},
        "light");

    return light;
}

// ─── materials ──────────────────────────────────────────────────────────────

// Resolve a texture reference. "$GLB[N]" returns the Nth element of glbTextures (or nullptr).
// Any other non-empty string is treated as a path relative to resourcesPath.
static Core::Texture* resolve_texture(const json&                          jvalue,
                                      const std::string&                   resourcesPath,
                                      const std::vector<Core::Texture*>&   glbTextures,
                                      TextureFormatType                    fmt) {
    if (jvalue.is_null())
        return nullptr;

    const std::string ref = jvalue.get<std::string>();
    if (ref.empty())
        return nullptr;

    // "$GLB[N]" sentinel
    if (ref.rfind("$GLB[", 0) == 0) {
        const auto close = ref.find(']');
        if (close == std::string::npos)
            return nullptr;
        const int idx = std::stoi(ref.substr(5, close - 5));
        if (idx >= 0 && static_cast<size_t>(idx) < glbTextures.size())
            return glbTextures[idx];
        LOG_WARN("scene_loader: $GLB[" + std::to_string(idx) + "] out of range");
        return nullptr;
    }

    auto* tex = new Core::Texture();
    Tools::Loaders::load_texture(tex, resourcesPath + ref, fmt);
    return tex;
}

static Core::IMaterial* build_pbr(const json&                        jm,
                                  const std::string&                 resourcesPath,
                                  const std::vector<Core::Texture*>& glbTextures) {
    auto* mat = new Core::PhysicallyBasedMaterial();
    if (jm.contains("albedo"))           mat->set_albedo(to_vec3(jm["albedo"], Vec3(1.0f)));
    if (jm.contains("albedo_weight"))    mat->set_albedo_weight(jm["albedo_weight"].get<float>());
    if (jm.contains("opacity"))          mat->set_opacity(jm["opacity"].get<float>());
    if (jm.contains("opacity_weight"))   mat->set_opacity_weight(jm["opacity_weight"].get<float>());
    if (jm.contains("metalness"))        mat->set_metalness(jm["metalness"].get<float>());
    if (jm.contains("metalness_weight")) mat->set_metalness_weight(jm["metalness_weight"].get<float>());
    if (jm.contains("roughness"))        mat->set_roughness(jm["roughness"].get<float>());
    if (jm.contains("roughness_weight")) mat->set_roughness_weight(jm["roughness_weight"].get<float>());
    if (jm.contains("occlusion"))        mat->set_occlusion(jm["occlusion"].get<float>());
    if (jm.contains("occlusion_weight")) mat->set_occlusion_weight(jm["occlusion_weight"].get<float>());
    if (jm.contains("emissive_color"))   mat->set_emissive_color(to_vec3(jm["emissive_color"]));
    if (jm.contains("emissive_weight"))  mat->set_emissive_weight(jm["emissive_weight"].get<float>());
    if (jm.contains("emission_intensity")) mat->set_emission_intensity(jm["emission_intensity"].get<float>());
    if (jm.contains("reflective"))       mat->reflective(jm["reflective"].get<bool>());

    if (jm.contains("albedo_texture"))
        mat->set_albedo_texture(resolve_texture(jm["albedo_texture"], resourcesPath, glbTextures,
                                                TEXTURE_FORMAT_TYPE_COLOR));
    if (jm.contains("normal_texture"))
        mat->set_normal_texture(resolve_texture(jm["normal_texture"], resourcesPath, glbTextures,
                                                TEXTURE_FORMAT_TYPE_NORMAL));
    if (jm.contains("roughness_texture"))
        mat->set_roughness_texture(resolve_texture(jm["roughness_texture"], resourcesPath, glbTextures,
                                                   TEXTURE_FORMAT_TYPE_LINEAR));
    if (jm.contains("metallic_texture"))
        mat->set_metallic_texture(resolve_texture(jm["metallic_texture"], resourcesPath, glbTextures,
                                                  TEXTURE_FORMAT_TYPE_LINEAR));
    if (jm.contains("occlusion_texture"))
        mat->set_occlusion_texture(resolve_texture(jm["occlusion_texture"], resourcesPath, glbTextures,
                                                   TEXTURE_FORMAT_TYPE_LINEAR));
    if (jm.contains("emissive_texture"))
        mat->set_emissive_texture(resolve_texture(jm["emissive_texture"], resourcesPath, glbTextures,
                                                  TEXTURE_FORMAT_TYPE_COLOR));
    if (jm.contains("bent_normal_texture"))
        mat->set_bent_normal_texture(resolve_texture(jm["bent_normal_texture"], resourcesPath, glbTextures,
                                                     TEXTURE_FORMAT_TYPE_NORMAL));
    if (jm.contains("curvature_texture"))
        mat->set_curvature_texture(resolve_texture(jm["curvature_texture"], resourcesPath, glbTextures,
                                                   TEXTURE_FORMAT_TYPE_LINEAR));
    if (jm.contains("scattering_texture"))
        mat->set_scattering_texture(resolve_texture(jm["scattering_texture"], resourcesPath, glbTextures,
                                                    TEXTURE_FORMAT_TYPE_LINEAR));
    if (jm.contains("clothes_mask_texture"))
        mat->set_clothes_mask_texture(resolve_texture(jm["clothes_mask_texture"], resourcesPath, glbTextures,
                                                      TEXTURE_FORMAT_TYPE_LINEAR));
    if (jm.contains("detail_normal_texture"))
        mat->set_detail_normal_texture(resolve_texture(jm["detail_normal_texture"], resourcesPath, glbTextures,
                                                       TEXTURE_FORMAT_TYPE_NORMAL));
    if (jm.contains("detail_cavity_texture"))
        mat->set_detail_cavity_texture(resolve_texture(jm["detail_cavity_texture"], resourcesPath, glbTextures,
                                                       TEXTURE_FORMAT_TYPE_LINEAR));
    if (jm.contains("detail_tiling"))
        mat->set_detail_tiling(jm["detail_tiling"].get<float>());
    if (jm.contains("detail_normal_strength"))
        mat->set_detail_normal_strength(jm["detail_normal_strength"].get<float>());
    if (jm.contains("cavity_spec_occlusion"))
        mat->set_cavity_spec_occlusion(jm["cavity_spec_occlusion"].get<float>());
    if (jm.contains("dual_lobe_mix"))
        mat->set_dual_lobe_mix(jm["dual_lobe_mix"].get<float>());
    if (jm.contains("dual_lobe_roughness_soft"))
        mat->set_dual_lobe_roughness_soft(jm["dual_lobe_roughness_soft"].get<float>());
    if (jm.contains("sheen_color")) {
        auto c = jm["sheen_color"];
        mat->set_sheen_color({c[0].get<float>(), c[1].get<float>(), c[2].get<float>()});
    }
    if (jm.contains("sheen_intensity"))
        mat->set_sheen_intensity(jm["sheen_intensity"].get<float>());
    // Note: per-channel detail-normal blur biases (detail_blur_r/g/b) are
    // intentionally NOT JSON-driven — they're derived from the renderer's
    // sss_scatter_lut so the d'Eon hybrid normals stay consistent with the
    // SSS pass's spectral profile. See `derive_detail_blur_from_lut` below.

    warn_unknown(jm,
        {"type", "albedo", "albedo_weight", "albedo_texture",
         "opacity", "opacity_weight",
         "metalness", "metalness_weight", "metallic_texture",
         "roughness", "roughness_weight", "roughness_texture",
         "occlusion", "occlusion_weight", "occlusion_texture",
         "emissive_color", "emissive_weight", "emission_intensity", "emissive_texture",
         "normal_texture", "reflective",
         "bent_normal_texture", "curvature_texture", "scattering_texture", "clothes_mask_texture",
         "detail_normal_texture", "detail_cavity_texture",
         "detail_tiling", "detail_normal_strength",
         "cavity_spec_occlusion",
         "dual_lobe_mix", "dual_lobe_roughness_soft",
         "sheen_color", "sheen_intensity"},
        "material(pbr)");

    return mat;
}

static Core::IMaterial* build_haircard(const json&                        jm,
                                       const std::string&                 resourcesPath,
                                       const std::vector<Core::Texture*>& glbTextures) {
    auto* mat = new Core::HairCardMaterial();
    if (jm.contains("hair_color"))         mat->set_hair_color(to_vec3(jm["hair_color"]));
    if (jm.contains("alpha_threshold"))    mat->set_alpha_threshold(jm["alpha_threshold"].get<float>());
    if (jm.contains("roughness"))          mat->set_roughness(jm["roughness"].get<float>());
    if (jm.contains("specular_intensity")) mat->set_specular_intensity(jm["specular_intensity"].get<float>());
    if (jm.contains("specular_shift"))     mat->set_specular_shift(jm["specular_shift"].get<float>());
    if (jm.contains("color_variation"))    mat->set_color_variation(jm["color_variation"].get<float>());
    if (jm.contains("root_color"))         mat->set_root_color(to_vec3(jm["root_color"]));
    if (jm.contains("tip_color"))          mat->set_tip_color(to_vec3(jm["tip_color"]));

    if (jm.contains("hair_data_texture"))
        mat->set_hair_data_texture(resolve_texture(jm["hair_data_texture"], resourcesPath, glbTextures,
                                                   TEXTURE_FORMAT_TYPE_NORMAL));
    if (jm.contains("tangent_texture"))
        mat->set_tangent_texture(resolve_texture(jm["tangent_texture"], resourcesPath, glbTextures,
                                                 TEXTURE_FORMAT_TYPE_NORMAL));

    warn_unknown(jm,
        {"type", "hair_color", "alpha_threshold", "roughness",
         "specular_intensity", "specular_shift", "color_variation",
         "root_color", "tip_color", "hair_data_texture", "tangent_texture"},
        "material(haircard)");

    return mat;
}

static Core::IMaterial* build_hairepic(const json& jm) {
    Vec3 tint(0.35f);
    if (jm.contains("tint_color"))
        tint = to_vec3(jm["tint_color"], tint);
    auto* mat = new Core::HairEpicMaterial(tint);

    if (jm.contains("thickness"))     mat->set_thickness(jm["thickness"].get<float>());
    if (jm.contains("roughness"))     mat->set_roughness(jm["roughness"].get<float>());
    if (jm.contains("specular"))      mat->set_specular(jm["specular"].get<float>());
    if (jm.contains("metallic"))      mat->set_metallic(jm["metallic"].get<float>());
    if (jm.contains("shift"))         mat->set_shift(jm["shift"].get<float>());
    if (jm.contains("ior"))           mat->set_ior(jm["ior"].get<float>());
    if (jm.contains("R"))             mat->set_R(jm["R"].get<bool>());
    if (jm.contains("R_power"))       mat->set_Rpower(jm["R_power"].get<float>());
    if (jm.contains("TT"))            mat->set_TT(jm["TT"].get<bool>());
    if (jm.contains("TT_power"))      mat->set_TTpower(jm["TT_power"].get<float>());
    if (jm.contains("TRT"))           mat->set_TRT(jm["TRT"].get<bool>());
    if (jm.contains("TRT_power"))     mat->set_TRTpower(jm["TRT_power"].get<float>());
    if (jm.contains("eumelanine"))    mat->set_eumelanine(jm["eumelanine"].get<float>());
    if (jm.contains("pheomelanine"))  mat->set_pheomelanine(jm["pheomelanine"].get<float>());
    if (jm.contains("use_pigmentation")) mat->use_pigmentation(jm["use_pigmentation"].get<bool>());
    if (jm.contains("use_scatter"))   mat->set_useScatter(jm["use_scatter"].get<bool>());
    if (jm.contains("use_glints"))    mat->use_glints(jm["use_glints"].get<bool>());
    if (jm.contains("adv_shadows"))   mat->set_adv_shadows(jm["adv_shadows"].get<bool>());
    if (jm.contains("density_boost")) mat->set_density_boost(jm["density_boost"].get<float>());
    if (jm.contains("scatter_boost")) mat->set_scatter_boost(jm["scatter_boost"].get<float>());
    if (jm.contains("root_darkening")) mat->set_root_darkening(jm["root_darkening"].get<float>());
    if (jm.contains("tip_bleaching")) mat->set_tip_bleaching(jm["tip_bleaching"].get<float>());
    if (jm.contains("tip_falloff"))   mat->set_tip_falloff(jm["tip_falloff"].get<float>());
    if (jm.contains("variability"))   mat->set_variabilty(jm["variability"].get<float>());

    warn_unknown(jm,
        {"type", "tint_color", "thickness", "roughness", "specular", "metallic",
         "shift", "ior", "R", "R_power", "TT", "TT_power", "TRT", "TRT_power",
         "eumelanine", "pheomelanine", "use_pigmentation", "use_scatter", "use_glints",
         "adv_shadows", "density_boost", "scatter_boost",
         "root_darkening", "tip_bleaching", "tip_falloff", "variability"},
        "material(hairepic)");

    return mat;
}

static Core::IMaterial* build_hair(const json& jm) {
    float eume = 1.3f, pheo = 0.2f;
    if (jm.contains("eumelanine"))   eume = jm["eumelanine"].get<float>();
    if (jm.contains("pheomelanine")) pheo = jm["pheomelanine"].get<float>();
    auto* mat = new Core::HairMaterial(eume, pheo);

    if (jm.contains("thickness"))    mat->set_thickness(jm["thickness"].get<float>());
    if (jm.contains("density"))      mat->set_density(jm["density"].get<float>());
    if (jm.contains("roughness"))    mat->set_roughness(jm["roughness"].get<float>());
    if (jm.contains("az_roughness")) mat->set_azimuthal_roughness(jm["az_roughness"].get<float>());
    if (jm.contains("shift"))        mat->set_shift(jm["shift"].get<float>());
    if (jm.contains("ior"))          mat->set_ior(jm["ior"].get<float>());
    if (jm.contains("R"))            mat->set_R(jm["R"].get<bool>());
    if (jm.contains("R_power"))      mat->set_Rpower(jm["R_power"].get<float>());
    if (jm.contains("TT"))           mat->set_TT(jm["TT"].get<bool>());
    if (jm.contains("TT_power"))     mat->set_TTpower(jm["TT_power"].get<float>());
    if (jm.contains("TRT"))          mat->set_TRT(jm["TRT"].get<bool>());
    if (jm.contains("TRT_power"))    mat->set_TRTpower(jm["TRT_power"].get<float>());
    if (jm.contains("use_scatter"))     mat->enable_scattering(jm["use_scatter"].get<bool>());
    if (jm.contains("use_pigmentation")) mat->use_pigmentation(jm["use_pigmentation"].get<bool>());

    warn_unknown(jm,
        {"type", "eumelanine", "pheomelanine", "thickness", "density",
         "roughness", "az_roughness", "shift", "ior",
         "R", "R_power", "TT", "TT_power", "TRT", "TRT_power",
         "use_scatter", "use_pigmentation"},
        "material(hair)");

    return mat;
}

static Core::IMaterial* build_hairdisney(const json& jm) {
    auto* mat = new Core::HairDisneyMaterial();
    if (jm.contains("r_color"))           mat->set_R_color(to_vec3(jm["r_color"]));
    if (jm.contains("tt_color"))          mat->set_TT_color(to_vec3(jm["tt_color"]));
    if (jm.contains("trt_color"))         mat->set_TRT_color(to_vec3(jm["trt_color"]));
    if (jm.contains("backscatter_color")) mat->set_backscatter_color(to_vec3(jm["backscatter_color"]));
    if (jm.contains("frontscatter_color")) mat->set_frontscatter_color(to_vec3(jm["frontscatter_color"]));

    if (jm.contains("r_intensity"))   mat->set_R_intensity(jm["r_intensity"].get<float>());
    if (jm.contains("tt_intensity"))  mat->set_TT_intensity(jm["tt_intensity"].get<float>());
    if (jm.contains("trt_intensity")) mat->set_TRT_intensity(jm["trt_intensity"].get<float>());
    if (jm.contains("glints_intensity")) mat->set_glints_intensity(jm["glints_intensity"].get<float>());
    if (jm.contains("frontscatter_intensity")) mat->set_frontscatter_intensity(jm["frontscatter_intensity"].get<float>());
    if (jm.contains("backscatter_intensity")) mat->set_backscatter_intensity(jm["backscatter_intensity"].get<float>());

    if (jm.contains("thickness"))    mat->set_thickness(jm["thickness"].get<float>());
    if (jm.contains("density"))      mat->set_density(jm["density"].get<float>());
    if (jm.contains("roughness"))    mat->set_roughness(jm["roughness"].get<float>());
    if (jm.contains("az_roughness")) mat->set_azimuthal_roughness(jm["az_roughness"].get<float>());
    if (jm.contains("shift"))        mat->set_shift(jm["shift"].get<float>());
    if (jm.contains("ior"))          mat->set_ior(jm["ior"].get<float>());
    if (jm.contains("R"))            mat->set_R(jm["R"].get<bool>());
    if (jm.contains("TT"))           mat->set_TT(jm["TT"].get<bool>());
    if (jm.contains("TRT"))          mat->set_TRT(jm["TRT"].get<bool>());
    if (jm.contains("use_scatter"))     mat->enable_scattering(jm["use_scatter"].get<bool>());
    if (jm.contains("use_pigmentation")) mat->use_pigmentation(jm["use_pigmentation"].get<bool>());

    warn_unknown(jm,
        {"type", "r_color", "tt_color", "trt_color",
         "backscatter_color", "frontscatter_color",
         "r_intensity", "tt_intensity", "trt_intensity",
         "glints_intensity", "frontscatter_intensity", "backscatter_intensity",
         "thickness", "density", "roughness", "az_roughness",
         "shift", "ior", "R", "TT", "TRT", "use_scatter", "use_pigmentation"},
        "material(hairdisney)");

    return mat;
}

static Core::IMaterial* build_unlit(const json&                        jm,
                                    const std::string&                 resourcesPath,
                                    const std::vector<Core::Texture*>& glbTextures) {
    Vec4 color(1.0f, 1.0f, 0.5f, 1.0f);
    if (jm.contains("color"))
        color = to_vec4(jm["color"], color);
    auto* mat = new Core::UnlitMaterial(color);

    if (jm.contains("color_texture"))
        mat->set_color_texture(resolve_texture(jm["color_texture"], resourcesPath, glbTextures,
                                               TEXTURE_FORMAT_TYPE_COLOR));

    warn_unknown(jm, {"type", "color", "color_texture"}, "material(unlit)");
    return mat;
}

static Core::IMaterial* build_material_inline(const json&                        jm,
                                              const std::string&                 resourcesPath,
                                              const std::vector<Core::Texture*>& glbTextures) {
    require(jm, "type", "material");
    const std::string type = jm.at("type").get<std::string>();
    if (type == "pbr")        return build_pbr(jm, resourcesPath, glbTextures);
    if (type == "haircard")   return build_haircard(jm, resourcesPath, glbTextures);
    if (type == "hairepic")   return build_hairepic(jm);
    if (type == "hair")       return build_hair(jm);
    if (type == "hairdisney") return build_hairdisney(jm);
    if (type == "unlit")      return build_unlit(jm, resourcesPath, glbTextures);
    throw std::runtime_error("scene_loader: unknown material type '" + type + "'");
}

// ─── material library ───────────────────────────────────────────────────────

// Resolves a mesh's "material" field, which can take three shapes:
//   1. Inline object with "type"  → build fresh material (legacy behavior).
//   2. String "<name>"            → shared reference to a library entry. The
//                                   library material is built lazily the first
//                                   time it's referenced and the same pointer
//                                   is returned for every subsequent reference,
//                                   so GUI tweaks propagate across every mesh
//                                   that uses the name.
//   3. Object { "base": "<name>", ...overrides } → independent instance built
//                                   by deep-merging the override fields over
//                                   the library entry's JSON (merge_patch /
//                                   RFC 7396 semantics — override keys win).
//                                   Each override site produces its own
//                                   material, the shared library entry is
//                                   untouched.
struct MaterialLibrary {
    // Definition JSON keyed by name, populated from the top-level "materials" block.
    std::unordered_map<std::string, json> defs;
    // Cache of built materials for by-name (shared) references.
    std::unordered_map<std::string, Core::IMaterial*> sharedCache;
};

static Core::IMaterial* resolve_material(const json&                        jm,
                                         const std::string&                 resourcesPath,
                                         const std::vector<Core::Texture*>& glbTextures,
                                         MaterialLibrary&                   lib) {
    // Case 2: bare string → shared library reference.
    if (jm.is_string()) {
        const std::string name = jm.get<std::string>();
        auto cacheIt = lib.sharedCache.find(name);
        if (cacheIt != lib.sharedCache.end())
            return cacheIt->second;
        auto defIt = lib.defs.find(name);
        if (defIt == lib.defs.end())
            throw std::runtime_error("scene_loader: material reference '" + name +
                                     "' has no entry in the top-level 'materials' library");
        // Library entries are scene-global → no GLB texture context available.
        Core::IMaterial* mat = build_material_inline(defIt->second, resourcesPath, {});
        lib.sharedCache[name] = mat;
        return mat;
    }

    if (!jm.is_object())
        throw std::runtime_error("scene_loader: material must be an object or a library-entry name (string)");

    // Case 3: { "base": "<name>", ... } → cloned instance with overrides.
    if (jm.contains("base")) {
        if (jm.contains("type"))
            throw std::runtime_error("scene_loader: material cannot specify both 'base' and 'type' "
                                     "— use one form or the other");
        const std::string name = jm.at("base").get<std::string>();
        auto defIt = lib.defs.find(name);
        if (defIt == lib.defs.end())
            throw std::runtime_error("scene_loader: material base '" + name +
                                     "' has no entry in the top-level 'materials' library");
        json merged = defIt->second;
        json overrides = jm;
        overrides.erase("base");
        merged.merge_patch(overrides); // RFC 7396 — override keys win, missing fields keep base values
        return build_material_inline(merged, resourcesPath, glbTextures);
    }

    // Case 1: inline material object with "type".
    return build_material_inline(jm, resourcesPath, glbTextures);
}

// ─── meshes ─────────────────────────────────────────────────────────────────

// Forward references for joint attachments (e.g. hair attached to character
// head). Resolved in a second pass once every mesh exists and can be looked up
// by name.
struct PendingAttachment {
    Core::Mesh* child;          // mesh that declared attach_to
    std::string sourceName;     // name of the mesh holding the skeleton
    std::string jointName;      // joint inside that skeleton
};

static Core::Mesh* build_mesh(const json&                     jm,
                              const std::string&              resourcesPath,
                              std::vector<PendingAttachment>& pending,
                              MaterialLibrary&                lib) {
    require(jm, "type", "mesh");
    require(jm, "file", "mesh");

    const std::string type     = jm.at("type").get<std::string>();
    const std::string file     = jm.at("file").get<std::string>();
    const std::string fullPath = resourcesPath + file;

    auto* mesh = new Core::Mesh();
    std::vector<Core::Texture*> glbTextures; // only populated for type == "glb"

    if (type == "glb") {
        int meshIndex = jm.value("glb_mesh_index", -1);
        Tools::Loaders::load_GLB(mesh, fullPath, meshIndex, &glbTextures);
    } else if (type == "obj" || type == "ply" || type == "hair") {
        Tools::Loaders::load_3D_file(mesh, fullPath, false);
        if (mesh->get_num_geometries() == 0) {
            throw std::runtime_error("scene_loader: mesh '" +
                jm.value("name", std::string("?")) + "' (" + type + ") at '" +
                fullPath + "' loaded zero geometries — file missing, unreadable, or malformed");
        }
    } else if (type == "neural_hair") {
        // Matches application.cpp's threaded path for neural avatars.
        std::thread t(hair_loaders::load_neural_hair, mesh, fullPath.c_str(),
                      /*skullMesh*/ nullptr,
                      jm.value("preload", true),
                      jm.value("verbose", false),
                      jm.value("calculate_tangents", false),
                      jm.value("save_output", false));
        t.detach();
    } else {
        throw std::runtime_error("scene_loader: unknown mesh type '" + type + "'");
    }

    // Transform
    if (jm.contains("position")) mesh->set_position(to_vec3(jm["position"]));
    if (jm.contains("scale"))    mesh->set_scale(to_scale(jm["scale"]));
    if (jm.contains("rotation")) mesh->set_rotation(to_vec3(jm["rotation"]));

    // Material — may be inline, a library reference (string), or { base, ...overrides }.
    if (jm.contains("material")) {
        auto* mat = resolve_material(jm["material"], resourcesPath, glbTextures, lib);
        mesh->push_material(mat);
    }

    // Optional extra material slots, used by GLB meshes with multiple primitives
    // (e.g. body + teeth + tongue). Each entry is resolved like `material` and
    // appended after the primary material.
    if (jm.contains("extra_materials")) {
        if (!jm["extra_materials"].is_array())
            throw std::runtime_error("scene_loader: 'extra_materials' must be an array");
        for (const auto& em : jm["extra_materials"])
            mesh->push_material(resolve_material(em, resourcesPath, glbTextures, lib));
    }

    // Optional per-primitive material slot mapping. Entry i is the material slot
    // index used by geometry i (= primitive i in load order). Required when a
    // mesh has more than one geometry and you want anything other than every
    // geometry using slot 0.
    if (jm.contains("primitive_materials")) {
        if (!jm["primitive_materials"].is_array())
            throw std::runtime_error("scene_loader: 'primitive_materials' must be an array of slot indices");
        const auto& pm = jm["primitive_materials"];
        const size_t numGeoms = mesh->get_num_geometries();
        const size_t numMats  = mesh->get_num_materials();
        for (size_t i = 0; i < pm.size() && i < numGeoms; ++i) {
            size_t slot = pm[i].get<size_t>();
            if (slot >= numMats)
                throw std::runtime_error("scene_loader: 'primitive_materials[" + std::to_string(i) +
                                         "]' references slot " + std::to_string(slot) +
                                         " but mesh has only " + std::to_string(numMats) + " material slot(s)");
            mesh->set_material_ID(i, slot);
        }
    }

    // Name / flags
    if (jm.contains("name"))            mesh->set_name(jm["name"].get<std::string>());
    if (jm.contains("active"))          mesh->set_active(jm["active"].get<bool>());
    if (jm.contains("cast_shadows"))    mesh->cast_shadows(jm["cast_shadows"].get<bool>());
    if (jm.contains("affected_by_fog")) mesh->affected_by_fog(jm["affected_by_fog"].get<bool>());

    // Child meshes — transforms are inherited from this parent.
    if (jm.contains("children")) {
        for (const auto& jc : jm["children"])
            mesh->add_child(build_mesh(jc, resourcesPath, pending, lib));
    }

    // Joint attachment — resolved after every top-level mesh is built so the
    // source mesh's name lookup succeeds regardless of declaration order.
    if (jm.contains("attach_to")) {
        const auto& ja = jm["attach_to"];
        require(ja, "mesh",  "attach_to");
        require(ja, "joint", "attach_to");
        pending.push_back({mesh,
                           ja["mesh"].get<std::string>(),
                           ja["joint"].get<std::string>()});
    }

    warn_unknown(jm,
        {"name", "type", "file", "glb_mesh_index",
         "preload", "verbose", "calculate_tangents", "save_output",
         "position", "scale", "rotation",
         "material", "extra_materials", "primitive_materials",
         "animation", "children", "attach_to",
         "active", "cast_shadows", "affected_by_fog"},
        "mesh");

    return mesh;
}

// ─── animation attach ───────────────────────────────────────────────────────

static bool attach_animation(Core::Mesh* mesh, const std::string& animPath) {
    Core::Geometry* g = mesh->get_geometry(0);
    if (!g)
        return false;
    const auto& props = g->get_properties();

    static const Core::SkinData        emptySkin;
    static const Core::MorphTargetData emptyMorphs;
    const Core::SkinData*        skin   = props.skinData.has_value()       ? &*props.skinData       : &emptySkin;
    const Core::MorphTargetData* morphs = props.morphTargetData.has_value() ? &*props.morphTargetData : &emptyMorphs;

    try {
        Core::Animation anim = Core::load_animation_json(animPath, *skin, *morphs);
        mesh->set_animation(std::make_unique<Core::Animation>(std::move(anim)));
        LOG_DEBUG("scene_loader: animation '" + animPath + "' attached to mesh '" + mesh->get_name() + "'");
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("scene_loader: animation load failed: ") + e.what());
        return false;
    }
}

// ─── public entry ───────────────────────────────────────────────────────────

LoadResult load_scene_json(const std::string&     scenePath,
                           const std::string&     resourcesPath,
                           const std::string&     engineResourcesPath,
                           const std::string&     animationOverride,
                           Systems::BaseRenderer* renderer) {
    std::ifstream file(scenePath);
    if (!file.is_open())
        throw std::runtime_error("scene_loader: cannot open " + scenePath);

    json root;
    file >> root;

    LoadResult result;

    // ── camera ──────────────────────────────────────────────────────────────
    result.camera = new Core::Camera();
    if (root.contains("camera")) {
        const auto& jc = root["camera"];
        if (jc.contains("position")) result.camera->set_position(to_vec3(jc["position"]));
        if (jc.contains("near"))     result.camera->set_near(jc["near"].get<float>());
        if (jc.contains("far"))      result.camera->set_far(jc["far"].get<float>());
        if (jc.contains("fov"))      result.camera->set_field_of_view(jc["fov"].get<float>());
        warn_unknown(jc, {"position", "near", "far", "fov"}, "camera");
    }

    result.scene = new Core::Scene(result.camera);

    // ── material library ────────────────────────────────────────────────────
    // Optional top-level "materials" object: { "name": { type: ..., ... }, ... }.
    // Each entry is stored as JSON and only built on first reference. Meshes
    // can reference an entry by name (shared instance) or via { "base":
    // "<name>", ...overrides } (independent instance — see SCENE.md §6.8).
    MaterialLibrary materialLib;
    if (root.contains("materials")) {
        const auto& jmat = root["materials"];
        if (!jmat.is_object())
            throw std::runtime_error("scene_loader: top-level 'materials' must be an object");
        for (auto it = jmat.begin(); it != jmat.end(); ++it) {
            if (!it.value().is_object() || !it.value().contains("type"))
                throw std::runtime_error("scene_loader: material '" + it.key() +
                                         "' in the library must be an object with a 'type' field");
            materialLib.defs.emplace(it.key(), it.value());
        }
    }

    // ── lights ──────────────────────────────────────────────────────────────
    if (root.contains("lights")) {
        for (const auto& jl : root["lights"]) {
            Core::Light* light = build_light(jl, engineResourcesPath);
            result.scene->add(light);
        }
    }

    // ── meshes ──────────────────────────────────────────────────────────────
    // Build all meshes first. Remember which one (if any) declared an animation,
    // and which is the first skinned mesh — both are candidates for the SLViewer
    // override.
    Core::Mesh* firstWithAnimField = nullptr;
    std::string firstAnimFieldPath;
    Core::Mesh* firstSkinned = nullptr;

    std::vector<PendingAttachment> pendingAttachments;

    if (root.contains("meshes")) {
        for (const auto& jm : root["meshes"]) {
            Core::Mesh* mesh = build_mesh(jm, resourcesPath, pendingAttachments, materialLib);
            result.scene->add(mesh);

            if (jm.contains("animation") && firstWithAnimField == nullptr) {
                firstWithAnimField = mesh;
                firstAnimFieldPath = jm["animation"].get<std::string>();
            }
            if (!firstSkinned) {
                Core::Geometry* g = mesh->get_geometry(0);
                if (g && g->get_properties().skinData.has_value())
                    firstSkinned = mesh;
            }
        }
    }

    // ── resolve joint attachments ───────────────────────────────────────────
    for (const auto& pa : pendingAttachments) {
        Core::Mesh* source = nullptr;
        for (Core::Mesh* m : result.scene->get_meshes()) {
            if (m && m->get_name() == pa.sourceName) { source = m; break; }
        }
        if (!source) {
            LOG_ERROR("scene_loader: attach_to references unknown mesh '" +
                      pa.sourceName + "' — '" + pa.child->get_name() + "' will not be attached");
            continue;
        }
        auto* anchor = new Core::JointAttachment(source, pa.jointName);
        anchor->set_name(pa.child->get_name() + "_attach_" + pa.jointName);
        source->add_child(anchor);
        pa.child->set_parent(anchor);
    }

    // ── animation: override beats scene field ───────────────────────────────
    if (!animationOverride.empty()) {
        Core::Mesh* target = firstWithAnimField ? firstWithAnimField : firstSkinned;
        if (target) {
            if (attach_animation(target, animationOverride))
                result.primaryAnimated = target;
        } else {
            LOG_WARN("scene_loader: animation override given but no animated/skinned mesh found");
        }
    } else if (firstWithAnimField) {
        if (attach_animation(firstWithAnimField, resourcesPath + firstAnimFieldPath))
            result.primaryAnimated = firstWithAnimField;
    }
    // Else: no animation at all — rest pose.

    // ── scene globals ───────────────────────────────────────────────────────
    if (root.contains("scene")) {
        const auto& js = root["scene"];
        if (js.contains("ambient_color"))     result.scene->set_ambient_color(to_vec3(js["ambient_color"]));
        if (js.contains("ambient_intensity")) result.scene->set_ambient_intensity(js["ambient_intensity"].get<float>());
        if (js.contains("use_ibl"))           result.scene->set_use_IBL(js["use_ibl"].get<bool>());

        if (js.contains("fog")) {
            const auto& jf = js["fog"];
            if (jf.contains("enabled")) result.scene->enable_fog(jf["enabled"].get<bool>());
            if (jf.contains("color"))   result.scene->set_fog_color(to_vec3(jf["color"]));
            if (jf.contains("intensity")) result.scene->set_fog_intensity(jf["intensity"].get<float>());
            warn_unknown(jf, {"enabled", "color", "intensity"}, "scene.fog");
        }

        if (js.contains("skybox")) {
            const auto& jsky = js["skybox"];
            require(jsky, "hdri", "scene.skybox");
            auto* envMap = new Core::TextureHDR();
            Tools::Loaders::load_HDRi(envMap, resourcesPath + jsky["hdri"].get<std::string>());
            auto* sky = new Core::Skybox(envMap);
            if (jsky.contains("intensity")) sky->set_color_intensity(jsky["intensity"].get<float>());
            if (jsky.contains("rotation"))  sky->set_rotation(jsky["rotation"].get<float>());
            if (jsky.contains("blurriness")) sky->set_blurriness(jsky["blurriness"].get<float>());
            result.scene->set_skybox(sky);
            warn_unknown(jsky, {"hdri", "intensity", "rotation", "blurriness"}, "scene.skybox");
        }

        warn_unknown(js, {"ambient_color", "ambient_intensity", "use_ibl", "fog", "skybox"}, "scene");
    }

    // ── renderer hooks ──────────────────────────────────────────────────────
    if (root.contains("renderer")) {
        const auto& jr = root["renderer"];
        if (jr.contains("clear_color"))
            result.clearColor = to_vec4(jr["clear_color"], result.clearColor);
        if (renderer && jr.contains("sss_scatter_lut")) {
            if (auto* fwd = dynamic_cast<Systems::ForwardRenderer*>(renderer))
                fwd->load_sss_scatter_lut(resourcesPath + jr["sss_scatter_lut"].get<std::string>());
        }
        warn_unknown(jr, {"clear_color", "sss_scatter_lut"}, "renderer");
    }

    warn_unknown(root, {"name", "camera", "lights", "materials", "meshes", "scene", "renderer"}, "root");

    return result;
}

} // namespace scene_loader
