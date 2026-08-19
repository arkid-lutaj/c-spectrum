/*
 * C-Spectrum — CRT Post-Processing Fragment Shader
 *
 * Effects:
 *   - Barrel distortion (simulates curved CRT glass)
 *   - Scanlines (horizontal darkening bands)
 *   - Vignette (darkened edges)
 *   - Chromatic aberration (RGB channel offset)
 *   - Subtle noise grain
 */

#version 330

in vec2 fragTexCoord;
in vec4 fragColor;

out vec4 finalColor;

uniform sampler2D texture0;
uniform vec2  resolution;
uniform float time;

/* ── Barrel Distortion ──────────────────────────────────────────────────── */

vec2 barrel_distort(vec2 uv) {
    vec2 cc = uv - 0.5;
    float dist = dot(cc, cc);
    float curvature = 0.08;
    return uv + cc * dist * curvature;
}

/* ── Pseudo-Random Noise ────────────────────────────────────────────────── */

float random(vec2 st) {
    return fract(sin(dot(st, vec2(12.9898, 78.233))) * 43758.5453);
}

/* ── Main ───────────────────────────────────────────────────────────────── */

void main() {
    vec2 uv = barrel_distort(fragTexCoord);

    /* Black outside curved area */
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        finalColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    /* ── Chromatic Aberration ──────────────────────────────────────────── */
    float aberration = 0.0012;
    float r = texture(texture0, vec2(uv.x + aberration, uv.y)).r;
    float g = texture(texture0, uv).g;
    float b = texture(texture0, vec2(uv.x - aberration, uv.y)).b;
    vec3 color = vec3(r, g, b);

    /* ── Scanlines ─────────────────────────────────────────────────────── */
    float scanline = sin(uv.y * resolution.y * 3.14159265) * 0.5 + 0.5;
    scanline = mix(0.85, 1.0, scanline);  /* Subtle: 85%-100% brightness */
    color *= scanline;

    /* ── Vignette (darkened edges) ─────────────────────────────────────── */
    vec2 vig_uv = uv * (1.0 - uv);
    float vignette = vig_uv.x * vig_uv.y * 15.0;
    vignette = pow(vignette, 0.25);
    color *= vignette;

    /* ── Film Grain Noise ──────────────────────────────────────────────── */
    float noise = random(uv * time) * 0.03;
    color += vec3(noise);

    /* ── Phosphor Glow (brighten slightly) ─────────────────────────────── */
    float brightness = dot(color, vec3(0.299, 0.587, 0.114));
    color += color * brightness * 0.08;

    finalColor = vec4(color, 1.0);
}
