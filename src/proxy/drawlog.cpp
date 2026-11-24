#include "drawlog.h"

#include "../common/log.h"

#include <windows.h>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace psyvr
{
    DrawLogState g_dls = {};
    bool g_drawlog_active = false;

    void DrawLogState::clear()
    {
        memset(this, 0, sizeof(*this));
        memset(rs, 0xFF, sizeof(rs));
    }

    namespace
    {
        const unsigned kMaxDraws = 20000;
        const size_t kMaxVsc = 64;
        const size_t kMaxPsc = 32;
        const DWORD kUnknown = 0xFFFFFFFFul;

        // --- shaders -------------------------------------------------------

        struct Shader
        {
            bool pixel = false;
            bool patched = false;
            uint64_t hash = 0;
            std::vector<DWORD> code;        // the game's bytecode, through the end token
            unsigned logged = 0;            // g_serial of the frame it was last written in
        };
        std::unordered_map<const void*, Shader> g_shaders;
        std::unordered_set<uint64_t> g_dumped;      // hashes written (or found) this process

        // Words through the 0x0000FFFF end token. Comment blocks (the constant
        // table) are skipped whole: their data may hold any value.
        size_t bytecode_words(const DWORD* code)
        {
            size_t n = 1;                   // version token
            while (n < 65536)
            {
                const DWORD t = code[n++];
                if ((t & 0xFFFF) == 0xFFFE)    // comment
                    n += (t >> 16) & 0x7FFF;
                else if (t == 0x0000FFFFul)
                    break;
            }
            return n;
        }

        uint64_t fnv1a(const void* data, size_t bytes)
        {
            uint64_t h = 14695981039346656037ull;
            const unsigned char* p = static_cast<const unsigned char*>(data);
            for (size_t i = 0; i < bytes; ++i)
            {
                h ^= p[i];
                h *= 1099511628211ull;
            }
            return h;
        }

        // --- text ----------------------------------------------------------

        void add(std::string& s, const char* f, ...)
        {
            char buf[512];
            va_list args;
            va_start(args, f);
            vsnprintf(buf, sizeof(buf), f, args);
            va_end(args);
            s += buf;
        }

        void add_float(std::string& s, float v)
        {
            if (!std::isfinite(v))
                s += "null";
            else
                add(s, "%.7g", v);
        }

        void add_floats(std::string& s, const float* v, size_t n)
        {
            s += '[';
            for (size_t i = 0; i < n; ++i)
            {
                if (i)
                    s += ',';
                add_float(s, v[i]);
            }
            s += ']';
        }

        void add_hex(std::string& s, const void* p)
        {
            add(s, "\"%08x\"", (unsigned)(uintptr_t)p);
        }

        void add_name(std::string& s, const char* name, size_t max)
        {
            s += '"';
            for (size_t i = 0; i < max && name[i]; ++i)
            {
                const char c = name[i];
                s += (c >= 0x20 && c < 0x7F && c != '"' && c != '\\') ? c : '?';
            }
            s += '"';
        }

        // --- constant table --------------------------------------------------

        // D3DXSHADER_CONSTANTTABLE: Size, Creator, Version, Constants,
        // ConstantInfo, Flags, Target (7 DWORDs); D3DXSHADER_CONSTANTINFO is
        // Name(DWORD offset), RegisterSet(WORD), RegisterIndex(WORD),
        // RegisterCount(WORD), Reserved(WORD), TypeInfo, DefaultValue = 20
        // bytes. Offsets count from the byte after the 'CTAB' fourcc.
        // Appends the JSON array, or `null`.
        void add_ctab(std::string& s, const std::vector<DWORD>& code)
        {
            size_t n = 1;
            while (n < code.size())
            {
                const DWORD t = code[n++];
                if ((t & 0xFFFF) != 0xFFFE)
                {
                    if (t == 0x0000FFFFul)
                        break;
                    continue;
                }
                const size_t len = (t >> 16) & 0x7FFF;
                if (n + len > code.size())
                    break;
                if (len >= 8 && code[n] == 0x42415443ul)    // 'CTAB'
                {
                    const unsigned char* base = reinterpret_cast<const unsigned char*>(&code[n + 1]);
                    const size_t size = (len - 1) * 4;
                    DWORD count = 0, info = 0;
                    memcpy(&count, base + 12, 4);
                    memcpy(&info, base + 16, 4);
                    if (size < 28 || count > 512 || (size_t)info + (size_t)count * 20 > size)
                        break;
                    s += '[';
                    for (DWORD i = 0; i < count; ++i)
                    {
                        DWORD name_off = 0;
                        WORD set = 0, index = 0, cnt = 0;
                        const unsigned char* e = base + info + i * 20;
                        memcpy(&name_off, e, 4);
                        memcpy(&set, e + 4, 2);
                        memcpy(&index, e + 6, 2);
                        memcpy(&cnt, e + 8, 2);
                        if (i)
                            s += ',';
                        s += "{\"name\":";
                        if (name_off < size)
                            add_name(s, reinterpret_cast<const char*>(base + name_off), size - name_off);
                        else
                            s += "\"?\"";
                        add(s, ",\"set\":\"%c\",\"reg\":%u,\"count\":%u}",
                            set == 0 ? 'b' : set == 1 ? 'i' : set == 2 ? 'c' : set == 3 ? 's' : '?',
                            (unsigned)index, (unsigned)cnt);
                    }
                    s += ']';
                    return;
                }
                n += len;
            }
            s += "null";
        }

        // <kind>_<hash>.bin under psyvr_shots\shaders, once per process.
        void dump_shader(const Shader& sh)
        {
            if (!g_dumped.insert(sh.hash).second)
                return;
            wchar_t dir[MAX_PATH]{};
            swprintf_s(dir, L"%spsyvr_shots", exe_dir());
            CreateDirectoryW(dir, nullptr);
            swprintf_s(dir, L"%spsyvr_shots\\shaders", exe_dir());
            CreateDirectoryW(dir, nullptr);
            wchar_t path[MAX_PATH]{};
            swprintf_s(path, L"%s\\%cs_%016llx.bin", dir, sh.pixel ? L'p' : L'v',
                       (unsigned long long)sh.hash);
            HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
            if (f == INVALID_HANDLE_VALUE)
                return;                     // already there
            DWORD written = 0;
            WriteFile(f, sh.code.data(), (DWORD)(sh.code.size() * 4), &written, nullptr);
            CloseHandle(f);
        }

        // --- the running log -------------------------------------------------

        struct Surf { unsigned w = 0, h = 0, fmt = 0, ms = 0; };
        struct Tex { unsigned w = 0, h = 0, fmt = 0; bool rt = false; };
        struct Decl { std::string json; bool pretransformed = false; };

        bool g_pending = false;
        std::string g_name;
        unsigned g_frame = 0;               // frames seen by drawlog_frame_boundary
        unsigned g_serial = 0;              // logged frames so far
        unsigned g_frame_logged = 0;
        std::string g_buf;                  // the records of the frame
        unsigned g_draws = 0;
        unsigned g_dropped = 0;
        UINT g_back_w = 0, g_back_h = 0;
        std::vector<std::string> g_vsc, g_psc;

        std::unordered_map<const void*, Surf> g_surfs;
        std::unordered_map<const void*, Tex> g_texs;
        std::unordered_map<const void*, Decl> g_decls;
        std::unordered_map<const void*, bool> g_dynamic;    // vertex buffer -> D3DUSAGE_DYNAMIC
        DrawLogResult g_result;
        bool g_dump_vertices = false;       // drawlog_dump_vertices: the next draw's range
        std::unordered_set<std::string> g_dumped_ranges;    // "vb:at:bytes" this frame

        // Stream 0's vertices [first, first + n) to psyvr_shots\<name>_vb_<vb>_<at>.bin
        // (raw, the draw's stride); "" if the buffer would not lock.
        std::string dump_vertex_range(IDirect3DVertexBuffer9* vb, UINT at, UINT bytes)
        {
            char key[64];
            snprintf(key, sizeof(key), "%p_%u_%u", (void*)vb, at, bytes);
            if (g_dumped_ranges.count(key))
                return key;
            void* data = nullptr;
            if (FAILED(vb->Lock(at, bytes, &data, D3DLOCK_READONLY)) || !data)
                return "";
            wchar_t path[MAX_PATH];
            swprintf_s(path, L"%spsyvr_shots\\%S_vb_%S.bin", exe_dir(), g_name.c_str(), key);
            bool ok = false;
            FILE* f = _wfopen(path, L"wb");
            if (f)
            {
                ok = fwrite(data, 1, bytes, f) == bytes;
                fclose(f);
            }
            vb->Unlock();
            if (!ok)
                return "";
            g_dumped_ranges.insert(key);
            return key;
        }

        void drop_caches()
        {
            g_surfs.clear();
            g_texs.clear();
            g_decls.clear();
            g_dynamic.clear();
        }

        const Surf& surf_info(IDirect3DSurface9* s)
        {
            auto it = g_surfs.find(s);
            if (it != g_surfs.end())
                return it->second;
            Surf out;
            D3DSURFACE_DESC d{};
            if (SUCCEEDED(s->GetDesc(&d)))
            {
                out.w = d.Width;
                out.h = d.Height;
                out.fmt = (unsigned)d.Format;
                out.ms = (unsigned)d.MultiSampleType;
            }
            return g_surfs[s] = out;
        }

        const Tex& tex_info(IDirect3DBaseTexture9* t)
        {
            auto it = g_texs.find(t);
            if (it != g_texs.end())
                return it->second;
            Tex out;
            D3DSURFACE_DESC d{};
            bool ok = false;
            if (t->GetType() == D3DRTYPE_TEXTURE)
                ok = SUCCEEDED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &d));
            else if (t->GetType() == D3DRTYPE_CUBETEXTURE)
                ok = SUCCEEDED(static_cast<IDirect3DCubeTexture9*>(t)->GetLevelDesc(0, &d));
            if (ok)
            {
                out.w = d.Width;
                out.h = d.Height;
                out.fmt = (unsigned)d.Format;
                out.rt = (d.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) != 0;
            }
            return g_texs[t] = out;
        }

        const Decl& decl_info(IDirect3DVertexDeclaration9* d)
        {
            auto it = g_decls.find(d);
            if (it != g_decls.end())
                return it->second;
            Decl out;
            D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
            UINT n = 0;
            out.json = "null";
            if (SUCCEEDED(d->GetDeclaration(el, &n)))
            {
                out.json = "[";
                bool first = true;
                for (UINT i = 0; i < n && el[i].Stream != 0xFF; ++i)
                {
                    if (!first)
                        out.json += ',';
                    first = false;
                    add(out.json, "{\"stream\":%u,\"offset\":%u,\"type\":%u,\"usage\":%u,\"index\":%u}",
                        (unsigned)el[i].Stream, (unsigned)el[i].Offset, (unsigned)el[i].Type,
                        (unsigned)el[i].Usage, (unsigned)el[i].UsageIndex);
                    if (el[i].Usage == D3DDECLUSAGE_POSITIONT)
                        out.pretransformed = true;
                }
                out.json += ']';
            }
            return g_decls[d] = out;
        }

        void add_surf(std::string& s, const void* p, const Surf& i)
        {
            s += "{\"ptr\":";
            add_hex(s, p);
            add(s, ",\"w\":%u,\"h\":%u,\"fmt\":%u,\"ms\":%u}", i.w, i.h, i.fmt, i.ms);
        }

        void emit_shader(const Shader& sh, const void* ptr)
        {
            g_buf += "{\"t\":\"shader\",\"kind\":\"";
            g_buf += sh.pixel ? "ps" : "vs";
            add(g_buf, "\",\"hash\":\"%016llx\",\"ptr\":", (unsigned long long)sh.hash);
            add_hex(g_buf, ptr);
            if (sh.patched)
                g_buf += ",\"patched\":true";
            g_buf += ",\"ctab\":";
            add_ctab(g_buf, sh.code);
            g_buf += "}\n";
            dump_shader(sh);
        }

        // The shader's hash as JSON ("<16 hex>" or null), writing its record
        // on first use in this frame.
        void add_shader_ref(std::string& s, const void* ptr)
        {
            auto it = ptr ? g_shaders.find(ptr) : g_shaders.end();
            if (it == g_shaders.end())
            {
                // null = none bound (fixed function); a bound shader created
                // before the hooks saw it is named by pointer instead.
                if (ptr)
                    add(s, "\"unknown:%p\"", ptr);
                else
                    s += "null";
                return;
            }
            Shader& sh = it->second;
            if (sh.logged != g_serial)
            {
                sh.logged = g_serial;
                emit_shader(sh, ptr);
            }
            add(s, "\"%016llx\"", (unsigned long long)sh.hash);
        }

        void flush_file()
        {
            g_result = DrawLogResult();
            g_result.draws = g_draws;
            std::string head;
            add(head, "{\"t\":\"frame\",\"frame\":%u,\"back\":[%u,%u],\"draws\":%u", g_frame_logged,
                g_back_w, g_back_h, g_draws);
            if (g_dropped)
                add(head, ",\"dropped\":%u", g_dropped);
            head += "}\n";

            wchar_t dir[MAX_PATH]{};
            swprintf_s(dir, L"%spsyvr_shots", exe_dir());
            CreateDirectoryW(dir, nullptr);
            wchar_t path[MAX_PATH]{};
            wchar_t wname[64]{};
            for (size_t i = 0; i < g_name.size() && i < 63; ++i)
                wname[i] = (wchar_t)(unsigned char)g_name[i];
            swprintf_s(path, L"%s\\%s.draws.jsonl", dir, wname);
            HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE)
            {
                DWORD w = 0;
                bool ok = WriteFile(f, head.data(), (DWORD)head.size(), &w, nullptr) && w == head.size();
                ok = ok && WriteFile(f, g_buf.data(), (DWORD)g_buf.size(), &w, nullptr) && w == g_buf.size();
                CloseHandle(f);
                g_result.ok = ok;
            }
            snprintf(g_result.path, sizeof(g_result.path), "%S", path);
            log("drawlog %s: %u draws%s -> %s", g_name.c_str(), g_draws,
                g_result.ok ? "" : " (WRITE FAILED)", g_result.path);
        }

        unsigned vertices_of(UINT prim, UINT n)
        {
            switch (prim)
            {
            case D3DPT_POINTLIST: return n;
            case D3DPT_LINELIST: return n * 2;
            case D3DPT_LINESTRIP: return n + 1;
            case D3DPT_TRIANGLELIST: return n * 3;
            default: return n + 2;      // strip, fan
            }
        }

        void note_consts(std::vector<std::string>& list, size_t cap, UINT reg, const float* data,
                         UINT count, void* caller)
        {
            if (!data || list.size() >= cap)
                return;
            std::string e;
            add(e, "{\"reg\":%u,\"count\":%u,\"caller\":", reg, count);
            add_hex(e, caller);
            e += ",\"v0\":";
            add_floats(e, data, count ? 4 : 0);
            // Matrices (c6..c9 WVP and the like): every register.
            if (count > 1 && count <= 4)
            {
                e += ",\"v\":";
                add_floats(e, data, count * 4);
            }
            e += '}';
            list.push_back(std::move(e));
        }

        void add_const_list(std::string& s, const char* key, std::vector<std::string>& list)
        {
            add(s, ",\"%s\":[", key);
            for (size_t i = 0; i < list.size(); ++i)
            {
                if (i)
                    s += ',';
                s += list[i];
            }
            s += ']';
            list.clear();
        }
    }

    void drawlog_shader_created(bool pixel, const void* shader, const DWORD* bytecode, bool patched)
    {
        if (!shader || !bytecode)
            return;
        const size_t words = bytecode_words(bytecode);
        Shader sh;
        sh.pixel = pixel;
        sh.patched = patched;
        sh.code.assign(bytecode, bytecode + words);
        sh.hash = fnv1a(bytecode, words * 4);
        g_shaders[shader] = std::move(sh);
    }

    bool drawlog_request(const char* name)
    {
        if (drawlog_busy())
            return false;
        g_name = name;
        g_pending = true;
        return true;
    }

    bool drawlog_busy()
    {
        return g_pending || g_drawlog_active;
    }

    void drawlog_frame_boundary()
    {
        ++g_frame;
        if (g_drawlog_active)
        {
            g_drawlog_active = false;
            flush_file();
            g_buf.clear();
            g_buf.shrink_to_fit();
            drop_caches();
        }
        if (g_pending)
        {
            g_pending = false;
            g_drawlog_active = true;
            ++g_serial;
            g_frame_logged = g_frame;
            g_buf.clear();
            g_draws = 0;
            g_dropped = 0;
            g_dumped_ranges.clear();
            g_dump_vertices = false;
            g_vsc.clear();
            g_psc.clear();
            drop_caches();
        }
    }

    void drawlog_device_reset()
    {
        g_dls.clear();
        drop_caches();
    }

    const DrawLogResult& drawlog_last_result()
    {
        return g_result;
    }

    void drawlog_note_vsc(UINT reg, const float* data, UINT count, void* caller)
    {
        note_consts(g_vsc, kMaxVsc, reg, data, count, caller);
    }

    void drawlog_note_psc(UINT reg, const float* data, UINT count, void* caller)
    {
        note_consts(g_psc, kMaxPsc, reg, data, count, caller);
    }

    void drawlog_raw(const char* text, size_t len)
    {
        if (g_drawlog_active)
            g_buf.append(text, len);
    }

    unsigned drawlog_serial()
    {
        return g_serial;
    }

    void drawlog_dump_vertices(bool on)
    {
        g_dump_vertices = on;
    }

    void drawlog_draw(IDirect3DDevice9* dev, const DrawCall& c, void* caller, const char* kind)
    {
        if (g_draws >= kMaxDraws)
        {
            ++g_dropped;
            g_vsc.clear();
            g_psc.clear();
            return;
        }
        DrawLogState& s = g_dls;
        if (g_draws == 0)
        {
            IDirect3DSurface9* back = nullptr;
            if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back)
            {
                const Surf& b = surf_info(back);
                g_back_w = b.w;
                g_back_h = b.h;
                back->Release();
            }
        }

        // State the setters have not seen (start-up, after a Reset, a render
        // target change): ask the device, once.
        static const D3DRENDERSTATETYPE kStates[] = {D3DRS_ZENABLE, D3DRS_ZWRITEENABLE,
            D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_CULLMODE,
            D3DRS_SCISSORTESTENABLE};
        for (D3DRENDERSTATETYPE st : kStates)
            if (s.rs[st] == kUnknown)
            {
                DWORD v = 0;
                if (SUCCEEDED(dev->GetRenderState(st, &v)))
                    s.rs[st] = v;
            }
        if (!s.viewport_valid && SUCCEEDED(dev->GetViewport(&s.viewport)))
            s.viewport_valid = true;
        if (!s.scissor_valid && SUCCEEDED(dev->GetScissorRect(&s.scissor)))
            s.scissor_valid = true;

        std::string r;
        r.reserve(1024);
        const unsigned verts = c.num_vertices ? c.num_vertices : vertices_of(c.prim, c.prim_count);
        const bool up = c.up != nullptr;
        add(r, "{\"t\":\"draw\",\"i\":%u,\"call\":\"%s\",\"prim\":%u,\"count\":%u,\"verts\":%u,\"caller\":",
            g_draws, c.call, c.prim, c.prim_count, verts);
        add_hex(r, caller);

        // The frames after the draw method's return address, if the walker finds it.
        void* frames[16] = {};
        const USHORT got = RtlCaptureStackBackTrace(0, 16, frames, nullptr);
        USHORT from = 0;
        for (USHORT i = 0; i < got; ++i)
            if (frames[i] == caller)
            {
                from = (USHORT)(i + 1);
                break;
            }
        r += ",\"stack\":[";
        for (USHORT i = from, n = 0; i < got && n < 6; ++i, ++n)
        {
            if (n)
                r += ',';
            add_hex(r, frames[i]);
        }
        r += "],\"vs\":";
        add_shader_ref(r, s.vs);
        r += ",\"ps\":";
        add_shader_ref(r, s.ps);

        bool pretransformed = false;
        if (s.use_fvf)
        {
            add(r, ",\"fvf\":\"%x\"", (unsigned)s.fvf);
            pretransformed = (s.fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
        }
        else if (s.decl)
        {
            const Decl& d = decl_info(s.decl);
            r += ",\"decl\":";
            r += d.json;
            pretransformed = d.pretransformed;
        }
        else
            r += ",\"decl\":null";
        add(r, ",\"pretransformed\":%s", pretransformed ? "true" : "false");

        float vb_first[16];
        UINT vb_first_n = 0;
        if (!up)
        {
            r += ",\"stream0\":{";
            if (s.vb0)
            {
                auto it = g_dynamic.find(s.vb0);
                if (it == g_dynamic.end())
                {
                    D3DVERTEXBUFFER_DESC d{};
                    const bool dyn = SUCCEEDED(s.vb0->GetDesc(&d)) && (d.Usage & D3DUSAGE_DYNAMIC);
                    it = g_dynamic.emplace(s.vb0, dyn).first;
                }
                add(r, "\"stride\":%u,\"offset\":%u,\"vb\":", s.vb0_stride, s.vb0_offset);
                add_hex(r, s.vb0);
                add(r, ",\"dynamic\":%s", it->second ? "true" : "false");
                // A dynamic buffer is filled by the CPU each frame (sprites):
                // its first vertex, read back as the old sprite probe did.
                if (it->second && s.vb0_stride >= 4)
                {
                    const UINT bytes = s.vb0_stride > 64 ? 64 : s.vb0_stride;
                    void* data = nullptr;
                    const UINT at = s.vb0_offset + c.up_first_vertex * s.vb0_stride;
                    if (SUCCEEDED(s.vb0->Lock(at, bytes, &data, D3DLOCK_READONLY)) && data)
                    {
                        memcpy(vb_first, data, bytes);
                        s.vb0->Unlock();
                        vb_first_n = bytes / 4;
                    }
                }
            }
            else
                r += "\"stride\":0,\"offset\":0,\"vb\":null,\"dynamic\":false";
            r += '}';
            if (g_dump_vertices && s.vb0 && s.vb0_stride)
            {
                UINT n = c.num_vertices;
                const std::string key = n ? dump_vertex_range(s.vb0, s.vb0_offset + c.up_first_vertex * s.vb0_stride,
                                                              n * s.vb0_stride) : std::string();
                add(r, ",\"vdump\":{\"first\":%u,\"n\":%u,\"file\":\"%s\"}", c.up_first_vertex, n,
                    key.c_str());
                g_dump_vertices = false;        // the skin probe re-arms it before each skinned draw
            }
            if (vb_first_n)
            {
                r += ",\"vb_first\":";
                add_floats(r, vb_first, vb_first_n);
            }
        }
        else if (c.up_stride >= 4)
        {
            const float* v = reinterpret_cast<const float*>(static_cast<const unsigned char*>(c.up) +
                                                            (size_t)c.up_first_vertex * c.up_stride);
            UINT n = c.up_stride / 4;
            r += ",\"up_first\":";
            add_floats(r, v, n > 16 ? 16 : n);
        }

        add_const_list(r, "vsc", g_vsc);
        add_const_list(r, "psc", g_psc);

        add(r, ",\"scissor\":{\"on\":%s,\"rect\":[%ld,%ld,%ld,%ld],\"caller\":",
            s.rs[D3DRS_SCISSORTESTENABLE] ? "true" : "false", s.scissor.left, s.scissor.top,
            s.scissor.right, s.scissor.bottom);
        if (s.scissor_caller)
            add_hex(r, s.scissor_caller);
        else
            r += "null";
        add(r, "},\"viewport\":[%lu,%lu,%lu,%lu,", s.viewport.X, s.viewport.Y, s.viewport.Width,
            s.viewport.Height);
        add_float(r, s.viewport.MinZ);
        r += ',';
        add_float(r, s.viewport.MaxZ);
        r += ']';

        // Targets: the tracked ones, else the device's own (the back buffer
        // and its depth buffer before the game sets any).
        r += ",\"rt0\":";
        {
            IDirect3DSurface9* t = s.rt0;
            IDirect3DSurface9* held = nullptr;
            if (!t && SUCCEEDED(dev->GetRenderTarget(0, &held)))
                t = held;
            if (t)
                add_surf(r, t, surf_info(t));
            else
                r += "null";
            if (held)
                held->Release();
        }
        r += ",\"ds\":";
        {
            IDirect3DSurface9* t = s.ds;
            IDirect3DSurface9* held = nullptr;
            if (!t && SUCCEEDED(dev->GetDepthStencilSurface(&held)))
                t = held;
            if (t)
            {
                const Surf& i = surf_info(t);
                r += "{\"ptr\":";
                add_hex(r, t);
                add(r, ",\"w\":%u,\"h\":%u}", i.w, i.h);
            }
            else
                r += "null";
            if (held)
                held->Release();
        }

        r += ",\"tex\":[";
        bool first = true;
        for (int i = 0; i < 16; ++i)
            if (s.tex[i])
            {
                const Tex& t = tex_info(s.tex[i]);
                if (!first)
                    r += ',';
                first = false;
                add(r, "{\"stage\":%d,\"ptr\":", i);
                add_hex(r, s.tex[i]);
                add(r, ",\"w\":%u,\"h\":%u,\"fmt\":%u,\"rt\":%s}", t.w, t.h, t.fmt,
                    t.rt ? "true" : "false");
            }
        r += ']';

        add(r, ",\"zenable\":%lu,\"zwrite\":%lu,\"blend\":%lu,\"srcblend\":%lu,\"destblend\":%lu,\"cull\":%lu",
            s.rs[D3DRS_ZENABLE], s.rs[D3DRS_ZWRITEENABLE], s.rs[D3DRS_ALPHABLENDENABLE],
            s.rs[D3DRS_SRCBLEND], s.rs[D3DRS_DESTBLEND], s.rs[D3DRS_CULLMODE]);
        r += ",\"kind\":";
        add_name(r, kind ? kind : "", 40);
        r += "}\n";

        g_buf += r;
        ++g_draws;
    }
}
