`timescale 1ns/1ps
//==============================================================================================//
// FILE: engine_rast.v
//
// DESCRIPTION: R2 三维光栅核：消费 engine_rlst 交付的 128B 参数槽（双缓冲在槽内），
//   复用 4 个 19×18 DSP 做位置求值（30 个乘积分 8 拍发射），1px/cycle 增量扫描，
//   z 四 bank（2048+512×16b，strict LESS），RAST_TEX 时固定延迟采样+色键+调制，
//   原子提交（色与 z 同拍写），三角形边界排空像素流水后才换参数或收条带。
//
// NOTE:
//   1. 数值契约见 Main/doc/三维数值契约.md：半值向 +∞、两级舍入不可合并、
//      基值 z×65536 / UV×4096 / RGB×65536、原点 Q12.4、属性累加器 48 位、
//      覆盖 = 三条 E+bias≥0（bias 由 A_px/B_px 符号推导）
//   2. 本模块只认槽寄存器里的记录；记录字段合法性在这里校验（BAD_FIELD=2），
//      索引/流协议归 engine_rlst；首错后停止提交但继续退休在途像素
//   3. 三角形屏障：扫描结束且 in_flight=0 才置槽空闲（深度拒绝/色键跳过也算
//      退休）；不依赖 BRAM WRITE_FIRST 或跨三角形旁路
//   4. z/TEX 返回按固定延迟对齐，先落 8 项寄存器队列；发射至提交共用 8 个
//      credit，响应无 ready，提交端停顿不会丢失返回；配置在整帧期间保持
//   5. 调制 /255 用精确整数恒等式 (x+1+(x>>8))>>8（值域 0..65152 穷举零误差）
//   6. 注错开关见 engine-manual.md；TEX_LATENCY 必须与 engine_tex.READ_LATENCY
//      一致（由构建入口保证）
//==============================================================================================//
`default_nettype none
`include "engine_feature.vh"
`include "engine_regs_offsets.vh"

module engine_rast #(
    parameter integer TEX_LATENCY = 1
)(
    input  wire         clk,
    input  wire         rst,
    input  wire         cancel,

    // 参数槽（engine_rlst 直写；槽寄存器在本模块）
    input  wire         rp_we,
    input  wire [1:0]   rp_slot,
    input  wire [4:0]   rp_wa,
    input  wire [127:0] rp_wd,
    input  wire         rp_valid,
    input  wire [1:0]   rp_slot_v,
    input  wire [15:0]  rp_index,
    input  wire [4:0]   rp_strip,
    input  wire [15:0]  rp_ordinal,
    output wire [1:0]   rp_free,        // ~slot_busy

    // BAND 光栅写口（租约由帧层给 engine_band；本模块只发写）
    output wire         ras_wr_valid,
    output wire [13:0]  ras_wr_pix,
    output wire [31:0]  ras_wr_data,
    input  wire         ras_wr_ready,

    // z 清零（帧层在条带边界驱动；与扫描互斥由帧层保证）
    input  wire         z_clear,
    output wire         z_clear_busy,
    output reg          z_clear_done,

    // TEX 采样口（GFX_HAS_RAST_TEX；与 engine_tex 新采样口直连）
`ifdef GFX_HAS_RAST_TEX
    output wire         samp_valid,
    input  wire         samp_ready,
    output wire [11:0]  samp_word,
    input  wire         samp_rvalid,
    input  wire [31:0]  samp_rdata,
    input  wire [12:0]  tex_width,      // atlas 尺寸（v1: 64×64）
    input  wire [12:0]  tex_height,
    input  wire [3:0]   tex_slog2,      // 行距 log2（texel 词）
    input  wire [31:0]  ras_key,
`endif

    // 错误（BAD_FIELD；单拍脉冲，负载同拍有效）
    output reg          err_valid,
    output reg  [3:0]   err_code,
    output reg  [15:0]  err_index,
    output reg  [31:0]  err_tag,        // {11'd0, strip, ref_ordinal}

    // 仿真观测（综合悬空）
    output reg  [31:0]  dbg_retire_cnt
);
    localparam [3:0] EC_BAD_FIELD = 4'd2;

    localparam [2:0] M_IDLE = 0, M_VALID = 1, M_INIT = 2, M_SCAN = 3, M_DRAIN = 4;

    // ============================================================ 参数槽
    reg [31:0] slot0 [0:31];
    reg [31:0] slot1 [0:31];
    reg        slot_busy0, slot_busy1;
    reg [15:0] busy_idx0, busy_idx1;
    reg [4:0]  busy_strip0, busy_strip1;
    reg [15:0] busy_ord0, busy_ord1;
    assign rp_free = {~slot_busy1, ~slot_busy0};

    always @(posedge clk) begin
        if (rp_we) begin
            if (rp_slot[0] == 1'b0) begin
                {slot0[rp_wa+3], slot0[rp_wa+2], slot0[rp_wa+1], slot0[rp_wa]} <= rp_wd;
            end else begin
                {slot1[rp_wa+3], slot1[rp_wa+2], slot1[rp_wa+1], slot1[rp_wa]} <= rp_wd;
            end
        end
    end

    // 活动槽选择（M_VALID 期锁定；deliver 也可落在另一槽）
    reg         act_sel;
    wire [15:0] act_index = act_sel ? busy_idx1 : busy_idx0;
    wire [15:0] act_ordinal = act_sel ? busy_ord1 : busy_ord0;
    wire [4:0]  act_strip = act_sel ? busy_strip1 : busy_strip0;
    wire [31:0] w_flags = act_sel ? slot1[0]  : slot0[0];
    wire signed [31:0] w_a0 = act_sel ? slot1[1]  : slot0[1];
    wire signed [31:0] w_b0 = act_sel ? slot1[2]  : slot0[2];
    wire signed [31:0] w_c0 = act_sel ? slot1[3]  : slot0[3];
    wire signed [31:0] w_a1 = act_sel ? slot1[4]  : slot0[4];
    wire signed [31:0] w_b1 = act_sel ? slot1[5]  : slot0[5];
    wire signed [31:0] w_c1 = act_sel ? slot1[6]  : slot0[6];
    wire signed [31:0] w_a2 = act_sel ? slot1[7]  : slot0[7];
    wire signed [31:0] w_b2 = act_sel ? slot1[8]  : slot0[8];
    wire signed [31:0] w_c2 = act_sel ? slot1[9]  : slot0[9];
    wire [15:0]  w_z0 = act_sel ? slot1[10][15:0] : slot0[10][15:0];
    wire [15:0]  w_rsvd_a = act_sel ? slot1[10][31:16] : slot0[10][31:16];
    wire signed [31:0] w_gzx = act_sel ? slot1[11] : slot0[11];
    wire signed [31:0] w_gzy = act_sel ? slot1[12] : slot0[12];
    wire [15:0]  w_u0 = act_sel ? slot1[13][15:0]  : slot0[13][15:0];
    wire [15:0]  w_v0 = act_sel ? slot1[13][31:16] : slot0[13][31:16];
    wire signed [31:0] w_gux = act_sel ? slot1[14] : slot0[14];
    wire signed [31:0] w_guy = act_sel ? slot1[15] : slot0[15];
    wire signed [31:0] w_gvx = act_sel ? slot1[16] : slot0[16];
    wire signed [31:0] w_gvy = act_sel ? slot1[17] : slot0[17];
    wire signed [15:0] w_x0 = act_sel ? slot1[18][15:0]  : slot0[18][15:0];
    wire signed [15:0] w_x1 = act_sel ? slot1[18][31:16] : slot0[18][31:16];
    wire signed [15:0] w_y0 = act_sel ? slot1[19][15:0]  : slot0[19][15:0];
    wire signed [15:0] w_y1 = act_sel ? slot1[19][31:16] : slot0[19][31:16];
    wire [31:0]  w_col0 = act_sel ? slot1[20] : slot0[20];
    wire [31:0]  w_rsvd_b = act_sel ? slot1[23] : slot0[23];
    wire signed [31:0] w_grx = act_sel ? slot1[24] : slot0[24];
    wire signed [31:0] w_gry = act_sel ? slot1[25] : slot0[25];
    wire signed [31:0] w_ggx = act_sel ? slot1[26] : slot0[26];
    wire signed [31:0] w_ggy = act_sel ? slot1[27] : slot0[27];
    wire signed [31:0] w_gbx = act_sel ? slot1[28] : slot0[28];
    wire signed [31:0] w_gby = act_sel ? slot1[29] : slot0[29];
    wire signed [15:0] w_ox = act_sel ? slot1[30][15:0]  : slot0[30][15:0];
    wire signed [15:0] w_oy = act_sel ? slot1[30][31:16] : slot0[30][31:16];
    wire [31:0]  w_rsvd_c = act_sel ? slot1[31] : slot0[31];

    // ============================================================ 记录校验（组合，M_VALID 用）
    wire flags_bad = (w_flags & ~32'd7) != 32'd0;
`ifdef GFX_HAS_RAST_TEX
    wire tex_cap_bad = 1'b0;
`else
    wire tex_cap_bad = w_flags[0];
`endif
    wire rsvd_bad   = (w_rsvd_a != 16'd0) || (w_rsvd_b != 32'd0) || (w_rsvd_c != 32'd0);
    wire bbox_bad   = (w_x0 < 16'sd0) || (w_x1 < w_x0) || (w_x1 > 16'sd639) ||
                      (w_y0 < 16'sd0) || (w_y1 < w_y0) || (w_y1 > 16'sd479);
    wire origin_bad = (w_ox < -16'sd4096) || (w_ox > 16'sd14335) ||
                      (w_oy < -16'sd4096) || (w_oy > 16'sd11775);
    // 严格检查整个界外区间，不能只拒绝两个边界值。
    function grad_bad;
        input signed [31:0] g;
        begin
            grad_bad = (g >= 32'sd1073741824) || (g <= -32'sd1073741824);
        end
    endfunction
    wire grad_bad_all =
        grad_bad(w_gzx) || grad_bad(w_gzy) || grad_bad(w_gux) || grad_bad(w_guy) ||
        grad_bad(w_gvx) || grad_bad(w_gvy) || grad_bad(w_grx) || grad_bad(w_gry) ||
        grad_bad(w_ggx) || grad_bad(w_ggy) || grad_bad(w_gbx) || grad_bad(w_gby);
    function coef_bad;
        input signed [31:0] a;
        input signed [31:0] b;
        input signed [31:0] c;
        begin
            coef_bad = (a[3:0] != 4'd0) || (b[3:0] != 4'd0) ||
                       (a > 32'sd524288)  || (a < -32'sd524288) ||
                       (b > 32'sd524288)  || (b < -32'sd524288) ||
                       (c > 32'sd536870912) || (c < -32'sd536870912);
        end
    endfunction
    wire coef_bad_all =
        coef_bad(w_a0, w_b0, w_c0) || coef_bad(w_a1, w_b1, w_c1) || coef_bad(w_a2, w_b2, w_c2);
    wire record_bad = flags_bad || tex_cap_bad || rsvd_bad || bbox_bad ||
                      origin_bad || grad_bad_all || coef_bad_all;

    // ============================================================ z RAM（四 bank，2048+512×16b）
    localparam integer ZBIG = 2048;
    wire [15:0] z_rdq [0:3];

    // 读口：扫描发射级驱动；写口：提交与清零复用（帧层保证清零不与扫描重叠）
    wire        zrd_en;
    wire [13:0] zrd_pix;
    wire [1:0]  zrd_bank = zrd_pix[1:0];
    wire [11:0] zrd_row  = zrd_pix[13:2];
    wire        zwr_en;
    wire [13:0] zwr_pix;
    wire [15:0] zwr_data;
    wire        clr_we;
    wire [11:0] clr_row;

    // 写口统一：清零优先
    wire        zwr_we   = clr_we || zwr_en;
    wire [1:0]  zwr_bank = clr_we ? 2'd0 : zwr_pix[1:0];   // 逐 bank 写块里按 bank 分派
    wire [11:0] zwr_row  = clr_we ? clr_row : zwr_pix[13:2];
    wire [15:0] zwr_dat  = clr_we ? 16'hFFFF : zwr_data;

    // 各段各自保留同步输出寄存器，段选随读地址延迟一拍。
    // 不能在一个动态索引的二维数组上混写四 bank，否则工具会展开为 FF。
    genvar bank;
    generate for(bank=0;bank<4;bank=bank+1) begin : g_zbank
        reg [15:0] mem_big [0:2047];
        reg [15:0] mem_small [0:511];
        reg [15:0] q_big,q_small;
        reg q_sel;
        always @(posedge clk) begin
            if(zrd_en && zrd_bank==bank) begin
                q_sel <= zrd_row >= ZBIG;
                if(zrd_row >= ZBIG) q_small <= mem_small[zrd_row[8:0]];
                else q_big <= mem_big[zrd_row[10:0]];
            end
            if(clr_we || (zwr_en && zwr_bank==bank)) begin
                if(zwr_row >= ZBIG) mem_small[zwr_row[8:0]] <= zwr_dat;
                else mem_big[zwr_row[10:0]] <= zwr_dat;
            end
        end
        assign z_rdq[bank]=q_sel?q_small:q_big;
    end endgenerate
    reg        clr_busy;
    reg [11:0] clr_cnt;
    assign z_clear_busy = clr_busy;
    assign clr_we = clr_busy;
    assign clr_row = clr_cnt;
    always @(posedge clk) begin
        if (rst) begin
            clr_busy     <= 1'b0;
            clr_cnt      <= 12'd0;
            z_clear_done <= 1'b0;
        end else begin
            z_clear_done <= 1'b0;
            if (z_clear && !clr_busy) begin
                clr_busy <= 1'b1;
                clr_cnt  <= 12'd0;
            end else if (clr_busy) begin
                if (clr_cnt == 12'd2559) begin
                    clr_busy     <= 1'b0;
                    z_clear_done <= 1'b1;
                end else begin
                    clr_cnt <= clr_cnt + 12'd1;
                end
            end
        end
    end

    // ============================================================ 状态与扫描寄存器
    reg  [2:0]  state;
    reg         canceled, err_hold;
    reg  [3:0]  init_cnt;
    reg         scan_active;
    reg  [9:0]  i_cur, i_end, j_cur, j_end;
    reg  [13:0] row_base;
    reg  [3:0]  jms;                    // 条带内行号（pix = row_base + i）
    reg signed [47:0] acc0, acc1, acc2, acc3, acc4, acc5;
    reg signed [47:0] ar0, ar1, ar2, ar3, ar4, ar5;
    reg signed [31:0] e_row0, e_row1, e_row2; // 行首边缘值
    reg signed [31:0] e_cur0, e_cur1, e_cur2; // 当前像素边缘值
    reg         bias_pos0, bias_pos1, bias_pos2;
    reg  [9:0]  i0v, j0v;
    reg signed [16:0] dx0v, dy0v;
    reg  [3:0]  in_flight;

    // 记录量化/调制的逐像素标志（fire0 锁进流水）
    wire tex_en_f = w_flags[0];
    wire vc_en_f  = w_flags[1];
    wire key_en_f = w_flags[2];

    // ============================================================ 量化（发射级组合）
    // q = (acc + 32768) >>> 16 = acc[47:16] + acc[15]（acc < 2^41，无溢出）
    function signed [31:0] q16;
        input [47:0] a;
        begin
`ifdef INJ_RAST_ROUND
            q16 = a[47:16];
`else
            q16 = a[47:16] + {31'd0, a[15]};
`endif
        end
    endfunction
    function [15:0] clamp_z;
        input signed [31:0] q;
        begin
            clamp_z = (q < 0) ? 16'd0 : (q > 32'sd65535) ? 16'hFFFF : q[15:0];
        end
    endfunction
`ifdef GFX_HAS_RAST_TEX
    function [12:0] clamp_uv;
        input signed [31:0] q;
        input [12:0] lim;
        begin
            clamp_uv = (q < 0) ? 13'd0 : (q > $signed({19'd0, lim})) ? lim : q[12:0];
        end
    endfunction
    wire [12:0] uw_lim = tex_width - 13'd1;
    wire [12:0] uh_lim = tex_height - 13'd1;
    // 配置检查：(height-1)*stride + width ≤ 4096（atlas 不回绕）
    wire tex_cfg_bad = w_flags[0] &&
        (tex_width == 0 || tex_height == 0 || tex_width > 4096 ||
         tex_height > 4096 || tex_slog2 > 12 ||
         {12'd0, tex_width} > (25'd1 << tex_slog2) ||
         (({12'd0, tex_height} - 25'd1) << tex_slog2) +
           {12'd0, tex_width} > 25'd4096);
`endif
    function [7:0] clamp_255;
        input signed [31:0] q;
        begin
            clamp_255 = (q < 0) ? 8'd0 : (q > 32'sd255) ? 8'd255 : q[7:0];
        end
    endfunction
    wire signed [31:0] qz = q16(acc0);
    wire signed [31:0] qu = q16(acc1);
    wire signed [31:0] qv = q16(acc2);
    wire signed [31:0] qr = q16(acc3);
    wire signed [31:0] qg = q16(acc4);
    wire signed [31:0] qb = q16(acc5);
    wire [15:0] qz_c = clamp_z(qz);
`ifdef GFX_HAS_RAST_TEX
    wire [12:0] qu_c = clamp_uv(qu, uw_lim);
    wire [12:0] qv_c = clamp_uv(qv, uh_lim);
`endif
    wire [7:0] qr_c = clamp_255(qr);
    wire [7:0] qg_c = clamp_255(qg);
    wire [7:0] qb_c = clamp_255(qb);
    wire [13:0] pix_now = row_base + {4'd0, i_cur};
    wire cov_now = (bias_pos0 ? (e_cur0 >= 32'sd0) : (e_cur0 > 32'sd0)) &&
                   (bias_pos1 ? (e_cur1 >= 32'sd0) : (e_cur1 > 32'sd0)) &&
                   (bias_pos2 ? (e_cur2 >= 32'sd0) : (e_cur2 > 32'sd0));

    // ============================================================ 位置求值 DSP
    reg  signed [18:0] dsp_a [0:3];
    reg  signed [17:0] dsp_b [0:3];
    wire signed [36:0] dsp_p [0:3];
    genvar gd;
    generate
        for (gd = 0; gd < 4; gd = gd + 1) begin : g_dsp
            assign dsp_p[gd] = dsp_a[gd] * dsp_b[gd];
        end
    endgenerate
    // 发射乘积目的地：边 6（EN/ED×3）+ 属性 24（高/低×x/y×6）
    reg signed [25:0] ep_en [0:2];
    reg signed [25:0] ep_ed [0:2];
    reg signed [29:0] pxh [0:5];       // X 方向高位部分积 g[30:16]*dx
    reg signed [30:0] pxl [0:5];       // X 方向低位部分积 g[15:0]*dx
    reg signed [29:0] pyh [0:5];       // Y 方向高位部分积
    reg signed [30:0] pyl [0:5];       // Y 方向低位部分积
    reg signed [47:0] c_a1 [0:5];
    reg signed [47:0] c_a2 [0:5];
    reg signed [26:0] c_es [0:2];
    reg signed [47:0] c_sum [0:5];
    reg signed [31:0] c_er [0:2];
    reg  [9:0]  i1v, j1v;
    reg  [3:0]  jmsv;

    // 发射操作数（组合，按 init_cnt 选）——见 INIT 时序块的 case

    // 固定延迟读响应先落 FIFO，再进入可停顿的调制/提交级。
    // in_flight 覆盖从发射到退休的全部存储，预留 8 个槽，返回端永不反压。
    localparam integer RL = TEX_LATENCY;
    reg [RL-1:0] mv;
    reg [13:0] mp [0:RL-1];
    reg [15:0] mz [0:RL-1];
    reg [23:0] mc [0:RL-1];
    reg [RL-1:0] mcover, mtex, mkey;
    reg [15:0] z_align;
    wire [15:0] z_response = (RL == 1) ? z_rdq[mp[0][1:0]] : z_align;
    wire [87:0] response [0:7];
    reg [3:0] resp_wp, resp_rp;
    wire [87:0] head = response[resp_rp[2:0]];
    reg mul_v, mul_wr, mul_tex;
    reg [13:0] mul_pix;
    reg [15:0] mul_z;
    reg [23:0] mul_rgb;
`ifdef GFX_HAS_RAST_TEX
    reg [15:0] mul_r, mul_g, mul_b;
`endif
    reg out_v, out_wr;
    reg [13:0] out_pix;
    reg [15:0] out_z;
    reg [23:0] out_rgb;
    wire stopping = canceled || cancel || err_hold;
    wire retire = out_v && (!out_wr || ras_wr_ready || stopping);
    wire out_ready = !out_v || retire;
    wire mul_ready = !mul_v || out_ready;
    wire resp_pop = resp_wp != resp_rp && mul_ready;
    wire resp_push = mv[RL-1];
`ifdef GFX_HAS_RAST_TEX
    wire tex_req_now = cov_now && tex_en_f;
    wire tex_ok = !tex_req_now || samp_ready;
    wire [31:0] response_texel = samp_rdata;
`ifdef INJ_RAST_KEY_MISS
    wire response_key = 1'b0;
`else
    wire response_key = mtex[RL-1] && mkey[RL-1] && response_texel == ras_key;
`endif
    wire tex_cfg_bad_c = tex_cfg_bad;
    assign samp_valid = scan_active && !stopping &&
        (in_flight < 4'd8 || retire) && tex_req_now;
    wire [12:0] samp_address = (qv_c << tex_slog2) + qu_c;
    assign samp_word = samp_address[11:0]; // 配置检查保证地址 <4096。
`else
    wire tex_req_now = 1'b0;
    wire tex_ok = 1'b1;
    wire [31:0] response_texel = 32'd0;
    wire response_key = 1'b0;
    wire tex_cfg_bad_c = 1'b0;
`endif
    wire fire0 = scan_active && !stopping && !clr_busy && tex_ok &&
        (in_flight < 4'd8 || retire);
`ifdef INJ_RAST_Z_REVERSE
    wire response_write = mcover[RL-1] && mz[RL-1] > z_response && !response_key;
`else
    wire response_write = mcover[RL-1] && mz[RL-1] < z_response && !response_key;
`endif
    // 浅返回队列用寄存器。按项写使能避免 8×88 被映成数块 RAM。
    genvar response_slot;
    generate for(response_slot=0;response_slot<8;response_slot=response_slot+1) begin : g_response
        reg [87:0] payload;
        always @(posedge clk) if(resp_push && resp_wp[2:0]==response_slot)
            payload <= {mp[RL-1],mz[RL-1],mc[RL-1],response_texel,response_write,mtex[RL-1]};
        assign response[response_slot]=payload;
    end endgenerate
    wire commit_fire = out_v && out_wr && ras_wr_ready && !stopping;
    assign ras_wr_valid = out_v && out_wr && !stopping;
    assign ras_wr_pix = out_pix;
    assign ras_wr_data = {8'd0, out_rgb};
`ifdef INJ_RAST_PARTIAL_COMMIT
    assign zwr_en = out_v && out_wr && !stopping;
`else
    assign zwr_en = commit_fire;
`endif
    assign zwr_pix = out_pix;
    assign zwr_data = out_z;
    assign zrd_en = fire0 && cov_now;
    assign zrd_pix = pix_now;

    // x = texel*c+127，精确实现 floor(x/255)，无除法器。
    function [7:0] div255;
        input [15:0] product;
        reg [16:0] x;
        begin
            x = {1'b0, product} + 17'd127;
            x = x + 17'd1 + (x >> 8);
            div255 = x[15:8]; // 最大 65407，最高位恒 0。
        end
    endfunction
    integer mi;
    always @(posedge clk) begin
        if (rst) begin
            mv <= 0;
            resp_wp <= 0; resp_rp <= 0;
            mul_v <= 0; out_v <= 0; in_flight <= 0;
        end else begin
            in_flight <= in_flight + (fire0 ? 4'd1 : 4'd0) - (retire ? 4'd1 : 4'd0);
            mv[0] <= fire0;
            mp[0] <= pix_now; mz[0] <= qz_c;
            mc[0] <= vc_en_f ? {qr_c, qg_c, qb_c} : 24'hffffff;
            mcover[0] <= cov_now; mtex[0] <= tex_req_now; mkey[0] <= key_en_f;
            z_align <= z_rdq[mp[0][1:0]];
            for (mi = 1; mi < RL; mi = mi + 1) begin
                mv[mi] <= mv[mi-1]; mp[mi] <= mp[mi-1]; mz[mi] <= mz[mi-1];
                mc[mi] <= mc[mi-1]; mcover[mi] <= mcover[mi-1];
                mtex[mi] <= mtex[mi-1]; mkey[mi] <= mkey[mi-1];
            end
            if (resp_push) begin
                resp_wp <= resp_wp + 4'd1;
            end
            if (mul_ready) begin
                mul_v <= resp_pop;
                if (resp_pop) begin
                    resp_rp <= resp_rp + 4'd1;
                    {mul_pix, mul_z, mul_rgb} <= head[87:34];
                    mul_wr <= head[1]; mul_tex <= head[0];
`ifdef GFX_HAS_RAST_TEX
                    mul_r <= head[57:50] * head[25:18];
                    mul_g <= head[49:42] * head[17:10];
                    mul_b <= head[41:34] * head[9:2];
`endif
                end
            end
            if (out_ready) begin
                out_v <= mul_v; out_pix <= mul_pix; out_z <= mul_z;
                out_wr <= mul_wr;
`ifdef GFX_HAS_RAST_TEX
                out_rgb <= mul_tex ? {div255(mul_r), div255(mul_g), div255(mul_b)} : mul_rgb;
`else
                out_rgb <= mul_rgb;
`endif
            end
        end
    end

    wire signed [17:0] dx0s = {dx0v[16], dx0v};
    wire signed [17:0] dy0s = {dy0v[16], dy0v};
    wire [15:0] j0_low = {7'd0, act_strip, 4'd0};
    wire [15:0] j1_high = j0_low + 16'd15;
    wire signed [47:0] attr_gx [0:5];
    wire signed [47:0] attr_gy [0:5];
    assign attr_gx[0] = {{16{w_gzx[31]}}, w_gzx};
    assign attr_gy[0] = {{16{w_gzy[31]}}, w_gzy};
    assign attr_gx[1] = {{16{w_gux[31]}}, w_gux};
    assign attr_gy[1] = {{16{w_guy[31]}}, w_guy};
    assign attr_gx[2] = {{16{w_gvx[31]}}, w_gvx};
    assign attr_gy[2] = {{16{w_gvy[31]}}, w_gvy};
    assign attr_gx[3] = {{16{w_grx[31]}}, w_grx};
    assign attr_gy[3] = {{16{w_gry[31]}}, w_gry};
    assign attr_gx[4] = {{16{w_ggx[31]}}, w_ggx};
    assign attr_gy[4] = {{16{w_ggy[31]}}, w_ggy};
    assign attr_gx[5] = {{16{w_gbx[31]}}, w_gbx};
    assign attr_gy[5] = {{16{w_gby[31]}}, w_gby};

    // ============================================================ 主时序：DSP/FSM
    always @(posedge clk) begin
        if (rst) begin
            state <= M_IDLE;
            canceled <= 1'b0;
            err_hold <= 1'b0;
            scan_active <= 1'b0;
            slot_busy0 <= 1'b0;
            slot_busy1 <= 1'b0;
            err_valid <= 1'b0;
            init_cnt <= 4'd0;
            dbg_retire_cnt <= 32'd0;
        end else begin
            err_valid <= 1'b0;
            if (cancel && !canceled) canceled <= 1'b1;
            if (canceled && !cancel && state == M_IDLE && !slot_busy0 && !slot_busy1) begin
                canceled <= 1'b0; err_hold <= 1'b0;
            end

            // ---- 参数槽登记 ----
            if (rp_valid) begin
                if (rp_slot_v[0] == 1'b0) begin
                    slot_busy0 <= 1'b1; busy_idx0 <= rp_index; busy_strip0 <= rp_strip; busy_ord0 <= rp_ordinal;
                end else begin
                    slot_busy1 <= 1'b1; busy_idx1 <= rp_index; busy_strip1 <= rp_strip; busy_ord1 <= rp_ordinal;
                end
            end

            case (state)
                M_IDLE: begin
                    if (canceled || cancel || err_hold) begin
                        // 取消：丢掉已交付未启动的记录
                        slot_busy0 <= 1'b0;
                        slot_busy1 <= 1'b0;
                    end else if (slot_busy0 || slot_busy1) begin
                        act_sel <= slot_busy0 ? 1'b0 : 1'b1;
                        state <= M_VALID;
                    end
                end

                M_VALID: begin
`ifdef INJ_RAST_BIAS_REVERSE
                    bias_pos0 <= !((w_a0 > 32'sd0) || (w_a0 == 32'sd0 && w_b0 > 32'sd0));
`else
                    bias_pos0 <= (w_a0 > 32'sd0) || (w_a0 == 32'sd0 && w_b0 > 32'sd0);
`endif
                    bias_pos1 <= (w_a1 > 32'sd0) || (w_a1 == 32'sd0 && w_b1 > 32'sd0);
                    bias_pos2 <= (w_a2 > 32'sd0) || (w_a2 == 32'sd0 && w_b2 > 32'sd0);
                    if (record_bad || tex_cfg_bad_c) begin
                        err_valid  <= 1'b1;
                        err_code   <= EC_BAD_FIELD;
                        err_index  <= act_index;
                        err_tag    <= {11'd0, act_strip, act_ordinal};
                        err_hold   <= 1'b1;
                        if (act_sel) slot_busy1 <= 1'b0; else slot_busy0 <= 1'b0;
                        state <= M_IDLE;
                    end else if (w_y1 < $signed(j0_low) || w_y0 > $signed(j1_high)) begin
                        dbg_retire_cnt <= dbg_retire_cnt + 32'd1;
                        if (act_sel) slot_busy1 <= 1'b0; else slot_busy0 <= 1'b0;
                        state <= M_IDLE;
                    end else begin
                        i0v  <= w_x0[9:0];
                        i1v  <= w_x1[9:0];
                        j0v  <= (w_y0 > j0_low) ? w_y0[9:0] : j0_low[9:0];
                        j1v  <= (w_y1 < j1_high) ? w_y1[9:0] : j1_high[9:0];
                        jmsv <= (w_y0 > j0_low) ? w_y0[3:0] : 4'd0;
                        dx0v <= (w_x0 <<< 4) - w_ox;
                        dy0v <= (((w_y0 > j0_low) ? w_y0 : j0_low) <<< 4) - w_oy;
                        init_cnt <= 4'd0;
                        state <= M_INIT;
                    end
                end

                M_INIT: begin
                    // 发射 8 拍 × 4 DSP；随后三级归并
                    if (init_cnt < 8) begin
                        case (init_cnt)
                            4'd0: begin
                                dsp_a[0] <= {{2{w_a0[20]}}, w_a0[20:4]};
                                dsp_b[0] <= {8'd0, i0v};
                                dsp_a[1] <= {{2{w_b0[20]}}, w_b0[20:4]};
                                dsp_b[1] <= {8'd0, j0v};
                                dsp_a[2] <= {{2{w_a1[20]}}, w_a1[20:4]};
                                dsp_b[2] <= {8'd0, i0v};
                                dsp_a[3] <= {{2{w_b1[20]}}, w_b1[20:4]};
                                dsp_b[3] <= {8'd0, j0v};
                            end
                            4'd1: begin
                                dsp_a[0] <= {{2{w_a2[20]}}, w_a2[20:4]};
                                dsp_b[0] <= {8'd0, i0v};
                                dsp_a[1] <= {{2{w_b2[20]}}, w_b2[20:4]};
                                dsp_b[1] <= {8'd0, j0v};
                                dsp_a[2] <= {{4{w_gzx[30]}}, w_gzx[30:16]};
                                dsp_b[2] <= dx0s;
                                dsp_a[3] <= {3'd0, w_gzx[15:0]};
                                dsp_b[3] <= dx0s;
                            end
                            4'd2: begin
                                dsp_a[0] <= {{4{w_gzy[30]}}, w_gzy[30:16]};
                                dsp_b[0] <= dy0s;
                                dsp_a[1] <= {3'd0, w_gzy[15:0]};
                                dsp_b[1] <= dy0s;
                                dsp_a[2] <= {{4{w_gux[30]}}, w_gux[30:16]};
                                dsp_b[2] <= dx0s;
                                dsp_a[3] <= {3'd0, w_gux[15:0]};
                                dsp_b[3] <= dx0s;
                            end
                            4'd3: begin
                                dsp_a[0] <= {{4{w_guy[30]}}, w_guy[30:16]};
                                dsp_b[0] <= dy0s;
                                dsp_a[1] <= {3'd0, w_guy[15:0]};
                                dsp_b[1] <= dy0s;
                                dsp_a[2] <= {{4{w_gvx[30]}}, w_gvx[30:16]};
                                dsp_b[2] <= dx0s;
                                dsp_a[3] <= {3'd0, w_gvx[15:0]};
                                dsp_b[3] <= dx0s;
                            end
                            4'd4: begin
                                dsp_a[0] <= {{4{w_gvy[30]}}, w_gvy[30:16]};
                                dsp_b[0] <= dy0s;
                                dsp_a[1] <= {3'd0, w_gvy[15:0]};
                                dsp_b[1] <= dy0s;
                                dsp_a[2] <= {{4{w_grx[30]}}, w_grx[30:16]};
                                dsp_b[2] <= dx0s;
                                dsp_a[3] <= {3'd0, w_grx[15:0]};
                                dsp_b[3] <= dx0s;
                            end
                            4'd5: begin
                                dsp_a[0] <= {{4{w_gry[30]}}, w_gry[30:16]};
                                dsp_b[0] <= dy0s;
                                dsp_a[1] <= {3'd0, w_gry[15:0]};
                                dsp_b[1] <= dy0s;
                                dsp_a[2] <= {{4{w_ggx[30]}}, w_ggx[30:16]};
                                dsp_b[2] <= dx0s;
                                dsp_a[3] <= {3'd0, w_ggx[15:0]};
                                dsp_b[3] <= dx0s;
                            end
                            4'd6: begin
                                dsp_a[0] <= {{4{w_ggy[30]}}, w_ggy[30:16]};
                                dsp_b[0] <= dy0s;
                                dsp_a[1] <= {3'd0, w_ggy[15:0]};
                                dsp_b[1] <= dy0s;
                                dsp_a[2] <= {{4{w_gbx[30]}}, w_gbx[30:16]};
                                dsp_b[2] <= dx0s;
                                dsp_a[3] <= {3'd0, w_gbx[15:0]};
                                dsp_b[3] <= dx0s;
                            end
                            default: begin // 4'd7
                                dsp_a[0] <= {{4{w_gby[30]}}, w_gby[30:16]};
                                dsp_b[0] <= dy0s;
                                dsp_a[1] <= {3'd0, w_gby[15:0]};
                                dsp_b[1] <= dy0s;
                            end
                        endcase
                    end
                    // 捕获（发射次拍落位）
                    case (init_cnt)
                        4'd1: begin ep_en[0] <= dsp_p[0][25:0]; ep_ed[0] <= dsp_p[1][25:0];
                                    ep_en[1] <= dsp_p[2][25:0]; ep_ed[1] <= dsp_p[3][25:0]; end
                        4'd2: begin ep_en[2] <= dsp_p[0][25:0]; ep_ed[2] <= dsp_p[1][25:0];
                                    pxh[0] <= dsp_p[2][29:0]; pxl[0] <= dsp_p[3][30:0]; end
                        4'd3: begin pyh[0] <= dsp_p[0][29:0]; pyl[0] <= dsp_p[1][30:0];
                                    pxh[1] <= dsp_p[2][29:0]; pxl[1] <= dsp_p[3][30:0]; end
                        4'd4: begin pyh[1] <= dsp_p[0][29:0]; pyl[1] <= dsp_p[1][30:0];
                                    pxh[2] <= dsp_p[2][29:0]; pxl[2] <= dsp_p[3][30:0]; end
                        4'd5: begin pyh[2] <= dsp_p[0][29:0]; pyl[2] <= dsp_p[1][30:0];
                                    pxh[3] <= dsp_p[2][29:0]; pxl[3] <= dsp_p[3][30:0]; end
                        4'd6: begin pyh[3] <= dsp_p[0][29:0]; pyl[3] <= dsp_p[1][30:0];
                                    pxh[4] <= dsp_p[2][29:0]; pxl[4] <= dsp_p[3][30:0]; end
                        4'd7: begin pyh[4] <= dsp_p[0][29:0]; pyl[4] <= dsp_p[1][30:0];
                                    pxh[5] <= dsp_p[2][29:0]; pxl[5] <= dsp_p[3][30:0]; end
                        4'd8: begin pyh[5] <= dsp_p[0][29:0]; pyl[5] <= dsp_p[1][30:0]; end
                        4'd9: begin
                            c_es[0] <= ep_en[0] + ep_ed[0];
                            c_es[1] <= ep_en[1] + ep_ed[1];
                            c_es[2] <= ep_en[2] + ep_ed[2];
                            c_a1[0] <= (pxh[0] <<< 16) + pxl[0];
                            c_a1[1] <= (pxh[1] <<< 16) + pxl[1];
                            c_a1[2] <= (pxh[2] <<< 16) + pxl[2];
                            c_a1[3] <= (pxh[3] <<< 16) + pxl[3];
                            c_a1[4] <= (pxh[4] <<< 16) + pxl[4];
                            c_a1[5] <= (pxh[5] <<< 16) + pxl[5];
                            c_a2[0] <= (pyh[0] <<< 16) + pyl[0];
                            c_a2[1] <= (pyh[1] <<< 16) + pyl[1];
                            c_a2[2] <= (pyh[2] <<< 16) + pyl[2];
                            c_a2[3] <= (pyh[3] <<< 16) + pyl[3];
                            c_a2[4] <= (pyh[4] <<< 16) + pyl[4];
                            c_a2[5] <= (pyh[5] <<< 16) + pyl[5];
                        end
                        4'd10: begin
                            c_sum[0] <= c_a1[0] + c_a2[0] + 48'sd8;
                            c_sum[1] <= c_a1[1] + c_a2[1] + 48'sd8;
                            c_sum[2] <= c_a1[2] + c_a2[2] + 48'sd8;
                            c_sum[3] <= c_a1[3] + c_a2[3] + 48'sd8;
                            c_sum[4] <= c_a1[4] + c_a2[4] + 48'sd8;
                            c_sum[5] <= c_a1[5] + c_a2[5] + 48'sd8;
`ifdef INJ_RAST_EDGE_SCALE
                            c_er[0] <= w_c0 + c_es[0];
`else
                            c_er[0] <= w_c0 + (c_es[0] <<< 4);
`endif
                            c_er[1] <= w_c1 + (c_es[1] <<< 4);
                            c_er[2] <= w_c2 + (c_es[2] <<< 4);
                        end
                        4'd11: begin
                            acc0 <= $signed({16'd0, w_z0, 16'd0}) + (c_sum[0] >>> 4);
                            ar0 <= $signed({16'd0, w_z0, 16'd0}) + (c_sum[0] >>> 4);
                            acc1 <= $signed({20'd0, w_u0, 12'd0}) + (c_sum[1] >>> 4);
                            ar1 <= $signed({20'd0, w_u0, 12'd0}) + (c_sum[1] >>> 4);
                            acc2 <= $signed({20'd0, w_v0, 12'd0}) + (c_sum[2] >>> 4);
                            ar2 <= $signed({20'd0, w_v0, 12'd0}) + (c_sum[2] >>> 4);
                            acc3 <= $signed({24'd0, w_col0[23:16], 16'd0}) + (c_sum[3] >>> 4);
                            ar3 <= $signed({24'd0, w_col0[23:16], 16'd0}) + (c_sum[3] >>> 4);
                            acc4 <= $signed({24'd0, w_col0[15:8],  16'd0}) + (c_sum[4] >>> 4);
                            ar4 <= $signed({24'd0, w_col0[15:8],  16'd0}) + (c_sum[4] >>> 4);
                            acc5 <= $signed({24'd0, w_col0[7:0],   16'd0}) + (c_sum[5] >>> 4);
                            ar5 <= $signed({24'd0, w_col0[7:0],   16'd0}) + (c_sum[5] >>> 4);
                            e_row0 <= c_er[0]; e_cur0 <= c_er[0];
                            e_row1 <= c_er[1]; e_cur1 <= c_er[1];
                            e_row2 <= c_er[2]; e_cur2 <= c_er[2];
                            i_cur <= i0v; j_cur <= j0v; i_end <= i1v; j_end <= j1v;
                            row_base <= {10'd0, jmsv} * 14'd640;
                            jms <= jmsv;
                            scan_active <= !stopping;
                            state <= M_SCAN;
                        end
                        default: ;
                    endcase
                    if (init_cnt < 4'd11) init_cnt <= init_cnt + 4'd1;
                end

                M_SCAN: begin
                    if (stopping) scan_active <= 1'b0;
                    // 行进与属性累加（fire0 驱动）
                    if (fire0) begin
                        if (i_cur == i_end) begin
                            i_cur <= i0v;
                            j_cur <= j_cur + 10'd1;
                            jms <= jms + 4'd1;
                            row_base <= row_base + 14'd640;
                            e_row0 <= e_row0 + w_b0; e_cur0 <= e_row0 + w_b0;
                            e_row1 <= e_row1 + w_b1; e_cur1 <= e_row1 + w_b1;
                            e_row2 <= e_row2 + w_b2; e_cur2 <= e_row2 + w_b2;
                            acc0 <= ar0 + attr_gy[0]; ar0 <= ar0 + attr_gy[0]; acc1 <= ar1 + attr_gy[1]; ar1 <= ar1 + attr_gy[1];
                            acc2 <= ar2 + attr_gy[2]; ar2 <= ar2 + attr_gy[2]; acc3 <= ar3 + attr_gy[3]; ar3 <= ar3 + attr_gy[3];
                            acc4 <= ar4 + attr_gy[4]; ar4 <= ar4 + attr_gy[4]; acc5 <= ar5 + attr_gy[5]; ar5 <= ar5 + attr_gy[5];
                            if (j_cur == j_end) scan_active <= 1'b0;
                        end else begin
                            i_cur <= i_cur + 10'd1;
                            e_cur0 <= e_cur0 + w_a0;
                            e_cur1 <= e_cur1 + w_a1;
                            e_cur2 <= e_cur2 + w_a2;
                            acc0 <= acc0 + attr_gx[0]; acc1 <= acc1 + attr_gx[1];
                            acc2 <= acc2 + attr_gx[2]; acc3 <= acc3 + attr_gx[3];
                            acc4 <= acc4 + attr_gx[4]; acc5 <= acc5 + attr_gx[5];
                        end
                    end
`ifdef INJ_RAST_NO_BARRIER
                    if (!scan_active || stopping) begin
`else
                    if ((!scan_active || stopping) && in_flight == 4'd0) begin
`endif
                        dbg_retire_cnt <= dbg_retire_cnt + 32'd1;
                        if (act_sel) slot_busy1 <= 1'b0; else slot_busy0 <= 1'b0;
                        state <= M_IDLE;
                    end
                end
                default: state <= M_IDLE;
            endcase
        end
    end

endmodule
`default_nettype wire
