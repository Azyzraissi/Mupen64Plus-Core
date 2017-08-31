#include "rdp.h"
#include "vi.h"
#include "common.h"
#include "rdram.h"
#include "trace_write.h"
#include "msg.h"
#include "irand.h"
#include "file.h"
#include "parallel_c.hpp"

#include <memory.h>
#include <string.h>

// tctables.h
static const int32_t norm_point_table[64] = {
    0x4000, 0x3f04, 0x3e10, 0x3d22, 0x3c3c, 0x3b5d, 0x3a83, 0x39b1,
    0x38e4, 0x381c, 0x375a, 0x369d, 0x35e5, 0x3532, 0x3483, 0x33d9,
    0x3333, 0x3291, 0x31f4, 0x3159, 0x30c3, 0x3030, 0x2fa1, 0x2f15,
    0x2e8c, 0x2e06, 0x2d83, 0x2d03, 0x2c86, 0x2c0b, 0x2b93, 0x2b1e,
    0x2aab, 0x2a3a, 0x29cc, 0x2960, 0x28f6, 0x288e, 0x2828, 0x27c4,
    0x2762, 0x2702, 0x26a4, 0x2648, 0x25ed, 0x2594, 0x253d, 0x24e7,
    0x2492, 0x243f, 0x23ee, 0x239e, 0x234f, 0x2302, 0x22b6, 0x226c,
    0x2222, 0x21da, 0x2193, 0x214d, 0x2108, 0x20c5, 0x2082, 0x2041
};

static const int32_t norm_slope_table[64] = {
    0xf03, 0xf0b, 0xf11, 0xf19, 0xf20, 0xf25, 0xf2d, 0xf32,
    0xf37, 0xf3d, 0xf42, 0xf47, 0xf4c, 0xf50, 0xf55, 0xf59,
    0xf5d, 0xf62, 0xf64, 0xf69, 0xf6c, 0xf70, 0xf73, 0xf76,
    0xf79, 0xf7c, 0xf7f, 0xf82, 0xf84, 0xf87, 0xf8a, 0xf8c,
    0xf8e, 0xf91, 0xf93, 0xf95, 0xf97, 0xf99, 0xf9b, 0xf9d,
    0xf9f, 0xfa1, 0xfa3, 0xfa4, 0xfa6, 0xfa8, 0xfa9, 0xfaa,
    0xfac, 0xfae, 0xfaf, 0xfb0, 0xfb2, 0xfb3, 0xfb5, 0xfb5,
    0xfb7, 0xfb8, 0xfb9, 0xfba, 0xfbc, 0xfbc, 0xfbe, 0xfbe
};

#define SIGN16(x)   ((int16_t)(x))
#define SIGN8(x)    ((int8_t)(x))


#define SIGN(x, numb)   (((x) & ((1 << numb) - 1)) | -((x) & (1 << (numb - 1))))
#define SIGNF(x, numb)  ((x) | -((x) & (1 << (numb - 1))))







#define GET_LOW_RGBA16_TMEM(x)  (replicated_rgba[((x) >> 1) & 0x1f])
#define GET_MED_RGBA16_TMEM(x)  (replicated_rgba[((x) >> 6) & 0x1f])
#define GET_HI_RGBA16_TMEM(x)   (replicated_rgba[(x) >> 11])

// bit constants for DP_STATUS
#define DP_STATUS_XBUS_DMA      0x001   // DMEM DMA mode is set
#define DP_STATUS_FREEZE        0x002   // Freeze has been set
#define DP_STATUS_FLUSH         0x004   // Flush has been set
#define DP_STATUS_START_GCLK    0x008   // Unknown
#define DP_STATUS_TMEM_BUSY     0x010   // TMEM is in use on the RDP
#define DP_STATUS_PIPE_BUSY     0x020   // Graphics pipe is in use on the RDP
#define DP_STATUS_CMD_BUSY      0x040   // RDP is currently executing a command
#define DP_STATUS_CBUF_BUSY     0x080   // RDRAM RDP command buffer is in use
#define DP_STATUS_DMA_BUSY      0x100   // DMEM RDP command buffer is in use
#define DP_STATUS_END_VALID     0x200   // Unknown
#define DP_STATUS_START_VALID   0x400   // Unknown

static struct core_config* config;
static struct plugin_api* plugin;

static uint32_t rdp_cmd_data[0x10000];
static uint32_t rdp_cmd_ptr = 0;
static uint32_t rdp_cmd_cur = 0;
static uint32_t ptr_onstart = 0;

#define CMD_BUFFER_COUNT 1024

static uint32_t rdp_cmd_buf[CMD_BUFFER_COUNT][CMD_MAX_INTS];
static uint32_t rdp_cmd_buf_pos;

static TLS int blshifta = 0, blshiftb = 0, pastblshifta = 0, pastblshiftb = 0;
static TLS int32_t pastrawdzmem = 0;

struct span
{
    int lx, rx;
    int unscrx;
    int validline;
    int32_t r, g, b, a, s, t, w, z;
    int32_t majorx[4];
    int32_t minorx[4];
    int32_t invalyscan[4];
};

static TLS struct span span[1024];
static TLS uint8_t cvgbuf[1024];


static TLS int spans_ds;
static TLS int spans_dt;
static TLS int spans_dw;
static TLS int spans_dr;
static TLS int spans_dg;
static TLS int spans_db;
static TLS int spans_da;
static TLS int spans_dz;
static TLS int spans_dzpix;

static TLS int spans_drdy, spans_dgdy, spans_dbdy, spans_dady, spans_dzdy;
static TLS int spans_cdr, spans_cdg, spans_cdb, spans_cda, spans_cdz;

static TLS int spans_dsdy, spans_dtdy, spans_dwdy;



struct color
{
    int32_t r, g, b, a;
};

struct fbcolor
{
    uint8_t r, g, b;
};

struct rectangle
{
    uint16_t xl, yl, xh, yh;
};

struct tex_rectangle
{
    int tilenum;
    uint16_t xl, yl, xh, yh;
    int16_t s, t;
    int16_t dsdx, dtdy;
    uint32_t flip;
};

struct tile
{
    int format;
    int size;
    int line;
    int tmem;
    int palette;
    int ct, mt, cs, ms;
    int mask_t, shift_t, mask_s, shift_s;

    uint16_t sl, tl, sh, th;

    struct
    {
        int clampdiffs, clampdifft;
        int clampens, clampent;
        int masksclamped, masktclamped;
        int notlutswitch, tlutswitch;
    } f;
};

struct combine_modes
{
    int sub_a_rgb0;
    int sub_b_rgb0;
    int mul_rgb0;
    int add_rgb0;
    int sub_a_a0;
    int sub_b_a0;
    int mul_a0;
    int add_a0;

    int sub_a_rgb1;
    int sub_b_rgb1;
    int mul_rgb1;
    int add_rgb1;
    int sub_a_a1;
    int sub_b_a1;
    int mul_a1;
    int add_a1;
};

struct modederivs
{
    int stalederivs;
    int dolod;
    int partialreject_1cycle;
    int partialreject_2cycle;
    int special_bsel0;
    int special_bsel1;
    int rgb_alpha_dither;
    int realblendershiftersneeded;
    int interpixelblendershiftersneeded;
};

struct other_modes
{
    int cycle_type;
    int persp_tex_en;
    int detail_tex_en;
    int sharpen_tex_en;
    int tex_lod_en;
    int en_tlut;
    int tlut_type;
    int sample_type;
    int mid_texel;
    int bi_lerp0;
    int bi_lerp1;
    int convert_one;
    int key_en;
    int rgb_dither_sel;
    int alpha_dither_sel;
    int blend_m1a_0;
    int blend_m1a_1;
    int blend_m1b_0;
    int blend_m1b_1;
    int blend_m2a_0;
    int blend_m2a_1;
    int blend_m2b_0;
    int blend_m2b_1;
    int force_blend;
    int alpha_cvg_select;
    int cvg_times_alpha;
    int z_mode;
    int cvg_dest;
    int color_on_cvg;
    int image_read_en;
    int z_update_en;
    int z_compare_en;
    int antialias_en;
    int z_source_sel;
    int dither_alpha_en;
    int alpha_compare_en;
    struct modederivs f;
};



#define PIXEL_SIZE_4BIT         0
#define PIXEL_SIZE_8BIT         1
#define PIXEL_SIZE_16BIT        2
#define PIXEL_SIZE_32BIT        3

#define CYCLE_TYPE_1            0
#define CYCLE_TYPE_2            1
#define CYCLE_TYPE_COPY         2
#define CYCLE_TYPE_FILL         3


#define FORMAT_RGBA             0
#define FORMAT_YUV              1
#define FORMAT_CI               2
#define FORMAT_IA               3
#define FORMAT_I                4


#define TEXEL_RGBA4             0
#define TEXEL_RGBA8             1
#define TEXEL_RGBA16            2
#define TEXEL_RGBA32            3
#define TEXEL_YUV4              4
#define TEXEL_YUV8              5
#define TEXEL_YUV16             6
#define TEXEL_YUV32             7
#define TEXEL_CI4               8
#define TEXEL_CI8               9
#define TEXEL_CI16              0xa
#define TEXEL_CI32              0xb
#define TEXEL_IA4               0xc
#define TEXEL_IA8               0xd
#define TEXEL_IA16              0xe
#define TEXEL_IA32              0xf
#define TEXEL_I4                0x10
#define TEXEL_I8                0x11
#define TEXEL_I16               0x12
#define TEXEL_I32               0x13


#define CVG_CLAMP               0
#define CVG_WRAP                1
#define CVG_ZAP                 2
#define CVG_SAVE                3


#define ZMODE_OPAQUE            0
#define ZMODE_INTERPENETRATING  1
#define ZMODE_TRANSPARENT       2
#define ZMODE_DECAL             3

static TLS struct combine_modes combine;
static TLS struct other_modes other_modes;

static TLS struct color blend_color;
static TLS struct color prim_color;
static TLS struct color env_color;
static TLS struct color fog_color;
static TLS struct color combined_color;
static TLS struct color texel0_color;
static TLS struct color texel1_color;
static TLS struct color nexttexel_color;
static TLS struct color shade_color;
static TLS struct color key_scale;
static TLS struct color key_center;
static TLS struct color key_width;
static TLS int32_t noise = 0;
static TLS int32_t primitive_lod_frac = 0;
static int32_t one_color = 0x100;
static int32_t zero_color = 0x00;

static TLS int32_t keyalpha;

static int32_t blenderone   = 0xff;


static TLS int32_t *combiner_rgbsub_a_r[2];
static TLS int32_t *combiner_rgbsub_a_g[2];
static TLS int32_t *combiner_rgbsub_a_b[2];
static TLS int32_t *combiner_rgbsub_b_r[2];
static TLS int32_t *combiner_rgbsub_b_g[2];
static TLS int32_t *combiner_rgbsub_b_b[2];
static TLS int32_t *combiner_rgbmul_r[2];
static TLS int32_t *combiner_rgbmul_g[2];
static TLS int32_t *combiner_rgbmul_b[2];
static TLS int32_t *combiner_rgbadd_r[2];
static TLS int32_t *combiner_rgbadd_g[2];
static TLS int32_t *combiner_rgbadd_b[2];

static TLS int32_t *combiner_alphasub_a[2];
static TLS int32_t *combiner_alphasub_b[2];
static TLS int32_t *combiner_alphamul[2];
static TLS int32_t *combiner_alphaadd[2];


static TLS int32_t *blender1a_r[2];
static TLS int32_t *blender1a_g[2];
static TLS int32_t *blender1a_b[2];
static TLS int32_t *blender1b_a[2];
static TLS int32_t *blender2a_r[2];
static TLS int32_t *blender2a_g[2];
static TLS int32_t *blender2a_b[2];
static TLS int32_t *blender2b_a[2];

static TLS struct color pixel_color;
static TLS struct color inv_pixel_color;
static TLS struct color blended_pixel_color;
static TLS struct color memory_color;
static TLS struct color pre_memory_color;

static TLS uint32_t fill_color;

static TLS uint32_t primitive_z;
static TLS uint16_t primitive_delta_z;

static TLS int fb_format = FORMAT_RGBA;
static TLS int fb_size = PIXEL_SIZE_4BIT;
static TLS int fb_width = 0;
static TLS uint32_t fb_address = 0;

static TLS int ti_format = FORMAT_RGBA;
static TLS int ti_size = PIXEL_SIZE_4BIT;
static TLS int ti_width = 0;
static TLS uint32_t ti_address = 0;

static TLS uint32_t zb_address = 0;

static TLS struct tile tile[8];

static TLS struct rectangle clip = {0,0,0x2000,0x2000};
static TLS int scfield = 0;
static TLS int sckeepodd = 0;
static TLS int oldscyl = 0;

static TLS uint8_t tmem[0x1000];

#define tlut ((uint16_t*)(&tmem[0x800]))

#define PIXELS_TO_BYTES(pix, siz) (((pix) << (siz)) >> 1)

struct spansigs {
    int startspan;
    int endspan;
    int preendspan;
    int nextspan;
    int midspan;
    int longspan;
    int onelessthanmid;
};


static void rdp_set_other_modes(const uint32_t* args);
static INLINE void fetch_texel(struct color *color, int s, int t, uint32_t tilenum);
static INLINE void fetch_texel_entlut(struct color *color, int s, int t, uint32_t tilenum);
static INLINE void fetch_texel_quadro(struct color *color0, struct color *color1, struct color *color2, struct color *color3, int s0, int s1, int t0, int t1, uint32_t tilenum);
static INLINE void fetch_texel_entlut_quadro(struct color *color0, struct color *color1, struct color *color2, struct color *color3, int s0, int s1, int t0, int t1, uint32_t tilenum);
static void tile_tlut_common_cs_decoder(const uint32_t* args);
static void loading_pipeline(int start, int end, int tilenum, int coord_quad, int ltlut);
static void get_tmem_idx(int s, int t, uint32_t tilenum, uint32_t* idx0, uint32_t* idx1, uint32_t* idx2, uint32_t* idx3, uint32_t* bit3flipped, uint32_t* hibit);
static void sort_tmem_idx(uint32_t *idx, uint32_t idxa, uint32_t idxb, uint32_t idxc, uint32_t idxd, uint32_t bankno);
static void sort_tmem_shorts_lowhalf(uint32_t* bindshort, uint32_t short0, uint32_t short1, uint32_t short2, uint32_t short3, uint32_t bankno);
static void compute_color_index(uint32_t* cidx, uint32_t readshort, uint32_t nybbleoffset, uint32_t tilenum);
static void read_tmem_copy(int s, int s1, int s2, int s3, int t, uint32_t tilenum, uint32_t* sortshort, int* hibits, int* lowbits);
static void replicate_for_copy(uint32_t* outbyte, uint32_t inshort, uint32_t nybbleoffset, uint32_t tilenum, uint32_t tformat, uint32_t tsize);
static void fetch_qword_copy(uint32_t* hidword, uint32_t* lowdword, int32_t ssss, int32_t ssst, uint32_t tilenum);
static void render_spans_1cycle_complete(int start, int end, int tilenum, int flip);
static void render_spans_1cycle_notexel1(int start, int end, int tilenum, int flip);
static void render_spans_1cycle_notex(int start, int end, int tilenum, int flip);
static void render_spans_2cycle_complete(int start, int end, int tilenum, int flip);
static void render_spans_2cycle_notexelnext(int start, int end, int tilenum, int flip);
static void render_spans_2cycle_notexel1(int start, int end, int tilenum, int flip);
static void render_spans_2cycle_notex(int start, int end, int tilenum, int flip);
static void render_spans_fill(int start, int end, int flip);
static void render_spans_copy(int start, int end, int tilenum, int flip);
static STRICTINLINE void combiner_1cycle(int adseed, uint32_t* curpixel_cvg);
static STRICTINLINE void combiner_2cycle(int adseed, uint32_t* curpixel_cvg, int32_t* acalpha);
static STRICTINLINE int blender_1cycle(uint32_t* fr, uint32_t* fg, uint32_t* fb, int dith, uint32_t blend_en, uint32_t prewrap, uint32_t curpixel_cvg, uint32_t curpixel_cvbit);
static STRICTINLINE int blender_2cycle(uint32_t* fr, uint32_t* fg, uint32_t* fb, int dith, uint32_t blend_en, uint32_t prewrap, uint32_t curpixel_cvg, uint32_t curpixel_cvbit, int32_t acalpha);
static STRICTINLINE void texture_pipeline_cycle(struct color* TEX, struct color* prev, int32_t SSS, int32_t SST, uint32_t tilenum, uint32_t cycle);
static STRICTINLINE void tc_pipeline_copy(int32_t* sss0, int32_t* sss1, int32_t* sss2, int32_t* sss3, int32_t* sst, int tilenum);
static STRICTINLINE void tc_pipeline_load(int32_t* sss, int32_t* sst, int tilenum, int coord_quad);
static STRICTINLINE void tcclamp_generic(int32_t* S, int32_t* T, int32_t* SFRAC, int32_t* TFRAC, int32_t maxs, int32_t maxt, int32_t num);
static STRICTINLINE void tcclamp_cycle(int32_t* S, int32_t* T, int32_t* SFRAC, int32_t* TFRAC, int32_t maxs, int32_t maxt, int32_t num);
static STRICTINLINE void tcclamp_cycle_light(int32_t* S, int32_t* T, int32_t maxs, int32_t maxt, int32_t num);
static STRICTINLINE void tcshift_cycle(int32_t* S, int32_t* T, int32_t* maxs, int32_t* maxt, uint32_t num);
static STRICTINLINE void tcshift_copy(int32_t* S, int32_t* T, uint32_t num);
static INLINE void precalculate_everything(void);
static STRICTINLINE int alpha_compare(int32_t comb_alpha);
static STRICTINLINE int32_t color_combiner_equation(int32_t a, int32_t b, int32_t c, int32_t d);
static STRICTINLINE int32_t alpha_combiner_equation(int32_t a, int32_t b, int32_t c, int32_t d);
static STRICTINLINE void blender_equation_cycle0(int* r, int* g, int* b);
static STRICTINLINE void blender_equation_cycle0_2(int* r, int* g, int* b);
static STRICTINLINE void blender_equation_cycle1(int* r, int* g, int* b);
static STRICTINLINE uint32_t rightcvghex(uint32_t x, uint32_t fmask);
static STRICTINLINE uint32_t leftcvghex(uint32_t x, uint32_t fmask);
static STRICTINLINE void compute_cvg_noflip(int32_t scanline);
static STRICTINLINE void compute_cvg_flip(int32_t scanline);
static INLINE void fbwrite_4(uint32_t curpixel, uint32_t r, uint32_t g, uint32_t b, uint32_t blend_en, uint32_t curpixel_cvg, uint32_t curpixel_memcvg);
static INLINE void fbwrite_8(uint32_t curpixel, uint32_t r, uint32_t g, uint32_t b, uint32_t blend_en, uint32_t curpixel_cvg, uint32_t curpixel_memcvg);
static INLINE void fbwrite_16(uint32_t curpixel, uint32_t r, uint32_t g, uint32_t b, uint32_t blend_en, uint32_t curpixel_cvg, uint32_t curpixel_memcvg);
static INLINE void fbwrite_32(uint32_t curpixel, uint32_t r, uint32_t g, uint32_t b, uint32_t blend_en, uint32_t curpixel_cvg, uint32_t curpixel_memcvg);
static INLINE void fbfill_4(uint32_t curpixel);
static INLINE void fbfill_8(uint32_t curpixel);
static INLINE void fbfill_16(uint32_t curpixel);
static INLINE void fbfill_32(uint32_t curpixel);
static INLINE void fbread_4(uint32_t num, uint32_t* curpixel_memcvg);
static INLINE void fbread_8(uint32_t num, uint32_t* curpixel_memcvg);
static INLINE void fbread_16(uint32_t num, uint32_t* curpixel_memcvg);
static INLINE void fbread_32(uint32_t num, uint32_t* curpixel_memcvg);
static INLINE void fbread2_4(uint32_t num, uint32_t* curpixel_memcvg);
static INLINE void fbread2_8(uint32_t num, uint32_t* curpixel_memcvg);
static INLINE void fbread2_16(uint32_t num, uint32_t* curpixel_memcvg);
static INLINE void fbread2_32(uint32_t num, uint32_t* curpixel_memcvg);
static STRICTINLINE uint32_t z_decompress(uint32_t rawz);
static STRICTINLINE uint32_t dz_decompress(uint32_t compresseddz);
static STRICTINLINE uint32_t dz_compress(uint32_t value);
static INLINE void z_build_com_table(void);
static INLINE void precalc_cvmask_derivatives(void);
static STRICTINLINE uint16_t decompress_cvmask_frombyte(uint8_t byte);
static STRICTINLINE void lookup_cvmask_derivatives(uint32_t mask, uint8_t* offx, uint8_t* offy, uint32_t* curpixel_cvg, uint32_t* curpixel_cvbit);
static STRICTINLINE void z_store(uint32_t zcurpixel, uint32_t z, int dzpixenc);
static STRICTINLINE uint32_t z_compare(uint32_t zcurpixel, uint32_t sz, uint16_t dzpix, int dzpixenc, uint32_t* blend_en, uint32_t* prewrap, uint32_t* curpixel_cvg, uint32_t curpixel_memcvg);
static STRICTINLINE int finalize_spanalpha(uint32_t blend_en, uint32_t curpixel_cvg, uint32_t curpixel_memcvg);
static STRICTINLINE int32_t normalize_dzpix(int32_t sum);
static STRICTINLINE int32_t clamp(int32_t value,int32_t min,int32_t max);
static INLINE void tcdiv_persp(int32_t ss, int32_t st, int32_t sw, int32_t* sss, int32_t* sst);
static INLINE void tcdiv_nopersp(int32_t ss, int32_t st, int32_t sw, int32_t* sss, int32_t* sst);
static STRICTINLINE void tclod_4x17_to_15(int32_t scurr, int32_t snext, int32_t tcurr, int32_t tnext, int32_t previous, int32_t* lod);
static STRICTINLINE void tclod_tcclamp(int32_t* sss, int32_t* sst);
static STRICTINLINE void lodfrac_lodtile_signals(int lodclamp, int32_t lod, uint32_t* l_tile, uint32_t* magnify, uint32_t* distant, int32_t* lfdst);
static STRICTINLINE void tclod_1cycle_current(int32_t* sss, int32_t* sst, int32_t nexts, int32_t nextt, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t scanline, int32_t prim_tile, int32_t* t1, struct spansigs* sigs);
static STRICTINLINE void tclod_1cycle_current_simple(int32_t* sss, int32_t* sst, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t scanline, int32_t prim_tile, int32_t* t1, struct spansigs* sigs);
static STRICTINLINE void tclod_1cycle_next(int32_t* sss, int32_t* sst, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t scanline, int32_t prim_tile, int32_t* t1, struct spansigs* sigs, int32_t* prelodfrac);
static STRICTINLINE void tclod_2cycle_current(int32_t* sss, int32_t* sst, int32_t nexts, int32_t nextt, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t prim_tile, int32_t* t1, int32_t* t2);
static STRICTINLINE void tclod_2cycle_current_simple(int32_t* sss, int32_t* sst, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t prim_tile, int32_t* t1, int32_t* t2);
static STRICTINLINE void tclod_2cycle_current_notexel1(int32_t* sss, int32_t* sst, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t prim_tile, int32_t* t1);
static STRICTINLINE void tclod_2cycle_next(int32_t* sss, int32_t* sst, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t prim_tile, int32_t* t1, int32_t* t2, int32_t* prelodfrac);
static STRICTINLINE void tclod_copy(int32_t* sss, int32_t* sst, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t prim_tile, int32_t* t1);
static STRICTINLINE void get_texel1_1cycle(int32_t* s1, int32_t* t1, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t scanline, struct spansigs* sigs);
static STRICTINLINE void get_nexttexel0_2cycle(int32_t* s1, int32_t* t1, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc);
static INLINE void calculate_clamp_diffs(uint32_t tile);
static INLINE void calculate_tile_derivs(uint32_t tile);
static INLINE void rgb_dither_complete(int* r, int* g, int* b, int dith);
static INLINE void rgb_dither_nothing(int* r, int* g, int* b, int dith);
static INLINE void get_dither_noise_complete(int x, int y, int* cdith, int* adith);
static INLINE void get_dither_only(int x, int y, int* cdith, int* adith);
static INLINE void get_dither_nothing(int x, int y, int* cdith, int* adith);
static STRICTINLINE void rgbaz_correct_clip(int offx, int offy, int r, int g, int b, int a, int* z, uint32_t curpixel_cvg);
static void deduce_derivatives(void);
static STRICTINLINE int32_t irand();

static TLS int32_t k0_tf = 0, k1_tf = 0, k2_tf = 0, k3_tf = 0;
static TLS int32_t k4 = 0, k5 = 0;
static TLS int32_t lod_frac = 0;

static struct {uint32_t shift; uint32_t add;} z_dec_table[8] = {
     6, 0x00000,
     5, 0x20000,
     4, 0x30000,
     3, 0x38000,
     2, 0x3c000,
     1, 0x3e000,
     0, 0x3f000,
     0, 0x3f800,
};

static void (*fbread_func[4])(uint32_t, uint32_t*) =
{
    fbread_4, fbread_8, fbread_16, fbread_32
};

static void (*fbread2_func[4])(uint32_t, uint32_t*) =
{
    fbread2_4, fbread2_8, fbread2_16, fbread2_32
};

static void (*fbwrite_func[4])(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) =
{
    fbwrite_4, fbwrite_8, fbwrite_16, fbwrite_32
};

static void (*fbfill_func[4])(uint32_t) =
{
    fbfill_4, fbfill_8, fbfill_16, fbfill_32
};

static void (*get_dither_noise_func[3])(int, int, int*, int*) =
{
    get_dither_noise_complete, get_dither_only, get_dither_nothing
};

static void (*rgb_dither_func[2])(int*, int*, int*, int) =
{
    rgb_dither_complete, rgb_dither_nothing
};

static void (*tcdiv_func[2])(int32_t, int32_t, int32_t, int32_t*, int32_t*) =
{
    tcdiv_nopersp, tcdiv_persp
};

static void (*render_spans_1cycle_func[3])(int, int, int, int) =
{
    render_spans_1cycle_notex, render_spans_1cycle_notexel1, render_spans_1cycle_complete
};

static void (*render_spans_2cycle_func[4])(int, int, int, int) =
{
    render_spans_2cycle_notex, render_spans_2cycle_notexel1, render_spans_2cycle_notexelnext, render_spans_2cycle_complete
};

static TLS void (*fbread1_ptr)(uint32_t, uint32_t*);
static TLS void (*fbread2_ptr)(uint32_t, uint32_t*);
static TLS void (*fbwrite_ptr)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static TLS void (*fbfill_ptr)(uint32_t);
static TLS void (*get_dither_noise_ptr)(int, int, int*, int*);
static TLS void (*rgb_dither_ptr)(int*, int*, int*, int);
static TLS void (*tcdiv_ptr)(int32_t, int32_t, int32_t, int32_t*, int32_t*);
static TLS void (*render_spans_1cycle_ptr)(int, int, int, int);
static TLS void (*render_spans_2cycle_ptr)(int, int, int, int);

struct cvtcmaskderivative {
    uint8_t cvg;
    uint8_t cvbit;
    uint8_t xoff;
    uint8_t yoff;
};

static uint16_t z_com_table[0x40000];
static uint32_t z_complete_dec_table[0x4000];
static uint8_t replicated_rgba[32];
static int32_t maskbits_table[16];
static uint32_t special_9bit_clamptable[512];
static int32_t special_9bit_exttable[512];
static int32_t ge_two_table[128];
static int32_t log2table[256];
static int32_t tcdiv_table[0x8000];
static uint8_t bldiv_hwaccurate_table[0x8000];
static uint16_t deltaz_comparator_lut[0x10000];
static int32_t clamp_t_diff[8];
static int32_t clamp_s_diff[8];
static struct cvtcmaskderivative cvarray[0x100];

static struct
{
    int copymstrangecrashes, fillmcrashes, fillmbitcrashes, syncfullcrash;
} onetimewarnings;

static TLS uint32_t max_level = 0;
static TLS int32_t min_level = 0;
static int rdp_pipeline_crashed = 0;

static STRICTINLINE void tcmask(int32_t* S, int32_t* T, int32_t num);
static STRICTINLINE void tcmask(int32_t* S, int32_t* T, int32_t num)
{
    int32_t wrap;



    if (tile[num].mask_s)
    {
        if (tile[num].ms)
        {
            wrap = *S >> tile[num].f.masksclamped;
            wrap &= 1;
            *S ^= (-wrap);
        }
        *S &= maskbits_table[tile[num].mask_s];
    }

    if (tile[num].mask_t)
    {
        if (tile[num].mt)
        {
            wrap = *T >> tile[num].f.masktclamped;
            wrap &= 1;
            *T ^= (-wrap);
        }

        *T &= maskbits_table[tile[num].mask_t];
    }
}


static STRICTINLINE void tcmask_coupled(int32_t* S, int32_t* S1, int32_t* T, int32_t* T1, int32_t num);
static STRICTINLINE void tcmask_coupled(int32_t* S, int32_t* S1, int32_t* T, int32_t* T1, int32_t num)
{
    int32_t wrap;
    int32_t maskbits;
    int32_t wrapthreshold;


    if (tile[num].mask_s)
    {
        if (tile[num].ms)
        {
            wrapthreshold = tile[num].f.masksclamped;

            wrap = (*S >> wrapthreshold) & 1;
            *S ^= (-wrap);

            wrap = (*S1 >> wrapthreshold) & 1;
            *S1 ^= (-wrap);
        }

        maskbits = maskbits_table[tile[num].mask_s];
        *S &= maskbits;
        *S1 &= maskbits;
    }

    if (tile[num].mask_t)
    {
        if (tile[num].mt)
        {
            wrapthreshold = tile[num].f.masktclamped;

            wrap = (*T >> wrapthreshold) & 1;
            *T ^= (-wrap);

            wrap = (*T1 >> wrapthreshold) & 1;
            *T1 ^= (-wrap);
        }
        maskbits = maskbits_table[tile[num].mask_t];
        *T &= maskbits;
        *T1 &= maskbits;
    }
}


static STRICTINLINE void tcmask_copy(int32_t* S, int32_t* S1, int32_t* S2, int32_t* S3, int32_t* T, int32_t num);
static STRICTINLINE void tcmask_copy(int32_t* S, int32_t* S1, int32_t* S2, int32_t* S3, int32_t* T, int32_t num)
{
    int32_t wrap;
    int32_t maskbits_s;
    int32_t swrapthreshold;

    if (tile[num].mask_s)
    {
        if (tile[num].ms)
        {
            swrapthreshold = tile[num].f.masksclamped;

            wrap = (*S >> swrapthreshold) & 1;
            *S ^= (-wrap);

            wrap = (*S1 >> swrapthreshold) & 1;
            *S1 ^= (-wrap);

            wrap = (*S2 >> swrapthreshold) & 1;
            *S2 ^= (-wrap);

            wrap = (*S3 >> swrapthreshold) & 1;
            *S3 ^= (-wrap);
        }

        maskbits_s = maskbits_table[tile[num].mask_s];
        *S &= maskbits_s;
        *S1 &= maskbits_s;
        *S2 &= maskbits_s;
        *S3 &= maskbits_s;
    }

    if (tile[num].mask_t)
    {
        if (tile[num].mt)
        {
            wrap = *T >> tile[num].f.masktclamped;
            wrap &= 1;
            *T ^= (-wrap);
        }

        *T &= maskbits_table[tile[num].mask_t];
    }
}


static STRICTINLINE void tcshift_cycle(int32_t* S, int32_t* T, int32_t* maxs, int32_t* maxt, uint32_t num)
{



    int32_t coord = *S;
    int32_t shifter = tile[num].shift_s;


    if (shifter < 11)
    {
        coord = SIGN16(coord);
        coord >>= shifter;
    }
    else
    {
        coord <<= (16 - shifter);
        coord = SIGN16(coord);
    }
    *S = coord;




    *maxs = ((coord >> 3) >= tile[num].sh);



    coord = *T;
    shifter = tile[num].shift_t;

    if (shifter < 11)
    {
        coord = SIGN16(coord);
        coord >>= shifter;
    }
    else
    {
        coord <<= (16 - shifter);
        coord = SIGN16(coord);
    }
    *T = coord;
    *maxt = ((coord >> 3) >= tile[num].th);
}


static STRICTINLINE void tcshift_copy(int32_t* S, int32_t* T, uint32_t num)
{
    int32_t coord = *S;
    int32_t shifter = tile[num].shift_s;

    if (shifter < 11)
    {
        coord = SIGN16(coord);
        coord >>= shifter;
    }
    else
    {
        coord <<= (16 - shifter);
        coord = SIGN16(coord);
    }
    *S = coord;

    coord = *T;
    shifter = tile[num].shift_t;

    if (shifter < 11)
    {
        coord = SIGN16(coord);
        coord >>= shifter;
    }
    else
    {
        coord <<= (16 - shifter);
        coord = SIGN16(coord);
    }
    *T = coord;

}



static STRICTINLINE void tcclamp_cycle(int32_t* S, int32_t* T, int32_t* SFRAC, int32_t* TFRAC, int32_t maxs, int32_t maxt, int32_t num)
{



    int32_t locs = *S, loct = *T;
    if (tile[num].f.clampens)
    {

        if (maxs)
        {
            *S = tile[num].f.clampdiffs;
            *SFRAC = 0;
        }
        else if (!(locs & 0x10000))
            *S = locs >> 5;
        else
        {
            *S = 0;
            *SFRAC = 0;
        }
    }
    else
        *S = (locs >> 5);

    if (tile[num].f.clampent)
    {
        if (maxt)
        {
            *T = tile[num].f.clampdifft;
            *TFRAC = 0;
        }
        else if (!(loct & 0x10000))
            *T = loct >> 5;
        else
        {
            *T = 0;
            *TFRAC = 0;
        }
    }
    else
        *T = (loct >> 5);
}


static STRICTINLINE void tcclamp_cycle_light(int32_t* S, int32_t* T, int32_t maxs, int32_t maxt, int32_t num)
{
    int32_t locs = *S, loct = *T;
    if (tile[num].f.clampens)
    {
        if (maxs)
            *S = tile[num].f.clampdiffs;
        else if (!(locs & 0x10000))
            *S = locs >> 5;
        else
            *S = 0;
    }
    else
        *S = (locs >> 5);

    if (tile[num].f.clampent)
    {
        if (maxt)
            *T = tile[num].f.clampdifft;
        else if (!(loct & 0x10000))
            *T = loct >> 5;
        else
            *T = 0;
    }
    else
        *T = (loct >> 5);
}


int rdp_init(struct core_config* _config, struct plugin_api* _plugin)
{
    config = _config;
    plugin = _plugin;

    fbread1_ptr = fbread_func[0];
    fbread2_ptr = fbread2_func[0];
    fbwrite_ptr = fbwrite_func[0];
    fbfill_ptr = fbfill_func[0];
    get_dither_noise_ptr = get_dither_noise_func[0];
    rgb_dither_ptr = rgb_dither_func[0];
    tcdiv_ptr = tcdiv_func[0];
    render_spans_1cycle_ptr = render_spans_1cycle_func[2];
    render_spans_2cycle_ptr = render_spans_2cycle_func[1];

    combiner_rgbsub_a_r[0] = combiner_rgbsub_a_r[1] = &one_color;
    combiner_rgbsub_a_g[0] = combiner_rgbsub_a_g[1] = &one_color;
    combiner_rgbsub_a_b[0] = combiner_rgbsub_a_b[1] = &one_color;
    combiner_rgbsub_b_r[0] = combiner_rgbsub_b_r[1] = &one_color;
    combiner_rgbsub_b_g[0] = combiner_rgbsub_b_g[1] = &one_color;
    combiner_rgbsub_b_b[0] = combiner_rgbsub_b_b[1] = &one_color;
    combiner_rgbmul_r[0] = combiner_rgbmul_r[1] = &one_color;
    combiner_rgbmul_g[0] = combiner_rgbmul_g[1] = &one_color;
    combiner_rgbmul_b[0] = combiner_rgbmul_b[1] = &one_color;
    combiner_rgbadd_r[0] = combiner_rgbadd_r[1] = &one_color;
    combiner_rgbadd_g[0] = combiner_rgbadd_g[1] = &one_color;
    combiner_rgbadd_b[0] = combiner_rgbadd_b[1] = &one_color;

    combiner_alphasub_a[0] = combiner_alphasub_a[1] = &one_color;
    combiner_alphasub_b[0] = combiner_alphasub_b[1] = &one_color;
    combiner_alphamul[0] = combiner_alphamul[1] = &one_color;
    combiner_alphaadd[0] = combiner_alphaadd[1] = &one_color;

    uint32_t tmp[2] = {0};
    rdp_set_other_modes(tmp);
    other_modes.f.stalederivs = 1;

    memset(tmem, 0, 0x1000);

    memset(tile, 0, sizeof(tile));

    for (int i = 0; i < 8; i++)
    {
        calculate_tile_derivs(i);
        calculate_clamp_diffs(i);
    }

    memset(&combined_color, 0, sizeof(struct color));
    memset(&prim_color, 0, sizeof(struct color));
    memset(&env_color, 0, sizeof(struct color));
    memset(&key_scale, 0, sizeof(struct color));
    memset(&key_center, 0, sizeof(struct color));

    rdp_pipeline_crashed = 0;
    memset(&onetimewarnings, 0, sizeof(onetimewarnings));

    precalculate_everything();

    return 0;
}


static INLINE void set_suba_rgb_input(int32_t **input_r, int32_t **input_g, int32_t **input_b, int code)
{
    switch (code & 0xf)
    {
        case 0:     *input_r = &combined_color.r;   *input_g = &combined_color.g;   *input_b = &combined_color.b;   break;
        case 1:     *input_r = &texel0_color.r;     *input_g = &texel0_color.g;     *input_b = &texel0_color.b;     break;
        case 2:     *input_r = &texel1_color.r;     *input_g = &texel1_color.g;     *input_b = &texel1_color.b;     break;
        case 3:     *input_r = &prim_color.r;       *input_g = &prim_color.g;       *input_b = &prim_color.b;       break;
        case 4:     *input_r = &shade_color.r;      *input_g = &shade_color.g;      *input_b = &shade_color.b;      break;
        case 5:     *input_r = &env_color.r;        *input_g = &env_color.g;        *input_b = &env_color.b;        break;
        case 6:     *input_r = &one_color;          *input_g = &one_color;          *input_b = &one_color;      break;
        case 7:     *input_r = &noise;              *input_g = &noise;              *input_b = &noise;              break;
        case 8: case 9: case 10: case 11: case 12: case 13: case 14: case 15:
        {
            *input_r = &zero_color;     *input_g = &zero_color;     *input_b = &zero_color;     break;
        }
    }
}

static INLINE void set_subb_rgb_input(int32_t **input_r, int32_t **input_g, int32_t **input_b, int code)
{
    switch (code & 0xf)
    {
        case 0:     *input_r = &combined_color.r;   *input_g = &combined_color.g;   *input_b = &combined_color.b;   break;
        case 1:     *input_r = &texel0_color.r;     *input_g = &texel0_color.g;     *input_b = &texel0_color.b;     break;
        case 2:     *input_r = &texel1_color.r;     *input_g = &texel1_color.g;     *input_b = &texel1_color.b;     break;
        case 3:     *input_r = &prim_color.r;       *input_g = &prim_color.g;       *input_b = &prim_color.b;       break;
        case 4:     *input_r = &shade_color.r;      *input_g = &shade_color.g;      *input_b = &shade_color.b;      break;
        case 5:     *input_r = &env_color.r;        *input_g = &env_color.g;        *input_b = &env_color.b;        break;
        case 6:     *input_r = &key_center.r;       *input_g = &key_center.g;       *input_b = &key_center.b;       break;
        case 7:     *input_r = &k4;                 *input_g = &k4;                 *input_b = &k4;                 break;
        case 8: case 9: case 10: case 11: case 12: case 13: case 14: case 15:
        {
            *input_r = &zero_color;     *input_g = &zero_color;     *input_b = &zero_color;     break;
        }
    }
}

static INLINE void set_mul_rgb_input(int32_t **input_r, int32_t **input_g, int32_t **input_b, int code)
{
    switch (code & 0x1f)
    {
        case 0:     *input_r = &combined_color.r;   *input_g = &combined_color.g;   *input_b = &combined_color.b;   break;
        case 1:     *input_r = &texel0_color.r;     *input_g = &texel0_color.g;     *input_b = &texel0_color.b;     break;
        case 2:     *input_r = &texel1_color.r;     *input_g = &texel1_color.g;     *input_b = &texel1_color.b;     break;
        case 3:     *input_r = &prim_color.r;       *input_g = &prim_color.g;       *input_b = &prim_color.b;       break;
        case 4:     *input_r = &shade_color.r;      *input_g = &shade_color.g;      *input_b = &shade_color.b;      break;
        case 5:     *input_r = &env_color.r;        *input_g = &env_color.g;        *input_b = &env_color.b;        break;
        case 6:     *input_r = &key_scale.r;        *input_g = &key_scale.g;        *input_b = &key_scale.b;        break;
        case 7:     *input_r = &combined_color.a;   *input_g = &combined_color.a;   *input_b = &combined_color.a;   break;
        case 8:     *input_r = &texel0_color.a;     *input_g = &texel0_color.a;     *input_b = &texel0_color.a;     break;
        case 9:     *input_r = &texel1_color.a;     *input_g = &texel1_color.a;     *input_b = &texel1_color.a;     break;
        case 10:    *input_r = &prim_color.a;       *input_g = &prim_color.a;       *input_b = &prim_color.a;       break;
        case 11:    *input_r = &shade_color.a;      *input_g = &shade_color.a;      *input_b = &shade_color.a;      break;
        case 12:    *input_r = &env_color.a;        *input_g = &env_color.a;        *input_b = &env_color.a;        break;
        case 13:    *input_r = &lod_frac;           *input_g = &lod_frac;           *input_b = &lod_frac;           break;
        case 14:    *input_r = &primitive_lod_frac; *input_g = &primitive_lod_frac; *input_b = &primitive_lod_frac; break;
        case 15:    *input_r = &k5;                 *input_g = &k5;                 *input_b = &k5;                 break;
        case 16: case 17: case 18: case 19: case 20: case 21: case 22: case 23:
        case 24: case 25: case 26: case 27: case 28: case 29: case 30: case 31:
        {
            *input_r = &zero_color;     *input_g = &zero_color;     *input_b = &zero_color;     break;
        }
    }
}

static INLINE void set_add_rgb_input(int32_t **input_r, int32_t **input_g, int32_t **input_b, int code)
{
    switch (code & 0x7)
    {
        case 0:     *input_r = &combined_color.r;   *input_g = &combined_color.g;   *input_b = &combined_color.b;   break;
        case 1:     *input_r = &texel0_color.r;     *input_g = &texel0_color.g;     *input_b = &texel0_color.b;     break;
        case 2:     *input_r = &texel1_color.r;     *input_g = &texel1_color.g;     *input_b = &texel1_color.b;     break;
        case 3:     *input_r = &prim_color.r;       *input_g = &prim_color.g;       *input_b = &prim_color.b;       break;
        case 4:     *input_r = &shade_color.r;      *input_g = &shade_color.g;      *input_b = &shade_color.b;      break;
        case 5:     *input_r = &env_color.r;        *input_g = &env_color.g;        *input_b = &env_color.b;        break;
        case 6:     *input_r = &one_color;          *input_g = &one_color;          *input_b = &one_color;          break;
        case 7:     *input_r = &zero_color;         *input_g = &zero_color;         *input_b = &zero_color;         break;
    }
}

static INLINE void set_sub_alpha_input(int32_t **input, int code)
{
    switch (code & 0x7)
    {
        case 0:     *input = &combined_color.a; break;
        case 1:     *input = &texel0_color.a; break;
        case 2:     *input = &texel1_color.a; break;
        case 3:     *input = &prim_color.a; break;
        case 4:     *input = &shade_color.a; break;
        case 5:     *input = &env_color.a; break;
        case 6:     *input = &one_color; break;
        case 7:     *input = &zero_color; break;
    }
}

static INLINE void set_mul_alpha_input(int32_t **input, int code)
{
    switch (code & 0x7)
    {
        case 0:     *input = &lod_frac; break;
        case 1:     *input = &texel0_color.a; break;
        case 2:     *input = &texel1_color.a; break;
        case 3:     *input = &prim_color.a; break;
        case 4:     *input = &shade_color.a; break;
        case 5:     *input = &env_color.a; break;
        case 6:     *input = &primitive_lod_frac; break;
        case 7:     *input = &zero_color; break;
    }
}

static STRICTINLINE void combiner_1cycle(int adseed, uint32_t* curpixel_cvg)
{

    int32_t redkey, greenkey, bluekey, temp;
    struct color chromabypass;

    if (other_modes.key_en)
    {
        chromabypass.r = *combiner_rgbsub_a_r[1];
        chromabypass.g = *combiner_rgbsub_a_g[1];
        chromabypass.b = *combiner_rgbsub_a_b[1];
    }






    if (combiner_rgbmul_r[1] != &zero_color)
    {
















        combined_color.r = color_combiner_equation(*combiner_rgbsub_a_r[1],*combiner_rgbsub_b_r[1],*combiner_rgbmul_r[1],*combiner_rgbadd_r[1]);
        combined_color.g = color_combiner_equation(*combiner_rgbsub_a_g[1],*combiner_rgbsub_b_g[1],*combiner_rgbmul_g[1],*combiner_rgbadd_g[1]);
        combined_color.b = color_combiner_equation(*combiner_rgbsub_a_b[1],*combiner_rgbsub_b_b[1],*combiner_rgbmul_b[1],*combiner_rgbadd_b[1]);
    }
    else
    {
        combined_color.r = ((special_9bit_exttable[*combiner_rgbadd_r[1]] << 8) + 0x80) & 0x1ffff;
        combined_color.g = ((special_9bit_exttable[*combiner_rgbadd_g[1]] << 8) + 0x80) & 0x1ffff;
        combined_color.b = ((special_9bit_exttable[*combiner_rgbadd_b[1]] << 8) + 0x80) & 0x1ffff;
    }

    if (combiner_alphamul[1] != &zero_color)
        combined_color.a = alpha_combiner_equation(*combiner_alphasub_a[1],*combiner_alphasub_b[1],*combiner_alphamul[1],*combiner_alphaadd[1]);
    else
        combined_color.a = special_9bit_exttable[*combiner_alphaadd[1]] & 0x1ff;

    pixel_color.a = special_9bit_clamptable[combined_color.a];
    if (pixel_color.a == 0xff)
        pixel_color.a = 0x100;

    if (!other_modes.key_en)
    {

        combined_color.r >>= 8;
        combined_color.g >>= 8;
        combined_color.b >>= 8;
        pixel_color.r = special_9bit_clamptable[combined_color.r];
        pixel_color.g = special_9bit_clamptable[combined_color.g];
        pixel_color.b = special_9bit_clamptable[combined_color.b];
    }
    else
    {
        redkey = SIGN(combined_color.r, 17);
        if (redkey >= 0)
            redkey = (key_width.r << 4) - redkey;
        else
            redkey = (key_width.r << 4) + redkey;
        greenkey = SIGN(combined_color.g, 17);
        if (greenkey >= 0)
            greenkey = (key_width.g << 4) - greenkey;
        else
            greenkey = (key_width.g << 4) + greenkey;
        bluekey = SIGN(combined_color.b, 17);
        if (bluekey >= 0)
            bluekey = (key_width.b << 4) - bluekey;
        else
            bluekey = (key_width.b << 4) + bluekey;
        keyalpha = (redkey < greenkey) ? redkey : greenkey;
        keyalpha = (bluekey < keyalpha) ? bluekey : keyalpha;
        keyalpha = clamp(keyalpha, 0, 0xff);



        pixel_color.r = special_9bit_clamptable[chromabypass.r];
        pixel_color.g = special_9bit_clamptable[chromabypass.g];
        pixel_color.b = special_9bit_clamptable[chromabypass.b];


        combined_color.r >>= 8;
        combined_color.g >>= 8;
        combined_color.b >>= 8;
    }


    if (other_modes.cvg_times_alpha)
    {
        temp = (pixel_color.a * (*curpixel_cvg) + 4) >> 3;
        *curpixel_cvg = (temp >> 5) & 0xf;
    }

    if (!other_modes.alpha_cvg_select)
    {
        if (!other_modes.key_en)
        {
            pixel_color.a += adseed;
            if (pixel_color.a & 0x100)
                pixel_color.a = 0xff;
        }
        else
            pixel_color.a = keyalpha;
    }
    else
    {
        if (other_modes.cvg_times_alpha)
            pixel_color.a = temp;
        else
            pixel_color.a = (*curpixel_cvg) << 5;
        if (pixel_color.a > 0xff)
            pixel_color.a = 0xff;
    }

    shade_color.a += adseed;
    if (shade_color.a & 0x100)
        shade_color.a = 0xff;
}

static STRICTINLINE void combiner_2cycle(int adseed, uint32_t* curpixel_cvg, int32_t* acalpha)
{
    int32_t redkey, greenkey, bluekey, temp;
    struct color chromabypass;

    if (combiner_rgbmul_r[0] != &zero_color)
    {
        combined_color.r = color_combiner_equation(*combiner_rgbsub_a_r[0],*combiner_rgbsub_b_r[0],*combiner_rgbmul_r[0],*combiner_rgbadd_r[0]);
        combined_color.g = color_combiner_equation(*combiner_rgbsub_a_g[0],*combiner_rgbsub_b_g[0],*combiner_rgbmul_g[0],*combiner_rgbadd_g[0]);
        combined_color.b = color_combiner_equation(*combiner_rgbsub_a_b[0],*combiner_rgbsub_b_b[0],*combiner_rgbmul_b[0],*combiner_rgbadd_b[0]);
    }
    else
    {
        combined_color.r = ((special_9bit_exttable[*combiner_rgbadd_r[0]] << 8) + 0x80) & 0x1ffff;
        combined_color.g = ((special_9bit_exttable[*combiner_rgbadd_g[0]] << 8) + 0x80) & 0x1ffff;
        combined_color.b = ((special_9bit_exttable[*combiner_rgbadd_b[0]] << 8) + 0x80) & 0x1ffff;
    }

    if (combiner_alphamul[0] != &zero_color)
        combined_color.a = alpha_combiner_equation(*combiner_alphasub_a[0],*combiner_alphasub_b[0],*combiner_alphamul[0],*combiner_alphaadd[0]);
    else
        combined_color.a = special_9bit_exttable[*combiner_alphaadd[0]] & 0x1ff;



    if (other_modes.alpha_compare_en)
    {
        if (other_modes.key_en)
        {
            redkey = SIGN(combined_color.r, 17);
            if (redkey >= 0)
                redkey = (key_width.r << 4) - redkey;
            else
                redkey = (key_width.r << 4) + redkey;
            greenkey = SIGN(combined_color.g, 17);
            if (greenkey >= 0)
                greenkey = (key_width.g << 4) - greenkey;
            else
                greenkey = (key_width.g << 4) + greenkey;
            bluekey = SIGN(combined_color.b, 17);
            if (bluekey >= 0)
                bluekey = (key_width.b << 4) - bluekey;
            else
                bluekey = (key_width.b << 4) + bluekey;
            keyalpha = (redkey < greenkey) ? redkey : greenkey;
            keyalpha = (bluekey < keyalpha) ? bluekey : keyalpha;
            keyalpha = clamp(keyalpha, 0, 0xff);
        }

        int32_t preacalpha = special_9bit_clamptable[combined_color.a];
        if (preacalpha == 0xff)
            preacalpha = 0x100;

        if (other_modes.cvg_times_alpha)
            temp = (preacalpha * (*curpixel_cvg) + 4) >> 3;

        if (!other_modes.alpha_cvg_select)
        {
            if (!other_modes.key_en)
            {
                preacalpha += adseed;
                if (preacalpha & 0x100)
                    preacalpha = 0xff;
            }
            else
                preacalpha = keyalpha;
        }
        else
        {
            if (other_modes.cvg_times_alpha)
                preacalpha = temp;
            else
                preacalpha = (*curpixel_cvg) << 5;
            if (preacalpha > 0xff)
                preacalpha = 0xff;
        }

        *acalpha = preacalpha;
    }





    combined_color.r >>= 8;
    combined_color.g >>= 8;
    combined_color.b >>= 8;


    texel0_color = texel1_color;
    texel1_color = nexttexel_color;









    if (other_modes.key_en)
    {
        chromabypass.r = *combiner_rgbsub_a_r[1];
        chromabypass.g = *combiner_rgbsub_a_g[1];
        chromabypass.b = *combiner_rgbsub_a_b[1];
    }

    if (combiner_rgbmul_r[1] != &zero_color)
    {
        combined_color.r = color_combiner_equation(*combiner_rgbsub_a_r[1],*combiner_rgbsub_b_r[1],*combiner_rgbmul_r[1],*combiner_rgbadd_r[1]);
        combined_color.g = color_combiner_equation(*combiner_rgbsub_a_g[1],*combiner_rgbsub_b_g[1],*combiner_rgbmul_g[1],*combiner_rgbadd_g[1]);
        combined_color.b = color_combiner_equation(*combiner_rgbsub_a_b[1],*combiner_rgbsub_b_b[1],*combiner_rgbmul_b[1],*combiner_rgbadd_b[1]);
    }
    else
    {
        combined_color.r = ((special_9bit_exttable[*combiner_rgbadd_r[1]] << 8) + 0x80) & 0x1ffff;
        combined_color.g = ((special_9bit_exttable[*combiner_rgbadd_g[1]] << 8) + 0x80) & 0x1ffff;
        combined_color.b = ((special_9bit_exttable[*combiner_rgbadd_b[1]] << 8) + 0x80) & 0x1ffff;
    }

    if (combiner_alphamul[1] != &zero_color)
        combined_color.a = alpha_combiner_equation(*combiner_alphasub_a[1],*combiner_alphasub_b[1],*combiner_alphamul[1],*combiner_alphaadd[1]);
    else
        combined_color.a = special_9bit_exttable[*combiner_alphaadd[1]] & 0x1ff;

    if (!other_modes.key_en)
    {

        combined_color.r >>= 8;
        combined_color.g >>= 8;
        combined_color.b >>= 8;

        pixel_color.r = special_9bit_clamptable[combined_color.r];
        pixel_color.g = special_9bit_clamptable[combined_color.g];
        pixel_color.b = special_9bit_clamptable[combined_color.b];
    }
    else
    {
        redkey = SIGN(combined_color.r, 17);
        if (redkey >= 0)
            redkey = (key_width.r << 4) - redkey;
        else
            redkey = (key_width.r << 4) + redkey;
        greenkey = SIGN(combined_color.g, 17);
        if (greenkey >= 0)
            greenkey = (key_width.g << 4) - greenkey;
        else
            greenkey = (key_width.g << 4) + greenkey;
        bluekey = SIGN(combined_color.b, 17);
        if (bluekey >= 0)
            bluekey = (key_width.b << 4) - bluekey;
        else
            bluekey = (key_width.b << 4) + bluekey;
        keyalpha = (redkey < greenkey) ? redkey : greenkey;
        keyalpha = (bluekey < keyalpha) ? bluekey : keyalpha;
        keyalpha = clamp(keyalpha, 0, 0xff);



        pixel_color.r = special_9bit_clamptable[chromabypass.r];
        pixel_color.g = special_9bit_clamptable[chromabypass.g];
        pixel_color.b = special_9bit_clamptable[chromabypass.b];


        combined_color.r >>= 8;
        combined_color.g >>= 8;
        combined_color.b >>= 8;
    }

    pixel_color.a = special_9bit_clamptable[combined_color.a];
    if (pixel_color.a == 0xff)
        pixel_color.a = 0x100;


    if (other_modes.cvg_times_alpha)
    {
        temp = (pixel_color.a * (*curpixel_cvg) + 4) >> 3;

        *curpixel_cvg = (temp >> 5) & 0xf;


    }

    if (!other_modes.alpha_cvg_select)
    {
        if (!other_modes.key_en)
        {
            pixel_color.a += adseed;
            if (pixel_color.a & 0x100)
                pixel_color.a = 0xff;
        }
        else
            pixel_color.a = keyalpha;
    }
    else
    {
        if (other_modes.cvg_times_alpha)
            pixel_color.a = temp;
        else
            pixel_color.a = (*curpixel_cvg) << 5;
        if (pixel_color.a > 0xff)
            pixel_color.a = 0xff;
    }

    shade_color.a += adseed;
    if (shade_color.a & 0x100)
        shade_color.a = 0xff;
}

static INLINE void precalculate_everything(void)
{
    int i = 0, k = 0, j = 0;






    z_build_com_table();




    uint32_t exponent;
    uint32_t mantissa;
    for (i = 0; i < 0x4000; i++)
    {
        exponent = (i >> 11) & 7;
        mantissa = i & 0x7ff;
        z_complete_dec_table[i] = ((mantissa << z_dec_table[exponent].shift) + z_dec_table[exponent].add) & 0x3ffff;
    }



    precalc_cvmask_derivatives();



    i = 0;
    log2table[0] = log2table[1] = 0;
    for (i = 2; i < 256; i++)
    {
        for (k = 7; k > 0; k--)
        {
            if((i >> k) & 1)
            {
                log2table[i] = k;
                break;
            }
        }
    }










    for (i = 0; i < 32; i++)
        replicated_rgba[i] = (i << 3) | ((i >> 2) & 7);



    maskbits_table[0] = 0x3ff;
    for (i = 1; i < 16; i++)
        maskbits_table[i] = ((uint16_t)(0xffff) >> (16 - i)) & 0x3ff;



    for(i = 0; i < 0x200; i++)
    {
        switch((i >> 7) & 3)
        {
        case 0:
        case 1:
            special_9bit_clamptable[i] = i & 0xff;
            break;
        case 2:
            special_9bit_clamptable[i] = 0xff;
            break;
        case 3:
            special_9bit_clamptable[i] = 0;
            break;
        }
    }



    for(i = 0; i < 0x200; i++)
    {
        special_9bit_exttable[i] = ((i & 0x180) == 0x180) ? (i | ~0x1ff) : (i & 0x1ff);
    }







    int temppoint, tempslope;
    int normout;
    int wnorm;
    int shift, tlu_rcp;

    for (i = 0; i < 0x8000; i++)
    {
        for (k = 1; k <= 14 && !((i << k) & 0x8000); k++)
            ;
        shift = k - 1;
        normout = (i << shift) & 0x3fff;
        wnorm = (normout & 0xff) << 2;
        normout >>= 8;



        temppoint = norm_point_table[normout];
        tempslope = norm_slope_table[normout];

        tempslope = (tempslope | ~0x3ff) + 1;

        tlu_rcp = (((tempslope * wnorm) >> 10) + temppoint) & 0x7fff;

        tcdiv_table[i] = shift | (tlu_rcp << 4);
    }


    int d = 0, n = 0, temp = 0, res = 0, invd = 0, nbit = 0;
    int ps[9];
    for (i = 0; i < 0x8000; i++)
    {
        res = 0;
        d = (i >> 11) & 0xf;
        n = i & 0x7ff;
        invd = (~d) & 0xf;


        temp = invd + (n >> 8) + 1;
        ps[0] = temp & 7;
        for (k = 0; k < 8; k++)
        {
            nbit = (n >> (7 - k)) & 1;
            if (res & (0x100 >> k))
                temp = invd + (ps[k] << 1) + nbit + 1;
            else
                temp = d + (ps[k] << 1) + nbit;
            ps[k + 1] = temp & 7;
            if (temp & 0x10)
                res |= (1 << (7 - k));
        }
        bldiv_hwaccurate_table[i] = res;
    }







    deltaz_comparator_lut[0] = 0;
    for (i = 1; i < 0x10000; i++)
    {
        for (k = 15; k >= 0; k--)
        {
            if (i & (1 << k))
            {
                deltaz_comparator_lut[i] = 1 << k;
                break;
            }
        }
    }

}

static INLINE void set_blender_input(int cycle, int which, int32_t **input_r, int32_t **input_g, int32_t **input_b, int32_t **input_a, int a, int b)
{

    switch (a & 0x3)
    {
        case 0:
        {
            if (cycle == 0)
            {
                *input_r = &pixel_color.r;
                *input_g = &pixel_color.g;
                *input_b = &pixel_color.b;
            }
            else
            {
                *input_r = &blended_pixel_color.r;
                *input_g = &blended_pixel_color.g;
                *input_b = &blended_pixel_color.b;
            }
            break;
        }

        case 1:
        {
            *input_r = &memory_color.r;
            *input_g = &memory_color.g;
            *input_b = &memory_color.b;
            break;
        }

        case 2:
        {
            *input_r = &blend_color.r;      *input_g = &blend_color.g;      *input_b = &blend_color.b;
            break;
        }

        case 3:
        {
            *input_r = &fog_color.r;        *input_g = &fog_color.g;        *input_b = &fog_color.b;
            break;
        }
    }

    if (which == 0)
    {
        switch (b & 0x3)
        {
            case 0:     *input_a = &pixel_color.a; break;
            case 1:     *input_a = &fog_color.a; break;
            case 2:     *input_a = &shade_color.a; break;
            case 3:     *input_a = &zero_color; break;
        }
    }
    else
    {
        switch (b & 0x3)
        {
            case 0:     *input_a = &inv_pixel_color.a; break;
            case 1:     *input_a = &memory_color.a; break;
            case 2:     *input_a = &blenderone; break;
            case 3:     *input_a = &zero_color; break;
        }
    }
}



static const uint8_t bayer_matrix[16] =
{
     0,  4,  1, 5,
     4,  0,  5, 1,
     3,  7,  2, 6,
     7,  3,  6, 2
};


static const uint8_t magic_matrix[16] =
{
     0,  6,  1, 7,
     4,  2,  5, 3,
     3,  5,  2, 4,
     7,  1,  6, 0
};

static STRICTINLINE int blender_1cycle(uint32_t* fr, uint32_t* fg, uint32_t* fb, int dith, uint32_t blend_en, uint32_t prewrap, uint32_t curpixel_cvg, uint32_t curpixel_cvbit)
{
    int r, g, b, dontblend;


    if (alpha_compare(pixel_color.a))
    {






        if (other_modes.antialias_en ? (curpixel_cvg) : (curpixel_cvbit))
        {

            if (!other_modes.color_on_cvg || prewrap)
            {
                dontblend = (other_modes.f.partialreject_1cycle && pixel_color.a >= 0xff);
                if (!blend_en || dontblend)
                {
                    r = *blender1a_r[0];
                    g = *blender1a_g[0];
                    b = *blender1a_b[0];
                }
                else
                {
                    inv_pixel_color.a =  (~(*blender1b_a[0])) & 0xff;





                    blender_equation_cycle0(&r, &g, &b);
                }
            }
            else
            {
                r = *blender2a_r[0];
                g = *blender2a_g[0];
                b = *blender2a_b[0];
            }

            rgb_dither_ptr(&r, &g, &b, dith);
            *fr = r;
            *fg = g;
            *fb = b;
            return 1;
        }
        else
            return 0;
        }
    else
        return 0;
}

static STRICTINLINE int blender_2cycle(uint32_t* fr, uint32_t* fg, uint32_t* fb, int dith, uint32_t blend_en, uint32_t prewrap, uint32_t curpixel_cvg, uint32_t curpixel_cvbit, int32_t acalpha)
{
    int r, g, b, dontblend;


    if (alpha_compare(acalpha))
    {
        if (other_modes.antialias_en ? (curpixel_cvg) : (curpixel_cvbit))
        {

            inv_pixel_color.a =  (~(*blender1b_a[0])) & 0xff;
            blender_equation_cycle0_2(&r, &g, &b);


            memory_color = pre_memory_color;

            blended_pixel_color.r = r;
            blended_pixel_color.g = g;
            blended_pixel_color.b = b;
            blended_pixel_color.a = pixel_color.a;

            if (!other_modes.color_on_cvg || prewrap)
            {
                dontblend = (other_modes.f.partialreject_2cycle && pixel_color.a >= 0xff);
                if (!blend_en || dontblend)
                {
                    r = *blender1a_r[1];
                    g = *blender1a_g[1];
                    b = *blender1a_b[1];
                }
                else
                {
                    inv_pixel_color.a =  (~(*blender1b_a[1])) & 0xff;
                    blender_equation_cycle1(&r, &g, &b);
                }
            }
            else
            {
                r = *blender2a_r[1];
                g = *blender2a_g[1];
                b = *blender2a_b[1];
            }


            rgb_dither_ptr(&r, &g, &b, dith);
            *fr = r;
            *fg = g;
            *fb = b;
            return 1;
        }
        else
        {
            memory_color = pre_memory_color;
            return 0;
        }
    }
    else
    {
        memory_color = pre_memory_color;
        return 0;
    }
}



static INLINE void fetch_texel(struct color *color, int s, int t, uint32_t tilenum)
{
    uint32_t tbase = tile[tilenum].line * (t & 0xff) + tile[tilenum].tmem;



    uint32_t tpal   = tile[tilenum].palette;








    uint16_t *tc16 = (uint16_t*)tmem;
    uint32_t taddr = 0;





    switch (tile[tilenum].f.notlutswitch)
    {
    case TEXEL_RGBA4:
        {
            taddr = ((tbase << 4) + s) >> 1;
            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);
            uint8_t byteval, c;

            byteval = tmem[taddr & 0xfff];
            c = ((s & 1)) ? (byteval & 0xf) : (byteval >> 4);
            c |= (c << 4);
            color->r = c;
            color->g = c;
            color->b = c;
            color->a = c;
        }
        break;
    case TEXEL_RGBA8:
        {
            taddr = (tbase << 3) + s;
            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);

            uint8_t p;

            p = tmem[taddr & 0xfff];
            color->r = p;
            color->g = p;
            color->b = p;
            color->a = p;
        }
        break;
    case TEXEL_RGBA16:
        {
            taddr = (tbase << 2) + s;
            taddr ^= ((t & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR);


            uint16_t c;

            c = tc16[taddr & 0x7ff];
            color->r = GET_HI_RGBA16_TMEM(c);
            color->g = GET_MED_RGBA16_TMEM(c);
            color->b = GET_LOW_RGBA16_TMEM(c);
            color->a = (c & 1) ? 0xff : 0;
        }
        break;
    case TEXEL_RGBA32:
        {



            taddr = (tbase << 2) + s;
            taddr ^= ((t & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR);

            uint16_t c;


            taddr &= 0x3ff;
            c = tc16[taddr];
            color->r = c >> 8;
            color->g = c & 0xff;
            c = tc16[taddr | 0x400];
            color->b = c >> 8;
            color->a = c & 0xff;
        }
        break;
    case TEXEL_YUV4:
    case TEXEL_YUV8:
        {
            taddr = (tbase << 3) + s;

            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);

            int32_t u, save;

            save = u = tmem[taddr & 0x7ff];

            u = u - 0x80;

            color->r = u;
            color->g = u;
            color->b = save;
            color->a = save;
        }
        break;
    case TEXEL_YUV16:
    case TEXEL_YUV32:
        {
            taddr = (tbase << 3) + s;
            int taddrlow = taddr >> 1;

            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);
            taddrlow ^= ((t & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR);

            taddr &= 0x7ff;
            taddrlow &= 0x3ff;

            uint16_t c = tc16[taddrlow];

            int32_t y, u, v;
            y = tmem[taddr | 0x800];
            u = c >> 8;
            v = c & 0xff;

            u = u - 0x80;
            v = v - 0x80;



            color->r = u;
            color->g = v;
            color->b = y;
            color->a = y;
        }
        break;
    case TEXEL_CI4:
        {
            taddr = ((tbase << 4) + s) >> 1;
            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);

            uint8_t p;



            p = tmem[taddr & 0xfff];
            p = (s & 1) ? (p & 0xf) : (p >> 4);
            p = (tpal << 4) | p;
            color->r = color->g = color->b = color->a = p;
        }
        break;
    case TEXEL_CI8:
        {
            taddr = (tbase << 3) + s;
            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);

            uint8_t p;


            p = tmem[taddr & 0xfff];
            color->r = p;
            color->g = p;
            color->b = p;
            color->a = p;
        }
        break;
    case TEXEL_CI16:
        {
            taddr = (tbase << 2) + s;
            taddr ^= ((t & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR);

            uint16_t c;

            c = tc16[taddr & 0x7ff];
            color->r = c >> 8;
            color->g = c & 0xff;
            color->b = color->r;
            color->a = (c & 1) ? 0xff : 0;
        }
        break;
    case TEXEL_CI32:
        {
            taddr = (tbase << 2) + s;
            taddr ^= ((t & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR);

            uint16_t c;

            c = tc16[taddr & 0x7ff];
            color->r = c >> 8;
            color->g = c & 0xff;
            color->b = color->r;
            color->a = (c & 1) ? 0xff : 0;

        }
        break;
    case TEXEL_IA4:
        {
            taddr = ((tbase << 4) + s) >> 1;
            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);

            uint8_t p, i;


            p = tmem[taddr & 0xfff];
            p = (s & 1) ? (p & 0xf) : (p >> 4);
            i = p & 0xe;
            i = (i << 4) | (i << 1) | (i >> 2);
            color->r = i;
            color->g = i;
            color->b = i;
            color->a = (p & 0x1) ? 0xff : 0;
        }
        break;
    case TEXEL_IA8:
        {
            taddr = (tbase << 3) + s;
            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);

            uint8_t p, i;


            p = tmem[taddr & 0xfff];
            i = p & 0xf0;
            i |= (i >> 4);
            color->r = i;
            color->g = i;
            color->b = i;
            color->a = ((p & 0xf) << 4) | (p & 0xf);
        }
        break;
    case TEXEL_IA16:
        {


            taddr = (tbase << 2) + s;
            taddr ^= ((t & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR);

            uint16_t c;

            c = tc16[taddr & 0x7ff];
            color->r = color->g = color->b = (c >> 8);
            color->a = c & 0xff;
        }
        break;
    case TEXEL_IA32:
        {
            taddr = (tbase << 2) + s;
            taddr ^= ((t & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR);

            uint16_t c;

            c = tc16[taddr & 0x7ff];
            color->r = c >> 8;
            color->g = c & 0xff;
            color->b = color->r;
            color->a = (c & 1) ? 0xff : 0;
        }
        break;
    case TEXEL_I4:
        {
            taddr = ((tbase << 4) + s) >> 1;
            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);

            uint8_t byteval, c;

            byteval = tmem[taddr & 0xfff];
            c = (s & 1) ? (byteval & 0xf) : (byteval >> 4);
            c |= (c << 4);
            color->r = c;
            color->g = c;
            color->b = c;
            color->a = c;
        }
        break;
    case TEXEL_I8:
        {
            taddr = (tbase << 3) + s;
            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);

            uint8_t c;

            c = tmem[taddr & 0xfff];
            color->r = c;
            color->g = c;
            color->b = c;
            color->a = c;
        }
        break;
    case TEXEL_I16:
        {
            taddr = (tbase << 2) + s;
            taddr ^= ((t & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR);

            uint16_t c;

            c = tc16[taddr & 0x7ff];
            color->r = c >> 8;
            color->g = c & 0xff;
            color->b = color->r;
            color->a = (c & 1) ? 0xff : 0;
        }
        break;
    case TEXEL_I32:
        {
            taddr = (tbase << 2) + s;
            taddr ^= ((t & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR);

            uint16_t c;

            c = tc16[taddr & 0x7ff];
            color->r = c >> 8;
            color->g = c & 0xff;
            color->b = color->r;
            color->a = (c & 1) ? 0xff : 0;
        }
        break;
    default:
        msg_error("fetch_texel: unknown texture format %d, size %d, tilenum %d\n", tile[tilenum].format, tile[tilenum].size, tilenum);
        break;
    }
}

static INLINE void fetch_texel_entlut(struct color *color, int s, int t, uint32_t tilenum)
{
    uint32_t tbase = tile[tilenum].line * (t & 0xff) + tile[tilenum].tmem;
    uint32_t tpal   = tile[tilenum].palette << 4;
    uint16_t *tc16 = (uint16_t*)tmem;
    uint32_t taddr = 0;
    uint32_t c;



    switch(tile[tilenum].f.tlutswitch)
    {
    case 0:
    case 1:
    case 2:
        {
            taddr = ((tbase << 4) + s) >> 1;
            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);
            c = tmem[taddr & 0x7ff];
            c = (s & 1) ? (c & 0xf) : (c >> 4);
            c = tlut[((tpal | c) << 2) ^ WORD_ADDR_XOR];
        }
        break;
    case 3:
        {
            taddr = (tbase << 3) + s;
            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);
            c = tmem[taddr & 0x7ff];
            c = (s & 1) ? (c & 0xf) : (c >> 4);
            c = tlut[((tpal | c) << 2) ^ WORD_ADDR_XOR];
        }
        break;
    case 4:
    case 5:
    case 6:
    case 7:
        {
            taddr = (tbase << 3) + s;
            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);
            c = tmem[taddr & 0x7ff];
            c = tlut[(c << 2) ^ WORD_ADDR_XOR];
        }
        break;
    case 8:
    case 9:
    case 10:
        {
            taddr = (tbase << 2) + s;
            taddr ^= ((t & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR);
            c = tc16[taddr & 0x3ff];
            c = tlut[((c >> 6) & ~3) ^ WORD_ADDR_XOR];

        }
        break;
    case 11:
        {
            taddr = (tbase << 3) + s;
            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);
            c = tmem[taddr & 0x7ff];
            c = tlut[(c << 2) ^ WORD_ADDR_XOR];
        }
        break;
    case 12:
    case 13:
    case 14:
        {
            taddr = (tbase << 2) + s;
            taddr ^= ((t & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR);
            c = tc16[taddr & 0x3ff];
            c = tlut[((c >> 6) & ~3) ^ WORD_ADDR_XOR];
        }
        break;
    case 15:
        {
            taddr = (tbase << 3) + s;
            taddr ^= ((t & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR);
            c = tmem[taddr & 0x7ff];
            c = tlut[(c << 2) ^ WORD_ADDR_XOR];
        }
        break;
    default:
        msg_error("fetch_texel_entlut: unknown texture format %d, size %d, tilenum %d\n", tile[tilenum].format, tile[tilenum].size, tilenum);
        break;
    }

    if (!other_modes.tlut_type)
    {
        color->r = GET_HI_RGBA16_TMEM(c);
        color->g = GET_MED_RGBA16_TMEM(c);
        color->b = GET_LOW_RGBA16_TMEM(c);
        color->a = (c & 1) ? 0xff : 0;
    }
    else
    {
        color->r = color->g = color->b = c >> 8;
        color->a = c & 0xff;
    }

}



static INLINE void fetch_texel_quadro(struct color *color0, struct color *color1, struct color *color2, struct color *color3, int s0, int s1, int t0, int t1, uint32_t tilenum)
{

    uint32_t tbase0 = tile[tilenum].line * (t0 & 0xff) + tile[tilenum].tmem;
    uint32_t tbase2 = tile[tilenum].line * (t1 & 0xff) + tile[tilenum].tmem;
    uint32_t tpal   = tile[tilenum].palette;
    uint32_t xort = 0, ands = 0;




    uint16_t *tc16 = (uint16_t*)tmem;
    uint32_t taddr0 = 0, taddr1 = 0, taddr2 = 0, taddr3 = 0;
    uint32_t taddrlow0 = 0, taddrlow1 = 0, taddrlow2 = 0, taddrlow3 = 0;

    switch (tile[tilenum].f.notlutswitch)
    {
    case TEXEL_RGBA4:
        {
            taddr0 = ((tbase0 << 4) + s0) >> 1;
            taddr1 = ((tbase0 << 4) + s1) >> 1;
            taddr2 = ((tbase2 << 4) + s0) >> 1;
            taddr3 = ((tbase2 << 4) + s1) >> 1;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint32_t byteval, c;

            taddr0 &= 0xfff;
            taddr1 &= 0xfff;
            taddr2 &= 0xfff;
            taddr3 &= 0xfff;
            ands = s0 & 1;
            byteval = tmem[taddr0];
            c = (ands) ? (byteval & 0xf) : (byteval >> 4);
            c |= (c << 4);
            color0->r = c;
            color0->g = c;
            color0->b = c;
            color0->a = c;
            byteval = tmem[taddr2];
            c = (ands) ? (byteval & 0xf) : (byteval >> 4);
            c |= (c << 4);
            color2->r = c;
            color2->g = c;
            color2->b = c;
            color2->a = c;

            ands = s1 & 1;
            byteval = tmem[taddr1];
            c = (ands) ? (byteval & 0xf) : (byteval >> 4);
            c |= (c << 4);
            color1->r = c;
            color1->g = c;
            color1->b = c;
            color1->a = c;
            byteval = tmem[taddr3];
            c = (ands) ? (byteval & 0xf) : (byteval >> 4);
            c |= (c << 4);
            color3->r = c;
            color3->g = c;
            color3->b = c;
            color3->a = c;
        }
        break;
    case TEXEL_RGBA8:
        {
            taddr0 = ((tbase0 << 3) + s0);
            taddr1 = ((tbase0 << 3) + s1);
            taddr2 = ((tbase2 << 3) + s0);
            taddr3 = ((tbase2 << 3) + s1);
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint32_t p;

            taddr0 &= 0xfff;
            taddr1 &= 0xfff;
            taddr2 &= 0xfff;
            taddr3 &= 0xfff;
            p = tmem[taddr0];
            color0->r = p;
            color0->g = p;
            color0->b = p;
            color0->a = p;
            p = tmem[taddr2];
            color2->r = p;
            color2->g = p;
            color2->b = p;
            color2->a = p;
            p = tmem[taddr1];
            color1->r = p;
            color1->g = p;
            color1->b = p;
            color1->a = p;
            p = tmem[taddr3];
            color3->r = p;
            color3->g = p;
            color3->b = p;
            color3->a = p;
        }
        break;
    case TEXEL_RGBA16:
        {
            taddr0 = ((tbase0 << 2) + s0);
            taddr1 = ((tbase0 << 2) + s1);
            taddr2 = ((tbase2 << 2) + s0);
            taddr3 = ((tbase2 << 2) + s1);
            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint32_t c0, c1, c2, c3;

            taddr0 &= 0x7ff;
            taddr1 &= 0x7ff;
            taddr2 &= 0x7ff;
            taddr3 &= 0x7ff;
            c0 = tc16[taddr0];
            c1 = tc16[taddr1];
            c2 = tc16[taddr2];
            c3 = tc16[taddr3];
            color0->r = GET_HI_RGBA16_TMEM(c0);
            color0->g = GET_MED_RGBA16_TMEM(c0);
            color0->b = GET_LOW_RGBA16_TMEM(c0);
            color0->a = (c0 & 1) ? 0xff : 0;
            color1->r = GET_HI_RGBA16_TMEM(c1);
            color1->g = GET_MED_RGBA16_TMEM(c1);
            color1->b = GET_LOW_RGBA16_TMEM(c1);
            color1->a = (c1 & 1) ? 0xff : 0;
            color2->r = GET_HI_RGBA16_TMEM(c2);
            color2->g = GET_MED_RGBA16_TMEM(c2);
            color2->b = GET_LOW_RGBA16_TMEM(c2);
            color2->a = (c2 & 1) ? 0xff : 0;
            color3->r = GET_HI_RGBA16_TMEM(c3);
            color3->g = GET_MED_RGBA16_TMEM(c3);
            color3->b = GET_LOW_RGBA16_TMEM(c3);
            color3->a = (c3 & 1) ? 0xff : 0;
        }
        break;
    case TEXEL_RGBA32:
        {
            taddr0 = ((tbase0 << 2) + s0);
            taddr1 = ((tbase0 << 2) + s1);
            taddr2 = ((tbase2 << 2) + s0);
            taddr3 = ((tbase2 << 2) + s1);
            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint16_t c0, c1, c2, c3;

            taddr0 &= 0x3ff;
            taddr1 &= 0x3ff;
            taddr2 &= 0x3ff;
            taddr3 &= 0x3ff;
            c0 = tc16[taddr0];
            color0->r = c0 >> 8;
            color0->g = c0 & 0xff;
            c0 = tc16[taddr0 | 0x400];
            color0->b = c0 >>  8;
            color0->a = c0 & 0xff;
            c1 = tc16[taddr1];
            color1->r = c1 >> 8;
            color1->g = c1 & 0xff;
            c1 = tc16[taddr1 | 0x400];
            color1->b = c1 >>  8;
            color1->a = c1 & 0xff;
            c2 = tc16[taddr2];
            color2->r = c2 >> 8;
            color2->g = c2 & 0xff;
            c2 = tc16[taddr2 | 0x400];
            color2->b = c2 >>  8;
            color2->a = c2 & 0xff;
            c3 = tc16[taddr3];
            color3->r = c3 >> 8;
            color3->g = c3 & 0xff;
            c3 = tc16[taddr3 | 0x400];
            color3->b = c3 >>  8;
            color3->a = c3 & 0xff;
        }
        break;
    case TEXEL_YUV4:
    case TEXEL_YUV8:
        {
            taddr0 = (tbase0 << 3) + s0;
            taddr1 = (tbase0 << 3) + s1;
            taddr2 = (tbase2 << 3) + s0;
            taddr3 = (tbase2 << 3) + s1;

            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            int32_t u0, u1, u2, u3, save0, save1, save2, save3;

            save0 = u0 = tmem[taddr0 & 0x7ff];
            u0 = u0 - 0x80;
            save1 = u1 = tmem[taddr1 & 0x7ff];
            u1 = u1 - 0x80;
            save2 = u2 = tmem[taddr2 & 0x7ff];
            u2 = u2 - 0x80;
            save3 = u3 = tmem[taddr3 & 0x7ff];
            u3 = u3 - 0x80;

            color0->r = u0;
            color0->g = u0;
            color0->b = save0;
            color0->a = save0;
            color1->r = u1;
            color1->g = u1;
            color1->b = save1;
            color1->a = save1;
            color2->r = u2;
            color2->g = u2;
            color2->b = save2;
            color2->a = save2;
            color3->r = u3;
            color3->g = u3;
            color3->b = save3;
            color3->a = save3;
        }
        break;
    case TEXEL_YUV16:
    case TEXEL_YUV32:
        {
            taddr0 = ((tbase0 << 3) + s0);
            taddr1 = ((tbase0 << 3) + s1);
            taddr2 = ((tbase2 << 3) + s0);
            taddr3 = ((tbase2 << 3) + s1);
            taddrlow0 = taddr0 >> 1;
            taddrlow1 = taddr1 >> 1;
            taddrlow2 = taddr2 >> 1;
            taddrlow3 = taddr3 >> 1;

            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;
            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddrlow0 ^= xort;
            taddrlow1 ^= xort;
            xort = (t1 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddrlow2 ^= xort;
            taddrlow3 ^= xort;

            taddr0 &= 0x7ff;
            taddr1 &= 0x7ff;
            taddr2 &= 0x7ff;
            taddr3 &= 0x7ff;
            taddrlow0 &= 0x3ff;
            taddrlow1 &= 0x3ff;
            taddrlow2 &= 0x3ff;
            taddrlow3 &= 0x3ff;

            uint16_t c0, c1, c2, c3;
            int32_t y0, y1, y2, y3, u0, u1, u2, u3, v0, v1, v2, v3;

            c0 = tc16[taddrlow0];
            c1 = tc16[taddrlow1];
            c2 = tc16[taddrlow2];
            c3 = tc16[taddrlow3];

            y0 = tmem[taddr0 | 0x800];
            u0 = c0 >> 8;
            v0 = c0 & 0xff;
            y1 = tmem[taddr1 | 0x800];
            u1 = c1 >> 8;
            v1 = c1 & 0xff;
            y2 = tmem[taddr2 | 0x800];
            u2 = c2 >> 8;
            v2 = c2 & 0xff;
            y3 = tmem[taddr3 | 0x800];
            u3 = c3 >> 8;
            v3 = c3 & 0xff;

            u0 = u0 - 0x80;
            v0 = v0 - 0x80;
            u1 = u1 - 0x80;
            v1 = v1 - 0x80;
            u2 = u2 - 0x80;
            v2 = v2 - 0x80;
            u3 = u3 - 0x80;
            v3 = v3 - 0x80;

            color0->r = u0;
            color0->g = v0;
            color0->b = y0;
            color0->a = y0;
            color1->r = u1;
            color1->g = v1;
            color1->b = y1;
            color1->a = y1;
            color2->r = u2;
            color2->g = v2;
            color2->b = y2;
            color2->a = y2;
            color3->r = u3;
            color3->g = v3;
            color3->b = y3;
            color3->a = y3;
        }
        break;
    case TEXEL_CI4:
        {
            taddr0 = ((tbase0 << 4) + s0) >> 1;
            taddr1 = ((tbase0 << 4) + s1) >> 1;
            taddr2 = ((tbase2 << 4) + s0) >> 1;
            taddr3 = ((tbase2 << 4) + s1) >> 1;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint32_t p;

            taddr0 &= 0xfff;
            taddr1 &= 0xfff;
            taddr2 &= 0xfff;
            taddr3 &= 0xfff;
            ands = s0 & 1;
            p = tmem[taddr0];
            p = (ands) ? (p & 0xf) : (p >> 4);
            p = (tpal << 4) | p;
            color0->r = color0->g = color0->b = color0->a = p;
            p = tmem[taddr2];
            p = (ands) ? (p & 0xf) : (p >> 4);
            p = (tpal << 4) | p;
            color2->r = color2->g = color2->b = color2->a = p;

            ands = s1 & 1;
            p = tmem[taddr1];
            p = (ands) ? (p & 0xf) : (p >> 4);
            p = (tpal << 4) | p;
            color1->r = color1->g = color1->b = color1->a = p;
            p = tmem[taddr3];
            p = (ands) ? (p & 0xf) : (p >> 4);
            p = (tpal << 4) | p;
            color3->r = color3->g = color3->b = color3->a = p;
        }
        break;
    case TEXEL_CI8:
        {
            taddr0 = ((tbase0 << 3) + s0);
            taddr1 = ((tbase0 << 3) + s1);
            taddr2 = ((tbase2 << 3) + s0);
            taddr3 = ((tbase2 << 3) + s1);
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint32_t p;

            taddr0 &= 0xfff;
            taddr1 &= 0xfff;
            taddr2 &= 0xfff;
            taddr3 &= 0xfff;
            p = tmem[taddr0];
            color0->r = p;
            color0->g = p;
            color0->b = p;
            color0->a = p;
            p = tmem[taddr2];
            color2->r = p;
            color2->g = p;
            color2->b = p;
            color2->a = p;
            p = tmem[taddr1];
            color1->r = p;
            color1->g = p;
            color1->b = p;
            color1->a = p;
            p = tmem[taddr3];
            color3->r = p;
            color3->g = p;
            color3->b = p;
            color3->a = p;
        }
        break;
    case TEXEL_CI16:
        {
            taddr0 = ((tbase0 << 2) + s0);
            taddr1 = ((tbase0 << 2) + s1);
            taddr2 = ((tbase2 << 2) + s0);
            taddr3 = ((tbase2 << 2) + s1);
            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint16_t c0, c1, c2, c3;

            taddr0 &= 0x7ff;
            taddr1 &= 0x7ff;
            taddr2 &= 0x7ff;
            taddr3 &= 0x7ff;
            c0 = tc16[taddr0];
            color0->r = c0 >> 8;
            color0->g = c0 & 0xff;
            color0->b = c0 >> 8;
            color0->a = (c0 & 1) ? 0xff : 0;
            c1 = tc16[taddr1];
            color1->r = c1 >> 8;
            color1->g = c1 & 0xff;
            color1->b = c1 >> 8;
            color1->a = (c1 & 1) ? 0xff : 0;
            c2 = tc16[taddr2];
            color2->r = c2 >> 8;
            color2->g = c2 & 0xff;
            color2->b = c2 >> 8;
            color2->a = (c2 & 1) ? 0xff : 0;
            c3 = tc16[taddr3];
            color3->r = c3 >> 8;
            color3->g = c3 & 0xff;
            color3->b = c3 >> 8;
            color3->a = (c3 & 1) ? 0xff : 0;

        }
        break;
    case TEXEL_CI32:
        {
            taddr0 = ((tbase0 << 2) + s0);
            taddr1 = ((tbase0 << 2) + s1);
            taddr2 = ((tbase2 << 2) + s0);
            taddr3 = ((tbase2 << 2) + s1);
            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint16_t c0, c1, c2, c3;

            taddr0 &= 0x7ff;
            taddr1 &= 0x7ff;
            taddr2 &= 0x7ff;
            taddr3 &= 0x7ff;
            c0 = tc16[taddr0];
            color0->r = c0 >> 8;
            color0->g = c0 & 0xff;
            color0->b = c0 >> 8;
            color0->a = (c0 & 1) ? 0xff : 0;
            c1 = tc16[taddr1];
            color1->r = c1 >> 8;
            color1->g = c1 & 0xff;
            color1->b = c1 >> 8;
            color1->a = (c1 & 1) ? 0xff : 0;
            c2 = tc16[taddr2];
            color2->r = c2 >> 8;
            color2->g = c2 & 0xff;
            color2->b = c2 >> 8;
            color2->a = (c2 & 1) ? 0xff : 0;
            c3 = tc16[taddr3];
            color3->r = c3 >> 8;
            color3->g = c3 & 0xff;
            color3->b = c3 >> 8;
            color3->a = (c3 & 1) ? 0xff : 0;

        }
        break;
    case TEXEL_IA4:
        {
            taddr0 = ((tbase0 << 4) + s0) >> 1;
            taddr1 = ((tbase0 << 4) + s1) >> 1;
            taddr2 = ((tbase2 << 4) + s0) >> 1;
            taddr3 = ((tbase2 << 4) + s1) >> 1;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint32_t p, i;

            taddr0 &= 0xfff;
            taddr1 &= 0xfff;
            taddr2 &= 0xfff;
            taddr3 &= 0xfff;
            ands = s0 & 1;
            p = tmem[taddr0];
            p = ands ? (p & 0xf) : (p >> 4);
            i = p & 0xe;
            i = (i << 4) | (i << 1) | (i >> 2);
            color0->r = i;
            color0->g = i;
            color0->b = i;
            color0->a = (p & 0x1) ? 0xff : 0;
            p = tmem[taddr2];
            p = ands ? (p & 0xf) : (p >> 4);
            i = p & 0xe;
            i = (i << 4) | (i << 1) | (i >> 2);
            color2->r = i;
            color2->g = i;
            color2->b = i;
            color2->a = (p & 0x1) ? 0xff : 0;

            ands = s1 & 1;
            p = tmem[taddr1];
            p = ands ? (p & 0xf) : (p >> 4);
            i = p & 0xe;
            i = (i << 4) | (i << 1) | (i >> 2);
            color1->r = i;
            color1->g = i;
            color1->b = i;
            color1->a = (p & 0x1) ? 0xff : 0;
            p = tmem[taddr3];
            p = ands ? (p & 0xf) : (p >> 4);
            i = p & 0xe;
            i = (i << 4) | (i << 1) | (i >> 2);
            color3->r = i;
            color3->g = i;
            color3->b = i;
            color3->a = (p & 0x1) ? 0xff : 0;

        }
        break;
    case TEXEL_IA8:
        {
            taddr0 = ((tbase0 << 3) + s0);
            taddr1 = ((tbase0 << 3) + s1);
            taddr2 = ((tbase2 << 3) + s0);
            taddr3 = ((tbase2 << 3) + s1);
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint32_t p, i;

            taddr0 &= 0xfff;
            taddr1 &= 0xfff;
            taddr2 &= 0xfff;
            taddr3 &= 0xfff;
            p = tmem[taddr0];
            i = p & 0xf0;
            i |= (i >> 4);
            color0->r = i;
            color0->g = i;
            color0->b = i;
            color0->a = ((p & 0xf) << 4) | (p & 0xf);
            p = tmem[taddr1];
            i = p & 0xf0;
            i |= (i >> 4);
            color1->r = i;
            color1->g = i;
            color1->b = i;
            color1->a = ((p & 0xf) << 4) | (p & 0xf);
            p = tmem[taddr2];
            i = p & 0xf0;
            i |= (i >> 4);
            color2->r = i;
            color2->g = i;
            color2->b = i;
            color2->a = ((p & 0xf) << 4) | (p & 0xf);
            p = tmem[taddr3];
            i = p & 0xf0;
            i |= (i >> 4);
            color3->r = i;
            color3->g = i;
            color3->b = i;
            color3->a = ((p & 0xf) << 4) | (p & 0xf);


        }
        break;
    case TEXEL_IA16:
        {
            taddr0 = ((tbase0 << 2) + s0);
            taddr1 = ((tbase0 << 2) + s1);
            taddr2 = ((tbase2 << 2) + s0);
            taddr3 = ((tbase2 << 2) + s1);
            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint16_t c0, c1, c2, c3;

            taddr0 &= 0x7ff;
            taddr1 &= 0x7ff;
            taddr2 &= 0x7ff;
            taddr3 &= 0x7ff;
            c0 = tc16[taddr0];
            color0->r = color0->g = color0->b = c0 >> 8;
            color0->a = c0 & 0xff;
            c1 = tc16[taddr1];
            color1->r = color1->g = color1->b = c1 >> 8;
            color1->a = c1 & 0xff;
            c2 = tc16[taddr2];
            color2->r = color2->g = color2->b = c2 >> 8;
            color2->a = c2 & 0xff;
            c3 = tc16[taddr3];
            color3->r = color3->g = color3->b = c3 >> 8;
            color3->a = c3 & 0xff;

        }
        break;
    case TEXEL_IA32:
        {
            taddr0 = ((tbase0 << 2) + s0);
            taddr1 = ((tbase0 << 2) + s1);
            taddr2 = ((tbase2 << 2) + s0);
            taddr3 = ((tbase2 << 2) + s1);
            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint16_t c0, c1, c2, c3;

            taddr0 &= 0x7ff;
            taddr1 &= 0x7ff;
            taddr2 &= 0x7ff;
            taddr3 &= 0x7ff;
            c0 = tc16[taddr0];
            color0->r = c0 >> 8;
            color0->g = c0 & 0xff;
            color0->b = c0 >> 8;
            color0->a = (c0 & 1) ? 0xff : 0;
            c1 = tc16[taddr1];
            color1->r = c1 >> 8;
            color1->g = c1 & 0xff;
            color1->b = c1 >> 8;
            color1->a = (c1 & 1) ? 0xff : 0;
            c2 = tc16[taddr2];
            color2->r = c2 >> 8;
            color2->g = c2 & 0xff;
            color2->b = c2 >> 8;
            color2->a = (c2 & 1) ? 0xff : 0;
            c3 = tc16[taddr3];
            color3->r = c3 >> 8;
            color3->g = c3 & 0xff;
            color3->b = c3 >> 8;
            color3->a = (c3 & 1) ? 0xff : 0;

        }
        break;
    case TEXEL_I4:
        {
            taddr0 = ((tbase0 << 4) + s0) >> 1;
            taddr1 = ((tbase0 << 4) + s1) >> 1;
            taddr2 = ((tbase2 << 4) + s0) >> 1;
            taddr3 = ((tbase2 << 4) + s1) >> 1;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint32_t p, c0, c1, c2, c3;

            taddr0 &= 0xfff;
            taddr1 &= 0xfff;
            taddr2 &= 0xfff;
            taddr3 &= 0xfff;
            ands = s0 & 1;
            p = tmem[taddr0];
            c0 = ands ? (p & 0xf) : (p >> 4);
            c0 |= (c0 << 4);
            color0->r = color0->g = color0->b = color0->a = c0;
            p = tmem[taddr2];
            c2 = ands ? (p & 0xf) : (p >> 4);
            c2 |= (c2 << 4);
            color2->r = color2->g = color2->b = color2->a = c2;

            ands = s1 & 1;
            p = tmem[taddr1];
            c1 = ands ? (p & 0xf) : (p >> 4);
            c1 |= (c1 << 4);
            color1->r = color1->g = color1->b = color1->a = c1;
            p = tmem[taddr3];
            c3 = ands ? (p & 0xf) : (p >> 4);
            c3 |= (c3 << 4);
            color3->r = color3->g = color3->b = color3->a = c3;

        }
        break;
    case TEXEL_I8:
        {
            taddr0 = ((tbase0 << 3) + s0);
            taddr1 = ((tbase0 << 3) + s1);
            taddr2 = ((tbase2 << 3) + s0);
            taddr3 = ((tbase2 << 3) + s1);
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint32_t p;

            taddr0 &= 0xfff;
            taddr1 &= 0xfff;
            taddr2 &= 0xfff;
            taddr3 &= 0xfff;
            p = tmem[taddr0];
            color0->r = p;
            color0->g = p;
            color0->b = p;
            color0->a = p;
            p = tmem[taddr1];
            color1->r = p;
            color1->g = p;
            color1->b = p;
            color1->a = p;
            p = tmem[taddr2];
            color2->r = p;
            color2->g = p;
            color2->b = p;
            color2->a = p;
            p = tmem[taddr3];
            color3->r = p;
            color3->g = p;
            color3->b = p;
            color3->a = p;
        }
        break;
    case TEXEL_I16:
        {
            taddr0 = ((tbase0 << 2) + s0);
            taddr1 = ((tbase0 << 2) + s1);
            taddr2 = ((tbase2 << 2) + s0);
            taddr3 = ((tbase2 << 2) + s1);
            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint16_t c0, c1, c2, c3;

            taddr0 &= 0x7ff;
            taddr1 &= 0x7ff;
            taddr2 &= 0x7ff;
            taddr3 &= 0x7ff;
            c0 = tc16[taddr0];
            color0->r = c0 >> 8;
            color0->g = c0 & 0xff;
            color0->b = c0 >> 8;
            color0->a = (c0 & 1) ? 0xff : 0;
            c1 = tc16[taddr1];
            color1->r = c1 >> 8;
            color1->g = c1 & 0xff;
            color1->b = c1 >> 8;
            color1->a = (c1 & 1) ? 0xff : 0;
            c2 = tc16[taddr2];
            color2->r = c2 >> 8;
            color2->g = c2 & 0xff;
            color2->b = c2 >> 8;
            color2->a = (c2 & 1) ? 0xff : 0;
            c3 = tc16[taddr3];
            color3->r = c3 >> 8;
            color3->g = c3 & 0xff;
            color3->b = c3 >> 8;
            color3->a = (c3 & 1) ? 0xff : 0;
        }
        break;
    case TEXEL_I32:
        {
            taddr0 = ((tbase0 << 2) + s0);
            taddr1 = ((tbase0 << 2) + s1);
            taddr2 = ((tbase2 << 2) + s0);
            taddr3 = ((tbase2 << 2) + s1);
            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            uint16_t c0, c1, c2, c3;

            taddr0 &= 0x7ff;
            taddr1 &= 0x7ff;
            taddr2 &= 0x7ff;
            taddr3 &= 0x7ff;
            c0 = tc16[taddr0];
            color0->r = c0 >> 8;
            color0->g = c0 & 0xff;
            color0->b = c0 >> 8;
            color0->a = (c0 & 1) ? 0xff : 0;
            c1 = tc16[taddr1];
            color1->r = c1 >> 8;
            color1->g = c1 & 0xff;
            color1->b = c1 >> 8;
            color1->a = (c1 & 1) ? 0xff : 0;
            c2 = tc16[taddr2];
            color2->r = c2 >> 8;
            color2->g = c2 & 0xff;
            color2->b = c2 >> 8;
            color2->a = (c2 & 1) ? 0xff : 0;
            c3 = tc16[taddr3];
            color3->r = c3 >> 8;
            color3->g = c3 & 0xff;
            color3->b = c3 >> 8;
            color3->a = (c3 & 1) ? 0xff : 0;
        }
        break;
    default:
        msg_error("fetch_texel_quadro: unknown texture format %d, size %d, tilenum %d\n", tile[tilenum].format, tile[tilenum].size, tilenum);
        break;
    }
}

static INLINE void fetch_texel_entlut_quadro(struct color *color0, struct color *color1, struct color *color2, struct color *color3, int s0, int s1, int t0, int t1, uint32_t tilenum)
{
    uint32_t tbase0 = tile[tilenum].line * (t0 & 0xff) + tile[tilenum].tmem;
    uint32_t tbase2 = tile[tilenum].line * (t1 & 0xff) + tile[tilenum].tmem;
    uint32_t tpal   = tile[tilenum].palette << 4;
    uint32_t xort = 0, ands = 0;

    uint16_t *tc16 = (uint16_t*)tmem;
    uint32_t taddr0 = 0, taddr1 = 0, taddr2 = 0, taddr3 = 0;
    uint16_t c0, c1, c2, c3;



    switch(tile[tilenum].f.tlutswitch)
    {
    case 0:
    case 1:
    case 2:
        {
            taddr0 = ((tbase0 << 4) + s0) >> 1;
            taddr1 = ((tbase0 << 4) + s1) >> 1;
            taddr2 = ((tbase2 << 4) + s0) >> 1;
            taddr3 = ((tbase2 << 4) + s1) >> 1;
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            ands = s0 & 1;
            c0 = tmem[taddr0 & 0x7ff];
            c0 = (ands) ? (c0 & 0xf) : (c0 >> 4);
            c0 = tlut[((tpal | c0) << 2) ^ WORD_ADDR_XOR];
            c2 = tmem[taddr2 & 0x7ff];
            c2 = (ands) ? (c2 & 0xf) : (c2 >> 4);
            c2 = tlut[((tpal | c2) << 2) ^ WORD_ADDR_XOR];

            ands = s1 & 1;
            c1 = tmem[taddr1 & 0x7ff];
            c1 = (ands) ? (c1 & 0xf) : (c1 >> 4);
            c1 = tlut[((tpal | c1) << 2) ^ WORD_ADDR_XOR];
            c3 = tmem[taddr3 & 0x7ff];
            c3 = (ands) ? (c3 & 0xf) : (c3 >> 4);
            c3 = tlut[((tpal | c3) << 2) ^ WORD_ADDR_XOR];
        }
        break;
    case 3:
        {
            taddr0 = ((tbase0 << 3) + s0);
            taddr1 = ((tbase0 << 3) + s1);
            taddr2 = ((tbase2 << 3) + s0);
            taddr3 = ((tbase2 << 3) + s1);
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            ands = s0 & 1;
            c0 = tmem[taddr0 & 0x7ff];
            c0 = (ands) ? (c0 & 0xf) : (c0 >> 4);
            c0 = tlut[((tpal | c0) << 2) ^ WORD_ADDR_XOR];
            c2 = tmem[taddr2 & 0x7ff];
            c2 = (ands) ? (c2 & 0xf) : (c2 >> 4);
            c2 = tlut[((tpal | c2) << 2) ^ WORD_ADDR_XOR];

            ands = s1 & 1;
            c1 = tmem[taddr1 & 0x7ff];
            c1 = (ands) ? (c1 & 0xf) : (c1 >> 4);
            c1 = tlut[((tpal | c1) << 2) ^ WORD_ADDR_XOR];
            c3 = tmem[taddr3 & 0x7ff];
            c3 = (ands) ? (c3 & 0xf) : (c3 >> 4);
            c3 = tlut[((tpal | c3) << 2) ^ WORD_ADDR_XOR];
        }
        break;
    case 4:
    case 5:
    case 6:
    case 7:
        {
            taddr0 = ((tbase0 << 3) + s0);
            taddr1 = ((tbase0 << 3) + s1);
            taddr2 = ((tbase2 << 3) + s0);
            taddr3 = ((tbase2 << 3) + s1);
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            c0 = tmem[taddr0 & 0x7ff];
            c0 = tlut[(c0 << 2) ^ WORD_ADDR_XOR];
            c2 = tmem[taddr2 & 0x7ff];
            c2 = tlut[(c2 << 2) ^ WORD_ADDR_XOR];
            c1 = tmem[taddr1 & 0x7ff];
            c1 = tlut[(c1 << 2) ^ WORD_ADDR_XOR];
            c3 = tmem[taddr3 & 0x7ff];
            c3 = tlut[(c3 << 2) ^ WORD_ADDR_XOR];
        }
        break;
    case 8:
    case 9:
    case 10:
        {
            taddr0 = ((tbase0 << 2) + s0);
            taddr1 = ((tbase0 << 2) + s1);
            taddr2 = ((tbase2 << 2) + s0);
            taddr3 = ((tbase2 << 2) + s1);
            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            c0 = tc16[taddr0 & 0x3ff];
            c0 = tlut[((c0 >> 6) & ~3) ^ WORD_ADDR_XOR];
            c1 = tc16[taddr1 & 0x3ff];
            c1 = tlut[((c1 >> 6) & ~3) ^ WORD_ADDR_XOR];
            c2 = tc16[taddr2 & 0x3ff];
            c2 = tlut[((c2 >> 6) & ~3) ^ WORD_ADDR_XOR];
            c3 = tc16[taddr3 & 0x3ff];
            c3 = tlut[((c3 >> 6) & ~3) ^ WORD_ADDR_XOR];
        }
        break;
    case 11:
        {
            taddr0 = ((tbase0 << 3) + s0);
            taddr1 = ((tbase0 << 3) + s1);
            taddr2 = ((tbase2 << 3) + s0);
            taddr3 = ((tbase2 << 3) + s1);
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            c0 = tmem[taddr0 & 0x7ff];
            c0 = tlut[(c0 << 2) ^ WORD_ADDR_XOR];
            c2 = tmem[taddr2 & 0x7ff];
            c2 = tlut[(c2 << 2) ^ WORD_ADDR_XOR];
            c1 = tmem[taddr1 & 0x7ff];
            c1 = tlut[(c1 << 2) ^ WORD_ADDR_XOR];
            c3 = tmem[taddr3 & 0x7ff];
            c3 = tlut[(c3 << 2) ^ WORD_ADDR_XOR];
        }
        break;
    case 12:
    case 13:
    case 14:
        {
            taddr0 = ((tbase0 << 2) + s0);
            taddr1 = ((tbase0 << 2) + s1);
            taddr2 = ((tbase2 << 2) + s0);
            taddr3 = ((tbase2 << 2) + s1);
            xort = (t0 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? WORD_XOR_DWORD_SWAP : WORD_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            c0 = tc16[taddr0 & 0x3ff];
            c0 = tlut[((c0 >> 6) & ~3) ^ WORD_ADDR_XOR];
            c1 = tc16[taddr1 & 0x3ff];
            c1 = tlut[((c1 >> 6) & ~3) ^ WORD_ADDR_XOR];
            c2 = tc16[taddr2 & 0x3ff];
            c2 = tlut[((c2 >> 6) & ~3) ^ WORD_ADDR_XOR];
            c3 = tc16[taddr3 & 0x3ff];
            c3 = tlut[((c3 >> 6) & ~3) ^ WORD_ADDR_XOR];
        }
        break;
    case 15:
        {
            taddr0 = ((tbase0 << 3) + s0);
            taddr1 = ((tbase0 << 3) + s1);
            taddr2 = ((tbase2 << 3) + s0);
            taddr3 = ((tbase2 << 3) + s1);
            xort = (t0 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr0 ^= xort;
            taddr1 ^= xort;
            xort = (t1 & 1) ? BYTE_XOR_DWORD_SWAP : BYTE_ADDR_XOR;
            taddr2 ^= xort;
            taddr3 ^= xort;

            c0 = tmem[taddr0 & 0x7ff];
            c0 = tlut[(c0 << 2) ^ WORD_ADDR_XOR];
            c2 = tmem[taddr2 & 0x7ff];
            c2 = tlut[(c2 << 2) ^ WORD_ADDR_XOR];
            c1 = tmem[taddr1 & 0x7ff];
            c1 = tlut[(c1 << 2) ^ WORD_ADDR_XOR];
            c3 = tmem[taddr3 & 0x7ff];
            c3 = tlut[(c3 << 2) ^ WORD_ADDR_XOR];
        }
        break;
    default:
        msg_error("fetch_texel_entlut_quadro: unknown texture format %d, size %d, tilenum %d\n", tile[tilenum].format, tile[tilenum].size, tilenum);
        break;
    }

    if (!other_modes.tlut_type)
    {
        color0->r = GET_HI_RGBA16_TMEM(c0);
        color0->g = GET_MED_RGBA16_TMEM(c0);
        color0->b = GET_LOW_RGBA16_TMEM(c0);
        color0->a = (c0 & 1) ? 0xff : 0;
        color1->r = GET_HI_RGBA16_TMEM(c1);
        color1->g = GET_MED_RGBA16_TMEM(c1);
        color1->b = GET_LOW_RGBA16_TMEM(c1);
        color1->a = (c1 & 1) ? 0xff : 0;
        color2->r = GET_HI_RGBA16_TMEM(c2);
        color2->g = GET_MED_RGBA16_TMEM(c2);
        color2->b = GET_LOW_RGBA16_TMEM(c2);
        color2->a = (c2 & 1) ? 0xff : 0;
        color3->r = GET_HI_RGBA16_TMEM(c3);
        color3->g = GET_MED_RGBA16_TMEM(c3);
        color3->b = GET_LOW_RGBA16_TMEM(c3);
        color3->a = (c3 & 1) ? 0xff : 0;
    }
    else
    {
        color0->r = color0->g = color0->b = c0 >> 8;
        color0->a = c0 & 0xff;
        color1->r = color1->g = color1->b = c1 >> 8;
        color1->a = c1 & 0xff;
        color2->r = color2->g = color2->b = c2 >> 8;
        color2->a = c2 & 0xff;
        color3->r = color3->g = color3->b = c3 >> 8;
        color3->a = c3 & 0xff;
    }
}


static void get_tmem_idx(int s, int t, uint32_t tilenum, uint32_t* idx0, uint32_t* idx1, uint32_t* idx2, uint32_t* idx3, uint32_t* bit3flipped, uint32_t* hibit)
{
    uint32_t tbase = (tile[tilenum].line * t) & 0x1ff;
    tbase += tile[tilenum].tmem;
    uint32_t tsize = tile[tilenum].size;
    uint32_t tformat = tile[tilenum].format;
    uint32_t sshorts = 0;


    if (tsize == PIXEL_SIZE_8BIT || tformat == FORMAT_YUV)
        sshorts = s >> 1;
    else if (tsize >= PIXEL_SIZE_16BIT)
        sshorts = s;
    else
        sshorts = s >> 2;
    sshorts &= 0x7ff;

    *bit3flipped = ((sshorts & 2) ? 1 : 0) ^ (t & 1);

    int tidx_a = ((tbase << 2) + sshorts) & 0x7fd;
    int tidx_b = (tidx_a + 1) & 0x7ff;
    int tidx_c = (tidx_a + 2) & 0x7ff;
    int tidx_d = (tidx_a + 3) & 0x7ff;

    *hibit = (tidx_a & 0x400) ? 1 : 0;

    if (t & 1)
    {
        tidx_a ^= 2;
        tidx_b ^= 2;
        tidx_c ^= 2;
        tidx_d ^= 2;
    }


    sort_tmem_idx(idx0, tidx_a, tidx_b, tidx_c, tidx_d, 0);
    sort_tmem_idx(idx1, tidx_a, tidx_b, tidx_c, tidx_d, 1);
    sort_tmem_idx(idx2, tidx_a, tidx_b, tidx_c, tidx_d, 2);
    sort_tmem_idx(idx3, tidx_a, tidx_b, tidx_c, tidx_d, 3);
}







static void read_tmem_copy(int s, int s1, int s2, int s3, int t, uint32_t tilenum, uint32_t* sortshort, int* hibits, int* lowbits)
{
    uint32_t tbase = (tile[tilenum].line * t) & 0x1ff;
    tbase += tile[tilenum].tmem;
    uint32_t tsize = tile[tilenum].size;
    uint32_t tformat = tile[tilenum].format;
    uint32_t shbytes = 0, shbytes1 = 0, shbytes2 = 0, shbytes3 = 0;
    int32_t delta = 0;
    uint32_t sortidx[8];


    if (tsize == PIXEL_SIZE_8BIT || tformat == FORMAT_YUV)
    {
        shbytes = s << 1;
        shbytes1 = s1 << 1;
        shbytes2 = s2 << 1;
        shbytes3 = s3 << 1;
    }
    else if (tsize >= PIXEL_SIZE_16BIT)
    {
        shbytes = s << 2;
        shbytes1 = s1 << 2;
        shbytes2 = s2 << 2;
        shbytes3 = s3 << 2;
    }
    else
    {
        shbytes = s;
        shbytes1 = s1;
        shbytes2 = s2;
        shbytes3 = s3;
    }

    shbytes &= 0x1fff;
    shbytes1 &= 0x1fff;
    shbytes2 &= 0x1fff;
    shbytes3 &= 0x1fff;

    int tidx_a, tidx_blow, tidx_bhi, tidx_c, tidx_dlow, tidx_dhi;

    tbase <<= 4;
    tidx_a = (tbase + shbytes) & 0x1fff;
    tidx_bhi = (tbase + shbytes1) & 0x1fff;
    tidx_c = (tbase + shbytes2) & 0x1fff;
    tidx_dhi = (tbase + shbytes3) & 0x1fff;

    if (tformat == FORMAT_YUV)
    {
        delta = shbytes1 - shbytes;
        tidx_blow = (tidx_a + (delta << 1)) & 0x1fff;
        tidx_dlow = (tidx_blow + shbytes3 - shbytes) & 0x1fff;
    }
    else
    {
        tidx_blow = tidx_bhi;
        tidx_dlow = tidx_dhi;
    }

    if (t & 1)
    {
        tidx_a ^= 8;
        tidx_blow ^= 8;
        tidx_bhi ^= 8;
        tidx_c ^= 8;
        tidx_dlow ^= 8;
        tidx_dhi ^= 8;
    }

    hibits[0] = (tidx_a & 0x1000) ? 1 : 0;
    hibits[1] = (tidx_blow & 0x1000) ? 1 : 0;
    hibits[2] = (tidx_bhi & 0x1000) ? 1 : 0;
    hibits[3] = (tidx_c & 0x1000) ? 1 : 0;
    hibits[4] = (tidx_dlow & 0x1000) ? 1 : 0;
    hibits[5] = (tidx_dhi & 0x1000) ? 1 : 0;
    lowbits[0] = tidx_a & 0xf;
    lowbits[1] = tidx_blow & 0xf;
    lowbits[2] = tidx_bhi & 0xf;
    lowbits[3] = tidx_c & 0xf;
    lowbits[4] = tidx_dlow & 0xf;
    lowbits[5] = tidx_dhi & 0xf;

    uint16_t* tmem16 = (uint16_t*)tmem;
    uint32_t short0, short1, short2, short3;


    tidx_a >>= 2;
    tidx_blow >>= 2;
    tidx_bhi >>= 2;
    tidx_c >>= 2;
    tidx_dlow >>= 2;
    tidx_dhi >>= 2;


    sort_tmem_idx(&sortidx[0], tidx_a, tidx_blow, tidx_c, tidx_dlow, 0);
    sort_tmem_idx(&sortidx[1], tidx_a, tidx_blow, tidx_c, tidx_dlow, 1);
    sort_tmem_idx(&sortidx[2], tidx_a, tidx_blow, tidx_c, tidx_dlow, 2);
    sort_tmem_idx(&sortidx[3], tidx_a, tidx_blow, tidx_c, tidx_dlow, 3);

    short0 = tmem16[sortidx[0] ^ WORD_ADDR_XOR];
    short1 = tmem16[sortidx[1] ^ WORD_ADDR_XOR];
    short2 = tmem16[sortidx[2] ^ WORD_ADDR_XOR];
    short3 = tmem16[sortidx[3] ^ WORD_ADDR_XOR];


    sort_tmem_shorts_lowhalf(&sortshort[0], short0, short1, short2, short3, lowbits[0] >> 2);
    sort_tmem_shorts_lowhalf(&sortshort[1], short0, short1, short2, short3, lowbits[1] >> 2);
    sort_tmem_shorts_lowhalf(&sortshort[2], short0, short1, short2, short3, lowbits[3] >> 2);
    sort_tmem_shorts_lowhalf(&sortshort[3], short0, short1, short2, short3, lowbits[4] >> 2);

    if (other_modes.en_tlut)
    {

        compute_color_index(&short0, sortshort[0], lowbits[0] & 3, tilenum);
        compute_color_index(&short1, sortshort[1], lowbits[1] & 3, tilenum);
        compute_color_index(&short2, sortshort[2], lowbits[3] & 3, tilenum);
        compute_color_index(&short3, sortshort[3], lowbits[4] & 3, tilenum);


        sortidx[4] = (short0 << 2);
        sortidx[5] = (short1 << 2) | 1;
        sortidx[6] = (short2 << 2) | 2;
        sortidx[7] = (short3 << 2) | 3;
    }
    else
    {
        sort_tmem_idx(&sortidx[4], tidx_a, tidx_bhi, tidx_c, tidx_dhi, 0);
        sort_tmem_idx(&sortidx[5], tidx_a, tidx_bhi, tidx_c, tidx_dhi, 1);
        sort_tmem_idx(&sortidx[6], tidx_a, tidx_bhi, tidx_c, tidx_dhi, 2);
        sort_tmem_idx(&sortidx[7], tidx_a, tidx_bhi, tidx_c, tidx_dhi, 3);
    }

    short0 = tmem16[(sortidx[4] | 0x400) ^ WORD_ADDR_XOR];
    short1 = tmem16[(sortidx[5] | 0x400) ^ WORD_ADDR_XOR];
    short2 = tmem16[(sortidx[6] | 0x400) ^ WORD_ADDR_XOR];
    short3 = tmem16[(sortidx[7] | 0x400) ^ WORD_ADDR_XOR];



    if (other_modes.en_tlut)
    {
        sort_tmem_shorts_lowhalf(&sortshort[4], short0, short1, short2, short3, 0);
        sort_tmem_shorts_lowhalf(&sortshort[5], short0, short1, short2, short3, 1);
        sort_tmem_shorts_lowhalf(&sortshort[6], short0, short1, short2, short3, 2);
        sort_tmem_shorts_lowhalf(&sortshort[7], short0, short1, short2, short3, 3);
    }
    else
    {
        sort_tmem_shorts_lowhalf(&sortshort[4], short0, short1, short2, short3, lowbits[0] >> 2);
        sort_tmem_shorts_lowhalf(&sortshort[5], short0, short1, short2, short3, lowbits[2] >> 2);
        sort_tmem_shorts_lowhalf(&sortshort[6], short0, short1, short2, short3, lowbits[3] >> 2);
        sort_tmem_shorts_lowhalf(&sortshort[7], short0, short1, short2, short3, lowbits[5] >> 2);
    }
}




static void sort_tmem_idx(uint32_t *idx, uint32_t idxa, uint32_t idxb, uint32_t idxc, uint32_t idxd, uint32_t bankno)
{
    if ((idxa & 3) == bankno)
        *idx = idxa & 0x3ff;
    else if ((idxb & 3) == bankno)
        *idx = idxb & 0x3ff;
    else if ((idxc & 3) == bankno)
        *idx = idxc & 0x3ff;
    else if ((idxd & 3) == bankno)
        *idx = idxd & 0x3ff;
    else
        *idx = 0;
}


static void sort_tmem_shorts_lowhalf(uint32_t* bindshort, uint32_t short0, uint32_t short1, uint32_t short2, uint32_t short3, uint32_t bankno)
{
    switch(bankno)
    {
    case 0:
        *bindshort = short0;
        break;
    case 1:
        *bindshort = short1;
        break;
    case 2:
        *bindshort = short2;
        break;
    case 3:
        *bindshort = short3;
        break;
    }
}



static void compute_color_index(uint32_t* cidx, uint32_t readshort, uint32_t nybbleoffset, uint32_t tilenum)
{
    uint32_t lownib, hinib;
    if (tile[tilenum].size == PIXEL_SIZE_4BIT)
    {
        lownib = (nybbleoffset ^ 3) << 2;
        hinib = tile[tilenum].palette;
    }
    else
    {
        lownib = ((nybbleoffset & 2) ^ 2) << 2;
        hinib = lownib ? ((readshort >> 12) & 0xf) : ((readshort >> 4) & 0xf);
    }
    lownib = (readshort >> lownib) & 0xf;
    *cidx = (hinib << 4) | lownib;
}


static void replicate_for_copy(uint32_t* outbyte, uint32_t inshort, uint32_t nybbleoffset, uint32_t tilenum, uint32_t tformat, uint32_t tsize)
{
    uint32_t lownib, hinib;
    switch(tsize)
    {
    case PIXEL_SIZE_4BIT:
        lownib = (nybbleoffset ^ 3) << 2;
        lownib = hinib = (inshort >> lownib) & 0xf;
        if (tformat == FORMAT_CI)
        {
            *outbyte = (tile[tilenum].palette << 4) | lownib;
        }
        else if (tformat == FORMAT_IA)
        {
            lownib = (lownib << 4) | lownib;
            *outbyte = (lownib & 0xe0) | ((lownib & 0xe0) >> 3) | ((lownib & 0xc0) >> 6);
        }
        else
            *outbyte = (lownib << 4) | lownib;
        break;
    case PIXEL_SIZE_8BIT:
        hinib = ((nybbleoffset ^ 3) | 1) << 2;
        if (tformat == FORMAT_IA)
        {
            lownib = (inshort >> hinib) & 0xf;
            *outbyte = (lownib << 4) | lownib;
        }
        else
        {
            lownib = (inshort >> (hinib & ~4)) & 0xf;
            hinib = (inshort >> hinib) & 0xf;
            *outbyte = (hinib << 4) | lownib;
        }
        break;
    default:
        *outbyte = (inshort >> 8) & 0xff;
        break;
    }
}

static void fetch_qword_copy(uint32_t* hidword, uint32_t* lowdword, int32_t ssss, int32_t ssst, uint32_t tilenum)
{
    uint32_t shorta, shortb, shortc, shortd;
    uint32_t sortshort[8];
    int hibits[6];
    int lowbits[6];
    int32_t sss = ssss, sst = ssst, sss1 = 0, sss2 = 0, sss3 = 0;
    int largetex = 0;

    uint32_t tformat, tsize;
    if (other_modes.en_tlut)
    {
        tsize = PIXEL_SIZE_16BIT;
        tformat = other_modes.tlut_type ? FORMAT_IA : FORMAT_RGBA;
    }
    else
    {
        tsize = tile[tilenum].size;
        tformat = tile[tilenum].format;
    }

    tc_pipeline_copy(&sss, &sss1, &sss2, &sss3, &sst, tilenum);
    read_tmem_copy(sss, sss1, sss2, sss3, sst, tilenum, sortshort, hibits, lowbits);
    largetex = (tformat == FORMAT_YUV || (tformat == FORMAT_RGBA && tsize == PIXEL_SIZE_32BIT));


    if (other_modes.en_tlut)
    {
        shorta = sortshort[4];
        shortb = sortshort[5];
        shortc = sortshort[6];
        shortd = sortshort[7];
    }
    else if (largetex)
    {
        shorta = sortshort[0];
        shortb = sortshort[1];
        shortc = sortshort[2];
        shortd = sortshort[3];
    }
    else
    {
        shorta = hibits[0] ? sortshort[4] : sortshort[0];
        shortb = hibits[1] ? sortshort[5] : sortshort[1];
        shortc = hibits[3] ? sortshort[6] : sortshort[2];
        shortd = hibits[4] ? sortshort[7] : sortshort[3];
    }

    *lowdword = (shortc << 16) | shortd;

    if (tsize == PIXEL_SIZE_16BIT)
        *hidword = (shorta << 16) | shortb;
    else
    {
        replicate_for_copy(&shorta, shorta, lowbits[0] & 3, tilenum, tformat, tsize);
        replicate_for_copy(&shortb, shortb, lowbits[1] & 3, tilenum, tformat, tsize);
        replicate_for_copy(&shortc, shortc, lowbits[3] & 3, tilenum, tformat, tsize);
        replicate_for_copy(&shortd, shortd, lowbits[4] & 3, tilenum, tformat, tsize);
        *hidword = (shorta << 24) | (shortb << 16) | (shortc << 8) | shortd;
    }
}

static STRICTINLINE void texture_pipeline_cycle(struct color* TEX, struct color* prev, int32_t SSS, int32_t SST, uint32_t tilenum, uint32_t cycle)
{
#define TRELATIVE(x, y)     ((x) - ((y) << 3))


#define UPPER ((sfrac + tfrac) & 0x20)




    int32_t maxs, maxt, invt0r, invt0g, invt0b, invt0a;
    int32_t sfrac, tfrac, invsf, invtf;
    int upper = 0;


    int bilerp = cycle ? other_modes.bi_lerp1 : other_modes.bi_lerp0;
    int convert = other_modes.convert_one && cycle;
    struct color t0, t1, t2, t3;
    int sss1, sst1, sss2, sst2;

    sss1 = SSS;
    sst1 = SST;

    tcshift_cycle(&sss1, &sst1, &maxs, &maxt, tilenum);


    sss1 = TRELATIVE(sss1, tile[tilenum].sl);
    sst1 = TRELATIVE(sst1, tile[tilenum].tl);


    if (other_modes.sample_type)
    {
        sfrac = sss1 & 0x1f;
        tfrac = sst1 & 0x1f;

        tcclamp_cycle(&sss1, &sst1, &sfrac, &tfrac, maxs, maxt, tilenum);


        if (tile[tilenum].format != FORMAT_YUV)
            sss2 = sss1 + 1;
        else
            sss2 = sss1 + 2;




        sst2 = sst1 + 1;



        tcmask_coupled(&sss1, &sss2, &sst1, &sst2, tilenum);












        if (bilerp)
        {

            if (!other_modes.en_tlut)
                fetch_texel_quadro(&t0, &t1, &t2, &t3, sss1, sss2, sst1, sst2, tilenum);
            else
                fetch_texel_entlut_quadro(&t0, &t1, &t2, &t3, sss1, sss2, sst1, sst2, tilenum);







            if (!other_modes.mid_texel || sfrac != 0x10 || tfrac != 0x10)
            {
                if (!convert)
                {
                    if (UPPER)
                    {

                        invsf = 0x20 - sfrac;
                        invtf = 0x20 - tfrac;
                        TEX->r = t3.r + ((((invsf * (t2.r - t3.r)) + (invtf * (t1.r - t3.r))) + 0x10) >> 5);
                        TEX->g = t3.g + ((((invsf * (t2.g - t3.g)) + (invtf * (t1.g - t3.g))) + 0x10) >> 5);
                        TEX->b = t3.b + ((((invsf * (t2.b - t3.b)) + (invtf * (t1.b - t3.b))) + 0x10) >> 5);
                        TEX->a = t3.a + ((((invsf * (t2.a - t3.a)) + (invtf * (t1.a - t3.a))) + 0x10) >> 5);
                    }
                    else
                    {
                        TEX->r = t0.r + ((((sfrac * (t1.r - t0.r)) + (tfrac * (t2.r - t0.r))) + 0x10) >> 5);
                        TEX->g = t0.g + ((((sfrac * (t1.g - t0.g)) + (tfrac * (t2.g - t0.g))) + 0x10) >> 5);
                        TEX->b = t0.b + ((((sfrac * (t1.b - t0.b)) + (tfrac * (t2.b - t0.b))) + 0x10) >> 5);
                        TEX->a = t0.a + ((((sfrac * (t1.a - t0.a)) + (tfrac * (t2.a - t0.a))) + 0x10) >> 5);
                    }
                }
                else
                {
                    if (UPPER)
                    {
                        TEX->r = prev->b + ((((prev->r * (t2.r - t3.r)) + (prev->g * (t1.r - t3.r))) + 0x80) >> 8);
                        TEX->g = prev->b + ((((prev->r * (t2.g - t3.g)) + (prev->g * (t1.g - t3.g))) + 0x80) >> 8);
                        TEX->b = prev->b + ((((prev->r * (t2.b - t3.b)) + (prev->g * (t1.b - t3.b))) + 0x80) >> 8);
                        TEX->a = prev->b + ((((prev->r * (t2.a - t3.a)) + (prev->g * (t1.a - t3.a))) + 0x80) >> 8);
                    }
                    else
                    {
                        TEX->r = prev->b + ((((prev->r * (t1.r - t0.r)) + (prev->g * (t2.r - t0.r))) + 0x80) >> 8);
                        TEX->g = prev->b + ((((prev->r * (t1.g - t0.g)) + (prev->g * (t2.g - t0.g))) + 0x80) >> 8);
                        TEX->b = prev->b + ((((prev->r * (t1.b - t0.b)) + (prev->g * (t2.b - t0.b))) + 0x80) >> 8);
                        TEX->a = prev->b + ((((prev->r * (t1.a - t0.a)) + (prev->g * (t2.a - t0.a))) + 0x80) >> 8);
                    }
                }

            }
            else
            {
                invt0r  = ~t0.r; invt0g = ~t0.g; invt0b = ~t0.b; invt0a = ~t0.a;
                if (!convert)
                {
                    sfrac <<= 2;
                    tfrac <<= 2;
                    TEX->r = t0.r + ((((sfrac * (t1.r - t0.r)) + (tfrac * (t2.r - t0.r))) + ((invt0r + t3.r) << 6) + 0xc0) >> 8);
                    TEX->g = t0.g + ((((sfrac * (t1.g - t0.g)) + (tfrac * (t2.g - t0.g))) + ((invt0g + t3.g) << 6) + 0xc0) >> 8);
                    TEX->b = t0.b + ((((sfrac * (t1.b - t0.b)) + (tfrac * (t2.b - t0.b))) + ((invt0b + t3.b) << 6) + 0xc0) >> 8);
                    TEX->a = t0.a + ((((sfrac * (t1.a - t0.a)) + (tfrac * (t2.a - t0.a))) + ((invt0a + t3.a) << 6) + 0xc0) >> 8);
                }
                else
                {
                    TEX->r = prev->b + ((((prev->r * (t1.r - t0.r)) + (prev->g * (t2.r - t0.r))) + ((invt0r + t3.r) << 6) + 0xc0) >> 8);
                    TEX->g = prev->b + ((((prev->r * (t1.g - t0.g)) + (prev->g * (t2.g - t0.g))) + ((invt0g + t3.g) << 6) + 0xc0) >> 8);
                    TEX->b = prev->b + ((((prev->r * (t1.b - t0.b)) + (prev->g * (t2.b - t0.b))) + ((invt0b + t3.b) << 6) + 0xc0) >> 8);
                    TEX->a = prev->b + ((((prev->r * (t1.a - t0.a)) + (prev->g * (t2.a - t0.a))) + ((invt0a + t3.a) << 6) + 0xc0) >> 8);
                }
            }

        }
        else
        {
            if (!other_modes.en_tlut)
                fetch_texel(&t0, sss1, sst1, tilenum);
            else
                fetch_texel_entlut(&t0, sss1, sst1, tilenum);
            if (convert)
                t0 = *prev;


            TEX->r = t0.b + ((k0_tf * t0.g + 0x80) >> 8);
            TEX->g = t0.b + ((k1_tf * t0.r + k2_tf * t0.g + 0x80) >> 8);
            TEX->b = t0.b + ((k3_tf * t0.r + 0x80) >> 8);
            TEX->a = t0.b;
        }

        TEX->r &= 0x1ff;
        TEX->g &= 0x1ff;
        TEX->b &= 0x1ff;
        TEX->a &= 0x1ff;


    }
    else
    {




        tcclamp_cycle_light(&sss1, &sst1, maxs, maxt, tilenum);

        tcmask(&sss1, &sst1, tilenum);


        if (!other_modes.en_tlut)
            fetch_texel(&t0, sss1, sst1, tilenum);
        else
            fetch_texel_entlut(&t0, sss1, sst1, tilenum);

        if (bilerp)
        {
            if (!convert)
            {
                TEX->r = t0.r & 0x1ff;
                TEX->g = t0.g & 0x1ff;
                TEX->b = t0.b;
                TEX->a = t0.a;
            }
            else
                TEX->r = TEX->g = TEX->b = TEX->a = prev->b;
        }
        else
        {
            if (convert)
                t0 = *prev;

            TEX->r = t0.b + ((k0_tf * t0.g + 0x80) >> 8);
            TEX->g = t0.b + ((k1_tf * t0.r + k2_tf * t0.g + 0x80) >> 8);
            TEX->b = t0.b + ((k3_tf * t0.r + 0x80) >> 8);
            TEX->a = t0.b;
            TEX->r &= 0x1ff;
            TEX->g &= 0x1ff;
            TEX->b &= 0x1ff;
            TEX->a &= 0x1ff;
        }
    }

}


static STRICTINLINE void tc_pipeline_copy(int32_t* sss0, int32_t* sss1, int32_t* sss2, int32_t* sss3, int32_t* sst, int tilenum)
{
    int ss0 = *sss0, ss1 = 0, ss2 = 0, ss3 = 0, st = *sst;

    tcshift_copy(&ss0, &st, tilenum);



    ss0 = TRELATIVE(ss0, tile[tilenum].sl);
    st = TRELATIVE(st, tile[tilenum].tl);
    ss0 = (ss0 >> 5);
    st = (st >> 5);

    ss1 = ss0 + 1;
    ss2 = ss0 + 2;
    ss3 = ss0 + 3;

    tcmask_copy(&ss0, &ss1, &ss2, &ss3, &st, tilenum);

    *sss0 = ss0;
    *sss1 = ss1;
    *sss2 = ss2;
    *sss3 = ss3;
    *sst = st;
}

static STRICTINLINE void tc_pipeline_load(int32_t* sss, int32_t* sst, int tilenum, int coord_quad)
{
    int sss1 = *sss, sst1 = *sst;
    sss1 = SIGN16(sss1);
    sst1 = SIGN16(sst1);


    sss1 = TRELATIVE(sss1, tile[tilenum].sl);
    sst1 = TRELATIVE(sst1, tile[tilenum].tl);



    if (!coord_quad)
    {
        sss1 = (sss1 >> 5);
        sst1 = (sst1 >> 5);
    }
    else
    {
        sss1 = (sss1 >> 3);
        sst1 = (sst1 >> 3);
    }

    *sss = sss1;
    *sst = sst1;
}



static void render_spans_1cycle_complete(int start, int end, int tilenum, int flip)
{
    int zb = zb_address >> 1;
    int zbcur;
    uint8_t offx, offy;
    struct spansigs sigs;
    uint32_t blend_en;
    uint32_t prewrap;
    uint32_t curpixel_cvg, curpixel_cvbit, curpixel_memcvg;

    int prim_tile = tilenum;
    int tile1 = tilenum;
    int newtile = tilenum;
    int news, newt;

    int i, j;

    int drinc, dginc, dbinc, dainc, dzinc, dsinc, dtinc, dwinc;
    int xinc;

    if (flip)
    {
        drinc = spans_dr;
        dginc = spans_dg;
        dbinc = spans_db;
        dainc = spans_da;
        dzinc = spans_dz;
        dsinc = spans_ds;
        dtinc = spans_dt;
        dwinc = spans_dw;
        xinc = 1;
    }
    else
    {
        drinc = -spans_dr;
        dginc = -spans_dg;
        dbinc = -spans_db;
        dainc = -spans_da;
        dzinc = -spans_dz;
        dsinc = -spans_ds;
        dtinc = -spans_dt;
        dwinc = -spans_dw;
        xinc = -1;
    }

    int dzpix;
    if (!other_modes.z_source_sel)
        dzpix = spans_dzpix;
    else
    {
        dzpix = primitive_delta_z;
        dzinc = spans_cdz = spans_dzdy = 0;
    }
    int dzpixenc = dz_compress(dzpix);

    int cdith = 7, adith = 0;
    int r, g, b, a, z, s, t, w;
    int sr, sg, sb, sa, sz, ss, st, sw;
    int xstart, xend, xendsc;
    int sss = 0, sst = 0;
    int32_t prelodfrac;
    int curpixel = 0;
    int x, length, scdiff, lodlength;
    uint32_t fir, fig, fib;

    for (i = start; i <= end; i++)
    {
        if (span[i].validline)
        {

        xstart = span[i].lx;
        xend = span[i].unscrx;
        xendsc = span[i].rx;
        r = span[i].r;
        g = span[i].g;
        b = span[i].b;
        a = span[i].a;
        z = other_modes.z_source_sel ? primitive_z : span[i].z;
        s = span[i].s;
        t = span[i].t;
        w = span[i].w;

        x = xendsc;
        curpixel = fb_width * i + x;
        zbcur = zb + curpixel;

        if (!flip)
        {
            length = xendsc - xstart;
            scdiff = xend - xendsc;
            compute_cvg_noflip(i);
        }
        else
        {
            length = xstart - xendsc;
            scdiff = xendsc - xend;
            compute_cvg_flip(i);
        }



        if (scdiff)
        {


            scdiff &= 0xfff;
            r += (drinc * scdiff);
            g += (dginc * scdiff);
            b += (dbinc * scdiff);
            a += (dainc * scdiff);
            z += (dzinc * scdiff);
            s += (dsinc * scdiff);
            t += (dtinc * scdiff);
            w += (dwinc * scdiff);
        }

        lodlength = length + scdiff;

        sigs.longspan = (lodlength > 7);
        sigs.midspan = (lodlength == 7);
        sigs.onelessthanmid = (lodlength == 6);

        sigs.startspan = 1;

        for (j = 0; j <= length; j++)
        {
            sr = r >> 14;
            sg = g >> 14;
            sb = b >> 14;
            sa = a >> 14;
            ss = s >> 16;
            st = t >> 16;
            sw = w >> 16;
            sz = (z >> 10) & 0x3fffff;


            sigs.endspan = (j == length);
            sigs.preendspan = (j == (length - 1));

            lookup_cvmask_derivatives(cvgbuf[x], &offx, &offy, &curpixel_cvg, &curpixel_cvbit);


            get_texel1_1cycle(&news, &newt, s, t, w, dsinc, dtinc, dwinc, i, &sigs);



            if (!sigs.startspan)
            {
                texel0_color = texel1_color;
                lod_frac = prelodfrac;
            }
            else
            {
                tcdiv_ptr(ss, st, sw, &sss, &sst);


                tclod_1cycle_current(&sss, &sst, news, newt, s, t, w, dsinc, dtinc, dwinc, i, prim_tile, &tile1, &sigs);




                texture_pipeline_cycle(&texel0_color, &texel0_color, sss, sst, tile1, 0);


                sigs.startspan = 0;
            }

            sigs.nextspan = sigs.endspan;
            sigs.endspan = sigs.preendspan;
            sigs.preendspan = (j == (length - 2));

            s += dsinc;
            t += dtinc;
            w += dwinc;

            tclod_1cycle_next(&news, &newt, s, t, w, dsinc, dtinc, dwinc, i, prim_tile, &newtile, &sigs, &prelodfrac);

            texture_pipeline_cycle(&texel1_color, &texel1_color, news, newt, newtile, 0);

            rgbaz_correct_clip(offx, offy, sr, sg, sb, sa, &sz, curpixel_cvg);

            get_dither_noise_ptr(x, i, &cdith, &adith);
            combiner_1cycle(adith, &curpixel_cvg);

            fbread1_ptr(curpixel, &curpixel_memcvg);
            if (z_compare(zbcur, sz, dzpix, dzpixenc, &blend_en, &prewrap, &curpixel_cvg, curpixel_memcvg))
            {
                if (blender_1cycle(&fir, &fig, &fib, cdith, blend_en, prewrap, curpixel_cvg, curpixel_cvbit))
                {
                    fbwrite_ptr(curpixel, fir, fig, fib, blend_en, curpixel_cvg, curpixel_memcvg);
                    if (other_modes.z_update_en)
                        z_store(zbcur, sz, dzpixenc);
                }
            }




            r += drinc;
            g += dginc;
            b += dbinc;
            a += dainc;
            z += dzinc;

            x += xinc;
            curpixel += xinc;
            zbcur += xinc;
        }
        }
    }
}


static void render_spans_1cycle_notexel1(int start, int end, int tilenum, int flip)
{
    int zb = zb_address >> 1;
    int zbcur;
    uint8_t offx, offy;
    struct spansigs sigs;
    uint32_t blend_en;
    uint32_t prewrap;
    uint32_t curpixel_cvg, curpixel_cvbit, curpixel_memcvg;

    int prim_tile = tilenum;
    int tile1 = tilenum;

    int i, j;

    int drinc, dginc, dbinc, dainc, dzinc, dsinc, dtinc, dwinc;
    int xinc;
    if (flip)
    {
        drinc = spans_dr;
        dginc = spans_dg;
        dbinc = spans_db;
        dainc = spans_da;
        dzinc = spans_dz;
        dsinc = spans_ds;
        dtinc = spans_dt;
        dwinc = spans_dw;
        xinc = 1;
    }
    else
    {
        drinc = -spans_dr;
        dginc = -spans_dg;
        dbinc = -spans_db;
        dainc = -spans_da;
        dzinc = -spans_dz;
        dsinc = -spans_ds;
        dtinc = -spans_dt;
        dwinc = -spans_dw;
        xinc = -1;
    }

    int dzpix;
    if (!other_modes.z_source_sel)
        dzpix = spans_dzpix;
    else
    {
        dzpix = primitive_delta_z;
        dzinc = spans_cdz = spans_dzdy = 0;
    }
    int dzpixenc = dz_compress(dzpix);

    int cdith = 7, adith = 0;
    int r, g, b, a, z, s, t, w;
    int sr, sg, sb, sa, sz, ss, st, sw;
    int xstart, xend, xendsc;
    int sss = 0, sst = 0;
    int curpixel = 0;
    int x, length, scdiff, lodlength;
    uint32_t fir, fig, fib;

    for (i = start; i <= end; i++)
    {
        if (span[i].validline)
        {

        xstart = span[i].lx;
        xend = span[i].unscrx;
        xendsc = span[i].rx;
        r = span[i].r;
        g = span[i].g;
        b = span[i].b;
        a = span[i].a;
        z = other_modes.z_source_sel ? primitive_z : span[i].z;
        s = span[i].s;
        t = span[i].t;
        w = span[i].w;

        x = xendsc;
        curpixel = fb_width * i + x;
        zbcur = zb + curpixel;

        if (!flip)
        {
            length = xendsc - xstart;
            scdiff = xend - xendsc;
            compute_cvg_noflip(i);
        }
        else
        {
            length = xstart - xendsc;
            scdiff = xendsc - xend;
            compute_cvg_flip(i);
        }

        if (scdiff)
        {
            scdiff &= 0xfff;
            r += (drinc * scdiff);
            g += (dginc * scdiff);
            b += (dbinc * scdiff);
            a += (dainc * scdiff);
            z += (dzinc * scdiff);
            s += (dsinc * scdiff);
            t += (dtinc * scdiff);
            w += (dwinc * scdiff);
        }

        lodlength = length + scdiff;

        sigs.longspan = (lodlength > 7);
        sigs.midspan = (lodlength == 7);

        for (j = 0; j <= length; j++)
        {
            sr = r >> 14;
            sg = g >> 14;
            sb = b >> 14;
            sa = a >> 14;
            ss = s >> 16;
            st = t >> 16;
            sw = w >> 16;
            sz = (z >> 10) & 0x3fffff;

            sigs.endspan = (j == length);
            sigs.preendspan = (j == (length - 1));

            lookup_cvmask_derivatives(cvgbuf[x], &offx, &offy, &curpixel_cvg, &curpixel_cvbit);

            tcdiv_ptr(ss, st, sw, &sss, &sst);

            tclod_1cycle_current_simple(&sss, &sst, s, t, w, dsinc, dtinc, dwinc, i, prim_tile, &tile1, &sigs);

            texture_pipeline_cycle(&texel0_color, &texel0_color, sss, sst, tile1, 0);

            rgbaz_correct_clip(offx, offy, sr, sg, sb, sa, &sz, curpixel_cvg);

            get_dither_noise_ptr(x, i, &cdith, &adith);
            combiner_1cycle(adith, &curpixel_cvg);

            fbread1_ptr(curpixel, &curpixel_memcvg);
            if (z_compare(zbcur, sz, dzpix, dzpixenc, &blend_en, &prewrap, &curpixel_cvg, curpixel_memcvg))
            {
                if (blender_1cycle(&fir, &fig, &fib, cdith, blend_en, prewrap, curpixel_cvg, curpixel_cvbit))
                {
                    fbwrite_ptr(curpixel, fir, fig, fib, blend_en, curpixel_cvg, curpixel_memcvg);
                    if (other_modes.z_update_en)
                        z_store(zbcur, sz, dzpixenc);
                }
            }

            s += dsinc;
            t += dtinc;
            w += dwinc;
            r += drinc;
            g += dginc;
            b += dbinc;
            a += dainc;
            z += dzinc;

            x += xinc;
            curpixel += xinc;
            zbcur += xinc;
        }
        }
    }
}


static void render_spans_1cycle_notex(int start, int end, int tilenum, int flip)
{
    int zb = zb_address >> 1;
    int zbcur;
    uint8_t offx, offy;
    uint32_t blend_en;
    uint32_t prewrap;
    uint32_t curpixel_cvg, curpixel_cvbit, curpixel_memcvg;

    int i, j;

    int drinc, dginc, dbinc, dainc, dzinc;
    int xinc;

    if (flip)
    {
        drinc = spans_dr;
        dginc = spans_dg;
        dbinc = spans_db;
        dainc = spans_da;
        dzinc = spans_dz;
        xinc = 1;
    }
    else
    {
        drinc = -spans_dr;
        dginc = -spans_dg;
        dbinc = -spans_db;
        dainc = -spans_da;
        dzinc = -spans_dz;
        xinc = -1;
    }

    int dzpix;
    if (!other_modes.z_source_sel)
        dzpix = spans_dzpix;
    else
    {
        dzpix = primitive_delta_z;
        dzinc = spans_cdz = spans_dzdy = 0;
    }
    int dzpixenc = dz_compress(dzpix);

    int cdith = 7, adith = 0;
    int r, g, b, a, z;
    int sr, sg, sb, sa, sz;
    int xstart, xend, xendsc;
    int curpixel = 0;
    int x, length, scdiff;
    uint32_t fir, fig, fib;

    for (i = start; i <= end; i++)
    {
        if (span[i].validline)
        {

        xstart = span[i].lx;
        xend = span[i].unscrx;
        xendsc = span[i].rx;
        r = span[i].r;
        g = span[i].g;
        b = span[i].b;
        a = span[i].a;
        z = other_modes.z_source_sel ? primitive_z : span[i].z;

        x = xendsc;
        curpixel = fb_width * i + x;
        zbcur = zb + curpixel;

        if (!flip)
        {
            length = xendsc - xstart;
            scdiff = xend - xendsc;
            compute_cvg_noflip(i);
        }
        else
        {
            length = xstart - xendsc;
            scdiff = xendsc - xend;
            compute_cvg_flip(i);
        }

        if (scdiff)
        {
            scdiff &= 0xfff;
            r += (drinc * scdiff);
            g += (dginc * scdiff);
            b += (dbinc * scdiff);
            a += (dainc * scdiff);
            z += (dzinc * scdiff);
        }

        for (j = 0; j <= length; j++)
        {
            sr = r >> 14;
            sg = g >> 14;
            sb = b >> 14;
            sa = a >> 14;
            sz = (z >> 10) & 0x3fffff;

            lookup_cvmask_derivatives(cvgbuf[x], &offx, &offy, &curpixel_cvg, &curpixel_cvbit);

            rgbaz_correct_clip(offx, offy, sr, sg, sb, sa, &sz, curpixel_cvg);

            get_dither_noise_ptr(x, i, &cdith, &adith);
            combiner_1cycle(adith, &curpixel_cvg);

            fbread1_ptr(curpixel, &curpixel_memcvg);
            if (z_compare(zbcur, sz, dzpix, dzpixenc, &blend_en, &prewrap, &curpixel_cvg, curpixel_memcvg))
            {
                if (blender_1cycle(&fir, &fig, &fib, cdith, blend_en, prewrap, curpixel_cvg, curpixel_cvbit))
                {
                    fbwrite_ptr(curpixel, fir, fig, fib, blend_en, curpixel_cvg, curpixel_memcvg);
                    if (other_modes.z_update_en)
                        z_store(zbcur, sz, dzpixenc);
                }
            }
            r += drinc;
            g += dginc;
            b += dbinc;
            a += dainc;
            z += dzinc;

            x += xinc;
            curpixel += xinc;
            zbcur += xinc;
        }
        }
    }
}

static void render_spans_2cycle_complete(int start, int end, int tilenum, int flip)
{
    int zb = zb_address >> 1;
    int zbcur;
    uint8_t offx, offy;
    struct spansigs sigs;
    int32_t prelodfrac;
    struct color nexttexel1_color;
    uint32_t blend_en;
    uint32_t prewrap;
    uint32_t curpixel_cvg, curpixel_cvbit, curpixel_memcvg;
    int32_t acalpha;



    int tile2 = (tilenum + 1) & 7;
    int tile1 = tilenum;
    int prim_tile = tilenum;

    int newtile1 = tile1;
    int newtile2 = tile2;
    int news, newt;

    int i, j;

    int drinc, dginc, dbinc, dainc, dzinc, dsinc, dtinc, dwinc;
    int xinc;
    if (flip)
    {
        drinc = spans_dr;
        dginc = spans_dg;
        dbinc = spans_db;
        dainc = spans_da;
        dzinc = spans_dz;
        dsinc = spans_ds;
        dtinc = spans_dt;
        dwinc = spans_dw;
        xinc = 1;
    }
    else
    {
        drinc = -spans_dr;
        dginc = -spans_dg;
        dbinc = -spans_db;
        dainc = -spans_da;
        dzinc = -spans_dz;
        dsinc = -spans_ds;
        dtinc = -spans_dt;
        dwinc = -spans_dw;
        xinc = -1;
    }

    int dzpix;
    if (!other_modes.z_source_sel)
        dzpix = spans_dzpix;
    else
    {
        dzpix = primitive_delta_z;
        dzinc = spans_cdz = spans_dzdy = 0;
    }
    int dzpixenc = dz_compress(dzpix);

    int cdith = 7, adith = 0;
    int r, g, b, a, z, s, t, w;
    int sr, sg, sb, sa, sz, ss, st, sw;
    int xstart, xend, xendsc;
    int sss = 0, sst = 0;
    int curpixel = 0;

    int x, length, scdiff;
    uint32_t fir, fig, fib;

    for (i = start; i <= end; i++)
    {
        if (span[i].validline)
        {

        xstart = span[i].lx;
        xend = span[i].unscrx;
        xendsc = span[i].rx;
        r = span[i].r;
        g = span[i].g;
        b = span[i].b;
        a = span[i].a;
        z = other_modes.z_source_sel ? primitive_z : span[i].z;
        s = span[i].s;
        t = span[i].t;
        w = span[i].w;

        x = xendsc;
        curpixel = fb_width * i + x;
        zbcur = zb + curpixel;

        if (!flip)
        {
            length = xendsc - xstart;
            scdiff = xend - xendsc;
            compute_cvg_noflip(i);
        }
        else
        {
            length = xstart - xendsc;
            scdiff = xendsc - xend;
            compute_cvg_flip(i);
        }








        if (scdiff)
        {
            scdiff &= 0xfff;
            r += (drinc * scdiff);
            g += (dginc * scdiff);
            b += (dbinc * scdiff);
            a += (dainc * scdiff);
            z += (dzinc * scdiff);
            s += (dsinc * scdiff);
            t += (dtinc * scdiff);
            w += (dwinc * scdiff);
        }
        sigs.startspan = 1;

        for (j = 0; j <= length; j++)
        {
            sr = r >> 14;
            sg = g >> 14;
            sb = b >> 14;
            sa = a >> 14;
            ss = s >> 16;
            st = t >> 16;
            sw = w >> 16;
            sz = (z >> 10) & 0x3fffff;


            lookup_cvmask_derivatives(cvgbuf[x], &offx, &offy, &curpixel_cvg, &curpixel_cvbit);

            get_nexttexel0_2cycle(&news, &newt, s, t, w, dsinc, dtinc, dwinc);

            if (!sigs.startspan)
            {
                lod_frac = prelodfrac;
                texel0_color = nexttexel_color;
                texel1_color = nexttexel1_color;
            }
            else
            {
                tcdiv_ptr(ss, st, sw, &sss, &sst);

                tclod_2cycle_current(&sss, &sst, news, newt, s, t, w, dsinc, dtinc, dwinc, prim_tile, &tile1, &tile2);



                texture_pipeline_cycle(&texel0_color, &texel0_color, sss, sst, tile1, 0);
                texture_pipeline_cycle(&texel1_color, &texel0_color, sss, sst, tile2, 1);

                sigs.startspan = 0;
            }

            s += dsinc;
            t += dtinc;
            w += dwinc;

            tclod_2cycle_next(&news, &newt, s, t, w, dsinc, dtinc, dwinc, prim_tile, &newtile1, &newtile2, &prelodfrac);

            texture_pipeline_cycle(&nexttexel_color, &nexttexel_color, news, newt, newtile1, 0);
            texture_pipeline_cycle(&nexttexel1_color, &nexttexel_color, news, newt, newtile2, 1);

            rgbaz_correct_clip(offx, offy, sr, sg, sb, sa, &sz, curpixel_cvg);

            get_dither_noise_ptr(x, i, &cdith, &adith);
            combiner_2cycle(adith, &curpixel_cvg, &acalpha);

            fbread2_ptr(curpixel, &curpixel_memcvg);

            if (z_compare(zbcur, sz, dzpix, dzpixenc, &blend_en, &prewrap, &curpixel_cvg, curpixel_memcvg))
            {
                if (blender_2cycle(&fir, &fig, &fib, cdith, blend_en, prewrap, curpixel_cvg, curpixel_cvbit, acalpha))
                {
                    fbwrite_ptr(curpixel, fir, fig, fib, blend_en, curpixel_cvg, curpixel_memcvg);
                    if (other_modes.z_update_en)
                        z_store(zbcur, sz, dzpixenc);
                }
            }
            else
                memory_color = pre_memory_color;









            r += drinc;
            g += dginc;
            b += dbinc;
            a += dainc;
            z += dzinc;

            x += xinc;
            curpixel += xinc;
            zbcur += xinc;
        }
        }
    }
}



static void render_spans_2cycle_notexelnext(int start, int end, int tilenum, int flip)
{
    int zb = zb_address >> 1;
    int zbcur;
    uint8_t offx, offy;
    uint32_t blend_en;
    uint32_t prewrap;
    uint32_t curpixel_cvg, curpixel_cvbit, curpixel_memcvg;
    int32_t acalpha;

    int tile2 = (tilenum + 1) & 7;
    int tile1 = tilenum;
    int prim_tile = tilenum;

    int i, j;

    int drinc, dginc, dbinc, dainc, dzinc, dsinc, dtinc, dwinc;
    int xinc;
    if (flip)
    {
        drinc = spans_dr;
        dginc = spans_dg;
        dbinc = spans_db;
        dainc = spans_da;
        dzinc = spans_dz;
        dsinc = spans_ds;
        dtinc = spans_dt;
        dwinc = spans_dw;
        xinc = 1;
    }
    else
    {
        drinc = -spans_dr;
        dginc = -spans_dg;
        dbinc = -spans_db;
        dainc = -spans_da;
        dzinc = -spans_dz;
        dsinc = -spans_ds;
        dtinc = -spans_dt;
        dwinc = -spans_dw;
        xinc = -1;
    }

    int dzpix;
    if (!other_modes.z_source_sel)
        dzpix = spans_dzpix;
    else
    {
        dzpix = primitive_delta_z;
        dzinc = spans_cdz = spans_dzdy = 0;
    }
    int dzpixenc = dz_compress(dzpix);

    int cdith = 7, adith = 0;
    int r, g, b, a, z, s, t, w;
    int sr, sg, sb, sa, sz, ss, st, sw;
    int xstart, xend, xendsc;
    int sss = 0, sst = 0;
    int curpixel = 0;

    int x, length, scdiff;
    uint32_t fir, fig, fib;

    for (i = start; i <= end; i++)
    {
        if (span[i].validline)
        {

        xstart = span[i].lx;
        xend = span[i].unscrx;
        xendsc = span[i].rx;
        r = span[i].r;
        g = span[i].g;
        b = span[i].b;
        a = span[i].a;
        z = other_modes.z_source_sel ? primitive_z : span[i].z;
        s = span[i].s;
        t = span[i].t;
        w = span[i].w;

        x = xendsc;
        curpixel = fb_width * i + x;
        zbcur = zb + curpixel;

        if (!flip)
        {
            length = xendsc - xstart;
            scdiff = xend - xendsc;
            compute_cvg_noflip(i);
        }
        else
        {
            length = xstart - xendsc;
            scdiff = xendsc - xend;
            compute_cvg_flip(i);
        }

        if (scdiff)
        {
            scdiff &= 0xfff;
            r += (drinc * scdiff);
            g += (dginc * scdiff);
            b += (dbinc * scdiff);
            a += (dainc * scdiff);
            z += (dzinc * scdiff);
            s += (dsinc * scdiff);
            t += (dtinc * scdiff);
            w += (dwinc * scdiff);
        }

        for (j = 0; j <= length; j++)
        {
            sr = r >> 14;
            sg = g >> 14;
            sb = b >> 14;
            sa = a >> 14;
            ss = s >> 16;
            st = t >> 16;
            sw = w >> 16;
            sz = (z >> 10) & 0x3fffff;

            lookup_cvmask_derivatives(cvgbuf[x], &offx, &offy, &curpixel_cvg, &curpixel_cvbit);

            tcdiv_ptr(ss, st, sw, &sss, &sst);

            tclod_2cycle_current_simple(&sss, &sst, s, t, w, dsinc, dtinc, dwinc, prim_tile, &tile1, &tile2);

            texture_pipeline_cycle(&texel0_color, &texel0_color, sss, sst, tile1, 0);
            texture_pipeline_cycle(&texel1_color, &texel0_color, sss, sst, tile2, 1);

            rgbaz_correct_clip(offx, offy, sr, sg, sb, sa, &sz, curpixel_cvg);

            get_dither_noise_ptr(x, i, &cdith, &adith);
            combiner_2cycle(adith, &curpixel_cvg, &acalpha);

            fbread2_ptr(curpixel, &curpixel_memcvg);




            if (z_compare(zbcur, sz, dzpix, dzpixenc, &blend_en, &prewrap, &curpixel_cvg, curpixel_memcvg))
            {
                if (blender_2cycle(&fir, &fig, &fib, cdith, blend_en, prewrap, curpixel_cvg, curpixel_cvbit, acalpha))
                {
                    fbwrite_ptr(curpixel, fir, fig, fib, blend_en, curpixel_cvg, curpixel_memcvg);
                    if (other_modes.z_update_en)
                        z_store(zbcur, sz, dzpixenc);
                }
            }
            else
                memory_color = pre_memory_color;

            s += dsinc;
            t += dtinc;
            w += dwinc;
            r += drinc;
            g += dginc;
            b += dbinc;
            a += dainc;
            z += dzinc;

            x += xinc;
            curpixel += xinc;
            zbcur += xinc;
        }
        }
    }
}


static void render_spans_2cycle_notexel1(int start, int end, int tilenum, int flip)
{
    int zb = zb_address >> 1;
    int zbcur;
    uint8_t offx, offy;
    uint32_t blend_en;
    uint32_t prewrap;
    uint32_t curpixel_cvg, curpixel_cvbit, curpixel_memcvg;
    int32_t acalpha;

    int tile1 = tilenum;
    int prim_tile = tilenum;

    int i, j;

    int drinc, dginc, dbinc, dainc, dzinc, dsinc, dtinc, dwinc;
    int xinc;
    if (flip)
    {
        drinc = spans_dr;
        dginc = spans_dg;
        dbinc = spans_db;
        dainc = spans_da;
        dzinc = spans_dz;
        dsinc = spans_ds;
        dtinc = spans_dt;
        dwinc = spans_dw;
        xinc = 1;
    }
    else
    {
        drinc = -spans_dr;
        dginc = -spans_dg;
        dbinc = -spans_db;
        dainc = -spans_da;
        dzinc = -spans_dz;
        dsinc = -spans_ds;
        dtinc = -spans_dt;
        dwinc = -spans_dw;
        xinc = -1;
    }

    int dzpix;
    if (!other_modes.z_source_sel)
        dzpix = spans_dzpix;
    else
    {
        dzpix = primitive_delta_z;
        dzinc = spans_cdz = spans_dzdy = 0;
    }
    int dzpixenc = dz_compress(dzpix);

    int cdith = 7, adith = 0;
    int r, g, b, a, z, s, t, w;
    int sr, sg, sb, sa, sz, ss, st, sw;
    int xstart, xend, xendsc;
    int sss = 0, sst = 0;
    int curpixel = 0;

    int x, length, scdiff;
    uint32_t fir, fig, fib;

    for (i = start; i <= end; i++)
    {
        if (span[i].validline)
        {

        xstart = span[i].lx;
        xend = span[i].unscrx;
        xendsc = span[i].rx;
        r = span[i].r;
        g = span[i].g;
        b = span[i].b;
        a = span[i].a;
        z = other_modes.z_source_sel ? primitive_z : span[i].z;
        s = span[i].s;
        t = span[i].t;
        w = span[i].w;

        x = xendsc;
        curpixel = fb_width * i + x;
        zbcur = zb + curpixel;

        if (!flip)
        {
            length = xendsc - xstart;
            scdiff = xend - xendsc;
            compute_cvg_noflip(i);
        }
        else
        {
            length = xstart - xendsc;
            scdiff = xendsc - xend;
            compute_cvg_flip(i);
        }

        if (scdiff)
        {
            scdiff &= 0xfff;
            r += (drinc * scdiff);
            g += (dginc * scdiff);
            b += (dbinc * scdiff);
            a += (dainc * scdiff);
            z += (dzinc * scdiff);
            s += (dsinc * scdiff);
            t += (dtinc * scdiff);
            w += (dwinc * scdiff);
        }

        for (j = 0; j <= length; j++)
        {
            sr = r >> 14;
            sg = g >> 14;
            sb = b >> 14;
            sa = a >> 14;
            ss = s >> 16;
            st = t >> 16;
            sw = w >> 16;
            sz = (z >> 10) & 0x3fffff;

            lookup_cvmask_derivatives(cvgbuf[x], &offx, &offy, &curpixel_cvg, &curpixel_cvbit);

            tcdiv_ptr(ss, st, sw, &sss, &sst);

            tclod_2cycle_current_notexel1(&sss, &sst, s, t, w, dsinc, dtinc, dwinc, prim_tile, &tile1);


            texture_pipeline_cycle(&texel0_color, &texel0_color, sss, sst, tile1, 0);

            rgbaz_correct_clip(offx, offy, sr, sg, sb, sa, &sz, curpixel_cvg);

            get_dither_noise_ptr(x, i, &cdith, &adith);
            combiner_2cycle(adith, &curpixel_cvg, &acalpha);

            fbread2_ptr(curpixel, &curpixel_memcvg);

            if (z_compare(zbcur, sz, dzpix, dzpixenc, &blend_en, &prewrap, &curpixel_cvg, curpixel_memcvg))
            {
                if (blender_2cycle(&fir, &fig, &fib, cdith, blend_en, prewrap, curpixel_cvg, curpixel_cvbit, acalpha))
                {
                    fbwrite_ptr(curpixel, fir, fig, fib, blend_en, curpixel_cvg, curpixel_memcvg);
                    if (other_modes.z_update_en)
                        z_store(zbcur, sz, dzpixenc);
                }

            }
            else
                memory_color = pre_memory_color;

            s += dsinc;
            t += dtinc;
            w += dwinc;
            r += drinc;
            g += dginc;
            b += dbinc;
            a += dainc;
            z += dzinc;

            x += xinc;
            curpixel += xinc;
            zbcur += xinc;
        }
        }
    }
}


static void render_spans_2cycle_notex(int start, int end, int tilenum, int flip)
{
    int zb = zb_address >> 1;
    int zbcur;
    uint8_t offx, offy;
    int i, j;
    uint32_t blend_en;
    uint32_t prewrap;
    uint32_t curpixel_cvg, curpixel_cvbit, curpixel_memcvg;
    int32_t acalpha;

    int drinc, dginc, dbinc, dainc, dzinc;
    int xinc;
    if (flip)
    {
        drinc = spans_dr;
        dginc = spans_dg;
        dbinc = spans_db;
        dainc = spans_da;
        dzinc = spans_dz;
        xinc = 1;
    }
    else
    {
        drinc = -spans_dr;
        dginc = -spans_dg;
        dbinc = -spans_db;
        dainc = -spans_da;
        dzinc = -spans_dz;
        xinc = -1;
    }

    int dzpix;
    if (!other_modes.z_source_sel)
        dzpix = spans_dzpix;
    else
    {
        dzpix = primitive_delta_z;
        dzinc = spans_cdz = spans_dzdy = 0;
    }
    int dzpixenc = dz_compress(dzpix);

    int cdith = 7, adith = 0;
    int r, g, b, a, z;
    int sr, sg, sb, sa, sz;
    int xstart, xend, xendsc;
    int curpixel = 0;

    int x, length, scdiff;
    uint32_t fir, fig, fib;

    for (i = start; i <= end; i++)
    {
        if (span[i].validline)
        {

        xstart = span[i].lx;
        xend = span[i].unscrx;
        xendsc = span[i].rx;
        r = span[i].r;
        g = span[i].g;
        b = span[i].b;
        a = span[i].a;
        z = other_modes.z_source_sel ? primitive_z : span[i].z;

        x = xendsc;
        curpixel = fb_width * i + x;
        zbcur = zb + curpixel;

        if (!flip)
        {
            length = xendsc - xstart;
            scdiff = xend - xendsc;
            compute_cvg_noflip(i);
        }
        else
        {
            length = xstart - xendsc;
            scdiff = xendsc - xend;
            compute_cvg_flip(i);
        }

        if (scdiff)
        {
            scdiff &= 0xfff;
            r += (drinc * scdiff);
            g += (dginc * scdiff);
            b += (dbinc * scdiff);
            a += (dainc * scdiff);
            z += (dzinc * scdiff);
        }

        for (j = 0; j <= length; j++)
        {
            sr = r >> 14;
            sg = g >> 14;
            sb = b >> 14;
            sa = a >> 14;
            sz = (z >> 10) & 0x3fffff;

            lookup_cvmask_derivatives(cvgbuf[x], &offx, &offy, &curpixel_cvg, &curpixel_cvbit);

            rgbaz_correct_clip(offx, offy, sr, sg, sb, sa, &sz, curpixel_cvg);

            get_dither_noise_ptr(x, i, &cdith, &adith);
            combiner_2cycle(adith, &curpixel_cvg, &acalpha);

            fbread2_ptr(curpixel, &curpixel_memcvg);

            if (z_compare(zbcur, sz, dzpix, dzpixenc, &blend_en, &prewrap, &curpixel_cvg, curpixel_memcvg))
            {
                if (blender_2cycle(&fir, &fig, &fib, cdith, blend_en, prewrap, curpixel_cvg, curpixel_cvbit, acalpha))
                {
                    fbwrite_ptr(curpixel, fir, fig, fib, blend_en, curpixel_cvg, curpixel_memcvg);
                    if (other_modes.z_update_en)
                        z_store(zbcur, sz, dzpixenc);
                }
            }
            else
                memory_color = pre_memory_color;

            r += drinc;
            g += dginc;
            b += dbinc;
            a += dainc;
            z += dzinc;

            x += xinc;
            curpixel += xinc;
            zbcur += xinc;
        }
        }
    }
}

static void render_spans_fill(int start, int end, int flip)
{
    if (fb_size == PIXEL_SIZE_4BIT)
    {
        rdp_pipeline_crashed = 1;
        return;
    }

    int i, j;

    int fastkillbits = other_modes.image_read_en || other_modes.z_compare_en;
    int slowkillbits = other_modes.z_update_en && !other_modes.z_source_sel && !fastkillbits;

    int xinc = flip ? 1 : -1;

    int xstart = 0, xendsc;
    int prevxstart;
    int curpixel = 0;
    int x, length;

    for (i = start; i <= end; i++)
    {
        prevxstart = xstart;
        xstart = span[i].lx;
        xendsc = span[i].rx;

        x = xendsc;
        curpixel = fb_width * i + x;
        length = flip ? (xstart - xendsc) : (xendsc - xstart);

        if (span[i].validline)
        {
            if (fastkillbits && length >= 0)
            {
                if (!onetimewarnings.fillmbitcrashes)
                    msg_warning("render_spans_fill: image_read_en %x z_update_en %x z_compare_en %x. RDP crashed",
                    other_modes.image_read_en, other_modes.z_update_en, other_modes.z_compare_en);
                onetimewarnings.fillmbitcrashes = 1;
                rdp_pipeline_crashed = 1;
                return;
            }







            for (j = 0; j <= length; j++)
            {
                fbfill_ptr(curpixel);

                x += xinc;
                curpixel += xinc;
            }

            if (slowkillbits && length >= 0)
            {
                if (!onetimewarnings.fillmbitcrashes)
                    msg_warning("render_spans_fill: image_read_en %x z_update_en %x z_compare_en %x z_source_sel %x. RDP crashed",
                    other_modes.image_read_en, other_modes.z_update_en, other_modes.z_compare_en, other_modes.z_source_sel);
                onetimewarnings.fillmbitcrashes = 1;
                rdp_pipeline_crashed = 1;
                return;
            }
        }
    }
}

static void render_spans_copy(int start, int end, int tilenum, int flip)
{
    int i, j, k;

    if (fb_size == PIXEL_SIZE_32BIT)
    {
        rdp_pipeline_crashed = 1;
        return;
    }

    int tile1 = tilenum;
    int prim_tile = tilenum;

    int dsinc, dtinc, dwinc;
    int xinc;
    if (flip)
    {
        dsinc = spans_ds;
        dtinc = spans_dt;
        dwinc = spans_dw;
        xinc = 1;
    }
    else
    {
        dsinc = -spans_ds;
        dtinc = -spans_dt;
        dwinc = -spans_dw;
        xinc = -1;
    }

    int xstart = 0, xendsc;
    int s = 0, t = 0, w = 0, ss = 0, st = 0, sw = 0, sss = 0, sst = 0, ssw = 0;
    int fb_index, length;
    int diff = 0;

    uint32_t hidword = 0, lowdword = 0;
    uint32_t hidword1 = 0, lowdword1 = 0;
    int fbadvance = (fb_size == PIXEL_SIZE_4BIT) ? 8 : 16 >> fb_size;
    uint32_t fbptr = 0;
    int fbptr_advance = flip ? 8 : -8;
    uint64_t copyqword = 0;
    uint32_t tempdword = 0, tempbyte = 0;
    int copywmask = 0, alphamask = 0;
    int bytesperpixel = (fb_size == PIXEL_SIZE_4BIT) ? 1 : (1 << (fb_size - 1));
    uint32_t fbendptr = 0;
    int32_t threshold, currthreshold;

#define PIXELS_TO_BYTES_SPECIAL4(pix, siz) ((siz) ? PIXELS_TO_BYTES(pix, siz) : (pix))

    for (i = start; i <= end; i++)
    {
        if (span[i].validline)
        {

        s = span[i].s;
        t = span[i].t;
        w = span[i].w;

        xstart = span[i].lx;
        xendsc = span[i].rx;

        fb_index = fb_width * i + xendsc;
        fbptr = fb_address + PIXELS_TO_BYTES_SPECIAL4(fb_index, fb_size);
        fbendptr = fb_address + PIXELS_TO_BYTES_SPECIAL4((fb_width * i + xstart), fb_size);
        length = flip ? (xstart - xendsc) : (xendsc - xstart);




        for (j = 0; j <= length; j += fbadvance)
        {
            ss = s >> 16;
            st = t >> 16;
            sw = w >> 16;

            tcdiv_ptr(ss, st, sw, &sss, &sst);

            tclod_copy(&sss, &sst, s, t, w, dsinc, dtinc, dwinc, prim_tile, &tile1);



            fetch_qword_copy(&hidword, &lowdword, sss, sst, tile1);



            if (fb_size == PIXEL_SIZE_16BIT || fb_size == PIXEL_SIZE_8BIT)
                copyqword = ((uint64_t)hidword << 32) | ((uint64_t)lowdword);
            else
                copyqword = 0;


            if (!other_modes.alpha_compare_en)
                alphamask = 0xff;
            else if (fb_size == PIXEL_SIZE_16BIT)
            {
                alphamask = 0;
                alphamask |= (((copyqword >> 48) & 1) ? 0xC0 : 0);
                alphamask |= (((copyqword >> 32) & 1) ? 0x30 : 0);
                alphamask |= (((copyqword >> 16) & 1) ? 0xC : 0);
                alphamask |= ((copyqword & 1) ? 0x3 : 0);
            }
            else if (fb_size == PIXEL_SIZE_8BIT)
            {
                alphamask = 0;
                threshold = (other_modes.dither_alpha_en) ? (irand() & 0xff) : blend_color.a;
                if (other_modes.dither_alpha_en)
                {
                    currthreshold = threshold;
                    alphamask |= (((copyqword >> 24) & 0xff) >= currthreshold ? 0xC0 : 0);
                    currthreshold = ((threshold & 3) << 6) | (threshold >> 2);
                    alphamask |= (((copyqword >> 16) & 0xff) >= currthreshold ? 0x30 : 0);
                    currthreshold = ((threshold & 0xf) << 4) | (threshold >> 4);
                    alphamask |= (((copyqword >> 8) & 0xff) >= currthreshold ? 0xC : 0);
                    currthreshold = ((threshold & 0x3f) << 2) | (threshold >> 6);
                    alphamask |= ((copyqword & 0xff) >= currthreshold ? 0x3 : 0);
                }
                else
                {
                    alphamask |= (((copyqword >> 24) & 0xff) >= threshold ? 0xC0 : 0);
                    alphamask |= (((copyqword >> 16) & 0xff) >= threshold ? 0x30 : 0);
                    alphamask |= (((copyqword >> 8) & 0xff) >= threshold ? 0xC : 0);
                    alphamask |= ((copyqword & 0xff) >= threshold ? 0x3 : 0);
                }
            }
            else
                alphamask = 0;

            copywmask = (flip) ? (fbendptr - fbptr + bytesperpixel) : (fbptr - fbendptr + bytesperpixel);

            if (copywmask > 8)
                copywmask = 8;
            tempdword = fbptr;
            k = 7;
            while(copywmask > 0)
            {
                tempbyte = (uint32_t)((copyqword >> (k << 3)) & 0xff);
                if (alphamask & (1 << k))
                {
                    PAIRWRITE8(tempdword, tempbyte, (tempbyte & 1) ? 3 : 0);
                }
                k--;
                tempdword += xinc;
                copywmask--;
            }

            s += dsinc;
            t += dtinc;
            w += dwinc;
            fbptr += fbptr_advance;
        }
        }
    }
}


static void loading_pipeline(int start, int end, int tilenum, int coord_quad, int ltlut)
{


    int localdebugmode = 0, cnt = 0;
    int i, j;

    int dsinc, dtinc;
    dsinc = spans_ds;
    dtinc = spans_dt;

    int s, t;
    int ss, st;
    int xstart, xend, xendsc;
    int sss = 0, sst = 0;
    int ti_index, length;

    uint32_t tmemidx0 = 0, tmemidx1 = 0, tmemidx2 = 0, tmemidx3 = 0;
    int dswap = 0;
    uint16_t* tmem16 = (uint16_t*)tmem;
    uint32_t readval0, readval1, readval2, readval3;
    uint32_t readidx32;
    uint64_t loadqword;
    uint16_t tempshort;
    int tmem_formatting = 0;
    uint32_t bit3fl = 0, hibit = 0;

    if (end > start && ltlut)
    {
        rdp_pipeline_crashed = 1;
        return;
    }

    if (tile[tilenum].format == FORMAT_YUV)
        tmem_formatting = 0;
    else if (tile[tilenum].format == FORMAT_RGBA && tile[tilenum].size == PIXEL_SIZE_32BIT)
        tmem_formatting = 1;
    else
        tmem_formatting = 2;

    int tiadvance = 0, spanadvance = 0;
    int tiptr = 0;
    switch (ti_size)
    {
    case PIXEL_SIZE_4BIT:
        rdp_pipeline_crashed = 1;
        return;
        break;
    case PIXEL_SIZE_8BIT:
        tiadvance = 8;
        spanadvance = 8;
        break;
    case PIXEL_SIZE_16BIT:
        if (!ltlut)
        {
            tiadvance = 8;
            spanadvance = 4;
        }
        else
        {
            tiadvance = 2;
            spanadvance = 1;
        }
        break;
    case PIXEL_SIZE_32BIT:
        tiadvance = 8;
        spanadvance = 2;
        break;
    }

    for (i = start; i <= end; i++)
    {
        xstart = span[i].lx;
        xend = span[i].unscrx;
        xendsc = span[i].rx;
        s = span[i].s;
        t = span[i].t;

        ti_index = ti_width * i + xend;
        tiptr = ti_address + PIXELS_TO_BYTES(ti_index, ti_size);

        length = (xstart - xend + 1) & 0xfff;

        if (trace_write_is_open()) {
            trace_write_rdram((tiptr >> 2), length);
        }

        for (j = 0; j < length; j+= spanadvance)
        {
            ss = s >> 16;
            st = t >> 16;







            sss = ss & 0xffff;
            sst = st & 0xffff;

            tc_pipeline_load(&sss, &sst, tilenum, coord_quad);

            dswap = sst & 1;


            get_tmem_idx(sss, sst, tilenum, &tmemidx0, &tmemidx1, &tmemidx2, &tmemidx3, &bit3fl, &hibit);

            readidx32 = (tiptr >> 2) & ~1;
            RREADIDX32(readval0, readidx32);
            readidx32++;
            RREADIDX32(readval1, readidx32);
            readidx32++;
            RREADIDX32(readval2, readidx32);
            readidx32++;
            RREADIDX32(readval3, readidx32);


            switch(tiptr & 7)
            {
            case 0:
                if (!ltlut)
                    loadqword = ((uint64_t)readval0 << 32) | readval1;
                else
                {
                    tempshort = readval0 >> 16;
                    loadqword = ((uint64_t)tempshort << 48) | ((uint64_t) tempshort << 32) | ((uint64_t) tempshort << 16) | tempshort;
                }
                break;
            case 1:
                loadqword = ((uint64_t)readval0 << 40) | ((uint64_t)readval1 << 8) | (readval2 >> 24);
                break;
            case 2:
                if (!ltlut)
                    loadqword = ((uint64_t)readval0 << 48) | ((uint64_t)readval1 << 16) | (readval2 >> 16);
                else
                {
                    tempshort = readval0 & 0xffff;
                    loadqword = ((uint64_t)tempshort << 48) | ((uint64_t) tempshort << 32) | ((uint64_t) tempshort << 16) | tempshort;
                }
                break;
            case 3:
                loadqword = ((uint64_t)readval0 << 56) | ((uint64_t)readval1 << 24) | (readval2 >> 8);
                break;
            case 4:
                if (!ltlut)
                    loadqword = ((uint64_t)readval1 << 32) | readval2;
                else
                {
                    tempshort = readval1 >> 16;
                    loadqword = ((uint64_t)tempshort << 48) | ((uint64_t) tempshort << 32) | ((uint64_t) tempshort << 16) | tempshort;
                }
                break;
            case 5:
                loadqword = ((uint64_t)readval1 << 40) | ((uint64_t)readval2 << 8) | (readval3 >> 24);
                break;
            case 6:
                if (!ltlut)
                    loadqword = ((uint64_t)readval1 << 48) | ((uint64_t)readval2 << 16) | (readval3 >> 16);
                else
                {
                    tempshort = readval1 & 0xffff;
                    loadqword = ((uint64_t)tempshort << 48) | ((uint64_t) tempshort << 32) | ((uint64_t) tempshort << 16) | tempshort;
                }
                break;
            case 7:
                loadqword = ((uint64_t)readval1 << 56) | ((uint64_t)readval2 << 24) | (readval3 >> 8);
                break;
            }


            switch(tmem_formatting)
            {
            case 0:
                readval0 = (uint32_t)((((loadqword >> 56) & 0xff) << 24) | (((loadqword >> 40) & 0xff) << 16) | (((loadqword >> 24) & 0xff) << 8) | (((loadqword >> 8) & 0xff) << 0));
                readval1 = (uint32_t)((((loadqword >> 48) & 0xff) << 24) | (((loadqword >> 32) & 0xff) << 16) | (((loadqword >> 16) & 0xff) << 8) | (((loadqword >> 0) & 0xff) << 0));
                if (bit3fl)
                {
                    tmem16[tmemidx2 ^ WORD_ADDR_XOR] = (uint16_t)(readval0 >> 16);
                    tmem16[tmemidx3 ^ WORD_ADDR_XOR] = (uint16_t)(readval0 & 0xffff);
                    tmem16[(tmemidx2 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(readval1 >> 16);
                    tmem16[(tmemidx3 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(readval1 & 0xffff);
                }
                else
                {
                    tmem16[tmemidx0 ^ WORD_ADDR_XOR] = (uint16_t)(readval0 >> 16);
                    tmem16[tmemidx1 ^ WORD_ADDR_XOR] = (uint16_t)(readval0 & 0xffff);
                    tmem16[(tmemidx0 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(readval1 >> 16);
                    tmem16[(tmemidx1 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(readval1 & 0xffff);
                }
                break;
            case 1:
                readval0 = (uint32_t)(((loadqword >> 48) << 16) | ((loadqword >> 16) & 0xffff));
                readval1 = (uint32_t)((((loadqword >> 32) & 0xffff) << 16) | (loadqword & 0xffff));

                if (bit3fl)
                {
                    tmem16[tmemidx2 ^ WORD_ADDR_XOR] = (uint16_t)(readval0 >> 16);
                    tmem16[tmemidx3 ^ WORD_ADDR_XOR] = (uint16_t)(readval0 & 0xffff);
                    tmem16[(tmemidx2 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(readval1 >> 16);
                    tmem16[(tmemidx3 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(readval1 & 0xffff);
                }
                else
                {
                    tmem16[tmemidx0 ^ WORD_ADDR_XOR] = (uint16_t)(readval0 >> 16);
                    tmem16[tmemidx1 ^ WORD_ADDR_XOR] = (uint16_t)(readval0 & 0xffff);
                    tmem16[(tmemidx0 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(readval1 >> 16);
                    tmem16[(tmemidx1 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(readval1 & 0xffff);
                }
                break;
            case 2:
                if (!dswap)
                {
                    if (!hibit)
                    {
                        tmem16[tmemidx0 ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 48);
                        tmem16[tmemidx1 ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 32);
                        tmem16[tmemidx2 ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 16);
                        tmem16[tmemidx3 ^ WORD_ADDR_XOR] = (uint16_t)(loadqword & 0xffff);
                    }
                    else
                    {
                        tmem16[(tmemidx0 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 48);
                        tmem16[(tmemidx1 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 32);
                        tmem16[(tmemidx2 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 16);
                        tmem16[(tmemidx3 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(loadqword & 0xffff);
                    }
                }
                else
                {
                    if (!hibit)
                    {
                        tmem16[tmemidx0 ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 16);
                        tmem16[tmemidx1 ^ WORD_ADDR_XOR] = (uint16_t)(loadqword & 0xffff);
                        tmem16[tmemidx2 ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 48);
                        tmem16[tmemidx3 ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 32);
                    }
                    else
                    {
                        tmem16[(tmemidx0 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 16);
                        tmem16[(tmemidx1 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(loadqword & 0xffff);
                        tmem16[(tmemidx2 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 48);
                        tmem16[(tmemidx3 | 0x400) ^ WORD_ADDR_XOR] = (uint16_t)(loadqword >> 32);
                    }
                }
            break;
            }


            s = (s + dsinc) & ~0x1f;
            t = (t + dtinc) & ~0x1f;
            tiptr += tiadvance;
        }
    }
}

static void edgewalker_for_prims(int32_t* ewdata)
{
    int j = 0;
    int xleft = 0, xright = 0, xleft_inc = 0, xright_inc = 0;
    int r = 0, g = 0, b = 0, a = 0, z = 0, s = 0, t = 0, w = 0;
    int dr = 0, dg = 0, db = 0, da = 0;
    int drdx = 0, dgdx = 0, dbdx = 0, dadx = 0, dzdx = 0, dsdx = 0, dtdx = 0, dwdx = 0;
    int drdy = 0, dgdy = 0, dbdy = 0, dady = 0, dzdy = 0, dsdy = 0, dtdy = 0, dwdy = 0;
    int drde = 0, dgde = 0, dbde = 0, dade = 0, dzde = 0, dsde = 0, dtde = 0, dwde = 0;
    int tilenum = 0, flip = 0;
    int32_t yl = 0, ym = 0, yh = 0;
    int32_t xl = 0, xm = 0, xh = 0;
    int32_t dxldy = 0, dxhdy = 0, dxmdy = 0;

    if (other_modes.f.stalederivs)
    {
        deduce_derivatives();
        other_modes.f.stalederivs = 0;
    }


    flip = (ewdata[0] & 0x800000) ? 1 : 0;
    max_level = (ewdata[0] >> 19) & 7;
    tilenum = (ewdata[0] >> 16) & 7;


    yl = SIGN(ewdata[0], 14);
    ym = ewdata[1] >> 16;
    ym = SIGN(ym, 14);
    yh = SIGN(ewdata[1], 14);

    xl = SIGN(ewdata[2], 28);
    xh = SIGN(ewdata[4], 28);
    xm = SIGN(ewdata[6], 28);

    dxldy = SIGN(ewdata[3], 30);



    dxhdy = SIGN(ewdata[5], 30);
    dxmdy = SIGN(ewdata[7], 30);


    r    = (ewdata[8] & 0xffff0000) | ((ewdata[12] >> 16) & 0x0000ffff);
    g    = ((ewdata[8] << 16) & 0xffff0000) | (ewdata[12] & 0x0000ffff);
    b    = (ewdata[9] & 0xffff0000) | ((ewdata[13] >> 16) & 0x0000ffff);
    a    = ((ewdata[9] << 16) & 0xffff0000) | (ewdata[13] & 0x0000ffff);
    drdx = (ewdata[10] & 0xffff0000) | ((ewdata[14] >> 16) & 0x0000ffff);
    dgdx = ((ewdata[10] << 16) & 0xffff0000) | (ewdata[14] & 0x0000ffff);
    dbdx = (ewdata[11] & 0xffff0000) | ((ewdata[15] >> 16) & 0x0000ffff);
    dadx = ((ewdata[11] << 16) & 0xffff0000) | (ewdata[15] & 0x0000ffff);
    drde = (ewdata[16] & 0xffff0000) | ((ewdata[20] >> 16) & 0x0000ffff);
    dgde = ((ewdata[16] << 16) & 0xffff0000) | (ewdata[20] & 0x0000ffff);
    dbde = (ewdata[17] & 0xffff0000) | ((ewdata[21] >> 16) & 0x0000ffff);
    dade = ((ewdata[17] << 16) & 0xffff0000) | (ewdata[21] & 0x0000ffff);
    drdy = (ewdata[18] & 0xffff0000) | ((ewdata[22] >> 16) & 0x0000ffff);
    dgdy = ((ewdata[18] << 16) & 0xffff0000) | (ewdata[22] & 0x0000ffff);
    dbdy = (ewdata[19] & 0xffff0000) | ((ewdata[23] >> 16) & 0x0000ffff);
    dady = ((ewdata[19] << 16) & 0xffff0000) | (ewdata[23] & 0x0000ffff);


    s    = (ewdata[24] & 0xffff0000) | ((ewdata[28] >> 16) & 0x0000ffff);
    t    = ((ewdata[24] << 16) & 0xffff0000)    | (ewdata[28] & 0x0000ffff);
    w    = (ewdata[25] & 0xffff0000) | ((ewdata[29] >> 16) & 0x0000ffff);
    dsdx = (ewdata[26] & 0xffff0000) | ((ewdata[30] >> 16) & 0x0000ffff);
    dtdx = ((ewdata[26] << 16) & 0xffff0000)    | (ewdata[30] & 0x0000ffff);
    dwdx = (ewdata[27] & 0xffff0000) | ((ewdata[31] >> 16) & 0x0000ffff);
    dsde = (ewdata[32] & 0xffff0000) | ((ewdata[36] >> 16) & 0x0000ffff);
    dtde = ((ewdata[32] << 16) & 0xffff0000)    | (ewdata[36] & 0x0000ffff);
    dwde = (ewdata[33] & 0xffff0000) | ((ewdata[37] >> 16) & 0x0000ffff);
    dsdy = (ewdata[34] & 0xffff0000) | ((ewdata[38] >> 16) & 0x0000ffff);
    dtdy = ((ewdata[34] << 16) & 0xffff0000)    | (ewdata[38] & 0x0000ffff);
    dwdy = (ewdata[35] & 0xffff0000) | ((ewdata[39] >> 16) & 0x0000ffff);


    z    = ewdata[40];
    dzdx = ewdata[41];
    dzde = ewdata[42];
    dzdy = ewdata[43];







    spans_ds = dsdx & ~0x1f;
    spans_dt = dtdx & ~0x1f;
    spans_dw = dwdx & ~0x1f;
    spans_dr = drdx & ~0x1f;
    spans_dg = dgdx & ~0x1f;
    spans_db = dbdx & ~0x1f;
    spans_da = dadx & ~0x1f;
    spans_dz = dzdx;


    spans_drdy = drdy >> 14;
    spans_dgdy = dgdy >> 14;
    spans_dbdy = dbdy >> 14;
    spans_dady = dady >> 14;
    spans_dzdy = dzdy >> 10;
    spans_drdy = SIGN(spans_drdy, 13);
    spans_dgdy = SIGN(spans_dgdy, 13);
    spans_dbdy = SIGN(spans_dbdy, 13);
    spans_dady = SIGN(spans_dady, 13);
    spans_dzdy = SIGN(spans_dzdy, 22);
    spans_cdr = spans_dr >> 14;
    spans_cdr = SIGN(spans_cdr, 13);
    spans_cdg = spans_dg >> 14;
    spans_cdg = SIGN(spans_cdg, 13);
    spans_cdb = spans_db >> 14;
    spans_cdb = SIGN(spans_cdb, 13);
    spans_cda = spans_da >> 14;
    spans_cda = SIGN(spans_cda, 13);
    spans_cdz = spans_dz >> 10;
    spans_cdz = SIGN(spans_cdz, 22);

    spans_dsdy = dsdy & ~0x7fff;
    spans_dtdy = dtdy & ~0x7fff;
    spans_dwdy = dwdy & ~0x7fff;


    int dzdy_dz = (dzdy >> 16) & 0xffff;
    int dzdx_dz = (dzdx >> 16) & 0xffff;

    spans_dzpix = ((dzdy_dz & 0x8000) ? ((~dzdy_dz) & 0x7fff) : dzdy_dz) + ((dzdx_dz & 0x8000) ? ((~dzdx_dz) & 0x7fff) : dzdx_dz);
    spans_dzpix = normalize_dzpix(spans_dzpix & 0xffff) & 0xffff;



    xleft_inc = (dxmdy >> 2) & ~0x1;
    xright_inc = (dxhdy >> 2) & ~0x1;



    xright = xh & ~0x1;
    xleft = xm & ~0x1;

    int k = 0;

    int dsdiff, dtdiff, dwdiff, drdiff, dgdiff, dbdiff, dadiff, dzdiff;
    int sign_dxhdy = (ewdata[5] & 0x80000000) ? 1 : 0;

    int dsdeh, dtdeh, dwdeh, drdeh, dgdeh, dbdeh, dadeh, dzdeh, dsdyh, dtdyh, dwdyh, drdyh, dgdyh, dbdyh, dadyh, dzdyh;
    int do_offset = !(sign_dxhdy ^ flip);

    if (do_offset)
    {
        dsdeh = dsde & ~0x1ff;
        dtdeh = dtde & ~0x1ff;
        dwdeh = dwde & ~0x1ff;
        drdeh = drde & ~0x1ff;
        dgdeh = dgde & ~0x1ff;
        dbdeh = dbde & ~0x1ff;
        dadeh = dade & ~0x1ff;
        dzdeh = dzde & ~0x1ff;

        dsdyh = dsdy & ~0x1ff;
        dtdyh = dtdy & ~0x1ff;
        dwdyh = dwdy & ~0x1ff;
        drdyh = drdy & ~0x1ff;
        dgdyh = dgdy & ~0x1ff;
        dbdyh = dbdy & ~0x1ff;
        dadyh = dady & ~0x1ff;
        dzdyh = dzdy & ~0x1ff;







        dsdiff = dsdeh - (dsdeh >> 2) - dsdyh + (dsdyh >> 2);
        dtdiff = dtdeh - (dtdeh >> 2) - dtdyh + (dtdyh >> 2);
        dwdiff = dwdeh - (dwdeh >> 2) - dwdyh + (dwdyh >> 2);
        drdiff = drdeh - (drdeh >> 2) - drdyh + (drdyh >> 2);
        dgdiff = dgdeh - (dgdeh >> 2) - dgdyh + (dgdyh >> 2);
        dbdiff = dbdeh - (dbdeh >> 2) - dbdyh + (dbdyh >> 2);
        dadiff = dadeh - (dadeh >> 2) - dadyh + (dadyh >> 2);
        dzdiff = dzdeh - (dzdeh >> 2) - dzdyh + (dzdyh >> 2);

    }
    else
        dsdiff = dtdiff = dwdiff = drdiff = dgdiff = dbdiff = dadiff = dzdiff = 0;

    int xfrac = 0;

    int dsdxh, dtdxh, dwdxh, drdxh, dgdxh, dbdxh, dadxh, dzdxh;
    if (other_modes.cycle_type != CYCLE_TYPE_COPY)
    {
        dsdxh = (dsdx >> 8) & ~1;
        dtdxh = (dtdx >> 8) & ~1;
        dwdxh = (dwdx >> 8) & ~1;
        drdxh = (drdx >> 8) & ~1;
        dgdxh = (dgdx >> 8) & ~1;
        dbdxh = (dbdx >> 8) & ~1;
        dadxh = (dadx >> 8) & ~1;
        dzdxh = (dzdx >> 8) & ~1;
    }
    else
        dsdxh = dtdxh = dwdxh = drdxh = dgdxh = dbdxh = dadxh = dzdxh = 0;





#define ADJUST_ATTR_PRIM()      \
{                           \
    span[j].s = ((s & ~0x1ff) + dsdiff - (xfrac * dsdxh)) & ~0x3ff;             \
    span[j].t = ((t & ~0x1ff) + dtdiff - (xfrac * dtdxh)) & ~0x3ff;             \
    span[j].w = ((w & ~0x1ff) + dwdiff - (xfrac * dwdxh)) & ~0x3ff;             \
    span[j].r = ((r & ~0x1ff) + drdiff - (xfrac * drdxh)) & ~0x3ff;             \
    span[j].g = ((g & ~0x1ff) + dgdiff - (xfrac * dgdxh)) & ~0x3ff;             \
    span[j].b = ((b & ~0x1ff) + dbdiff - (xfrac * dbdxh)) & ~0x3ff;             \
    span[j].a = ((a & ~0x1ff) + dadiff - (xfrac * dadxh)) & ~0x3ff;             \
    span[j].z = ((z & ~0x1ff) + dzdiff - (xfrac * dzdxh)) & ~0x3ff;             \
}


#define ADDVALUES_PRIM() {  \
            s += dsde;  \
            t += dtde;  \
            w += dwde; \
            r += drde; \
            g += dgde; \
            b += dbde; \
            a += dade; \
            z += dzde; \
}

    int32_t maxxmx, minxmx, maxxhx, minxhx;

    int spix = 0;
    int ycur =  yh & ~3;
    int ldflag = (sign_dxhdy ^ flip) ? 0 : 3;
    int invaly = 1;
    int length = 0;
    int32_t xrsc = 0, xlsc = 0, stickybit = 0;
    int32_t yllimit = 0, yhlimit = 0;
    if (yl & 0x2000)
        yllimit = 1;
    else if (yl & 0x1000)
        yllimit = 0;
    else
        yllimit = (yl & 0xfff) < clip.yl;
    yllimit = yllimit ? yl : clip.yl;

    int ylfar = yllimit | 3;
    if ((yl >> 2) > (ylfar >> 2))
        ylfar += 4;
    else if ((yllimit >> 2) >= 0 && (yllimit >> 2) < 1023)
        span[(yllimit >> 2) + 1].validline = 0;


    if (yh & 0x2000)
        yhlimit = 0;
    else if (yh & 0x1000)
        yhlimit = 1;
    else
        yhlimit = (yh >= clip.yh);
    yhlimit = yhlimit ? yh : clip.yh;

    int yhclose = yhlimit & ~3;

    int32_t clipxlshift = clip.xl << 1;
    int32_t clipxhshift = clip.xh << 1;
    int allover = 1, allunder = 1, curover = 0, curunder = 0;
    int allinval = 1;
    int32_t curcross = 0;

    xfrac = ((xright >> 8) & 0xff);


    uint32_t worker_id = parallel_worker_id();
    uint32_t worker_num = parallel_worker_num();

    if (flip)
    {
    for (k = ycur; k <= ylfar; k++)
    {
        if (k == ym)
        {

            xleft = xl & ~1;
            xleft_inc = (dxldy >> 2) & ~1;
        }

        spix = k & 3;

        if (k >= yhclose)
        {
            invaly = k < yhlimit || k >= yllimit;

            j = k >> 2;

            if (spix == 0)
            {
                maxxmx = 0;
                minxhx = 0xfff;
                allover = allunder = 1;
                allinval = 1;
            }

            stickybit = ((xright >> 1) & 0x1fff) > 0;
            xrsc = ((xright >> 13) & 0x1ffe) | stickybit;


            curunder = ((xright & 0x8000000) || (xrsc < clipxhshift && !(xright & 0x4000000)));

            xrsc = curunder ? clipxhshift : (((xright >> 13) & 0x3ffe) | stickybit);
            curover = ((xrsc & 0x2000) || (xrsc & 0x1fff) >= clipxlshift);
            xrsc = curover ? clipxlshift : xrsc;
            span[j].majorx[spix] = xrsc & 0x1fff;
            allover &= curover;
            allunder &= curunder;

            stickybit = ((xleft >> 1) & 0x1fff) > 0;
            xlsc = ((xleft >> 13) & 0x1ffe) | stickybit;
            curunder = ((xleft & 0x8000000) || (xlsc < clipxhshift && !(xleft & 0x4000000)));
            xlsc = curunder ? clipxhshift : (((xleft >> 13) & 0x3ffe) | stickybit);
            curover = ((xlsc & 0x2000) || (xlsc & 0x1fff) >= clipxlshift);
            xlsc = curover ? clipxlshift : xlsc;
            span[j].minorx[spix] = xlsc & 0x1fff;
            allover &= curover;
            allunder &= curunder;



            curcross = ((xleft ^ (1 << 27)) & (0x3fff << 14)) < ((xright ^ (1 << 27)) & (0x3fff << 14));


            invaly |= curcross;
            span[j].invalyscan[spix] = invaly;
            allinval &= invaly;

            if (!invaly)
            {
                maxxmx = (((xlsc >> 3) & 0xfff) > maxxmx) ? (xlsc >> 3) & 0xfff : maxxmx;
                minxhx = (((xrsc >> 3) & 0xfff) < minxhx) ? (xrsc >> 3) & 0xfff : minxhx;
            }

            if (spix == ldflag)
            {




                span[j].unscrx = SIGN(xright >> 16, 12);
                xfrac = (xright >> 8) & 0xff;
                ADJUST_ATTR_PRIM();
            }

            if (spix == 3)
            {
                span[j].lx = maxxmx;
                span[j].rx = minxhx;
                span[j].validline  = !allinval && !allover && !allunder && (!scfield || (scfield && !(sckeepodd ^ (j & 1)))) && (config->num_workers == 1 || j % worker_num == worker_id);

            }


        }

        if (spix == 3)
        {
            ADDVALUES_PRIM();
        }



        xleft += xleft_inc;
        xright += xright_inc;

    }
    }
    else
    {
    for (k = ycur; k <= ylfar; k++)
    {
        if (k == ym)
        {
            xleft = xl & ~1;
            xleft_inc = (dxldy >> 2) & ~1;
        }

        spix = k & 3;

        if (k >= yhclose)
        {
            invaly = k < yhlimit || k >= yllimit;
            j = k >> 2;

            if (spix == 0)
            {
                maxxhx = 0;
                minxmx = 0xfff;
                allover = allunder = 1;
                allinval = 1;
            }

            stickybit = ((xright >> 1) & 0x1fff) > 0;
            xrsc = ((xright >> 13) & 0x1ffe) | stickybit;
            curunder = ((xright & 0x8000000) || (xrsc < clipxhshift && !(xright & 0x4000000)));
            xrsc = curunder ? clipxhshift : (((xright >> 13) & 0x3ffe) | stickybit);
            curover = ((xrsc & 0x2000) || (xrsc & 0x1fff) >= clipxlshift);
            xrsc = curover ? clipxlshift : xrsc;
            span[j].majorx[spix] = xrsc & 0x1fff;
            allover &= curover;
            allunder &= curunder;

            stickybit = ((xleft >> 1) & 0x1fff) > 0;
            xlsc = ((xleft >> 13) & 0x1ffe) | stickybit;
            curunder = ((xleft & 0x8000000) || (xlsc < clipxhshift && !(xleft & 0x4000000)));
            xlsc = curunder ? clipxhshift : (((xleft >> 13) & 0x3ffe) | stickybit);
            curover = ((xlsc & 0x2000) || (xlsc & 0x1fff) >= clipxlshift);
            xlsc = curover ? clipxlshift : xlsc;
            span[j].minorx[spix] = xlsc & 0x1fff;
            allover &= curover;
            allunder &= curunder;

            curcross = ((xright ^ (1 << 27)) & (0x3fff << 14)) < ((xleft ^ (1 << 27)) & (0x3fff << 14));

            invaly |= curcross;
            span[j].invalyscan[spix] = invaly;
            allinval &= invaly;

            if (!invaly)
            {
                minxmx = (((xlsc >> 3) & 0xfff) < minxmx) ? (xlsc >> 3) & 0xfff : minxmx;
                maxxhx = (((xrsc >> 3) & 0xfff) > maxxhx) ? (xrsc >> 3) & 0xfff : maxxhx;
            }

            if (spix == ldflag)
            {
                span[j].unscrx  = SIGN(xright >> 16, 12);
                xfrac = (xright >> 8) & 0xff;
                ADJUST_ATTR_PRIM();
            }

            if (spix == 3)
            {
                span[j].lx = minxmx;
                span[j].rx = maxxhx;
                span[j].validline  = !allinval && !allover && !allunder && (!scfield || (scfield && !(sckeepodd ^ (j & 1)))) && (config->num_workers == 1 || j % worker_num == worker_id);
            }

        }

        if (spix == 3)
        {
            ADDVALUES_PRIM();
        }

        xleft += xleft_inc;
        xright += xright_inc;

    }
    }




    switch(other_modes.cycle_type)
    {
        case CYCLE_TYPE_1: render_spans_1cycle_ptr(yhlimit >> 2, yllimit >> 2, tilenum, flip); break;
        case CYCLE_TYPE_2: render_spans_2cycle_ptr(yhlimit >> 2, yllimit >> 2, tilenum, flip); break;
        case CYCLE_TYPE_COPY: render_spans_copy(yhlimit >> 2, yllimit >> 2, tilenum, flip); break;
        case CYCLE_TYPE_FILL: render_spans_fill(yhlimit >> 2, yllimit >> 2, flip); break;
        default: msg_error("cycle_type %d", other_modes.cycle_type); break;
    }


}



static void edgewalker_for_loads(int32_t* lewdata)
{
    int j = 0;
    int xleft = 0, xright = 0;
    int xstart = 0, xend = 0;
    int s = 0, t = 0, w = 0;
    int dsdx = 0, dtdx = 0;
    int dsdy = 0, dtdy = 0;
    int dsde = 0, dtde = 0;
    int tilenum = 0, flip = 0;
    int32_t yl = 0, ym = 0, yh = 0;
    int32_t xl = 0, xm = 0, xh = 0;
    int32_t dxldy = 0, dxhdy = 0, dxmdy = 0;

    int cmd_id = CMD_ID(lewdata);
    int ltlut = (cmd_id == CMD_ID_LOAD_TLUT);
    int coord_quad = ltlut || (cmd_id == CMD_ID_LOAD_BLOCK);
    flip = 1;
    max_level = 0;
    tilenum = (lewdata[0] >> 16) & 7;


    yl = SIGN(lewdata[0], 14);
    ym = lewdata[1] >> 16;
    ym = SIGN(ym, 14);
    yh = SIGN(lewdata[1], 14);

    xl = SIGN(lewdata[2], 28);
    xh = SIGN(lewdata[3], 28);
    xm = SIGN(lewdata[4], 28);

    dxldy = 0;
    dxhdy = 0;
    dxmdy = 0;


    s    = lewdata[5] & 0xffff0000;
    t    = (lewdata[5] & 0xffff) << 16;
    w    = 0;
    dsdx = (lewdata[7] & 0xffff0000) | ((lewdata[6] >> 16) & 0xffff);
    dtdx = ((lewdata[7] << 16) & 0xffff0000)    | (lewdata[6] & 0xffff);
    dsde = 0;
    dtde = (lewdata[9] & 0xffff) << 16;
    dsdy = 0;
    dtdy = (lewdata[8] & 0xffff) << 16;

    spans_ds = dsdx & ~0x1f;
    spans_dt = dtdx & ~0x1f;
    spans_dw = 0;






    xright = xh & ~0x1;
    xleft = xm & ~0x1;

    int k = 0;

    int sign_dxhdy = 0;

    int do_offset = 0;

    int xfrac = 0;






#define ADJUST_ATTR_LOAD()                                      \
{                                                               \
    span[j].s = s & ~0x3ff;                                     \
    span[j].t = t & ~0x3ff;                                     \
}


#define ADDVALUES_LOAD() {  \
            t += dtde;      \
}

    int32_t maxxmx, minxhx;

    int spix = 0;
    int ycur =  yh & ~3;
    int ylfar = yl | 3;

    int valid_y = 1;
    int length = 0;
    int32_t xrsc = 0, xlsc = 0, stickybit = 0;
    int32_t yllimit = yl;
    int32_t yhlimit = yh;

    xfrac = 0;
    xend = xright >> 16;


    for (k = ycur; k <= ylfar; k++)
    {
        if (k == ym)
            xleft = xl & ~1;

        spix = k & 3;

        if (!(k & ~0xfff))
        {
            j = k >> 2;
            valid_y = !(k < yhlimit || k >= yllimit);

            if (spix == 0)
            {
                maxxmx = 0;
                minxhx = 0xfff;
            }

            xrsc = (xright >> 13) & 0x7ffe;



            xlsc = (xleft >> 13) & 0x7ffe;

            if (valid_y)
            {
                maxxmx = (((xlsc >> 3) & 0xfff) > maxxmx) ? (xlsc >> 3) & 0xfff : maxxmx;
                minxhx = (((xrsc >> 3) & 0xfff) < minxhx) ? (xrsc >> 3) & 0xfff : minxhx;
            }

            if (spix == 0)
            {
                span[j].unscrx = xend;
                ADJUST_ATTR_LOAD();
            }

            if (spix == 3)
            {
                span[j].lx = maxxmx;
                span[j].rx = minxhx;


            }


        }

        if (spix == 3)
        {
            ADDVALUES_LOAD();
        }



    }

    loading_pipeline(yhlimit >> 2, yllimit >> 2, tilenum, coord_quad, ltlut);
}


static const char *const image_format[] = { "RGBA", "YUV", "CI", "IA", "I", "???", "???", "???" };
static const char *const image_size[] = { "4-bit", "8-bit", "16-bit", "32-bit" };


static void rdp_invalid(const uint32_t* args)
{
}

static void rdp_noop(const uint32_t* args)
{
}

static void rdp_tri_noshade(const uint32_t* args)
{
    int32_t ewdata[CMD_MAX_INTS];
    memcpy(&ewdata[0], args, 8 * sizeof(int32_t));
    memset(&ewdata[8], 0, 36 * sizeof(int32_t));
    edgewalker_for_prims(ewdata);
}

static void rdp_tri_noshade_z(const uint32_t* args)
{
    int32_t ewdata[CMD_MAX_INTS];
    memcpy(&ewdata[0], args, 8 * sizeof(int32_t));
    memset(&ewdata[8], 0, 32 * sizeof(int32_t));
    memcpy(&ewdata[40], args + 8, 4 * sizeof(int32_t));
    edgewalker_for_prims(ewdata);
}

static void rdp_tri_tex(const uint32_t* args)
{
    int32_t ewdata[CMD_MAX_INTS];
    memcpy(&ewdata[0], args, 8 * sizeof(int32_t));
    memset(&ewdata[8], 0, 16 * sizeof(int32_t));
    memcpy(&ewdata[24], args + 8, 16 * sizeof(int32_t));
    memset(&ewdata[40], 0, 4 * sizeof(int32_t));
    edgewalker_for_prims(ewdata);
}

static void rdp_tri_tex_z(const uint32_t* args)
{
    int32_t ewdata[CMD_MAX_INTS];
    memcpy(&ewdata[0], args, 8 * sizeof(int32_t));
    memset(&ewdata[8], 0, 16 * sizeof(int32_t));
    memcpy(&ewdata[24], args + 8, 16 * sizeof(int32_t));
    memcpy(&ewdata[40], args + 24, 4 * sizeof(int32_t));






    edgewalker_for_prims(ewdata);


}

static void rdp_tri_shade(const uint32_t* args)
{
    int32_t ewdata[CMD_MAX_INTS];
    memcpy(&ewdata[0], args, 24 * sizeof(int32_t));
    memset(&ewdata[24], 0, 20 * sizeof(int32_t));
    edgewalker_for_prims(ewdata);
}

static void rdp_tri_shade_z(const uint32_t* args)
{
    int32_t ewdata[CMD_MAX_INTS];
    memcpy(&ewdata[0], args, 24 * sizeof(int32_t));
    memset(&ewdata[24], 0, 16 * sizeof(int32_t));
    memcpy(&ewdata[40], args + 24, 4 * sizeof(int32_t));
    edgewalker_for_prims(ewdata);
}

static void rdp_tri_texshade(const uint32_t* args)
{
    int32_t ewdata[CMD_MAX_INTS];
    memcpy(&ewdata[0], args, 40 * sizeof(int32_t));
    memset(&ewdata[40], 0, 4 * sizeof(int32_t));
    edgewalker_for_prims(ewdata);
}

static void rdp_tri_texshade_z(const uint32_t* args)
{
    int32_t ewdata[CMD_MAX_INTS];
    memcpy(&ewdata[0], args, CMD_MAX_SIZE);





    edgewalker_for_prims(ewdata);


}

static void rdp_tex_rect(const uint32_t* args)
{
    uint32_t tilenum    = (args[1] >> 24) & 0x7;
    uint32_t xl = (args[0] >> 12) & 0xfff;
    uint32_t yl = (args[0] >>  0) & 0xfff;
    uint32_t xh = (args[1] >> 12) & 0xfff;
    uint32_t yh = (args[1] >>  0) & 0xfff;

    int32_t s = (args[2] >> 16) & 0xffff;
    int32_t t = (args[2] >>  0) & 0xffff;
    int32_t dsdx = (args[3] >> 16) & 0xffff;
    int32_t dtdy = (args[3] >>  0) & 0xffff;

    dsdx = SIGN16(dsdx);
    dtdy = SIGN16(dtdy);

    if (other_modes.cycle_type == CYCLE_TYPE_FILL || other_modes.cycle_type == CYCLE_TYPE_COPY)
        yl |= 3;

    uint32_t xlint = (xl >> 2) & 0x3ff;
    uint32_t xhint = (xh >> 2) & 0x3ff;

    int32_t ewdata[CMD_MAX_INTS];
    ewdata[0] = (0x24 << 24) | ((0x80 | tilenum) << 16) | yl;
    ewdata[1] = (yl << 16) | yh;
    ewdata[2] = (xlint << 16) | ((xl & 3) << 14);
    ewdata[3] = 0;
    ewdata[4] = (xhint << 16) | ((xh & 3) << 14);
    ewdata[5] = 0;
    ewdata[6] = (xlint << 16) | ((xl & 3) << 14);
    ewdata[7] = 0;
    memset(&ewdata[8], 0, 16 * sizeof(uint32_t));
    ewdata[24] = (s << 16) | t;
    ewdata[25] = 0;
    ewdata[26] = ((dsdx >> 5) << 16);
    ewdata[27] = 0;
    ewdata[28] = 0;
    ewdata[29] = 0;
    ewdata[30] = ((dsdx & 0x1f) << 11) << 16;
    ewdata[31] = 0;
    ewdata[32] = (dtdy >> 5) & 0xffff;
    ewdata[33] = 0;
    ewdata[34] = (dtdy >> 5) & 0xffff;
    ewdata[35] = 0;
    ewdata[36] = (dtdy & 0x1f) << 11;
    ewdata[37] = 0;
    ewdata[38] = (dtdy & 0x1f) << 11;
    ewdata[39] = 0;
    memset(&ewdata[40], 0, 4 * sizeof(int32_t));



    edgewalker_for_prims(ewdata);

}

static void rdp_tex_rect_flip(const uint32_t* args)
{
    uint32_t tilenum    = (args[1] >> 24) & 0x7;
    uint32_t xl = (args[0] >> 12) & 0xfff;
    uint32_t yl = (args[0] >>  0) & 0xfff;
    uint32_t xh = (args[1] >> 12) & 0xfff;
    uint32_t yh = (args[1] >>  0) & 0xfff;

    int32_t s = (args[2] >> 16) & 0xffff;
    int32_t t = (args[2] >>  0) & 0xffff;
    int32_t dsdx = (args[3] >> 16) & 0xffff;
    int32_t dtdy = (args[3] >>  0) & 0xffff;

    dsdx = SIGN16(dsdx);
    dtdy = SIGN16(dtdy);

    if (other_modes.cycle_type == CYCLE_TYPE_FILL || other_modes.cycle_type == CYCLE_TYPE_COPY)
        yl |= 3;

    uint32_t xlint = (xl >> 2) & 0x3ff;
    uint32_t xhint = (xh >> 2) & 0x3ff;

    int32_t ewdata[CMD_MAX_INTS];
    ewdata[0] = (0x25 << 24) | ((0x80 | tilenum) << 16) | yl;
    ewdata[1] = (yl << 16) | yh;
    ewdata[2] = (xlint << 16) | ((xl & 3) << 14);
    ewdata[3] = 0;
    ewdata[4] = (xhint << 16) | ((xh & 3) << 14);
    ewdata[5] = 0;
    ewdata[6] = (xlint << 16) | ((xl & 3) << 14);
    ewdata[7] = 0;
    memset(&ewdata[8], 0, 16 * sizeof(int32_t));
    ewdata[24] = (s << 16) | t;
    ewdata[25] = 0;

    ewdata[26] = (dtdy >> 5) & 0xffff;
    ewdata[27] = 0;
    ewdata[28] = 0;
    ewdata[29] = 0;
    ewdata[30] = ((dtdy & 0x1f) << 11);
    ewdata[31] = 0;
    ewdata[32] = (dsdx >> 5) << 16;
    ewdata[33] = 0;
    ewdata[34] = (dsdx >> 5) << 16;
    ewdata[35] = 0;
    ewdata[36] = (dsdx & 0x1f) << 27;
    ewdata[37] = 0;
    ewdata[38] = (dsdx & 0x1f) << 27;
    ewdata[39] = 0;
    memset(&ewdata[40], 0, 4 * sizeof(int32_t));

    edgewalker_for_prims(ewdata);
}



static void rdp_sync_load(const uint32_t* args)
{

}

static void rdp_sync_pipe(const uint32_t* args)
{


}

static void rdp_sync_tile(const uint32_t* args)
{

}

static void rdp_sync_full(const uint32_t* args)
{
    core_sync_dp();
}

static void rdp_set_key_gb(const uint32_t* args)
{
    key_width.g = (args[0] >> 12) & 0xfff;
    key_width.b = args[0] & 0xfff;
    key_center.g = (args[1] >> 24) & 0xff;
    key_scale.g = (args[1] >> 16) & 0xff;
    key_center.b = (args[1] >> 8) & 0xff;
    key_scale.b = args[1] & 0xff;
}

static void rdp_set_key_r(const uint32_t* args)
{
    key_width.r = (args[1] >> 16) & 0xfff;
    key_center.r = (args[1] >> 8) & 0xff;
    key_scale.r = args[1] & 0xff;
}

static void rdp_set_convert(const uint32_t* args)
{
    int32_t k0 = (args[0] >> 13) & 0x1ff;
    int32_t k1 = (args[0] >> 4) & 0x1ff;
    int32_t k2 = ((args[0] & 0xf) << 5) | ((args[1] >> 27) & 0x1f);
    int32_t k3 = (args[1] >> 18) & 0x1ff;
    k0_tf = (SIGN(k0, 9) << 1) + 1;
    k1_tf = (SIGN(k1, 9) << 1) + 1;
    k2_tf = (SIGN(k2, 9) << 1) + 1;
    k3_tf = (SIGN(k3, 9) << 1) + 1;
    k4 = (args[1] >> 9) & 0x1ff;
    k5 = args[1] & 0x1ff;
}

static void rdp_set_scissor(const uint32_t* args)
{
    clip.xh = (args[0] >> 12) & 0xfff;
    clip.yh = (args[0] >>  0) & 0xfff;
    clip.xl = (args[1] >> 12) & 0xfff;
    clip.yl = (args[1] >>  0) & 0xfff;

    scfield = (args[1] >> 25) & 1;
    sckeepodd = (args[1] >> 24) & 1;
}

static void rdp_set_prim_depth(const uint32_t* args)
{
    primitive_z = args[1] & (0x7fff << 16);


    primitive_delta_z = (uint16_t)(args[1]);
}

static void rdp_set_other_modes(const uint32_t* args)
{
    other_modes.cycle_type          = (args[0] >> 20) & 0x3;
    other_modes.persp_tex_en        = (args[0] & 0x80000) ? 1 : 0;
    other_modes.detail_tex_en       = (args[0] & 0x40000) ? 1 : 0;
    other_modes.sharpen_tex_en      = (args[0] & 0x20000) ? 1 : 0;
    other_modes.tex_lod_en          = (args[0] & 0x10000) ? 1 : 0;
    other_modes.en_tlut             = (args[0] & 0x08000) ? 1 : 0;
    other_modes.tlut_type           = (args[0] & 0x04000) ? 1 : 0;
    other_modes.sample_type         = (args[0] & 0x02000) ? 1 : 0;
    other_modes.mid_texel           = (args[0] & 0x01000) ? 1 : 0;
    other_modes.bi_lerp0            = (args[0] & 0x00800) ? 1 : 0;
    other_modes.bi_lerp1            = (args[0] & 0x00400) ? 1 : 0;
    other_modes.convert_one         = (args[0] & 0x00200) ? 1 : 0;
    other_modes.key_en              = (args[0] & 0x00100) ? 1 : 0;
    other_modes.rgb_dither_sel      = (args[0] >> 6) & 0x3;
    other_modes.alpha_dither_sel    = (args[0] >> 4) & 0x3;
    other_modes.blend_m1a_0         = (args[1] >> 30) & 0x3;
    other_modes.blend_m1a_1         = (args[1] >> 28) & 0x3;
    other_modes.blend_m1b_0         = (args[1] >> 26) & 0x3;
    other_modes.blend_m1b_1         = (args[1] >> 24) & 0x3;
    other_modes.blend_m2a_0         = (args[1] >> 22) & 0x3;
    other_modes.blend_m2a_1         = (args[1] >> 20) & 0x3;
    other_modes.blend_m2b_0         = (args[1] >> 18) & 0x3;
    other_modes.blend_m2b_1         = (args[1] >> 16) & 0x3;
    other_modes.force_blend         = (args[1] >> 14) & 1;
    other_modes.alpha_cvg_select    = (args[1] >> 13) & 1;
    other_modes.cvg_times_alpha     = (args[1] >> 12) & 1;
    other_modes.z_mode              = (args[1] >> 10) & 0x3;
    other_modes.cvg_dest            = (args[1] >> 8) & 0x3;
    other_modes.color_on_cvg        = (args[1] >> 7) & 1;
    other_modes.image_read_en       = (args[1] >> 6) & 1;
    other_modes.z_update_en         = (args[1] >> 5) & 1;
    other_modes.z_compare_en        = (args[1] >> 4) & 1;
    other_modes.antialias_en        = (args[1] >> 3) & 1;
    other_modes.z_source_sel        = (args[1] >> 2) & 1;
    other_modes.dither_alpha_en     = (args[1] >> 1) & 1;
    other_modes.alpha_compare_en    = (args[1]) & 1;

    set_blender_input(0, 0, &blender1a_r[0], &blender1a_g[0], &blender1a_b[0], &blender1b_a[0],
                      other_modes.blend_m1a_0, other_modes.blend_m1b_0);
    set_blender_input(0, 1, &blender2a_r[0], &blender2a_g[0], &blender2a_b[0], &blender2b_a[0],
                      other_modes.blend_m2a_0, other_modes.blend_m2b_0);
    set_blender_input(1, 0, &blender1a_r[1], &blender1a_g[1], &blender1a_b[1], &blender1b_a[1],
                      other_modes.blend_m1a_1, other_modes.blend_m1b_1);
    set_blender_input(1, 1, &blender2a_r[1], &blender2a_g[1], &blender2a_b[1], &blender2b_a[1],
                      other_modes.blend_m2a_1, other_modes.blend_m2b_1);

    other_modes.f.stalederivs = 1;
}

static void deduce_derivatives()
{

    other_modes.f.partialreject_1cycle = (blender2b_a[0] == &inv_pixel_color.a && blender1b_a[0] == &pixel_color.a);
    other_modes.f.partialreject_2cycle = (blender2b_a[1] == &inv_pixel_color.a && blender1b_a[1] == &pixel_color.a);


    other_modes.f.special_bsel0 = (blender2b_a[0] == &memory_color.a);
    other_modes.f.special_bsel1 = (blender2b_a[1] == &memory_color.a);


    other_modes.f.realblendershiftersneeded = (other_modes.f.special_bsel0 && other_modes.cycle_type == CYCLE_TYPE_1) || (other_modes.f.special_bsel1 && other_modes.cycle_type == CYCLE_TYPE_2);
    other_modes.f.interpixelblendershiftersneeded = (other_modes.f.special_bsel0 && other_modes.cycle_type == CYCLE_TYPE_2);

    other_modes.f.rgb_alpha_dither = (other_modes.rgb_dither_sel << 2) | other_modes.alpha_dither_sel;

    if (other_modes.rgb_dither_sel == 3)
        rgb_dither_ptr = rgb_dither_func[1];
    else
        rgb_dither_ptr = rgb_dither_func[0];

    tcdiv_ptr = tcdiv_func[other_modes.persp_tex_en];


    int texel1_used_in_cc1 = 0, texel0_used_in_cc1 = 0, texel0_used_in_cc0 = 0, texel1_used_in_cc0 = 0;
    int texels_in_cc0 = 0, texels_in_cc1 = 0;
    int lod_frac_used_in_cc1 = 0, lod_frac_used_in_cc0 = 0;

    if ((combiner_rgbmul_r[1] == &lod_frac) || (combiner_alphamul[1] == &lod_frac))
        lod_frac_used_in_cc1 = 1;
    if ((combiner_rgbmul_r[0] == &lod_frac) || (combiner_alphamul[0] == &lod_frac))
        lod_frac_used_in_cc0 = 1;

    if (combiner_rgbmul_r[1] == &texel1_color.r || combiner_rgbsub_a_r[1] == &texel1_color.r || combiner_rgbsub_b_r[1] == &texel1_color.r || combiner_rgbadd_r[1] == &texel1_color.r || \
        combiner_alphamul[1] == &texel1_color.a || combiner_alphasub_a[1] == &texel1_color.a || combiner_alphasub_b[1] == &texel1_color.a || combiner_alphaadd[1] == &texel1_color.a || \
        combiner_rgbmul_r[1] == &texel1_color.a)
        texel1_used_in_cc1 = 1;
    if (combiner_rgbmul_r[1] == &texel0_color.r || combiner_rgbsub_a_r[1] == &texel0_color.r || combiner_rgbsub_b_r[1] == &texel0_color.r || combiner_rgbadd_r[1] == &texel0_color.r || \
        combiner_alphamul[1] == &texel0_color.a || combiner_alphasub_a[1] == &texel0_color.a || combiner_alphasub_b[1] == &texel0_color.a || combiner_alphaadd[1] == &texel0_color.a || \
        combiner_rgbmul_r[1] == &texel0_color.a)
        texel0_used_in_cc1 = 1;
    if (combiner_rgbmul_r[0] == &texel1_color.r || combiner_rgbsub_a_r[0] == &texel1_color.r || combiner_rgbsub_b_r[0] == &texel1_color.r || combiner_rgbadd_r[0] == &texel1_color.r || \
        combiner_alphamul[0] == &texel1_color.a || combiner_alphasub_a[0] == &texel1_color.a || combiner_alphasub_b[0] == &texel1_color.a || combiner_alphaadd[0] == &texel1_color.a || \
        combiner_rgbmul_r[0] == &texel1_color.a)
        texel1_used_in_cc0 = 1;
    if (combiner_rgbmul_r[0] == &texel0_color.r || combiner_rgbsub_a_r[0] == &texel0_color.r || combiner_rgbsub_b_r[0] == &texel0_color.r || combiner_rgbadd_r[0] == &texel0_color.r || \
        combiner_alphamul[0] == &texel0_color.a || combiner_alphasub_a[0] == &texel0_color.a || combiner_alphasub_b[0] == &texel0_color.a || combiner_alphaadd[0] == &texel0_color.a || \
        combiner_rgbmul_r[0] == &texel0_color.a)
        texel0_used_in_cc0 = 1;
    texels_in_cc0 = texel0_used_in_cc0 || texel1_used_in_cc0;
    texels_in_cc1 = texel0_used_in_cc1 || texel1_used_in_cc1;


    if (texel1_used_in_cc1)
        render_spans_1cycle_ptr = render_spans_1cycle_func[2];
    else if (texel0_used_in_cc1 || lod_frac_used_in_cc1)
        render_spans_1cycle_ptr = render_spans_1cycle_func[1];
    else
        render_spans_1cycle_ptr = render_spans_1cycle_func[0];

    if (texel1_used_in_cc1)
        render_spans_2cycle_ptr = render_spans_2cycle_func[3];
    else if (texel1_used_in_cc0 || texel0_used_in_cc1)
        render_spans_2cycle_ptr = render_spans_2cycle_func[2];
    else if (texel0_used_in_cc0 || lod_frac_used_in_cc0 || lod_frac_used_in_cc1)
        render_spans_2cycle_ptr = render_spans_2cycle_func[1];
    else
        render_spans_2cycle_ptr = render_spans_2cycle_func[0];


    int lodfracused = 0;

    if ((other_modes.cycle_type == CYCLE_TYPE_2 && (lod_frac_used_in_cc0 || lod_frac_used_in_cc1)) || \
        (other_modes.cycle_type == CYCLE_TYPE_1 && lod_frac_used_in_cc1))
        lodfracused = 1;

    if ((other_modes.cycle_type == CYCLE_TYPE_1 && combiner_rgbsub_a_r[1] == &noise) || \
        (other_modes.cycle_type == CYCLE_TYPE_2 && (combiner_rgbsub_a_r[0] == &noise || combiner_rgbsub_a_r[1] == &noise)) || \
        other_modes.alpha_dither_sel == 2)
        get_dither_noise_ptr = get_dither_noise_func[0];
    else if (other_modes.f.rgb_alpha_dither != 0xf)
        get_dither_noise_ptr = get_dither_noise_func[1];
    else
        get_dither_noise_ptr = get_dither_noise_func[2];

    other_modes.f.dolod = other_modes.tex_lod_en || lodfracused;
}

static void rdp_set_tile_size(const uint32_t* args)
{
    int tilenum = (args[1] >> 24) & 0x7;
    tile[tilenum].sl = (args[0] >> 12) & 0xfff;
    tile[tilenum].tl = (args[0] >>  0) & 0xfff;
    tile[tilenum].sh = (args[1] >> 12) & 0xfff;
    tile[tilenum].th = (args[1] >>  0) & 0xfff;

    calculate_clamp_diffs(tilenum);
}

static void rdp_load_block(const uint32_t* args)
{
    int tilenum = (args[1] >> 24) & 0x7;
    int sl, sh, tl, dxt;


    tile[tilenum].sl = sl = ((args[0] >> 12) & 0xfff);
    tile[tilenum].tl = tl = ((args[0] >>  0) & 0xfff);
    tile[tilenum].sh = sh = ((args[1] >> 12) & 0xfff);
    tile[tilenum].th = dxt  = ((args[1] >>  0) & 0xfff);

    calculate_clamp_diffs(tilenum);

    int tlclamped = tl & 0x3ff;

    int32_t lewdata[10];

    lewdata[0] = (args[0] & 0xff000000) | (0x10 << 19) | (tilenum << 16) | ((tlclamped << 2) | 3);
    lewdata[1] = (((tlclamped << 2) | 3) << 16) | (tlclamped << 2);
    lewdata[2] = sh << 16;
    lewdata[3] = sl << 16;
    lewdata[4] = sh << 16;
    lewdata[5] = ((sl << 3) << 16) | (tl << 3);
    lewdata[6] = (dxt & 0xff) << 8;
    lewdata[7] = ((0x80 >> ti_size) << 16) | (dxt >> 8);
    lewdata[8] = 0x20;
    lewdata[9] = 0x20;

    edgewalker_for_loads(lewdata);

}

static void rdp_load_tlut(const uint32_t* args)
{


    tile_tlut_common_cs_decoder(args);
}

static void rdp_load_tile(const uint32_t* args)
{
    tile_tlut_common_cs_decoder(args);
}

static void tile_tlut_common_cs_decoder(const uint32_t* args)
{
    int tilenum = (args[1] >> 24) & 0x7;
    int sl, tl, sh, th;


    tile[tilenum].sl = sl = ((args[0] >> 12) & 0xfff);
    tile[tilenum].tl = tl = ((args[0] >>  0) & 0xfff);
    tile[tilenum].sh = sh = ((args[1] >> 12) & 0xfff);
    tile[tilenum].th = th = ((args[1] >>  0) & 0xfff);

    calculate_clamp_diffs(tilenum);


    int32_t lewdata[10];

    lewdata[0] = (args[0] & 0xff000000) | (0x10 << 19) | (tilenum << 16) | (th | 3);
    lewdata[1] = ((th | 3) << 16) | (tl);
    lewdata[2] = ((sh >> 2) << 16) | ((sh & 3) << 14);
    lewdata[3] = ((sl >> 2) << 16) | ((sl & 3) << 14);
    lewdata[4] = ((sh >> 2) << 16) | ((sh & 3) << 14);
    lewdata[5] = ((sl << 3) << 16) | (tl << 3);
    lewdata[6] = 0;
    lewdata[7] = (0x200 >> ti_size) << 16;
    lewdata[8] = 0x20;
    lewdata[9] = 0x20;

    edgewalker_for_loads(lewdata);
}

static void rdp_set_tile(const uint32_t* args)
{
    int tilenum = (args[1] >> 24) & 0x7;

    tile[tilenum].format    = (args[0] >> 21) & 0x7;
    tile[tilenum].size      = (args[0] >> 19) & 0x3;
    tile[tilenum].line      = (args[0] >>  9) & 0x1ff;
    tile[tilenum].tmem      = (args[0] >>  0) & 0x1ff;
    tile[tilenum].palette   = (args[1] >> 20) & 0xf;
    tile[tilenum].ct        = (args[1] >> 19) & 0x1;
    tile[tilenum].mt        = (args[1] >> 18) & 0x1;
    tile[tilenum].mask_t    = (args[1] >> 14) & 0xf;
    tile[tilenum].shift_t   = (args[1] >> 10) & 0xf;
    tile[tilenum].cs        = (args[1] >>  9) & 0x1;
    tile[tilenum].ms        = (args[1] >>  8) & 0x1;
    tile[tilenum].mask_s    = (args[1] >>  4) & 0xf;
    tile[tilenum].shift_s   = (args[1] >>  0) & 0xf;

    calculate_tile_derivs(tilenum);
}

static void rdp_fill_rect(const uint32_t* args)
{
    uint32_t xl = (args[0] >> 12) & 0xfff;
    uint32_t yl = (args[0] >>  0) & 0xfff;
    uint32_t xh = (args[1] >> 12) & 0xfff;
    uint32_t yh = (args[1] >>  0) & 0xfff;

    if (other_modes.cycle_type == CYCLE_TYPE_FILL || other_modes.cycle_type == CYCLE_TYPE_COPY)
        yl |= 3;

    uint32_t xlint = (xl >> 2) & 0x3ff;
    uint32_t xhint = (xh >> 2) & 0x3ff;

    int32_t ewdata[CMD_MAX_INTS];
    ewdata[0] = (0x3680 << 16) | yl;
    ewdata[1] = (yl << 16) | yh;
    ewdata[2] = (xlint << 16) | ((xl & 3) << 14);
    ewdata[3] = 0;
    ewdata[4] = (xhint << 16) | ((xh & 3) << 14);
    ewdata[5] = 0;
    ewdata[6] = (xlint << 16) | ((xl & 3) << 14);
    ewdata[7] = 0;
    memset(&ewdata[8], 0, 36 * sizeof(int32_t));

    edgewalker_for_prims(ewdata);
}

static void rdp_set_fill_color(const uint32_t* args)
{
    fill_color = args[1];
}

static void rdp_set_fog_color(const uint32_t* args)
{
    fog_color.r = (args[1] >> 24) & 0xff;
    fog_color.g = (args[1] >> 16) & 0xff;
    fog_color.b = (args[1] >>  8) & 0xff;
    fog_color.a = (args[1] >>  0) & 0xff;
}

static void rdp_set_blend_color(const uint32_t* args)
{
    blend_color.r = (args[1] >> 24) & 0xff;
    blend_color.g = (args[1] >> 16) & 0xff;
    blend_color.b = (args[1] >>  8) & 0xff;
    blend_color.a = (args[1] >>  0) & 0xff;
}

static void rdp_set_prim_color(const uint32_t* args)
{
    min_level = (args[0] >> 8) & 0x1f;
    primitive_lod_frac = args[0] & 0xff;
    prim_color.r = (args[1] >> 24) & 0xff;
    prim_color.g = (args[1] >> 16) & 0xff;
    prim_color.b = (args[1] >>  8) & 0xff;
    prim_color.a = (args[1] >>  0) & 0xff;
}

static void rdp_set_env_color(const uint32_t* args)
{
    env_color.r = (args[1] >> 24) & 0xff;
    env_color.g = (args[1] >> 16) & 0xff;
    env_color.b = (args[1] >>  8) & 0xff;
    env_color.a = (args[1] >>  0) & 0xff;
}

static void rdp_set_combine(const uint32_t* args)
{
    combine.sub_a_rgb0  = (args[0] >> 20) & 0xf;
    combine.mul_rgb0    = (args[0] >> 15) & 0x1f;
    combine.sub_a_a0    = (args[0] >> 12) & 0x7;
    combine.mul_a0      = (args[0] >>  9) & 0x7;
    combine.sub_a_rgb1  = (args[0] >>  5) & 0xf;
    combine.mul_rgb1    = (args[0] >>  0) & 0x1f;

    combine.sub_b_rgb0  = (args[1] >> 28) & 0xf;
    combine.sub_b_rgb1  = (args[1] >> 24) & 0xf;
    combine.sub_a_a1    = (args[1] >> 21) & 0x7;
    combine.mul_a1      = (args[1] >> 18) & 0x7;
    combine.add_rgb0    = (args[1] >> 15) & 0x7;
    combine.sub_b_a0    = (args[1] >> 12) & 0x7;
    combine.add_a0      = (args[1] >>  9) & 0x7;
    combine.add_rgb1    = (args[1] >>  6) & 0x7;
    combine.sub_b_a1    = (args[1] >>  3) & 0x7;
    combine.add_a1      = (args[1] >>  0) & 0x7;


    set_suba_rgb_input(&combiner_rgbsub_a_r[0], &combiner_rgbsub_a_g[0], &combiner_rgbsub_a_b[0], combine.sub_a_rgb0);
    set_subb_rgb_input(&combiner_rgbsub_b_r[0], &combiner_rgbsub_b_g[0], &combiner_rgbsub_b_b[0], combine.sub_b_rgb0);
    set_mul_rgb_input(&combiner_rgbmul_r[0], &combiner_rgbmul_g[0], &combiner_rgbmul_b[0], combine.mul_rgb0);
    set_add_rgb_input(&combiner_rgbadd_r[0], &combiner_rgbadd_g[0], &combiner_rgbadd_b[0], combine.add_rgb0);
    set_sub_alpha_input(&combiner_alphasub_a[0], combine.sub_a_a0);
    set_sub_alpha_input(&combiner_alphasub_b[0], combine.sub_b_a0);
    set_mul_alpha_input(&combiner_alphamul[0], combine.mul_a0);
    set_sub_alpha_input(&combiner_alphaadd[0], combine.add_a0);

    set_suba_rgb_input(&combiner_rgbsub_a_r[1], &combiner_rgbsub_a_g[1], &combiner_rgbsub_a_b[1], combine.sub_a_rgb1);
    set_subb_rgb_input(&combiner_rgbsub_b_r[1], &combiner_rgbsub_b_g[1], &combiner_rgbsub_b_b[1], combine.sub_b_rgb1);
    set_mul_rgb_input(&combiner_rgbmul_r[1], &combiner_rgbmul_g[1], &combiner_rgbmul_b[1], combine.mul_rgb1);
    set_add_rgb_input(&combiner_rgbadd_r[1], &combiner_rgbadd_g[1], &combiner_rgbadd_b[1], combine.add_rgb1);
    set_sub_alpha_input(&combiner_alphasub_a[1], combine.sub_a_a1);
    set_sub_alpha_input(&combiner_alphasub_b[1], combine.sub_b_a1);
    set_mul_alpha_input(&combiner_alphamul[1], combine.mul_a1);
    set_sub_alpha_input(&combiner_alphaadd[1], combine.add_a1);

    other_modes.f.stalederivs = 1;
}

static void rdp_set_texture_image(const uint32_t* args)
{
    ti_format   = (args[0] >> 21) & 0x7;
    ti_size     = (args[0] >> 19) & 0x3;
    ti_width    = (args[0] & 0x3ff) + 1;
    ti_address  = args[1] & 0x0ffffff;



}

static void rdp_set_mask_image(const uint32_t* args)
{
    zb_address  = args[1] & 0x0ffffff;
}

static void rdp_set_color_image(const uint32_t* args)
{
    fb_format   = (args[0] >> 21) & 0x7;
    fb_size     = (args[0] >> 19) & 0x3;
    fb_width    = (args[0] & 0x3ff) + 1;
    fb_address  = args[1] & 0x0ffffff;


    fbread1_ptr = fbread_func[fb_size];
    fbread2_ptr = fbread2_func[fb_size];
    fbwrite_ptr = fbwrite_func[fb_size];
    fbfill_ptr = fbfill_func[fb_size];
}

static const struct
{
    void (*handler)(const uint32_t*);   // command handler function pointer
    uint32_t length;                    // command data length in bytes
    bool singlethread;                  // run in main thread
    bool multithread;                   // run in worker threads
    bool sync;                          // synchronize all workers before execution
    char name[32];                      // descriptive name for debugging
} rdp_commands[] = {
    {rdp_noop,              8,   true,  false, false, "No_Op"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_tri_noshade,       32,  false, true,  false, "Fill_Triangle"},
    {rdp_tri_noshade_z,     48,  false, true,  false, "Fill_ZBuffer_Triangle"},
    {rdp_tri_tex,           96,  false, true,  false, "Texture_Triangle"},
    {rdp_tri_tex_z,         112, false, true,  false, "Texture_ZBuffer_Triangle"},
    {rdp_tri_shade,         96,  false, true,  false, "Shade_Triangle"},
    {rdp_tri_shade_z,       112, false, true,  false, "Shade_ZBuffer_Triangle"},
    {rdp_tri_texshade,      160, false, true,  false, "Shade_Texture_Triangle"},
    {rdp_tri_texshade_z,    176, false, true,  false, "Shade_Texture_Z_Buffer_Triangle"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_tex_rect,          16,  false, true,  false, "Texture_Rectangle"},
    {rdp_tex_rect_flip,     16,  false, true,  false, "Texture_Rectangle_Flip"},
    {rdp_sync_load,         8,   true,  false, false, "Sync_Load"},
    {rdp_sync_pipe,         8,   true,  false, false, "Sync_Pipe"},
    {rdp_sync_tile,         8,   true,  false, false, "Sync_Tile"},
    {rdp_sync_full,         8,   true,  false, true,  "Sync_Full"},
    {rdp_set_key_gb,        8,   false, true,  false, "Set_Key_GB"},
    {rdp_set_key_r,         8,   false, true,  false, "Set_Key_R"},
    {rdp_set_convert,       8,   false, true,  false, "Set_Convert"},
    {rdp_set_scissor,       8,   false, true,  false, "Set_Scissor"},
    {rdp_set_prim_depth,    8,   false, true,  false, "Set_Prim_Depth"},
    {rdp_set_other_modes,   8,   false, true,  false, "Set_Other_Modes"},
    {rdp_load_tlut,         8,   false, true,  false, "Load_TLUT"},
    {rdp_invalid,           8,   true,  false, false, "???"},
    {rdp_set_tile_size,     8,   false, true,  false, "Set_Tile_Size"},
    {rdp_load_block,        8,   false, true,  false, "Load_Block"},
    {rdp_load_tile,         8,   false, true,  false, "Load_Tile"},
    {rdp_set_tile,          8,   false, true,  false, "Set_Tile"},
    {rdp_fill_rect,         8,   false, true,  false, "Fill_Rectangle"},
    {rdp_set_fill_color,    8,   false, true,  false, "Set_Fill_Color"},
    {rdp_set_fog_color,     8,   false, true,  false, "Set_Fog_Color"},
    {rdp_set_blend_color,   8,   false, true,  false, "Set_Blend_Color"},
    {rdp_set_prim_color,    8,   false, true,  false, "Set_Prim_Color"},
    {rdp_set_env_color,     8,   false, true,  false, "Set_Env_Color"},
    {rdp_set_combine,       8,   false, true,  false, "Set_Combine"},
    {rdp_set_texture_image, 8,   false, true,  false, "Set_Texture_Image"},
    {rdp_set_mask_image,    8,   true,  true,  true,  "Set_Mask_Image"},
    {rdp_set_color_image,   8,   false, true,  true,  "Set_Color_Image"}
};

static void rdp_cmd_run(const uint32_t* arg)
{
    uint32_t cmd_id = CMD_ID(arg);
    rdp_commands[cmd_id].handler(arg);
}

static void rdp_cmd_run_buffered(void)
{
    for (uint32_t pos = 0; pos < rdp_cmd_buf_pos; pos++) {
        rdp_cmd_run(rdp_cmd_buf[pos]);
    }
}

static void rdp_cmd_flush(void)
{
    // only run if there's something buffered
    if (rdp_cmd_buf_pos) {
        // let workers run all buffered commands in parallel
        parallel_run(rdp_cmd_run_buffered);

        // reset buffer by starting from the beginning
        rdp_cmd_buf_pos = 0;
    }
}

static void rdp_cmd_push(const uint32_t* arg, uint32_t length)
{
    // copy command data to current buffer position
    memcpy(rdp_cmd_buf + rdp_cmd_buf_pos, arg, length * sizeof(uint32_t));

    // increment buffer position and flush buffer when it is full
    if (++rdp_cmd_buf_pos >= CMD_BUFFER_COUNT) {
        rdp_cmd_flush();
    }
}

void rdp_cmd(const uint32_t* arg, uint32_t length)
{
    uint32_t cmd_id = CMD_ID(arg);

    bool parallel = config->num_workers != 1;

    if (rdp_commands[cmd_id].sync && parallel) {
        rdp_cmd_flush();
    }

    if (rdp_commands[cmd_id].singlethread || !parallel) {
        rdp_cmd_run(arg);
    }

    if (rdp_commands[cmd_id].multithread && parallel) {
        rdp_cmd_push(arg, length);
    }

    // send z-buffer address to VI for VI_MODE_COVERAGE
    if (cmd_id == CMD_ID_SET_MASK_IMAGE) {
        vi_set_zb_address(zb_address);
    }
}

void rdp_update(void)
{
    int i, length;
    uint32_t cmd, cmd_length;

    uint32_t** dp_reg = plugin->get_dp_registers();
    uint32_t dp_current_al = *dp_reg[DP_CURRENT] & ~7, dp_end_al = *dp_reg[DP_END] & ~7;

    *dp_reg[DP_STATUS] &= ~DP_STATUS_FREEZE;







    if (dp_end_al <= dp_current_al)
    {






        return;
    }

    length = (dp_end_al - dp_current_al) >> 2;

    ptr_onstart = rdp_cmd_ptr;









    uint32_t remaining_length = length;


    dp_current_al >>= 2;

    while (remaining_length)
    {

    int toload = remaining_length > 0x10000 ? 0x10000 : remaining_length;

    if (*dp_reg[DP_STATUS] & DP_STATUS_XBUS_DMA)
    {
        uint32_t* dmem = (uint32_t*)plugin->get_dmem();
        for (i = 0; i < toload; i ++)
        {
            rdp_cmd_data[rdp_cmd_ptr] = dmem[dp_current_al & 0x3ff];
            rdp_cmd_ptr++;
            dp_current_al++;
        }
    }
    else
    {
        for (i = 0; i < toload; i ++)
        {
            RREADIDX32(rdp_cmd_data[rdp_cmd_ptr], dp_current_al);





            rdp_cmd_ptr++;
            dp_current_al++;
        }
    }

    remaining_length -= toload;

    while (rdp_cmd_cur < rdp_cmd_ptr && !rdp_pipeline_crashed)
    {
        cmd = CMD_ID(rdp_cmd_data + rdp_cmd_cur);
        cmd_length = rdp_commands[cmd].length >> 2;



        if ((rdp_cmd_ptr - rdp_cmd_cur) < cmd_length)
        {
            if (!remaining_length)
            {

                *dp_reg[DP_START] = *dp_reg[DP_CURRENT] = *dp_reg[DP_END];
                return;
            }
            else
            {
                dp_current_al -= (rdp_cmd_ptr - rdp_cmd_cur);
                remaining_length += (rdp_cmd_ptr - rdp_cmd_cur);
                break;
            }
        }

        rdp_cmd(rdp_cmd_data + rdp_cmd_cur, cmd_length);

        if (trace_write_is_open()) {
            trace_write_cmd(rdp_cmd_data + rdp_cmd_cur, cmd_length);
        }

        rdp_cmd_cur += cmd_length;
    };
    rdp_cmd_ptr = 0;
    rdp_cmd_cur = 0;
    };

    *dp_reg[DP_START] = *dp_reg[DP_CURRENT] = *dp_reg[DP_END];
}

static STRICTINLINE int alpha_compare(int32_t comb_alpha)
{
    int32_t threshold;
    if (!other_modes.alpha_compare_en)
        return 1;
    else
    {
        if (!other_modes.dither_alpha_en)
            threshold = blend_color.a;
        else
            threshold = irand() & 0xff;


        if (comb_alpha >= threshold)
            return 1;
        else
            return 0;
    }
}

static STRICTINLINE int32_t color_combiner_equation(int32_t a, int32_t b, int32_t c, int32_t d)
{





    a = special_9bit_exttable[a];
    b = special_9bit_exttable[b];
    c = SIGNF(c, 9);
    d = special_9bit_exttable[d];
    a = ((a - b) * c) + (d << 8) + 0x80;
    return (a & 0x1ffff);
}

static STRICTINLINE int32_t alpha_combiner_equation(int32_t a, int32_t b, int32_t c, int32_t d)
{
    a = special_9bit_exttable[a];
    b = special_9bit_exttable[b];
    c = SIGNF(c, 9);
    d = special_9bit_exttable[d];
    a = (((a - b) * c) + (d << 8) + 0x80) >> 8;
    return (a & 0x1ff);
}


static STRICTINLINE void blender_equation_cycle0(int* r, int* g, int* b)
{
    int blend1a, blend2a;
    int blr, blg, blb, sum;
    blend1a = *blender1b_a[0] >> 3;
    blend2a = *blender2b_a[0] >> 3;

    int mulb;



    if (other_modes.f.special_bsel0)
    {
        blend1a = (blend1a >> blshifta) & 0x3C;
        blend2a = (blend2a >> blshiftb) | 3;
    }

    mulb = blend2a + 1;


    blr = (*blender1a_r[0]) * blend1a + (*blender2a_r[0]) * mulb;
    blg = (*blender1a_g[0]) * blend1a + (*blender2a_g[0]) * mulb;
    blb = (*blender1a_b[0]) * blend1a + (*blender2a_b[0]) * mulb;



    if (!other_modes.force_blend)
    {





        sum = ((blend1a & ~3) + (blend2a & ~3) + 4) << 9;
        *r = bldiv_hwaccurate_table[sum | ((blr >> 2) & 0x7ff)];
        *g = bldiv_hwaccurate_table[sum | ((blg >> 2) & 0x7ff)];
        *b = bldiv_hwaccurate_table[sum | ((blb >> 2) & 0x7ff)];
    }
    else
    {
        *r = (blr >> 5) & 0xff;
        *g = (blg >> 5) & 0xff;
        *b = (blb >> 5) & 0xff;
    }
}

static STRICTINLINE void blender_equation_cycle0_2(int* r, int* g, int* b)
{
    int blend1a, blend2a;
    blend1a = *blender1b_a[0] >> 3;
    blend2a = *blender2b_a[0] >> 3;

    if (other_modes.f.special_bsel0)
    {
        blend1a = (blend1a >> pastblshifta) & 0x3C;
        blend2a = (blend2a >> pastblshiftb) | 3;
    }

    blend2a += 1;
    *r = (((*blender1a_r[0]) * blend1a + (*blender2a_r[0]) * blend2a) >> 5) & 0xff;
    *g = (((*blender1a_g[0]) * blend1a + (*blender2a_g[0]) * blend2a) >> 5) & 0xff;
    *b = (((*blender1a_b[0]) * blend1a + (*blender2a_b[0]) * blend2a) >> 5) & 0xff;
}

static STRICTINLINE void blender_equation_cycle1(int* r, int* g, int* b)
{
    int blend1a, blend2a;
    int blr, blg, blb, sum;
    blend1a = *blender1b_a[1] >> 3;
    blend2a = *blender2b_a[1] >> 3;

    int mulb;
    if (other_modes.f.special_bsel1)
    {
        blend1a = (blend1a >> blshifta) & 0x3C;
        blend2a = (blend2a >> blshiftb) | 3;
    }

    mulb = blend2a + 1;
    blr = (*blender1a_r[1]) * blend1a + (*blender2a_r[1]) * mulb;
    blg = (*blender1a_g[1]) * blend1a + (*blender2a_g[1]) * mulb;
    blb = (*blender1a_b[1]) * blend1a + (*blender2a_b[1]) * mulb;

    if (!other_modes.force_blend)
    {
        sum = ((blend1a & ~3) + (blend2a & ~3) + 4) << 9;
        *r = bldiv_hwaccurate_table[sum | ((blr >> 2) & 0x7ff)];
        *g = bldiv_hwaccurate_table[sum | ((blg >> 2) & 0x7ff)];
        *b = bldiv_hwaccurate_table[sum | ((blb >> 2) & 0x7ff)];
    }
    else
    {
        *r = (blr >> 5) & 0xff;
        *g = (blg >> 5) & 0xff;
        *b = (blb >> 5) & 0xff;
    }
}




static STRICTINLINE uint32_t rightcvghex(uint32_t x, uint32_t fmask)
{
    uint32_t covered = ((x & 7) + 1) >> 1;

    covered = 0xf0 >> covered;
    return (covered & fmask);
}

static STRICTINLINE uint32_t leftcvghex(uint32_t x, uint32_t fmask)
{
    uint32_t covered = ((x & 7) + 1) >> 1;
    covered = 0xf >> covered;
    return (covered & fmask);
}



static STRICTINLINE void compute_cvg_flip(int32_t scanline)
{
    int32_t purgestart, purgeend;
    int i, length, fmask, maskshift, fmaskshifted;
    int32_t minorcur, majorcur, minorcurint, majorcurint, samecvg;

    purgestart = span[scanline].rx;
    purgeend = span[scanline].lx;
    length = purgeend - purgestart;
    if (length >= 0)
    {






        memset(&cvgbuf[purgestart], 0xff, length + 1);
        for(i = 0; i < 4; i++)
        {

                fmask = 0xa >> (i & 1);




                maskshift = (i - 2) & 4;
                fmaskshifted = fmask << maskshift;

                if (!span[scanline].invalyscan[i])
                {
                    minorcur = span[scanline].minorx[i];
                    majorcur = span[scanline].majorx[i];
                    minorcurint = minorcur >> 3;
                    majorcurint = majorcur >> 3;


                    for (int k = purgestart; k <= majorcurint; k++)
                        cvgbuf[k] &= ~fmaskshifted;
                    for (int k = minorcurint; k <= purgeend; k++)
                        cvgbuf[k] &= ~fmaskshifted;









                    if (minorcurint > majorcurint)
                    {
                        cvgbuf[minorcurint] |= (rightcvghex(minorcur, fmask) << maskshift);
                        cvgbuf[majorcurint] |= (leftcvghex(majorcur, fmask) << maskshift);
                    }
                    else if (minorcurint == majorcurint)
                    {
                        samecvg = rightcvghex(minorcur, fmask) & leftcvghex(majorcur, fmask);
                        cvgbuf[majorcurint] |= (samecvg << maskshift);
                    }
                }
                else
                {
                    for (int k = purgestart; k <= purgeend; k++)
                        cvgbuf[k] &= ~fmaskshifted;
                }

        }
    }


}

static STRICTINLINE void compute_cvg_noflip(int32_t scanline)
{
    int32_t purgestart, purgeend;
    int i, length, fmask, maskshift, fmaskshifted;
    int32_t minorcur, majorcur, minorcurint, majorcurint, samecvg;

    purgestart = span[scanline].lx;
    purgeend = span[scanline].rx;
    length = purgeend - purgestart;

    if (length >= 0)
    {
        memset(&cvgbuf[purgestart], 0xff, length + 1);

        for(i = 0; i < 4; i++)
        {
            fmask = 0xa >> (i & 1);
            maskshift = (i - 2) & 4;
            fmaskshifted = fmask << maskshift;

            if (!span[scanline].invalyscan[i])
            {
                minorcur = span[scanline].minorx[i];
                majorcur = span[scanline].majorx[i];
                minorcurint = minorcur >> 3;
                majorcurint = majorcur >> 3;

                for (int k = purgestart; k <= minorcurint; k++)
                    cvgbuf[k] &= ~fmaskshifted;
                for (int k = majorcurint; k <= purgeend; k++)
                    cvgbuf[k] &= ~fmaskshifted;

                if (majorcurint > minorcurint)
                {
                    cvgbuf[minorcurint] |= (leftcvghex(minorcur, fmask) << maskshift);
                    cvgbuf[majorcurint] |= (rightcvghex(majorcur, fmask) << maskshift);
                }
                else if (minorcurint == majorcurint)
                {
                    samecvg = leftcvghex(minorcur, fmask) & rightcvghex(majorcur, fmask);
                    cvgbuf[majorcurint] |= (samecvg << maskshift);
                }
            }
            else
            {
                for (int k = purgestart; k <= purgeend; k++)
                    cvgbuf[k] &= ~fmaskshifted;
            }
        }
    }
}

static INLINE void fbwrite_4(uint32_t curpixel, uint32_t r, uint32_t g, uint32_t b, uint32_t blend_en, uint32_t curpixel_cvg, uint32_t curpixel_memcvg)
{
    uint32_t fb = fb_address + curpixel;
    RWRITEADDR8(fb, 0);
}

static INLINE void fbwrite_8(uint32_t curpixel, uint32_t r, uint32_t g, uint32_t b, uint32_t blend_en, uint32_t curpixel_cvg, uint32_t curpixel_memcvg)
{
    uint32_t fb = fb_address + curpixel;
    PAIRWRITE8(fb, r & 0xff, (r & 1) ? 3 : 0);
}

static INLINE void fbwrite_16(uint32_t curpixel, uint32_t r, uint32_t g, uint32_t b, uint32_t blend_en, uint32_t curpixel_cvg, uint32_t curpixel_memcvg)
{
#undef CVG_DRAW
#ifdef CVG_DRAW
    int covdraw = (curpixel_cvg - 1) << 5;
    r=covdraw; g=covdraw; b=covdraw;
#endif

    uint32_t fb;
    uint16_t rval;
    uint8_t hval;
    fb = (fb_address >> 1) + curpixel;

    int32_t finalcvg = finalize_spanalpha(blend_en, curpixel_cvg, curpixel_memcvg);
    int16_t finalcolor;

    if (fb_format == FORMAT_RGBA)
    {
        finalcolor = ((r & ~7) << 8) | ((g & ~7) << 3) | ((b & ~7) >> 2);
    }
    else
    {
        finalcolor = (r << 8) | (finalcvg << 5);
        finalcvg = 0;
    }


    rval = finalcolor|(finalcvg >> 2);
    hval = finalcvg & 3;
    PAIRWRITE16(fb, rval, hval);
}

static INLINE void fbwrite_32(uint32_t curpixel, uint32_t r, uint32_t g, uint32_t b, uint32_t blend_en, uint32_t curpixel_cvg, uint32_t curpixel_memcvg)
{
    uint32_t fb = (fb_address >> 2) + curpixel;

    int32_t finalcolor;
    int32_t finalcvg = finalize_spanalpha(blend_en, curpixel_cvg, curpixel_memcvg);

    finalcolor = (r << 24) | (g << 16) | (b << 8);
    finalcolor |= (finalcvg << 5);

    PAIRWRITE32(fb, finalcolor, (g & 1) ? 3 : 0, 0);
}

static INLINE void fbfill_4(uint32_t curpixel)
{
    rdp_pipeline_crashed = 1;
}

static INLINE void fbfill_8(uint32_t curpixel)
{
    uint32_t fb = fb_address + curpixel;
    uint32_t val = (fill_color >> (((fb & 3) ^ 3) << 3)) & 0xff;
    uint8_t hval = ((val & 1) << 1) | (val & 1);
    PAIRWRITE8(fb, val, hval);
}

static INLINE void fbfill_16(uint32_t curpixel)
{
    uint16_t val;
    uint8_t hval;
    uint32_t fb = (fb_address >> 1) + curpixel;
    if (fb & 1)
        val = fill_color & 0xffff;
    else
        val = (fill_color >> 16) & 0xffff;
    hval = ((val & 1) << 1) | (val & 1);
    PAIRWRITE16(fb, val, hval);
}

static INLINE void fbfill_32(uint32_t curpixel)
{
    uint32_t fb = (fb_address >> 2) + curpixel;
    PAIRWRITE32(fb, fill_color, (fill_color & 0x10000) ? 3 : 0, (fill_color & 0x1) ? 3 : 0);
}

static INLINE void fbread_4(uint32_t curpixel, uint32_t* curpixel_memcvg)
{
    memory_color.r = memory_color.g = memory_color.b = 0;

    *curpixel_memcvg = 7;
    memory_color.a = 0xe0;
}

static INLINE void fbread2_4(uint32_t curpixel, uint32_t* curpixel_memcvg)
{
    pre_memory_color.r = pre_memory_color.g = pre_memory_color.b = 0;
    pre_memory_color.a = 0xe0;
    *curpixel_memcvg = 7;
}

static INLINE void fbread_8(uint32_t curpixel, uint32_t* curpixel_memcvg)
{
    uint8_t mem;
    uint32_t addr = fb_address + curpixel;
    RREADADDR8(mem, addr);
    memory_color.r = memory_color.g = memory_color.b = mem;
    *curpixel_memcvg = 7;
    memory_color.a = 0xe0;
}

static INLINE void fbread2_8(uint32_t curpixel, uint32_t* curpixel_memcvg)
{
    uint8_t mem;
    uint32_t addr = fb_address + curpixel;
    RREADADDR8(mem, addr);
    pre_memory_color.r = pre_memory_color.g = pre_memory_color.b = mem;
    pre_memory_color.a = 0xe0;
    *curpixel_memcvg = 7;
}

static INLINE void fbread_16(uint32_t curpixel, uint32_t* curpixel_memcvg)
{
    uint16_t fword;
    uint8_t hbyte;
    uint32_t addr = (fb_address >> 1) + curpixel;

    uint8_t lowbits;


    if (other_modes.image_read_en)
    {
        PAIRREAD16(fword, hbyte, addr);

        if (fb_format == FORMAT_RGBA)
        {
            memory_color.r = GET_HI(fword);
            memory_color.g = GET_MED(fword);
            memory_color.b = GET_LOW(fword);
            lowbits = ((fword & 1) << 2) | hbyte;
        }
        else
        {
            memory_color.r = memory_color.g = memory_color.b = fword >> 8;
            lowbits = (fword >> 5) & 7;
        }

        *curpixel_memcvg = lowbits;
        memory_color.a = lowbits << 5;
    }
    else
    {
        RREADIDX16(fword, addr);

        if (fb_format == FORMAT_RGBA)
        {
            memory_color.r = GET_HI(fword);
            memory_color.g = GET_MED(fword);
            memory_color.b = GET_LOW(fword);
        }
        else
            memory_color.r = memory_color.g = memory_color.b = fword >> 8;

        *curpixel_memcvg = 7;
        memory_color.a = 0xe0;
    }
}

static INLINE void fbread2_16(uint32_t curpixel, uint32_t* curpixel_memcvg)
{
    uint16_t fword;
    uint8_t hbyte;
    uint32_t addr = (fb_address >> 1) + curpixel;

    uint8_t lowbits;

    if (other_modes.image_read_en)
    {
        PAIRREAD16(fword, hbyte, addr);

        if (fb_format == FORMAT_RGBA)
        {
            pre_memory_color.r = GET_HI(fword);
            pre_memory_color.g = GET_MED(fword);
            pre_memory_color.b = GET_LOW(fword);
            lowbits = ((fword & 1) << 2) | hbyte;
        }
        else
        {
            pre_memory_color.r = pre_memory_color.g = pre_memory_color.b = fword >> 8;
            lowbits = (fword >> 5) & 7;
        }

        *curpixel_memcvg = lowbits;
        pre_memory_color.a = lowbits << 5;
    }
    else
    {
        RREADIDX16(fword, addr);

        if (fb_format == FORMAT_RGBA)
        {
            pre_memory_color.r = GET_HI(fword);
            pre_memory_color.g = GET_MED(fword);
            pre_memory_color.b = GET_LOW(fword);
        }
        else
            pre_memory_color.r = pre_memory_color.g = pre_memory_color.b = fword >> 8;

        *curpixel_memcvg = 7;
        pre_memory_color.a = 0xe0;
    }

}

static INLINE void fbread_32(uint32_t curpixel, uint32_t* curpixel_memcvg)
{
    uint32_t mem, addr = (fb_address >> 2) + curpixel;
    RREADIDX32(mem, addr);
    memory_color.r = (mem >> 24) & 0xff;
    memory_color.g = (mem >> 16) & 0xff;
    memory_color.b = (mem >> 8) & 0xff;
    if (other_modes.image_read_en)
    {
        *curpixel_memcvg = (mem >> 5) & 7;
        memory_color.a = (mem) & 0xe0;
    }
    else
    {
        *curpixel_memcvg = 7;
        memory_color.a = 0xe0;
    }
}

static INLINE void fbread2_32(uint32_t curpixel, uint32_t* curpixel_memcvg)
{
    uint32_t mem, addr = (fb_address >> 2) + curpixel;
    RREADIDX32(mem, addr);
    pre_memory_color.r = (mem >> 24) & 0xff;
    pre_memory_color.g = (mem >> 16) & 0xff;
    pre_memory_color.b = (mem >> 8) & 0xff;
    if (other_modes.image_read_en)
    {
        *curpixel_memcvg = (mem >> 5) & 7;
        pre_memory_color.a = (mem) & 0xe0;
    }
    else
    {
        *curpixel_memcvg = 7;
        pre_memory_color.a = 0xe0;
    }
}

static STRICTINLINE uint32_t z_decompress(uint32_t zb)
{
    return z_complete_dec_table[(zb >> 2) & 0x3fff];
}

static INLINE void z_build_com_table(void)
{

    uint16_t altmem = 0;
    for(int z = 0; z < 0x40000; z++)
    {
    switch((z >> 11) & 0x7f)
    {
    case 0x00:
    case 0x01:
    case 0x02:
    case 0x03:
    case 0x04:
    case 0x05:
    case 0x06:
    case 0x07:
    case 0x08:
    case 0x09:
    case 0x0a:
    case 0x0b:
    case 0x0c:
    case 0x0d:
    case 0x0e:
    case 0x0f:
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13:
    case 0x14:
    case 0x15:
    case 0x16:
    case 0x17:
    case 0x18:
    case 0x19:
    case 0x1a:
    case 0x1b:
    case 0x1c:
    case 0x1d:
    case 0x1e:
    case 0x1f:
    case 0x20:
    case 0x21:
    case 0x22:
    case 0x23:
    case 0x24:
    case 0x25:
    case 0x26:
    case 0x27:
    case 0x28:
    case 0x29:
    case 0x2a:
    case 0x2b:
    case 0x2c:
    case 0x2d:
    case 0x2e:
    case 0x2f:
    case 0x30:
    case 0x31:
    case 0x32:
    case 0x33:
    case 0x34:
    case 0x35:
    case 0x36:
    case 0x37:
    case 0x38:
    case 0x39:
    case 0x3a:
    case 0x3b:
    case 0x3c:
    case 0x3d:
    case 0x3e:
    case 0x3f:
        altmem = (z >> 4) & 0x1ffc;
        break;
    case 0x40:
    case 0x41:
    case 0x42:
    case 0x43:
    case 0x44:
    case 0x45:
    case 0x46:
    case 0x47:
    case 0x48:
    case 0x49:
    case 0x4a:
    case 0x4b:
    case 0x4c:
    case 0x4d:
    case 0x4e:
    case 0x4f:
    case 0x50:
    case 0x51:
    case 0x52:
    case 0x53:
    case 0x54:
    case 0x55:
    case 0x56:
    case 0x57:
    case 0x58:
    case 0x59:
    case 0x5a:
    case 0x5b:
    case 0x5c:
    case 0x5d:
    case 0x5e:
    case 0x5f:
        altmem = ((z >> 3) & 0x1ffc) | 0x2000;
        break;
    case 0x60:
    case 0x61:
    case 0x62:
    case 0x63:
    case 0x64:
    case 0x65:
    case 0x66:
    case 0x67:
    case 0x68:
    case 0x69:
    case 0x6a:
    case 0x6b:
    case 0x6c:
    case 0x6d:
    case 0x6e:
    case 0x6f:
        altmem = ((z >> 2) & 0x1ffc) | 0x4000;
        break;
    case 0x70:
    case 0x71:
    case 0x72:
    case 0x73:
    case 0x74:
    case 0x75:
    case 0x76:
    case 0x77:
        altmem = ((z >> 1) & 0x1ffc) | 0x6000;
        break;
    case 0x78:
    case 0x79:
    case 0x7a:
    case 0x7b:
        altmem = (z & 0x1ffc) | 0x8000;
        break;
    case 0x7c:
    case 0x7d:
        altmem = ((z << 1) & 0x1ffc) | 0xa000;
        break;
    case 0x7e:
        altmem = ((z << 2) & 0x1ffc) | 0xc000;
        break;
    case 0x7f:
        altmem = ((z << 2) & 0x1ffc) | 0xe000;
        break;
    default:
        msg_error("z_build_com_table failed");
        break;
    }

    z_com_table[z] = altmem;

    }
}

static INLINE void precalc_cvmask_derivatives(void)
{
    int i = 0, k = 0;
    uint16_t mask = 0, maskx = 0, masky = 0;
    uint8_t offx = 0, offy = 0;
    const uint8_t yarray[16] = {0, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0};
    const uint8_t xarray[16] = {0, 3, 2, 2, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0};


    for (; i < 0x100; i++)
    {
        mask = decompress_cvmask_frombyte(i);
        cvarray[i].cvg = cvarray[i].cvbit = 0;
        cvarray[i].cvbit = (i >> 7) & 1;
        for (k = 0; k < 8; k++)
            cvarray[i].cvg += ((i >> k) & 1);


        masky = maskx = offx = offy = 0;
        for (k = 0; k < 4; k++)
            masky |= ((mask & (0xf000 >> (k << 2))) > 0) << k;

        offy = yarray[masky];

        maskx = (mask & (0xf000 >> (offy << 2))) >> ((offy ^ 3) << 2);


        offx = xarray[maskx];

        cvarray[i].xoff = offx;
        cvarray[i].yoff = offy;
    }
}

static STRICTINLINE uint16_t decompress_cvmask_frombyte(uint8_t x)
{
    uint16_t y = (x & 1) | ((x & 2) << 4) | (x & 4) | ((x & 8) << 4) |
        ((x & 0x10) << 4) | ((x & 0x20) << 8) | ((x & 0x40) << 4) | ((x & 0x80) << 8);
    return y;
}

static STRICTINLINE void lookup_cvmask_derivatives(uint32_t mask, uint8_t* offx, uint8_t* offy, uint32_t* curpixel_cvg, uint32_t* curpixel_cvbit)
{
    *curpixel_cvg = cvarray[mask].cvg;
    *curpixel_cvbit = cvarray[mask].cvbit;
    *offx = cvarray[mask].xoff;
    *offy = cvarray[mask].yoff;
}

static STRICTINLINE void z_store(uint32_t zcurpixel, uint32_t z, int dzpixenc)
{
    uint16_t zval = z_com_table[z & 0x3ffff]|(dzpixenc >> 2);
    uint8_t hval = dzpixenc & 3;
    PAIRWRITE16(zcurpixel, zval, hval);
}

static STRICTINLINE uint32_t dz_decompress(uint32_t dz_compressed)
{
    return (1 << dz_compressed);
}


static STRICTINLINE uint32_t dz_compress(uint32_t value)
{
    int j = 0;
    if (value & 0xff00)
        j |= 8;
    if (value & 0xf0f0)
        j |= 4;
    if (value & 0xcccc)
        j |= 2;
    if (value & 0xaaaa)
        j |= 1;
    return j;
}

static STRICTINLINE uint32_t z_compare(uint32_t zcurpixel, uint32_t sz, uint16_t dzpix, int dzpixenc, uint32_t* blend_en, uint32_t* prewrap, uint32_t* curpixel_cvg, uint32_t curpixel_memcvg)
{


    int force_coplanar = 0;
    sz &= 0x3ffff;

    uint8_t hval;
    uint16_t zval;
    uint32_t oz, dzmem;
    int32_t rawdzmem;

    if (other_modes.z_compare_en)
    {
        PAIRREAD16(zval, hval, zcurpixel);
        oz = z_decompress(zval);
        rawdzmem = ((zval & 3) << 2) | hval;
        dzmem = dz_decompress(rawdzmem);



        if (other_modes.f.realblendershiftersneeded)
        {
            blshifta = clamp(dzpixenc - rawdzmem, 0, 4);
            blshiftb = clamp(rawdzmem - dzpixenc, 0, 4);

        }


        if (other_modes.f.interpixelblendershiftersneeded)
        {
            pastblshifta = clamp(dzpixenc - pastrawdzmem, 0, 4);
            pastblshiftb = clamp(pastrawdzmem - dzpixenc, 0, 4);
        }

        pastrawdzmem = rawdzmem;

        int precision_factor = (zval >> 13) & 0xf;




        uint32_t dzmemmodifier;
        if (precision_factor < 3)
        {
            if (dzmem != 0x8000)
            {
                dzmemmodifier = 16 >> precision_factor;
                dzmem <<= 1;
                if (dzmem < dzmemmodifier)
                    dzmem = dzmemmodifier;

            }
            else
            {
                force_coplanar = 1;
                dzmem = 0xffff;
            }
        }






        uint32_t dznew = (uint32_t)deltaz_comparator_lut[dzpix | dzmem];

        uint32_t dznotshift = dznew;
        dznew <<= 3;


        uint32_t farther = force_coplanar || ((sz + dznew) >= oz);

        int overflow = (curpixel_memcvg + *curpixel_cvg) & 8;
        *blend_en = other_modes.force_blend || (!overflow && other_modes.antialias_en && farther);

        *prewrap = overflow;



        int cvgcoeff = 0;
        uint32_t dzenc = 0;

        int32_t diff;
        uint32_t nearer, max, infront;

        switch(other_modes.z_mode)
        {
        case ZMODE_OPAQUE:
            infront = sz < oz;
            diff = (int32_t)sz - (int32_t)dznew;
            nearer = force_coplanar || (diff <= (int32_t)oz);
            max = (oz == 0x3ffff);
            return (max || (overflow ? infront : nearer));
            break;
        case ZMODE_INTERPENETRATING:
            infront = sz < oz;
            if (!infront || !farther || !overflow)
            {
                diff = (int32_t)sz - (int32_t)dznew;
                nearer = force_coplanar || (diff <= (int32_t)oz);
                max = (oz == 0x3ffff);
                return (max || (overflow ? infront : nearer));
            }
            else
            {
                dzenc = dz_compress(dznotshift & 0xffff);
                cvgcoeff = ((oz >> dzenc) - (sz >> dzenc)) & 0xf;
                *curpixel_cvg = ((cvgcoeff * (*curpixel_cvg)) >> 3) & 0xf;
                return 1;
            }
            break;
        case ZMODE_TRANSPARENT:
            infront = sz < oz;
            max = (oz == 0x3ffff);
            return (infront || max);
            break;
        case ZMODE_DECAL:
            diff = (int32_t)sz - (int32_t)dznew;
            nearer = force_coplanar || (diff <= (int32_t)oz);
            max = (oz == 0x3ffff);
            return (farther && nearer && !max);
            break;
        }
        return 0;
    }
    else
    {


        if (other_modes.f.realblendershiftersneeded)
        {
            blshifta = 0;
            if (dzpixenc < 0xb)
                blshiftb = 4;
            else
                blshiftb = 0xf - dzpixenc;
        }

        if (other_modes.f.interpixelblendershiftersneeded)
        {
            pastblshifta = 0;
            if (dzpixenc < 0xb)
                pastblshiftb = 4;
            else
                pastblshiftb = 0xf - dzpixenc;
        }

        pastrawdzmem = 0xf;

        int overflow = (curpixel_memcvg + *curpixel_cvg) & 8;
        *blend_en = other_modes.force_blend || (!overflow && other_modes.antialias_en);
        *prewrap = overflow;

        return 1;
    }
}

static STRICTINLINE int finalize_spanalpha(uint32_t blend_en, uint32_t curpixel_cvg, uint32_t curpixel_memcvg)
{
    int finalcvg;



    switch(other_modes.cvg_dest)
    {
    case CVG_CLAMP:
        if (!blend_en)
        {
            finalcvg = curpixel_cvg - 1;


        }
        else
        {
            finalcvg = curpixel_cvg + curpixel_memcvg;
        }



        if (!(finalcvg & 8))
            finalcvg &= 7;
        else
            finalcvg = 7;

        break;
    case CVG_WRAP:
        finalcvg = (curpixel_cvg + curpixel_memcvg) & 7;
        break;
    case CVG_ZAP:
        finalcvg = 7;
        break;
    case CVG_SAVE:
        finalcvg = curpixel_memcvg;
        break;
    }

    return finalcvg;
}

static STRICTINLINE int32_t normalize_dzpix(int32_t sum)
{
    if (sum & 0xc000)
        return 0x8000;
    if (!(sum & 0xffff))
        return 1;

    if (sum == 1)
        return 3;

    for(int count = 0x2000; count > 0; count >>= 1)
    {
        if (sum & count)
            return(count << 1);
    }
    msg_error("normalize_dzpix: invalid codepath taken");
    return 0;
}

static STRICTINLINE int32_t clamp(int32_t value,int32_t min,int32_t max)
{
    if (value < min)
        return min;
    else if (value > max)
        return max;
    else
        return value;
}

static INLINE void calculate_clamp_diffs(uint32_t i)
{
    tile[i].f.clampdiffs = ((tile[i].sh >> 2) - (tile[i].sl >> 2)) & 0x3ff;
    tile[i].f.clampdifft = ((tile[i].th >> 2) - (tile[i].tl >> 2)) & 0x3ff;
}


static INLINE void calculate_tile_derivs(uint32_t i)
{
    tile[i].f.clampens = tile[i].cs || !tile[i].mask_s;
    tile[i].f.clampent = tile[i].ct || !tile[i].mask_t;
    tile[i].f.masksclamped = tile[i].mask_s <= 10 ? tile[i].mask_s : 10;
    tile[i].f.masktclamped = tile[i].mask_t <= 10 ? tile[i].mask_t : 10;
    tile[i].f.notlutswitch = (tile[i].format << 2) | tile[i].size;
    tile[i].f.tlutswitch = (tile[i].size << 2) | ((tile[i].format + 2) & 3);
}

static INLINE void rgb_dither_complete(int* r, int* g, int* b, int dith)
{

    int32_t newr = *r, newg = *g, newb = *b;
    int32_t rcomp, gcomp, bcomp;


    if (newr > 247)
        newr = 255;
    else
        newr = (newr & 0xf8) + 8;
    if (newg > 247)
        newg = 255;
    else
        newg = (newg & 0xf8) + 8;
    if (newb > 247)
        newb = 255;
    else
        newb = (newb & 0xf8) + 8;

    if (other_modes.rgb_dither_sel != 2)
        rcomp = gcomp = bcomp = dith;
    else
    {
        rcomp = dith & 7;
        gcomp = (dith >> 3) & 7;
        bcomp = (dith >> 6) & 7;
    }





    int32_t replacesign = (rcomp - (*r & 7)) >> 31;

    int32_t ditherdiff = newr - *r;
    *r = *r + (ditherdiff & replacesign);

    replacesign = (gcomp - (*g & 7)) >> 31;
    ditherdiff = newg - *g;
    *g = *g + (ditherdiff & replacesign);

    replacesign = (bcomp - (*b & 7)) >> 31;
    ditherdiff = newb - *b;
    *b = *b + (ditherdiff & replacesign);

}

static INLINE void rgb_dither_nothing(int* r, int* g, int* b, int dith)
{
}


static INLINE void get_dither_noise_complete(int x, int y, int* cdith, int* adith)
{


    noise = ((irand() & 7) << 6) | 0x20;


    int dithindex;
    switch(other_modes.f.rgb_alpha_dither)
    {
    case 0:
        dithindex = ((y & 3) << 2) | (x & 3);
        *adith = *cdith = magic_matrix[dithindex];
        break;
    case 1:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = magic_matrix[dithindex];
        *adith = (~(*cdith)) & 7;
        break;
    case 2:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = magic_matrix[dithindex];
        *adith = (noise >> 6) & 7;
        break;
    case 3:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = magic_matrix[dithindex];
        *adith = 0;
        break;
    case 4:
        dithindex = ((y & 3) << 2) | (x & 3);
        *adith = *cdith = bayer_matrix[dithindex];
        break;
    case 5:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = bayer_matrix[dithindex];
        *adith = (~(*cdith)) & 7;
        break;
    case 6:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = bayer_matrix[dithindex];
        *adith = (noise >> 6) & 7;
        break;
    case 7:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = bayer_matrix[dithindex];
        *adith = 0;
        break;
    case 8:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = irand();
        *adith = magic_matrix[dithindex];
        break;
    case 9:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = irand();
        *adith = (~magic_matrix[dithindex]) & 7;
        break;
    case 10:
        *cdith = irand();
        *adith = (noise >> 6) & 7;
        break;
    case 11:
        *cdith = irand();
        *adith = 0;
        break;
    case 12:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = 7;
        *adith = bayer_matrix[dithindex];
        break;
    case 13:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = 7;
        *adith = (~bayer_matrix[dithindex]) & 7;
        break;
    case 14:
        *cdith = 7;
        *adith = (noise >> 6) & 7;
        break;
    case 15:
        *cdith = 7;
        *adith = 0;
        break;
    }
}


static INLINE void get_dither_only(int x, int y, int* cdith, int* adith)
{
    int dithindex;
    switch(other_modes.f.rgb_alpha_dither)
    {
    case 0:
        dithindex = ((y & 3) << 2) | (x & 3);
        *adith = *cdith = magic_matrix[dithindex];
        break;
    case 1:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = magic_matrix[dithindex];
        *adith = (~(*cdith)) & 7;
        break;
    case 2:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = magic_matrix[dithindex];
        *adith = (noise >> 6) & 7;
        break;
    case 3:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = magic_matrix[dithindex];
        *adith = 0;
        break;
    case 4:
        dithindex = ((y & 3) << 2) | (x & 3);
        *adith = *cdith = bayer_matrix[dithindex];
        break;
    case 5:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = bayer_matrix[dithindex];
        *adith = (~(*cdith)) & 7;
        break;
    case 6:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = bayer_matrix[dithindex];
        *adith = (noise >> 6) & 7;
        break;
    case 7:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = bayer_matrix[dithindex];
        *adith = 0;
        break;
    case 8:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = irand();
        *adith = magic_matrix[dithindex];
        break;
    case 9:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = irand();
        *adith = (~magic_matrix[dithindex]) & 7;
        break;
    case 10:
        *cdith = irand();
        *adith = (noise >> 6) & 7;
        break;
    case 11:
        *cdith = irand();
        *adith = 0;
        break;
    case 12:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = 7;
        *adith = bayer_matrix[dithindex];
        break;
    case 13:
        dithindex = ((y & 3) << 2) | (x & 3);
        *cdith = 7;
        *adith = (~bayer_matrix[dithindex]) & 7;
        break;
    case 14:
        *cdith = 7;
        *adith = (noise >> 6) & 7;
        break;
    case 15:
        *cdith = 7;
        *adith = 0;
        break;
    }
}

static INLINE void get_dither_nothing(int x, int y, int* cdith, int* adith)
{
}



static STRICTINLINE void rgbaz_correct_clip(int offx, int offy, int r, int g, int b, int a, int* z, uint32_t curpixel_cvg)
{
    int summand_r, summand_b, summand_g, summand_a;
    int summand_z;
    int sz = *z;
    int zanded;




    if (curpixel_cvg == 8)
    {
        r >>= 2;
        g >>= 2;
        b >>= 2;
        a >>= 2;
        sz = sz >> 3;
    }
    else
    {
        summand_r = offx * spans_cdr + offy * spans_drdy;
        summand_g = offx * spans_cdg + offy * spans_dgdy;
        summand_b = offx * spans_cdb + offy * spans_dbdy;
        summand_a = offx * spans_cda + offy * spans_dady;
        summand_z = offx * spans_cdz + offy * spans_dzdy;

        r = ((r << 2) + summand_r) >> 4;
        g = ((g << 2) + summand_g) >> 4;
        b = ((b << 2) + summand_b) >> 4;
        a = ((a << 2) + summand_a) >> 4;
        sz = ((sz << 2) + summand_z) >> 5;
    }


    shade_color.r = special_9bit_clamptable[r & 0x1ff];
    shade_color.g = special_9bit_clamptable[g & 0x1ff];
    shade_color.b = special_9bit_clamptable[b & 0x1ff];
    shade_color.a = special_9bit_clamptable[a & 0x1ff];



    zanded = (sz & 0x60000) >> 17;


    switch(zanded)
    {
        case 0: *z = sz & 0x3ffff;                      break;
        case 1: *z = sz & 0x3ffff;                      break;
        case 2: *z = 0x3ffff;                           break;
        case 3: *z = 0;                                 break;
    }
}






static INLINE void tcdiv_nopersp(int32_t ss, int32_t st, int32_t sw, int32_t* sss, int32_t* sst)
{



    *sss = (SIGN16(ss)) & 0x1ffff;
    *sst = (SIGN16(st)) & 0x1ffff;
}

static INLINE void tcdiv_persp(int32_t ss, int32_t st, int32_t sw, int32_t* sss, int32_t* sst)
{


    int w_carry = 0;
    int shift;
    int tlu_rcp;
    int sprod, tprod;
    int outofbounds_s, outofbounds_t;
    int tempmask;
    int shift_value;
    int32_t temps, tempt;



    int overunder_s = 0, overunder_t = 0;


    if (SIGN16(sw) <= 0)
        w_carry = 1;

    sw &= 0x7fff;



    shift = tcdiv_table[sw];
    tlu_rcp = shift >> 4;
    shift &= 0xf;

    sprod = SIGN16(ss) * tlu_rcp;
    tprod = SIGN16(st) * tlu_rcp;




    tempmask = ((1 << 30) - 1) & -((1 << 29) >> shift);

    outofbounds_s = sprod & tempmask;
    outofbounds_t = tprod & tempmask;

    if (shift != 0xe)
    {
        shift_value = 13 - shift;
        temps = sprod = (sprod >> shift_value);
        tempt = tprod = (tprod >> shift_value);
    }
    else
    {
        temps = sprod << 1;
        tempt = tprod << 1;
    }

    if (outofbounds_s != tempmask && outofbounds_s != 0)
    {
        if (!(sprod & (1 << 29)))
            overunder_s = 2 << 17;
        else
            overunder_s = 1 << 17;
    }

    if (outofbounds_t != tempmask && outofbounds_t != 0)
    {
        if (!(tprod & (1 << 29)))
            overunder_t = 2 << 17;
        else
            overunder_t = 1 << 17;
    }

    if (w_carry)
    {
        overunder_s |= (2 << 17);
        overunder_t |= (2 << 17);
    }

    *sss = (temps & 0x1ffff) | overunder_s;
    *sst = (tempt & 0x1ffff) | overunder_t;
}

static STRICTINLINE void tclod_2cycle_current(int32_t* sss, int32_t* sst, int32_t nexts, int32_t nextt, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t prim_tile, int32_t* t1, int32_t* t2)
{








    int nextys, nextyt, nextysw;
    int lodclamp = 0;
    int32_t lod = 0;
    uint32_t l_tile;
    uint32_t magnify = 0;
    uint32_t distant = 0;
    int inits = *sss, initt = *sst;

    tclod_tcclamp(sss, sst);

    if (other_modes.f.dolod)
    {






        nextys = (s + spans_dsdy) >> 16;
        nextyt = (t + spans_dtdy) >> 16;
        nextysw = (w + spans_dwdy) >> 16;

        tcdiv_ptr(nextys, nextyt, nextysw, &nextys, &nextyt);

        lodclamp = (initt & 0x60000) || (nextt & 0x60000) || (inits & 0x60000) || (nexts & 0x60000) || (nextys & 0x60000) || (nextyt & 0x60000);




        if (!lodclamp)
        {
            tclod_4x17_to_15(inits, nexts, initt, nextt, 0, &lod);
            tclod_4x17_to_15(inits, nextys, initt, nextyt, lod, &lod);
        }

        lodfrac_lodtile_signals(lodclamp, lod, &l_tile, &magnify, &distant, &lod_frac);


        if (other_modes.tex_lod_en)
        {
            if (distant)
                l_tile = max_level;
            if (!other_modes.detail_tex_en)
            {
                *t1 = (prim_tile + l_tile) & 7;
                if (!(distant || (!other_modes.sharpen_tex_en && magnify)))
                    *t2 = (*t1 + 1) & 7;
                else
                    *t2 = *t1;
            }
            else
            {
                if (!magnify)
                    *t1 = (prim_tile + l_tile + 1);
                else
                    *t1 = (prim_tile + l_tile);
                *t1 &= 7;
                if (!distant && !magnify)
                    *t2 = (prim_tile + l_tile + 2) & 7;
                else
                    *t2 = (prim_tile + l_tile + 1) & 7;
            }
        }
    }
}


static STRICTINLINE void tclod_2cycle_current_simple(int32_t* sss, int32_t* sst, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t prim_tile, int32_t* t1, int32_t* t2)
{
    int nextys, nextyt, nextysw, nexts, nextt, nextsw;
    int lodclamp = 0;
    int32_t lod = 0;
    uint32_t l_tile;
    uint32_t magnify = 0;
    uint32_t distant = 0;
    int inits = *sss, initt = *sst;

    tclod_tcclamp(sss, sst);

    if (other_modes.f.dolod)
    {
        nextsw = (w + dwinc) >> 16;
        nexts = (s + dsinc) >> 16;
        nextt = (t + dtinc) >> 16;
        nextys = (s + spans_dsdy) >> 16;
        nextyt = (t + spans_dtdy) >> 16;
        nextysw = (w + spans_dwdy) >> 16;

        tcdiv_ptr(nexts, nextt, nextsw, &nexts, &nextt);
        tcdiv_ptr(nextys, nextyt, nextysw, &nextys, &nextyt);

        lodclamp = (initt & 0x60000) || (nextt & 0x60000) || (inits & 0x60000) || (nexts & 0x60000) || (nextys & 0x60000) || (nextyt & 0x60000);

        if (!lodclamp)
        {
            tclod_4x17_to_15(inits, nexts, initt, nextt, 0, &lod);
            tclod_4x17_to_15(inits, nextys, initt, nextyt, lod, &lod);
        }

        lodfrac_lodtile_signals(lodclamp, lod, &l_tile, &magnify, &distant, &lod_frac);

        if (other_modes.tex_lod_en)
        {
            if (distant)
                l_tile = max_level;
            if (!other_modes.detail_tex_en)
            {
                *t1 = (prim_tile + l_tile) & 7;
                if (!(distant || (!other_modes.sharpen_tex_en && magnify)))
                    *t2 = (*t1 + 1) & 7;
                else
                    *t2 = *t1;
            }
            else
            {
                if (!magnify)
                    *t1 = (prim_tile + l_tile + 1);
                else
                    *t1 = (prim_tile + l_tile);
                *t1 &= 7;
                if (!distant && !magnify)
                    *t2 = (prim_tile + l_tile + 2) & 7;
                else
                    *t2 = (prim_tile + l_tile + 1) & 7;
            }
        }
    }
}


static STRICTINLINE void tclod_2cycle_current_notexel1(int32_t* sss, int32_t* sst, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t prim_tile, int32_t* t1)
{
    int nextys, nextyt, nextysw, nexts, nextt, nextsw;
    int lodclamp = 0;
    int32_t lod = 0;
    uint32_t l_tile;
    uint32_t magnify = 0;
    uint32_t distant = 0;
    int inits = *sss, initt = *sst;

    tclod_tcclamp(sss, sst);

    if (other_modes.f.dolod)
    {
        nextsw = (w + dwinc) >> 16;
        nexts = (s + dsinc) >> 16;
        nextt = (t + dtinc) >> 16;
        nextys = (s + spans_dsdy) >> 16;
        nextyt = (t + spans_dtdy) >> 16;
        nextysw = (w + spans_dwdy) >> 16;

        tcdiv_ptr(nexts, nextt, nextsw, &nexts, &nextt);
        tcdiv_ptr(nextys, nextyt, nextysw, &nextys, &nextyt);

        lodclamp = (initt & 0x60000) || (nextt & 0x60000) || (inits & 0x60000) || (nexts & 0x60000) || (nextys & 0x60000) || (nextyt & 0x60000);

        if (!lodclamp)
        {
            tclod_4x17_to_15(inits, nexts, initt, nextt, 0, &lod);
            tclod_4x17_to_15(inits, nextys, initt, nextyt, lod, &lod);
        }

        lodfrac_lodtile_signals(lodclamp, lod, &l_tile, &magnify, &distant, &lod_frac);

        if (other_modes.tex_lod_en)
        {
            if (distant)
                l_tile = max_level;
            if (!other_modes.detail_tex_en || magnify)
                *t1 = (prim_tile + l_tile) & 7;
            else
                *t1 = (prim_tile + l_tile + 1) & 7;
        }

    }
}

static STRICTINLINE void tclod_2cycle_next(int32_t* sss, int32_t* sst, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t prim_tile, int32_t* t1, int32_t* t2, int32_t* prelodfrac)
{
    int nexts, nextt, nextsw, nextys, nextyt, nextysw;
    int lodclamp = 0;
    int32_t lod = 0;
    uint32_t l_tile;
    uint32_t magnify = 0;
    uint32_t distant = 0;
    int inits = *sss, initt = *sst;

    tclod_tcclamp(sss, sst);

    if (other_modes.f.dolod)
    {
        nextsw = (w + dwinc) >> 16;
        nexts = (s + dsinc) >> 16;
        nextt = (t + dtinc) >> 16;
        nextys = (s + spans_dsdy) >> 16;
        nextyt = (t + spans_dtdy) >> 16;
        nextysw = (w + spans_dwdy) >> 16;

        tcdiv_ptr(nexts, nextt, nextsw, &nexts, &nextt);
        tcdiv_ptr(nextys, nextyt, nextysw, &nextys, &nextyt);

        lodclamp = (initt & 0x60000) || (nextt & 0x60000) || (inits & 0x60000) || (nexts & 0x60000) || (nextys & 0x60000) || (nextyt & 0x60000);

        if (!lodclamp)
        {
            tclod_4x17_to_15(inits, nexts, initt, nextt, 0, &lod);
            tclod_4x17_to_15(inits, nextys, initt, nextyt, lod, &lod);
        }

        lodfrac_lodtile_signals(lodclamp, lod, &l_tile, &magnify, &distant, prelodfrac);

        if (other_modes.tex_lod_en)
        {
            if (distant)
                l_tile = max_level;
            if (!other_modes.detail_tex_en)
            {
                *t1 = (prim_tile + l_tile) & 7;
                if (!(distant || (!other_modes.sharpen_tex_en && magnify)))
                    *t2 = (*t1 + 1) & 7;
                else
                    *t2 = *t1;
            }
            else
            {
                if (!magnify)
                    *t1 = (prim_tile + l_tile + 1);
                else
                    *t1 = (prim_tile + l_tile);
                *t1 &= 7;
                if (!distant && !magnify)
                    *t2 = (prim_tile + l_tile + 2) & 7;
                else
                    *t2 = (prim_tile + l_tile + 1) & 7;
            }
        }
    }
}

static STRICTINLINE void tclod_1cycle_current(int32_t* sss, int32_t* sst, int32_t nexts, int32_t nextt, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t scanline, int32_t prim_tile, int32_t* t1, struct spansigs* sigs)
{









    int fars, fart, farsw;
    int lodclamp = 0;
    int32_t lod = 0;
    uint32_t l_tile = 0, magnify = 0, distant = 0;

    tclod_tcclamp(sss, sst);

    if (other_modes.f.dolod)
    {
        int nextscan = scanline + 1;


        if (span[nextscan].validline)
        {
            if (!sigs->endspan || !sigs->longspan)
            {
                if (!(sigs->preendspan && sigs->longspan) && !(sigs->endspan && sigs->midspan))
                {
                    farsw = (w + (dwinc << 1)) >> 16;
                    fars = (s + (dsinc << 1)) >> 16;
                    fart = (t + (dtinc << 1)) >> 16;
                }
                else
                {
                    farsw = (w - dwinc) >> 16;
                    fars = (s - dsinc) >> 16;
                    fart = (t - dtinc) >> 16;
                }
            }
            else
            {
                fart = (span[nextscan].t + dtinc) >> 16;
                fars = (span[nextscan].s + dsinc) >> 16;
                farsw = (span[nextscan].w + dwinc) >> 16;
            }
        }
        else
        {
            farsw = (w + (dwinc << 1)) >> 16;
            fars = (s + (dsinc << 1)) >> 16;
            fart = (t + (dtinc << 1)) >> 16;
        }

        tcdiv_ptr(fars, fart, farsw, &fars, &fart);

        lodclamp = (fart & 0x60000) || (nextt & 0x60000) || (fars & 0x60000) || (nexts & 0x60000);




        if (!lodclamp)
            tclod_4x17_to_15(nexts, fars, nextt, fart, 0, &lod);

        lodfrac_lodtile_signals(lodclamp, lod, &l_tile, &magnify, &distant, &lod_frac);

        if (other_modes.tex_lod_en)
        {
            if (distant)
                l_tile = max_level;



            if (!other_modes.detail_tex_en || magnify)
                *t1 = (prim_tile + l_tile) & 7;
            else
                *t1 = (prim_tile + l_tile + 1) & 7;
        }
    }
}



static STRICTINLINE void tclod_1cycle_current_simple(int32_t* sss, int32_t* sst, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t scanline, int32_t prim_tile, int32_t* t1, struct spansigs* sigs)
{
    int fars, fart, farsw, nexts, nextt, nextsw;
    int lodclamp = 0;
    int32_t lod = 0;
    uint32_t l_tile = 0, magnify = 0, distant = 0;

    tclod_tcclamp(sss, sst);

    if (other_modes.f.dolod)
    {

        int nextscan = scanline + 1;
        if (span[nextscan].validline)
        {
            if (!sigs->endspan || !sigs->longspan)
            {
                nextsw = (w + dwinc) >> 16;
                nexts = (s + dsinc) >> 16;
                nextt = (t + dtinc) >> 16;

                if (!(sigs->preendspan && sigs->longspan) && !(sigs->endspan && sigs->midspan))
                {
                    farsw = (w + (dwinc << 1)) >> 16;
                    fars = (s + (dsinc << 1)) >> 16;
                    fart = (t + (dtinc << 1)) >> 16;
                }
                else
                {
                    farsw = (w - dwinc) >> 16;
                    fars = (s - dsinc) >> 16;
                    fart = (t - dtinc) >> 16;
                }
            }
            else
            {
                nextt = span[nextscan].t >> 16;
                nexts = span[nextscan].s >> 16;
                nextsw = span[nextscan].w >> 16;
                fart = (span[nextscan].t + dtinc) >> 16;
                fars = (span[nextscan].s + dsinc) >> 16;
                farsw = (span[nextscan].w + dwinc) >> 16;
            }
        }
        else
        {
            nextsw = (w + dwinc) >> 16;
            nexts = (s + dsinc) >> 16;
            nextt = (t + dtinc) >> 16;
            farsw = (w + (dwinc << 1)) >> 16;
            fars = (s + (dsinc << 1)) >> 16;
            fart = (t + (dtinc << 1)) >> 16;
        }

        tcdiv_ptr(nexts, nextt, nextsw, &nexts, &nextt);
        tcdiv_ptr(fars, fart, farsw, &fars, &fart);

        lodclamp = (fart & 0x60000) || (nextt & 0x60000) || (fars & 0x60000) || (nexts & 0x60000);

        if (!lodclamp)
            tclod_4x17_to_15(nexts, fars, nextt, fart, 0, &lod);

        lodfrac_lodtile_signals(lodclamp, lod, &l_tile, &magnify, &distant, &lod_frac);

        if (other_modes.tex_lod_en)
        {
            if (distant)
                l_tile = max_level;
            if (!other_modes.detail_tex_en || magnify)
                *t1 = (prim_tile + l_tile) & 7;
            else
                *t1 = (prim_tile + l_tile + 1) & 7;
        }
    }
}

static STRICTINLINE void tclod_1cycle_next(int32_t* sss, int32_t* sst, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t scanline, int32_t prim_tile, int32_t* t1, struct spansigs* sigs, int32_t* prelodfrac)
{
    int nexts, nextt, nextsw, fars, fart, farsw;
    int lodclamp = 0;
    int32_t lod = 0;
    uint32_t l_tile = 0, magnify = 0, distant = 0;

    tclod_tcclamp(sss, sst);

    if (other_modes.f.dolod)
    {

        int nextscan = scanline + 1;

        if (span[nextscan].validline)
        {

            if (!sigs->nextspan)
            {
                if (!sigs->endspan || !sigs->longspan)
                {
                    nextsw = (w + dwinc) >> 16;
                    nexts = (s + dsinc) >> 16;
                    nextt = (t + dtinc) >> 16;

                    if (!(sigs->preendspan && sigs->longspan) && !(sigs->endspan && sigs->midspan))
                    {
                        farsw = (w + (dwinc << 1)) >> 16;
                        fars = (s + (dsinc << 1)) >> 16;
                        fart = (t + (dtinc << 1)) >> 16;
                    }
                    else
                    {
                        farsw = (w - dwinc) >> 16;
                        fars = (s - dsinc) >> 16;
                        fart = (t - dtinc) >> 16;
                    }
                }
                else
                {
                    nextt = span[nextscan].t;
                    nexts = span[nextscan].s;
                    nextsw = span[nextscan].w;
                    fart = (nextt + dtinc) >> 16;
                    fars = (nexts + dsinc) >> 16;
                    farsw = (nextsw + dwinc) >> 16;
                    nextt >>= 16;
                    nexts >>= 16;
                    nextsw >>= 16;
                }
            }
            else
            {









                if (sigs->longspan)
                {
                    nextt = (span[nextscan].t + dtinc) >> 16;
                    nexts = (span[nextscan].s + dsinc) >> 16;
                    nextsw = (span[nextscan].w + dwinc) >> 16;
                    fart = (span[nextscan].t + (dtinc << 1)) >> 16;
                    fars = (span[nextscan].s + (dsinc << 1)) >> 16;
                    farsw = (span[nextscan].w  + (dwinc << 1)) >> 16;
                }
                else if (sigs->midspan)
                {
                    nextt = span[nextscan].t >> 16;
                    nexts = span[nextscan].s >> 16;
                    nextsw = span[nextscan].w >> 16;
                    fart = (span[nextscan].t + dtinc) >> 16;
                    fars = (span[nextscan].s + dsinc) >> 16;
                    farsw = (span[nextscan].w  + dwinc) >> 16;
                }
                else if (sigs->onelessthanmid)
                {
                    nextsw = (w + dwinc) >> 16;
                    nexts = (s + dsinc) >> 16;
                    nextt = (t + dtinc) >> 16;
                    farsw = (w - dwinc) >> 16;
                    fars = (s - dsinc) >> 16;
                    fart = (t - dtinc) >> 16;
                }
                else
                {
                    nextt = (t + dtinc) >> 16;
                    nexts = (s + dsinc) >> 16;
                    nextsw = (w + dwinc) >> 16;
                    fart = (t + (dtinc << 1)) >> 16;
                    fars = (s + (dsinc << 1)) >> 16;
                    farsw = (w + (dwinc << 1)) >> 16;
                }
            }
        }
        else
        {
            nextsw = (w + dwinc) >> 16;
            nexts = (s + dsinc) >> 16;
            nextt = (t + dtinc) >> 16;
            farsw = (w + (dwinc << 1)) >> 16;
            fars = (s + (dsinc << 1)) >> 16;
            fart = (t + (dtinc << 1)) >> 16;
        }

        tcdiv_ptr(nexts, nextt, nextsw, &nexts, &nextt);
        tcdiv_ptr(fars, fart, farsw, &fars, &fart);

        lodclamp = (fart & 0x60000) || (nextt & 0x60000) || (fars & 0x60000) || (nexts & 0x60000);



        if (!lodclamp)
            tclod_4x17_to_15(nexts, fars, nextt, fart, 0, &lod);

        lodfrac_lodtile_signals(lodclamp, lod, &l_tile, &magnify, &distant, prelodfrac);

        if (other_modes.tex_lod_en)
        {
            if (distant)
                l_tile = max_level;
            if (!other_modes.detail_tex_en || magnify)
                *t1 = (prim_tile + l_tile) & 7;
            else
                *t1 = (prim_tile + l_tile + 1) & 7;
        }
    }
}

static STRICTINLINE void tclod_copy(int32_t* sss, int32_t* sst, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t prim_tile, int32_t* t1)
{




    int nexts, nextt, nextsw, fars, fart, farsw;
    int lodclamp = 0;
    int32_t lod = 0;
    uint32_t l_tile = 0, magnify = 0, distant = 0;

    tclod_tcclamp(sss, sst);

    if (other_modes.tex_lod_en)
    {



        nextsw = (w + dwinc) >> 16;
        nexts = (s + dsinc) >> 16;
        nextt = (t + dtinc) >> 16;
        farsw = (w + (dwinc << 1)) >> 16;
        fars = (s + (dsinc << 1)) >> 16;
        fart = (t + (dtinc << 1)) >> 16;

        tcdiv_ptr(nexts, nextt, nextsw, &nexts, &nextt);
        tcdiv_ptr(fars, fart, farsw, &fars, &fart);

        lodclamp = (fart & 0x60000) || (nextt & 0x60000) || (fars & 0x60000) || (nexts & 0x60000);

        if (!lodclamp)
            tclod_4x17_to_15(nexts, fars, nextt, fart, 0, &lod);

        if ((lod & 0x4000) || lodclamp)
        {


            magnify = 0;
            l_tile = max_level;
        }
        else if (lod < 32)
        {
            magnify = 1;
            l_tile = 0;
        }
        else
        {
            magnify = 0;
            l_tile =  log2table[(lod >> 5) & 0xff];

            if (max_level)
                distant = ((lod & 0x6000) || (l_tile >= max_level)) ? 1 : 0;
            else
                distant = 1;

            if (distant)
                l_tile = max_level;
        }

        if (!other_modes.detail_tex_en || magnify)
            *t1 = (prim_tile + l_tile) & 7;
        else
            *t1 = (prim_tile + l_tile + 1) & 7;
    }

}

static STRICTINLINE void get_texel1_1cycle(int32_t* s1, int32_t* t1, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc, int32_t scanline, struct spansigs* sigs)
{
    int32_t nexts, nextt, nextsw;

    if (!sigs->endspan || !sigs->longspan || !span[scanline + 1].validline)
    {


        nextsw = (w + dwinc) >> 16;
        nexts = (s + dsinc) >> 16;
        nextt = (t + dtinc) >> 16;
    }
    else
    {







        int32_t nextscan = scanline + 1;
        nextt = span[nextscan].t >> 16;
        nexts = span[nextscan].s >> 16;
        nextsw = span[nextscan].w >> 16;
    }

    tcdiv_ptr(nexts, nextt, nextsw, s1, t1);
}

static STRICTINLINE void get_nexttexel0_2cycle(int32_t* s1, int32_t* t1, int32_t s, int32_t t, int32_t w, int32_t dsinc, int32_t dtinc, int32_t dwinc)
{


    int32_t nexts, nextt, nextsw;
    nextsw = (w + dwinc) >> 16;
    nexts = (s + dsinc) >> 16;
    nextt = (t + dtinc) >> 16;

    tcdiv_ptr(nexts, nextt, nextsw, s1, t1);
}



static STRICTINLINE void tclod_4x17_to_15(int32_t scurr, int32_t snext, int32_t tcurr, int32_t tnext, int32_t previous, int32_t* lod)
{



    int dels = SIGN(snext, 17) - SIGN(scurr, 17);
    if (dels & 0x20000)
        dels = ~dels & 0x1ffff;
    int delt = SIGN(tnext, 17) - SIGN(tcurr, 17);
    if(delt & 0x20000)
        delt = ~delt & 0x1ffff;


    dels = (dels > delt) ? dels : delt;
    dels = (previous > dels) ? previous : dels;
    *lod = dels & 0x7fff;
    if (dels & 0x1c000)
        *lod |= 0x4000;
}

static STRICTINLINE void tclod_tcclamp(int32_t* sss, int32_t* sst)
{
    int32_t tempanded, temps = *sss, tempt = *sst;





    if (!(temps & 0x40000))
    {
        if (!(temps & 0x20000))
        {
            tempanded = temps & 0x18000;
            if (tempanded != 0x8000)
            {
                if (tempanded != 0x10000)
                    *sss &= 0xffff;
                else
                    *sss = 0x8000;
            }
            else
                *sss = 0x7fff;
        }
        else
            *sss = 0x8000;
    }
    else
        *sss = 0x7fff;

    if (!(tempt & 0x40000))
    {
        if (!(tempt & 0x20000))
        {
            tempanded = tempt & 0x18000;
            if (tempanded != 0x8000)
            {
                if (tempanded != 0x10000)
                    *sst &= 0xffff;
                else
                    *sst = 0x8000;
            }
            else
                *sst = 0x7fff;
        }
        else
            *sst = 0x8000;
    }
    else
        *sst = 0x7fff;

}


static STRICTINLINE void lodfrac_lodtile_signals(int lodclamp, int32_t lod, uint32_t* l_tile, uint32_t* magnify, uint32_t* distant, int32_t* lfdst)
{
    uint32_t ltil, dis, mag;
    int32_t lf;


    if ((lod & 0x4000) || lodclamp)
    {


        mag = 0;
        ltil = 7;
        dis = 1;
        lf = 0xff;
    }
    else if (lod < min_level)
    {


        mag = 1;
        ltil = 0;
        dis = max_level ? 0 : 1;

        if(!other_modes.sharpen_tex_en && !other_modes.detail_tex_en)
        {
            if (dis)
                lf = 0xff;
            else
                lf = 0;
        }
        else
        {
            lf = min_level << 3;
            if (other_modes.sharpen_tex_en)
                lf |= 0x100;
        }
    }
    else if (lod < 32)
    {
        mag = 1;
        ltil = 0;
        dis = max_level ? 0 : 1;

        if(!other_modes.sharpen_tex_en && !other_modes.detail_tex_en)
        {
            if (dis)
                lf = 0xff;
            else
                lf = 0;
        }
        else
        {
            lf = lod << 3;
            if (other_modes.sharpen_tex_en)
                lf |= 0x100;
        }
    }
    else
    {
        mag = 0;
        ltil =  log2table[(lod >> 5) & 0xff];

        if (max_level)
            dis = ((lod & 0x6000) || (ltil >= max_level)) ? 1 : 0;
        else
            dis = 1;


        if(!other_modes.sharpen_tex_en && !other_modes.detail_tex_en && dis)
            lf = 0xff;
        else
            lf = ((lod << 3) >> ltil) & 0xff;






    }

    *distant = dis;
    *l_tile = ltil;
    *magnify = mag;
    *lfdst = lf;
}
