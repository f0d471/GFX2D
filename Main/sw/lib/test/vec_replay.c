/******************************************************************************
* @file vec_replay.c: 一致性向量的读取与重放
*
* @brief  交换格式的解析照 spec 第 02 章规范条款 §7.2 写：
*         [vector] 起一条，[surf] 登记表面，[list] 开一张命令表，
*         其后的 [cmd] 属于该表直到下一段，[mem] 是初始内容，[exp] 是期望内容。
*
*         设备地址空间用一块 64 KiB 的内存模拟，dev_base = 0。
*         向量里的表面基址落在 0x0000_1000 ~ 0x0000_E000，
*         [mem]/[exp] 触及的最高地址是 0x0000_D0xx，一块就放得下；
*         另有一条 base = 0xFFFF_FFF0 的向量是故意让末像素溢出 32 位的非法描述符，
*         它在表面结构检查那一步就被拒，不会有人去访问它。
******************************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "vec_replay.h"
#include "ref_exec.h"
#include "core/format.h"

#define DEV_BYTES 0x10000u

static void report(const char *name, const char *why)
{
    char     line[256];
    core_out o;

    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_str(&o, "  ");
    core_out_str(&o, name);
    core_out_str(&o, " FAIL   ");
    core_out_str(&o, why);
    core_out_str(&o, "\n");
    fputs(line, stdout);
}

static uint32_t parse_u32(const char *s)
{
    return (uint32_t)strtoul(s, 0, 0);
}

/* 从 "key=value" 形式的一行里取某个键。找到返回值的起始位置，否则返回 NULL。
   只看以空白分隔的 token，且遇到 '(' 开头的 token 就停：
   [surf] 行末尾那段括号里的复述也含 '='，不是数据。 */
static const char *field(const char *line, const char *key)
{
    size_t klen = strlen(key);
    const char *p = line;

    while (*p != '\0') {
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '(' || *p == '\0')
            return 0;
        if (strncmp(p, key, klen) == 0 && p[klen] == '=')
            return p + klen + 1;
        while (*p != '\0' && *p != ' ' && *p != '\t')
            p++;
    }
    return 0;
}

static void copy_token(char *dst, uint32_t cap, const char *src)
{
    uint32_t i = 0u;

    if (src == 0) {
        dst[0] = '\0';
        return;
    }
    while (src[i] != '\0' && src[i] != ' ' && src[i] != '\t'
        && src[i] != '\n' && src[i] != '\r' && i + 1u < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void parse_clip(const char *s, int32_t *out)
{
    int i;

    for (i = 0; i < 4; i++) {
        out[i] = (int32_t)strtol(s, (char **)&s, 10);
        if (*s == ',')
            s++;
    }
}

static void vec_clear(vec_case *v)
{
    memset(v, 0, sizeof *v);
    v->reads = -1;
}

/* 把一条向量的初始状态摆好，然后按 nlist 张表依次跑。 */
int vec_run_one(const vec_case *v, int use_ref, char *why, uint32_t cap)
{
    static uint8_t     dev[DEV_BYTES];
    gfx_surface_table  t;
    gfx_mem            m;
    exec_sw_state      es;
    gfx_rect           refclip;
    exec_sw_stats      st;
    exec_sw_error      err;
    core_out           o;
    uint32_t           i, base;
    uint32_t           gotlist = 0u;
    int                code = GFX_OK;

    core_out_init(&o, why, cap);

    memset(dev, 0, sizeof dev);
    m.dev_base = 0u;
    m.host     = dev;
    m.bytes    = DEV_BYTES;

    gfx_surface_reset(&t);
    for (i = 0u; i < GFX_SURF_N; i++) {
        if ((v->surf_seen & (1u << i)) != 0u)
            gfx_surface_set_raw(&t, (int)i, v->surf[i].base,
                                v->surf[i].dim, v->surf[i].ctrl);
    }

    for (i = 0u; i < v->nmem; i++) {
        if (v->mem_addr[i] + 4u <= DEV_BYTES)
            *(uint32_t *)(void *)(dev + v->mem_addr[i]) = v->mem_val[i];
    }

    exec_sw_stats_clear(&st);
    exec_sw_reset(&es);
    ref_exec_reset(&refclip);

    base = 0u;
    for (i = 0u; i < v->nlist; i++) {
        if (use_ref)
            code = ref_exec_run(&refclip, &t, &m, &v->cmds[base],
                                v->listcount[i], v->listncmd[i], &st, &err);
        else
            code = exec_sw_run(&es, &t, &m, &v->cmds[base],
                               v->listcount[i], v->listncmd[i], &st, &err);
        gotlist = i;
        if (code != GFX_OK)
            break;
        base += v->listncmd[i];
    }

    if (code != v->code) {
        core_out_str(&o, "code got=");
        core_out_u32(&o, (uint32_t)code);
        core_out_str(&o, " expect=");
        core_out_u32(&o, (uint32_t)v->code);
        return 0;
    }
    if (code != GFX_OK) {
        if (gotlist != v->errlist) {
            core_out_str(&o, "errlist got=");
            core_out_u32(&o, gotlist);
            core_out_str(&o, " expect=");
            core_out_u32(&o, v->errlist);
            return 0;
        }
        if (err.index != v->erridx) {
            core_out_str(&o, "erridx got=");
            core_out_u32(&o, err.index);
            core_out_str(&o, " expect=");
            core_out_u32(&o, v->erridx);
            return 0;
        }
        if (err.tag != v->errtag) {
            core_out_str(&o, "errtag got=");
            core_out_hex32(&o, err.tag);
            core_out_str(&o, " expect=");
            core_out_hex32(&o, v->errtag);
            return 0;
        }
    }

    if (v->reads >= 0 && st.records_read != (uint32_t)v->reads) {
        core_out_str(&o, "reads got=");
        core_out_u32(&o, st.records_read);
        core_out_str(&o, " expect=");
        core_out_u32(&o, (uint32_t)v->reads);
        return 0;
    }

    {
        const gfx_rect *cl = use_ref ? &refclip : &es.clip;

        if (cl->x0 != v->clip[0] || cl->y0 != v->clip[1]
         || cl->x1 != v->clip[2] || cl->y1 != v->clip[3]) {
            core_out_str(&o, "clip got=");
            core_out_u32(&o, (uint32_t)cl->x0);
            core_out_str(&o, ",");
            core_out_u32(&o, (uint32_t)cl->y0);
            core_out_str(&o, ",");
            core_out_u32(&o, (uint32_t)cl->x1);
            core_out_str(&o, ",");
            core_out_u32(&o, (uint32_t)cl->y1);
            return 0;
        }
    }

    for (i = 0u; i < v->nexp; i++) {
        uint32_t got;

        if (v->exp_addr[i] + 4u > DEV_BYTES)
            continue;
        got = *(uint32_t *)(void *)(dev + v->exp_addr[i]);
        if (got != v->exp_val[i]) {
            core_out_str(&o, "mem[");
            core_out_hex32(&o, v->exp_addr[i]);
            core_out_str(&o, "] got=");
            core_out_hex32(&o, got);
            core_out_str(&o, " expect=");
            core_out_hex32(&o, v->exp_val[i]);
            return 0;
        }
    }

    /* 对账：裁剪后的像素只有三种结局。 */
    if (st.pixel_total != st.pixel_written + st.key_skip + st.src_oob) {
        core_out_str(&o, "pixel account total=");
        core_out_u32(&o, st.pixel_total);
        core_out_str(&o, " w=");
        core_out_u32(&o, st.pixel_written);
        core_out_str(&o, " k=");
        core_out_u32(&o, st.key_skip);
        core_out_str(&o, " o=");
        core_out_u32(&o, st.src_oob);
        return 0;
    }

    return 1;
}

int vec_replay_file(const char *path, int use_ref, uint32_t *ntotal)
{
    FILE     *f;
    char      line[512];
    vec_case  v;
    int       have = 0;
    int       fails = 0;
    uint32_t  total = 0u;
    int32_t   curlist = -1;

    f = fopen(path, "r");
    if (f == 0)
        return -1;

    vec_clear(&v);

    while (fgets(line, (int)sizeof line, f) != 0) {
        if (line[0] == '#')
            continue;

        if (strncmp(line, "[vector]", 8) == 0) {
            if (have) {
                char why[160];

                total++;
                if (!vec_run_one(&v, use_ref, why, (uint32_t)sizeof why)) {
                    report(v.name, why);
                    fails++;
                }
            }
            vec_clear(&v);
            have = 1;
            curlist = -1;

            copy_token(v.group, (uint32_t)sizeof v.group, field(line, "group"));
            copy_token(v.name,  (uint32_t)sizeof v.name,  field(line, "name"));
            v.nlist   = parse_u32(field(line, "lists"));
            v.reads   = (int32_t)strtol(field(line, "reads"), 0, 10);
            v.code    = (int)parse_u32(field(line, "code"));
            v.errlist = parse_u32(field(line, "errlist"));
            v.erridx  = parse_u32(field(line, "erridx"));
            v.errtag  = parse_u32(field(line, "errtag"));
            parse_clip(field(line, "clip"), v.clip);
            if (v.nlist > VEC_MAX_LIST)
                v.nlist = VEC_MAX_LIST;
        } else if (strncmp(line, "[surf]", 6) == 0) {
            uint32_t slot = parse_u32(field(line, "slot"));

            if (slot < GFX_SURF_N) {
                v.surf[slot].base = parse_u32(field(line, "base"));
                v.surf[slot].dim  = (parse_u32(field(line, "width")) & 0xFFFFu)
                                  | ((parse_u32(field(line, "height")) & 0xFFFFu) << 16);
                v.surf[slot].ctrl = parse_u32(field(line, "ctrl"));
                v.surf_seen |= 1u << slot;
            }
        } else if (strncmp(line, "[list]", 6) == 0) {
            curlist++;
            if (curlist < (int32_t)VEC_MAX_LIST)
                v.listcount[curlist] = parse_u32(field(line, "count"));
        } else if (strncmp(line, "[cmd]", 5) == 0) {
            const char *p = line + 5;
            uint32_t    i;

            if (curlist >= 0 && curlist < (int32_t)VEC_MAX_LIST
             && v.ncmd < VEC_MAX_CMD) {
                for (i = 0u; i < 8u; i++)
                    v.cmds[v.ncmd].w[i] = (uint32_t)strtoul(p, (char **)&p, 0);
                v.ncmd++;
                v.listncmd[curlist]++;
            }
        } else if (strncmp(line, "[mem]", 5) == 0) {
            const char *p = line + 5;

            if (v.nmem < VEC_MAX_MEM) {
                v.mem_addr[v.nmem] = (uint32_t)strtoul(p, (char **)&p, 0);
                v.mem_val[v.nmem]  = (uint32_t)strtoul(p, (char **)&p, 0);
                v.nmem++;
            }
        } else if (strncmp(line, "[exp]", 5) == 0) {
            const char *p = line + 5;

            if (v.nexp < VEC_MAX_MEM) {
                v.exp_addr[v.nexp] = (uint32_t)strtoul(p, (char **)&p, 0);
                v.exp_val[v.nexp]  = (uint32_t)strtoul(p, (char **)&p, 0);
                v.nexp++;
            }
        }
    }

    if (have) {
        char why[160];

        total++;
        if (!vec_run_one(&v, use_ref, why, (uint32_t)sizeof why)) {
            report(v.name, why);
            fails++;
        }
    }

    fclose(f);
    if (ntotal != 0)
        *ntotal = total;
    return fails;
}
