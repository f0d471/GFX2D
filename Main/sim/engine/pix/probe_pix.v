// probe_pix：计算阵列与片上缓存前期研究的探针，**不是入库实现**
//
// 回答研究清单的五问里能在仿真与综合台上量的三件：
//   第 1 问 旋转器与首末拍：两拍窗口、4 个位置的像素旋转怎么写；
//            首末拍「不属于本行的像素」的掩码怎么打（X 字节不写标记的像素一侧）。
//   第 3 问 流水线的拍对齐：源拍、目的拍、色键结果与混合核的 L=3 怎么对齐，
//            出口怎么保证每拍一拍（判据「两行在片上之后写流不断拍」）。
//   第 5 问 实验：行缓冲 + 旋转器 + 一条 4 像素通路（12 个 engine_blend）单独综合，
//            RAM 的 14 块对账、100 MHz 余量（配套 syn/ 下的 Efinity 小工程）。
//
// 第 2 问（行缓冲的乒乓与空闲握手）的结论在注释与开发日志里：
//   Efinity 的 RAM 读、写口都是寄存的（efinity-synthesis.pdf「RAM」章），
//   本文件的 probe_pix_linebuf 用最普通的推断写法（读写分立 always、非阻塞），
//   读延迟恒 1 拍；「这一半空出来」的握手：row_free 的释放时机是
//   这一半的写流末拍被写核收走那一拍。探针不含这段握手。
//
// 写流的反压（2026-10-01 复核补的）：写核 axi_dma_wr 的 s_axis_write_data_tready 是寄存器，
//   写核在 STATE_START 里看到 tvalid 才发 AW、下一拍才拉高 tready（axi_dma_wr.v:509、515），
//   所以每个写描述符的第一拍至少要等一拍；输出 FIFO 半满（16 拍）也拉低（axi_dma_wr.v:905），
//   所以像素通路必须看 ws_tready：整条流水共用一个时钟使能 en = ws_tready || !ws_tvalid，
//   行缓冲读口、各级寄存器、12 个 engine_blend 的 en（研究契约留的口）、行状态机都挂在它上面。
//
// 四个模块：
//   probe_pix_linebuf  128 位 × 512 的简单双口 RAM，位 [8] 选两行乒乓的半；
//                      深度 256 与 512 占同样的 7 块（efx_map_ramest 实测），取 512。
//                      读口带读使能 re（流水停顿时读出寄存器保持）。
//   probe_pix_rot      纯组合的两拍窗口旋转器。窗口 = {prev, cur}，4 个位置：
//                      off  = (ls >= ld) ? 1 : 0   读地址超前量
//                      st   = (ls >= ld) ? ls-ld : ls-ld+4   窗口内起始槽（0~3）
//                      px[t] = 窗口槽 [st+t]，t = 0..3 对齐到目的拍槽 t。
//                      摊开写就是 4 个 32 位 4:1 mux，推导与 Amiga blitter
//                      的桶形移位同源。
//   probe_pix_mask     纯组合的首末拍掩码：目的拍 k 的槽 t 有效
//                      ⟺ 0 ≤ 4k − ld + t < npx。行外像素打「不写」。
//   probe_pix_path     一行的数据通路：两块行缓冲、旋转、色键、XRGB 的 a、
//                      12 个 rtl/engine/engine_blend.v、标记合并、写流（含 tlast）。
//                      行级驱动简化为 start 脉冲 + 行参数（数据由测试台预填进 RAM），
//                      不含读流写入与 row_free 的完整握手——那是入库实现的事。
// 单独综合的顶层在 syn/syn_probe_pix.v（LFSR 驱动全部输入，含 ws_tready）。
//
// 探针只回答上面三问；入库依据在 rtl/engine/engine_pix.v 与同目录 README。
`timescale 1ns/1ps
`default_nettype none

// ---------------------------------------------------------------------------
// 128 位 × 512 简单双口 RAM，两行乒乓。位 [8] 是半号：半 0 = 行偶数，半 1 = 行奇数。
// 推断写法：读写分立 always、非阻塞、同步读（读延迟 1 拍），读口带读使能。
// Efinity 2026.1 映射为 6 块 512×20 + 1 块 1024×8，共 7 块（efx_map_ramest 与 syn 实测）。
// ---------------------------------------------------------------------------
module probe_pix_linebuf (
    input  wire         clk,
    input  wire         we,
    input  wire [8:0]   waddr,          // {半, 半内拍地址}
    input  wire [127:0] wdata,
    input  wire         re,             // 读使能：流水停顿时为 0，读出寄存器保持
    input  wire [8:0]   raddr,
    output reg  [127:0] rdata
);
    reg [127:0] mem [0:511];

    always @(posedge clk) if (we) mem[waddr] <= wdata;
    always @(posedge clk) if (re) rdata <= mem[raddr];
endmodule

// ---------------------------------------------------------------------------
// 两拍窗口、4 个位置的像素旋转器。纯组合。
//   ls、ld  源、目的行首地址拍内相位 lead（边带）
//   prev、cur  源行缓冲连续两拍的读出（cur 的地址 = 目的拍号 + off）
//   px[0..3]   已对齐到目的拍槽位的 4 个源像素字（32 位原样，不做字节重排）
// off 与 st 的公式见文件头；px[t] = 窗口 8 槽 {prev[0..3], cur[0..3]} 的 [st+t]。
// 槽 0 在 32 位低位（小端，低地址像素在低位，设计文档 3.5 节 *7）。
// ---------------------------------------------------------------------------
module probe_pix_rot #(
    parameter integer INJECT = 0        // 位 0：st 加 1（旋转错一个位置）
)(
    input  wire [1:0]  ls,
    input  wire [1:0]  ld,
    input  wire [127:0] prev,
    input  wire [127:0] cur,
    output wire [1:0]  off,             // 读地址超前量（给地址发生器）
    output wire [1:0]  st,              // 窗口内起始槽 0~3
    output wire [31:0] px0, px1, px2, px3
);
    wire ge = (ls >= ld);
    assign off = ge ? 2'd1 : 2'd0;
    // ls < ld 时 st = ls - ld + 4（借位用 3 位算，2 位的 4 会回绕成 0）；
    // ls >= ld 时 st = ls - ld。均落在 0~3。
    wire [2:0] st_raw = ge ? ({1'b0, ls} - {1'b0, ld})
                           : ({1'b0, ls} + 3'd4 - {1'b0, ld});
    wire [2:0] st_use = (INJECT & 1) ? (st_raw + 3'd1) : st_raw;

    wire [31:0] w [0:7];
    assign w[0] = prev[31:0];   assign w[1] = prev[63:32];
    assign w[2] = prev[95:64];  assign w[3] = prev[127:96];
    assign w[4] = cur[31:0];    assign w[5] = cur[63:32];
    assign w[6] = cur[95:64];   assign w[7] = cur[127:96];

    assign st  = st_use[1:0];
    // 4 个像素 × 4 个候选起点，纯 4:1 mux
    assign px0 = (st_use == 3'd0) ? w[0] : (st_use == 3'd1) ? w[1] :
                 (st_use == 3'd2) ? w[2] : w[3];
    assign px1 = (st_use == 3'd0) ? w[1] : (st_use == 3'd1) ? w[2] :
                 (st_use == 3'd2) ? w[3] : w[4];
    assign px2 = (st_use == 3'd0) ? w[2] : (st_use == 3'd1) ? w[3] :
                 (st_use == 3'd2) ? w[4] : w[5];
    assign px3 = (st_use == 3'd0) ? w[3] : (st_use == 3'd1) ? w[4] :
                 (st_use == 3'd2) ? w[5] : w[6];
endmodule

// ---------------------------------------------------------------------------
// 首末拍掩码（X 字节不写标记的像素一侧）：目的拍 k 的槽 t 属于本行 ⟺ 0 ≤ 4k − ld + t < npx。
// 行外像素的源槽内容是覆盖区间带出的相邻像素，读了但标记「不写」。
// 纯组合；入库实现里在边带级预计算首末两掩码、按拍号二选一，这里直接逐槽比较，
// 两者等价（首拍只有头部行外、末拍只有尾部行外，中间拍恒满拍）。
// ---------------------------------------------------------------------------
module probe_pix_mask #(
    parameter integer INJECT = 0        // 位 0：首拍掩码丢掉（行首行外像素照样写）
)(
    input  wire [1:0]  ld,
    input  wire [10:0] npx,
    input  wire [7:0]  k,
    output wire [3:0]  mask             // 1 = 本行像素（写），0 = 行外（不写）
);
    // 目的拍 k 槽 t 的像素号 j = 4k + t − ld：行外 ⟺ j < 0（首拍头部）或 j ≥ npx（末拍尾部）。
    // 4k+t ≤ 4·255+3 = 1023、ld+npx ≤ 2047，12 位无符号比较不回绕。
    wire [11:0] low  = {10'b0, ld};                      // j ≥ 0 的界：4k+t ≥ ld
    wire [11:0] high = {1'b0, ld} + {1'b0, npx};         // j < npx 的界：4k+t < ld+npx
    wire [11:0] j0 = {4'b0, k, 2'b00};
    wire [3:0] m;
    assign m[0] = ((j0 + 12'd0) >= low) && ((j0 + 12'd0) < high);
    assign m[1] = ((j0 + 12'd1) >= low) && ((j0 + 12'd1) < high);
    assign m[2] = ((j0 + 12'd2) >= low) && ((j0 + 12'd2) < high);
    assign m[3] = ((j0 + 12'd3) >= low) && ((j0 + 12'd3) < high);
    assign mask = ((INJECT & 1) != 0 && k == 8'd0) ? 4'b1111 : m;
    // 注错：首拍掩码丢掉（行首行外像素照样写）。见红要用 ld>0 或 npx+ld≤4 的用例。
endmodule

// ---------------------------------------------------------------------------
// 一行的数据通路：行缓冲 ×2 → 旋转 → 色键 → XRGB 的 a → 12 个 engine_blend →
// 标记合并 → 写流。研究与综合共用；例化真核 rtl/engine/engine_blend.v（L=3 与正式一致）。
//
// 拍对齐（研究第 3 问的答案，周期编号从「地址寄存器输出第 0 拍地址」起）：
//   周期 0   地址发生器：源地址 = 半基 + k + off，目的地址 = 半基 + k
//   周期 1   两块 RAM 的输出寄存器出数：q_src(k+off)、q_dst(k)
//   周期 2   r_cur ← q_src、r_prev ← 旧 r_cur、r_dst ← q_dst、r_k ← k（数据级）
//   周期 3   旋转（{r_prev, r_cur} → px）、色键比较、a 的选择组合后落寄存器：
//            r_px0..3、r_a、r_key、r_mask；r_dst2 ← r_dst（与 r_px 同拍进 blend）
//   周期 4~6 engine_blend 的三级：r_p → r_x → out
//   周期 7   标记合并（行外 | 色键命中 → X 字节 bit7）落写流寄存器 ws_*
//   延迟合计 7 拍；地址发生器行内无气泡 → 下游 ready 时写流每拍一拍。
//   实测 PIX_LAT 4 与 12 每颗弹幕周期数完全相同，这一拍两拍的延迟
//   被「写与下一行的读并行」盖住，不为省它把 dst 接成 RAM 输出直驱。
//
// 停顿：上面的「周期」都按 en 计。en = ws_tready || !ws_tvalid，en 为 0 的那一拍
//   所有级（地址、行缓冲读出、数据级、旋转级、blend 三级、标记与 valid 的延迟像、
//   写流寄存器、行状态机）一起保持；写流的 tvalid 在停顿期间保持为 1，不出气泡。
//
// 行驱动（研究简化）：start 一拍 + 行参数；源、目的两行假定已预填进第 half 半。
// tlast 按 tb 给的段拍数打（一行最多两段）：每段最后一拍 tlast。
// 这只是为了让测试台把写流直接接到写核上。边带只有本行写拍数之和（sb_beats），没有段界，
// 所以定案改由搬运层按写描述符拍数打 tlast，入库的 engine_pix 不出 tlast。
// ---------------------------------------------------------------------------
module probe_pix_path #(
    // 位 0 旋转错位、位 1 跳过色键、位 2 丢首末拍掩码、
    // 位 3 不看 ws_tready（en 恒 1）、位 4 只有 12 个混合核不停（blend 的 en 恒 1）
    parameter integer INJECT = 0
)(
    input  wire        clk,
    input  wire        rst,

    // 行参数（start 那拍有效）
    input  wire        start,
    input  wire [1:0]  half,           // 本行用的行缓冲半（rd_buf 的位 0）
    input  wire        is_blit,        // 0 FILL（源 = w4 常量）、1 BLIT
    input  wire [2:0]  eq,
    input  wire        key_en,
    input  wire [31:0] key_w5,
    input  wire [31:0] w4,
    input  wire [7:0]  c,
    input  wire [1:0]  sfmt,           // 0 XRGB8888、1 ARGB8888_PM、2 ARGB8888_STRAIGHT
    input  wire        read_dst,       // 读目的：方程不是 COPY
    input  wire [10:0] npx,
    input  wire [1:0]  lead_src,
    input  wire [1:0]  lead_dst,
    input  wire [7:0]  seg1_beats,     // 写流第 1 段拍数（tlast 位置）
    input  wire [7:0]  seg2_beats,     // 第 2 段拍数（0 = 不拆段）

    // 行缓冲预填写口（研究用；入库实现换成读流的写入）
    input  wire        lb_we,
    input  wire        lb_src_sel,     // 预填目标：0 源块、1 目的块
    input  wire [8:0]  lb_waddr,
    input  wire [127:0] lb_wdata,

    // 写流（与正式写流同形）
    output reg  [127:0] ws_tdata,      // 每像素 X 字节 bit7 = 1 表示不写
    output reg  [15:0]  ws_tkeep,
    output reg          ws_tlast,
    output reg          ws_tvalid,
    input  wire         ws_tready,     // 写核的 s_axis_write_data_tready，每个描述符首拍必为 0

    output wire        busy,
    output wire [1:0]  dbg_off,
    output wire [1:0]  dbg_st
);
    localparam [2:0] FMT_XRGB = 3'd0, FMT_PM = 3'd1, FMT_ST = 3'd2;

    // ---------------------------------------------------------------- 流水的时钟使能
    // 写流寄存器空着或下游收走这一拍时整条流水前进一拍。
    wire en      = ((INJECT & 8) != 0) ? 1'b1 : (ws_tready || !ws_tvalid);
    wire en_core = ((INJECT & 16) != 0) ? 1'b1 : en;

    // ---------------------------------------------------------------- 行缓冲
    wire [127:0] q_src, q_dst;
    probe_pix_linebuf u_lbs (
        .clk(clk), .we(lb_we && !lb_src_sel), .waddr(lb_waddr), .wdata(lb_wdata),
        .re(en), .raddr(rd_addr_src), .rdata(q_src));
    probe_pix_linebuf u_lbd (
        .clk(clk), .we(lb_we && lb_src_sel), .waddr(lb_waddr), .wdata(lb_wdata),
        .re(en), .raddr(rd_addr_dst), .rdata(q_dst));

    // ---------------------------------------------------------------- 行参数寄存
    reg        r_is_blit, r_key_en, r_read_dst;
    reg [2:0]  r_eq;
    reg [31:0] r_key, r_w4;
    reg [7:0]  r_c;
    reg [1:0]  r_sfmt, r_ls, r_ld, r_half;
    reg [10:0] r_npx;
    reg [7:0]  r_seg1, r_seg2;

    // ---------------------------------------------------------------- 地址发生器
    // 一行连发 dst_beats 拍：源地址 = 半基 + k + off，目的 = 半基 + k。
    // off=1 时窗口是「前拍=beat k、当前拍=beat k+1」，第 0 拍的 prev 也要有真数据，
    // 所以 RUN 之前先走一拍 PS_PRE 把地址 0 压进读口（否则第 0 拍的 prev 是行外旧值，
    // st>0 时真实像素会经过它）。off=0 时窗口是「前拍=beat k−1、当前拍=beat k」，
    // 第 0 拍的 prev 只喂行外槽（掩码掉），不需要预热。
    // 地址 = k+1 ≤ beats ≤ 162 < 256，不会越过半界；越界槽对应的像素必行外，读到旧数据无妨。
    wire [1:0] rot_off, rot_st;
    probe_pix_rot rot_calc (.ls(r_ls), .ld(r_ld), .prev(128'd0), .cur(128'd0),
                            .off(rot_off), .st(rot_st),
                            .px0(), .px1(), .px2(), .px3());
    assign dbg_off = rot_off;
    assign dbg_st  = rot_st;

    reg [7:0] cnt_k;                    // 0..dst_beats-1
    reg [8:0] rd_addr_src, rd_addr_dst;
    // 12 位中间量 + 显式切片：npx 的 LFSR 会给满 11 位，中间量按最大位宽算、
    // 取位用切片，Efinity 就不发位宽截断告警。边带真实上限 npx=640 → beats ≤ 161。
    wire [11:0] beats_w = {1'b0, r_npx} + {9'b0, r_ld} + 12'd3;
    wire [9:0]  dst_beats = beats_w[11:2];               // ceil((ld+npx)/4)
    wire [7:0]  src_k8 = cnt_k + {6'b0, rot_off};        // 半内拍地址（行内 ≤ 162，不会出半）
    reg         running;

    assign busy = running;
    always @* begin
        rd_addr_src = (pst == PS_PRE) ? {r_half[0], 8'd0}
                                      : {r_half[0], src_k8};
        rd_addr_dst = {r_half[0], cnt_k};
    end

    // ---------------------------------------------------------------- 数据级与旋转级
    reg [127:0] r_cur, r_prev, r_dst;
    reg [7:0]   r_k;
    reg [7:0]   r_k2;                   // 窗口数据那一级的拍号（r_k 距 cnt_k 一级，
                                    // 窗口距 cnt_k 两级——RAM 输出寄存器多一级，
                                    // 掩码必须吃 r_k2 才与 r_px 同拍；差一级 = 掩码提前一拍）
    reg         r_v1;                   // RAM 数据有效（cnt_k 的延迟像）

    always @(posedge clk) begin
        if (rst) begin
            r_cur <= 128'd0; r_prev <= 128'd0; r_dst <= 128'd0;
            r_k <= 8'd0; r_k2 <= 8'd0; r_v1 <= 1'b0;
        end else if (en) begin
            r_cur  <= q_src;
            r_prev <= r_cur;
            r_dst  <= q_dst;
            r_k    <= cnt_k;
            r_k2   <= r_k;
            r_v1   <= running;          // 下一拍的 q_* 属于第 cnt_k 拍
        end
    end

    // 旋转、色键、a 选择（周期 3 的输入，组合）
    wire [31:0] px0, px1, px2, px3;
    probe_pix_rot #(.INJECT((INJECT & 1) != 0 ? 1 : 0)) u_rot (
        .ls(r_ls), .ld(r_ld), .prev(r_prev), .cur(r_cur),
        .off(), .st(), .px0(px0), .px1(px1), .px2(px2), .px3(px3));

    // FILL 的源是常量 w4：四个槽同值，不读源块、不旋转
    wire [31:0] s0 = r_is_blit ? px0 : r_w4;
    wire [31:0] s1 = r_is_blit ? px1 : r_w4;
    wire [31:0] s2 = r_is_blit ? px2 : r_w4;
    wire [31:0] s3 = r_is_blit ? px3 : r_w4;

    // 色键：比较源的原始存储字；FILL 无色键
    wire k0 = r_is_blit && r_key_en && (((INJECT & 2) == 0) ? (s0 == r_key) : 1'b0);
    wire k1 = r_is_blit && r_key_en && (((INJECT & 2) == 0) ? (s1 == r_key) : 1'b0);
    wire k2 = r_is_blit && r_key_en && (((INJECT & 2) == 0) ? (s2 == r_key) : 1'b0);
    wire k3 = r_is_blit && r_key_en && (((INJECT & 2) == 0) ? (s3 == r_key) : 1'b0);

    // XRGB 源的有效 alpha 是 255（spec *4）
    wire [7:0] a0 = (r_sfmt == FMT_XRGB) ? 8'd255 : s0[31:24];
    wire [7:0] a1 = (r_sfmt == FMT_XRGB) ? 8'd255 : s1[31:24];
    wire [7:0] a2 = (r_sfmt == FMT_XRGB) ? 8'd255 : s2[31:24];
    wire [7:0] a3 = (r_sfmt == FMT_XRGB) ? 8'd255 : s3[31:24];

    // 首末拍掩码
    wire [3:0] mask_raw;
    probe_pix_mask #(.INJECT((INJECT & 4) != 0 ? 1 : 0)) u_mask (
        .ld(r_ld), .npx(r_npx), .k(r_k2), .mask(mask_raw));

    // blend 级（寄存器直驱）
    reg [31:0] r_px0, r_px1, r_px2, r_px3;
    reg [7:0]  r_a0, r_a1, r_a2, r_a3;
    reg [127:0] r_dst2;
    reg [3:0]  r_keys, r_maskk;
    reg        r_v2;

    always @(posedge clk) begin
        if (rst) begin
            r_px0 <= 0; r_px1 <= 0; r_px2 <= 0; r_px3 <= 0;
            r_a0 <= 0; r_a1 <= 0; r_a2 <= 0; r_a3 <= 0;
            r_dst2 <= 0; r_keys <= 0; r_maskk <= 0; r_v2 <= 1'b0;
        end else if (en) begin
            r_px0 <= s0; r_px1 <= s1; r_px2 <= s2; r_px3 <= s3;
            r_a0 <= a0;   r_a1 <= a1;   r_a2 <= a2;   r_a3 <= a3;
            r_dst2 <= r_dst;
            r_keys <= {k3, k2, k1, k0};
            r_maskk <= mask_raw;
            r_v2 <= r_v1;
        end
    end

    // 不读目的时 dst 喂 0：只有 COPY 不看 dst，MUL 属非 COPY 必然在读
    wire [31:0] d0 = r_read_dst ? r_dst2[31:0]    : 32'd0;
    wire [31:0] d1 = r_read_dst ? r_dst2[63:32]   : 32'd0;
    wire [31:0] d2 = r_read_dst ? r_dst2[95:64]   : 32'd0;
    wire [31:0] d3 = r_read_dst ? r_dst2[127:96]  : 32'd0;

    // 12 个混合核：4 像素 × B/G/R（en 接整条流水的时钟使能；输入全寄存器驱动）
    wire [7:0] ob0, og0, or0, ob1, og1, or1, ob2, og2, or2, ob3, og3, or3;
    engine_blend u_b0 (.clk(clk), .en(en_core), .src(r_px0[7:0]),   .dst(d0[7:0]),   .a(r_a0), .c(r_c), .eq(r_eq), .out(ob0));
    engine_blend u_g0 (.clk(clk), .en(en_core), .src(r_px0[15:8]),  .dst(d0[15:8]),  .a(r_a0), .c(r_c), .eq(r_eq), .out(og0));
    engine_blend u_r0 (.clk(clk), .en(en_core), .src(r_px0[23:16]), .dst(d0[23:16]), .a(r_a0), .c(r_c), .eq(r_eq), .out(or0));
    engine_blend u_b1 (.clk(clk), .en(en_core), .src(r_px1[7:0]),   .dst(d1[7:0]),   .a(r_a1), .c(r_c), .eq(r_eq), .out(ob1));
    engine_blend u_g1 (.clk(clk), .en(en_core), .src(r_px1[15:8]),  .dst(d1[15:8]),  .a(r_a1), .c(r_c), .eq(r_eq), .out(og1));
    engine_blend u_r1 (.clk(clk), .en(en_core), .src(r_px1[23:16]), .dst(d1[23:16]), .a(r_a1), .c(r_c), .eq(r_eq), .out(or1));
    engine_blend u_b2 (.clk(clk), .en(en_core), .src(r_px2[7:0]),   .dst(d2[7:0]),   .a(r_a2), .c(r_c), .eq(r_eq), .out(ob2));
    engine_blend u_g2 (.clk(clk), .en(en_core), .src(r_px2[15:8]),  .dst(d2[15:8]),  .a(r_a2), .c(r_c), .eq(r_eq), .out(og2));
    engine_blend u_r2 (.clk(clk), .en(en_core), .src(r_px2[23:16]), .dst(d2[23:16]), .a(r_a2), .c(r_c), .eq(r_eq), .out(or2));
    engine_blend u_b3 (.clk(clk), .en(en_core), .src(r_px3[7:0]),   .dst(d3[7:0]),   .a(r_a3), .c(r_c), .eq(r_eq), .out(ob3));
    engine_blend u_g3 (.clk(clk), .en(en_core), .src(r_px3[15:8]),  .dst(d3[15:8]),  .a(r_a3), .c(r_c), .eq(r_eq), .out(og3));
    engine_blend u_r3 (.clk(clk), .en(en_core), .src(r_px3[23:16]), .dst(d3[23:16]), .a(r_a3), .c(r_c), .eq(r_eq), .out(or3));

    // ---------------------------------------------------------------- 输出级
    // 标记与 valid 的延迟像，逐级贴着数据链：
    //   数据链（有效周期）：q(1) → r_cur/r_dst(2) → r_px(3) → blend r_p(4) → r_x(5) → out(6) → ws(7)
    //   keys/mask：r_keys(3) → k_d1(4) → k_d2(5) → k_d3(6)   与 out 同拍
    //   拍号 k：  r_k(1) → k_d1k(2) → … → k_d5k(6)           与 v_d4 同拍（差一级都会错位）
    //   valid：   r_v1(1) → r_v2(2) → v_d1(3) → v_d2(4) → v_d3(5) → v_d4(6) → ws_tvalid(7)
    reg [3:0] k_d1, k_d2, k_d3, m_d1, m_d2, m_d3;
    reg       v_d1, v_d2, v_d3, v_d4;
    reg [7:0] k_d1k, k_d2k, k_d3k, k_d4k, k_d5k;
    always @(posedge clk) begin
        if (rst) begin
            k_d1 <= 0; k_d2 <= 0; k_d3 <= 0; m_d1 <= 0; m_d2 <= 0; m_d3 <= 0;
            v_d1 <= 0; v_d2 <= 0; v_d3 <= 0; v_d4 <= 0;
            k_d1k <= 0; k_d2k <= 0; k_d3k <= 0; k_d4k <= 0; k_d5k <= 0;
        end else if (en) begin
            k_d1 <= r_keys;   k_d2 <= k_d1;   k_d3 <= k_d2;
            m_d1 <= r_maskk;  m_d2 <= m_d1;   m_d3 <= m_d2;
            v_d1 <= r_v2;     v_d2 <= v_d1;   v_d3 <= v_d2;   v_d4 <= v_d3;
            k_d1k <= r_k;     k_d2k <= k_d1k; k_d3k <= k_d2k; k_d4k <= k_d3k; k_d5k <= k_d4k;
        end
    end

    // 不写标记：色键命中 | 行外（X 字节最高位；XRGB 写回高字节本就强制 0）
    wire [3:0] nowrite = k_d3 | ~m_d3;
    // tlast：段 1 的末拍（k = seg1-1）、段 2 的末拍（k = seg1+seg2-1）
    wire last_s1 = (k_d5k == (r_seg1 - 8'd1));
    wire last_s2 = (r_seg2 != 8'd0) && (k_d5k == (r_seg1 + r_seg2 - 8'd1));

    // 像素字 0x00RRGGBB（小端，设计文档 3.5 节 *7）：X[31:24]（不写标记在 bit7）、
    // R[23:16]、G[15:8]、B[7:0]。XRGB 写回高字节本就强制 0，bit7 借作不写标记。
    wire [7:0] nw3 = nowrite[3] ? 8'h80 : 8'h00;
    wire [7:0] nw2 = nowrite[2] ? 8'h80 : 8'h00;
    wire [7:0] nw1 = nowrite[1] ? 8'h80 : 8'h00;
    wire [7:0] nw0 = nowrite[0] ? 8'h80 : 8'h00;
    wire [127:0] ws_d = { {nw3, or3[7:0], og3[7:0], ob3[7:0]},
                          {nw2, or2[7:0], og2[7:0], ob2[7:0]},
                          {nw1, or1[7:0], og1[7:0], ob1[7:0]},
                          {nw0, or0[7:0], og0[7:0], ob0[7:0]} };

    always @(posedge clk) begin
        if (rst) begin
            ws_tdata <= 128'd0; ws_tkeep <= 16'hFFFF; ws_tlast <= 1'b0; ws_tvalid <= 1'b0;
        end else if (en) begin
            ws_tdata  <= ws_d;
            ws_tkeep  <= 16'hFFFF;
            ws_tvalid <= v_d4;
            ws_tlast  <= v_d4 && (last_s1 || last_s2);
        end
    end

    // ---------------------------------------------------------------- 行状态机（研究简化）
    // RUN：地址发生器连发 dst_beats 拍（无气泡）；DRAIN：等 7 拍流水排空。
    // 整个状态机按 en 走：PRE 压进读口的地址 0 与 RUN 的每一拍地址都要等读口真的采到。
    // 回到 IDLE 时写流寄存器已空（DRAIN 按 en 数满 8 拍），en 为 1，start 不会被吞。
    localparam [1:0] PS_IDLE = 2'd0, PS_RUN = 2'd1, PS_DRAIN = 2'd2, PS_PRE = 2'd3;
    reg [1:0] pst;
    reg [7:0] drain_cnt;

    always @(posedge clk) begin
        if (rst) begin
            pst <= PS_IDLE; cnt_k <= 8'd0; running <= 1'b0; drain_cnt <= 8'd0;
            r_is_blit <= 0; r_key_en <= 0; r_read_dst <= 0; r_eq <= 0;
            r_key <= 0; r_w4 <= 0; r_c <= 0; r_sfmt <= 0; r_ls <= 0; r_ld <= 0;
            r_half <= 0; r_npx <= 0; r_seg1 <= 0; r_seg2 <= 0;
        end else if (en) begin
            case (pst)
            PS_IDLE: begin
                if (start) begin
                    r_is_blit <= is_blit; r_key_en <= key_en; r_read_dst <= read_dst;
                    r_eq <= eq; r_key <= key_w5; r_w4 <= w4; r_c <= c;
                    r_sfmt <= sfmt; r_ls <= lead_src; r_ld <= lead_dst;
                    r_half <= half; r_npx <= npx; r_seg1 <= seg1_beats; r_seg2 <= seg2_beats;
                    cnt_k <= 8'd0;
                    // off=1：先走一拍 PRE 把地址 0 压进读口，第 0 拍的窗口前拍才有真数据
                    //（PRE 拍 running=0，进 RUN 才拉起）；off=0 直接进 RUN 并拉起 running
                    if (lead_src >= lead_dst) begin
                        running <= 1'b0; pst <= PS_PRE;
                    end else begin
                        running <= 1'b1; pst <= PS_RUN;
                    end
                end
            end
            PS_PRE: begin
                cnt_k <= 8'd0; running <= 1'b1; pst <= PS_RUN;
            end
            PS_RUN: begin
                if (cnt_k + 8'd1 >= dst_beats) begin
                    cnt_k <= 8'd0; running <= 1'b0; drain_cnt <= 8'd7; pst <= PS_DRAIN;
                end else begin
                    cnt_k <= cnt_k + 8'd1;
                end
            end
            PS_DRAIN: begin
                if (drain_cnt == 8'd0) pst <= PS_IDLE;
                else drain_cnt <= drain_cnt - 8'd1;
            end
            default: pst <= PS_IDLE;
            endcase
        end
    end
endmodule

`default_nettype wire
