`timescale 1ns/1ps
//==============================================================================================//
// FILE: engine_geom.v
//
// DESCRIPTION: 几何层：接一条通过十步检查的 FILL/BLIT，五者求交得实际矩形，逐行
//   拆成读源/读目的/写目的描述符与每行边带。只碰坐标与地址，不碰像素。
//
// NOTE:
//   1. 求交中间量 18 位有符号（最坏值域 131069 < 2^17+4，17 位不够）；空宽高先判；
//      零面积命令回一拍 cmd_done；地址通路保持 32 位并在锁存处显式扩展
//   2. 每行发边带前等 row_free 对应半区空着，含每条命令的第 0 行
//   3. wr_last_row 只打在最后一行的最后一段，跨 4KB 拆段时首段不打
//   4. 发出顺序 = 边带 → 读源 → 读目的 → 写目的，测试台按同序期望（geom_model.py）
//==============================================================================================//
`default_nettype none
`include "engine_feature.vh"

module engine_geom #(
    parameter integer STRIP_H = 16,
    // 拍数上限，与搬运层两个核的 AXI_MAX_BURST_LEN 同一个值
    parameter integer MAX_BEATS = 192
)(
    input  wire        clk,
    input  wire        rst,

    // 命令口：控制层 → 几何层
    input  wire        start,        // 一拍脉冲：提交一条已经通过十步检查的 FILL/BLIT
    input  wire [7:0]  op,           // 1 FILL、2 BLIT（其余不到本层）
    input  wire [3:0]  eq,           // 0 COPY、1 ALPHA、2 ADD、3 CONST、4 MUL、5 ALPHA_STRAIGHT
    input  wire        key_en,
    input  wire [31:0] key_w5,       // 色键字
    input  wire [31:0] const_w4,     // FILL 的常量源字
    input  wire [7:0]  c,            // CONST 方程用的 c
    input  wire [3:0]  sfmt,         // 源格式（FILL 由方程决定，BLIT 取源表面）
    input  wire [31:0] src_base,
    input  wire [15:0] src_w,
    input  wire [15:0] src_h,
    input  wire [4:0]  src_slog2,
    input  wire        src_tex,       // 源在片上 TEX（仅 BLIT 源会置 1）
    input  wire        band_enable,   // 目的片上化，按当前条带求交
    input  wire [15:0] band_y0,
    input  wire [31:0] dst_base,
    input  wire [15:0] dst_w,
    input  wire [15:0] dst_h,
    input  wire [4:0]  dst_slog2,
    input  wire signed [15:0] src_x,
    input  wire signed [15:0] src_y,
    input  wire signed [15:0] dst_x,
    input  wire signed [15:0] dst_y,
    input  wire [15:0] w,
    input  wire [15:0] h,
    input  wire signed [15:0] clip_x0,
    input  wire signed [15:0] clip_y0,
    input  wire signed [15:0] clip_x1,
    input  wire signed [15:0] clip_y1,
    input  wire [3:0]  cidx,         // 命令索引，错误归属
    input  wire [1:0]  row_free,     // 行缓冲空闲握手，电平，每一半一位

    output reg         busy,
    output reg         cmd_done,     // 零面积命令的完成脉冲
    output reg  [3:0]  done_cidx,

    // 读描述符
    output reg  [31:0] rd_addr,
    output reg  [11:0] rd_beats,
    output reg  [1:0]  rd_buf,
    output reg         rd_tex,        // 源读描述符的片上标记：仅 BLIT 源可能置 1
    output reg         rd_band,
    output reg  [3:0]  rd_cidx,
    output reg         rd_valid,
    input  wire        rd_ready,

    // 写描述符
    output reg  [31:0] wr_addr,
    output reg  [11:0] wr_beats,
    output reg  [3:0]  wr_cidx,
    output reg         wr_last_row,
    output reg         wr_band,
    output reg         wr_valid,
    input  wire        wr_ready,

    // 每行边带
    output reg  [15:0] sb_row,       // 本命令内的零基行号
    output reg  [15:0] sb_py,        // 目的面上的绝对 y
    output reg  [10:0] sb_npx,       // 本行像素数
    output reg  [7:0]  sb_beats,     // 本行写拍数（两段之和）
    output reg  [1:0]  sb_lead_dst,  // 目的行首地址的拍内像素相位
    output reg  [1:0]  sb_lead_src,  // 源行首地址的拍内像素相位
    output reg  [7:0]  sb_op,
    output reg  [3:0]  sb_eq,
    output reg         sb_key_en,
    output reg  [31:0] sb_key,
    output reg  [31:0] sb_w4,
    output reg  [7:0]  sb_c,
    output reg  [3:0]  sb_sfmt,
    output reg         sb_read_dst,  // 这一行读不读目的
    output reg         sb_valid,
    input  wire        sb_ready
);
    localparam [7:0] OP_FILL = 8'd1;
    localparam [7:0] OP_BLIT = 8'd2;
    localparam [3:0] EQ_COPY = 4'd0;
    localparam [3:0] EQ_ALPHA = 4'd1;
    // 条带高的 18 位形态：integer 参数会把表达式撑到 32 位，触发截断告警
    localparam signed [17:0] BAND_H18 = STRIP_H;

    localparam [2:0] S_IDLE = 3'd0, S_ROW = 3'd1, S_ADV = 3'd2, S_DONE = 3'd3,
                     S_GATE = 3'd4;

    reg [2:0]  st;
    reg [2:0]  seg_i;                    // 0 边带、1/2 读源、3/4 读目的、5/6 写目的
    reg [15:0] row_i;
    reg [31:0] dst_cur, src_cur;         // 本行的行首字节地址
    reg [31:0] nb;                       // 本行字节数 = npx*4
    reg [31:0] dst_stride, src_stride;   // 1 << stride_log2
    reg [15:0] nrows;
    reg [10:0] npx;
    reg signed [15:0] py;

    // 命令的锁存副本
    reg [7:0]  r_op;
    reg [3:0]  r_eq;
    reg        r_key_en;
    reg [31:0] r_key, r_w4;
    reg [7:0]  r_c;
    reg [3:0]  r_sfmt;
    reg        r_src_tex;
    reg        r_dst_band;
    reg [3:0]  r_cidx;

    // 五者求交：纯组合，start 那一拍算一次，与全部命令字段一起锁存，命令期间不再看输入。
    // 中间量 18 位有符号：最坏值域 s_hi_x = 32767−(−32768) + 65535−1 = 131069 < 2^17
    //（17 位不够），空判定与各钳制在 18 位内逐值等价；地址通路（dst_cur/src_cur）
    // 保持 32 位，锁存处显式符号扩展。
    wire signed [17:0] p_dx = $signed({{2{dst_x[15]}}, dst_x});
    wire signed [17:0] p_dy = $signed({{2{dst_y[15]}}, dst_y});
    wire signed [17:0] p_sx = $signed({{2{src_x[15]}}, src_x});
    wire signed [17:0] p_sy = $signed({{2{src_y[15]}}, src_y});
    wire        [17:0] p_w  = {2'b00, w};
    wire        [17:0] p_h  = {2'b00, h};
    wire        [17:0] p_ssw = {2'b00, src_w};
    wire        [17:0] p_ssh = {2'b00, src_h};
    wire        [17:0] p_dsw = {2'b00, dst_w};
    wire        [17:0] p_dsh = {2'b00, dst_h};

    reg signed [17:0] t_x0, t_y0, t_x1, t_y1;
    reg signed [17:0] dx1, dy1;
    reg signed [17:0] s_hi_x, s_hi_y;
    reg               i_empty;

    always @* begin
        // 先给全部中间量与输出默认值，少给一条路径就会推断出锁存器
        dx1 = 18'sd0; dy1 = 18'sd0;
        t_x0 = 18'sd0; t_y0 = 18'sd0; t_x1 = -18'sd1; t_y1 = -18'sd1;
        s_hi_x = 18'sd0; s_hi_y = 18'sd0;
        i_empty = 1'b1;
        if ((p_w == 18'd0) || (p_h == 18'd0)) begin
            i_empty = 1'b1;
        end else begin
            dx1 = p_dx + $signed(p_w) - 18'sd1;
            dy1 = p_dy + $signed(p_h) - 18'sd1;

            t_x0 = (p_dx > 18'sd0) ? p_dx : 18'sd0;
            t_y0 = (p_dy > 18'sd0) ? p_dy : 18'sd0;
            // 拼接结果无符号，必须 $signed 恢复有符号比较（clip 可为负）
            if ($signed({{2{clip_x0[15]}}, clip_x0}) > t_x0) t_x0 = {{2{clip_x0[15]}}, clip_x0};
            if ($signed({{2{clip_y0[15]}}, clip_y0}) > t_y0) t_y0 = {{2{clip_y0[15]}}, clip_y0};

            t_x1 = (dx1 < 18'sd639) ? dx1 : 18'sd639;
            t_y1 = (dy1 < 18'sd479) ? dy1 : 18'sd479;
            if ($signed({{2{clip_x1[15]}}, clip_x1}) < t_x1) t_x1 = {{2{clip_x1[15]}}, clip_x1};
            if ($signed({{2{clip_y1[15]}}, clip_y1}) < t_y1) t_y1 = {{2{clip_y1[15]}}, clip_y1};
`ifdef INJ_BAND_CLIP
            // 注错：漏掉当前条带求交。
`else
            if(band_enable) begin
                if($signed({2'b00,band_y0})>t_y0) t_y0=$signed({2'b00,band_y0});
                if($signed({2'b00,band_y0})+BAND_H18-18'sd1<t_y1)
                    t_y1=$signed({2'b00,band_y0})+BAND_H18-18'sd1;
            end
`endif
            if ($signed(p_dsw) - 18'sd1 < t_x1) t_x1 = $signed(p_dsw) - 18'sd1;
            if ($signed(p_dsh) - 18'sd1 < t_y1) t_y1 = $signed(p_dsh) - 18'sd1;

            // 第五项：源表面
`ifdef INJ_SKIP_SURF
            // 注错：整块跳过第五项求交
`else
            if (op == OP_BLIT) begin
                if (p_dx - p_sx > t_x0) t_x0 = p_dx - p_sx;
                if (p_dy - p_sy > t_y0) t_y0 = p_dy - p_sy;
                s_hi_x = p_dx - p_sx + $signed(p_ssw) - 18'sd1;
                s_hi_y = p_dy - p_sy + $signed(p_ssh) - 18'sd1;
                if (s_hi_x < t_x1) t_x1 = s_hi_x;
                if (s_hi_y < t_y1) t_y1 = s_hi_y;
            end
`endif

            if ((t_x1 < t_x0) || (t_y1 < t_y0)) i_empty = 1'b1;
            else                                    i_empty = 1'b0;
        end
    end

    // 源的行首 x 偏移：sx0 = src_x + (x0 - dst_x)
    wire signed [17:0] sx_off = p_sx + (t_x0 - p_dx);
    // 地址通路保持 32 位（stride 移位可达 2^31 量级），符号扩展在此显式发生
    wire signed [31:0] sx_off32 = {{14{sx_off[17]}}, sx_off};
    wire signed [31:0] src_row_y = {{14{p_sy[17]}}, p_sy} +
        ({{14{t_y0[17]}}, t_y0} - {{14{p_dy[17]}}, p_dy});

    // 读写避让判定；声明放在用它的段计算之前（先用后声明可移植性没有保证）。
    wire d7_read_dst;
`ifdef INJ_ALPHA_AS_COPY
    assign d7_read_dst = ((r_eq == EQ_COPY) || (r_eq == EQ_ALPHA));  // 注错：ALPHA 也按 COPY
`else
    assign d7_read_dst = !(r_eq == EQ_COPY);
`endif
    // 本行发不发读目的描述符：注错版让 COPY 也发，而边带仍说不读，
    // 测试台的握手计数要抓住多出来的那几笔。
`ifdef INJ_COPY_READ
    wire seg_read_dst = 1'b1;             // 注错：COPY 也发读目的描述符
`else
    wire seg_read_dst = d7_read_dst;
`endif

    // 本行的段：从 dst_cur / src_cur / nb 算出本行最多六个候选段与一个边带槽
    reg [31:0] a0, a1, alen;
    reg [11:0] b0, b1;
    reg [31:0] s_a0, s_a1, s_len;
    reg [11:0] s_b0, s_b1;
    reg [12:0] page_left, page_left_s;
    reg [11:0] max_bytes;
    reg        xcross, sxcross;

    reg [31:0] seg_addr  [0:6];
    reg [11:0] seg_beats [0:6];
    reg        seg_iswr  [0:6];
    reg        seg_isrd  [0:6];
    reg        seg_dir   [0:6];      // 读：0 源、1 目的
    reg [6:0]  seg_pres;
    reg [2:0]  nxt_i;
    reg [11:0] wr_total;
    integer    k;

    always @* begin
        max_bytes = MAX_BEATS * 12'd16;      // 192 × 16 = 3072
        // 目的行的覆盖区间
        a0   = {dst_cur[31:4], 4'b0000};
        a1   = (dst_cur + nb + 32'd15) & 32'hFFFFFFF0;
        alen = a1 - a0;
        page_left = 13'h1000 - {1'b0, a0[11:0]};
`ifdef INJ_NO_SPLIT
        xcross = 1'b0;                       // 注错：跨 4KB 不拆
`else
        xcross = (({19'b0, a0[11:0]} + {19'b0, alen[11:0]}) > 32'h1000);
`endif
        if (xcross) begin
            b0 = (page_left > {1'b0, max_bytes}) ? max_bytes : page_left[11:0];
            b1 = alen[11:0] - b0;
        end else begin
            b0 = (alen > {20'b0, max_bytes}) ? max_bytes : alen[11:0];
            b1 = alen[11:0] - b0;
        end
        // 源行的覆盖区间（长度与目的相同，相位可能不同）
        s_a0  = {src_cur[31:4], 4'b0000};
        s_a1  = (src_cur + nb + 32'd15) & 32'hFFFFFFF0;
        s_len = s_a1 - s_a0;
        page_left_s = 13'h1000 - {1'b0, s_a0[11:0]};
`ifdef INJ_NO_SPLIT
        sxcross = 1'b0;                      // 注错：跨 4KB 不拆
`else
        sxcross = (({19'b0, s_a0[11:0]} + {19'b0, s_len[11:0]}) > 32'h1000);
`endif
        if (sxcross) begin
            s_b0 = (page_left_s > {1'b0, max_bytes}) ? max_bytes : page_left_s[11:0];
            s_b1 = s_len[11:0] - s_b0;
        end else begin
            s_b0 = (s_len > {20'b0, max_bytes}) ? max_bytes : s_len[11:0];
            s_b1 = s_len[11:0] - s_b0;
        end
        wr_total = (b0 >> 4) + (b1 >> 4);

        for (k = 0; k <= 6; k = k + 1) begin
            seg_addr[k]  = 32'd0;
            seg_beats[k] = 12'd0;
            seg_iswr[k]  = 1'b0;
            seg_isrd[k]  = 1'b0;
            seg_dir[k]   = 1'b0;
            seg_pres[k]  = 1'b0;
        end
        seg_pres[0] = 1'b1;                                  // 边带每行一条

        // 读源：只有 BLIT 有
        if (r_op == OP_BLIT) begin
            seg_addr[1] = s_a0; seg_beats[1] = s_b0 >> 4; seg_isrd[1] = (s_b0 != 0);
            seg_addr[2] = s_a0 + {{20{1'b0}}, s_b0}; seg_beats[2] = s_b1 >> 4;
            seg_isrd[2] = (s_b1 != 0);
        end
        // 读目的：只有方程不是 COPY 才有。注错位 2 让 ALPHA 也走 COPY 的避让，
        // 注错位 3 让 COPY 也发。
        if (seg_read_dst) begin
            seg_addr[3] = a0; seg_beats[3] = b0 >> 4; seg_isrd[3] = (b0 != 0);
            seg_dir[3]  = 1'b1;
            seg_addr[4] = a0 + {{20{1'b0}}, b0}; seg_beats[4] = b1 >> 4;
            seg_isrd[4] = (b1 != 0); seg_dir[4] = 1'b1;
        end
        // 写目的：每段一笔
        seg_addr[5] = a0; seg_beats[5] = b0 >> 4; seg_iswr[5] = (b0 != 0);
        seg_addr[6] = a0 + {{20{1'b0}}, b0}; seg_beats[6] = b1 >> 4; seg_iswr[6] = (b1 != 0);

        seg_pres[1] = seg_isrd[1]; seg_pres[2] = seg_isrd[2];
        seg_pres[3] = seg_isrd[3]; seg_pres[4] = seg_isrd[4];
        seg_pres[5] = seg_iswr[5]; seg_pres[6] = seg_iswr[6];
    end

    // 跳到 seg_i 之后第一个有货的槽（没有就是 7）
    always @* begin
        nxt_i = 3'd7;
        for (k = 0; k <= 6; k = k + 1)
            if ((k[2:0] > seg_i) && seg_pres[k] && (nxt_i == 3'd7))
                nxt_i = k[2:0];
    end

    // 发射
    wire cur_sb = (st == S_ROW) && (seg_i == 3'd0);
    wire cur_on = (st == S_ROW) && (seg_i != 3'd0) && (seg_i != 3'd7) && seg_pres[seg_i];
    wire cur_wr = cur_on && seg_iswr[seg_i];
    wire cur_rd = cur_on && seg_isrd[seg_i];
    wire last_wr_seg = (seg_i == 3'd6) || (seg_pres[6] == 1'b0);
    wire last_row_now = (row_i == (nrows - 16'd1));

    // 行缓冲那一半空没空
`ifdef INJ_ROW_NOFREE
    // 注错：不看 row_free，直接开下一行
    wire rf_first = 1'b1;                 // 命令的第 0 行
    wire rf_cur   = 1'b1;                 // S_GATE 里等的这一行
    wire rf_next  = 1'b1;                 // S_ADV 要推进到的下一行
`else
    wire rf_first = row_free[0];          // 命令的第 0 行
    wire rf_cur   = row_free[row_i[0]];   // S_GATE 里等的这一行
    wire rf_next  = row_free[~row_i[0]];  // S_ADV 要推进到的下一行
`endif

    always @* begin
        sb_valid      = cur_sb;
        // 边带的数据在 S_ROW 的整个期间都有效，接收方按 sb_valid 取
        sb_row        = row_i;
        sb_py         = py[15:0];
        sb_npx        = npx;
        sb_beats      = wr_total[7:0];
        sb_lead_dst   = dst_cur[3:2];
        // FILL 没有源行，这个字段无意义；固定发 0，独立模型也按 0 记（计算阵列不看它）
        sb_lead_src   = (r_op == OP_BLIT) ? src_cur[3:2] : 2'd0;
        sb_op         = r_op;
        sb_eq         = r_eq;
        sb_key_en     = r_key_en;
        sb_key        = r_key;
        sb_w4         = r_w4;
        sb_c          = r_c;
        sb_sfmt       = r_sfmt;
        sb_read_dst   = d7_read_dst;

        rd_valid      = cur_rd;
        rd_addr       = seg_addr[seg_i];
        rd_beats      = seg_beats[seg_i];
        rd_buf        = {seg_dir[seg_i], row_i[0]};
        // TEX 标记仅用于源读；BAND 标记仅用于目的读写，逻辑地址仍为全帧地址。
        rd_tex        = cur_rd && !seg_dir[seg_i] && r_src_tex;
        rd_band       = cur_rd && seg_dir[seg_i] && r_dst_band;
        wr_band       = cur_wr && r_dst_band;
        rd_cidx       = r_cidx;

        wr_valid      = cur_wr;
        wr_addr       = seg_addr[seg_i];
        wr_beats      = seg_beats[seg_i];
        wr_cidx       = r_cidx;
        wr_last_row   = cur_wr && last_row_now && last_wr_seg;
    end

    // 时序
    always @(posedge clk) begin
        if (rst) begin
            st <= S_IDLE; busy <= 1'b0;
            cmd_done <= 1'b0; done_cidx <= 4'd0;
            seg_i <= 3'd0; row_i <= 16'd0;
            dst_cur <= 32'd0; src_cur <= 32'd0; nb <= 32'd0;
            dst_stride <= 32'd0; src_stride <= 32'd0;
            nrows <= 16'd0; npx <= 11'd0; py <= 16'sd0;
            r_op <= 8'd0; r_eq <= 4'd0; r_key_en <= 1'b0; r_key <= 32'd0;
            r_w4 <= 32'd0; r_c <= 8'd0; r_sfmt <= 4'd0; r_src_tex <= 1'b0; r_dst_band<=0; r_cidx <= 4'd0;
        end else begin
            cmd_done <= 1'b0;
            case (st)
            S_IDLE: begin
                busy <= 1'b0;
                if (start) begin
                    // start 那一拍端口值有效：组合求交的结果当拍锁存
                    r_op <= op; r_eq <= eq; r_key_en <= key_en; r_key <= key_w5;
                    r_w4 <= const_w4; r_c <= c; r_sfmt <= sfmt;
                    r_src_tex <= src_tex; r_cidx <= cidx;
                    r_dst_band<=band_enable;
                    done_cidx <= cidx;
                    if (i_empty) begin
                        cmd_done <= 1'b1;                 // 零面积命令
                        st <= S_IDLE;
                    end else begin
                        busy      <= 1'b1;
                        nrows     <= t_y1[15:0] - t_y0[15:0] + 16'd1;
                        npx       <= t_x1[10:0] - t_x0[10:0] + 11'd1;
                        nb        <= ({21'b0, (t_x1[10:0] - t_x0[10:0] + 11'd1)}) << 2;
                        py        <= t_y0[15:0];
                        dst_stride <= 32'd1 << dst_slog2;
                        src_stride <= 32'd1 << src_slog2;
                        dst_cur   <= dst_base + ({14'd0, t_y0} << dst_slog2) +
                                     ({14'd0, t_x0} << 2);
                        src_cur   <= src_base +
                                     (src_row_y << src_slog2) +
                                     (sx_off32 << 2);
                        row_i     <= 16'd0;
                        seg_i     <= 3'd0;
                        // 第 0 行也要等它那一半空着：上一条命令的最后几行可能还占着
                        st        <= rf_first ? S_ROW : S_GATE;
                    end
                end
            end
            S_GATE: begin
                if (rf_cur) st <= S_ROW;
            end
            S_ROW: begin
                if (seg_i == 3'd7) begin
                    st <= S_ADV;                          // 本行发完
                end else if (seg_i == 3'd0) begin
                    if (sb_ready) seg_i <= nxt_i;         // 边带握手（本行第一件事）
                end else if (!seg_pres[seg_i]) begin
                    seg_i <= nxt_i;                       // 空槽不占拍
                end else if (seg_iswr[seg_i] ? wr_ready : rd_ready) begin
                    seg_i <= nxt_i;                       // 握手当拍完成
                end
            end
            S_ADV: begin
                // 行缓冲的乒乓：下一行要用的那一半空着才推（row_free 由计算阵列给）
                if (last_row_now) begin
                    st <= S_DONE;
                end else if (rf_next) begin
                    dst_cur <= dst_cur + dst_stride;
                    src_cur <= src_cur + src_stride;
                    py      <= py + 16'sd1;
                    row_i   <= row_i + 16'd1;
                    seg_i   <= 3'd0;
                    st      <= S_ROW;
                end
            end
            S_DONE: begin
                busy <= 1'b0;
                st   <= S_IDLE;
            end
            default: st <= S_IDLE;
            endcase
        end
    end
endmodule

`default_nettype wire
