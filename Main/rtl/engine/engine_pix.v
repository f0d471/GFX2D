// engine_pix：引擎 P1 B4 的计算阵列与片上缓存
//
// 几何层每行先发一条边带（I5），搬运层随后把这一行的源、目的读回来（I3），
// 本层把两行放进片上，按边带逐拍算出 4 个像素，喂给搬运层的写流（I4）。
// 这一层只碰像素，不碰地址：它一错，错的是颜色、相位或「写不写」。
//
// 五件事：
//
//   1. 行缓冲（D23、D46）。源一块 512×128、目的一块 512×96，地址最高位选半，
//      每块两半乒乓，半内 256 拍，够一行最多 161 拍。目的像素的 X 字节不参与混合
//      也不写回，读写口都只存 RGB 三字节（96 位，5 块 RAM；128 位要 7 块）。
//      I3 的读流按 rs_buf = {源/目的, 半} 落进对应块的对应半，消费者恒 ready；
//      一行跨 4KB 时读流有两个 tlast，写指针在两段之间不清，只在该半的边带握手时清。
//
//   2. 两拍窗口的像素旋转（D15、D42）。描述符按 16 字节对齐发，源与目的的
//      行首拍内相位 ls、ld（I5 的 lead）不同时，用 {前拍, 当前拍} 八个像素的窗口
//      取 px[t] = 窗口[st + t] 把源拍对齐到目的拍：
//        off = (ls >= ld)，源读地址比目的拍号超前 off 拍；
//        st  = (ls - ld) mod 4。
//      off = 1 时第 0 拍的前拍也是行内像素，行首要先走一拍预热（PS_PRE，把地址 0
//      压进读口）；off = 0 时第 0 拍的前拍只喂行外槽，不用预热。
//
//   3. 首末拍掩码与色键（D16、D43）。目的拍 k 槽 t 的像素号 j = 4k + t - ld，
//      属于本行 ⟺ 0 <= j < npx。边带握手时预算出首拍、末拍两个 4 位掩码，
//      中间拍满拍；一拍的行把行尾也算进首拍掩码。色键比较源的原始 32 位存储字
//      （spec 第 01 章 3.5 节），只对 BLIT。行外或色键命中的槽在 X 字节最高位打 1，
//      搬运层据此清掉对应 4 字节的 wstrb。
//
//   4. 四像素通路。12 个 engine_blend（4 像素 × RGB），a 的来源：
//        BLIT 源格式为 XRGB 时取 255，其余格式取源字的高字节；
//        FILL 的常量源字按方程解释（规范条款 §4.2）：ALPHA 按预乘、ALPHA_STRAIGHT
//        按直通，alpha 都是 w4[31:24]，其余方程不看 a，所以一律取 w4[31:24]，
//        不看边带的 sfmt（FILL 没有源表面，I1 上的源格式对它无意义）。
//      不读目的的行（D7，只有 COPY）给混合核的目的通道送 0，COPY 本来就不看它。
//
//   5. 流水与反压（D44、D47）。延迟 7 拍，周期编号从地址发出那拍起：
//        0  地址：源 = {半, k + off}（PS_PRE 时为 0），目的 = {半, k}
//        1  行缓冲读出 q_src / q_dst（同步读，1 拍）
//        2  数据级 r_cur / r_prev / r_dst
//        3  旋转、色键、a 选择、掩码，落 r_px / r_a / r_keys / r_mask
//        4~6  engine_blend 三级（I6 的 L = 3）
//        7  合并「不写」标记，落 ws_tdata / ws_tvalid
//      色键、掩码、valid、拍号四条延迟像必须逐级贴着数据链，差一级就提前或落后一拍。
//      写核每个写描述符的首拍至少停一拍、输出 FIFO 半满也停，整条流水挂一个时钟使能
//      en = ws_tready || !ws_tvalid：行缓冲读使能、各级寄存器、12 个核的 en、
//      行状态机都按它走，停顿时 ws_tdata 与 ws_tvalid 保持。行内下游 ready 时不出气泡；
//      一行发完之后等 8 拍排空再开下一行，下一行的参数寄存器才换，
//      这段时间另一半的读入照常进行。
//
// 接口（总览 2.4 节 I3、I4、I5）：
//   I5 几何层 → 本层：每行一条边带，sb_valid/sb_ready 握手。sb_row 是命令内行号，
//      最低位就是这一行用的半（D41），每条命令的第 0 行都用第 0 半（D49）。
//   I5 的反向 row_free[1:0]：第 h 半的边带握手之后拉低，该半写流的末拍被写核收走
//      之后拉高（D45）。sb_ready 另外查这一半没被占、行队列没满，几何层照 D41
//      先查 row_free 再发边带时它恒为 1。
//   I3 搬运层 → 本层：读流与 rs_buf，rs_tready 恒 1（D23）。读流按对齐覆盖区间来，
//      tkeep 恒全 1、tlast 只标描述符边界，两者都不用。
//   I4 本层 → 搬运层：写流，tkeep 恒全 1，没有 tlast（D48：每个写描述符的末拍由
//      搬运层按描述符拍数自己打）；本层按边带的 sb_beats 出拍。
//
// 仿真注错口（0 是正式配置，综合时是常量、对应分支被优化掉），
// 位定义与 sim/engine/pix/run_formal.sh 的用例一一对应：
//   位 0 (1)   旋转错一个位置：st 多加 1
//   位 1 (2)   跳过色键：命中的像素照写
//   位 2 (4)   丢首拍掩码：第 0 拍按满拍写，行首行外的像素被写坏
//   位 3 (8)   不看写流 tready：en 恒 1，写核停顿时拍子被丢掉
//   位 4 (16)  12 个混合核不随流水停：停顿时核里的数据照走
//
// 设计依据、定案 D42~D49 与实测数据见 Engineering/TechReports/engine/04-计算阵列与片上缓存.md。
`timescale 1ns/1ps
`default_nettype none

module engine_pix #(
    // 仿真注错口，见文件头；综合时是常量。
    parameter integer INJECT = 0
)(
    input  wire         clk,
    input  wire         rst,            // 同步复位，与几何层、搬运层同一个

    // ---- I5：每行边带（几何层 → 本层）----
    input  wire [15:0]  sb_row,         // 命令内行号，最低位 = 这一行用的半
    input  wire [10:0]  sb_npx,         // 本行像素数
    input  wire [7:0]   sb_beats,       // 本行写拍数（拆段时是两段之和）
    input  wire [1:0]   sb_lead_dst,    // 目的行首地址的拍内像素相位 ld
    input  wire [1:0]   sb_lead_src,    // 源行首地址的拍内像素相位 ls（FILL 为 0）
    input  wire [7:0]   sb_op,          // 操作码，只区分 BLIT 与 FILL
    input  wire [3:0]   sb_eq,          // 方程号，0~5
    input  wire         sb_key_en,
    input  wire [31:0]  sb_key,         // 色键字（w5），与源原始存储字整字比较
    input  wire [31:0]  sb_w4,          // FILL 的常量源字
    input  wire [7:0]   sb_c,           // CONST 的系数
    input  wire [3:0]   sb_sfmt,        // 源格式，只对 BLIT 有意义
    input  wire         sb_read_dst,    // 这一行读不读目的（D7）
    input  wire         sb_valid,
    output wire         sb_ready,
    output wire [1:0]   row_free,       // I5 的反向：第 h 半两块都空着（D41、D45）

    // ---- I3：读流（搬运层 → 本层）----
    input  wire [127:0] rs_tdata,
    input  wire [15:0]  rs_tkeep,       // 对齐描述符恒全 1，不用
    input  wire         rs_tlast,       // 标描述符边界而不是行边界，不用
    input  wire         rs_tvalid,
    output wire         rs_tready,      // 恒 1（D23）
    input  wire [1:0]   rs_buf,         // {源/目的, 半}

    // ---- I4：写流（本层 → 搬运层），没有 tlast（D48）----
    output reg  [127:0] ws_tdata,       // 每拍 4 像素，X 字节最高位 = 1 表示不写（D16）
    output wire [15:0]  ws_tkeep,
    output reg          ws_tvalid,
    input  wire         ws_tready
);
    localparam [7:0] OP_BLIT = 8'd2;
    localparam [3:0] FMT_XRGB = 4'd0;
    localparam [1:0] PS_IDLE = 2'd0, PS_PRE = 2'd1,
                     PS_RUN = 2'd2, PS_DRAIN = 2'd3;

    // D47：同一个时钟使能管住每一级，包括行缓冲读口与 12 个核。
    // 注错位 3 让流水不看 tready，位 4 只让 12 个核不看。
    wire en = ((INJECT & 8) != 0) ? 1'b1 : (ws_tready || !ws_tvalid);
    wire en_core = ((INJECT & 16) != 0) ? 1'b1 : en;
    assign ws_tkeep = 16'hffff;
    assign rs_tready = 1'b1;

    // 首末拍掩码（D43），边带握手那一拍算：第 beat 拍的四个槽谁属于本行。
    // 一拍的行把行尾也算进首拍掩码，所以像素通路只在首拍/末拍/满拍三者里选。
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

    // 两项的行队列按边带握手的先后记下半号，像素通路按这个顺序开行。
    // 一行开始处理时离开队列，但它的那一半要等写流末拍被写核收走才放（D45）。
    // 一半在放出之前不会再收边带，所以队列里的两项总是两个不同的半。
    reg [1:0] occupied;
    reg row_queue [0:1];
    reg q_rd, q_wr;
    reg [1:0] q_count;
    wire sb_fire = sb_valid && sb_ready;
    assign row_free = ~occupied;
    assign sb_ready = !rst && !occupied[sb_row[0]] && (q_count != 2'd2);

    // 每一半的行参数与读入计数，边带握手时装入
    reg h_blit [0:1], h_key_en [0:1], h_read_dst [0:1];
    reg [2:0] h_eq [0:1];
    reg [31:0] h_key [0:1], h_w4 [0:1];
    reg [7:0] h_c [0:1];
    reg [3:0] h_sfmt [0:1];
    reg [1:0] h_ls [0:1], h_ld [0:1];
    reg [7:0] h_beats [0:1], h_src_need [0:1];
    reg [7:0] h_src_count [0:1], h_dst_count [0:1];
    reg h_src_done [0:1], h_dst_done [0:1];
    reg [3:0] h_first_mask [0:1], h_last_mask [0:1];
    // 源要读的拍数 = ceil((ls + npx) / 4)，与几何层源覆盖区间的拍数相同
    wire [11:0] src_sum = {1'b0, sb_npx} + {10'd0, sb_lead_src} + 12'd3;
    wire [7:0] src_need = src_sum[9:2];
    wire next_half = row_queue[q_rd];
    reg [1:0] pst;
    // 开行条件：队头那一半要用的源、目的都已读齐（FILL 不等源，不读目的的行不等目的）
    wire row_start = !rst && en && (pst == PS_IDLE) &&
                     (q_count != 2'd0) && h_src_done[next_half] &&
                     h_dst_done[next_half];

    // ---- 行缓冲 ----
    // 一行跨 4KB 时读流有两个 tlast，两段共用同一个写指针，读齐按拍数判
    wire src_we = !rst && rs_tvalid && !rs_buf[1] && occupied[rs_buf[0]];
    wire dst_we = !rst && rs_tvalid && rs_buf[1] && occupied[rs_buf[0]];
    wire [8:0] src_waddr = {rs_buf[0], h_src_count[rs_buf[0]]};
    wire [8:0] dst_waddr = {rs_buf[0], h_dst_count[rs_buf[0]]};
    // D46：目的只存每个像素的 RGB，X 字节不进 RAM
    wire [95:0] dst_wdata = {rs_tdata[119:96], rs_tdata[87:64],
                             rs_tdata[55:32], rs_tdata[23:0]};
    reg [127:0] src_mem [0:511];
    reg [95:0] dst_mem [0:511];
    reg [127:0] q_src;
    reg [95:0] q_dst;
    reg [8:0] src_raddr, dst_raddr;
    // FILL 不读源块，COPY 不读目的块（读出值不进通路，关掉读使能省功耗）
    wire src_re = en && (pst == PS_PRE || pst == PS_RUN) && r_blit;
    wire dst_re = en && (pst == PS_PRE || pst == PS_RUN) && r_read_dst;
    always @(posedge clk) begin
        if (src_we) src_mem[src_waddr] <= rs_tdata;
        if (dst_we) dst_mem[dst_waddr] <= dst_wdata;
        if (src_re) q_src <= src_mem[src_raddr];
        if (dst_re) q_dst <= dst_mem[dst_raddr];
    end

    // 当前行的参数，开行时从该半装入，整行不变；排空之后才换下一行的
    reg r_blit, r_key_en, r_read_dst;
    reg [2:0] r_eq;
    reg [31:0] r_key, r_w4;
    reg [7:0] r_c;
    reg [3:0] r_sfmt;
    reg [1:0] r_ls, r_ld;
    reg r_half;
    reg [7:0] r_beats;
    reg [3:0] r_first_mask, r_last_mask;
    reg [7:0] cnt_k, out_count, drain_count;
    reg running;

    // ---- 周期 0：地址 ----
    wire rot_off = (r_ls >= r_ld);
    wire [2:0] st_raw = rot_off ? ({1'b0, r_ls} - {1'b0, r_ld})
                                 : ({1'b0, r_ls} + 3'd4 - {1'b0, r_ld});
    wire [2:0] st_use = ((INJECT & 1) != 0) ? (st_raw + 3'd1) : st_raw;
    wire [7:0] src_k = cnt_k + {7'd0, rot_off};
    always @* begin
        src_raddr = {r_half, (pst == PS_PRE ? 8'd0 : src_k)};
        dst_raddr = {r_half, cnt_k};
    end

    // ---- 周期 2：数据级 ----
    // 两拍窗口（D42）：r_prev 是前拍、r_cur 是当前拍。r_k 与 q_src 同级，
    // r_k2 与 r_cur 同级（行缓冲的输出寄存器比地址晚一级，拍号链也要多一级）。
    reg [127:0] r_cur, r_prev;
    reg [95:0] r_dst;
    reg [7:0] r_k, r_k2;
    reg r_v1;
    always @(posedge clk) begin
        if (rst) begin
            r_cur <= 128'd0; r_prev <= 128'd0; r_dst <= 96'd0;
            r_k <= 8'd0; r_k2 <= 8'd0; r_v1 <= 1'b0;
        end else if (en) begin
            r_cur <= q_src;
            r_prev <= r_cur;
            r_dst <= q_dst;
            r_k <= cnt_k;
            r_k2 <= r_k;
            r_v1 <= running;
        end
    end

    // ---- 周期 3：旋转、色键、a 选择、掩码（组合，落寄存器）----
    wire [31:0] w [0:7];
    assign w[0] = r_prev[31:0];  assign w[1] = r_prev[63:32];
    assign w[2] = r_prev[95:64]; assign w[3] = r_prev[127:96];
    assign w[4] = r_cur[31:0];   assign w[5] = r_cur[63:32];
    assign w[6] = r_cur[95:64];  assign w[7] = r_cur[127:96];
    wire [31:0] px0 = (st_use == 3'd0) ? w[0] : (st_use == 3'd1) ? w[1] :
                      (st_use == 3'd2) ? w[2] : w[3];
    wire [31:0] px1 = (st_use == 3'd0) ? w[1] : (st_use == 3'd1) ? w[2] :
                      (st_use == 3'd2) ? w[3] : w[4];
    wire [31:0] px2 = (st_use == 3'd0) ? w[2] : (st_use == 3'd1) ? w[3] :
                      (st_use == 3'd2) ? w[4] : w[5];
    wire [31:0] px3 = (st_use == 3'd0) ? w[3] : (st_use == 3'd1) ? w[4] :
                      (st_use == 3'd2) ? w[5] : w[6];
    // FILL 的源是常量，不读源块（DMA2D 的 R2M 同法）
    wire [31:0] s0 = r_blit ? px0 : r_w4;
    wire [31:0] s1 = r_blit ? px1 : r_w4;
    wire [31:0] s2 = r_blit ? px2 : r_w4;
    wire [31:0] s3 = r_blit ? px3 : r_w4;
    wire [3:0] keys = r_blit && r_key_en && ((INJECT & 2) == 0) ?
                      {s3 == r_key, s2 == r_key, s1 == r_key, s0 == r_key} : 4'd0;
    // a：BLIT 的 XRGB 源取 255；FILL 一律取 w4[31:24]（规范条款 §4.2，见文件头第 4 条）
    wire [7:0] a0 = (r_blit && r_sfmt == FMT_XRGB) ? 8'hff : s0[31:24];
    wire [7:0] a1 = (r_blit && r_sfmt == FMT_XRGB) ? 8'hff : s1[31:24];
    wire [7:0] a2 = (r_blit && r_sfmt == FMT_XRGB) ? 8'hff : s2[31:24];
    wire [7:0] a3 = (r_blit && r_sfmt == FMT_XRGB) ? 8'hff : s3[31:24];
    wire [3:0] mask_now = ((INJECT & 4) != 0 && r_k2 == 8'd0) ? 4'hf :
                          (r_k2 == 8'd0) ? r_first_mask :
                          (r_k2 == (r_beats - 8'd1)) ? r_last_mask : 4'hf;

    // 混合核的五个输入全由寄存器直接驱动（I6 的约定，D44）：
    // 旋转与色键的组合逻辑不能挂在核的第一级前面
    reg [127:0] r_px;
    reg [95:0] r_dst2;
    reg [31:0] r_a;
    reg [3:0] r_keys, r_mask;
    reg r_v2;
    always @(posedge clk) begin
        if (rst) begin
            r_px <= 128'd0; r_dst2 <= 96'd0; r_a <= 32'd0;
            r_keys <= 4'd0; r_mask <= 4'd0; r_v2 <= 1'b0;
        end else if (en) begin
            r_px <= {s3, s2, s1, s0};
            r_dst2 <= r_read_dst ? r_dst : 96'd0;
            r_a <= {a3, a2, a1, a0};
            r_keys <= keys;
            r_mask <= mask_now;
            r_v2 <= r_v1;
        end
    end

    // ---- 周期 4~6：12 个混合核（4 像素 × RGB）----
    wire [95:0] color;
    genvar p, ch;
    generate
        for (p = 0; p < 4; p = p + 1) begin : pixel
            for (ch = 0; ch < 3; ch = ch + 1) begin : channel
                engine_blend u_blend (
                    .clk(clk), .en(en_core),
                    .src(r_px[p*32+ch*8 +: 8]),
                    .dst(r_dst2[p*24+ch*8 +: 8]),
                    .a(r_a[p*8 +: 8]), .c(r_c), .eq(r_eq),
                    .out(color[p*24+ch*8 +: 8]));
            end
        end
    endgenerate

    // 色键、掩码与 valid 的延迟像，与混合核的三级寄存器逐级对齐
    reg [3:0] key_d1, key_d2, key_d3, mask_d1, mask_d2, mask_d3;
    reg v_d1, v_d2, v_d3, v_d4;
    always @(posedge clk) begin
        if (rst) begin
            key_d1 <= 0; key_d2 <= 0; key_d3 <= 0;
            mask_d1 <= 0; mask_d2 <= 0; mask_d3 <= 0;
            v_d1 <= 0; v_d2 <= 0; v_d3 <= 0; v_d4 <= 0;
        end else if (en) begin
            key_d1 <= r_keys; key_d2 <= key_d1; key_d3 <= key_d2;
            mask_d1 <= r_mask; mask_d2 <= mask_d1; mask_d3 <= mask_d2;
            v_d1 <= r_v2; v_d2 <= v_d1; v_d3 <= v_d2; v_d4 <= v_d3;
        end
    end

    // ---- 周期 7：合并「不写」标记（D16），输出 X 字节只有标记位 ----
    wire [3:0] no_write = key_d3 | ~mask_d3;
    wire [127:0] next_data = {
        (no_write[3] ? 8'h80 : 8'h00), color[95:72],
        (no_write[2] ? 8'h80 : 8'h00), color[71:48],
        (no_write[1] ? 8'h80 : 8'h00), color[47:24],
        (no_write[0] ? 8'h80 : 8'h00), color[23:0]};
    always @(posedge clk) begin
        if (rst) begin ws_tdata <= 128'd0; ws_tvalid <= 1'b0; end
        else if (en) begin ws_tdata <= next_data; ws_tvalid <= v_d4; end
    end

    // 本行写流的末拍被写核收走：放出这一半（D45）
    wire last_out = ws_tvalid && ws_tready &&
                    (out_count == (r_beats - 8'd1));
    always @(posedge clk) begin
        if (rst) out_count <= 8'd0;
        else if (row_start) out_count <= 8'd0;
        else if (ws_tvalid && ws_tready) out_count <= out_count + 8'd1;
    end

    // ---- 每一半的占用、行参数与读入计数 ----
    // 读齐按本行对齐后的总拍数判，不看 rs_tlast（拆段时一行有两个）
    always @(posedge clk) begin
        if (rst) begin
            occupied <= 2'b00;
            q_rd <= 1'b0; q_wr <= 1'b0; q_count <= 2'd0;
            h_src_count[0] <= 0; h_src_count[1] <= 0;
            h_dst_count[0] <= 0; h_dst_count[1] <= 0;
            h_src_done[0] <= 0; h_src_done[1] <= 0;
            h_dst_done[0] <= 0; h_dst_done[1] <= 0;
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

    // ---- 行状态机 ----
    // PS_RUN 每个有效拍发一个目的拍；发完进 PS_DRAIN 等 8 拍，流水里的上一行全部出去
    // 之后才回 PS_IDLE 开下一行（下一行的参数寄存器要等它）。这段时间另一半的读入照常。
    always @(posedge clk) begin
        if (rst) begin
            pst <= PS_IDLE; cnt_k <= 8'd0; drain_count <= 8'd0;
            running <= 1'b0;
            r_blit <= 0; r_key_en <= 0; r_read_dst <= 0; r_eq <= 0;
            r_key <= 0; r_w4 <= 0; r_c <= 0; r_sfmt <= 0;
            r_ls <= 0; r_ld <= 0; r_half <= 0;
            r_beats <= 0; r_first_mask <= 0; r_last_mask <= 0;
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
                    // off = 1 的行先预热一拍（D42）
                    if (h_ls[next_half] >= h_ld[next_half]) begin
                        running <= 1'b0; pst <= PS_PRE;
                    end else begin
                        running <= 1'b1; pst <= PS_RUN;
                    end
                end
                PS_PRE: begin
                    cnt_k <= 8'd0; running <= 1'b1; pst <= PS_RUN;
                end
                PS_RUN: begin
                    if (cnt_k + 8'd1 >= r_beats) begin
                        cnt_k <= 8'd0; running <= 1'b0;
                        drain_count <= 8'd7; pst <= PS_DRAIN;
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
