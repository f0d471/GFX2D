// hdmi_tx 通路仿真：把 4 路 10 位 TMDS 字按 DVI 1.0 规则解码回同步与像素，逐项核对
//
// 判据（每一项都对每个完整帧逐像素检查）：
//   1. 时钟通道恒为 10'b1111100000
//   2. 三个数据通道对「控制期/数据期」的判断一致
//   3. 行周期 H_TOTAL、行同步宽度 H_SYNC、有效区起点 H_SYNC+H_BP、有效宽度 H_ACTIVE
//   4. 场周期 V_TOTAL 行、场同步 V_SYNC 行、有效区起始行 V_SYNC+V_BP、有效行数 V_ACTIVE
//   5. 有效像素颜色与八色竖条一致
//
// 注错开关（run.sh 用来确认判据会报错）：
//   INJECT_HBP   ：被测模块的行后沿比期望少 1 个像素
//   INJECT_NOINV ：测试台不撤销板级 P/N 取反
`timescale 1ns/1ps
module tb_hdmi_tx;

localparam H_ACTIVE = 640;
localparam H_FP     = 16;
localparam H_SYNC   = 96;
localparam H_BP     = 48;
localparam V_ACTIVE = 480;
localparam V_FP     = 10;
localparam V_SYNC   = 2;
localparam V_BP     = 33;
localparam H_TOTAL  = H_ACTIVE + H_FP + H_SYNC + H_BP;
localparam V_TOTAL  = V_ACTIVE + V_FP + V_SYNC + V_BP;
localparam FRAMES_TO_CHECK = 2;

`ifdef INJECT_HBP
localparam DUT_H_BP = H_BP - 1;
`else
localparam DUT_H_BP = H_BP;
`endif

reg clk = 1'b0;
always #19.841 clk = ~clk;             // 25.2MHz

reg locked = 1'b0;

wire [9:0] tx_clk;
wire [9:0] tx_d0;
wire [9:0] tx_d1;
wire [9:0] tx_d2;

hdmi_tx #(
    .H_BP (DUT_H_BP)
) dut (
    .clk_pixel  (clk),
    .pll_locked (locked),
    .tmds_clk   (tx_clk),
    .tmds_data0 (tx_d0),
    .tmds_data1 (tx_d1),
    .tmds_data2 (tx_d2)
);

`ifdef INJECT_NOINV
wire [9:0] w_clk = tx_clk;
wire [9:0] w_d0  = tx_d0;
wire [9:0] w_d1  = tx_d1;
wire [9:0] w_d2  = tx_d2;
`else
wire [9:0] w_clk = ~tx_clk;
wire [9:0] w_d0  = ~tx_d0;
wire [9:0] w_d1  = ~tx_d1;
wire [9:0] w_d2  = ~tx_d2;
`endif

// DVI 1.0 控制字
function is_ctrl(input [9:0] w);
    is_ctrl = (w == 10'b1101010100) || (w == 10'b0010101011) ||
              (w == 10'b0101010100) || (w == 10'b1010101011);
endfunction

// 返回 {c1, c0}
function [1:0] ctrl_bits(input [9:0] w);
    case (w)
        10'b1101010100: ctrl_bits = 2'b00;
        10'b0010101011: ctrl_bits = 2'b01;
        10'b0101010100: ctrl_bits = 2'b10;
        default:        ctrl_bits = 2'b11;
    endcase
endfunction

function [7:0] dec8(input [9:0] w);
    reg [7:0] d;
    integer i;
    begin
        d = w[9] ? ~w[7:0] : w[7:0];
        dec8[0] = d[0];
        for (i = 1; i < 8; i = i + 1)
            dec8[i] = w[8] ? (d[i] ^ d[i-1]) : ~(d[i] ^ d[i-1]);
    end
endfunction

function [23:0] bar_rgb(input integer x);
    case ((x * 8) / H_ACTIVE)
        0: bar_rgb = 24'hffffff;
        1: bar_rgb = 24'hffff00;
        2: bar_rgb = 24'h00ffff;
        3: bar_rgb = 24'h00ff00;
        4: bar_rgb = 24'hff00ff;
        5: bar_rgb = 24'hff0000;
        6: bar_rgb = 24'h0000ff;
        default: bar_rgb = 24'h000000;
    endcase
endfunction

integer errors = 0;
task report(input [8*64-1:0] what, input integer got, input integer exp);
    begin
        errors = errors + 1;
        if (errors <= 10)
            $display("ERROR t=%0t %0s: got %0d expect %0d", $time, what, got, exp);
    end
endtask

// 同步脉冲低有效，空闲电平为 1
localparam SYNC_ACTIVE = 1'b0;

reg     hs_prev = ~SYNC_ACTIVE;
reg     vs_prev = ~SYNC_ACTIVE;
reg     de_prev = 1'b0;
reg     synced  = 1'b0;                // 检查已启用
integer vs_starts    = 0;              // 已见到的场同步起点个数
integer px_in_line   = 0;              // 距本行 HS 起点的像素数
integer hs_run       = 0;
integer de_run       = 0;
integer line_in_frame = 0;             // 距本帧 VS 起点的行数
integer vs_lines     = 0;
integer act_lines    = 0;
integer first_act_line = -1;
reg     line_has_de  = 1'b0;
integer frames_done  = 0;
integer pixels_checked = 0;

reg        ctrl0, ctrl1, ctrl2;
reg  [1:0] cb;
reg        hs, vs, de;
reg [23:0] rgb;

always @(negedge clk) if (locked) begin
    if (w_clk !== 10'b1111100000)
        report("clock channel", w_clk, 10'b1111100000);

    ctrl0 = is_ctrl(w_d0);
    ctrl1 = is_ctrl(w_d1);
    ctrl2 = is_ctrl(w_d2);
    if (ctrl0 != ctrl1 || ctrl0 != ctrl2)
        report("channel ctrl/data mismatch", {ctrl2, ctrl1, ctrl0}, 0);

    de = ~ctrl0;
    if (ctrl0) begin
        cb = ctrl_bits(w_d0);
        hs = cb[0];
        vs = cb[1];
    end else begin
        hs = ~SYNC_ACTIVE;
        vs = vs_prev;                  // 数据期不携带同步，沿用
    end
    rgb = {dec8(w_d2), dec8(w_d1), dec8(w_d0)};

    // 行起点：HS 由空闲变为有效
    if (hs == SYNC_ACTIVE && hs_prev != SYNC_ACTIVE) begin
        if (synced) begin
            if (px_in_line != H_TOTAL) report("line length", px_in_line, H_TOTAL);
            if (line_has_de) begin
                if (first_act_line < 0) first_act_line = line_in_frame;
                act_lines = act_lines + 1;
            end
            line_in_frame = line_in_frame + 1;
        end
        px_in_line  = 0;
        line_has_de = 1'b0;

        // 帧起点：VS 与 HS 同拍变为有效
        if (vs == SYNC_ACTIVE && vs_prev != SYNC_ACTIVE) begin
            if (synced) begin
                if (line_in_frame != V_TOTAL) report("frame lines", line_in_frame, V_TOTAL);
                if (vs_lines != V_SYNC) report("vsync lines", vs_lines, V_SYNC);
                if (first_act_line != V_SYNC + V_BP) report("first active line", first_act_line, V_SYNC + V_BP);
                if (act_lines != V_ACTIVE) report("active lines", act_lines, V_ACTIVE);
                frames_done = frames_done + 1;
            end
            // 复位释放后的第一帧首行不完整：编码器复位期输出 0（不是合法 TMDS 字），
            // 彩条模块的 hs/vs 寄存器上电初值为 0（低有效即“有效”）。从第二个场同步起点开始检查。
            vs_starts = vs_starts + 1;
            if (vs_starts >= 2)
                synced = 1'b1;
            line_in_frame  = 0;
            vs_lines       = 0;
            act_lines      = 0;
            first_act_line = -1;
        end
        if (vs == SYNC_ACTIVE) vs_lines = vs_lines + 1;
    end

    if (synced) begin
        if (hs == SYNC_ACTIVE) hs_run = hs_run + 1;
        if (hs != SYNC_ACTIVE && hs_prev == SYNC_ACTIVE) begin
            if (hs_run != H_SYNC) report("hsync width", hs_run, H_SYNC);
            hs_run = 0;
        end

        if (de && !de_prev) begin
            if (px_in_line != H_SYNC + H_BP) report("active start pixel", px_in_line, H_SYNC + H_BP);
            de_run = 0;
            line_has_de = 1'b1;
        end
        if (de) begin
            if (rgb !== bar_rgb(de_run)) report("pixel color", rgb, bar_rgb(de_run));
            de_run = de_run + 1;
            pixels_checked = pixels_checked + 1;
        end
        if (!de && de_prev && de_run != H_ACTIVE) report("active width", de_run, H_ACTIVE);
    end

    px_in_line = px_in_line + 1;
    hs_prev = hs;
    vs_prev = vs;
    de_prev = de;
end

initial begin
    #1000 locked = 1'b1;
    wait (frames_done >= FRAMES_TO_CHECK || errors > 1000);
    if (errors == 0 && frames_done >= FRAMES_TO_CHECK && pixels_checked == FRAMES_TO_CHECK * H_ACTIVE * V_ACTIVE)
        $display("RESULT: PASS frames=%0d pixels=%0d", frames_done, pixels_checked);
    else
        $display("RESULT: FAIL errors=%0d frames=%0d pixels=%0d", errors, frames_done, pixels_checked);
    $finish;
end

// 一帧约 16.7ms，跳过首帧再查两帧约 50ms，80ms 仍未结束即判失败
initial begin
    #80_000_000;
    $display("RESULT: FAIL timeout errors=%0d frames=%0d", errors, frames_done);
    $finish;
end

endmodule
