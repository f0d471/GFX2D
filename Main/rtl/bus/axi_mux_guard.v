`timescale 1ns/1ps
//==============================================================================================//
// FILE: axi_mux_guard.v
//
// DESCRIPTION: Axi_Mux 仲裁配置与 SoC（S2）防饿死保护：每个方向一个等待计数器，
//   达到 AGE_LIMIT 锁存 protect，把 S1 从 Cfg_Cnl_Mask 里摘掉直到该方向恢复。
//
// NOTE:
//   1. 掩码只影响新通道搜索，已锁存的授权与在途事务不受影响，不撤 valid 不违反 AXI
//   2. 两个 MAX 每拍更新（运行中取最大）：只在握手拍取值时，永不握手的那笔看不到
//   3. S0、S2 两位永不被保护清掉；GUARD_EN=0 时掩码恒全 1（只用于注错）
//   4. 寄存器表见 bus-manual.md
//==============================================================================================//
`default_nettype none

module axi_mux_guard #(
    parameter [31:0] ID      = 32'h4D55_5847,
    parameter [31:0] VERSION = 32'h0000_0002
)(
    input  wire        clk,
    input  wire        rst,        // 同步复位，高有效
    // APB 从口，paddr 只取窗口内偏移
    input  wire [11:0] paddr,
    input  wire        psel,
    input  wire        penable,
    input  wire        pwrite,
    input  wire [31:0] pwdata,
    output reg  [31:0] prdata,
    output wire        pready,
    output wire        pslverr,

    // 给 Axi_Mux 的配置
    output wire        arb_type,
    output wire [2:0]  cnl_mask,

    // S2（SoC）地址通道观测
    input  wire        s2_arvalid,
    input  wire        s2_arready,
    input  wire        s2_awvalid,
    input  wire        s2_awready
);

localparam [11:0] ADDR_ID        = 12'h000;
localparam [11:0] ADDR_VERSION   = 12'h004;
localparam [11:0] ADDR_CTRL      = 12'h008;
localparam [11:0] ADDR_CNL_MASK  = 12'h00C;
localparam [11:0] ADDR_AGE_LIMIT = 12'h010;
localparam [11:0] ADDR_STATUS    = 12'h014;
localparam [11:0] ADDR_HITS      = 12'h018;
localparam [11:0] ADDR_MAX_AR    = 12'h01C;
localparam [11:0] ADDR_MAX_AW    = 12'h020;
localparam [11:0] ADDR_CLEAR     = 12'h024;

// 配置寄存器
reg        cfg_arb_type;
reg        cfg_guard_en;
reg [2:0]  cfg_mask;
reg [31:0] cfg_age_limit;

// 统计寄存器
reg [31:0] stat_hits;
reg [31:0] stat_max_ar;
reg [31:0] stat_max_aw;

// 保护状态与等待计数器
reg        protect_ar;
reg        protect_aw;
reg [31:0] wait_ar;
reg [31:0] wait_aw;

wire access = psel & penable;

// 可写地址：CTRL、CHANNEL_MASK、AGE_LIMIT、CLEAR
wire wr_legal = (paddr == ADDR_CTRL) | (paddr == ADDR_CNL_MASK) |
                (paddr == ADDR_AGE_LIMIT) | (paddr == ADDR_CLEAR);

// 写 1 清三个统计寄存器；不改配置，也不清当前保护状态
wire stat_clear = access & pwrite & (paddr == ADDR_CLEAR) & pwdata[0];

// 读通路（access 阶段组合输出）
reg hit;

always @(*) begin
    hit = 1'b1;
    case (paddr)
        ADDR_ID:        prdata = ID;
        ADDR_VERSION:   prdata = VERSION;
        ADDR_CTRL:      prdata = {30'h0, cfg_guard_en, cfg_arb_type};
        ADDR_CNL_MASK:  prdata = {29'h0, cfg_mask};
        ADDR_AGE_LIMIT: prdata = cfg_age_limit;
        ADDR_STATUS:    prdata = {30'h0, protect_aw, protect_ar};
        ADDR_HITS:      prdata = stat_hits;
        ADDR_MAX_AR:    prdata = stat_max_ar;
        ADDR_MAX_AW:    prdata = stat_max_aw;
        ADDR_CLEAR:     prdata = 32'h0;          // 只写，读回 0
        default: begin
            prdata = 32'h0;
            hit    = 1'b0;
        end
    endcase
end

assign pready  = 1'b1;
assign pslverr = access & (pwrite ? ~wr_legal : ~hit);

// 保护逻辑：等待计数 -> 锁存 protect -> 掩码
wire ar_waiting = s2_arvalid & ~s2_arready;
wire aw_waiting = s2_awvalid & ~s2_awready;
wire ar_hand    = s2_arvalid & s2_arready;
wire aw_hand    = s2_awvalid & s2_awready;

// 计数达到阈值的那一拍锁存保护；AGE_LIMIT=0 时立即触发（注错用）
wire ar_reach = ar_waiting & ((cfg_age_limit == 32'h0) | (wait_ar + 32'h1 >= cfg_age_limit));
wire aw_reach = aw_waiting & ((cfg_age_limit == 32'h0) | (wait_aw + 32'h1 >= cfg_age_limit));

always @(posedge clk) begin
    if (rst) begin
        wait_ar    <= 32'h0;
        wait_aw    <= 32'h0;
        protect_ar <= 1'b0;
        protect_aw <= 1'b0;
    end else begin
        // 等待计数：valid 高 ready 低时加一（饱和），握手或 valid 撤销时清零
        if (ar_hand | ~s2_arvalid)
            wait_ar <= 32'h0;
        else if (ar_waiting & ~(&wait_ar))
            wait_ar <= wait_ar + 32'h1;

        if (aw_hand | ~s2_awvalid)
            wait_aw <= 32'h0;
        else if (aw_waiting & ~(&wait_aw))
            wait_aw <= wait_aw + 32'h1;

        // protect：触发时置位，握手或 valid 撤销时清除
        if (ar_hand | ~s2_arvalid)
            protect_ar <= 1'b0;
        else if (ar_reach)
            protect_ar <= 1'b1;

        if (aw_hand | ~s2_awvalid)
            protect_aw <= 1'b0;
        else if (aw_reach)
            protect_aw <= 1'b1;
    end
end

// guard 允许 S1 参与仲裁：两个方向都清干净才恢复；GUARD_EN=0 时恒允许
wire guard_allow_s1 = cfg_guard_en ? ~(protect_ar | protect_aw) : 1'b1;

assign arb_type = cfg_arb_type;
assign cnl_mask = cfg_mask & {1'b1, guard_allow_s1, 1'b1};

// 统计：protect 的 0->1 次数与两个方向的最大地址等待。
// 运行中取最大：等待中的这一拍结束后已等了 wait+1 拍，每拍与 MAX 比较。
// 一笔等了 N 拍再握手时，最大值正好是 N，与握手时取值相同；永不握手的那一笔也会一直往上涨。
// 计数饱和时不再加一，避免 wait+1 回绕成 0。
wire [31:0] ar_wait_now = wait_ar + 32'h1;
wire [31:0] aw_wait_now = wait_aw + 32'h1;
wire ar_grow = ar_waiting & ~(&wait_ar) & (ar_wait_now > stat_max_ar);
wire aw_grow = aw_waiting & ~(&wait_aw) & (aw_wait_now > stat_max_aw);

// 保护的 0->1 沿，按方向各算一次：同一拍两个方向都触发时要加 2
wire ar_rise = ar_reach & ~protect_ar;
wire aw_rise = aw_reach & ~protect_aw;

always @(posedge clk) begin
    if (rst) begin
        stat_hits   <= 32'h0;
        stat_max_ar <= 32'h0;
        stat_max_aw <= 32'h0;
    end else begin
        if (stat_clear) begin
            stat_hits   <= 32'h0;
            stat_max_ar <= 32'h0;
            stat_max_aw <= 32'h0;
        end else begin
            // 保护触发次数：AR、AW 两个 0->1 沿之和，同一拍可加 0/1/2
            stat_hits <= stat_hits + {31'b0, ar_rise} + {31'b0, aw_rise};

            if (ar_grow)
                stat_max_ar <= ar_wait_now;
            if (aw_grow)
                stat_max_aw <= aw_wait_now;
        end
    end
end

// 配置寄存器写
always @(posedge clk) begin
    if (rst) begin
        cfg_arb_type  <= 1'b1;
        cfg_guard_en  <= 1'b1;
        cfg_mask      <= 3'b111;
        cfg_age_limit <= 32'd64;
    end else if (access & pwrite) begin
        case (paddr)
            ADDR_CTRL: begin
                cfg_arb_type <= pwdata[0];
                cfg_guard_en <= pwdata[1];
            end
            ADDR_CNL_MASK:  cfg_mask      <= pwdata[2:0];
            ADDR_AGE_LIMIT: cfg_age_limit <= pwdata;
            default: ;   // CLEAR 只动统计寄存器，在上一段处理
        endcase
    end
end

endmodule

`default_nettype wire
