// probe_geom：几何层前期研究的探针，**不是入库实现**
//
// 入库实现是 rtl/engine/engine_geom.v，本文件只回答研究清单的两问：
//   第 1 问 产出速率：行迭代每行、每条命令花几拍（配套 tb_probe_geom.v 逐拍量）；
//   第 5 问 实验：把「五者求交的 32 位比较链」与「行迭代的加法链」单独综合，
//           看 100 MHz 下的余量与资源（配套 syn/ 下的 Efinity 小工程）。
//
// 四个模块：
//   probe_geom_intersect  纯组合的五者求交（32 位有符号 max/min 链），给综合看关键路径；
//   probe_geom_d7         读写避让的判定（读不读源、读不读目的），纯组合；
//   probe_geom_rowiter    多拍顺序的行迭代：每行算覆盖区间、按 4KB 与最大拍数拆段、
//                         按 rsrc/rdst（来自 probe_geom_d7）发哪几笔读、给每行边带；
//   probe_geom_top        两者接起来，带观测寄存器（综合删不掉任何一位，照混合核与
//                         搬运层探针的做法）。
//
// 关键路径的两种候选写在文件末的注释里。
`timescale 1ns/1ps
`default_nettype none

// ---------------------------------------------------------------------------
// 五者求交：目的矩形、游戏区、裁剪框、目的表面、源表面（FILL 交前四项）
// 中间量一律 32 位有符号；空宽高先判。与 tools/check/geom_model.py 的 intersect() 同规则。
// ---------------------------------------------------------------------------
module probe_geom_intersect #(
    parameter integer INJECT = 0        // 位 0：漏掉第五项（源表面）
)(
    input  wire signed [31:0] dst_x,
    input  wire signed [31:0] dst_y,
    input  wire        [31:0] w,
    input  wire        [31:0] h,
    input  wire signed [31:0] src_x,
    input  wire signed [31:0] src_y,
    input  wire        [31:0] src_surf_w,
    input  wire        [31:0] src_surf_h,
    input  wire        [31:0] dst_surf_w,
    input  wire        [31:0] dst_surf_h,
    input  wire signed [31:0] clip_x0,
    input  wire signed [31:0] clip_y0,
    input  wire signed [31:0] clip_x1,
    input  wire signed [31:0] clip_y1,
    input  wire               is_blit,
    output reg  signed [31:0] x0,
    output reg  signed [31:0] y0,
    output reg  signed [31:0] x1,
    output reg  signed [31:0] y1,
    output reg                empty
);
    // 四者与第五项的上下界，写成中间量便于看清链长
    reg signed [31:0] t_x0, t_y0, t_x1, t_y1;
    reg signed [31:0] dx1, dy1;
    reg signed [31:0] s_lo_x, s_lo_y, s_hi_x, s_hi_y;

    always @* begin
        // 先把所有中间量与输出给默认值：求交是纯组合，少赋一条路径就推断出锁存器
        // （首版漏了 dx1/dy1 的默认值，Efinity 报 latch inferred for 'dx1[31]'）。
        dx1 = 32'sd0; dy1 = 32'sd0;
        t_x0 = 32'sd0; t_y0 = 32'sd0; t_x1 = -32'sd1; t_y1 = -32'sd1;
        s_lo_x = 32'sd0; s_lo_y = 32'sd0; s_hi_x = 32'sd0; s_hi_y = 32'sd0;
        x0 = 32'sd0; y0 = 32'sd0; x1 = -32'sd1; y1 = -32'sd1;
        empty = 1'b1;
        if ((w == 32'd0) || (h == 32'd0)) begin
            x0 = 32'sd0; y0 = 32'sd0; x1 = -32'sd1; y1 = -32'sd1;
            empty = 1'b1;
        end else begin
            dx1 = dst_x + $signed(w) - 32'sd1;
            dy1 = dst_y + $signed(h) - 32'sd1;

            t_x0 = (dst_x > 32'sd0)     ? dst_x : 32'sd0;
            t_y0 = (dst_y > 32'sd0)     ? dst_y : 32'sd0;
            if (clip_x0 > t_x0) t_x0 = clip_x0;
            if (clip_y0 > t_y0) t_y0 = clip_y0;

            t_x1 = (dx1 < 32'sd639) ? dx1 : 32'sd639;
            t_y1 = (dy1 < 32'sd479) ? dy1 : 32'sd479;
            if (clip_x1 < t_x1) t_x1 = clip_x1;
            if (clip_y1 < t_y1) t_y1 = clip_y1;
            if ($signed(dst_surf_w) - 32'sd1 < t_x1) t_x1 = $signed(dst_surf_w) - 32'sd1;
            if ($signed(dst_surf_h) - 32'sd1 < t_y1) t_y1 = $signed(dst_surf_h) - 32'sd1;

            if (is_blit && !((INJECT & 1) != 0)) begin
                s_lo_x = dst_x - src_x;
                s_lo_y = dst_y - src_y;
                s_hi_x = dst_x - src_x + $signed(src_surf_w) - 32'sd1;
                s_hi_y = dst_y - src_y + $signed(src_surf_h) - 32'sd1;
                if (s_lo_x > t_x0) t_x0 = s_lo_x;
                if (s_lo_y > t_y0) t_y0 = s_lo_y;
                if (s_hi_x < t_x1) t_x1 = s_hi_x;
                if (s_hi_y < t_y1) t_y1 = s_hi_y;
            end

            if ((t_x1 < t_x0) || (t_y1 < t_y0)) begin
                x0 = 32'sd0; y0 = 32'sd0; x1 = -32'sd1; y1 = -32'sd1;
                empty = 1'b1;
            end else begin
                x0 = t_x0; y0 = t_y0; x1 = t_x1; y1 = t_y1;
                empty = 1'b0;
            end
        end
    end
endmodule

// ---------------------------------------------------------------------------
// 读写避让的判定：一条命令读不读源、读不读目的
//   FILL 是常量源，永远不读源；BLIT 永远读源。
//   方程为 COPY 时不读目的（COPY 不看目的，色键命中的像素走 X 字节的带内标记），其余五个方程都读目的。
// 行迭代把 rsrc/rdst 当输入，判定写在这里，注错才有一处真实的逻辑可改；
// 入库实现里它在 engine_geom.v 内部。
// ---------------------------------------------------------------------------
module probe_geom_d7 #(
    parameter integer INJECT = 0        // 位 0：ALPHA 也按 COPY 走「不读目的」的避让
)(
    input  wire [7:0] op,               // 1 FILL、2 BLIT
    input  wire [3:0] eq,               // 0 COPY、1 ALPHA、2 ADD、3 CONST、4 MUL、5 ALPHA_STRAIGHT
    output wire       rsrc,
    output wire       rdst
);
    localparam [7:0] OP_BLIT  = 8'd2;
    localparam [3:0] EQ_COPY  = 4'd0;
    localparam [3:0] EQ_ALPHA = 4'd1;

    wire as_copy = (eq == EQ_COPY) || (((INJECT & 1) != 0) && (eq == EQ_ALPHA));

    assign rsrc = (op == OP_BLIT);
    assign rdst = !as_copy;
endmodule

// ---------------------------------------------------------------------------
// 行迭代：多拍顺序实现（研究要回答「多拍够不够」，所以先写最朴素的一种）
//
// 每行的动作：
//   1 算目的、源的行首地址与 16 字节覆盖区间；
//   2 按「不跨 4KB」与「不超过 MAX_BEATS 拍」拆段（直通模式里最多两段）；
//   3 按避让判定发读描述符（源、目的），再发写描述符（每段一笔）；
//   4 给每行边带：本行像素数、源与目的的拍内像素相位（首拍前导填充）、写拍数。
// 描述符一拍一笔；消费者不 ready 就停在原处（搬运层每个方向一笔在途，天然反压）。
// ---------------------------------------------------------------------------
module probe_geom_rowiter #(
    parameter integer MAX_BEATS = 192,
    // 位 0：跨 4KB 不拆（避让的注错在 probe_geom_d7）
    parameter integer INJECT = 0
)(
    input  wire        clk,
    input  wire        rst,

    // 命令口（研究探针：只接求交结果之后的那部分字段）
    input  wire        start,
    input  wire [31:0] dst_row0,      // 第一行的行首字节地址（= base + (y0<<stride) + x0*4）
    input  wire [31:0] src_row0,
    input  wire [31:0] dst_stride,    // 1 << dst_stride_log2
    input  wire [31:0] src_stride,
    input  wire [15:0] nrows,
    input  wire [10:0] npx,           // 本行像素数
    input  wire        rsrc,          // 避让判定：读不读源（probe_geom_d7 给）
    input  wire        rdst,          // 避让判定：读不读目的（probe_geom_d7 给）
    input  wire        last_cmd,      // 本命令的最后一行由它标（写描述符的 wr_last_row）
    input  wire [3:0]  cidx,

    // 读描述符
    output reg  [31:0] rd_addr,
    output reg  [11:0] rd_beats,
    output reg  [1:0]  rd_buf,
    output reg         rd_valid,
    input  wire        rd_ready,

    // 写描述符
    output reg  [31:0] wr_addr,
    output reg  [11:0] wr_beats,
    output reg         wr_last_row,
    output reg         wr_valid,
    input  wire        wr_ready,

    // 每行边带
    output reg  [10:0] sb_npx,
    output reg  [1:0]  sb_lead_dst,
    output reg  [1:0]  sb_lead_src,
    output reg  [7:0]  sb_beats,
    output reg         sb_valid,
    input  wire        sb_ready,

    output reg         busy,
    output reg         done
);
    localparam [2:0] S_IDLE = 3'd0, S_CALC = 3'd1, S_EMIT = 3'd2,
                     S_SB   = 3'd3, S_NEXT = 3'd4, S_DONE = 3'd5;

    reg [2:0]  st;
    reg [31:0] dst_cur, src_cur;
    reg [15:0] row_i;
    reg [31:0] nb;                       // 本行字节数 = npx*4

    // 本行组合算出的六个候选段：0/1 源、2/3 目的读、4/5 写
    reg [31:0] seg_addr  [0:5];
    reg [11:0] seg_beats [0:5];
    reg        seg_iswr  [0:5];
    reg [5:0]  seg_pres;
    reg [2:0]  seg_i;

    // 组合的覆盖区间与拆段
    reg [31:0] a0, a1, alen;
    reg [11:0] b0, b1;
    reg        xcross;
    reg [31:0] s_a0, s_a1, s_len;
    reg [11:0] s_b0, s_b1;
    reg        sxcross;
    reg [11:0] max_bytes;
    reg [12:0] page_left, page_left_s;   // 13 位：4096 装不进 12 位

    integer k;

    always @* begin
        max_bytes = MAX_BEATS * 12'd16;
        // 目的
        a0   = {dst_cur[31:4], 4'b0000};
        a1   = (dst_cur + nb + 32'd15) & 32'hFFFFFFF0;
        alen = a1 - a0;
        page_left = 13'h1000 - {1'b0, a0[11:0]};
        xcross = (!((INJECT & 1) != 0)) && (({19'b0, a0[11:0]} + {19'b0, alen[11:0]}) > 32'h1000);
        if (xcross) begin
            b0 = (page_left > {1'b0, max_bytes}) ? max_bytes : page_left[11:0];
            b1 = alen[11:0] - b0;
            if (b1 > max_bytes) b1 = max_bytes;      // 直通模式到不了，留作用例
        end else begin
            b0 = (alen > {20'b0, max_bytes}) ? max_bytes : alen[11:0];
            b1 = alen[11:0] - b0;
        end
        // 源（行首地址与目的同一相位关系，长度相同）
        s_a0   = {src_cur[31:4], 4'b0000};
        s_a1   = (src_cur + nb + 32'd15) & 32'hFFFFFFF0;
        s_len  = s_a1 - s_a0;
        page_left_s = 13'h1000 - {1'b0, s_a0[11:0]};
        sxcross = (!((INJECT & 1) != 0)) && (({19'b0, s_a0[11:0]} + {19'b0, s_len[11:0]}) > 32'h1000);
        if (sxcross) begin
            s_b0 = (page_left_s > {1'b0, max_bytes}) ? max_bytes : page_left_s[11:0];
            s_b1 = s_len[11:0] - s_b0;
            if (s_b1 > max_bytes) s_b1 = max_bytes;
        end else begin
            s_b0 = (s_len > {20'b0, max_bytes}) ? max_bytes : s_len[11:0];
            s_b1 = s_len[11:0] - s_b0;
        end

        for (k = 0; k < 6; k = k + 1) begin
            seg_addr[k]  = 32'd0;
            seg_beats[k] = 12'd0;
            seg_iswr[k]  = 1'b0;
            seg_pres[k]  = 1'b0;
        end
        // 读源：只有 BLIT 有
        if (rsrc) begin
            seg_addr[0] = s_a0; seg_beats[0] = s_b0 >> 4; seg_pres[0] = (s_b0 != 0);
            seg_addr[1] = s_a0 + {{20{1'b0}}, s_b0}; seg_beats[1] = s_b1 >> 4;
            seg_pres[1] = (s_b1 != 0);
        end
        // 读目的：只有 rdst 为 1 才发
        if (rdst) begin
            seg_addr[2] = a0; seg_beats[2] = b0 >> 4; seg_pres[2] = (b0 != 0);
            seg_addr[3] = a0 + {{20{1'b0}}, b0}; seg_beats[3] = b1 >> 4;
            seg_pres[3] = (b1 != 0);
        end
        // 写目的：每段一笔
        seg_addr[4] = a0; seg_beats[4] = b0 >> 4; seg_iswr[4] = 1'b1;
        seg_pres[4] = (b0 != 0);
        seg_addr[5] = a0 + {{20{1'b0}}, b0}; seg_beats[5] = b1 >> 4; seg_iswr[5] = 1'b1;
        seg_pres[5] = (b1 != 0);
    end

    wire [11:0] wr_total = (b0 >> 4) + (b1 >> 4);

    // 跳到 seg_i 之后第一个有货的槽（没有就 6）
    reg [2:0] nxt_i;
    always @* begin
        nxt_i = 3'd6;
        for (k = 0; k < 6; k = k + 1)
            if ((k > seg_i) && seg_pres[k] && (nxt_i == 3'd6))
                nxt_i = k[2:0];
    end

    // 描述符与边带的 valid 与数据组合驱动：握手当拍完成，一笔描述符占一拍
    wire cur_pres = (st == S_EMIT) && (seg_i != 3'd6) && seg_pres[seg_i];
    wire cur_wr   = cur_pres && seg_iswr[seg_i];
    wire cur_rd   = cur_pres && !seg_iswr[seg_i];
    wire last_wr_seg = (seg_i == 3'd5) || (seg_pres[5] == 1'b0);

    always @* begin
        rd_valid = cur_rd;
        rd_addr  = seg_addr[seg_i];
        rd_beats = seg_beats[seg_i];
        rd_buf   = seg_i[1:0];                      // 研究探针：0/1 源、2/3 目的

        wr_valid    = cur_wr;
        wr_addr     = seg_addr[seg_i];
        wr_beats    = seg_beats[seg_i];
        wr_last_row = last_cmd && (row_i == (nrows - 16'd1)) && last_wr_seg;

        sb_valid = (st == S_SB);
        sb_npx      = npx;
        sb_lead_dst = dst_cur[3:2];
        sb_lead_src = src_cur[3:2];
        sb_beats    = wr_total[7:0];
    end

    always @(posedge clk) begin
        if (rst) begin
            st <= S_IDLE;
            busy <= 1'b0; done <= 1'b0;
            dst_cur <= 32'd0; src_cur <= 32'd0; row_i <= 16'd0; nb <= 32'd0;
            seg_i <= 3'd0;
        end else begin
            done <= 1'b0;
            case (st)
            S_IDLE: begin
                if (start) begin
                    dst_cur <= dst_row0;
                    src_cur <= src_row0;
                    nb      <= {21'b0, npx} << 2;
                    row_i   <= 16'd0;
                    busy    <= 1'b1;
                    seg_i   <= 3'd0;
                    st      <= S_EMIT;
                end else begin
                    busy <= 1'b0;
                end
            end
            S_EMIT: begin
                if (seg_i == 3'd6) begin
                    st <= S_SB;
                end else if (!seg_pres[seg_i]) begin
                    seg_i <= nxt_i;                     // 空槽不占拍，一次跳到下一个
                end else if (seg_iswr[seg_i] ? wr_ready : rd_ready) begin
                    seg_i <= nxt_i;                     // 握手当拍完成
                end
            end
            S_SB: begin
                if (sb_ready) st <= S_NEXT;
            end
            S_NEXT: begin
                if (row_i == (nrows - 16'd1)) begin
                    st <= S_DONE;
                end else begin
                    dst_cur <= dst_cur + dst_stride;
                    src_cur <= src_cur + src_stride;
                    row_i   <= row_i + 16'd1;
                    seg_i   <= 3'd0;
                    st      <= S_EMIT;
                end
            end
            S_DONE: begin
                busy <= 1'b0;
                done <= 1'b1;
                st   <= S_IDLE;
            end
            default: st <= S_IDLE;
            endcase
        end
    end
endmodule

// ---------------------------------------------------------------------------
// 单独综合的顶层：把求交与行迭代都编进去，并且不让综合删掉任何一位。
// 照混合核与搬运层探针的做法：LFSR 造激励，全部输出按位异或进观测寄存器，观测寄存器回灌 LFSR。
// ---------------------------------------------------------------------------
module probe_geom_top (
    input  wire        clk,
    input  wire        rst,
    output reg  [127:0] obs
);
    reg [127:0] lfsr;
    always @(posedge clk) begin
        if (rst) lfsr <= 128'h0123456789ABCDEF_FEDCBA9876543210;
        else     lfsr <= {lfsr[126:0], lfsr[127] ^ lfsr[125] ^ lfsr[124] ^ lfsr[121]};
    end

    wire signed [31:0] dst_x = $signed(lfsr[15:0]);
    wire signed [31:0] dst_y = $signed(lfsr[31:16]);
    wire        [31:0] w     = {6'b0, lfsr[41:16]};
    wire        [31:0] h     = {6'b0, lfsr[57:32]};
    wire signed [31:0] src_x = $signed(lfsr[63:48]);
    wire signed [31:0] src_y = $signed(lfsr[79:64]);
    wire        [31:0] ssw   = {26'b0, lfsr[85:80]};
    wire        [31:0] ssh   = {26'b0, lfsr[91:86]};
    wire        [31:0] dsw   = {26'b0, lfsr[97:92]};
    wire        [31:0] dsh   = {26'b0, lfsr[103:98]};

    wire signed [31:0] ix0, iy0, ix1, iy1;
    wire               iempty;

    probe_geom_intersect u_int (
        .dst_x(dst_x), .dst_y(dst_y), .w(w), .h(h),
        .src_x(src_x), .src_y(src_y),
        .src_surf_w(ssw), .src_surf_h(ssh),
        .dst_surf_w(dsw), .dst_surf_h(dsh),
        .clip_x0($signed({16'b0, lfsr[111:96]})),
        .clip_y0($signed({16'b0, lfsr[127:112]})),
        .clip_x1(32'sd639), .clip_y1(32'sd479),
        .is_blit(lfsr[0]),
        .x0(ix0), .y0(iy0), .x1(ix1), .y1(iy1), .empty(iempty)
    );

    // 行首地址：base 取 LFSR 的一段，保证非 4KB 对齐也编进去
    wire [31:0] dst_base = {lfsr[119:96], 8'h00};
    wire [31:0] dst_r0   = dst_base + (iy0 << 12) + (ix0 << 2);
    wire [31:0] src_r0   = {lfsr[95:72], 8'h00} + (iy0 << 8) + (ix0 << 2);

    wire [31:0] rd_addr, wr_addr;
    wire [11:0] rd_beats, wr_beats;
    wire [1:0]  rd_buf;
    wire        rd_valid, wr_valid, wr_last_row;
    wire [10:0] sb_npx;
    wire [1:0]  sb_lead_dst, sb_lead_src;
    wire [7:0]  sb_beats;
    wire        sb_valid, busy, done;
    reg         start_r;

    always @(posedge clk) start_r <= (lfsr[23:16] == 8'hA5);

    probe_geom_rowiter u_row (
        .clk(clk), .rst(rst),
        .start(start_r),
        .dst_row0(dst_r0), .src_row0(src_r0),
        .dst_stride(32'h1000), .src_stride(32'h100),
        .nrows({8'b0, lfsr[31:24]}),
        .npx(lfsr[10:0]),
        .rsrc(lfsr[1]), .rdst(lfsr[2]), .last_cmd(lfsr[3]), .cidx(lfsr[7:4]),
        .rd_addr(rd_addr), .rd_beats(rd_beats), .rd_buf(rd_buf),
        .rd_valid(rd_valid), .rd_ready(lfsr[4]),
        .wr_addr(wr_addr), .wr_beats(wr_beats), .wr_last_row(wr_last_row),
        .wr_valid(wr_valid), .wr_ready(lfsr[5]),
        .sb_npx(sb_npx), .sb_lead_dst(sb_lead_dst), .sb_lead_src(sb_lead_src),
        .sb_beats(sb_beats), .sb_valid(sb_valid), .sb_ready(lfsr[6]),
        .busy(busy), .done(done)
    );

    // 观测：被测的每一个输出都要按位异或进来，否则综合会把没被观测的位整块删掉
    // （踩过一次：观测只接了控制位，两个核的输出 FIFO 被剪光）。
    // 248 位折成 128 位：obsv[247:128] 是 120 位，补 8 位零才凑成 128。
    // 首版补了 120 位零（写成 {120'b0, obsv[247:128]}，结果 240 位），
    // Efinity 报 "expression size 240 truncated to fit in target size 128"，高 112 位被丢掉。
    wire [247:0] obsv = {
        ix0, iy0, ix1, iy1,
        rd_addr, rd_beats, rd_buf,
        wr_addr, wr_beats, wr_last_row,
        sb_npx, sb_lead_dst, sb_lead_src, sb_beats,
        rd_valid, wr_valid, sb_valid, busy, done, iempty
    };

    always @(posedge clk) begin
        if (rst) obs <= 128'd0;
        else obs <= obs ^ obsv[127:0] ^ {8'b0, obsv[247:128]} ^ lfsr;
    end
endmodule

`default_nettype wire
