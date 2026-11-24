#pragma once
// What a Psychonauts vertex shader (hand-written vs_1_1 / vs_2_0 assembly,
// no constant table) reads, from its bytecode (docs/shaders.md).
//
//  * reads_wvp: reads c6 (C_PROJECTION_MATRIX, the WVP every 3D shader uses).
//  * billboard: constants added to the clip-space position after the WVP
//    transform -- particle corner offsets, already in projection space:
//        m4x4 r0, v0, c6          ; r0 = clip position
//        mad  r7, v13.x, c10, r0  ; + corner offset c10   (ParticleBB: c10, c14;
//        mad  r7, v13.y, c14, r7  ;   ZSprite: c24, c25)
//    Found by following the register the c6 transform writes up to the output
//    position (oPos), so constants used for anything else are not: ZSprite's
//    c10..c13 view-projection, CheapWater's c15 (reflection coordinates made
//    from the clip position).
//  * reads_c10: reads c10 at all (Sprite.vsh: screen position = v0 * c10 + c11).

#include <cstdint>

namespace psyvr
{
    // c56..c59: the game's own WVP for a shader psy_patch_screen_coords
    // patched (below), which also writes that clip position to oT4 for the
    // replacement pixel shaders (shaders/water_ps.hlsl).
    constexpr unsigned kPsyGameWvpReg = 56;
    constexpr unsigned kPsyGameClipOut = 4;

    struct PsyShaderInfo
    {
        bool known = false;
        bool reads_wvp = false;
        bool reads_c10 = false;
        bool reads_game_wvp = false;    // c56: patched by psy_patch_screen_coords
        uint64_t billboard = 0;     // bit n = cn (n < 64)
    };

    inline PsyShaderInfo psy_scan_shader(const uint32_t* code, size_t max_tokens = 65536)
    {
        PsyShaderInfo info;
        if (!code || (code[0] & 0xFFFF0000u) != 0xFFFE0000u)
            return info;
        const unsigned major = (code[0] >> 8) & 0xFF;
        info.known = true;
        uint32_t clip = 0;      // temp registers holding the clip-space position
        uint64_t lineage[32] = {};   // per such temp: the constants added to it
        size_t i = 1;
        for (int guard = 0; guard < 65536 && i < max_tokens; ++guard)
        {
            const uint32_t t = code[i];
            if (t == 0x0000FFFFu)
                break;
            if ((t & 0xFFFFu) == 0xFFFEu)                       // comment
            {
                i += 1 + ((t >> 16) & 0x7FFFu);
                continue;
            }
            const uint32_t op = t & 0xFFFFu;
            ++i;
            if (op == 0x51 || op == 0x30) { i += 5; continue; }    // def, defi
            if (op == 0x2F) { i += 2; continue; }                  // defb

            // Parameters: type and number of each (dest first).
            struct Param { uint32_t type, num; };
            Param prm[6];
            int n = 0;
            while (i < max_tokens && (code[i] & 0x80000000u))
            {
                const uint32_t r = code[i++];
                // vs_2_0+: a relative-addressed source is followed by its
                // address token.
                const bool relative = (r & 0x2000u) != 0;
                if (n < 6)
                    prm[n++] = { ((r >> 28) & 7u) | ((r >> 8) & 0x18u), r & 0x7FFu };
                if (relative && major >= 2 && i < max_tokens && (code[i] & 0x80000000u))
                    ++i;
            }
            for (int k = 1; k < n; ++k)
                if (prm[k].type == 2)                           // D3DSPR_CONST
                {
                    if (prm[k].num == 6)
                        info.reads_wvp = true;
                    if (prm[k].num == 10)
                        info.reads_c10 = true;
                    if (prm[k].num == kPsyGameWvpReg)
                        info.reads_game_wvp = true;
                }
            const bool dest_temp = n > 0 && prm[0].type == 0 && prm[0].num < 32;
            const bool dest_opos = n > 0 && prm[0].type == 4 && prm[0].num == 0;   // D3DSPR_RASTOUT oPos
            auto clip_src = [&](int k) {
                return k < n && prm[k].type == 0 && prm[k].num < 32 && (clip & (1u << prm[k].num));
            };
            if (dest_opos)
            {
                // mov oPos, rClip / add oPos, rClip, c50 (the PC half-pixel fixup).
                for (int k = 1; k < n; ++k)
                    if (clip_src(k))
                        info.billboard |= lineage[prm[k].num];
            }
            else if (op == 0x14 && n >= 3 && prm[2].type == 2 && prm[2].num == 6 && dest_temp)  // m4x4 rX, v, c6
            {
                clip |= 1u << prm[0].num;
                lineage[prm[0].num] = 0;
            }
            else if (op == 0x04 && n >= 4 && clip_src(3) && prm[2].type == 2 && prm[2].num < 64 && dest_temp)
            {
                // mad d, a, cN, rClip
                const uint64_t l = lineage[prm[3].num] | (1ull << prm[2].num);
                clip |= 1u << prm[0].num;
                lineage[prm[0].num] = l;
            }
            else if (op == 0x01 && clip_src(1) && dest_temp)    // mov d, rClip
            {
                const uint64_t l = lineage[prm[1].num];
                clip |= 1u << prm[0].num;
                lineage[prm[0].num] = l;
            }
            else if (dest_temp)
                clip &= ~(1u << prm[0].num);                    // overwritten by anything else
        }
        return info;
    }

    // Water.vsh / CheapWater.vsh project their reflection coordinates from the
    // clip position (`mad r1, r0.wwww, c15.xzxx, r0` -> oT1), but the
    // reflection texture is rendered once, through the game camera. Per eye
    // the clip position is the eye's, so the reflection slid with the head
    // and stretched. The patch adds `m4x4 rG, v, c56` after the shader's
    // `m4x4 rC, v, c6` and makes the instructions reading both rC and c15 read
    // rG: the reflection is then looked up where the game camera sees the
    // water. The caller puts the game's own WVP in c56..c59 (unused by the
    // game's shaders: they use c0..c52 and c64..c95).
    inline bool psy_patch_screen_coords(const uint32_t* in, uint32_t* out, size_t capacity, size_t* out_len,
                                        size_t max_tokens = 65536)
    {
        if (!in || !out || (in[0] & 0xFFFF0000u) != 0xFFFE0000u)
            return false;
        const unsigned major = (in[0] >> 8) & 0xFF;
        auto num = [](uint32_t r) { return r & 0x7FFu; };
        auto type = [](uint32_t r) { return ((r >> 28) & 7u) | ((r >> 8) & 0x18u); };

        // Pass 1: the clip transform, the instructions to rewrite, what is used.
        size_t clip_at = 0, clip_len = 0;
        uint32_t clip_reg = 0, temps_used = 0;
        bool clip_live = false;
        size_t rewrite[8], rewrite_len[8];
        unsigned rewrites = 0;
        size_t end = 0;
        size_t i = 1;
        for (int guard = 0; guard < 65536 && i < max_tokens; ++guard)
        {
            const uint32_t t = in[i];
            if (t == 0x0000FFFFu)
            {
                end = i;
                break;
            }
            if ((t & 0xFFFFu) == 0xFFFEu)
            {
                i += 1 + ((t >> 16) & 0x7FFFu);
                continue;
            }
            const size_t start = i;
            const uint32_t op = t & 0xFFFFu;
            ++i;
            if (op == 0x51 || op == 0x30) { i += 5; continue; }
            if (op == 0x2F) { i += 2; continue; }
            uint32_t prm[6];
            int n = 0;
            while (i < max_tokens && (in[i] & 0x80000000u))
            {
                const uint32_t r = in[i++];
                if (r & 0x2000u)
                    return false;       // relative addressing: could read any constant
                if (n < 6)
                    prm[n++] = r;
            }
            bool reads_clip = false, reads_c15 = false;
            for (int k = 0; k < n; ++k)
            {
                if (type(prm[k]) == 0 && num(prm[k]) < 32)
                    temps_used |= 1u << num(prm[k]);
                if (k == 0)
                    continue;
                if (type(prm[k]) == 2 && num(prm[k]) >= kPsyGameWvpReg && num(prm[k]) < kPsyGameWvpReg + 4)
                    return false;       // the registers are taken
                if (type(prm[k]) == 2 && num(prm[k]) == 15)
                    reads_c15 = true;
                if (clip_live && type(prm[k]) == 0 && num(prm[k]) == clip_reg)
                    reads_clip = true;
            }
            const bool dest_temp = n > 0 && type(prm[0]) == 0;
            if (n > 0 && type(prm[0]) == 6 && num(prm[0]) == kPsyGameClipOut)
                return false;           // oT4 is taken
            if (!clip_at && op == 0x14 && n == 3 && dest_temp && type(prm[2]) == 2 && num(prm[2]) == 6)
            {
                clip_at = start;
                clip_len = i - start;
                clip_reg = num(prm[0]);
                clip_live = true;
                continue;
            }
            if (reads_clip && reads_c15 && dest_temp)
            {
                if (rewrites == 8)
                    return false;
                rewrite[rewrites] = start;
                rewrite_len[rewrites++] = i - start;
            }
            if (clip_live && dest_temp && num(prm[0]) == clip_reg)
                clip_live = false;      // overwritten: later reads are something else
        }
        if (!end || !clip_at || clip_len != 4 || !rewrites)
            return false;
        // A temp the shader never touches (vs_1_1 and vs_2_0 have r0..r11).
        uint32_t g = 0;
        while (g < 12 && (temps_used & (1u << g)))
            ++g;
        if (g == 12)
            return false;
        // Pass 2: copy, with the extra transform and the rewritten sources
        // (source tokens only: an instruction's token 0 is the opcode, 1 the
        // destination).
        if (end + 1 + clip_len + 3 > capacity)
            return false;
        size_t o = 0;
        for (size_t k = 0; k <= end; ++k)
        {
            uint32_t tok = in[k];
            for (unsigned r = 0; r < rewrites; ++r)
                if (k >= rewrite[r] + 2 && k < rewrite[r] + rewrite_len[r] && type(tok) == 0 && num(tok) == clip_reg)
                    tok = (tok & ~0x7FFu) | g;
            out[o++] = tok;
            if (k == clip_at + clip_len - 1)
            {
                out[o++] = in[clip_at];
                out[o++] = (in[clip_at + 1] & ~0x7FFu) | g;
                out[o++] = in[clip_at + 2];
                out[o++] = (in[clip_at + 3] & ~0x7FFu) | kPsyGameWvpReg;
                // mov oT4, rG (vs_2_0 instruction tokens carry their length).
                out[o++] = 0x01u | (major >= 2 ? 2u << 24 : 0u);
                out[o++] = 0x80000000u | (6u << 28) | 0x000F0000u | kPsyGameClipOut;
                out[o++] = 0x80000000u | 0x00E40000u | g;
            }
        }
        *out_len = o;
        return true;
    }
}
