`timescale 1ns/1ps
//==============================================================================================//
// FILE: engine_pix.v
//
// DESCRIPTION: 计算阵列与片上缓存：每行一条边带加读流入片上行缓冲，两拍窗口把
//   源相位对齐到目的，12 个 engine_blend 算出 4 像素交给写流。只碰像素不碰地址。
//
// NOTE:
//   1. 源块 512×128、目的一块 512×96 两半乒乓；目的 X 字节不进 RAM，读写口只存 RGB
//   2. 延迟 7 拍；色键/掩码/valid/拍号四条延迟像逐级贴着数据链，差一级就错拍
//   3. 整条流水挂一个时钟使能 en = ws_tready || !ws_tvalid；一行发完等 8 拍排空再开下一行
//   4. 一行跨 4KB 时读流有两个 tlast，写指针两段之间不清，只在该半边带握手时清
//   5. 混合核五个输入必须由寄存器直接驱动；注错开关见 engine-manual.md
//   6. 按四段组织：段一收货（行资源）、段二备料（行缓冲读写）、段三加工（七拍通路）、
//      段四节拍（行状态机）；跨段状态集中在顶部声明，段内只放逻辑
//==============================================================================================//
`default_nettype none
`include "engine_feature.vh"

module engine_pix (
    input  wire         clk,
    input  wire         rst,            // 同步复位

    // 每行边带（几何层 → 本层）
    input  wire [15:0]  sb_row,         // 行号（命令内计数，最低位 = 半）
    input  wire [10:0]  sb_npx,         // 本行像素数
    input  wire [7:0]   sb_beats,       // 本行写拍数（拆段时是两段之和）
    input  wire [1:0]   sb_lead_dst,    // 目的相位 ld（行首在拍内的像素位置）
    input  wire [1:0]   sb_lead_src,    // 源相位 ls（行首在拍内的像素位置，FILL 为 0）
    input  wire [7:0]   sb_op,          // 操作码，只区分 BLIT 与 FILL
    input  wire [3:0]   sb_eq,          // 方程号，0~5
    input  wire         sb_key_en,      // 色键使能
    input  wire [31:0]  sb_key,         // 色键字（w5，与源原始存储字整字比较）
    input  wire [31:0]  sb_w4,          // FILL 常量源字
    input  wire [7:0]   sb_c,           // CONST 系数
    input  wire [3:0]   sb_sfmt,        // 源格式（只对 BLIT 有意义）
    input  wire         sb_read_dst,    // 读目的使能（0 = 本行不读目的块）
    input  wire         sb_valid,       // 边带使能
    output wire         sb_ready,       // 边带就绪（本半空着且队列未满）
    output wire [1:0]   row_free,       // 半空闲（每半一位，1 = 该半空着）

    // 读流（搬运层 → 本层）
    input  wire [127:0] rs_tdata,       // 读数据（128 位 = 4 像素）
    input  wire [15:0]  rs_tkeep,       // 字节有效（恒全 1，暂时不用该信号）
    input  wire         rs_tlast,       // 末拍标志（标描述符边界，暂时不用该信号）
    input  wire         rs_tvalid,      // 读数据有效
    output wire         rs_tready,      // 读数据就绪，恒 1
    input  wire [1:0]   rs_buf,         // 缓冲号 {源/目的, 半}

    // 写流（本层 → 搬运层），没有 tlast
    output reg  [127:0] ws_tdata,       // 写数据（每拍 4 像素，X 字节最高位 = 不写标记）
    output wire [15:0]  ws_tkeep,       // 字节有效，恒全 1（逐像素掩码走带内标记）
    output reg          ws_tvalid,      // 写数据有效
    input  wire         ws_tready,      // 写数据就绪，低时整条流水停

    // 色键跳过事件（控制层 PERF_KEY_SKIP 计数源）
    output wire [3:0]   perf_key_lanes, // 色键掩码，与写数据同拍
    output wire         perf_key_valid  // 计数有效，写流握手那一拍
);
    localparam [7:0] OP_BLIT = 8'd2;  // BLIT 操作码
    localparam [3:0] FMT_XRGB = 4'd0; // XRGB 源格式全0
    localparam [1:0] PS_IDLE = 2'd0, PS_PRE = 2'd1, PS_RUN = 2'd2, PS_DRAIN = 2'd3;  // pst 状态编码

    // 时钟使能：流水各级与 12 个核同一门控
`ifdef INJ_EN_NOTREADY // 注错：流水不看 tready
    wire en = 1'b1;
`else
    wire en = (ws_tready || !ws_tvalid);
`endif

`ifdef INJ_CORE_NOEN // 注错：只让 12 个核不看流水停
    wire en_core = 1'b1;
`else
    wire en_core = en;
`endif

    // 端口常量
    assign ws_tkeep = 16'hffff;
    assign rs_tready = 1'b1;

    // 跨段状态：由段四在开行那一拍装载，段二与段三逐拍消费
    reg [1:0] pst;
    reg [7:0] cnt_k;
    reg [7:0] drain_count;
    reg running;
    reg [1:0] r_ls, r_ld;
    reg r_half;
    reg r_blit, r_read_dst;
    reg r_key_en;
    reg [2:0] r_eq;
    reg [31:0] r_key, r_w4;
    reg [7:0] r_c;
    reg [7:0] r_beats;
    reg [3:0] r_sfmt;
    reg [3:0] r_first_mask, r_last_mask;

    // 段一 收货：半占用、行队列、每半行参数与读齐
    // 半占用与行队列：边带握手在这一组进出，周期 1 的写使能按 occupied 判哪一半能收
    reg [1:0] occupied;
    reg row_queue [0:1];
    reg q_rd, q_wr;
    reg [1:0] q_count;
    assign row_free = ~occupied;
    assign sb_ready = !rst && !occupied[sb_row[0]] && (q_count != 2'd2);
    wire sb_fire = sb_valid && sb_ready;

    // 每一半的读入计数：边带握手时装入，随读流每拍递增，下一拍的写地址就是它
    reg [7:0] h_src_count [0:1], h_dst_count [0:1];

    // 每一半的读齐标志：源按 src_need 拍、目的按本行写拍数判
    reg h_src_done [0:1], h_dst_done [0:1];
    wire next_half = row_queue[q_rd];
    // 开行条件：队头那一半要用的源、目的都已读齐（FILL 不等源，不读目的的行不等目的）
    wire row_start = !rst && en && (pst == PS_IDLE) && (q_count != 2'd0) && h_src_done[next_half] && h_dst_done[next_half];

    // 本行写流的末拍被写核收走：放出这一半
    reg [7:0] out_count;
    wire last_out = ws_tvalid && ws_tready &&
                    (out_count == (r_beats - 8'd1));
    always @(posedge clk) begin
        if (rst) out_count <= 8'd0;
        else if (row_start) out_count <= 8'd0;
        else if (ws_tvalid && ws_tready) out_count <= out_count + 8'd1;
    end

    // 每一半的行参数：边带握手时装入，开行时整组搬进当前行寄存器，整行不变
    reg h_blit [0:1], h_key_en [0:1], h_read_dst [0:1];
    reg [2:0] h_eq [0:1];
    reg [31:0] h_key [0:1], h_w4 [0:1];
    reg [7:0] h_c [0:1];
    reg [3:0] h_sfmt [0:1];
    reg [1:0] h_ls [0:1], h_ld [0:1];
    reg [7:0] h_beats [0:1], h_src_need [0:1];
    reg [3:0] h_first_mask [0:1], h_last_mask [0:1];

    // 源要读的拍数 = ceil((ls + npx) / 4)，与几何层源覆盖区间的拍数相同
    wire [11:0] src_sum = {1'b0, sb_npx} + {10'd0, sb_lead_src} + 12'd3;
    wire [7:0] src_need = src_sum[9:2];

    // 首末拍掩码：第 beat 拍的四个槽谁属于本行；一拍的行把行尾也算进首拍
    function [3:0] row_mask;
        input [1:0] lead;
        input [10:0] pixels;
        input [7:0] beat;
        reg [11:0] first, limit, pos;
        integer t;
        begin
            first = {10'd0, lead};
            limit = first + {1'b0, pixels};
            row_mask = 4'b0000;
            for (t = 0; t < 4; t = t + 1) begin
                pos = {2'd0, beat, 2'b00} + t;
                row_mask[t] = (pos >= first) && (pos < limit);
            end
        end
    endfunction

    // 段二 备料：两块行缓冲的写入与同步读出，以及每半的占用与读入计数
    // 周期 1：行缓冲的装入与同步读出
    // 装入：读流入的拍按半写进行缓冲，写指针 = {半, 计数}，读齐按拍数判不看 rs_tlast
    wire src_we = !rst && rs_tvalid && !rs_buf[1] && occupied[rs_buf[0]];
    wire dst_we = !rst && rs_tvalid && rs_buf[1] && occupied[rs_buf[0]];
    wire [8:0] src_waddr = {rs_buf[0], h_src_count[rs_buf[0]]};
    wire [8:0] dst_waddr = {rs_buf[0], h_dst_count[rs_buf[0]]};
    // 目的只存每个像素的 RGB，X 字节不进 RAM
    reg [95:0] q_dst;
    reg [127:0] q_src;
    reg [95:0] dst_mem [0:511];
    reg [127:0] src_mem [0:511];
    wire [95:0] dst_wdata = {rs_tdata[119:96], rs_tdata[87:64], rs_tdata[55:32], rs_tdata[23:0]};
    // 周期 0：地址。纯组合、不占拍，本级只算出 RAM 的读地址
    // 旋转量与读地址同源：off 决定读哪一拍，st_use 由段三的旋转器使用
    wire rot_off = (r_ls >= r_ld);
    wire [2:0] st_raw = rot_off ? ({1'b0, r_ls} - {1'b0, r_ld}) : ({1'b0, r_ls} + 3'd4 - {1'b0, r_ld});

`ifdef INJ_ROT_OFF  // 注错：旋转错一个位置
    wire [2:0] st_use = (st_raw + 3'd1);
`else
    wire [2:0] st_use = st_raw;
`endif

    wire [7:0] src_k = cnt_k + {7'd0, rot_off};
    reg [8:0] src_raddr, dst_raddr;
    always @* begin
        src_raddr = {r_half, (pst == PS_PRE ? 8'd0 : src_k)};
        dst_raddr = {r_half, cnt_k};
    end

    // 读使能：FILL 不读源块，COPY 不读目的块，读出值不进通路，关掉省功耗
    wire src_re = en && (pst == PS_PRE || pst == PS_RUN) && r_blit;
    wire dst_re = en && (pst == PS_PRE || pst == PS_RUN) && r_read_dst;
    // 同步读：地址在周期 0 给出，数据这一拍才落进 q_src/q_dst
    always @(posedge clk) begin
        if (src_we) src_mem[src_waddr] <= rs_tdata;
        if (dst_we) dst_mem[dst_waddr] <= dst_wdata;
        if (src_re) q_src <= src_mem[src_raddr];
        if (dst_re) q_dst <= dst_mem[dst_raddr];
    end

    // 每一半的占用、行参数装入与读入计数
    always @(posedge clk) begin
        if (rst) begin
            occupied <= 2'b00;
            q_rd <= 1'b0;
            q_wr <= 1'b0;
            q_count <= 2'd0;
            h_src_count[0] <= 0;
            h_src_count[1] <= 0;
            h_dst_count[0] <= 0;
            h_dst_count[1] <= 0;
            h_src_done[0] <= 0;
            h_src_done[1] <= 0;
            h_dst_done[0] <= 0;
            h_dst_done[1] <= 0;
        end else begin
            if (sb_fire) begin
                occupied[sb_row[0]] <= 1'b1;
                row_queue[q_wr] <= sb_row[0]; q_wr <= ~q_wr;
                h_blit[sb_row[0]] <= (sb_op == OP_BLIT);
                h_key_en[sb_row[0]] <= sb_key_en;
                h_read_dst[sb_row[0]] <= sb_read_dst;
                h_eq[sb_row[0]] <= sb_eq[2:0];
                h_key[sb_row[0]] <= sb_key; h_w4[sb_row[0]] <= sb_w4;
                h_c[sb_row[0]] <= sb_c; h_sfmt[sb_row[0]] <= sb_sfmt;
                h_ls[sb_row[0]] <= sb_lead_src; h_ld[sb_row[0]] <= sb_lead_dst;
                h_beats[sb_row[0]] <= sb_beats;
                h_src_need[sb_row[0]] <= src_need;
                h_src_count[sb_row[0]] <= 8'd0;
                h_dst_count[sb_row[0]] <= 8'd0;
                h_src_done[sb_row[0]] <= (sb_op != OP_BLIT);
                h_dst_done[sb_row[0]] <= !sb_read_dst;
                h_first_mask[sb_row[0]] <= row_mask(sb_lead_dst, sb_npx, 8'd0);
                h_last_mask[sb_row[0]] <= row_mask(sb_lead_dst, sb_npx, sb_beats - 8'd1);
            end
            if (src_we) begin
                h_src_count[rs_buf[0]] <= h_src_count[rs_buf[0]] + 8'd1;
                if (h_src_count[rs_buf[0]] + 8'd1 == h_src_need[rs_buf[0]])
                    h_src_done[rs_buf[0]] <= 1'b1;
            end
            if (dst_we) begin
                h_dst_count[rs_buf[0]] <= h_dst_count[rs_buf[0]] + 8'd1;
                if (h_dst_count[rs_buf[0]] + 8'd1 == h_beats[rs_buf[0]])
                    h_dst_done[rs_buf[0]] <= 1'b1;
            end
            if (row_start) q_rd <= ~q_rd;
            case ({sb_fire, row_start})
                2'b10: q_count <= q_count + 2'd1;
                2'b01: q_count <= q_count - 2'd1;
                default: q_count <= q_count;
            endcase
            if (last_out) occupied[r_half] <= 1'b0;
        end
    end

    // 段三 加工：七拍通路，0 地址 → 1 行缓冲读出 → 2 数据级 → 3 旋转/色键/a/掩码
    //   → 4~6 混合核 → 7 合并不写标记
    // 周期 2：数据级
    reg [127:0] r_cur, r_prev;  // 两拍窗口：r_prev 是前拍，r_cur 是本拍
    reg [95:0] r_dst;
    reg [7:0] r_k, r_k2;
    reg r_v1;
    always @(posedge clk) begin
        if (rst) begin
            r_cur <= 128'd0;
            r_prev <= 128'd0;
            r_dst <= 96'd0;
            r_k <= 8'd0;
            r_k2 <= 8'd0;
            r_v1 <= 1'b0;
        end else if (en) begin
            r_cur <= q_src;
            r_prev <= r_cur;
            r_dst <= q_dst;
            r_k <= cnt_k;
            r_k2 <= r_k;
            r_v1 <= running;
        end
    end

    // 周期 3：准备计算核心的五个输入
    wire [31:0] w [0:7];  // 源像素
    assign w[0] = r_prev[31:0];
    assign w[1] = r_prev[63:32];
    assign w[2] = r_prev[95:64];
    assign w[3] = r_prev[127:96];
    assign w[4] = r_cur[31:0];
    assign w[5] = r_cur[63:32];
    assign w[6] = r_cur[95:64];
    assign w[7] = r_cur[127:96];

    // 旋转：按 st 从窗口里取连续 4 个，可跨两格
    wire [31:0] px0 = (st_use == 3'd0) ? w[0] : (st_use == 3'd1) ? w[1] : (st_use == 3'd2) ? w[2] : w[3];
    wire [31:0] px1 = (st_use == 3'd0) ? w[1] : (st_use == 3'd1) ? w[2] : (st_use == 3'd2) ? w[3] : w[4];
    wire [31:0] px2 = (st_use == 3'd0) ? w[2] : (st_use == 3'd1) ? w[3] : (st_use == 3'd2) ? w[4] : w[5];
    wire [31:0] px3 = (st_use == 3'd0) ? w[3] : (st_use == 3'd1) ? w[4] : (st_use == 3'd2) ? w[5] : w[6];

    // 源操作数：BLIT 取源块旋转结果，FILL 取命令常量字
    wire [31:0] s0 = r_blit ? px0 : r_w4;
    wire [31:0] s1 = r_blit ? px1 : r_w4;
    wire [31:0] s2 = r_blit ? px2 : r_w4;
    wire [31:0] s3 = r_blit ? px3 : r_w4;

`ifdef INJ_SKIP_KEY // 注错：跳过色键，命中的像素照写
    wire [3:0] keys = 4'd0;
`else    // 色键：源字整字比较，FILL 恒不命中
    `ifdef GFX_KEY
        wire [3:0] keys = r_blit && r_key_en ? {s3 == r_key, s2 == r_key, s1 == r_key, s0 == r_key} : 4'd0;
    `else    // 无色键：比较链与跳过计数不在编译输入里
        wire [3:0] keys = 4'd0;
    `endif
`endif

    // alpha：XRGB 源取 255，其余取源字高字节；FILL 落在后一支（规范 §4.2）
    wire [7:0] a0 = (r_blit && r_sfmt == FMT_XRGB) ? 8'hff : s0[31:24];
    wire [7:0] a1 = (r_blit && r_sfmt == FMT_XRGB) ? 8'hff : s1[31:24];
    wire [7:0] a2 = (r_blit && r_sfmt == FMT_XRGB) ? 8'hff : s2[31:24];
    wire [7:0] a3 = (r_blit && r_sfmt == FMT_XRGB) ? 8'hff : s3[31:24];
    wire [3:0] mask_now;

`ifdef INJ_DROP_MASK // 注错：丢首拍掩码
    assign mask_now = (r_k2 == 8'd0) ? 4'hf : (r_k2 == (r_beats - 8'd1)) ? r_last_mask : 4'hf;
`else      // 掩码：本拍哪些槽位属于本行，按拍号在首拍/末拍/满拍之间选
    assign mask_now = (r_k2 == 8'd0) ? r_first_mask : (r_k2 == (r_beats - 8'd1)) ? r_last_mask : 4'hf;
`endif

    // 核输入寄存器：旋转与色键的组合逻辑止于此，不进核第一级
    reg [127:0] r_px;
    reg [95:0] r_dst2;
    reg [31:0] r_a;
    reg [3:0] r_keys, r_mask;
    reg r_v2;
    always @(posedge clk) begin
        if (rst) begin
            r_px <= 128'd0;
            r_dst2 <= 96'd0;
            r_a <= 32'd0;
            r_keys <= 4'd0;
            r_mask <= 4'd0;
            r_v2 <= 1'b0;
        end else if (en) begin
            r_px <= {s3, s2, s1, s0};
            r_dst2 <= r_read_dst ? r_dst : 96'd0;
            r_a <= {a3, a2, a1, a0};
            r_keys <= keys;
            r_mask <= mask_now;
            r_v2 <= r_v1;
        end
    end

    // 周期 4~6：混合计算
    wire [95:0] color;
    genvar p, ch;
    generate  // 混合阵列：4 像素 × 3 通道，源按 32 位、结果按 24 位打包
        for (p = 0; p < 4; p = p + 1) begin : pixel
            for (ch = 0; ch < 3; ch = ch + 1) begin : channel
                engine_blend u_blend (
                    .clk(clk),
                    .en(en_core),
                    .src(r_px[p*32+ch*8 +: 8]),
                    .dst(r_dst2[p*24+ch*8 +: 8]),
                    .a(r_a[p*8 +: 8]),
                    .c(r_c),
                    .eq(r_eq),
                    .out(color[p*24+ch*8 +: 8]));
            end
        end
    endgenerate

    // 周期 4~6 的延迟像：色键、掩码与 valid 与混合核的三级寄存器逐级对齐
`ifdef GFX_KEY
    reg [3:0] key_d1, key_d2, key_d3, mask_d1, mask_d2, mask_d3;
`else
    reg [3:0] mask_d1, mask_d2, mask_d3;
`endif
    reg v_d1, v_d2, v_d3, v_d4;
    always @(posedge clk) begin
        if (rst) begin

`ifdef GFX_KEY
            key_d1 <= 0;
            key_d2 <= 0;
            key_d3 <= 0;
`endif

            mask_d1 <= 0;
            mask_d2 <= 0;
            mask_d3 <= 0;

            v_d1 <= 0;
            v_d2 <= 0;
            v_d3 <= 0;
            v_d4 <= 0;
        end else if (en) begin

`ifdef GFX_KEY
            key_d1 <= r_keys;
            key_d2 <= key_d1;
            key_d3 <= key_d2;
`endif

            mask_d1 <= r_mask;
            mask_d2 <= mask_d1;
            mask_d3 <= mask_d2;

            v_d1 <= r_v2;
            v_d2 <= v_d1;
            v_d3 <= v_d2;
            v_d4 <= v_d3;
        end
    end

    // 周期 7：合并「不写」标记，输出 X 字节只有标记位
`ifdef GFX_KEY
    wire [3:0] no_write = key_d3 | ~mask_d3;
`else
    wire [3:0] no_write = ~mask_d3;
`endif
    // 计数掩码与 ws_tdata 同拍寄存，否则握手时看到的是下一拍
`ifdef GFX_KEY
    reg [3:0] ws_key_lanes;
`else
    reg [3:0] ws_key_lanes;
`endif
    assign perf_key_lanes = ws_key_lanes;
    assign perf_key_valid = ws_tvalid && ws_tready;
    wire [127:0] next_data = {
        (no_write[3] ? 8'h80 : 8'h00), color[95:72],
        (no_write[2] ? 8'h80 : 8'h00), color[71:48],
        (no_write[1] ? 8'h80 : 8'h00), color[47:24],
        (no_write[0] ? 8'h80 : 8'h00), color[23:0]};
    always @(posedge clk) begin
        if (rst) begin
            ws_tdata <= 128'd0; ws_tvalid <= 1'b0;
`ifdef GFX_KEY
            ws_key_lanes <= 4'd0;
`endif
        end else if (en) begin
            ws_tdata <= next_data; ws_tvalid <= v_d4;
`ifdef GFX_KEY
            ws_key_lanes <= key_d3 & mask_d3;
`else
            ws_key_lanes <= 4'd0;
`endif
        end
    end

    // 段四 节拍：行状态机，发完等 8 拍排空再回 PS_IDLE（下一行参数寄存器要等它），
    //   另一半读入照常
    always @(posedge clk) begin
        if (rst) begin
            pst <= PS_IDLE;
            cnt_k <= 8'd0;
            drain_count <= 8'd0;
            running <= 1'b0;
            r_blit <= 0;
            r_key_en <= 0;
            r_read_dst <= 0;
            r_eq <= 0;
            r_key <= 0;
            r_w4 <= 0;
            r_c <= 0;
            r_sfmt <= 0;
            r_ls <= 0;
            r_ld <= 0;
            r_half <= 0;
            r_beats <= 0;
            r_first_mask <= 0;
            r_last_mask <= 0;
        end else if (en) begin
            case (pst)
                PS_IDLE: if (row_start) begin
                    r_half <= next_half;
                    r_blit <= h_blit[next_half];
                    r_key_en <= h_key_en[next_half];
                    r_read_dst <= h_read_dst[next_half];
                    r_eq <= h_eq[next_half];
                    r_key <= h_key[next_half];
                    r_w4 <= h_w4[next_half];
                    r_c <= h_c[next_half];
                    r_sfmt <= h_sfmt[next_half];
                    r_ls <= h_ls[next_half];
                    r_ld <= h_ld[next_half];
                    r_beats <= h_beats[next_half];
                    r_first_mask <= h_first_mask[next_half];
                    r_last_mask <= h_last_mask[next_half];
                    cnt_k <= 8'd0;
                    // off = 1 的行先预热一拍
                    if (h_ls[next_half] >= h_ld[next_half]) begin
                        running <= 1'b0;
                        pst <= PS_PRE;
                    end else begin
                        running <= 1'b1;
                        pst <= PS_RUN;
                    end
                end
                PS_PRE: begin
                    cnt_k <= 8'd0;
                    running <= 1'b1;
                    pst <= PS_RUN;
                end
                PS_RUN: begin
                    if (cnt_k + 8'd1 >= r_beats) begin
                        cnt_k <= 8'd0;
                        running <= 1'b0;
                        drain_count <= 8'd7;
                        pst <= PS_DRAIN;
                    end else cnt_k <= cnt_k + 8'd1;
                end
                PS_DRAIN: begin
                    if (drain_count == 8'd0) pst <= PS_IDLE;
                    else drain_count <= drain_count - 8'd1;
                end
                default: pst <= PS_IDLE;
            endcase
        end
    end
endmodule

`default_nettype wire
