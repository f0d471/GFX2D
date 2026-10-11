`timescale 1ns/1ps
//==============================================================================================//
// FILE: display_out.v
//
// DESCRIPTION: 扫描输出的像素侧（像素时钟域）：唯一视频时序发生器（color_bar_rgb），
//   128 位拍拆 4 像素并 D=1 对齐，SRC 彩条/帧缓冲二选一，水位与欠载观测、帧快照。
//
// NOTE:
//   1. color_bar_rgb 的 hs/vs/de 是全设计的「场」基准：取数、切换、中断、帧快照都从场起始出发
//   2. 对齐定值：FIFO 读延迟 1 个 rd 周期，rd_en = de0 & (psel==0) & ~empty 只在组首像素弹拍；
//      相位改成 psel==3 会每场多弹一次，场末必假欠载
//   3. 行号用自己的行计数器（de0 上升沿加一），不取 color_bar 的 v_cnt（比 de 早一拍）
//   4. 场末残留水位在「场起始」拍采样（取数侧清空之前），正常恒为 0，非 0 即有拍留在队里
//==============================================================================================//
`default_nettype none

module display_out #(
    parameter H_ACTIVE = 640,
    parameter H_FP     = 16,
    parameter H_SYNC   = 96,
    parameter H_BP     = 48,
    parameter V_ACTIVE = 480,
    parameter V_FP     = 10,
    parameter V_SYNC   = 2,
    parameter V_BP     = 33,
    parameter SYNC_POL = 1'b0
)(
    input  wire         clk,            // 像素时钟（hdmi_tx_slow_clk）
    input  wire         rst_n,          // vid_rst_n，来自 hdmi_tx 的复位同步
    input  wire         src_sel,        // CTRL.SRC（core_clk 域），场起始锁存
    input  wire         clear_toggle,   // CTRL.CLEAR（core_clk 域 toggle），清本侧累计值

    // FIFO 读侧
    input  wire [127:0] rd_data,
    input  wire [9:0]   rd_level,
    input  wire         rd_empty,
    output wire         rd_en,

    // 给 hdmi_tx 的像素与同步（同拍）
    output wire [23:0]  vid_rgb,
    output wire         vid_hs,
    output wire         vid_vs,
    output wire         vid_de,

    // 给取数侧（core_clk 域）
    output wire         fs_toggle,      // 场起始：vs 的下降沿，每个场翻一次
    output wire         snap_toggle,    // 帧快照号：快照更新时翻一次
    input  wire         snap_ack,       // 取数侧收到快照后把这个电平跟成 snap_toggle
    output wire [15:0]  snap_frame,
    output wire [9:0]   snap_min_level,
    output wire [31:0]  snap_zero_hits,
    output wire [31:0]  snap_underflow_beats,
    output wire [31:0]  snap_frame_beats,
    output wire [9:0]   snap_end_level
);

    localparam [9:0] LEVEL_MAX = 10'd512;

    // 注错（变异测试）：本文件是干净形态，三处变异各包在 `ifdef 内。
    // 都不定义时它们不进编译输入（预处理阶段已删除），不依赖综合器优化。
    // 编译开关与 sim/display/run.sh 的用例一一对应，一次只开一个：
    //   INJ_LAG0      对齐取 D=0，用 psel 而不是 psel_d1
    //   INJ_PHASE3    弹出相位改成 psel==3
    //   INJ_ORDER_REV 拍内像素顺序拆反
    // 见 Engineering/Todo/hw/扫描输出.md 2.3 节。

    // 「水位与触底」的计量窗口：一场取 480 行、弹 480 行，起始水位是 0（场起始清空过），
    // 所以**场末必然排空到 0**——这是守恒，不是故障。取数侧发完最后一行就没有行可发了，
    // 剩下的水位只够约 3 行（512/160），所以把最后 4 行排除在计量窗口外，
    // 「最低水位不低于一行」量的才是取数侧的预取余量。
    localparam [9:0] MEASURE_LINES = (V_ACTIVE > 4) ? (V_ACTIVE - 4) : 10'd0;

    // 唯一的时序发生器
    wire        hs0, vs0, de0;
    wire [23:0] bar_rgb;

    color_bar_rgb #(
        .HS_POLORY     (SYNC_POL),
        .VS_POLORY     (SYNC_POL),
        .SYMBOL_WIDTH  (8),
        .SYMBOL_NUM    (3),
        .PAR_PIXEL_NUM (1),
        .HFP           (H_FP),
        .HST           (H_SYNC),
        .HACT          (H_ACTIVE),
        .HBP           (H_BP),
        .VFP           (V_FP),
        .VST           (V_SYNC),
        .VACT          (V_ACTIVE),
        .VBP           (V_BP),
        .TEST_MODE     (2'd2)          // 白黄青绿品红蓝黑八色竖条
    ) u_color_bar (
        .clk        (clk),
        .rst_n      (rst_n),
        .i_cfg_vid  (24'd0),
        .h_cnt      (),
        .v_cnt      (),
        .hs         (hs0),
        .vs         (vs0),
        .de         (de0),
        .o_vid_data (bar_rgb)
    );

    // 场起始：同步低有效时 vs 的下降沿就是 VST 开始
    reg vs0_d = 1'b0;
    always @(posedge clk) vs0_d <= vs0;

    // 同步低有效时 vs 的下降沿就是 VST 开始；复位期间不产生场起始
    wire fs_pulse = rst_n & vs0_d & ~vs0;   // 一个像素时钟宽

    reg fs_toggle_r = 1'b0;
    always @(posedge clk) begin
        if (!rst_n)            fs_toggle_r <= 1'b0;
        else if (fs_pulse)     fs_toggle_r <= ~fs_toggle_r;
    end
    assign fs_toggle = fs_toggle_r;

    // 拍内相位与对齐：pix_cnt 只在 de0 有效时计数，psel 是它的低两位
    reg [10:0] pix_cnt = 11'd0;
    reg [1:0]  psel_d1 = 2'd0;
    reg        de0_d   = 1'b0;

    wire [1:0] psel = pix_cnt[1:0];
    wire       de0_rise = de0 & ~de0_d;

    always @(posedge clk) begin
        if (!rst_n) begin
            pix_cnt <= 11'd0;
            psel_d1 <= 2'd0;
            de0_d   <= 1'b0;
        end else begin
            de0_d   <= de0;
            psel_d1 <= psel;
            if (!de0) pix_cnt <= 11'd0;             // 组内相位由 de0 的沿对齐
            else      pix_cnt <= pix_cnt + 11'd1;
        end
    end

    // 时序延迟一拍，与 rd_data 在下一拍到达的数据对齐
    reg hs1 = 1'b1, vs1 = 1'b1, de1 = 1'b0;
    reg [23:0] bar_rgb_d1 = 24'h0;
    always @(posedge clk) begin
        if (!rst_n) begin
            hs1 <= 1'b1; vs1 <= 1'b1; de1 <= 1'b0; bar_rgb_d1 <= 24'h0;
        end else begin
            hs1        <= hs0;
            vs1        <= vs0;
            de1        <= de0;
            bar_rgb_d1 <= bar_rgb;
        end
    end

    // 每组的第一个像素周期弹这一组的拍（D=1，见文件头）
    wire group_start = de0 & (psel == 2'd0);
`ifdef INJ_PHASE3
    wire pop_phase   = de0 & (psel == 2'd3);   // 注错：弹出相位改成 psel==3
`else
    wire pop_phase   = group_start;
`endif
    assign rd_en = pop_phase & ~rd_empty;

    // 这一组没弹到（FIFO 空）：本组的 4 个像素输出黑色。miss_sh 在组首装载、每拍左移，
    // 于是输出第 k 个像素的那一拍上 miss_sh[k] 正好是「这一组当时有没有弹到」。
    //
    // 2026-09-21 复核订正：原先又给它加了一级 miss_d1 再用 miss_d1[sel]，等于整体晚一拍。
    // 后三个像素照样对（移位把标志铺满了），但**第 0 个像素取到的是上一组移剩的 0**，
    // 于是欠载时每组的第 0 个像素不黑化，输出 FIFO 上一拍的残留值。
    // 判别性实验：rd_empty 恒 1、SRC=1、rd_data 低 32 位填 0x00AABBCC，
    // 有效区里每 4 个像素就出现一次 aabbcc（928 个像素里 232 个），改回 miss_sh 之后全黑。
    reg [3:0] miss_sh = 4'b0;
    always @(posedge clk) begin
        if (!rst_n) begin
            miss_sh <= 4'b0;
        end else begin
            if (!de0)              miss_sh <= 4'b0;
            else if (pop_phase)    miss_sh <= {4{rd_empty}};
            else                   miss_sh <= {miss_sh[2:0], 1'b0};
        end
    end

`ifdef INJ_LAG0
    wire [1:0] sel_base = psel;                // 注错：对齐取 D=0
`else
    wire [1:0] sel_base = psel_d1;
`endif
`ifdef INJ_ORDER_REV
    wire [1:0] sel = ~sel_base;                // 注错：拍内像素顺序拆反
`else
    wire [1:0] sel = sel_base;
`endif
    wire missed = miss_sh[sel];

    wire [31:0] fifo_pixel = rd_data[{sel, 5'b0} +: 32];

    // SRC 选择（场起始锁存，见 D9）
    (* async_reg = "true" *) reg [1:0] src_sync = 2'b00;
    always @(posedge clk) src_sync <= {src_sync[0], src_sel};

    reg src_eff = 1'b0;
    always @(posedge clk) begin
        if (!rst_n)         src_eff <= 1'b0;
        else if (fs_pulse)  src_eff <= src_sync[1];
    end

    wire [23:0] px_fb  = (de1 && !missed) ? fifo_pixel[23:0] : 24'h0;
    wire [23:0] px_bar = de1 ? bar_rgb_d1 : 24'h0;

    assign vid_rgb = src_eff ? px_fb : px_bar;
    assign vid_hs  = hs1;
    assign vid_vs  = vs1;
    assign vid_de  = de1;

    // 行计数（不能用 color_bar 的 v_cnt）
    reg [9:0] line_cnt = 10'd0;
    always @(posedge clk) begin
        if (!rst_n)             line_cnt <= 10'd0;
        else if (fs_pulse)      line_cnt <= 10'd0;
        else if (de0_rise)      line_cnt <= line_cnt + 10'd1;
    end

    // 水位、触底、欠载（只在有效显示区统计）
    reg [9:0]  min_level = LEVEL_MAX;
    reg [31:0] frame_beats = 32'h0;
    reg [31:0] zero_hits = 32'h0;
    reg [31:0] underflow_beats = 32'h0;
    reg        level_was_zero = 1'b0;
    reg [15:0] frame_no = 16'h0;

    // CTRL.CLEAR 从 core 侧 toggle 过来（2026-09-21 上板补）：
    // 本侧的累计值只靠像素域复位（pll_locked）清，而 core 复位不会碰像素域，
    // 于是 reset halt 之后 src_eff 还留着上一次的 1，一整场的空读会灌进欠载计数，
    // 软件却清不掉。判据要求这两个数恒为 0，所以必须给软件一条清零的路。
    (* async_reg = "true" *) reg [1:0] clr_sync = 2'b00;
    reg clr_d = 1'b0;
    always @(posedge clk) begin
        clr_sync <= {clr_sync[0], clear_toggle};
        clr_d    <= clr_sync[1];
    end
    wire clear_event = clr_sync[1] ^ clr_d;

    wire pop = rd_en;                          // 本拍真的弹了一拍
    // 只在「帧缓冲模式的有效显示区」统计水位与欠载：SRC=0（彩条）时 FIFO 本来就空，
    // 上电到软件使能之间会白记一整场的欠载，把判据污染掉。
    wire vis_fb = de1 & src_eff;
    wire group_missed = group_start & rd_empty & src_eff;

    always @(posedge clk) begin
        if (!rst_n) begin
            min_level       <= LEVEL_MAX;
            frame_beats     <= 32'h0;
            zero_hits       <= 32'h0;
            underflow_beats <= 32'h0;
            level_was_zero  <= 1'b0;
            frame_no        <= 16'h0;
        end else begin
            if (fs_pulse) begin
                // 每场重置：最低水位与弹出拍数是「这一场」的量
                min_level       <= LEVEL_MAX;
                frame_beats     <= 32'h0;
                level_was_zero  <= 1'b0;
                frame_no        <= frame_no + 16'h1;
                // 触底次数与欠载拍数是累计量（判据要求恒为 0），不按场清零：
                // 若每场清零，中间某一场的偶发欠载会被下一场抹掉。
                // 要清就由软件写 CTRL.CLEAR，走下面的 clear_event。
            end else begin
                if (pop)
                    frame_beats <= frame_beats + 32'h1;
                if (group_missed)
                    underflow_beats <= underflow_beats + 32'h1;

                if (de1 && src_eff && (line_cnt < MEASURE_LINES)) begin
                    if (rd_level < min_level)
                        min_level <= rd_level;
                    if (rd_level == 10'd0 && !level_was_zero)
                        zero_hits <= zero_hits + 32'h1;
                end
                level_was_zero <= (rd_level == 10'd0);
            end

            // CLEAR 的优先级最高：同一拍里它压过上面的累加
            if (clear_event) begin
                zero_hits       <= 32'h0;
                underflow_beats <= 32'h0;
            end
        end
    end

    // 帧快照：场起始锁存一组，toggle/ack 送回 core_clk；
    // 接收侧确认之前源侧保持不变，更新前先看上一组是否已确认
    reg [15:0] h_frame = 16'h0;
    reg [9:0]  h_min   = 10'd0;
    reg [31:0] h_zero  = 32'h0;
    reg [31:0] h_under = 32'h0;
    reg [31:0] h_beats = 32'h0;
    reg [9:0]  h_end   = 10'd0;

    reg snap_toggle_r = 1'b0;
    reg snap_busy     = 1'b0;

    (* async_reg = "true" *) reg [1:0] ack_sync = 2'b00;
    always @(posedge clk) ack_sync <= {ack_sync[0], snap_ack};

    always @(posedge clk) begin
        if (!rst_n) begin
            h_frame     <= 16'h0;
            h_min       <= 10'd0;
            h_zero      <= 32'h0;
            h_under     <= 32'h0;
            h_beats     <= 32'h0;
            h_end       <= 10'd0;
            snap_toggle_r <= 1'b0;
            snap_busy     <= 1'b0;
        end else begin
            if (fs_pulse && !snap_busy) begin
                h_frame       <= frame_no;          // 刚结束的那一场
                h_min         <= min_level;
                h_zero        <= zero_hits;
                h_under       <= underflow_beats;
                h_beats       <= frame_beats;
                h_end         <= rd_level;          // 清空之前的残留水位
                snap_toggle_r <= ~snap_toggle_r;
                snap_busy     <= 1'b1;
            end
            if (snap_busy && (ack_sync[1] == snap_toggle_r))
                snap_busy <= 1'b0;
        end
    end

    assign snap_toggle        = snap_toggle_r;
    assign snap_frame         = h_frame;
    assign snap_min_level     = h_min;
    assign snap_zero_hits     = h_zero;
    assign snap_underflow_beats = h_under;
    assign snap_frame_beats   = h_beats;
    assign snap_end_level     = h_end;

endmodule

`default_nettype wire
