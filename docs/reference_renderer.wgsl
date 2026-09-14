// Reference voxel renderer (WGSL) from the project owner - the pass 3.5
// look features were ported from it: per-vertex simulated ambient
// occlusion (vertex_ao / calculate_ao), the sky model (compute_env:
// horizon/zenith gradient + haze + sun glow/core) and the sky palette.
// Kept verbatim for future reference (the selection outline and ground
// grid are not ported yet).

const BASE_COLOR = vec3<f32>(1.0, 1.0, 1.0);
const SKY_HORIZON_COLOR = vec3<f32>(0.8, 0.9, 1.0);
const SKY_ZENITH_COLOR = vec3<f32>(0.45, 0.62, 0.95);
const SUN_COLOR = vec3<f32>(1.0, 0.95, 0.85);
const SUN_DIR = normalize(vec3<f32>(0.5, 1.0, 0.5));
const GROUND_Y: f32 = 0.0;

struct Uniforms {
    resolution : vec2<f32>,
    _pad0 : vec2<f32>,

    camPos : vec3<f32>,
    _pad1 : f32,

    forward : vec3<f32>,
    _pad2 : f32,

    right : vec3<f32>,
    _pad3 : f32,

    up : vec3<f32>,
    fov : f32,

    chunkSize : f32,
    worldSize : f32,
    _pad4 : vec2<f32>,
    sel : vec4<f32>,
};

struct ChunkMap {
    slots: array<i32>
};

@group(0) @binding(0) var<uniform> u : Uniforms;
@group(0) @binding(1) var voxels : texture_3d<u32>;
@group(0) @binding(2) var<storage, read> chunkMap : ChunkMap;

@vertex
fn vs_main(@builtin(vertex_index) vertexIndex : u32) -> @builtin(position) vec4<f32> {
    const pos = array<vec2<f32>, 3>(
        vec2<f32>(-1.0, -1.0),
        vec2<f32>(3.0, -1.0),
        vec2<f32>(-1.0, 3.0)
    );
    return vec4<f32>(pos[vertexIndex], 0.0, 1.0);
}

fn safe_div(a: f32, b: f32) -> f32 {
    if (b == 0.0) {
        return 1e30;
    }
    return a / b;
}

fn saturate(x: vec2<f32>) -> vec2<f32> {
    return clamp(x, vec2<f32>(0.0), vec2<f32>(1.0));
}

fn ndc_from_pixel(pixel: vec2<f32>, invResolution: vec2<f32>) -> vec2<f32> {
    return vec2<f32>(
        pixel.x * invResolution.x * 2.0 - 1.0,
        1.0 - pixel.y * invResolution.y * 2.0
    );
}

fn make_ray_dir(ndc: vec2<f32>, aspect: f32, scale: f32) -> vec3<f32> {
    let p = vec2<f32>(ndc.x * aspect, ndc.y);
    return normalize(u.forward + p.x * scale * u.right + p.y * scale * u.up);
}

fn floor_div(a: i32, b: i32) -> i32 {
    if (a >= 0) {
        return a / b;
    }
    return (a - b + 1) / b;
}

fn floor_mod(a: i32, b: i32) -> i32 {
    return a - b * floor_div(a, b);
}

fn mod_pos(a: i32, b: i32) -> i32 {
    return ((a % b) + b) % b;
}

fn voxel_at(p: vec3<i32>) -> u32 {
    let chunkSize = i32(u.chunkSize);
    let worldSize = i32(u.worldSize);

    let cx = floor_div(p.x, chunkSize);
    let cy = floor_div(p.y, chunkSize);
    let cz = floor_div(p.z, chunkSize);

    if (cy != 0) {
        return 0u;
    }

    let mx = mod_pos(cx, worldSize);
    let my = mod_pos(cy, worldSize);
    let mz = mod_pos(cz, worldSize);

    let mapIdx = mx + my * worldSize + mz * worldSize * worldSize;
    let slot = chunkMap.slots[mapIdx];

    if (slot < 0) {
        return 0u;
    }

    let lx = floor_mod(p.x, chunkSize);
    let ly = floor_mod(p.y, chunkSize);
    let lz = floor_mod(p.z, chunkSize);

    let texCoord = vec3<i32>(lx, ly, (slot * chunkSize) + lz);
    return textureLoad(voxels, texCoord, 0).x;
}

fn initial_tmax(pos: f32, dir: f32, voxel: i32, step: i32) -> f32 {
    if (step > 0) {
        return (f32(voxel + 1) - pos) / dir;
    }
    return (pos - f32(voxel)) / -dir;
}

fn vertex_ao(side1: bool, side2: bool, corner: bool) -> f32 {
    if (side1 && side2) {
        return 0.0;
    }
    let s1 = select(0.0, 1.0, side1);
    let s2 = select(0.0, 1.0, side2);
    let c = select(0.0, 1.0, corner);
    return 1.0 - (s1 + s2 + c) / 3.0;
}

fn calculate_ao(voxel: vec3<i32>, hitNormal: vec3<f32>, hitPos: vec3<f32>) -> f32 {
    if (dot(abs(hitNormal), vec3<f32>(1.0)) < 0.5) {
        return 1.0;
    }

    let p = voxel + vec3<i32>(hitNormal);

    var du: vec3<i32>;
    var dv: vec3<i32>;
    var localPos: vec2<f32>;

    let absN = abs(hitNormal);
    let fractPos = fract(hitPos);

    if (absN.x > 0.5) {
        du = vec3<i32>(0, 0, 1);
        dv = vec3<i32>(0, 1, 0);
        localPos = fractPos.zy;
    } else if (absN.y > 0.5) {
        du = vec3<i32>(1, 0, 0);
        dv = vec3<i32>(0, 0, 1);
        localPos = fractPos.xz;
    } else {
        du = vec3<i32>(1, 0, 0);
        dv = vec3<i32>(0, 1, 0);
        localPos = fractPos.xy;
    }

    let s_minus_u = voxel_at(p - du) > 0u;
    let s_plus_u = voxel_at(p + du) > 0u;
    let s_minus_v = voxel_at(p - dv) > 0u;
    let s_plus_v = voxel_at(p + dv) > 0u;

    let c_minus_u_minus_v = voxel_at(p - du - dv) > 0u;
    let c_plus_u_minus_v = voxel_at(p + du - dv) > 0u;
    let c_minus_u_plus_v = voxel_at(p - du + vec3<i32>(dv)) > 0u;
    let c_plus_u_plus_v = voxel_at(p + du + dv) > 0u;

    let ao00 = vertex_ao(s_minus_u, s_minus_v, c_minus_u_minus_v);
    let ao10 = vertex_ao(s_plus_u, s_minus_v, c_plus_u_minus_v);
    let ao01 = vertex_ao(s_minus_u, s_plus_v, c_minus_u_plus_v);
    let ao11 = vertex_ao(s_plus_u, s_plus_v, c_plus_u_plus_v);

    let w00 = (1.0 - localPos.x) * (1.0 - localPos.y);
    let w10 = localPos.x * (1.0 - localPos.y);
    let w01 = (1.0 - localPos.x) * localPos.y;
    let w11 = localPos.x * localPos.y;

    return ao00 * w00 + ao10 * w10 + ao01 * w01 + ao11 * w11;
}

fn pristine_grid(uv: vec2<f32>, lineWidth: vec2<f32>) -> f32 {
    let uvDDXY = vec4<f32>(dpdx(uv), dpdy(uv));
    let uvDeriv = vec2<f32>(length(uvDDXY.xz), length(uvDDXY.yw));

    let invertLine = lineWidth > vec2<f32>(0.5);
    let targetWidth = select(lineWidth, 1.0 - lineWidth, invertLine);
    let drawWidth = clamp(targetWidth, uvDeriv, vec2<f32>(0.5));
    let lineAA = uvDeriv * 1.5;

    var gridUV = abs(fract(uv) * 2.0 - 1.0);
    gridUV = select(1.0 - gridUV, gridUV, invertLine);

    var grid2 = smoothstep(drawWidth + lineAA, drawWidth - lineAA, gridUV);
    grid2 *= saturate(targetWidth / drawWidth);

    grid2 = mix(grid2, targetWidth, saturate(uvDeriv * 2.0 - 1.0));
    grid2 = select(grid2, 1.0 - grid2, invertLine);

    return mix(grid2.x, 1.0, grid2.y);
}

struct SkyColors {
    sky : vec3<f32>,
    sun : vec3<f32>
};

fn compute_env(rayDir: vec3<f32>) -> SkyColors {
    let sunAmount = max(dot(rayDir, SUN_DIR), 0.0);
    let up = clamp(rayDir.y * 0.5 + 0.5, 0.0, 1.0);
    let horizon = pow(1.0 - abs(rayDir.y), 3.0);

    let sky = mix(SKY_HORIZON_COLOR, SKY_ZENITH_COLOR, smoothstep(0.0, 1.0, up));
    let haze = vec3<f32>(1.0, 0.95, 0.9) * (0.12 * horizon + 0.04 * horizon * horizon);

    let sunGlow = pow(sunAmount, 48.0) * 0.8;
    let sunCore = pow(sunAmount, 512.0) * 8.0;

    return SkyColors(
        sky + haze,
        SUN_COLOR * (sunGlow + sunCore)
    );
}

fn trace_voxel(rayDir: vec3<f32>, skyColor: vec3<f32>, sunColor: vec3<f32>) -> vec4<f32> {
    let chunkSize = i32(u.chunkSize);
    let worldSize = i32(u.worldSize);

    var curChunk = vec3<i32>(
        floor_div(i32(floor(u.camPos.x)), chunkSize),
        floor_div(i32(floor(u.camPos.y)), chunkSize),
        floor_div(i32(floor(u.camPos.z)), chunkSize)
    );

    var stepC = vec3<i32>(select(-1, 1, rayDir.x > 0.0), select(-1, 1, rayDir.y > 0.0), select(-1, 1, rayDir.z > 0.0));
    var tDeltaChunk = vec3<f32>(abs(safe_div(f32(chunkSize), rayDir.x)), abs(safe_div(f32(chunkSize), rayDir.y)), abs(safe_div(f32(chunkSize), rayDir.z)));

    var tMaxChunk = vec3<f32>(
        initial_tmax(u.camPos.x / f32(chunkSize), rayDir.x / f32(chunkSize), curChunk.x, stepC.x),
        initial_tmax(u.camPos.y / f32(chunkSize), rayDir.y / f32(chunkSize), curChunk.y, stepC.y),
        initial_tmax(u.camPos.z / f32(chunkSize), rayDir.z / f32(chunkSize), curChunk.z, stepC.z)
    );

    var traveled = 0.0;
    let MAX_DIST = 1000.0;
    for (var outer = 0; outer < 1024; outer = outer + 1) {
        let mx = mod_pos(curChunk.x, worldSize);
        let my = mod_pos(curChunk.y, worldSize);
        let mz = mod_pos(curChunk.z, worldSize);
        let mapIdx = mx + my * worldSize + mz * worldSize * worldSize;
        let slot = chunkMap.slots[mapIdx];

        if (slot >= 0) {
            let rayOriginInner = u.camPos + rayDir * traveled;
            var voxel = vec3<i32>(floor(rayOriginInner));

            let chunkMin = curChunk * chunkSize;
            let chunkMax = chunkMin + vec3<i32>(chunkSize - 1);
            voxel = clamp(voxel, chunkMin, chunkMax);

            let stepV = vec3<i32>(select(-1, 1, rayDir.x > 0.0), select(-1, 1, rayDir.y > 0.0), select(-1, 1, rayDir.z > 0.0));
            let tDeltaV = vec3<f32>(abs(safe_div(1.0, rayDir.x)), abs(safe_div(1.0, rayDir.y)), abs(safe_div(1.0, rayDir.z)));
            var tMaxV = vec3<f32>(
                initial_tmax(rayOriginInner.x, rayDir.x, voxel.x, stepV.x),
                initial_tmax(rayOriginInner.y, rayDir.y, voxel.y, stepV.y),
                initial_tmax(rayOriginInner.z, rayDir.z, voxel.z, stepV.z)
            );

            var hitNormal = vec3<f32>(0.0);

            for (var inner = 0; inner < chunkSize * 2; inner = inner + 1) {
                if (voxel_at(voxel) > 0u) {
                    let tHit = traveled + max(0.0, dot(abs(hitNormal), tMaxV - tDeltaV));
                    let hitPos = u.camPos + rayDir * tHit;
                    let ao = calculate_ao(voxel, hitNormal, hitPos);

                    let nDotSun = max(dot(hitNormal, SUN_DIR), 0.0);
                    let hemi = clamp(hitNormal.y * 0.5 + 0.5, 0.0, 1.0);
                    let view = max(dot(hitNormal, -rayDir), 0.0);
                    let fresnel = pow(1.0 - view, 5.0);

                    let ambient = mix(skyColor * 0.35, skyColor, hemi);
                    let direct = sunColor * (0.2 * nDotSun + 0.8 * pow(nDotSun, 8.0));
                    let rim = skyColor * (0.12 * fresnel);

                    var col = BASE_COLOR * (ambient + direct + rim) * ao;

                    if (u.sel.x > 0.5) {
                        let selVoxel = vec3<i32>(i32(u.sel.y), i32(u.sel.z), i32(u.sel.w));
                        if (all(voxel == selVoxel)) {
                            let fracPos = fract(hitPos);
                            let minFrac = min(min(fracPos.x, 1.0 - fracPos.x), min(fracPos.y, 1.0 - fracPos.y));
                            if (minFrac < 0.03) {
                                return vec4<f32>(vec3<f32>(0.0), 1.0);
                            }
                            col = 1.0 - col * 0.5;
                        }
                    }

                    return vec4<f32>(col, 1.0);
                }

                if (tMaxV.x < tMaxV.y && tMaxV.x < tMaxV.z) {
                    voxel.x += stepV.x;
                    tMaxV.x += tDeltaV.x;
                    hitNormal = vec3<f32>(-f32(stepV.x), 0.0, 0.0);
                } else if (tMaxV.y < tMaxV.z) {
                    voxel.y += stepV.y;
                    tMaxV.y += tDeltaV.y;
                    hitNormal = vec3<f32>(0.0, -f32(stepV.y), 0.0);
                } else {
                    voxel.z += stepV.z;
                    tMaxV.z += tDeltaV.z;
                    hitNormal = vec3<f32>(0.0, 0.0, -f32(stepV.z));
                }

                if (floor_div(voxel.x, chunkSize) != curChunk.x || floor_div(voxel.y, chunkSize) != curChunk.y || floor_div(voxel.z, chunkSize) != curChunk.z) {
                    break;
                }
            }
        }

        if (tMaxChunk.x < tMaxChunk.y && tMaxChunk.x < tMaxChunk.z) {
            traveled = tMaxChunk.x;
            curChunk.x += stepC.x;
            tMaxChunk.x += tDeltaChunk.x;
        } else if (tMaxChunk.y < tMaxChunk.z) {
            traveled = tMaxChunk.y;
            curChunk.y += stepC.y;
            tMaxChunk.y += tDeltaChunk.y;
        } else {
            traveled = tMaxChunk.z;
            curChunk.z += stepC.z;
            tMaxChunk.z += tDeltaChunk.z;
        }

        if (traveled > MAX_DIST) {
            break;
        }
    }

    return vec4<f32>(0.0);
}

@fragment
fn fs_main(@builtin(position) fragCoord: vec4<f32>) -> @location(0) vec4<f32> {
    let aspect = u.resolution.x / u.resolution.y;
    let scale = tan(u.fov * 0.5);
    let invResolution = vec2<f32>(1.0 / u.resolution.x, 1.0 / u.resolution.y);

    let centerNdc = ndc_from_pixel(fragCoord.xy, invResolution);
    let centerRayDir = make_ray_dir(centerNdc, aspect, scale);

    let tPlaneCenter = (GROUND_Y - u.camPos.y) / centerRayDir.y;
    let planeHitCenter = u.camPos + centerRayDir * max(tPlaneCenter, 0.001);
    let pGrid = pristine_grid(planeHitCenter.xz * 0.5, vec2<f32>(0.01));

    var totalColor = vec3<f32>(0.0);

    let sampleCoord = fragCoord.xy;

    let ndc = ndc_from_pixel(sampleCoord, invResolution);
    let rayDir = make_ray_dir(ndc, aspect, scale);

    let envColors = compute_env(rayDir);
    let voxelData = trace_voxel(rayDir, envColors.sky, envColors.sun);

    if (voxelData.w > 0.0) {
        totalColor += voxelData.xyz;
    } else {
        if (rayDir.y < 0.0 && ((GROUND_Y - u.camPos.y) / rayDir.y) > 0.0) {
            let distFade = clamp(1.0 - (tPlaneCenter / 1000.0), 0.0, 1.0);
            let gridColor = vec3<f32>(1.0) * pGrid * distFade;
            totalColor += gridColor + (envColors.sky + envColors.sun) * (1.0 - pGrid);
        } else {
            totalColor += envColors.sky + envColors.sun;
        }
    }

    return vec4<f32>(totalColor, 1.0);
}
