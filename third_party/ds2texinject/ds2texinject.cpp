// DS2TexInject - injetor de texturas estilo TexMod/uMod para Dead Space 2
// rodando no Linux (Proton/Wine + DXVK + ReShade).
//
// Carregado como plugin .asi pelo MarkerPatch (dinput8.dll). Intercepta
// Direct3DCreate9 na tabela de importacao do jogo e faz hook por vtable em
// IDirect3D9/IDirect3DDevice9/IDirect3DTexture9. Cada textura carregada pelo
// jogo recebe o mesmo hash CRC32 que o TexMod/uMod usam; se existir
// texmod\_cache\0xHASH.(dds|png|bmp|...), a textura e trocada no SetTexture.

#define CINTERFACE
#define COBJMACROS
#define INITGUID
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define DS2TI_VERSION "1.3.0"

// ---------------------------------------------------------------- config/log

static wchar_t g_gameDir[MAX_PATH];
static std::wstring g_texDir, g_dumpDir;
static bool g_enabled = true, g_logHashes = false, g_dump = false;
static D3DPOOL g_pool = D3DPOOL_MANAGED;
static int g_toggleKey = VK_F10;
static int g_loadBudget = 4; // ms de carregamento a cada 10 ms (0 = sem limite)
static FILE *g_log;
static CRITICAL_SECTION g_logCs;

static void Log(const char *fmt, ...)
{
    if (!g_log) return;
    EnterCriticalSection(&g_logCs);
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logCs);
}

static void LoadConfig()
{
    std::wstring ini = std::wstring(g_gameDir) + L"\\DS2TexInject.ini";
    wchar_t buf[MAX_PATH];
    g_enabled = GetPrivateProfileIntW(L"DS2TexInject", L"Enabled", 1, ini.c_str()) != 0;
    g_logHashes = GetPrivateProfileIntW(L"DS2TexInject", L"LogHashes", 0, ini.c_str()) != 0;
    g_dump = GetPrivateProfileIntW(L"DS2TexInject", L"DumpTextures", 0, ini.c_str()) != 0;
    GetPrivateProfileStringW(L"DS2TexInject", L"ToggleKey", L"0x79", buf, MAX_PATH, ini.c_str());
    g_toggleKey = (int)wcstol(buf, nullptr, 0);
    g_loadBudget = GetPrivateProfileIntW(L"DS2TexInject", L"LoadBudget", 4, ini.c_str());
    if (g_loadBudget < 0) g_loadBudget = 0;
    GetPrivateProfileStringW(L"DS2TexInject", L"Pool", L"managed", buf, MAX_PATH, ini.c_str());
    g_pool = (_wcsicmp(buf, L"default") == 0) ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED;
    GetPrivateProfileStringW(L"DS2TexInject", L"TextureDir", L"texmod\\_cache", buf, MAX_PATH, ini.c_str());
    g_texDir = (buf[0] && buf[1] == L':') ? buf : std::wstring(g_gameDir) + L"\\" + buf;
    GetPrivateProfileStringW(L"DS2TexInject", L"DumpDir", L"texmod\\_dump", buf, MAX_PATH, ini.c_str());
    g_dumpDir = (buf[0] && buf[1] == L':') ? buf : std::wstring(g_gameDir) + L"\\" + buf;
}

// ---------------------------------------------------------------- CRC32 (TexMod)

// TexMod/uMod: CRC32 refletido (0xEDB88320), valor inicial 0xFFFFFFFF, SEM xor final.
static uint32_t g_crcTab[8][256];
static void CrcInit()
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ ((c & 1) ? 0xEDB88320u : 0);
        g_crcTab[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; i++)
        for (int t = 1; t < 8; t++)
            g_crcTab[t][i] = (g_crcTab[t - 1][i] >> 8) ^ g_crcTab[0][g_crcTab[t - 1][i] & 0xFF];
}
static uint32_t TexModCrc(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n && ((uintptr_t)p & 3)) { c = (c >> 8) ^ g_crcTab[0][(c ^ *p++) & 0xFF]; n--; }
    while (n >= 8) {
        uint32_t a = *(const uint32_t *)p ^ c, b = *(const uint32_t *)(p + 4);
        c = g_crcTab[7][a & 0xFF] ^ g_crcTab[6][(a >> 8) & 0xFF] ^ g_crcTab[5][(a >> 16) & 0xFF] ^ g_crcTab[4][a >> 24] ^
            g_crcTab[3][b & 0xFF] ^ g_crcTab[2][(b >> 8) & 0xFF] ^ g_crcTab[1][(b >> 16) & 0xFF] ^ g_crcTab[0][b >> 24];
        p += 8; n -= 8;
    }
    while (n--) c = (c >> 8) ^ g_crcTab[0][(c ^ *p++) & 0xFF];
    return c;
}

static UINT BitsPerPixel(D3DFORMAT f)
{
    switch ((DWORD)f) {
    case D3DFMT_A32B32G32R32F: return 128;
    case D3DFMT_A16B16G16R16: case D3DFMT_A16B16G16R16F: case D3DFMT_Q16W16V16U16: case D3DFMT_G32R32F: return 64;
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8:
    case D3DFMT_A2R10G10B10: case D3DFMT_A2B10G10R10: case D3DFMT_G16R16: case D3DFMT_G16R16F:
    case D3DFMT_R32F: case D3DFMT_Q8W8V8U8: case D3DFMT_V16U16: case D3DFMT_X8L8V8U8: return 32;
    case D3DFMT_R8G8B8: return 24;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4:
    case D3DFMT_A8L8: case D3DFMT_V8U8: case D3DFMT_L16: case D3DFMT_R16F: case D3DFMT_A8R3G3B2: case D3DFMT_L6V5U5:
    case D3DFMT_A8P8: return 16;
    case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_P8: case D3DFMT_A4L4: case D3DFMT_R3G3B2: return 8;
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
    case MAKEFOURCC('A', 'T', 'I', '2'): return 8;
    case D3DFMT_DXT1: case MAKEFOURCC('A', 'T', 'I', '1'): return 4;
    default: return 0;
    }
}
static bool IsBlockFormat(D3DFORMAT f)
{
    return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5 ||
           f == (D3DFORMAT)MAKEFOURCC('A', 'T', 'I', '1') || f == (D3DFORMAT)MAKEFOURCC('A', 'T', 'I', '2');
}

// ---------------------------------------------------------------- vtable hooks

template <class T> static void HookVtbl(void *obj, size_t offset, T hook, T *orig)
{
    void **vtbl = *(void ***)obj;
    void **slot = (void **)((char *)vtbl + offset);
    if (*slot == (void *)hook) return;  // ja instalado
    DWORD old;
    VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    *orig = (T)*slot;
    *slot = (void *)hook;
    VirtualProtect(slot, sizeof(void *), old, &old);
}
#define VT(iface, method) offsetof(iface##Vtbl, method)

typedef IDirect3D9 *(WINAPI *Direct3DCreate9_t)(UINT);
typedef HRESULT(STDMETHODCALLTYPE *CreateDevice_t)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
typedef HRESULT(STDMETHODCALLTYPE *CreateTexture_t)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9 **, HANDLE *);
typedef HRESULT(STDMETHODCALLTYPE *SetTexture_t)(IDirect3DDevice9 *, DWORD, IDirect3DBaseTexture9 *);
typedef HRESULT(STDMETHODCALLTYPE *UpdateTexture_t)(IDirect3DDevice9 *, IDirect3DBaseTexture9 *, IDirect3DBaseTexture9 *);
typedef HRESULT(STDMETHODCALLTYPE *UpdateSurface_t)(IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const POINT *);
typedef HRESULT(STDMETHODCALLTYPE *Reset_t)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
typedef HRESULT(STDMETHODCALLTYPE *EndScene_t)(IDirect3DDevice9 *);
typedef HRESULT(STDMETHODCALLTYPE *TexLockRect_t)(IDirect3DTexture9 *, UINT, D3DLOCKED_RECT *, const RECT *, DWORD);
typedef HRESULT(STDMETHODCALLTYPE *TexUnlockRect_t)(IDirect3DTexture9 *, UINT);
typedef ULONG(STDMETHODCALLTYPE *Release_t)(IDirect3DTexture9 *);
typedef HRESULT(STDMETHODCALLTYPE *SurfUnlockRect_t)(IDirect3DSurface9 *);

static Direct3DCreate9_t o_Direct3DCreate9;
static CreateDevice_t o_CreateDevice;
static CreateTexture_t o_CreateTexture;
static SetTexture_t o_SetTexture;
static UpdateTexture_t o_UpdateTexture;
static UpdateSurface_t o_UpdateSurface;
static Reset_t o_Reset;
static EndScene_t o_EndScene;
static TexLockRect_t o_TexLockRect;
static TexUnlockRect_t o_TexUnlockRect;
static Release_t o_TexRelease;
static SurfUnlockRect_t o_SurfUnlockRect;

// ---------------------------------------------------------------- estado

struct Replacement {
    IDirect3DTexture9 *tex = nullptr;
    int users = 0;
    bool failed = false;
};

struct TexInfo {
    UINT w, h, levels;
    D3DFORMAT fmt;
    D3DPOOL pool;
    DWORD usage;
    uint32_t hash = 0;
    bool hashed = false;
    bool dirty = true;        // conteudo mudou desde o ultimo hash
    void *lockBits = nullptr; // LockRect(0) completo pendente
    INT lockPitch = 0;
    bool hasRep = false;      // existe arquivo para esse hash
    int rehashes = 0;         // texturas que mudam sempre (video/UI) param de ser hasheadas
};

static const int kMaxRehash = 8;

static SRWLOCK g_lock = SRWLOCK_INIT;
static std::unordered_map<IDirect3DTexture9 *, TexInfo> g_tex;
static std::unordered_map<uint32_t, Replacement> g_rep;
static std::unordered_map<uint32_t, std::wstring> g_files; // hash -> caminho
static std::unordered_set<uint32_t> g_dumped;
static IDirect3DDevice9 *g_device;
static thread_local int t_internal; // >0 enquanto nos mesmos criamos/lemos texturas
static volatile LONG g_statHashed, g_statMatched, g_statLoaded;

struct WLock { WLock() { AcquireSRWLockExclusive(&g_lock); } ~WLock() { ReleaseSRWLockExclusive(&g_lock); } };

static void ScanTextureDir()
{
    g_files.clear();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_texDir + L"\\0x*.*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { Log("AVISO: nenhuma textura em %ls (rode ds2tex.py)", g_texDir.c_str()); return; }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        wchar_t *end;
        unsigned long v = wcstoul(fd.cFileName + 2, &end, 16);
        if (end != fd.cFileName + 10 || *end != L'.') continue;
        g_files.emplace((uint32_t)v, g_texDir + L"\\" + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    Log("%u texturas de substituicao encontradas em %ls", (unsigned)g_files.size(), g_texDir.c_str());
}

// ---------------------------------------------------------------- carregar substituicao

typedef HRESULT(WINAPI *D3DXCreateTextureFromFileExW_t)(IDirect3DDevice9 *, LPCWSTR, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, DWORD, DWORD, D3DCOLOR, void *, PALETTEENTRY *, IDirect3DTexture9 **);
static D3DXCreateTextureFromFileExW_t p_D3DXCreateTextureFromFileExW;

static std::vector<uint8_t> ReadFileAll(const std::wstring &path)
{
    std::vector<uint8_t> v;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return v;
    DWORD sz = GetFileSize(f, nullptr), rd = 0;
    if (sz != INVALID_FILE_SIZE) {
        v.resize(sz);
        if (!ReadFile(f, v.data(), sz, &rd, nullptr) || rd != sz) v.clear();
    }
    CloseHandle(f);
    return v;
}

// Upload de niveis para uma textura bloqueavel
static bool FillLevels(IDirect3DTexture9 *t, D3DFORMAT fmt, UINT w, UINT h, UINT mips, const uint8_t *src, size_t avail)
{
    bool block = IsBlockFormat(fmt);
    UINT bpp = BitsPerPixel(fmt);
    for (UINT l = 0; l < mips; l++) {
        UINT lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
        UINT rows = block ? (lh + 3) / 4 : lh;
        UINT rowBytes = block ? ((lw + 3) / 4) * (bpp * 2) : lw * bpp / 8;
        if ((size_t)rows * rowBytes > avail) return false;
        D3DLOCKED_RECT lr;
        if (FAILED(o_TexLockRect(t, l, &lr, nullptr, 0))) return false;
        for (UINT r = 0; r < rows; r++) memcpy((uint8_t *)lr.pBits + (size_t)r * lr.Pitch, src + (size_t)r * rowBytes, rowBytes);
        o_TexUnlockRect(t, l);
        src += (size_t)rows * rowBytes;
        avail -= (size_t)rows * rowBytes;
    }
    return true;
}


// ---------------------------------------------------------------- mipmaps
// Muitos pacotes trazem DDS sem mipmaps: de longe a textura cintila e serrilha.
// Aqui geramos a cadeia completa (decodifica DXT -> reduz 2x2 -> recodifica DXT).

struct RGBA8 { uint8_t r, g, b, a; };

static void Unpack565(uint16_t c, uint8_t &r, uint8_t &g, uint8_t &b)
{
    r = (uint8_t)(((c >> 11) & 31) * 255 / 31);
    g = (uint8_t)(((c >> 5) & 63) * 255 / 63);
    b = (uint8_t)((c & 31) * 255 / 31);
}
static uint16_t Pack565(int r, int g, int b)
{
    return (uint16_t)(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255));
}

static void DecodeColorBlock(const uint8_t *blk, RGBA8 px[16], bool dxt1)
{
    uint16_t c0 = blk[0] | blk[1] << 8, c1 = blk[2] | blk[3] << 8;
    RGBA8 pal[4];
    Unpack565(c0, pal[0].r, pal[0].g, pal[0].b);
    Unpack565(c1, pal[1].r, pal[1].g, pal[1].b);
    pal[0].a = pal[1].a = 255;
    if (c0 > c1 || !dxt1) {
        pal[2] = {(uint8_t)((2 * pal[0].r + pal[1].r) / 3), (uint8_t)((2 * pal[0].g + pal[1].g) / 3), (uint8_t)((2 * pal[0].b + pal[1].b) / 3), 255};
        pal[3] = {(uint8_t)((pal[0].r + 2 * pal[1].r) / 3), (uint8_t)((pal[0].g + 2 * pal[1].g) / 3), (uint8_t)((pal[0].b + 2 * pal[1].b) / 3), 255};
    } else {
        pal[2] = {(uint8_t)((pal[0].r + pal[1].r) / 2), (uint8_t)((pal[0].g + pal[1].g) / 2), (uint8_t)((pal[0].b + pal[1].b) / 2), 255};
        pal[3] = {0, 0, 0, 0};
    }
    uint32_t idx = blk[4] | blk[5] << 8 | blk[6] << 16 | (uint32_t)blk[7] << 24;
    for (int i = 0; i < 16; i++) px[i] = pal[(idx >> (2 * i)) & 3];
}

static void DecodeDXT5Alpha(const uint8_t *blk, RGBA8 px[16])
{
    uint8_t a[8] = {blk[0], blk[1]};
    if (a[0] > a[1]) for (int i = 1; i < 7; i++) a[i + 1] = (uint8_t)(((7 - i) * a[0] + i * a[1]) / 7);
    else { for (int i = 1; i < 5; i++) a[i + 1] = (uint8_t)(((5 - i) * a[0] + i * a[1]) / 5); a[6] = 0; a[7] = 255; }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)blk[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) px[i].a = a[(bits >> (3 * i)) & 7];
}

static bool DecodeImage(D3DFORMAT fmt, const uint8_t *src, UINT w, UINT h, std::vector<RGBA8> &img)
{
    img.assign((size_t)w * h, RGBA8{0, 0, 0, 255});
    if (fmt == D3DFMT_A8R8G8B8 || fmt == D3DFMT_X8R8G8B8) {
        for (size_t i = 0; i < (size_t)w * h; i++)
            img[i] = {src[i * 4 + 2], src[i * 4 + 1], src[i * 4], fmt == D3DFMT_A8R8G8B8 ? src[i * 4 + 3] : (uint8_t)255};
        return true;
    }
    if (fmt != D3DFMT_DXT1 && fmt != D3DFMT_DXT3 && fmt != D3DFMT_DXT5) return false;
    UINT bw = (w + 3) / 4, bh = (h + 3) / 4, bsz = fmt == D3DFMT_DXT1 ? 8 : 16;
    for (UINT by = 0; by < bh; by++)
        for (UINT bx = 0; bx < bw; bx++) {
            const uint8_t *blk = src + ((size_t)by * bw + bx) * bsz;
            RGBA8 px[16];
            DecodeColorBlock(fmt == D3DFMT_DXT1 ? blk : blk + 8, px, fmt == D3DFMT_DXT1);
            if (fmt == D3DFMT_DXT3)
                for (int i = 0; i < 16; i++) { int v = (blk[i / 2] >> (4 * (i & 1))) & 15; px[i].a = (uint8_t)(v * 17); }
            else if (fmt == D3DFMT_DXT5) DecodeDXT5Alpha(blk, px);
            for (int i = 0; i < 16; i++) {
                UINT x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x < w && y < h) img[(size_t)y * w + x] = px[i];
            }
        }
    return true;
}

static void Downsample(const std::vector<RGBA8> &src, UINT w, UINT h, std::vector<RGBA8> &dst, UINT &nw, UINT &nh)
{
    nw = w > 1 ? w / 2 : 1; nh = h > 1 ? h / 2 : 1;
    dst.resize((size_t)nw * nh);
    for (UINT y = 0; y < nh; y++)
        for (UINT x = 0; x < nw; x++) {
            UINT x0 = x * 2 < w ? x * 2 : w - 1, x1 = x * 2 + 1 < w ? x * 2 + 1 : x0;
            UINT y0 = y * 2 < h ? y * 2 : h - 1, y1 = y * 2 + 1 < h ? y * 2 + 1 : y0;
            const RGBA8 &a = src[(size_t)y0 * w + x0], &b = src[(size_t)y0 * w + x1], &c = src[(size_t)y1 * w + x0], &d = src[(size_t)y1 * w + x1];
            dst[(size_t)y * nw + x] = {(uint8_t)((a.r + b.r + c.r + d.r + 2) / 4), (uint8_t)((a.g + b.g + c.g + d.g + 2) / 4),
                                       (uint8_t)((a.b + b.b + c.b + d.b + 2) / 4), (uint8_t)((a.a + b.a + c.a + d.a + 2) / 4)};
        }
}

static void EncodeColorBlock(const RGBA8 px[16], uint8_t *out, bool punchAlpha)
{
    int mn[3] = {255, 255, 255}, mx[3] = {0, 0, 0};
    bool anyTransparent = false;
    for (int i = 0; i < 16; i++) {
        if (punchAlpha && px[i].a < 128) { anyTransparent = true; continue; }
        const uint8_t c[3] = {px[i].r, px[i].g, px[i].b};
        for (int k = 0; k < 3; k++) { if (c[k] < mn[k]) mn[k] = c[k]; if (c[k] > mx[k]) mx[k] = c[k]; }
    }
    if (mn[0] > mx[0]) { mn[0] = mn[1] = mn[2] = mx[0] = mx[1] = mx[2] = 0; }
    for (int k = 0; k < 3; k++) { int inset = (mx[k] - mn[k]) / 16; mn[k] += inset; mx[k] -= inset; }
    uint16_t c0 = Pack565(mx[0], mx[1], mx[2]), c1 = Pack565(mn[0], mn[1], mn[2]);
    bool threeColor = anyTransparent;
    if (threeColor ? c0 > c1 : c0 < c1) { uint16_t t = c0; c0 = c1; c1 = t; }
    out[0] = c0 & 0xFF; out[1] = c0 >> 8; out[2] = c1 & 0xFF; out[3] = c1 >> 8;
    // paleta real dos endpoints: indices 0,1,2,3 nos 4 primeiros pixels
    RGBA8 pal[16], p[4];
    uint8_t tmp[8] = {out[0], out[1], out[2], out[3], 0xE4, 0, 0, 0};
    DecodeColorBlock(tmp, pal, true);
    for (int i = 0; i < 4; i++) p[i] = pal[i];
    uint32_t idx = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0, bestD = 1 << 30;
        if (threeColor && px[i].a < 128) best = 3;
        else
            for (int j = 0; j < (threeColor ? 3 : 4); j++) {
                int dr = px[i].r - p[j].r, dg = px[i].g - p[j].g, db = px[i].b - p[j].b, d = dr * dr + dg * dg + db * db;
                if (d < bestD) { bestD = d; best = j; }
            }
        idx |= (uint32_t)best << (2 * i);
    }
    out[4] = idx & 0xFF; out[5] = (idx >> 8) & 0xFF; out[6] = (idx >> 16) & 0xFF; out[7] = idx >> 24;
}

static void EncodeDXT5Alpha(const RGBA8 px[16], uint8_t *out)
{
    int mn = 255, mx = 0;
    for (int i = 0; i < 16; i++) { if (px[i].a < mn) mn = px[i].a; if (px[i].a > mx) mx = px[i].a; }
    out[0] = (uint8_t)mx; out[1] = (uint8_t)mn;
    uint8_t a[8] = {(uint8_t)mx, (uint8_t)mn};
    if (mx > mn) for (int i = 1; i < 7; i++) a[i + 1] = (uint8_t)(((7 - i) * mx + i * mn) / 7);
    else for (int i = 2; i < 8; i++) a[i] = (uint8_t)mx;
    uint64_t bits = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0, bestD = 1 << 30;
        for (int j = 0; j < 8; j++) { int d = abs(px[i].a - a[j]); if (d < bestD) { bestD = d; best = j; } }
        bits |= (uint64_t)best << (3 * i);
    }
    for (int i = 0; i < 6; i++) out[2 + i] = (uint8_t)(bits >> (8 * i));
}

static void EncodeImage(D3DFORMAT fmt, const std::vector<RGBA8> &img, UINT w, UINT h, std::vector<uint8_t> &out)
{
    if (fmt == D3DFMT_A8R8G8B8 || fmt == D3DFMT_X8R8G8B8) {
        for (auto &c : img) { out.push_back(c.b); out.push_back(c.g); out.push_back(c.r); out.push_back(c.a); }
        return;
    }
    UINT bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (UINT by = 0; by < bh; by++)
        for (UINT bx = 0; bx < bw; bx++) {
            RGBA8 px[16];
            for (int i = 0; i < 16; i++) {
                UINT x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                px[i] = img[(size_t)(y < h ? y : h - 1) * w + (x < w ? x : w - 1)];
            }
            uint8_t blk[16] = {};
            if (fmt == D3DFMT_DXT1) { EncodeColorBlock(px, blk, true); out.insert(out.end(), blk, blk + 8); continue; }
            if (fmt == D3DFMT_DXT3)
                for (int i = 0; i < 16; i++) blk[i / 2] |= (uint8_t)(((px[i].a + 8) / 17) << (4 * (i & 1)));
            else EncodeDXT5Alpha(px, blk);
            EncodeColorBlock(px, blk + 8, false);
            out.insert(out.end(), blk, blk + 16);
        }
}

// Gera mips 1..n a partir do nivel 0 e salva o DDS corrigido no lugar (so roda uma vez por arquivo).
static bool AddMipmaps(std::vector<uint8_t> &dds, D3DFORMAT fmt, UINT w, UINT h, UINT maxm, const std::wstring &path)
{
    std::vector<RGBA8> img, next;
    if (!DecodeImage(fmt, dds.data() + 128, w, h, img)) return false;
    bool block = IsBlockFormat(fmt);
    size_t level0 = block ? (size_t)((w + 3) / 4) * ((h + 3) / 4) * (fmt == D3DFMT_DXT1 ? 8 : 16) : (size_t)w * h * 4;
    dds.resize(128 + level0);
    UINT lw = w, lh = h;
    for (UINT l = 1; l < maxm; l++) {
        UINT nw, nh;
        Downsample(img, lw, lh, next, nw, nh);
        EncodeImage(fmt, next, nw, nh, dds);
        img.swap(next); lw = nw; lh = nh;
    }
    DWORD *hd = (DWORD *)(dds.data() + 4);
    hd[1] |= 0x20000;            // DDSD_MIPMAPCOUNT
    hd[6] = maxm;
    hd[26] |= 0x400008;          // DDSCAPS_COMPLEX | DDSCAPS_MIPMAP
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        BOOL ok = WriteFile(f, dds.data(), (DWORD)dds.size(), &wr, nullptr) && wr == dds.size();
        CloseHandle(f);
        if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
    }
    return true;
}

// Loader DDS proprio (DXT1-5/ATI1/ATI2/32bpp) - nao depende do d3dx9 do Wine.
static IDirect3DTexture9 *LoadDDS(IDirect3DDevice9 *dev, std::vector<uint8_t> &d, const std::wstring &path)
{
    if (d.size() < 128 || memcmp(d.data(), "DDS ", 4)) return nullptr;
    const DWORD *hd = (const DWORD *)(d.data() + 4);
    UINT h = hd[2], w = hd[3], mips = hd[6] ? hd[6] : 1;
    DWORD pfFlags = hd[19], fourcc = hd[20], bits = hd[21], rmask = hd[22], amask = hd[25], caps2 = hd[27];
    if (caps2 & 0x200) return nullptr; // cubemap: deixa para o d3dx
    D3DFORMAT fmt;
    if (pfFlags & 4) {
        fmt = (D3DFORMAT)fourcc;
        if (!IsBlockFormat(fmt)) return nullptr;
    } else if ((pfFlags & 0x40) && bits == 32 && rmask == 0x00FF0000) {
        fmt = (pfFlags & 1) && amask ? D3DFMT_A8R8G8B8 : D3DFMT_X8R8G8B8;
    } else return nullptr;
    // limita mips ao maximo possivel
    UINT maxm = 1; for (UINT s = w > h ? w : h; s > 1; s >>= 1) maxm++;
    if (mips > maxm) mips = maxm;
    if (mips == 1 && maxm > 1) {
        DWORD t0 = GetTickCount();
        if (AddMipmaps(d, fmt, w, h, maxm, path)) {
            mips = maxm;
            Log("mipmaps gerados para %ls (%u niveis, %lu ms)", path.c_str() + path.rfind(L'\\') + 1, mips, GetTickCount() - t0);
        }
    }

    IDirect3DTexture9 *t = nullptr;
    if (g_pool == D3DPOOL_MANAGED) {
        if (FAILED(o_CreateTexture(dev, w, h, mips, 0, fmt, D3DPOOL_MANAGED, &t, nullptr))) return nullptr;
        if (!FillLevels(t, fmt, w, h, mips, d.data() + 128, d.size() - 128)) { o_TexRelease(t); return nullptr; }
        return t;
    }
    IDirect3DTexture9 *stage = nullptr;
    if (FAILED(o_CreateTexture(dev, w, h, mips, 0, fmt, D3DPOOL_SYSTEMMEM, &stage, nullptr))) return nullptr;
    if (!FillLevels(stage, fmt, w, h, mips, d.data() + 128, d.size() - 128) ||
        FAILED(o_CreateTexture(dev, w, h, mips, 0, fmt, D3DPOOL_DEFAULT, &t, nullptr)) ||
        FAILED(o_UpdateTexture(dev, (IDirect3DBaseTexture9 *)stage, (IDirect3DBaseTexture9 *)t))) {
        if (t) o_TexRelease(t);
        t = nullptr;
    }
    o_TexRelease(stage);
    return t;
}

static IDirect3DTexture9 *LoadReplacementFile(IDirect3DDevice9 *dev, uint32_t hash, const std::wstring &path)
{
    IDirect3DTexture9 *t = nullptr;
    t_internal++;
    bool isDds = path.size() > 4 && _wcsicmp(path.c_str() + path.size() - 4, L".dds") == 0;
    if (isDds) { std::vector<uint8_t> data = ReadFileAll(path); t = LoadDDS(dev, data, path); }
    if (!t && p_D3DXCreateTextureFromFileExW) {
        // D3DX_FROM_FILE = 0xFFFFFFFD, D3DFMT_FROM_FILE = 0xFFFFFFFD, D3DX_DEFAULT = 0xFFFFFFFF, NONPOW2 = 0xFFFFFFFE
        HRESULT hr = p_D3DXCreateTextureFromFileExW(dev, path.c_str(), 0xFFFFFFFE, 0xFFFFFFFE,
                                                    isDds ? 0xFFFFFFFD : 0xFFFFFFFF, 0,
                                                    isDds ? (D3DFORMAT)0xFFFFFFFD : D3DFMT_UNKNOWN, g_pool,
                                                    0xFFFFFFFF, 0xFFFFFFFF, 0, nullptr, nullptr, &t);
        if (FAILED(hr)) { t = nullptr; Log("ERRO d3dx 0x%08X ao carregar %ls", (unsigned)hr, path.c_str()); }
    }
    t_internal--;
    return t;
}

// ---------------------------------------------------------------- limite de carregamento
// Ao entrar numa area nova chegam dezenas de substituicoes de uma vez. Carregar todas no mesmo
// quadro trava o jogo (ex.: 27 texturas = 81 ms). Aqui o carregamento usa no maximo LoadBudget ms
// a cada 10 ms (balde de fichas): o que passar do limite aparece com a textura original por alguns
// quadros e e carregado logo depois. Funciona por tempo, nao por quadro, entao tambem vale durante
// telas de carregamento, quando o jogo pode ficar muito tempo sem terminar um quadro.

static SRWLOCK g_budgetLock = SRWLOCK_INIT;
static double g_budgetTokens, g_budgetLast, g_qpcToMs;
static volatile LONG g_statDeferred;

static double NowMs()
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * g_qpcToMs;
}

static void BudgetInit()
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcToMs = 1000.0 / (double)f.QuadPart;
    g_budgetTokens = g_loadBudget;
    g_budgetLast = NowMs();
}

// true se ainda ha tempo de carregamento disponivel agora. O custo e descontado depois (BudgetSpend),
// entao uma textura grande sempre carrega e o saldo so fica negativo por alguns milissegundos.
static bool BudgetAllows()
{
    if (g_loadBudget <= 0) return true;
    AcquireSRWLockExclusive(&g_budgetLock);
    double now = NowMs();
    g_budgetTokens += (now - g_budgetLast) * g_loadBudget / 10.0;
    if (g_budgetTokens > g_loadBudget) g_budgetTokens = g_loadBudget;
    g_budgetLast = now;
    bool ok = g_budgetTokens > 0;
    ReleaseSRWLockExclusive(&g_budgetLock);
    return ok;
}

static void BudgetSpend(double ms)
{
    if (g_loadBudget <= 0) return;
    AcquireSRWLockExclusive(&g_budgetLock);
    g_budgetTokens -= ms;
    ReleaseSRWLockExclusive(&g_budgetLock);
}

// Garante que a substituicao do hash esta carregada. Chamado SEM g_lock.
// Se o limite de carregamento estourou, devolve nullptr sem marcar falha: o jogo usa a textura
// original neste quadro e a substituicao e carregada na proxima vez que a textura for usada.
// Devolve o ponteiro com AddRef (quem chama solta com ReleaseInternal), assim
// outra thread nao consegue destruir a textura enquanto ela esta sendo usada.
static void ReleaseInternal(IDirect3DTexture9 *t)
{
    if (!t) return;
    t_internal++; o_TexRelease(t); t_internal--;
}

static IDirect3DTexture9 *AcquireReplacement(IDirect3DDevice9 *dev, uint32_t hash)
{
    std::wstring path;
    {
        WLock l;
        auto it = g_rep.find(hash);
        if (it == g_rep.end() || it->second.users <= 0 || it->second.failed) return nullptr;
        if (it->second.tex) { IDirect3DTexture9_AddRef(it->second.tex); return it->second.tex; }
        auto f = g_files.find(hash);
        if (f == g_files.end()) { it->second.failed = true; return nullptr; }
        path = f->second;
    }
    if (!BudgetAllows()) { InterlockedIncrement(&g_statDeferred); return nullptr; }
    double t0 = NowMs();
    IDirect3DTexture9 *t = LoadReplacementFile(dev, hash, path);
    double ms = NowMs() - t0;
    BudgetSpend(ms);
    IDirect3DTexture9 *extra = nullptr, *ret = nullptr;
    {
        WLock l;
        auto it = g_rep.find(hash);
        if (it == g_rep.end() || it->second.users <= 0) {
            extra = t; // ninguem mais usa esse hash: descarta
        } else if (it->second.tex) {
            extra = t; // outra thread carregou primeiro
            ret = it->second.tex;
            IDirect3DTexture9_AddRef(ret);
        } else if (!t) {
            it->second.failed = true;
        } else {
            it->second.tex = t;
            ret = t;
            IDirect3DTexture9_AddRef(ret);
        }
    }
    ReleaseInternal(extra);
    if (!t) { Log("FALHA ao carregar substituicao 0x%08X (%ls)", hash, path.c_str()); return nullptr; }
    if (ret == t) {
        InterlockedIncrement(&g_statLoaded);
        D3DSURFACE_DESC sd; IDirect3DTexture9_GetLevelDesc(t, 0, &sd);
        Log("carregada 0x%08X  %ux%u fmt=0x%X  (%.1f ms)", hash, sd.Width, sd.Height, (unsigned)sd.Format, ms);
    }
    return ret;
}

// ---------------------------------------------------------------- dump (para criar mods)

static void DumpTexture(uint32_t hash, const TexInfo &ti, const uint8_t *bits, INT pitch)
{
    {
        WLock l;
        if (!g_dumped.insert(hash).second) return;
    }
    UINT bpp = BitsPerPixel(ti.fmt);
    bool block = IsBlockFormat(ti.fmt);
    DWORD hdr[32] = {};
    hdr[0] = MAKEFOURCC('D', 'D', 'S', ' ');
    hdr[1] = 124; hdr[2] = 0x1007; hdr[3] = ti.h; hdr[4] = ti.w; hdr[7] = 1;
    hdr[19] = 32;
    if (block) { hdr[20] = 4; hdr[21] = (DWORD)ti.fmt; }
    else if (ti.fmt == D3DFMT_A8R8G8B8 || ti.fmt == D3DFMT_X8R8G8B8) {
        hdr[20] = 0x40 | (ti.fmt == D3DFMT_A8R8G8B8 ? 1 : 0); hdr[22] = 32;
        hdr[23] = 0xFF0000; hdr[24] = 0xFF00; hdr[25] = 0xFF; hdr[26] = ti.fmt == D3DFMT_A8R8G8B8 ? 0xFF000000 : 0;
    } else return;
    hdr[28] = 0x1000;
    CreateDirectoryW(g_dumpDir.c_str(), nullptr);
    wchar_t name[64]; swprintf(name, 64, L"\\0x%08X.dds", hash);
    HANDLE f = CreateFileW((g_dumpDir + name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wr;
    WriteFile(f, hdr, 128, &wr, nullptr);
    UINT rows = block ? (ti.h + 3) / 4 : ti.h;
    UINT rowBytes = block ? ((ti.w + 3) / 4) * bpp * 2 : ti.w * bpp / 8;
    for (UINT r = 0; r < rows; r++) WriteFile(f, bits + (size_t)r * pitch, rowBytes, &wr, nullptr);
    CloseHandle(f);
}

// ---------------------------------------------------------------- hashing

// Calcula o hash TexMod a partir dos dados do nivel 0. Chamado SEM g_lock.
static uint32_t HashBits(const TexInfo &ti, const uint8_t *bits, INT pitch)
{
    // TexMod/uMod: CRC sobre (bpp*w*h/8) bytes contiguos a partir de pBits (ignora pitch).
    size_t size = (size_t)BitsPerPixel(ti.fmt) * ti.w * ti.h / 8;
    uint32_t crc = TexModCrc(bits, size);
    if (g_dump) DumpTexture(crc, ti, bits, pitch);
    return crc;
}

static bool Hashable(const TexInfo &ti)
{
    return BitsPerPixel(ti.fmt) && !(ti.usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) &&
           ti.rehashes < kMaxRehash;
}

// Registra o hash calculado e carrega a substituicao (se houver).
static void AssignHash(IDirect3DTexture9 *tex, uint32_t hash)
{
    bool want = false;
    TexInfo copy;
    IDirect3DTexture9 *drop = nullptr;
    {
        WLock l;
        auto it = g_tex.find(tex);
        if (it == g_tex.end()) return;
        TexInfo &ti = it->second;
        if (ti.hashed && ti.hasRep && ti.hash != hash) {
            auto r = g_rep.find(ti.hash);
            if (r != g_rep.end() && --r->second.users <= 0) { drop = r->second.tex; g_rep.erase(r); }
        }
        bool same = ti.hashed && ti.hash == hash;
        if (ti.hashed) ti.rehashes++;
        ti.hash = hash; ti.hashed = true; ti.dirty = false;
        if (!same) {
            ti.hasRep = g_files.count(hash) != 0;
            if (ti.hasRep) g_rep[hash].users++;
        }
        want = ti.hasRep && !same;
        copy = ti;
    }
    ReleaseInternal(drop);
    InterlockedIncrement(&g_statHashed);
    if (g_logHashes || want)
        Log("%s 0x%08X  %ux%u fmt=0x%X lv=%u pool=%d", want ? "MATCH" : "hash ", hash, copy.w, copy.h,
            (unsigned)copy.fmt, copy.levels, (int)copy.pool);
    if (want) {
        InterlockedIncrement(&g_statMatched);
        if (g_device) ReleaseInternal(AcquireReplacement(g_device, hash));
    }
}

// Hash "preguicoso": le o nivel 0 com LockRect READONLY.
static void LazyHash(IDirect3DTexture9 *tex, const TexInfo &ti)
{
    D3DLOCKED_RECT lr;
    HRESULT hr;
    if (!Hashable(ti) || (ti.pool == D3DPOOL_DEFAULT && !(ti.usage & D3DUSAGE_DYNAMIC))) goto fail; // nao bloqueavel
    t_internal++;
    hr = o_TexLockRect(tex, 0, &lr, nullptr, D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK);
    if (SUCCEEDED(hr)) {
        uint32_t h = HashBits(ti, (const uint8_t *)lr.pBits, lr.Pitch);
        o_TexUnlockRect(tex, 0);
        t_internal--;
        AssignHash(tex, h);
        return;
    }
    t_internal--;
fail:
    WLock l;
    auto it = g_tex.find(tex);
    if (it != g_tex.end()) it->second.dirty = false; // nao tenta de novo ate mudar
}

// ---------------------------------------------------------------- hooks: texture

static HRESULT STDMETHODCALLTYPE H_TexLockRect(IDirect3DTexture9 *t, UINT level, D3DLOCKED_RECT *lr, const RECT *rc, DWORD flags)
{
    HRESULT hr = o_TexLockRect(t, level, lr, rc, flags);
    if (t_internal || level != 0 || FAILED(hr) || (flags & D3DLOCK_READONLY)) return hr;
    WLock l;
    auto it = g_tex.find(t);
    if (it != g_tex.end()) {
        it->second.dirty = true;
        it->second.lockBits = rc ? nullptr : lr->pBits;
        it->second.lockPitch = lr->Pitch;
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_TexUnlockRect(IDirect3DTexture9 *t, UINT level)
{
    if (!t_internal && level == 0) {
        TexInfo ti; bool doHash = false;
        {
            WLock l;
            auto it = g_tex.find(t);
            if (it != g_tex.end() && it->second.lockBits) {
                ti = it->second;
                it->second.lockBits = nullptr;
                doHash = Hashable(ti);
            }
        }
        if (doHash) {
            uint32_t h = HashBits(ti, (const uint8_t *)ti.lockBits, ti.lockPitch);
            HRESULT hr = o_TexUnlockRect(t, level);
            AssignHash(t, h);
            return hr;
        }
    }
    return o_TexUnlockRect(t, level);
}

static ULONG STDMETHODCALLTYPE H_TexRelease(IDirect3DTexture9 *t)
{
    ULONG r = o_TexRelease(t);
    if (r == 0 && !t_internal) {
        IDirect3DTexture9 *drop = nullptr;
        {
            WLock l;
            auto it = g_tex.find(t);
            if (it != g_tex.end()) {
                if (it->second.hashed && it->second.hasRep) {
                    auto rp = g_rep.find(it->second.hash);
                    if (rp != g_rep.end() && --rp->second.users <= 0) { drop = rp->second.tex; g_rep.erase(rp); }
                }
                g_tex.erase(it);
            }
        }
        ReleaseInternal(drop);
    }
    return r;
}

static HRESULT STDMETHODCALLTYPE H_SurfUnlockRect(IDirect3DSurface9 *s)
{
    HRESULT hr = o_SurfUnlockRect(s);
    if (t_internal) return hr;
    // escrita direta via superficie: marca a textura dona como "suja"
    // (so o nivel 0 entra no hash; mipmaps menores sao ignorados)
    IDirect3DTexture9 *tex = nullptr;
    if (SUCCEEDED(IDirect3DSurface9_GetContainer(s, IID_IDirect3DTexture9, (void **)&tex)) && tex) {
        D3DSURFACE_DESC sd;
        if (SUCCEEDED(IDirect3DSurface9_GetDesc(s, &sd))) {
            WLock l;
            auto it = g_tex.find(tex);
            if (it != g_tex.end() && sd.Width == it->second.w && sd.Height == it->second.h) it->second.dirty = true;
        }
        ReleaseInternal(tex);
    }
    return hr;
}

// ---------------------------------------------------------------- hooks: device

static void HookTextureVtbl(IDirect3DTexture9 *t)
{
    static bool done;
    if (done) return;
    done = true;
    HookVtbl(t, VT(IDirect3DTexture9, LockRect), H_TexLockRect, &o_TexLockRect);
    HookVtbl(t, VT(IDirect3DTexture9, UnlockRect), H_TexUnlockRect, &o_TexUnlockRect);
    HookVtbl(t, VT(IDirect3DTexture9, Release), H_TexRelease, &o_TexRelease);
    IDirect3DSurface9 *s = nullptr;
    if (SUCCEEDED(IDirect3DTexture9_GetSurfaceLevel(t, 0, &s)) && s) {
        HookVtbl(s, VT(IDirect3DSurface9, UnlockRect), H_SurfUnlockRect, &o_SurfUnlockRect);
        IDirect3DSurface9_Release(s);
    }
    Log("hooks de textura instalados");
}

static HRESULT STDMETHODCALLTYPE H_CreateTexture(IDirect3DDevice9 *d, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt,
                                                 D3DPOOL pool, IDirect3DTexture9 **out, HANDLE *sh)
{
    HRESULT hr = o_CreateTexture(d, w, h, levels, usage, fmt, pool, out, sh);
    if (FAILED(hr) || !out || !*out || t_internal) return hr;
    if (!o_TexLockRect) { t_internal++; HookTextureVtbl(*out); t_internal--; }
    TexInfo ti;
    ti.w = w; ti.h = h; ti.levels = levels; ti.fmt = fmt; ti.pool = pool; ti.usage = usage;
    if (!Hashable(ti)) return hr;
    IDirect3DTexture9 *drop = nullptr;
    {
        WLock l;
        auto it = g_tex.find(*out);
        if (it != g_tex.end() && it->second.hashed && it->second.hasRep) { // entrada velha no mesmo endereco
            auto rp = g_rep.find(it->second.hash);
            if (rp != g_rep.end() && --rp->second.users <= 0) { drop = rp->second.tex; g_rep.erase(rp); }
        }
        g_tex[*out] = ti;
    }
    ReleaseInternal(drop);
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_SetTexture(IDirect3DDevice9 *d, DWORD stage, IDirect3DBaseTexture9 *bt)
{
    if (!bt || !g_enabled || t_internal) return o_SetTexture(d, stage, bt);
    IDirect3DTexture9 *t = (IDirect3DTexture9 *)bt;
    TexInfo ti;
    {
        AcquireSRWLockShared(&g_lock);
        auto it = g_tex.find(t);
        if (it == g_tex.end()) { ReleaseSRWLockShared(&g_lock); return o_SetTexture(d, stage, bt); }
        ti = it->second;
        ReleaseSRWLockShared(&g_lock);
    }
    if (ti.dirty && !ti.lockBits && Hashable(ti)) {
        LazyHash(t, ti);
        AcquireSRWLockShared(&g_lock);
        auto it = g_tex.find(t);
        if (it != g_tex.end()) ti = it->second;
        ReleaseSRWLockShared(&g_lock);
    }
    if (!ti.hashed || !ti.hasRep) return o_SetTexture(d, stage, bt);
    IDirect3DTexture9 *rep = AcquireReplacement(d, ti.hash); // com AddRef
    HRESULT hr = o_SetTexture(d, stage, rep ? (IDirect3DBaseTexture9 *)rep : bt);
    ReleaseInternal(rep); // o device guarda a propria referencia
    return hr;
}

// Textura DEFAULT preenchida via UpdateTexture(sysmem -> default): herda o hash da origem.
static HRESULT STDMETHODCALLTYPE H_UpdateTexture(IDirect3DDevice9 *d, IDirect3DBaseTexture9 *src, IDirect3DBaseTexture9 *dst)
{
    HRESULT hr = o_UpdateTexture(d, src, dst);
    if (FAILED(hr) || t_internal || !src || !dst) return hr;
    TexInfo si; bool known = false, dstKnown = false;
    {
        AcquireSRWLockShared(&g_lock);
        auto s = g_tex.find((IDirect3DTexture9 *)src);
        if (s != g_tex.end()) { si = s->second; known = true; }
        dstKnown = g_tex.count((IDirect3DTexture9 *)dst) != 0;
        ReleaseSRWLockShared(&g_lock);
    }
    if (!known || !dstKnown) return hr;
    if (si.dirty || !si.hashed) {
        LazyHash((IDirect3DTexture9 *)src, si);
        AcquireSRWLockShared(&g_lock);
        auto s = g_tex.find((IDirect3DTexture9 *)src);
        if (s != g_tex.end()) si = s->second;
        ReleaseSRWLockShared(&g_lock);
    }
    if (si.hashed) AssignHash((IDirect3DTexture9 *)dst, si.hash);
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_UpdateSurface(IDirect3DDevice9 *d, IDirect3DSurface9 *src, const RECT *rc, IDirect3DSurface9 *dst, const POINT *pt)
{
    HRESULT hr = o_UpdateSurface(d, src, rc, dst, pt);
    if (FAILED(hr) || t_internal || rc || pt || !src || !dst) return hr;
    t_internal++;
    IDirect3DTexture9 *tex = nullptr;
    D3DSURFACE_DESC dd, sd;
    if (SUCCEEDED(IDirect3DSurface9_GetContainer(dst, IID_IDirect3DTexture9, (void **)&tex)) && tex) {
        TexInfo ti; bool known = false;
        {
            AcquireSRWLockShared(&g_lock);
            auto it = g_tex.find(tex);
            if (it != g_tex.end()) { ti = it->second; known = true; }
            ReleaseSRWLockShared(&g_lock);
        }
        IDirect3DSurface9_GetDesc(dst, &dd);
        IDirect3DSurface9_GetDesc(src, &sd);
        if (known && dd.Width == ti.w && dd.Height == ti.h && sd.Width == ti.w && sd.Height == ti.h && sd.Format == ti.fmt) {
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(IDirect3DSurface9_LockRect(src, &lr, nullptr, D3DLOCK_READONLY))) {
                uint32_t h = HashBits(ti, (const uint8_t *)lr.pBits, lr.Pitch);
                IDirect3DSurface9_UnlockRect(src);
                t_internal--;
                AssignHash(tex, h);
                t_internal++;
            }
        }
        IDirect3DTexture9_Release(tex);
    }
    t_internal--;
    return hr;
}

static void ReleaseAllReplacements()
{
    std::vector<IDirect3DTexture9 *> drop;
    {
        WLock l;
        for (auto &r : g_rep) { if (r.second.tex) drop.push_back(r.second.tex); r.second.tex = nullptr; r.second.failed = false; }
    }
    for (auto *t : drop) ReleaseInternal(t);
}

static HRESULT STDMETHODCALLTYPE H_Reset(IDirect3DDevice9 *d, D3DPRESENT_PARAMETERS *pp)
{
    // texturas D3DPOOL_DEFAULT impedem o Reset; recarregadas sob demanda depois
    if (g_pool == D3DPOOL_DEFAULT) {
        // textura ainda ligada a um stage continua viva e faria o Reset falhar
        t_internal++;
        for (DWORD s = 0; s < 16; s++) o_SetTexture(d, s, nullptr);
        for (DWORD s = D3DVERTEXTEXTURESAMPLER0; s <= D3DVERTEXTEXTURESAMPLER3; s++) o_SetTexture(d, s, nullptr);
        t_internal--;
        ReleaseAllReplacements();
    }
    HRESULT hr = o_Reset(d, pp);
    Log("Reset -> 0x%08X", (unsigned)hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_EndScene(IDirect3DDevice9 *d)
{
    static bool keyWas;
    static DWORD lastStat;
    DWORD fgPid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &fgPid);
    bool down = fgPid == GetCurrentProcessId() && (GetAsyncKeyState(g_toggleKey) & 0x8000) != 0;
    if (down && !keyWas) {
        g_enabled = !g_enabled;
        Log("substituicao %s (tecla)", g_enabled ? "LIGADA" : "DESLIGADA");
    }
    keyWas = down;
    DWORD now = GetTickCount();
    if (now - lastStat > 30000) {
        lastStat = now;
        size_t live, reps = 0;
        { AcquireSRWLockShared(&g_lock); live = g_tex.size(); for (auto &r : g_rep) reps += r.second.tex != nullptr; ReleaseSRWLockShared(&g_lock); }
        Log("stats: texturas vivas=%u hashes=%ld matches=%ld carregadas=%ld substituicoes ativas=%u adiadas=%ld",
            (unsigned)live, g_statHashed, g_statMatched, g_statLoaded, (unsigned)reps, g_statDeferred);
    }
    return o_EndScene(d);
}

static HRESULT STDMETHODCALLTYPE H_CreateDevice(IDirect3D9 *d3d, UINT adapter, D3DDEVTYPE type, HWND wnd, DWORD flags,
                                                D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
    HRESULT hr = o_CreateDevice(d3d, adapter, type, wnd, flags, pp, out);
    Log("CreateDevice -> 0x%08X flags=0x%X", (unsigned)hr, (unsigned)flags);
    if (FAILED(hr) || !out || !*out) return hr;
    IDirect3DDevice9 *dev = *out;
    g_device = dev;
    HookVtbl(dev, VT(IDirect3DDevice9, CreateTexture), H_CreateTexture, &o_CreateTexture);
    HookVtbl(dev, VT(IDirect3DDevice9, SetTexture), H_SetTexture, &o_SetTexture);
    HookVtbl(dev, VT(IDirect3DDevice9, UpdateTexture), H_UpdateTexture, &o_UpdateTexture);
    HookVtbl(dev, VT(IDirect3DDevice9, UpdateSurface), H_UpdateSurface, &o_UpdateSurface);
    HookVtbl(dev, VT(IDirect3DDevice9, Reset), H_Reset, &o_Reset);
    HookVtbl(dev, VT(IDirect3DDevice9, EndScene), H_EndScene, &o_EndScene);
    // instala hooks de textura ja com uma textura de teste
    if (!o_TexLockRect) {
        IDirect3DTexture9 *t = nullptr;
        t_internal++;
        if (SUCCEEDED(o_CreateTexture(dev, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr)) && t) {
            HookTextureVtbl(t);
            o_TexRelease(t);
        }
        t_internal--;
    }
    Log("hooks de device instalados (device=%p)", (void *)dev);
    return hr;
}

// Hook inline na exportacao Direct3DCreate9 do d3d9.dll carregado (ReShade):
// o exe e empacotado pela DRM e monta a IAT so depois, entao a IAT nao serve.
static uint8_t *g_d3dCreateFn;
static uint8_t g_d3dCreateSaved[5];
static CRITICAL_SECTION g_d3dCreateCs;
static IDirect3D9 *WINAPI H_Direct3DCreate9(UINT sdk);

static void SetInlineHook(bool on)
{
    DWORD old;
    VirtualProtect(g_d3dCreateFn, 5, PAGE_EXECUTE_READWRITE, &old);
    if (on) {
        g_d3dCreateFn[0] = 0xE9;
        *(int32_t *)(g_d3dCreateFn + 1) = (int32_t)((uint8_t *)H_Direct3DCreate9 - g_d3dCreateFn - 5);
    } else memcpy(g_d3dCreateFn, g_d3dCreateSaved, 5);
    VirtualProtect(g_d3dCreateFn, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_d3dCreateFn, 5);
}

static IDirect3D9 *WINAPI H_Direct3DCreate9(UINT sdk)
{
    IDirect3D9 *d3d;
    if (g_d3dCreateFn) {
        EnterCriticalSection(&g_d3dCreateCs);
        SetInlineHook(false);
        d3d = ((Direct3DCreate9_t)g_d3dCreateFn)(sdk);
        SetInlineHook(true);
        LeaveCriticalSection(&g_d3dCreateCs);
    } else d3d = o_Direct3DCreate9(sdk);
    Log("Direct3DCreate9(0x%X) -> %p", sdk, (void *)d3d);
    if (d3d) HookVtbl(d3d, VT(IDirect3D9, CreateDevice), H_CreateDevice, &o_CreateDevice);
    return d3d;
}

// ---------------------------------------------------------------- IAT

static bool PatchIAT(HMODULE mod, const char *dll, const char *func, void *hook, void **orig)
{
    auto base = (uint8_t *)mod;
    auto dos = (IMAGE_DOS_HEADER *)base;
    auto nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; imp++) {
        if (_stricmp((char *)(base + imp->Name), dll)) continue;
        auto thunk = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        auto othunk = (IMAGE_THUNK_DATA *)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        for (; othunk->u1.AddressOfData; thunk++, othunk++) {
            if (IMAGE_SNAP_BY_ORDINAL(othunk->u1.Ordinal)) continue;
            auto ibn = (IMAGE_IMPORT_BY_NAME *)(base + othunk->u1.AddressOfData);
            if (strcmp((char *)ibn->Name, func)) continue;
            DWORD old;
            VirtualProtect(&thunk->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
            *orig = (void *)thunk->u1.Function;
            thunk->u1.Function = (uintptr_t)hook;
            VirtualProtect(&thunk->u1.Function, sizeof(void *), old, &old);
            return true;
        }
    }
    return false;
}

static void Init()
{
    InitializeCriticalSection(&g_logCs);
    GetModuleFileNameW(nullptr, g_gameDir, MAX_PATH);
    if (wchar_t *s = wcsrchr(g_gameDir, L'\\')) *s = 0;
    g_log = _wfopen((std::wstring(g_gameDir) + L"\\DS2TexInject.log").c_str(), L"w");
    LoadConfig();
    BudgetInit();
    Log("DS2TexInject " DS2TI_VERSION " iniciado. jogo=%ls pool=%s dump=%d logHashes=%d loadBudget=%d", g_gameDir,
        g_pool == D3DPOOL_MANAGED ? "managed" : "default", g_dump, g_logHashes, g_loadBudget);
    CrcInit();
    ScanTextureDir();
    if (HMODULE dx = LoadLibraryW(L"d3dx9_43.dll"))
        p_D3DXCreateTextureFromFileExW = (D3DXCreateTextureFromFileExW_t)GetProcAddress(dx, "D3DXCreateTextureFromFileExW");
    Log("d3dx9_43: %s", p_D3DXCreateTextureFromFileExW ? "ok" : "NAO encontrado (so DDS)");
    if (PatchIAT(GetModuleHandleW(nullptr), "d3d9.dll", "Direct3DCreate9", (void *)H_Direct3DCreate9, (void **)&o_Direct3DCreate9)) {
        Log("IAT Direct3DCreate9 interceptado (orig=%p)", (void *)o_Direct3DCreate9);
        return;
    }
    // mesmo caminho de busca do exe -> pega o d3d9.dll da pasta do jogo (ReShade) ou o do sistema
    HMODULE d3d9 = LoadLibraryW(L"d3d9.dll");
    g_d3dCreateFn = d3d9 ? (uint8_t *)GetProcAddress(d3d9, "Direct3DCreate9") : nullptr;
    if (!g_d3dCreateFn) { Log("ERRO: Direct3DCreate9 nao encontrado"); return; }
    wchar_t path[MAX_PATH]; GetModuleFileNameW(d3d9, path, MAX_PATH);
    InitializeCriticalSection(&g_d3dCreateCs);
    memcpy(g_d3dCreateSaved, g_d3dCreateFn, 5);
    SetInlineHook(true);
    Log("hook inline em Direct3DCreate9 de %ls (%p)", path, (void *)g_d3dCreateFn);
}

extern "C" BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        Init();
    }
    return TRUE;
}
