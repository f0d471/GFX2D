`timescale 1ns/1ps
//==============================================================================================//
// FILE: display_fetch.v
//
// DESCRIPTION: 扫描输出的取数侧（core_clk 域）：AXI 读主机（Axi_Mux S0，只发起读），
//   按水位逐行取数、场起始清空、APB 寄存器与统计、帧快照接收与场同步中断。
//   寄存器表见 display-manual.md，注错开关见文内 `ifdef。
//
// NOTE:
//   1. 发行条件 !in_flight && (row < V_ACTIVE) && en && (DEPTH - wr_level) >= LINE_BEATS；
//      水位不含在途拍，空位与在途两笔相加才等于不溢出
//   2. 场起始清空必须先等 rlast 再动写指针；CLEAR 的像素侧清理由 clear_toggle 送过去
//   3. ack 按电平追 toggle 不按沿：两个域复位源不同，core 复位窗口里的沿会被吃掉
//   4. 复位用 display_fifo 的 rst_busy_sync（原 rst_busy 是无同步器的跨域路径），连续低 4 拍再写
//==============================================================================================//
`default_nettype none

module display_fetch #(
    parameter [31:0]  FRONT_BASE_DEF = 32'h0200_0000,   // 帧缓冲 0（设计文档 2.1 节）
    parameter [31:0]  BACK_BASE_DEF  = 32'h0240_0000,   // 帧缓冲 1
    parameter integer V_ACTIVE       = 480,
    parameter integer LINE_BEATS     = 160,             // 一行 640 像素 = 160 个 128 位拍
    parameter integer DEPTH          = 512
)(
    input  wire         clk,            // core_clk
    input  wire         rst,            // 同步复位，高有效

    // APB 从口，paddr 是窗口内 12 位偏移
    input  wire [11:0]  paddr,
    input  wire         psel,
    input  wire         penable,
    input  wire         pwrite,
    input  wire [31:0]  pwdata,
    output reg  [31:0]  prdata,
    output wire         pready,
    output wire         pslverr,

    // ---- AXI 读主机口（接 Axi_Mux 的 S0；写通道由顶层接成不发起）----
    output reg  [31:0]  ar_addr,
    output wire [7:0]   ar_len,
    output wire [2:0]   ar_size,
    output wire [1:0]   ar_burst,
    output wire [1:0]   ar_lock,
    output wire [7:0]   ar_id,
    output reg          ar_valid,
    input  wire         ar_ready,

    input  wire [7:0]   r_id,
    input  wire [127:0] r_data,
    input  wire         r_last,
    input  wire [1:0]   r_resp,
    input  wire         r_valid,
    output wire         r_ready,

    // FIFO 写侧
    output wire         fifo_wr_en,
    output wire [127:0] fifo_wr_data,
    input  wire [9:0]   fifo_wr_level,
    input  wire         fifo_wr_full,
    input  wire         fifo_wr_overflow,
    output reg          fifo_rst,
    input  wire         fifo_rst_busy,

    // 与像素侧的握手
    input  wire         fs_toggle,      // 场起始（像素侧 vs 下降沿，toggle）
    output wire         src_sel,        // 生效的 CTRL.SRC，送像素侧
    output reg          clear_toggle,   // CTRL.CLEAR：每次写 1 翻一次，像素侧同步后清累计值
    input  wire         snap_toggle,    // 帧快照号（像素侧在场起始翻）
    output reg          snap_ack,       // 收到后把这个电平跟成 snap_toggle
    input  wire [15:0]  snap_frame,
    input  wire [9:0]   snap_min_level,
    input  wire [31:0]  snap_zero_hits,
    input  wire [31:0]  snap_underflow_beats,
    input  wire [31:0]  snap_frame_beats,
    input  wire [9:0]   snap_end_level,

    // ---- 中断（电平，接 SoC 的 userInterruptA）----
    output wire         irq
);

    // APB 偏移
    localparam [11:0] A_ID        = 12'h000;
    localparam [11:0] A_VERSION   = 12'h004;
    localparam [11:0] A_CTRL      = 12'h008;
    localparam [11:0] A_FRONT     = 12'h00C;
    localparam [11:0] A_BACK      = 12'h010;
    localparam [11:0] A_SWAPREQ   = 12'h014;
    localparam [11:0] A_MAX_FIRST = 12'h018;
    localparam [11:0] A_MAX_FULL  = 12'h01C;
    localparam [11:0] A_MAX_GAP   = 12'h020;
    localparam [11:0] A_TXN       = 12'h024;
    localparam [11:0] A_LINE      = 12'h028;
    localparam [11:0] A_BEAT      = 12'h02C;
    localparam [11:0] A_SKIP      = 12'h030;
    localparam [11:0] A_RESPERR   = 12'h034;
    localparam [11:0] A_STATUS    = 12'h038;
    localparam [11:0] A_HIST_FIRST= 12'h03C;
    localparam [11:0] A_HIST_FULL = 12'h06C;
    localparam [11:0] A_HIST_GAP  = 12'h09C;
    localparam [11:0] A_IRQ_EN    = 12'h0D0;
    localparam [11:0] A_IRQ_STAT  = 12'h0D4;
    localparam [11:0] A_FRAME_CNT = 12'h0D8;
    localparam [11:0] A_SWAP_CNT  = 12'h0DC;
    localparam [11:0] A_SNAP_FRAME= 12'h0E0;
    localparam [11:0] A_SNAP_MIN  = 12'h0E4;
    localparam [11:0] A_ZERO      = 12'h0E8;
    localparam [11:0] A_UNDER     = 12'h0EC;
    localparam [11:0] A_FRAME_BEAT= 12'h0F0;
    localparam [11:0] A_END_LEVEL = 12'h0F4;

    localparam [31:0] ID_VALUE = 32'h4449_5350;   // "DISP"
    localparam [31:0] VERSION  = 32'h0000_0001;

    localparam [31:0] LINE_BYTES = 32'd4096;      // 行跨（设计文档 2.1 节）
    localparam [7:0]  AR_LEN     = LINE_BEATS - 1;
    localparam [9:0]  LINE_BEATS_10 = LINE_BEATS;
    localparam [9:0]  DEPTH_10      = DEPTH;

    wire access = psel & penable;

    wire wr_legal = (paddr == A_CTRL) | (paddr == A_BACK) | (paddr == A_SWAPREQ) |
                    (paddr == A_IRQ_EN) | (paddr == A_IRQ_STAT);

    // 注错 SWAP_NOW 用的写请求脉冲（正式配置下是常量 0，被优化掉）
    wire swap_write = access & pwrite & (paddr == A_SWAPREQ) & pwdata[0];
    // 对 SWAP_REQ 的任意写：写 1 置请求，写 0 撤销
    wire swap_wr_any = access & pwrite & (paddr == A_SWAPREQ);

    // 配置寄存器（影子），生效值在「场起始」那一拍锁存（D9）
    reg        en_sh, src_sh, clear_pulse;
    reg [31:0] back_sh;
    reg        swap_req_sh;
    reg        irq_en, irq_stat;

    reg        en_eff, src_eff;
    reg [31:0] base_eff;
    reg [31:0] front_base;        // 本场在扫的基址（只读回给软件确认）

    // 运行状态
    reg [1:0]  state;
    localparam [1:0] S_RUN = 2'd0, S_DRAIN = 2'd1, S_RESET = 2'd2, S_WAIT = 2'd3;

    reg [1:0]  rst_cnt;
    reg [2:0]  busy_low_cnt;
    reg        in_flight;         // 一笔读已发行、还没到 rlast
    reg [9:0]  row;               // 场内行序号 0..V_ACTIVE-1
    reg        started;           // 本场已开始取数

    // 场起始锁存下来的那一组
    reg        lat_en, lat_src, lat_swap;
    reg [31:0] lat_back;

    // 计时与统计
    reg [31:0] stat_max_first, stat_max_full, stat_max_gap;
    reg [31:0] stat_txn, stat_beat, stat_skip, stat_resperr;
    reg [31:0] hist_first [0:11];
    reg [31:0] hist_full  [0:11];
    reg [31:0] hist_gap   [0:11];
    reg [31:0] frame_count, swap_count;
    reg [31:0] c_first, c_full, c_gap;
    reg        got_first;

    // 帧快照（接收侧）
    reg [15:0] s_frame;
    reg [9:0]  s_min;
    reg [31:0] s_zero, s_under, s_beats;
    reg [9:0]  s_end;

    integer i;

    assign ar_len   = AR_LEN;
    assign ar_size  = 3'b100;        // 16 字节 = 128 位
    assign ar_burst = 2'b01;         // INCR
    assign ar_lock  = 2'b00;
    assign ar_id    = 8'h00;
    assign r_ready  = 1'b1;          // 排空期间也要收，否则那笔读永远完不了

    wire ar_fire = ar_valid & ar_ready;
    wire r_fire  = r_valid & r_ready;
    wire txn_end = r_fire & r_last;

    wire draining = (state != S_RUN);
    assign fifo_wr_en   = r_fire & ~draining;
    assign fifo_wr_data = r_data;

    wire [9:0] free_beats = DEPTH_10 - fifo_wr_level;
    wire room_for_line = (free_beats >= LINE_BEATS_10);

    // 场起始事件的同步与边沿检测（像素侧 toggle）
    (* async_reg = "true" *) reg [1:0] fs_sync;
    always @(posedge clk) fs_sync <= {fs_sync[0], fs_toggle};
    wire fs_event = fs_sync[1] ^ fs_sync[0];

    // 帧快照的接收：收到就把整组锁存，并把 ack 跟成收到的电平
    // 判据是**电平**不是沿：同步进来的 snap_toggle 与回给源侧的 snap_ack 不一致，
    // 就说明有一组快照还没收。
    //
    // 不能用 snap_sync[1] ^ snap_sync[0] 这个**沿**来判：snap_sync 不受 rst 约束，
    // 复位期间照样移位，而事件位被 rst 压住，于是**落在 core 复位窗口里的那一次翻转会被吃掉**。
    // 两个域的复位源不同（core 是 soc_system_reset，像素是 hdmi PLL 锁定后的 vid_rst_n），
    // 谁先出复位不由本设计决定；像素侧先出复位时，它的第一个场起始就落在 core 复位里，
    // 那一次翻转被吃掉之后源侧的 snap_busy 永远等不到 ack、再也不翻，
    // SNAP_FRAME 就永久停在复位值 0（2026-09-21 上板 run 3 的现场，判据见 sim 的 corerst 用例）。
    // 比电平没有这个问题：复位期间错过了什么都不要紧，一撤销复位两边不一致就会被重新认出来。
    //
    // 再加两条，判据本身不变，都是为了**综合后的结构**与建立时间：
    //   `snap_sync[1] == snap_sync[0]`：两级同步器读数一致才算这个电平已经稳下来。
    //     它还有一个结构上的作用——只比 snap_sync[1] 的话第 0 级就成了死端，
    //     综合会把两级折成一条 EFX_SRL8 移位链，同步器的第一级不再是独立触发器
    //     （同一份设计里 fs_sync 因为两级都被用到才保持成两个带 async_reg 的 EFX_FF；
    //      网表核对见 TechReports display 第 01 章 5.2 节）。用上第 0 级就折不掉。
    //   snap_go：把锁存再推后一拍。快照数据总线是一条跨域的组合路径，
    //     SDC 把两个时钟切开了、STA 不查它，这样从像素侧那个边沿算起有三个 core 周期的建立时间。
    //     `& ~snap_go` 不能省：snap_ack 要到锁存那一拍才更新，在那之前 snap_new 一直成立，
    //     少了它会连锁存两次。
    //
    // snap_sync / snap_go / s_* / snap_ack 全部只在这一个块里驱动（map 会查多驱动）。
    (* async_reg = "true" *) reg [1:0] snap_sync;
    reg snap_go;
    wire snap_new = (snap_sync[1] ^ snap_ack) & (snap_sync[1] == snap_sync[0]);

    always @(posedge clk) begin
        snap_sync <= {snap_sync[0], snap_toggle};
        if (rst) begin
            snap_go <= 1'b0;
            s_frame <= 16'h0; s_min <= 10'h0; s_zero <= 32'h0;
            s_under <= 32'h0; s_beats <= 32'h0; s_end <= 10'h0;
            snap_ack <= 1'b0;
        end else begin
            snap_go <= snap_new & ~snap_go;
            if (snap_go) begin
                s_frame <= snap_frame;
                s_min   <= snap_min_level;
                s_zero  <= snap_zero_hits;
                s_under <= snap_underflow_beats;
                s_beats <= snap_frame_beats;
                s_end   <= snap_end_level;
                snap_ack <= snap_sync[1];
            end
        end
    end

    // 发行与收数
    // !ar_valid 这一条不能省：ar_fire 那一拍照样满足 !in_flight（寄存器还是旧值），
    // 少了它就变成「同一拍先清 ar_valid 又立刻拉起」，一笔没完就接着发下一笔。
    //
    // !fs_event 这一条也不能省（2026-09-21 复核补）：下面的 case 是用**旧的**
    // in_flight / ar_valid 判断要不要进 S_DRAIN 的。若同一拍既 fs_event 又 can_issue，
    // 就会一边把 ar_valid 拉起、一边因为「当时没有在途也没有挂起」直接跳过排空去复位 FIFO；
    // 那笔 AR 随后才被接走，它的拍正好落在 S_RUN 恢复之后，进新场的队头——
    // 这就是 D7 要防的那件事，由清空动作自己造出来。
    // 触发条件很窄（本场还有行没取 + 恰好在 fs_event 那一拍空闲），但那正是欠载自愈的场景。
    wire can_issue;
`ifdef INJ_INFLIGHT2
    // 注错：允许两笔在途
    assign can_issue = (state == S_RUN) && started && en_eff && !ar_valid && !fs_event &&
                       (row < V_ACTIVE) && room_for_line;
`else
    assign can_issue = (state == S_RUN) && started && en_eff && !ar_valid && !fs_event &&
                       !in_flight && (row < V_ACTIVE) && room_for_line;
`endif

    always @(posedge clk) begin
        if (rst) begin
            ar_valid <= 1'b0;
            ar_addr  <= 32'h0;
            in_flight <= 1'b0;
            row      <= 10'h0;
            started  <= 1'b0;
            got_first<= 1'b0;
            c_first  <= 32'h0;
            c_full   <= 32'h0;
            c_gap    <= 32'h0;
            state    <= S_RESET;      // 上电先清一次 FIFO：厂商 FIFO 的指针在 a_rst_i 之前是 X
            rst_cnt  <= 2'h0;
            busy_low_cnt <= 3'h0;
            fifo_rst <= 1'b0;
            en_eff   <= 1'b0;
            src_eff  <= 1'b0;
            base_eff   <= FRONT_BASE_DEF;
            front_base <= FRONT_BASE_DEF;
            swap_count <= 32'h0;
            swap_req_sh <= 1'b0;
            // 场起始锁存组也要复位：上电那次 S_RESET -> S_WAIT 发生在第一个 fs_event 之前，
            // 那时会拿 lat_* 去写 en_eff / src_eff / started。不复位的话仿真里是 X、
            // 板上是上电值，两边对不上，而且 if (lat_swap) 会走进未定义的分支。
            lat_en   <= 1'b0;
            lat_src  <= 1'b0;
            lat_swap <= 1'b0;
            lat_back <= BACK_BASE_DEF;
        end else begin
            // 计时（与 scanout_stub 同口径：从 AR 握手那一拍算起，握手后下一拍就回的记 0）
            if (in_flight) begin
                c_full <= c_full + 32'h1;
                if (!got_first) c_first <= c_first + 32'h1;
                else            c_gap   <= c_gap + 32'h1;
            end

            if (ar_fire) begin
                ar_valid  <= 1'b0;
                in_flight <= 1'b1;
                got_first <= 1'b0;
                c_first   <= 32'h0;
                c_full    <= 32'h0;
                c_gap     <= 32'h0;
            end

            if (r_fire) begin
                if (!got_first) begin
                    got_first <= 1'b1;
                    c_gap     <= 32'h0;
                end else begin
                    c_gap <= 32'h0;
                end
                if (r_last) in_flight <= 1'b0;
            end

            // 行推进：只在正常取数时按 rlast 推进；排空的那一笔不算
            if (txn_end && !draining)
                row <= row + 10'h1;

            // 发行（在途不超过一笔：in_flight 清零之后才可能再发）
            if (can_issue) begin
                ar_valid <= 1'b1;
                ar_addr  <= base_eff + ({22'h0, row} << 12);
            end

`ifdef INJ_SWAP_NOW
            // 注错 SWAP_NOW：写请求立刻切换，不等场起始（会切出撕裂帧）
            if (swap_write) begin
                base_eff   <= back_sh;
                front_base <= back_sh;
                swap_count <= swap_count + 32'h1;
            end
`endif

            // SWAP_REQ 的请求位：软件写它（写 0 撤销），场起始那一刻锁存并清零（D9）
`ifdef INJ_SWAP_NOW
            if (swap_wr_any)
                swap_req_sh <= 1'b0;      // 注错：请求位不再由软件置起
`else
            if (swap_wr_any)
                swap_req_sh <= pwdata[0];
`endif

            // ---- 场起始清空的六步（D7）----
            case (state)
                S_RUN: begin
                    if (fs_event) begin
                        // 第一步：停发新读，并锁存这一场要用的配置（D9）
                        lat_en   <= en_sh;
                        lat_src  <= src_sh;
                        lat_back <= back_sh;
                        lat_swap <= swap_req_sh;
                        swap_req_sh <= 1'b0;
                        // 第一步之后要等的那一笔：已经在途的，或者地址已经拉高、还没被接走的。
                        // ar_valid 挂起时不能直接去复位：那一笔迟早会被接走，然后它的拍会
                        // 流进新场的队头——正是 D7 要防的那件事。
                        // 注错 NO_DRAIN：跳过第二步，在途那笔剩下的拍会落进新场的队头
`ifdef INJ_NO_DRAIN
                        state <= S_RESET;
`else
                        state <= (in_flight || ar_valid) ? S_DRAIN : S_RESET;
`endif
                    end
                end
                S_DRAIN: begin
                    // 第二步：等 rlast、丢弃它的数据（fifo_wr_en 已按 draining 压住）
                    if (txn_end) state <= S_RESET;
                end
                S_RESET: begin
                    // 第三步：两侧一起复位，保持 2 个 core_clk 周期
`ifdef INJ_NO_CLEAR
                    fifo_rst <= 1'b0;             // 注错：场起始不清 FIFO
`else
                    fifo_rst <= 1'b1;
`endif
                    if (rst_cnt == 2'h1) begin
                        rst_cnt <= 2'h0;
                        state   <= S_WAIT;
                    end else begin
                        rst_cnt <= rst_cnt + 2'h1;
                    end
                end
                S_WAIT: begin
                    // 第四步：等复位真的落下（同步过的 rst_busy），连续 4 拍低
                    fifo_rst <= 1'b0;
                    if (busy_low_cnt == 3'h3) begin
                        busy_low_cnt <= 3'h0;
                        // 第五步：锁存本场基址与配置，第六步回到 S_RUN 开始预取
                        en_eff   <= lat_en;
                        src_eff  <= lat_src;
                        started  <= lat_en;
                        row      <= 10'h0;
                        if (lat_swap) begin
                            base_eff   <= lat_back;
                            front_base <= lat_back;
                            swap_count <= swap_count + 32'h1;
                        end
                        state <= S_RUN;
                    end else begin
                        busy_low_cnt <= fifo_rst_busy ? 3'h0 : (busy_low_cnt + 3'h1);
                    end
                end
                default: state <= S_RUN;
            endcase
        end
    end

    // 场起始的计数与中断（core_clk 域）
    always @(posedge clk) begin
        if (rst) begin
            frame_count <= 32'h0;
            irq_stat    <= 1'b0;
        end else begin
            if (fs_event) begin
                frame_count <= frame_count + 32'h1;
                irq_stat    <= 1'b1;                 // 电平中断，写 1 清除
            end
            if (access & pwrite & (paddr == A_IRQ_STAT) & pwdata[0])
                irq_stat <= 1'b0;
        end
    end

    assign irq = irq_stat & irq_en;
    assign src_sel = src_eff;

    // 统计：最大值与直方图（一次测量在这里落账，口径同 scanout_stub）
    function [3:0] bucket;
        input [31:0] v;
        begin
            if      (v >= 32'd2048) bucket = 4'd11;
            else if (v >= 32'd1024) bucket = 4'd10;
            else if (v >= 32'd512)  bucket = 4'd9;
            else if (v >= 32'd256)  bucket = 4'd8;
            else if (v >= 32'd128)  bucket = 4'd7;
            else if (v >= 32'd64)   bucket = 4'd6;
            else if (v >= 32'd32)   bucket = 4'd5;
            else if (v >= 32'd16)   bucket = 4'd4;
            else if (v >= 32'd8)    bucket = 4'd3;
            else if (v >= 32'd4)    bucket = 4'd2;
            else if (v >= 32'd2)    bucket = 4'd1;
            else                    bucket = 4'd0;
        end
    endfunction

    wire [11:0] dec_first = 12'b1 << bucket(c_first);
    wire [11:0] dec_gap   = 12'b1 << bucket(c_gap);
    wire [11:0] dec_full  = 12'b1 << bucket(c_full);

    always @(posedge clk) begin
        if (rst | clear_pulse) begin
            stat_max_first <= 32'h0;
            stat_max_full  <= 32'h0;
            stat_max_gap   <= 32'h0;
            stat_resperr   <= 32'h0;
            for (i = 0; i < 12; i = i + 1) begin
                hist_first[i] <= 32'h0;
                hist_full[i]  <= 32'h0;
                hist_gap[i]   <= 32'h0;
            end
        end else if (r_fire) begin
            if (r_resp != 2'b00)
                stat_resperr <= stat_resperr + 32'h1;

            if (!got_first) begin
                if (c_first > stat_max_first) stat_max_first <= c_first;
                for (i = 0; i < 12; i = i + 1)
                    if (dec_first[i]) hist_first[i] <= hist_first[i] + 32'h1;
            end else begin
                if (c_gap > stat_max_gap) stat_max_gap <= c_gap;
                for (i = 0; i < 12; i = i + 1)
                    if (dec_gap[i]) hist_gap[i] <= hist_gap[i] + 32'h1;
            end

            if (r_last) begin
                if (c_full > stat_max_full) stat_max_full <= c_full;
                for (i = 0; i < 12; i = i + 1)
                    if (dec_full[i]) hist_full[i] <= hist_full[i] + 32'h1;
            end
        end
    end

    always @(posedge clk) begin
        if (rst | clear_pulse) begin
            stat_txn  <= 32'h0;
            stat_beat <= 32'h0;
            stat_skip <= 32'h0;
        end else begin
            if (txn_end) stat_txn  <= stat_txn + 32'h1;
            if (r_fire)  stat_beat <= stat_beat + 32'h1;
            // 按水位取数不会「跳过」行：没空位只是推迟。这个偏移保留读回 0，
            // 让 ddrBurstProbe 的 skip=0 判据继续成立。
            stat_skip <= 32'h0;
        end
    end

    // APB 读
    reg hit;

    always @(*) begin
        hit = 1'b1;
        if      (paddr == A_ID)        prdata = ID_VALUE;
        else if (paddr == A_VERSION)   prdata = VERSION;
        else if (paddr == A_CTRL)      prdata = {29'h0, 1'b0, src_sh, en_sh};
        else if (paddr == A_FRONT)     prdata = front_base;
        else if (paddr == A_BACK)      prdata = back_sh;
        else if (paddr == A_SWAPREQ)   prdata = {31'h0, swap_req_sh};
        else if (paddr == A_MAX_FIRST) prdata = stat_max_first;
        else if (paddr == A_MAX_FULL)  prdata = stat_max_full;
        else if (paddr == A_MAX_GAP)   prdata = stat_max_gap;
        else if (paddr == A_TXN)       prdata = stat_txn;
        else if (paddr == A_LINE)      prdata = {22'h0, row};
        else if (paddr == A_BEAT)      prdata = stat_beat;
        else if (paddr == A_SKIP)      prdata = stat_skip;
        else if (paddr == A_RESPERR)   prdata = stat_resperr;
        else if (paddr == A_STATUS)    prdata = {29'h0, started, ar_valid, in_flight};
        else if ((paddr >= A_HIST_FIRST) && (paddr < A_HIST_FIRST + 12*4))
            prdata = hist_first[(paddr - A_HIST_FIRST) >> 2];
        else if ((paddr >= A_HIST_FULL) && (paddr < A_HIST_FULL + 12*4))
            prdata = hist_full[(paddr - A_HIST_FULL) >> 2];
        else if ((paddr >= A_HIST_GAP) && (paddr < A_HIST_GAP + 12*4))
            prdata = hist_gap[(paddr - A_HIST_GAP) >> 2];
        else if (paddr == A_IRQ_EN)    prdata = {31'h0, irq_en};
        else if (paddr == A_IRQ_STAT)  prdata = {31'h0, irq_stat};
        else if (paddr == A_FRAME_CNT) prdata = frame_count;
        else if (paddr == A_SWAP_CNT)  prdata = swap_count;
        else if (paddr == A_SNAP_FRAME)prdata = {16'h0, s_frame};
        else if (paddr == A_SNAP_MIN)  prdata = {22'h0, s_min};
        else if (paddr == A_ZERO)      prdata = s_zero;
        else if (paddr == A_UNDER)     prdata = s_under;
        else if (paddr == A_FRAME_BEAT)prdata = s_beats;
        else if (paddr == A_END_LEVEL) prdata = {22'h0, s_end};
        else begin
            prdata = 32'h0;
            hit    = 1'b0;
        end
    end

    assign pready = 1'b1;
    // FRONT_BASE 的写按「忽略且不报错」处理：替身时代它是可写的 BASE，
    // ddrBurstProbe 会往 0x00C 写，报 PSLVERROR 有触发总线异常的风险。
    assign pslverr = access & (pwrite ? ~(wr_legal | (paddr == A_FRONT)) : ~hit);

    // APB 写（影子寄存器；生效时刻见 D9）
    always @(posedge clk) begin
        if (rst) begin
            en_sh       <= 1'b0;
            src_sh      <= 1'b0;
            back_sh     <= BACK_BASE_DEF;
            // swap_req_sh 不在这里复位：它的唯一驱动在 FSM 那个 always 块里
            irq_en      <= 1'b0;
            clear_pulse <= 1'b0;
            clear_toggle<= 1'b0;
        end else begin
            clear_pulse <= 1'b0;           // 单拍脉冲，清 core 侧统计
            if (access & pwrite) begin
                case (paddr)
                    A_CTRL: begin
                        en_sh  <= pwdata[0];
                        src_sh <= pwdata[1];
                        if (pwdata[2]) begin
                            clear_pulse  <= 1'b1;
                            // 像素侧不共用这个时钟，用 toggle 送过去（见文件头）
                            clear_toggle <= ~clear_toggle;
                        end
                    end
                    A_BACK:    back_sh     <= pwdata;
                    // SWAP_REQ 的请求位由 FSM 那个 always 块独占驱动（它同时负责场起始的清零），
                    // 这里不写它，免得同一个寄存器被两个块驱动（map 会报 VDB-1000）
                    A_IRQ_EN:  irq_en      <= pwdata[0];
                    default: ;
                endcase
            end
        end
    end

endmodule

`default_nettype wire
